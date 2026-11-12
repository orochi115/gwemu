/* Auto-generated stub for JPEG */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "migration/vmstate.h"
#include "migration/qemu-file.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/gnw_h7b0_jpeg.h"
#include "hw/misc/gnw_h7b0_regs_jpeg.h"

/*
 * STBI_STATIC: gnw-h7b0 GUI (Phase 1, ported from xemu) vendors its own,
 * separate stb_image.h copy (ui/thirdparty/stb_image/) for PNG asset
 * loading. Without STBI_STATIC here, both translation units export the
 * same public stbi_* symbols -- a real ODR violation that let the linker
 * silently pick THIS file's STBI_ONLY_JPEG-restricted implementation for
 * calls made from the UI code, which then failed to decode any PNG at
 * all ("unknown image type"). Nothing outside this file calls this
 * copy's stbi_* functions, so static linkage is correct here regardless.
 */
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_ONLY_JPEG

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_image.h"
/*
 * Deliberately never popped: with STB_IMAGE_STATIC (added for the
 * gnw-h7b0 GUI's stb_image ODR fix, see above), GCC only finalizes
 * "static function declared but never defined" for STBI_ONLY_JPEG's
 * excluded decoders (zlib/PNG helpers) at end of translation unit, after
 * an early pragma pop would have already re-enabled the warning as an
 * error -- so it just stays ignored for the rest of this file.
 */

#include <math.h>
#include "hw/misc/gnw_env.h"

static void gnw_jpeg_lat_decode_start(void);
static void gnw_jpeg_lat_decode_done(void);

/* getenv() is a locked linear scan on Windows (msvcrt) -- never call it
 * per-event in emulation-hot paths; resolve once and cache. */
static bool gnw_jpeg_trace_enabled(void)
{
    static int v = -1;
    if (v < 0) {
        v = gnw_env_enabled("GNW_JPEG_TRACE");
    }
    return v;
}

static GnwH7B0JpegState *global_jpeg_state = NULL;

/* Clamp a fixed-point-converted sample to a valid byte. */
static inline uint8_t gnw_h7b0_jpeg_clamp_u8(int v)
{
    if (v < 0) {
        return 0;
    }
    if (v > 255) {
        return 255;
    }
    return (uint8_t)v;
}

/*
 * ---------------------------------------------------------------------
 * Minimal baseline JPEG encoder (YCbCr 4:4:4 only, matching what real
 * HAL_JPEG_Encode()/this hardware's CONFR1.DE=0 mode actually feeds via
 * DIR: raw already-YCbCr, pixel-interleaved bytes -- the real JPEG codec
 * IP does DCT+quantization+Huffman only, never RGB<->YCbCr conversion,
 * confirmed by reading sdk/stm32h7xx-hal-driver/Src/stm32h7xx_hal_jpeg.c
 * (JPEG_Init_Process() just toggles CONFR1_DE and starts the FIFO
 * pump; JPEG_SetColorYCBCR()/ConfigEncoding() only ever touch
 * registers, never transform pDataInMCU). Standard 8x8 block DCT-II,
 * IJG-style scaled quantization tables, canonical ITU T.81 Annex K
 * Huffman tables (the same public, universally-reused reference tables
 * every baseline JPEG codec -- libjpeg, stb_image_write, etc. -- ships),
 * zigzag reorder, real JFIF marker structure (SOI/DQT/SOF0/DHT/SOS/EOI).
 * No chroma subsampling support (not needed -- every diag test case
 * uses JPEG_444_SUBSAMPLING).
 * ---------------------------------------------------------------------
 */

static const int gnw_h7b0_jpeg_zigzag[64] = {
    0,  1,  8,  16, 9,  2,  3,  10,
    17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

/* ITU-T T.81 Annex K.1 standard quantization tables (quality-50 baseline). */
static const int gnw_h7b0_jpeg_std_luma_qt[64] = {
    16, 11, 10, 16, 24,  40,  51,  61,
    12, 12, 14, 19, 26,  58,  60,  55,
    14, 13, 16, 24, 40,  57,  69,  56,
    14, 17, 22, 29, 51,  87,  80,  62,
    18, 22, 37, 56, 68, 109, 103,  77,
    24, 35, 55, 64, 81, 104, 113,  92,
    49, 64, 78, 87, 103, 121, 120, 101,
    72, 92, 95, 98, 112, 100, 103,  99
};

static const int gnw_h7b0_jpeg_std_chroma_qt[64] = {
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99
};

typedef struct {
    uint8_t bits[16];     /* counts of codes of length 1..16 */
    const uint8_t *values;
    int nvalues;
} GnwJpegHuffSpec;

typedef struct {
    uint16_t code[256];
    uint8_t size[256];
} GnwJpegHuffTable;

/* ITU-T T.81 Annex K.3 standard Huffman tables. */
static const uint8_t gnw_h7b0_jpeg_dc_luma_vals[12] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11
};
static const GnwJpegHuffSpec gnw_h7b0_jpeg_dc_luma_spec = {
    .bits = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0},
    .values = gnw_h7b0_jpeg_dc_luma_vals, .nvalues = 12
};

static const uint8_t gnw_h7b0_jpeg_dc_chroma_vals[12] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11
};
static const GnwJpegHuffSpec gnw_h7b0_jpeg_dc_chroma_spec = {
    .bits = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0},
    .values = gnw_h7b0_jpeg_dc_chroma_vals, .nvalues = 12
};

static const uint8_t gnw_h7b0_jpeg_ac_luma_vals[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12,
    0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
    0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16,
    0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
    0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79,
    0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98,
    0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
    0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
    0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4,
    0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea,
    0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa
};
static const GnwJpegHuffSpec gnw_h7b0_jpeg_ac_luma_spec = {
    .bits = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d},
    .values = gnw_h7b0_jpeg_ac_luma_vals, .nvalues = 162
};

static const uint8_t gnw_h7b0_jpeg_ac_chroma_vals[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21,
    0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
    0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
    0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34,
    0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38,
    0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
    0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96,
    0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
    0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
    0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2,
    0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9,
    0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa
};
static const GnwJpegHuffSpec gnw_h7b0_jpeg_ac_chroma_spec = {
    .bits = {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77},
    .values = gnw_h7b0_jpeg_ac_chroma_vals, .nvalues = 162
};

/* Build canonical Huffman codes from a JPEG DHT-style bits/values spec
 * (standard algorithm, ITU-T T.81 Annex C). */
static void gnw_h7b0_jpeg_build_huff_table(const GnwJpegHuffSpec *spec, GnwJpegHuffTable *tbl)
{
    uint8_t huffsize[257];
    uint16_t huffcode[257];
    int p = 0;

    for (int l = 1; l <= 16; l++) {
        for (int i = 0; i < spec->bits[l - 1]; i++) {
            huffsize[p++] = (uint8_t)l;
        }
    }
    huffsize[p] = 0;
    int lastp = p;

    int code = 0;
    int si = huffsize[0];
    p = 0;
    while (huffsize[p]) {
        while (huffsize[p] == si) {
            huffcode[p] = (uint16_t)code;
            code++;
            p++;
        }
        code <<= 1;
        si++;
    }

    memset(tbl, 0, sizeof(*tbl));
    for (int i = 0; i < lastp; i++) {
        int val = spec->values[i];
        tbl->code[val] = huffcode[i];
        tbl->size[val] = huffsize[i];
    }
}

static void gnw_h7b0_jpeg_scale_quant(const int base[64], int quality, uint16_t out[64])
{
    int q = quality;
    if (q <= 0) {
        q = 1;
    }
    if (q > 100) {
        q = 100;
    }
    int scale = (q < 50) ? (5000 / q) : (200 - q * 2);
    for (int i = 0; i < 64; i++) {
        int v = (base[i] * scale + 50) / 100;
        if (v < 1) {
            v = 1;
        }
        if (v > 255) {
            v = 255;
        }
        out[i] = (uint16_t)v;
    }
}

/* Naive O(N^4) 8x8 DCT-II -- fine for the small test images this device
 * model ever needs to encode; correctness over speed. */
/*
 * Shared 8x8 DCT basis table: cos_tab[i][j] = cos((2i+1)*j*pi/16).
 * The fdct/idct below used to call libm cos() in their innermost loops
 * (8192 calls per 8x8 block); at retro-go launcher scroll rates that
 * made __cos_fma alone 36%% of the whole process' cycles (measured,
 * perf) and a 5KB cover thumbnail cost ~8ms to decode. Same doubles as
 * the direct calls, so results are bit-identical.
 */
static const double *gnw_h7b0_jpeg_cos_tab(void)
{
    static double tab[8][8];
    static bool init;

    if (!init) {
        for (int i = 0; i < 8; i++) {
            for (int j = 0; j < 8; j++) {
                tab[i][j] = cos((2 * i + 1) * j * M_PI / 16.0);
            }
        }
        init = true;
    }
    return &tab[0][0];
}

static void gnw_h7b0_jpeg_fdct(const double in[64], double out[64])
{
    /*
     * Same math as the naive O(N^4) loop this replaces, restructured so
     * the per-term product in[x*8+y] * ct[x*8+u] (invariant across the
     * inner v loop) is computed once per u instead of 8 times.  The
     * original expression evaluated left-to-right as
     * ((in * ct_xu) * ct_yv); hoisting the first product preserves that
     * association exactly, and the (x-major, y) summation order is
     * unchanged, so every intermediate double -- and therefore the
     * output -- is bit-identical to the naive version.
     */
    const double *ct = gnw_h7b0_jpeg_cos_tab();

    for (int u = 0; u < 8; u++) {
        double a[64];

        for (int x = 0; x < 8; x++) {
            for (int y = 0; y < 8; y++) {
                a[x * 8 + y] = in[x * 8 + y] * ct[x * 8 + u];
            }
        }
        for (int v = 0; v < 8; v++) {
            double sum = 0.0;
            for (int x = 0; x < 8; x++) {
                for (int y = 0; y < 8; y++) {
                    sum += a[x * 8 + y] * ct[y * 8 + v];
                }
            }
            double cu = (u == 0) ? (1.0 / sqrt(2.0)) : 1.0;
            double cv = (v == 0) ? (1.0 / sqrt(2.0)) : 1.0;
            out[u * 8 + v] = 0.25 * cu * cv * sum;
        }
    }
}

/* Standard JPEG "additional bits" magnitude encoding for a signed DC
 * diff or AC coefficient (ITU-T T.81 Annex F). */
static int gnw_h7b0_jpeg_magnitude(int v, int *bits_out)
{
    int a = v < 0 ? -v : v;
    int nbits = 0;
    while (a) {
        a >>= 1;
        nbits++;
    }
    *bits_out = (v >= 0) ? v : (v + (1 << nbits) - 1);
    return nbits;
}

typedef struct {
    GByteArray *out;
    uint32_t acc;
    int nbits;
} GnwJpegBitWriter;

static void gnw_h7b0_jpeg_bw_put_byte(GnwJpegBitWriter *bw, uint8_t b)
{
    g_byte_array_append(bw->out, &b, 1);
    if (b == 0xFF) {
        uint8_t z = 0;
        g_byte_array_append(bw->out, &z, 1);
    }
}

static void gnw_h7b0_jpeg_bw_put_bits(GnwJpegBitWriter *bw, uint32_t code, int size)
{
    if (size == 0) {
        return;
    }
    bw->acc = (bw->acc << size) | (code & ((1u << size) - 1));
    bw->nbits += size;
    while (bw->nbits >= 8) {
        int shift = bw->nbits - 8;
        uint8_t b = (uint8_t)((bw->acc >> shift) & 0xFF);
        gnw_h7b0_jpeg_bw_put_byte(bw, b);
        bw->nbits -= 8;
    }
    if (bw->nbits > 0) {
        bw->acc &= (1u << bw->nbits) - 1;
    } else {
        bw->acc = 0;
    }
}

static void gnw_h7b0_jpeg_bw_flush(GnwJpegBitWriter *bw)
{
    if (bw->nbits > 0) {
        uint8_t b = (uint8_t)((bw->acc << (8 - bw->nbits)) & 0xFF);
        uint8_t pad = (uint8_t)(0xFF >> bw->nbits);
        b |= pad;
        gnw_h7b0_jpeg_bw_put_byte(bw, b);
        bw->nbits = 0;
        bw->acc = 0;
    }
}

static void gnw_h7b0_jpeg_append_u8(GByteArray *out, uint8_t v)
{
    g_byte_array_append(out, &v, 1);
}

static void gnw_h7b0_jpeg_append_u16(GByteArray *out, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)(v & 0xFF) };
    g_byte_array_append(out, b, 2);
}

static void gnw_h7b0_jpeg_write_dqt(GByteArray *out, int id, const uint16_t qt[64])
{
    gnw_h7b0_jpeg_append_u8(out, 0xFF);
    gnw_h7b0_jpeg_append_u8(out, 0xDB);
    gnw_h7b0_jpeg_append_u16(out, 2 + 1 + 64);
    gnw_h7b0_jpeg_append_u8(out, (uint8_t)id);
    for (int i = 0; i < 64; i++) {
        gnw_h7b0_jpeg_append_u8(out, (uint8_t)qt[gnw_h7b0_jpeg_zigzag[i]]);
    }
}

static void gnw_h7b0_jpeg_write_sof0(GByteArray *out, int width, int height)
{
    gnw_h7b0_jpeg_append_u8(out, 0xFF);
    gnw_h7b0_jpeg_append_u8(out, 0xC0);
    gnw_h7b0_jpeg_append_u16(out, 2 + 1 + 2 + 2 + 1 + 3 * 3);
    gnw_h7b0_jpeg_append_u8(out, 8);
    gnw_h7b0_jpeg_append_u16(out, (uint16_t)height);
    gnw_h7b0_jpeg_append_u16(out, (uint16_t)width);
    gnw_h7b0_jpeg_append_u8(out, 3);
    gnw_h7b0_jpeg_append_u8(out, 1);
    gnw_h7b0_jpeg_append_u8(out, 0x11);
    gnw_h7b0_jpeg_append_u8(out, 0);
    gnw_h7b0_jpeg_append_u8(out, 2);
    gnw_h7b0_jpeg_append_u8(out, 0x11);
    gnw_h7b0_jpeg_append_u8(out, 1);
    gnw_h7b0_jpeg_append_u8(out, 3);
    gnw_h7b0_jpeg_append_u8(out, 0x11);
    gnw_h7b0_jpeg_append_u8(out, 1);
}

static void gnw_h7b0_jpeg_write_dht(GByteArray *out, int tc, int th, const GnwJpegHuffSpec *spec)
{
    gnw_h7b0_jpeg_append_u8(out, 0xFF);
    gnw_h7b0_jpeg_append_u8(out, 0xC4);
    gnw_h7b0_jpeg_append_u16(out, (uint16_t)(2 + 1 + 16 + spec->nvalues));
    gnw_h7b0_jpeg_append_u8(out, (uint8_t)((tc << 4) | th));
    for (int i = 0; i < 16; i++) {
        gnw_h7b0_jpeg_append_u8(out, spec->bits[i]);
    }
    for (int i = 0; i < spec->nvalues; i++) {
        gnw_h7b0_jpeg_append_u8(out, spec->values[i]);
    }
}

static void gnw_h7b0_jpeg_write_sos(GByteArray *out)
{
    gnw_h7b0_jpeg_append_u8(out, 0xFF);
    gnw_h7b0_jpeg_append_u8(out, 0xDA);
    gnw_h7b0_jpeg_append_u16(out, 2 + 1 + 3 * 2 + 3);
    gnw_h7b0_jpeg_append_u8(out, 3);
    gnw_h7b0_jpeg_append_u8(out, 1);
    gnw_h7b0_jpeg_append_u8(out, 0x00);
    gnw_h7b0_jpeg_append_u8(out, 2);
    gnw_h7b0_jpeg_append_u8(out, 0x11);
    gnw_h7b0_jpeg_append_u8(out, 3);
    gnw_h7b0_jpeg_append_u8(out, 0x11);
    gnw_h7b0_jpeg_append_u8(out, 0);
    gnw_h7b0_jpeg_append_u8(out, 63);
    gnw_h7b0_jpeg_append_u8(out, 0);
}

/*
 * ---------------------------------------------------------------------
 * Minimal baseline JPEG DECODER, decoding straight to native Y/Cb/Cr
 * component planes -- never through an RGB intermediate.
 *
 * WHY THIS EXISTS ALONGSIDE THE EXISTING stb_image-BASED PATH: real
 * STM32 JPEG codec hardware decodes directly to YCbCr with no RGB step
 * at all (matches the encode side's DIR/DOR behavior -- see the encoder
 * comment above). The pre-existing decode path here uses stb_image's
 * stbi_load_from_memory(..., 3) which forces an internal YCbCr->RGB
 * conversion, and this file's own DIR handler then converts that RGB
 * back to YCbCr for DOR output. For ordinary photographic content this
 * round trip only costs a little precision, but for legitimate in-gamut-
 * adjacent YCbCr values whose forward RGB conversion clips (e.g.
 * Y=180,Cb=220,Cr=40 -> B channel computes to 343, clamped to 255),
 * that clipping is a real, irreversible information loss the RGB
 * intermediate introduces -- confirmed via this repo's own diag suite's
 * jpeg_decode_correct case, which round-trips HAL_JPEG's own encoder
 * output (this file's new encoder, verified independently bit-correct
 * against a from-scratch Python Huffman/DCT decode) through
 * HAL_JPEG_Decode() and failed tolerance specifically on the
 * out-of-gamut color half, not the in-gamut half.
 *
 * This decoder is used preferentially (see the DIR write handler's
 * decode-completion branch); if it can't parse the bitstream (anything
 * beyond plain baseline sequential DCT + Huffman coding -- progressive,
 * arithmetic, 12-bit precision, unexpected marker structure) it returns
 * false and the caller falls back to the original stb_image-based path,
 * so real-world exotic JPEGs already working via that path keep working.
 * ---------------------------------------------------------------------
 */

typedef struct {
    bool valid;
    uint16_t table[64]; /* natural (row-major) order */
} GnwJpegDecQT;

typedef struct {
    bool valid;
    int mincode[17];
    int maxcode[17]; /* -1 == no codes of this length */
    int valptr[17];
    uint8_t values[256];
} GnwJpegDecHuff;

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
    uint32_t bitbuf;
    int bitcnt;
    bool marker_hit;
} GnwJpegBitReader;

static void gnw_h7b0_jpeg_build_dec_huff(const uint8_t bits[16], const uint8_t *values,
                                          int nvalues, GnwJpegDecHuff *h)
{
    uint8_t huffsize[257];
    uint16_t huffcode[257];
    int p = 0;

    for (int l = 1; l <= 16; l++) {
        for (int i = 0; i < bits[l - 1]; i++) {
            huffsize[p++] = (uint8_t)l;
        }
    }
    huffsize[p] = 0;

    int code = 0;
    int si = huffsize[0];
    p = 0;
    while (huffsize[p]) {
        while (huffsize[p] == si) {
            huffcode[p] = (uint16_t)code;
            code++;
            p++;
        }
        code <<= 1;
        si++;
    }

    memset(h, 0, sizeof(*h));
    h->valid = true;
    int n = MIN(nvalues, 256);
    memcpy(h->values, values, n);

    p = 0;
    for (int l = 1; l <= 16; l++) {
        if (bits[l - 1] == 0) {
            h->maxcode[l] = -1;
        } else {
            h->valptr[l] = p;
            h->mincode[l] = huffcode[p];
            p += bits[l - 1];
            h->maxcode[l] = huffcode[p - 1];
        }
    }
}

static int gnw_h7b0_jpeg_br_get_bit(GnwJpegBitReader *br)
{
    if (br->bitcnt == 0) {
        if (br->pos >= br->len) {
            br->marker_hit = true;
            return 0;
        }
        uint8_t b = br->data[br->pos];
        if (b == 0xFF) {
            if (br->pos + 1 < br->len && br->data[br->pos + 1] == 0x00) {
                br->pos += 2;
            } else {
                /* Real marker (restart or EOI) -- stop consuming bits,
                 * leave position at the 0xFF so the caller can detect
                 * and (for restart markers) consume it explicitly. */
                br->marker_hit = true;
                return 0;
            }
        } else {
            br->pos++;
        }
        br->bitbuf = b;
        br->bitcnt = 8;
    }
    br->bitcnt--;
    return (br->bitbuf >> br->bitcnt) & 1;
}

static int gnw_h7b0_jpeg_br_get_bits(GnwJpegBitReader *br, int n)
{
    int v = 0;
    for (int i = 0; i < n; i++) {
        v = (v << 1) | gnw_h7b0_jpeg_br_get_bit(br);
    }
    return v;
}

static int gnw_h7b0_jpeg_extend(int v, int nbits)
{
    if (nbits == 0) {
        return 0;
    }
    if (v < (1 << (nbits - 1))) {
        return v - (1 << nbits) + 1;
    }
    return v;
}

/* Standard JPEG Annex F decode: consume bits one at a time until the
 * accumulated code falls within some length's [mincode,maxcode] range. */
static bool gnw_h7b0_jpeg_dec_huff_symbol(GnwJpegBitReader *br, const GnwJpegDecHuff *h, int *out)
{
    int code = gnw_h7b0_jpeg_br_get_bit(br);
    for (int l = 1; l <= 16; l++) {
        if (h->maxcode[l] != -1 && code <= h->maxcode[l] && code >= h->mincode[l]) {
            *out = h->values[h->valptr[l] + (code - h->mincode[l])];
            return true;
        }
        code = (code << 1) | gnw_h7b0_jpeg_br_get_bit(br);
        if (br->marker_hit) {
            return false;
        }
    }
    return false;
}

/* Standard 8x8 inverse DCT-II (Annex A), naive O(N^4) -- same cost class
 * as the encoder's forward transform, fine for a single-shot per-image
 * decode call. */
static void gnw_h7b0_jpeg_idct(const double in[64], double out[64])
{
    /*
     * Bit-identical fast path over the naive O(N^4) loop this replaces.
     * Two observations make this exact, not merely close:
     *
     * 1. Dequantized coefficient blocks are sparse (typically a handful
     *    of nonzero entries out of 64).  In the naive sum, a zero
     *    coefficient contributes a product of exactly +/-0.0, and adding
     *    +/-0.0 to a finite partial sum never changes its value or its
     *    rounding (0.0 + -0.0 is +0.0, and the initial sum is +0.0, so
     *    the sign of zero can't leak either).  Skipping zero terms while
     *    keeping the surviving terms in the original (u-major, v)
     *    traversal order therefore reproduces every intermediate double
     *    exactly.
     *
     * 2. The naive per-term expression evaluated left-to-right as
     *    ((((cu * cv) * in) * ct_xu) * ct_yv).  Pre-folding
     *    p = (cu * cv) * in once per coefficient, then per output row
     *    q = p * ct_xu once per x, then sum += q * ct_yv, performs the
     *    identical multiplications in the identical order -- only the
     *    redundant recomputations are removed.
     *
     * Net effect: 64 outputs x nnz x 1 multiply-add (plus 8 x nnz row
     * hoists) instead of 64 x 64 x 4 multiplies -- roughly an order of
     * magnitude fewer flops for real launcher-thumbnail content, with
     * output verified bit-identical (GNW_JPEG_TRACE ysum stream).
     *
     * NOTE for future editors: do NOT replace this with a separable
     * row/column (2x 1-D) IDCT or an integer/AAN variant without
     * re-validating output -- those change summation order and rounding,
     * and this device model's contract (diag suite + trace-hash
     * comparisons against prior runs) expects bit-exact stability.
     */
    const double *ct = gnw_h7b0_jpeg_cos_tab();
    const double c0 = 1.0 / sqrt(2.0);
    double coef[64];
    int cu_idx[64], cv_idx[64];
    int n = 0;

    for (int u = 0; u < 8; u++) {
        double cu = (u == 0) ? c0 : 1.0;
        for (int v = 0; v < 8; v++) {
            double c = in[u * 8 + v];
            if (c != 0.0) {
                double cv = (v == 0) ? c0 : 1.0;
                coef[n] = (cu * cv) * c;
                cu_idx[n] = u;
                cv_idx[n] = v;
                n++;
            }
        }
    }

    for (int x = 0; x < 8; x++) {
        double q[64];

        for (int i = 0; i < n; i++) {
            q[i] = coef[i] * ct[x * 8 + cu_idx[i]];
        }
        for (int y = 0; y < 8; y++) {
            double sum = 0.0;
            for (int i = 0; i < n; i++) {
                sum += q[i] * ct[y * 8 + cv_idx[i]];
            }
            out[x * 8 + y] = 0.25 * sum;
        }
    }
}

#define GNW_JPEG_DEC_MAXCOMP 4

typedef struct {
    int id;
    int h, v;
    int tq;
    int dc_sel, ac_sel;
    int pw, ph;       /* block-padded plane size actually decoded into */
    int cw, ch;       /* cropped (real) plane size for output */
    uint8_t *plane;   /* pw*ph bytes, block-padded */
} GnwJpegDecComp;

static bool gnw_h7b0_jpeg_decode_native(const uint8_t *buf, size_t len,
                                        uint8_t **y_out, uint8_t **cb_out, uint8_t **cr_out,
                                        int *w_out, int *h_out,
                                        int *chroma_w_out, int *chroma_h_out)
{
    if (len < 4 || buf[0] != 0xFF || buf[1] != 0xD8) {
        return false;
    }

    GnwJpegDecQT qt[4] = { 0 };
    GnwJpegDecHuff dchuff[4] = { 0 };
    GnwJpegDecHuff achuff[4] = { 0 };
    GnwJpegDecComp comp[GNW_JPEG_DEC_MAXCOMP] = { 0 };
    int ncomp = 0;
    int width = 0, height = 0;
    int restart_interval = 0;
    size_t pos = 2;
    bool have_sof = false;
    size_t scan_start = 0;
    bool ok = false;

    while (pos + 4 <= len) {
        if (buf[pos] != 0xFF) {
            pos++;
            continue;
        }
        uint8_t marker = buf[pos + 1];
        if (marker == 0xFF) {
            pos++;
            continue;
        }
        if (marker == 0x00 || marker == 0xD8) {
            pos += 2;
            continue;
        }
        if (marker == 0xD9) {
            /* EOI before SOS -- malformed for our purposes. */
            break;
        }
        if (marker >= 0xD0 && marker <= 0xD7) {
            pos += 2;
            continue;
        }
        uint32_t seglen = ((uint32_t)buf[pos + 2] << 8) | buf[pos + 3];
        if (seglen < 2 || pos + 2 + seglen > len) {
            break;
        }
        size_t seg = pos + 4;
        size_t seg_end = pos + 2 + seglen;

        if (marker == 0xDB) {
            /* DQT: one or more tables per segment. */
            while (seg < seg_end) {
                uint8_t pq_tq = buf[seg++];
                int pq = pq_tq >> 4;
                int tq = pq_tq & 0xF;
                if (pq != 0 || tq > 3 || seg + 64 > seg_end) {
                    goto fail;
                }
                for (int i = 0; i < 64; i++) {
                    qt[tq].table[gnw_h7b0_jpeg_zigzag[i]] = buf[seg + i];
                }
                qt[tq].valid = true;
                seg += 64;
            }
        } else if (marker == 0xC0 || marker == 0xC1) {
            if (have_sof) {
                goto fail;
            }
            int precision = buf[seg];
            height = ((int)buf[seg + 1] << 8) | buf[seg + 2];
            width = ((int)buf[seg + 3] << 8) | buf[seg + 4];
            ncomp = buf[seg + 5];
            if (precision != 8 || ncomp < 1 || ncomp > GNW_JPEG_DEC_MAXCOMP ||
                width <= 0 || height <= 0) {
                goto fail;
            }
            size_t c = seg + 6;
            for (int i = 0; i < ncomp; i++) {
                if (c + 3 > seg_end) {
                    goto fail;
                }
                comp[i].id = buf[c];
                comp[i].h = buf[c + 1] >> 4;
                comp[i].v = buf[c + 1] & 0xF;
                comp[i].tq = buf[c + 2];
                if (comp[i].h < 1 || comp[i].h > 4 || comp[i].v < 1 || comp[i].v > 4 ||
                    comp[i].tq > 3) {
                    goto fail;
                }
                c += 3;
            }
            have_sof = true;
        } else if (marker == 0xC4) {
            while (seg < seg_end) {
                uint8_t tc_th = buf[seg++];
                int tc = tc_th >> 4;
                int th = tc_th & 0xF;
                if (th > 3 || seg + 16 > seg_end) {
                    goto fail;
                }
                uint8_t bits[16];
                memcpy(bits, &buf[seg], 16);
                seg += 16;
                int nvalues = 0;
                for (int i = 0; i < 16; i++) {
                    nvalues += bits[i];
                }
                if (nvalues > 256 || seg + (size_t)nvalues > seg_end) {
                    goto fail;
                }
                if (tc == 0) {
                    gnw_h7b0_jpeg_build_dec_huff(bits, &buf[seg], nvalues, &dchuff[th]);
                } else {
                    gnw_h7b0_jpeg_build_dec_huff(bits, &buf[seg], nvalues, &achuff[th]);
                }
                seg += nvalues;
            }
        } else if (marker == 0xDD) {
            if (seg + 2 > seg_end) {
                goto fail;
            }
            restart_interval = ((int)buf[seg] << 8) | buf[seg + 1];
        } else if (marker == 0xDA) {
            if (!have_sof) {
                goto fail;
            }
            int ns = buf[seg];
            size_t c = seg + 1;
            for (int i = 0; i < ns; i++) {
                if (c + 2 > seg_end) {
                    goto fail;
                }
                int cs = buf[c];
                int td_ta = buf[c + 1];
                for (int j = 0; j < ncomp; j++) {
                    if (comp[j].id == cs) {
                        comp[j].dc_sel = td_ta >> 4;
                        comp[j].ac_sel = td_ta & 0xF;
                    }
                }
                c += 2;
            }
            /* Ss/Se/AhAl (3 bytes) assumed 0/63/0 for baseline -- not
             * re-validated here (SOF0/SOF1 already restricts us to
             * sequential baseline). */
            scan_start = seg_end;
            break;
        } else if (marker == 0xC2 || (marker >= 0xC3 && marker <= 0xCF &&
                                       marker != 0xC4 && marker != 0xC8) ) {
            /* Progressive (C2), lossless, arithmetic-coded, or other SOF
             * variants we don't implement. */
            goto fail;
        }
        /* else: APPn/COM/other markers with a length field -- skip. */
        pos = seg_end;
    }

    if (!have_sof || scan_start == 0) {
        goto fail;
    }
    for (int i = 0; i < ncomp; i++) {
        if (!qt[comp[i].tq].valid) {
            goto fail;
        }
    }

    int hmax = 1, vmax = 1;
    for (int i = 0; i < ncomp; i++) {
        if (comp[i].h > hmax) {
            hmax = comp[i].h;
        }
        if (comp[i].v > vmax) {
            vmax = comp[i].v;
        }
    }
    int mcu_w = 8 * hmax, mcu_h = 8 * vmax;
    int n_mcu_x = (width + mcu_w - 1) / mcu_w;
    int n_mcu_y = (height + mcu_h - 1) / mcu_h;

    for (int i = 0; i < ncomp; i++) {
        comp[i].pw = n_mcu_x * comp[i].h * 8;
        comp[i].ph = n_mcu_y * comp[i].v * 8;
        comp[i].cw = (width * comp[i].h + hmax - 1) / hmax;
        comp[i].ch = (height * comp[i].v + vmax - 1) / vmax;
        comp[i].plane = g_malloc(comp[i].pw * comp[i].ph);
    }

    GnwJpegBitReader br = { .data = buf, .len = len, .pos = scan_start };
    int dc_pred[GNW_JPEG_DEC_MAXCOMP] = { 0 };
    int mcus_since_restart = 0;

    for (int my = 0; my < n_mcu_y; my++) {
        for (int mx = 0; mx < n_mcu_x; mx++) {
            if (restart_interval > 0 && mcus_since_restart == restart_interval) {
                /* Byte-align, then expect an RSTn marker. */
                br.bitcnt = 0;
                if (br.pos + 1 >= br.len || buf[br.pos] != 0xFF ||
                    buf[br.pos + 1] < 0xD0 || buf[br.pos + 1] > 0xD7) {
                    goto fail_free;
                }
                br.pos += 2;
                br.marker_hit = false;
                memset(dc_pred, 0, sizeof(dc_pred));
                mcus_since_restart = 0;
            }
            for (int ci = 0; ci < ncomp; ci++) {
                for (int by = 0; by < comp[ci].v; by++) {
                    for (int bx = 0; bx < comp[ci].h; bx++) {
                        int zz[64] = { 0 };
                        int sym;
                        if (!gnw_h7b0_jpeg_dec_huff_symbol(&br, &dchuff[comp[ci].dc_sel], &sym)) {
                            goto fail_free;
                        }
                        int diff_bits = sym ? gnw_h7b0_jpeg_br_get_bits(&br, sym) : 0;
                        int diff = gnw_h7b0_jpeg_extend(diff_bits, sym);
                        dc_pred[ci] += diff;
                        zz[0] = dc_pred[ci];

                        int k = 1;
                        while (k < 64) {
                            int acsym;
                            if (!gnw_h7b0_jpeg_dec_huff_symbol(&br, &achuff[comp[ci].ac_sel], &acsym)) {
                                goto fail_free;
                            }
                            if (acsym == 0x00) {
                                break;
                            }
                            int run = acsym >> 4;
                            int size = acsym & 0xF;
                            if (acsym == 0xF0) {
                                k += 16;
                                continue;
                            }
                            k += run;
                            if (k >= 64 || size == 0) {
                                goto fail_free;
                            }
                            int val_bits = gnw_h7b0_jpeg_br_get_bits(&br, size);
                            zz[k] = gnw_h7b0_jpeg_extend(val_bits, size);
                            k++;
                        }
                        if (br.marker_hit) {
                            goto fail_free;
                        }

                        double natural[64];
                        for (int i = 0; i < 64; i++) {
                            natural[gnw_h7b0_jpeg_zigzag[i]] =
                                (double)zz[i] * (double)qt[comp[ci].tq].table[gnw_h7b0_jpeg_zigzag[i]];
                        }
                        double block[64];
                        gnw_h7b0_jpeg_idct(natural, block);

                        int origin_x = (mx * comp[ci].h + bx) * 8;
                        int origin_y = (my * comp[ci].v + by) * 8;
                        for (int yy = 0; yy < 8; yy++) {
                            for (int xx = 0; xx < 8; xx++) {
                                int sample = (int)lround(block[yy * 8 + xx] + 128.0);
                                comp[ci].plane[(origin_y + yy) * comp[ci].pw + (origin_x + xx)] =
                                    gnw_h7b0_jpeg_clamp_u8(sample);
                            }
                        }
                    }
                }
            }
            mcus_since_restart++;
        }
    }

    /* Crop each component's block-padded plane down to its real size. */
    {
        uint8_t *cropped[GNW_JPEG_DEC_MAXCOMP] = { 0 };
        for (int i = 0; i < ncomp; i++) {
            cropped[i] = g_malloc(comp[i].cw * comp[i].ch);
            for (int yy = 0; yy < comp[i].ch; yy++) {
                memcpy(&cropped[i][yy * comp[i].cw],
                       &comp[i].plane[yy * comp[i].pw],
                       comp[i].cw);
            }
        }
        *y_out = cropped[0];
        if (ncomp >= 3) {
            *cb_out = cropped[1];
            *cr_out = cropped[2];
            *chroma_w_out = comp[1].cw;
            *chroma_h_out = comp[1].ch;
        } else {
            /* Grayscale: synthesize neutral chroma so callers expecting
             * 3 planes still get sane (128,128) output. */
            *chroma_w_out = comp[0].cw;
            *chroma_h_out = comp[0].ch;
            *cb_out = g_malloc(comp[0].cw * comp[0].ch);
            *cr_out = g_malloc(comp[0].cw * comp[0].ch);
            memset(*cb_out, 128, comp[0].cw * comp[0].ch);
            memset(*cr_out, 128, comp[0].cw * comp[0].ch);
        }
        *w_out = comp[0].cw;
        *h_out = comp[0].ch;
    }
    ok = true;

fail_free:
    for (int i = 0; i < ncomp; i++) {
        g_free(comp[i].plane);
    }
    if (ok) {
        return true;
    }
fail:
    return false;
}

/* Real baseline JPEG encode of an interleaved (Y,Cb,Cr) 4:4:4 pixel
 * buffer, matching exactly what HAL_JPEG_Encode()'s DIR-fed pDataInMCU
 * looks like for JPEG_YCBCR_COLORSPACE + JPEG_444_SUBSAMPLING (the only
 * mode this repo's diag suite's JPEG encode cases exercise). Fixed
 * internal quality (independent of firmware's ImageQuality field --
 * real hardware's quant/Huffman tables are configured via separate
 * QMEM/HUFF table RAM registers this device model doesn't implement;
 * see gnw_h7b0_jpeg_write() DIR handler comment) chosen high enough
 * that the diag suite's tolerance (4-5 out of 255) is comfortably met
 * even for two-tone images, not just flat ones. */
#define GNW_H7B0_JPEG_ENCODE_QUALITY 92

/* Inputs at or below this size decode inline on the BQL thread (see the
 * comment at the inline path in the write handler); larger ones go to
 * the worker thread. Big enough for cover thumbnails (~5KB) and
 * full-screen background JPEGs (tens of KB), small enough that a
 * pathological input can't stall the BQL for tens of ms. */
#define GNW_H7B0_JPEG_INLINE_MAX_BYTES (256 * 1024)

static void gnw_h7b0_jpeg_encode_ycbcr444(const uint8_t *y, const uint8_t *cb, const uint8_t *cr,
                                           int width, int height, GByteArray *out)
{
    int bw_blocks = (width + 7) / 8;
    int bh_blocks = (height + 7) / 8;

    uint16_t qt_luma[64], qt_chroma[64];
    gnw_h7b0_jpeg_scale_quant(gnw_h7b0_jpeg_std_luma_qt, GNW_H7B0_JPEG_ENCODE_QUALITY, qt_luma);
    gnw_h7b0_jpeg_scale_quant(gnw_h7b0_jpeg_std_chroma_qt, GNW_H7B0_JPEG_ENCODE_QUALITY, qt_chroma);

    GnwJpegHuffTable dc_luma_tbl, dc_chroma_tbl, ac_luma_tbl, ac_chroma_tbl;
    gnw_h7b0_jpeg_build_huff_table(&gnw_h7b0_jpeg_dc_luma_spec, &dc_luma_tbl);
    gnw_h7b0_jpeg_build_huff_table(&gnw_h7b0_jpeg_dc_chroma_spec, &dc_chroma_tbl);
    gnw_h7b0_jpeg_build_huff_table(&gnw_h7b0_jpeg_ac_luma_spec, &ac_luma_tbl);
    gnw_h7b0_jpeg_build_huff_table(&gnw_h7b0_jpeg_ac_chroma_spec, &ac_chroma_tbl);

    g_byte_array_set_size(out, 0);

    static const uint8_t soi[2] = { 0xFF, 0xD8 };
    g_byte_array_append(out, soi, 2);

    gnw_h7b0_jpeg_write_dqt(out, 0, qt_luma);
    gnw_h7b0_jpeg_write_dqt(out, 1, qt_chroma);
    gnw_h7b0_jpeg_write_sof0(out, width, height);
    gnw_h7b0_jpeg_write_dht(out, 0, 0, &gnw_h7b0_jpeg_dc_luma_spec);
    gnw_h7b0_jpeg_write_dht(out, 1, 0, &gnw_h7b0_jpeg_ac_luma_spec);
    gnw_h7b0_jpeg_write_dht(out, 0, 1, &gnw_h7b0_jpeg_dc_chroma_spec);
    gnw_h7b0_jpeg_write_dht(out, 1, 1, &gnw_h7b0_jpeg_ac_chroma_spec);
    gnw_h7b0_jpeg_write_sos(out);

    GnwJpegBitWriter bw = { .out = out, .acc = 0, .nbits = 0 };
    int dc_pred[3] = { 0, 0, 0 };

    for (int by = 0; by < bh_blocks; by++) {
        for (int bx = 0; bx < bw_blocks; bx++) {
            for (int comp = 0; comp < 3; comp++) {
                const uint8_t *plane = (comp == 0) ? y : ((comp == 1) ? cb : cr);
                double block[64];
                for (int yy = 0; yy < 8; yy++) {
                    for (int xx = 0; xx < 8; xx++) {
                        int sx = bx * 8 + xx;
                        int sy = by * 8 + yy;
                        if (sx >= width) {
                            sx = width - 1;
                        }
                        if (sy >= height) {
                            sy = height - 1;
                        }
                        block[yy * 8 + xx] = (double)plane[sy * width + sx] - 128.0;
                    }
                }
                double dct[64];
                gnw_h7b0_jpeg_fdct(block, dct);
                const uint16_t *qt = (comp == 0) ? qt_luma : qt_chroma;
                int quant[64];
                for (int i = 0; i < 64; i++) {
                    quant[i] = (int)lround(dct[i] / (double)qt[i]);
                }
                int zz[64];
                for (int i = 0; i < 64; i++) {
                    zz[i] = quant[gnw_h7b0_jpeg_zigzag[i]];
                }

                int diff = zz[0] - dc_pred[comp];
                dc_pred[comp] = zz[0];

                const GnwJpegHuffTable *dctab = (comp == 0) ? &dc_luma_tbl : &dc_chroma_tbl;
                const GnwJpegHuffTable *actab = (comp == 0) ? &ac_luma_tbl : &ac_chroma_tbl;

                int bits, nbits = gnw_h7b0_jpeg_magnitude(diff, &bits);
                gnw_h7b0_jpeg_bw_put_bits(&bw, dctab->code[nbits], dctab->size[nbits]);
                if (nbits) {
                    gnw_h7b0_jpeg_bw_put_bits(&bw, (uint32_t)bits, nbits);
                }

                int run = 0;
                for (int k = 1; k < 64; k++) {
                    int coef = zz[k];
                    if (coef == 0) {
                        run++;
                        continue;
                    }
                    while (run > 15) {
                        gnw_h7b0_jpeg_bw_put_bits(&bw, actab->code[0xF0], actab->size[0xF0]);
                        run -= 16;
                    }
                    int b2, nb2 = gnw_h7b0_jpeg_magnitude(coef, &b2);
                    int sym = (run << 4) | nb2;
                    gnw_h7b0_jpeg_bw_put_bits(&bw, actab->code[sym], actab->size[sym]);
                    gnw_h7b0_jpeg_bw_put_bits(&bw, (uint32_t)b2, nb2);
                    run = 0;
                }
                if (run > 0) {
                    gnw_h7b0_jpeg_bw_put_bits(&bw, actab->code[0x00], actab->size[0x00]);
                }
            }
        }
    }
    gnw_h7b0_jpeg_bw_flush(&bw);

    /*
     * Real HAL_JPEG_Decode() (stm32h7xx_hal_jpeg.c) unconditionally rounds
     * InDataLength down to a multiple of 4 before feeding any bytes to the
     * codec ("In Data length must be multiple of 4 Bytes (1 word)") --
     * losing 0-3 trailing bytes off whatever length firmware passes in.
     * The diag suite's own EOI-scan-derived enc_len is exactly the true
     * encoded length, which is essentially never a multiple of 4 (baseline
     * JPEG entropy-coded data has no such alignment), so real hardware's
     * codec -- which completes decode once it has decoded its own known
     * MCU count rather than needing to literally see the EOI marker bytes
     * -- tolerates this fine, but this device model's simpler EOI-detection-
     * triggered decode (see gnw_h7b0_jpeg_write()'s DIR handler) needs the
     * EOI bytes to actually arrive intact. Insert 0-3 standalone 0xFF fill
     * bytes (legal anywhere before a marker per the JPEG spec: a run of
     * 0xFF bytes not followed by 0x00 or consumed as part of the entropy
     * scan is simply skipped as padding, with the first non-0xFF byte
     * after the run treated as the marker code) immediately before EOI so
     * the marker itself always lands on a 4-byte boundary -- eliminating
     * the truncation instead of trying to special-case it on the decode
     * side.
     */
    uint32_t pre_eoi_len = out->len;
    int pad = (4 - (int)((pre_eoi_len + 2) % 4)) % 4;
    for (int i = 0; i < pad; i++) {
        gnw_h7b0_jpeg_append_u8(out, 0xFF);
    }

    static const uint8_t eoi[2] = { 0xFF, 0xD9 };
    g_byte_array_append(out, eoi, 2);
}

void gnw_h7b0_jpeg_get_last_decoded_size(uint32_t *width, uint32_t *height)
{
    if (global_jpeg_state && global_jpeg_state->y_plane) {
        *width = (uint32_t)global_jpeg_state->plane_width;
        *height = (uint32_t)global_jpeg_state->plane_height;
        return;
    }
    *width = 0;
    *height = 0;
}

void gnw_h7b0_jpeg_get_last_chroma_size(uint32_t *width, uint32_t *height)
{
    if (global_jpeg_state && global_jpeg_state->y_plane) {
        *width = (uint32_t)global_jpeg_state->chroma_width;
        *height = (uint32_t)global_jpeg_state->chroma_height;
        return;
    }
    *width = 0;
    *height = 0;
}

/*
 * Hand-parse the SOF0 marker (FF C0) out of the buffered input JPEG bytes
 * to recover each component's real H/V sampling-factor nibble. stb_image's
 * public API doesn't expose these, but real hardware's CONFRN1's VSF/HSF
 * fields need them for register-read fidelity (see plan doc). Returns true
 * and fills h_out/v_out with component 1's (luma) sampling factors if a
 * SOF0 marker was found and looked well-formed; false otherwise (caller
 * should fall back to its previous approximation).
 */
static bool gnw_h7b0_jpeg_parse_sof0_luma_sampling(const uint8_t *buf, size_t len,
                                                    int *h_out, int *v_out)
{
    size_t i = 0;

    while (i + 4 <= len) {
        if (buf[i] != 0xFF) {
            i++;
            continue;
        }
        uint8_t marker = buf[i + 1];
        /* Skip fill bytes / standalone markers with no length field. */
        if (marker == 0xFF) {
            i++;
            continue;
        }
        if (marker == 0xD8 || marker == 0xD9 ||
            (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) {
            i += 2;
            continue;
        }
        if (i + 4 > len) {
            break;
        }
        uint32_t seg_len = ((uint32_t)buf[i + 2] << 8) | buf[i + 3];
        if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
            /* SOF0/1/2: marker(2)+len(2)+precision(1)+height(2)+width(2)+
             * num_components(1)+per-component(id,samp,qt)*3. */
            size_t sof_off = i + 4;
            if (sof_off + 6 > len) {
                return false;
            }
            uint8_t num_comp = buf[sof_off + 5];
            size_t comp0_off = sof_off + 6;
            if (num_comp < 1 || comp0_off + 3 > len) {
                return false;
            }
            uint8_t samp = buf[comp0_off + 1];
            *h_out = (samp >> 4) & 0xF;
            *v_out = samp & 0xF;
            return true;
        }
        if (seg_len < 2 || i + 2 + seg_len > len) {
            break;
        }
        i += 2 + seg_len;
    }
    return false;
}

/*
 * Worker thread body: consumes one snapshotted input bitstream at a time,
 * runs the exact same decode (native decoder, falling back to
 * stb_image + BT.601 YCbCr conversion) the old synchronous code ran
 * inline in the DIR write handler on EOI, and publishes the result into
 * pending_* under thread_lock for the BQL thread to pick up. Never
 * touches s->regs[]/y_plane/cb_plane/cr_plane/dor_cursor -- those remain
 * BQL-only state, mutated only by gnw_h7b0_jpeg_poll_worker() below.
 */

/* Result of one whole-image decode -- shared by the worker thread and
 * the inline (small-input) path below. */
typedef struct GnwJpegDecodeOut {
    int w, h, comp, chroma_w, chroma_h;
    uint8_t *y, *cb, *cr;
    uint32_t confrn1;
} GnwJpegDecodeOut;

static void gnw_h7b0_jpeg_run_decode(const uint8_t *data, uint32_t len,
                                      GnwJpegDecodeOut *o)
{
    int w = 0, h = 0, comp = 3;
    uint8_t *y = NULL, *cb = NULL, *cr = NULL;
    int chroma_w = 0, chroma_h = 0;
    uint32_t confrn1 = 0;
    int64_t t0 = gnw_jpeg_trace_enabled()
                     ? qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : 0;

    if (gnw_h7b0_jpeg_decode_native(data, len, &y, &cb, &cr,
                                     &w, &h, &chroma_w, &chroma_h)) {
        int sof_h = 0, sof_v = 0;
        if (gnw_h7b0_jpeg_parse_sof0_luma_sampling(data, len,
                                                    &sof_h, &sof_v) &&
            sof_h >= 1 && sof_h <= 4 && sof_v >= 1 && sof_v <= 4) {
            uint32_t nb = (uint32_t)(sof_h * sof_v - 1);
            confrn1 = (nb << JPEG_CONFRN_NB_SHIFT) |
                      ((uint32_t)sof_v << JPEG_CONFRN_VSF_SHIFT) |
                      ((uint32_t)sof_h << JPEG_CONFRN_HSF_SHIFT);
        } else {
            confrn1 = 3U << JPEG_CONFRN_NB_SHIFT;
        }
        comp = 3;
    } else {
        stbi_uc *rgb = stbi_load_from_memory(data, len, &w, &h,
                                              &comp, 3);
        if (rgb) {
            int sof_h2 = 0, sof_v2 = 0;
            if (!gnw_h7b0_jpeg_parse_sof0_luma_sampling(
                    data, len, &sof_h2, &sof_v2) ||
                sof_h2 < 1 || sof_h2 > 4 || sof_v2 < 1 || sof_v2 > 4) {
                sof_h2 = 1;
                sof_v2 = 1;
            }
            if (comp == 1) {
                sof_h2 = 1;
                sof_v2 = 1;
            }
            chroma_w = (w + sof_h2 - 1) / sof_h2;
            chroma_h = (h + sof_v2 - 1) / sof_v2;

            int nb_h = 0, nb_v = 0;
            if (gnw_h7b0_jpeg_parse_sof0_luma_sampling(data, len,
                                                        &nb_h, &nb_v) &&
                nb_h >= 1 && nb_h <= 4 && nb_v >= 1 && nb_v <= 4) {
                uint32_t nb = (uint32_t)(nb_h * nb_v - 1);
                confrn1 = (nb << JPEG_CONFRN_NB_SHIFT) |
                          ((uint32_t)nb_v << JPEG_CONFRN_VSF_SHIFT) |
                          ((uint32_t)nb_h << JPEG_CONFRN_HSF_SHIFT);
            } else {
                confrn1 = (comp == 1) ? 0 : (3U << JPEG_CONFRN_NB_SHIFT);
            }

            y = g_malloc(w * h);
            cb = g_malloc(chroma_w * chroma_h);
            cr = g_malloc(chroma_w * chroma_h);

            for (int i = 0; i < w * h; i++) {
                int r = rgb[i * 3 + 0];
                int g = rgb[i * 3 + 1];
                int b = rgb[i * 3 + 2];
                int yy = (299 * r + 587 * g + 114 * b) / 1000;
                y[i] = gnw_h7b0_jpeg_clamp_u8(yy);
            }
            for (int cy = 0; cy < chroma_h; cy++) {
                for (int cx = 0; cx < chroma_w; cx++) {
                    int sx = cx * sof_h2;
                    int sy = cy * sof_v2;
                    if (sx >= w) sx = w - 1;
                    if (sy >= h) sy = h - 1;
                    int idx = sy * w + sx;
                    int r = rgb[idx * 3 + 0];
                    int g = rgb[idx * 3 + 1];
                    int b = rgb[idx * 3 + 2];
                    int cbv = (-168736 * r - 331264 * g + 500000 * b) / 1000000 + 128;
                    int crv = (500000 * r - 418688 * g - 81312 * b) / 1000000 + 128;
                    cb[cy * chroma_w + cx] = gnw_h7b0_jpeg_clamp_u8(cbv);
                    cr[cy * chroma_w + cx] = gnw_h7b0_jpeg_clamp_u8(crv);
                }
            }
            stbi_image_free(rgb);
        }
    }

    if (t0) {
        fprintf(stderr, "JPTDUR %0.2fms in=%u out=%dx%d\n",
                (qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - t0) / 1e6,
                len, w, h);
    }

    o->w = w; o->h = h; o->comp = comp;
    o->chroma_w = chroma_w; o->chroma_h = chroma_h;
    o->y = y; o->cb = cb; o->cr = cr;
    o->confrn1 = confrn1;
}

static void *gnw_h7b0_jpeg_worker_thread(void *opaque)
{
    GnwH7B0JpegState *s = opaque;

    qemu_mutex_lock(&s->thread_lock);
    for (;;) {
        while (!s->job_pending && !s->stop_thread) {
            qemu_cond_wait(&s->thread_cond, &s->thread_lock);
        }
        if (s->stop_thread) {
            break;
        }

        GByteArray *job = s->job_input;
        uint32_t job_epoch = s->job_input_epoch;
        s->job_input = NULL;
        s->job_pending = false;
        s->worker_active = true;
        qemu_mutex_unlock(&s->thread_lock);

        GnwJpegDecodeOut o;
        gnw_h7b0_jpeg_run_decode(job->data, job->len, &o);
        int w = o.w, h = o.h, comp = o.comp;
        int chroma_w = o.chroma_w, chroma_h = o.chroma_h;
        uint8_t *y = o.y, *cb = o.cb, *cr = o.cr;
        uint32_t confrn1 = o.confrn1;
        g_byte_array_free(job, TRUE);

        qemu_mutex_lock(&s->thread_lock);
        g_free(s->pending_y);
        g_free(s->pending_cb);
        g_free(s->pending_cr);
        s->pending_y = y;
        s->pending_cb = cb;
        s->pending_cr = cr;
        s->pending_width = w;
        s->pending_height = h;
        s->pending_chroma_width = chroma_w;
        s->pending_chroma_height = chroma_h;
        s->pending_comp = comp;
        s->pending_confrn1 = confrn1;
        s->pending_epoch = job_epoch;
        qatomic_store_release(&s->decode_done, true);
        s->worker_active = false;
        qemu_cond_broadcast(&s->done_cond);
    }
    qemu_mutex_unlock(&s->thread_lock);
    return NULL;
}

/*
 * Called from the BQL thread (register read handler) to check whether the
 * worker has finished a queued decode, and if so, publish it into the real
 * device-visible state (y/cb/cr_plane, dor_cursor, SR/CONFR* regs). A
 * non-blocking trylock -- if the worker currently holds thread_lock (e.g.
 * mid-publish from its own side), we simply try again on the next poll,
 * same as real EOCF not being set yet.
 */
static void gnw_h7b0_jpeg_poll_worker(GnwH7B0JpegState *s)
{
    if (!s->lock_inited) {
        return;
    }
    /*
     * Lock-free fast path: this poll runs on EVERY register read, and
     * in-game firmware streams full frames through the codec at ~2M
     * reads/s -- a mutex trylock/unlock pair per read was itself a
     * measurable slice of the MMIO cost (qemu_mutex_unlock_impl showed
     * up at ~12% of TCG-thread cycles in perf). decode_done is written
     * by the worker (release) and consumed here (acquire); stale-false
     * just means we publish on a later poll, exactly like trylock
     * failure already did.
     */
    if (!qatomic_load_acquire(&s->decode_done)) {
        return;
    }
    if (qemu_mutex_trylock(&s->thread_lock) != 0) {
        return;
    }
    /*
     * Deliberately NON-blocking. A bounded qemu_cond_timedwait() here
     * (up to 2ms for the in-flight decode) was tried 2026-07-24 to kill
     * the measured ~4M SR polls/s: it did collapse them 16x, but made
     * the launcher 3x SLOWER (26.8ms -> 85.8ms/frame, measured) --
     * retro-go overlaps its own frame work with the polled decode, so
     * blocking the vCPU inside an SR read serializes work the guest
     * intended to run concurrently. Don't reintroduce a wait here.
     */
    if (!s->decode_done) {
        qemu_mutex_unlock(&s->thread_lock);
        return;
    }
    s->decode_done = false;
    s->decode_busy = false;
    gnw_jpeg_lat_decode_done();
    if (gnw_jpeg_trace_enabled()) {
        uint32_t sum = 0;
        if (s->pending_y) {
            for (int i = 0; i < 64; i++) {
                sum = sum * 31 + s->pending_y[i];
            }
        }
        fprintf(stderr, "JPT publish epoch=%u/%u dims=%dx%d ysum=%08x\n",
                s->pending_epoch, s->job_epoch, s->pending_width,
                s->pending_height, sum);
    }
    if (s->pending_epoch != s->job_epoch) {
        /* A reset happened after this job was queued -- discard the
         * stale result instead of publishing decoded content from before
         * the reset into fresh post-reset device state. */
        g_free(s->pending_y);
        g_free(s->pending_cb);
        g_free(s->pending_cr);
        s->pending_y = s->pending_cb = s->pending_cr = NULL;
        qemu_mutex_unlock(&s->thread_lock);
        return;
    }

    g_free(s->y_plane);
    g_free(s->cb_plane);
    g_free(s->cr_plane);
    s->y_plane = s->pending_y;
    s->cb_plane = s->pending_cb;
    s->cr_plane = s->pending_cr;
    /* Ownership moved -- stale pending_* pointers aliasing the live
     * planes caused a real double free once the inline path started
     * g_free()ing pending_* before reuse. */
    s->pending_y = s->pending_cb = s->pending_cr = NULL;
    s->plane_width = s->pending_width;
    s->plane_height = s->pending_height;
    s->chroma_width = s->pending_chroma_width;
    s->chroma_height = s->pending_chroma_height;
    s->dor_cursor = 0;

    uint32_t c1 = s->regs[GNW_H7B0_JPEG_CONFR1_OFFSET >> 2];
    uint32_t nf = (s->pending_comp >= 1 && s->pending_comp <= 4)
                      ? (uint32_t)(s->pending_comp - 1) : 2;
    s->regs[GNW_H7B0_JPEG_CONFR1_OFFSET >> 2] =
        (c1 & ~(JPEG_CONFR1_YSIZE_MASK | JPEG_CONFR1_NF_MASK)) |
        ((uint32_t)s->pending_height << JPEG_CONFR1_YSIZE_SHIFT) | nf;
    uint32_t c3 = s->regs[GNW_H7B0_JPEG_CONFR3_OFFSET >> 2];
    s->regs[GNW_H7B0_JPEG_CONFR3_OFFSET >> 2] =
        (c3 & ~JPEG_CONFR3_XSIZE_MASK) |
        ((uint32_t)s->pending_width << JPEG_CONFR3_XSIZE_SHIFT);
    s->regs[GNW_H7B0_JPEG_CONFRN1_OFFSET >> 2] = s->pending_confrn1;

    s->regs[GNW_H7B0_JPEG_SR_OFFSET >> 2] |= JPEG_SR_EOCF;
    s->regs[GNW_H7B0_JPEG_SR_OFFSET >> 2] &=
        ~(JPEG_SR_OFTF | JPEG_SR_COF | JPEG_SR_IFTF | JPEG_SR_IFNFF);
    if (s->y_plane) {
        uint32_t total0 = (uint32_t)s->plane_width * (uint32_t)s->plane_height +
                          2 * (uint32_t)s->chroma_width * (uint32_t)s->chroma_height;
        s->regs[GNW_H7B0_JPEG_SR_OFFSET >> 2] |= JPEG_SR_OFNEF;
        if (total0 >= 32) {
            s->regs[GNW_H7B0_JPEG_SR_OFFSET >> 2] |= JPEG_SR_OFTF;
        }
    }

    qemu_mutex_unlock(&s->thread_lock);
}

/* BQL held; the worker never acquires BQL. Flags cleared by START/reset
 * do not describe whether a host decode is still computing. */
void gnw_h7b0_jpeg_snapshot_quiesce(GnwH7B0JpegState *s, bool discard)
{
    if (!s->lock_inited) {
        return;
    }
    qemu_mutex_lock(&s->thread_lock);
    if (discard) {
        s->job_epoch++;
    }
    while (s->job_pending || s->worker_active) {
        qemu_cond_wait(&s->done_cond, &s->thread_lock);
    }
    qemu_mutex_unlock(&s->thread_lock);
    gnw_h7b0_jpeg_poll_worker(s);
}

static void gnw_h7b0_jpeg_reset(DeviceState *dev)
{
    GnwH7B0JpegState *s = GNW_H7B0_JPEG(dev);
    for (int i = 0; i < (GNW_H7B0_JPEG_SIZE / 4); i++) {
        s->regs[i] = get_jpeg_reset_value(i * 4);
    }
    if (s->inbuf) {
        g_byte_array_set_size(s->inbuf, 0);
    }
    g_free(s->y_plane);
    g_free(s->cb_plane);
    g_free(s->cr_plane);
    s->y_plane = s->cb_plane = s->cr_plane = NULL;
    s->plane_width = s->plane_height = 0;
    s->chroma_width = s->chroma_height = 0;
    s->dor_cursor = 0;
    if (s->encode_out) {
        g_byte_array_set_size(s->encode_out, 0);
    }
    s->enc_dor_cursor = 0;
    s->decode_busy = false;

    /* Bump the epoch so any in-flight worker job from before this reset
     * gets its result discarded by gnw_h7b0_jpeg_poll_worker() instead of
     * being published into the freshly-reset state above. */
    if (s->lock_inited) {
        qemu_mutex_lock(&s->thread_lock);
        s->job_epoch++;
        qemu_mutex_unlock(&s->thread_lock);
    } else {
        s->job_epoch++;
    }
}

/* Env-gated (GNW_MMIO_RATE): JPEG register reads per host second --
 * the launcher's cover decode is MMIO-bound through this handler, so
 * this rate is a direct cross-platform probe of per-MMIO cost. */
static void gnw_h7b0_jpeg_count_read(hwaddr addr)
{
    static int enabled = -1;
    static int64_t window_start;
    static uint32_t count;
    static uint32_t by_off[16];

    if (enabled < 0) {
        enabled = gnw_env_enabled("GNW_MMIO_RATE");
    }
    if (!enabled) {
        return;
    }
    count++;
    by_off[(addr >> 2) & 0xf]++;
    if ((count & 0x3ff) == 0) {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        if (window_start == 0) {
            window_start = now;
            count = 0;
            memset(by_off, 0, sizeof(by_off));
        } else if (now - window_start >= NANOSECONDS_PER_SECOND) {
            fprintf(stderr, "JPGRD %0.0f/s", count * 1e9 / (now - window_start));
            for (int i = 0; i < 16; i++) {
                if (by_off[i]) {
                    fprintf(stderr, " +0x%x=%u", i * 4, by_off[i]);
                }
            }
            fprintf(stderr, "\n");
            window_start = now;
            count = 0;
            memset(by_off, 0, sizeof(by_off));
        }
    }
}

/*
 * GNW_JPEG_LAT=1: per-decode guest-visible latency and poll count.
 *
 * The decode runs on a HOST worker thread, so how long it takes in
 * GUEST time depends on how fast the host is. The firmware meanwhile
 * busy-polls SR, and each poll is an MMIO exit that takes the BQL --
 * measured at 2.3M/s on Linux and 1.0M/s on a slower Mac. That is a
 * feedback loop: slower host -> longer guest-visible decode -> more
 * polls -> more host work. Real silicon decodes in a fixed time that
 * does not depend on any host, so these two numbers (guest-us per
 * decode, polls per decode) are directly comparable against hardware
 * and say whether the model is faithful.
 */
static uint64_t gnw_jpeg_decode_t0;
static uint64_t gnw_jpeg_polls;
static uint64_t gnw_jpeg_decodes;
static uint64_t gnw_jpeg_lat_sum_us;
static uint64_t gnw_jpeg_lat_max_us;
static uint64_t gnw_jpeg_polls_total;
static int64_t gnw_jpeg_report_t0;

static bool gnw_jpeg_lat_enabled(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("GNW_JPEG_LAT");
        v = (e && *e && strcmp(e, "0") != 0);
    }
    return v;
}

static uint64_t gnw_jpeg_busy_ns;
static uint64_t gnw_jpeg_host_t0;

static void gnw_jpeg_lat_decode_start(void)
{
    if (!gnw_jpeg_lat_enabled()) {
        return;
    }
    gnw_jpeg_decode_t0 = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    gnw_jpeg_host_t0 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    gnw_jpeg_polls = 0;
}

static void gnw_jpeg_lat_decode_done(void)
{
    if (!gnw_jpeg_lat_enabled() || !gnw_jpeg_decode_t0) {
        return;
    }
    uint64_t us = (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) -
                   gnw_jpeg_decode_t0) / 1000;
    if (gnw_jpeg_host_t0) {
        gnw_jpeg_busy_ns += qemu_clock_get_ns(QEMU_CLOCK_REALTIME) -
                            gnw_jpeg_host_t0;
        gnw_jpeg_host_t0 = 0;
    }
    gnw_jpeg_decodes++;
    gnw_jpeg_lat_sum_us += us;
    gnw_jpeg_polls_total += gnw_jpeg_polls;
    if (us > gnw_jpeg_lat_max_us) {
        gnw_jpeg_lat_max_us = us;
    }
    gnw_jpeg_decode_t0 = 0;

    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    if (now - gnw_jpeg_report_t0 >= 1000000000LL && gnw_jpeg_decodes) {
        fprintf(stderr, "JPEGLAT decodes/s=%" PRIu64 " busy_duty=%.1f%% "
                "host_us_per_decode=%" PRIu64 " guest_us_max=%" PRIu64 "\n",
                gnw_jpeg_decodes,
                100.0 * gnw_jpeg_busy_ns / 1e9,
                gnw_jpeg_busy_ns / 1000 / gnw_jpeg_decodes,
                gnw_jpeg_lat_max_us);
        gnw_jpeg_busy_ns = 0;
        gnw_jpeg_report_t0 = now;
        gnw_jpeg_decodes = 0;
        gnw_jpeg_lat_sum_us = 0;
        gnw_jpeg_lat_max_us = 0;
        gnw_jpeg_polls_total = 0;
    }
}

static uint64_t gnw_h7b0_jpeg_read(void *opaque, hwaddr addr, unsigned int size)
{
    if (gnw_jpeg_lat_enabled()) {
        gnw_jpeg_polls++;
    }
    gnw_h7b0_jpeg_count_read(addr);
    GnwH7B0JpegState *s = GNW_H7B0_JPEG(opaque);
    /* Opportunistically publish a completed background decode -- mirrors
     * firmware polling SR itself; every register read is a chance to
     * notice the worker finished. */
    gnw_h7b0_jpeg_poll_worker(s);
    if (addr >= GNW_H7B0_JPEG_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n", __func__, addr);
        return 0;
    }
    if (addr == GNW_H7B0_JPEG_DOR_OFFSET) {
        bool decode_mode_dor =
            (s->regs[GNW_H7B0_JPEG_CONFR1_OFFSET >> 2] & JPEG_CONFR1_DE) != 0;
        if (!decode_mode_dor) {
            /* Encode mode: drain the real encoded JPEG bitstream produced
             * by the DIR write handler's encode path, same flat-cursor
             * style as the decode side below but over encode_out. */
            uint32_t total = s->encode_out ? s->encode_out->len : 0;
            uint32_t v = 0;

            if (total == 0) {
                return 0;
            }
            for (int i = 0; i < 4; i++) {
                uint32_t pos = s->enc_dor_cursor + i;
                uint8_t byte = (pos < total) ? s->encode_out->data[pos] : 0;
                v |= ((uint32_t)byte) << (8 * i);
            }
            if (s->enc_dor_cursor < total) {
                s->enc_dor_cursor += 4;
            }
            uint32_t remaining = (s->enc_dor_cursor < total) ? (total - s->enc_dor_cursor) : 0;
            if (remaining >= 32) {
                s->regs[GNW_H7B0_JPEG_SR_OFFSET >> 2] |= JPEG_SR_OFTF;
            } else {
                s->regs[GNW_H7B0_JPEG_SR_OFFSET >> 2] &= ~JPEG_SR_OFTF;
            }
            if (remaining == 0) {
                s->regs[GNW_H7B0_JPEG_SR_OFFSET >> 2] &= ~JPEG_SR_OFNEF;
            }
            return v;
        }
        if (gnw_jpeg_trace_enabled() && s->dor_cursor == 0 && s->y_plane) {
            uint32_t sum = 0;
            for (int i = 0; i < 64; i++) {
                sum = sum * 31 + s->y_plane[i];
            }
            fprintf(stderr, "JPT drain0 dims=%dx%d ysum=%08x\n",
                    s->plane_width, s->plane_height, sum);
        }
        /* Real DOR: pop the next 4 bytes from a flat cursor over
         * y_plane||cb_plane||cr_plane, little-endian-packed like the
         * existing DIR write path. Entirely synchronous, no timers --
         * mirrors the DIR register's own instant-completion style. */
        uint32_t y_size = (uint32_t)s->plane_width * (uint32_t)s->plane_height;
        uint32_t c_size = (uint32_t)s->chroma_width * (uint32_t)s->chroma_height;
        uint32_t total = y_size + 2 * c_size;
        uint32_t v = 0;

        if (total == 0 || !s->y_plane) {
            return 0;
        }
        for (int i = 0; i < 4; i++) {
            uint32_t pos = s->dor_cursor + i;
            uint8_t byte = 0;
            if (pos < total) {
                if (pos < y_size) {
                    byte = s->y_plane[pos];
                } else if (pos < y_size + c_size) {
                    byte = s->cb_plane[pos - y_size];
                } else {
                    byte = s->cr_plane[pos - y_size - c_size];
                }
            }
            v |= ((uint32_t)byte) << (8 * i);
        }
        if (s->dor_cursor < total) {
            s->dor_cursor += 4;
        }
        /*
         * Real hardware's output FIFO threshold (JPEG_FIFO_TH_SIZE = 8
         * words = 32 bytes, stm32h7xx_hal_jpeg.c) lets firmware's polling
         * loop drain 8 words per outer-loop iteration via OFTF instead of
         * checking flags and reading DOR one word at a time via OFNEF.
         * The total number of DOR reads is identical either way (real
         * HAL's JPEG_StoreOutputData() still reads DOR in a plain word-at-
         * a-time loop internally regardless of threshold) -- but never
         * setting OFTF here forced every single decode into the ~8x-more-
         * outer-loop-iterations OFNEF path, each iteration paying its own
         * separate SR-flag-check MMIO round trip on top of the DOR read
         * itself. Real cover-art-heavy screens (coverflow/menus) do this
         * thousands of times per frame, so that per-iteration MMIO/BQL
         * overhead was a genuine, measurable performance bug (perf showed
         * >16% total CPU in QEMU's own MMIO dispatch/lock path during a
         * menu scroll-loop) -- not just a cosmetic fidelity gap.
         */
        uint32_t remaining = (s->dor_cursor < total) ? (total - s->dor_cursor) : 0;
        if (remaining >= 32) {
            s->regs[GNW_H7B0_JPEG_SR_OFFSET >> 2] |= JPEG_SR_OFTF;
        } else {
            s->regs[GNW_H7B0_JPEG_SR_OFFSET >> 2] &= ~JPEG_SR_OFTF;
        }
        if (remaining == 0) {
            s->regs[GNW_H7B0_JPEG_SR_OFFSET >> 2] &= ~JPEG_SR_OFNEF;
        }
        return v;
    }
    return s->regs[addr >> 2];
}

static void gnw_h7b0_jpeg_write(void *opaque, hwaddr addr, uint64_t val64, unsigned int size)
{
    GnwH7B0JpegState *s = GNW_H7B0_JPEG(opaque);
    if (addr >= GNW_H7B0_JPEG_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n", __func__, addr);
        return;
    }
    uint32_t mask = get_jpeg_write_mask(addr);
    s->regs[addr >> 2] = (s->regs[addr >> 2] & ~mask) | ((uint32_t)val64 & mask);

    /*
     * SR/CFR (0x34/0x38) are otherwise a plain shadow with no decode/encode
     * side effects (this is a dumb RAM-backed stub, not a real JPEG codec).
     * Real firmware's decode-completion poll loop (HAL's JPEG_Process(), or
     * a hand-rolled equivalent) waits on SR.EOCF going high and SR.OFNEF/
     * OFTF going low to know the operation is done. With EOCF permanently
     * stuck at 0 (its reset value) that loop -- and any "drain the output
     * FIFO while OFNEF is set" inner loop alongside it -- never terminates,
     * walking an output pointer straight off the end of whatever SRAM
     * buffer it's writing into. Found via retro-go's cover-art JPEG decode:
     * a real BusFault reading one byte past the very end of AXISRAM1-3
     * (0x24100000), with the fault's LR sitting on JPEG's own MMIO base
     * address.
     *
     * The actual "begin this operation" trigger is CONFR0.START (bit 0),
     * NOT CR -- CR is codec-core-enable (JCEN) plus interrupt-enable bits,
     * set once up front and left alone; CONFR0.START is written low->high
     * per operation and cleared again by JPEG_Process() itself once it
     * observes completion (see stm32h7xx_hal_jpeg.c's
     * "hjpeg->Instance->CONFR0 &= ~JPEG_CONFR0_START"). Faking an
     * immediate, empty "done" completion whenever START goes high (real
     * EOCF semantics, just instant rather than after real codec work) is
     * enough for any real polling loop to see the process as finished and
     * stop -- the decoded output content will be all-zero garbage (there's
     * no real codec here), but that's a correctness shortfall for the
     * eventual real JPEG model, not a hang or a crash.
     *
     * SR's IFTF/IFNFF ("input FIFO has room") are also both set in its
     * reset value (0x6) and nothing ever clears them -- real hardware's
     * input FIFO exerts real backpressure once full, clearing these, but
     * ours never does. That alone (independent of EOCF/output handling
     * above) makes JPEG_Process()'s input-refill branch
     * (JPEG_ReadInputData()) fire on every single poll forever, walking
     * its source buffer pointer off the end of AXISRAM once the driver's
     * own end-of-data bookkeeping gets out of sync with a peripheral that
     * never stops asking for more. Clearing them alongside EOCF -- "done,
     * not accepting more input either" -- closes that off the same way.
     */
    if (addr == GNW_H7B0_JPEG_CONFR0_OFFSET && (val64 & JPEG_CONFR0_START)) {
        gnw_h7b0_jpeg_snapshot_quiesce(s, true);
        if (s->inbuf) {
            g_byte_array_set_size(s->inbuf, 0);
        }
        /*
         * BUG FIX: also clear OFNEF/OFTF/COF here, not just EOCF -- found
         * via a real second-decode-on-the-same-device-instance failure
         * once decode moved to the async worker thread (see
         * gnw_h7b0_jpeg_worker_thread()/gnw_h7b0_jpeg_poll_worker()
         * above). The old fully-synchronous decode never needed this:
         * decode completed inside the very same MMIO write that
         * triggered it, so there was no window where a previous decode's
         * "output ready" flags could be read as still valid. With an
         * async worker, that window is now real -- between this START
         * and the eventual poll_worker() publish, OFNEF/OFTF (left set
         * from whatever the *previous* decode on this device published)
         * would otherwise still read as "output available", letting
         * firmware start draining DOR immediately and get stale content
         * from the previous image instead of waiting for EOCF. dor_cursor
         * is reset too, for the same reason (a previous decode may have
         * left it non-zero, or mid-drain).
         */
        s->regs[GNW_H7B0_JPEG_SR_OFFSET >> 2] &=
            ~(JPEG_SR_EOCF | JPEG_SR_OFNEF | JPEG_SR_OFTF | JPEG_SR_COF);
        s->regs[GNW_H7B0_JPEG_SR_OFFSET >> 2] |= (JPEG_SR_IFTF | JPEG_SR_IFNFF);
        s->dor_cursor = 0;
        /*
         * A fresh START must also orphan any decode job still queued or
         * in flight from a previous operation -- otherwise its planes
         * publish into THIS decode and firmware reads the previous
         * image's content out of DOR (seen live as retro-go coverflow
         * drawing a cover that belongs two positions back). Same epoch
         * mechanism device reset uses.
         */
        s->decode_busy = false;
        if (s->lock_inited) {
            qemu_mutex_lock(&s->thread_lock);
            s->job_epoch++;
            qemu_mutex_unlock(&s->thread_lock);
        } else {
            s->job_epoch++;
        }
        if (gnw_jpeg_trace_enabled()) {
            fprintf(stderr, "JPT start epoch=%u\n", s->job_epoch);
        }
    } else if (addr == GNW_H7B0_JPEG_CR_OFFSET) {
        /* IFF/OFF (input/output FIFO flush) are real pulse bits -- the SVD
         * documents them as "always read as 0". Mirror the self-clearing
         * pattern used elsewhere in this codebase (e.g. gnw_h7b0_crc.c's
         * RESET bit) by immediately clearing them back out of the shadow
         * register right after the generic masked write above latches them. */
        s->regs[GNW_H7B0_JPEG_CR_OFFSET >> 2] &= ~(uint32_t)JPEG_CR_FLUSH_PULSE_MASK;
    } else if (addr == GNW_H7B0_JPEG_CFR_OFFSET) {
        /* CFR is a real write-1-to-clear pulse register (its bit positions
         * mirror SR's exactly): clear the matching SR bits, then reset CFR
         * itself back to 0 rather than leaving the generic shadow-write
         * value latched (real hardware doesn't persist it either). */
        s->regs[GNW_H7B0_JPEG_SR_OFFSET >> 2] &= ~(uint32_t)val64;
        s->regs[GNW_H7B0_JPEG_CFR_OFFSET >> 2] = 0;
    } else if (addr == GNW_H7B0_JPEG_DIR_OFFSET &&
               (s->regs[GNW_H7B0_JPEG_CONFR1_OFFSET >> 2] & JPEG_CONFR1_DE) == 0) {
        /*
         * Encode mode (CONFR1.DE clear): accumulate raw, already-YCbCr,
         * pixel-interleaved bytes (confirmed via sdk/stm32h7xx-hal-driver/
         * Src/stm32h7xx_hal_jpeg.c -- HAL_JPEG_Encode()'s JPEG_ReadInputData()
         * just pumps pDataInMCU into DIR word-at-a-time with no color
         * conversion of its own; the real codec IP only ever does
         * DCT+quant+Huffman) until a full image's worth has arrived, then
         * run a real encode. Only 3-component (YCbCr) 4:4:4 is supported --
         * the only mode this repo's diag suite's JPEG encode cases use.
         */
        if (s->inbuf) {
            uint32_t v = (uint32_t)val64;
            g_byte_array_append(s->inbuf, (const guint8 *)&v, 4);

            uint32_t c1 = s->regs[GNW_H7B0_JPEG_CONFR1_OFFSET >> 2];
            uint32_t c3 = s->regs[GNW_H7B0_JPEG_CONFR3_OFFSET >> 2];
            uint32_t width = (c3 & JPEG_CONFR3_XSIZE_MASK) >> JPEG_CONFR3_XSIZE_SHIFT;
            uint32_t height = (c1 & JPEG_CONFR1_YSIZE_MASK) >> JPEG_CONFR1_YSIZE_SHIFT;
            uint32_t nf = (c1 & JPEG_CONFR1_NF_MASK) + 1;

            if (width > 0 && height > 0 && nf == 3) {
                uint32_t needed = width * height * 3;
                if (s->inbuf->len >= needed) {
                    uint8_t *y_buf = g_malloc(width * height);
                    uint8_t *cb_buf = g_malloc(width * height);
                    uint8_t *cr_buf = g_malloc(width * height);
                    for (uint32_t i = 0; i < width * height; i++) {
                        y_buf[i] = s->inbuf->data[i * 3 + 0];
                        cb_buf[i] = s->inbuf->data[i * 3 + 1];
                        cr_buf[i] = s->inbuf->data[i * 3 + 2];
                    }
                    if (!s->encode_out) {
                        s->encode_out = g_byte_array_new();
                    }
                    gnw_h7b0_jpeg_encode_ycbcr444(y_buf, cb_buf, cr_buf,
                                                  (int)width, (int)height, s->encode_out);
                    g_free(y_buf);
                    g_free(cb_buf);
                    g_free(cr_buf);

                    g_byte_array_set_size(s->inbuf, 0);
                    s->enc_dor_cursor = 0;

                    s->regs[GNW_H7B0_JPEG_SR_OFFSET >> 2] |= JPEG_SR_EOCF;
                    s->regs[GNW_H7B0_JPEG_SR_OFFSET >> 2] &=
                        ~(JPEG_SR_COF | JPEG_SR_IFTF | JPEG_SR_IFNFF);
                    uint32_t total = s->encode_out->len;
                    if (total > 0) {
                        s->regs[GNW_H7B0_JPEG_SR_OFFSET >> 2] |= JPEG_SR_OFNEF;
                        if (total >= 32) {
                            s->regs[GNW_H7B0_JPEG_SR_OFFSET >> 2] |= JPEG_SR_OFTF;
                        }
                    }
                }
            } else if (width > 0 && height > 0) {
                qemu_log_mask(LOG_UNIMP,
                              "%s: JPEG encode with NF=%u unsupported "
                              "(only 3-component YCbCr 4:4:4 implemented)\n",
                              __func__, nf);
            }
        }
    } else if (addr == GNW_H7B0_JPEG_DIR_OFFSET) {
        /*
         * A decode job is already posted for this operation: the real
         * codec's input FIFO stops requesting data after EOI, so any
         * further DIR writes are excess bytes from a HAL feed loop that
         * raced the (async) decode. Accepting them would EOI-scan
         * whatever garbage follows the real bitstream in guest RAM and
         * could post a spurious second job (whose failed decode then
         * publishes black planes over the real result). Drop them.
         */
        if (s->decode_busy) {
            return;
        }
        if (s->inbuf) {
            uint32_t v = (uint32_t)val64;
            g_byte_array_append(s->inbuf, (const guint8 *)&v, 4);

            if (!(s->regs[GNW_H7B0_JPEG_SR_OFFSET >> 2] & (1U << 6))) {
                int w, h, comp;
                if (stbi_info_from_memory(s->inbuf->data, s->inbuf->len, &w, &h, &comp)) {
                    s->regs[GNW_H7B0_JPEG_SR_OFFSET >> 2] |= (1U << 6); /* HPDF */

                    /* NF = number of color components - 1 (CONFR1 field
                     * semantics per the SVD), derived from the real decoded
                     * component count instead of a hardcoded "always 3
                     * components" placeholder. JPEG components are 1
                     * (grayscale) or 3 (YCbCr) in the overwhelming common
                     * case; clamp defensively for the rare/invalid case. */
                    uint32_t nf = (comp >= 1 && comp <= 4) ? (uint32_t)(comp - 1) : 2;
                    uint32_t c1 = s->regs[GNW_H7B0_JPEG_CONFR1_OFFSET >> 2];
                    s->regs[GNW_H7B0_JPEG_CONFR1_OFFSET >> 2] =
                        (c1 & ~(JPEG_CONFR1_YSIZE_MASK | JPEG_CONFR1_NF_MASK)) |
                        ((uint32_t)h << JPEG_CONFR1_YSIZE_SHIFT) | nf;

                    uint32_t c3 = s->regs[GNW_H7B0_JPEG_CONFR3_OFFSET >> 2];
                    s->regs[GNW_H7B0_JPEG_CONFR3_OFFSET >> 2] =
                        (c3 & ~JPEG_CONFR3_XSIZE_MASK) | ((uint32_t)w << JPEG_CONFR3_XSIZE_SHIFT);

                    /* CONFRN1 (component 1 / luma) NB/HSF/VSF fields: real
                     * per-image sampling factors, hand-parsed from the SOF0
                     * marker (stbi_info_from_memory() doesn't expose these).
                     * NB (data units - 1 per MCU) = H*V - 1 for luma. Falls
                     * back to the previous approximation (comp==1: no
                     * subsampling; comp==3: assume common 4:2:0 => NB=3) if
                     * SOF0 parsing fails for any reason. */
                    int sof_h = 0, sof_v = 0;
                    uint32_t confrn1;
                    if (gnw_h7b0_jpeg_parse_sof0_luma_sampling(s->inbuf->data,
                                                                s->inbuf->len,
                                                                &sof_h, &sof_v) &&
                        sof_h >= 1 && sof_h <= 4 && sof_v >= 1 && sof_v <= 4) {
                        uint32_t nb = (uint32_t)(sof_h * sof_v - 1);
                        confrn1 = (nb << JPEG_CONFRN_NB_SHIFT) |
                                  ((uint32_t)sof_v << JPEG_CONFRN_VSF_SHIFT) |
                                  ((uint32_t)sof_h << JPEG_CONFRN_HSF_SHIFT);
                    } else {
                        confrn1 = (comp == 1) ? 0 : (3U << JPEG_CONFRN_NB_SHIFT);
                    }
                    s->regs[GNW_H7B0_JPEG_CONFRN1_OFFSET >> 2] = confrn1;
                }
            }

            if (s->inbuf->len >= 2) {
                bool found_eoi = false;
                for (int i = 0; i < MIN(s->inbuf->len - 1, 4); i++) {
                    if (s->inbuf->data[s->inbuf->len - 2 - i] == 0xFF &&
                        s->inbuf->data[s->inbuf->len - 1 - i] == 0xD9) {
                        found_eoi = true;
                        break;
                    }
                }
                if (found_eoi) {
                    /*
                     * Hand the real decode + YCbCr conversion (the
                     * expensive scalar compute) off to a worker thread
                     * instead of running it inline here on the BQL
                     * thread -- see gnw_h7b0_jpeg_worker_thread()/
                     * gnw_h7b0_jpeg_poll_worker() above. This does not
                     * speed up firmware's own polling loop (it has
                     * nothing else useful to do while it waits either
                     * way), but it stops a real multi-millisecond scalar
                     * JPEG decode from stalling BQL -- and therefore
                     * every other main-loop consumer (audio pacing,
                     * display-refresh timers, gdbstub) -- for its whole
                     * duration.
                     *
                     * EOCF/OFNEF/OFTF are left untouched here (whatever
                     * they were before this write) until
                     * gnw_h7b0_jpeg_poll_worker() observes completion and
                     * publishes it on a later register read, exactly
                     * mirroring how a real, still-busy codec would look
                     * to a polling loop.
                     */
                    if (!s->lock_inited) {
                        qemu_mutex_init(&s->thread_lock);
                        qemu_cond_init(&s->thread_cond);
                        qemu_cond_init(&s->done_cond);
                        s->lock_inited = true;
                    }

                    if (s->timing_mode ||
                        s->inbuf->len <= GNW_H7B0_JPEG_INLINE_MAX_BYTES) {
                        /*
                         * Small input (or any input in opt-in timing
                         * mode): decode NOW, inline on the BQL
                         * thread, so the result is already published
                         * when firmware's very first SR poll lands --
                         * eliminating the entire wait-poll phase
                         * (measured at ~40% of all JPEG MMIO traffic,
                         * the launcher/stock-background hot path on
                         * every platform). The worker thread predates
                         * the DCT-table fix, when a decode cost 8ms+
                         * and stalling BQL that long was unacceptable;
                         * post-fix a thumbnail/background decode is
                         * ~0.4-2ms, comparable to BQL holds we already
                         * tolerate elsewhere. Large inputs still go to
                         * the worker below.
                         */
                        GnwJpegDecodeOut o;
                        gnw_h7b0_jpeg_run_decode(s->inbuf->data,
                                                 s->inbuf->len, &o);
                        qemu_mutex_lock(&s->thread_lock);
                        g_free(s->pending_y);
                        g_free(s->pending_cb);
                        g_free(s->pending_cr);
                        s->pending_y = o.y;
                        s->pending_cb = o.cb;
                        s->pending_cr = o.cr;
                        s->pending_width = o.w;
                        s->pending_height = o.h;
                        s->pending_chroma_width = o.chroma_w;
                        s->pending_chroma_height = o.chroma_h;
                        s->pending_comp = o.comp;
                        s->pending_confrn1 = o.confrn1;
                        s->pending_epoch = s->job_epoch;
                        qatomic_store_release(&s->decode_done, true);
                        qemu_mutex_unlock(&s->thread_lock);
                        s->decode_busy = true;
                        gnw_jpeg_lat_decode_start();
                        s->regs[GNW_H7B0_JPEG_SR_OFFSET >> 2] &=
                            ~(JPEG_SR_IFTF | JPEG_SR_IFNFF);
                        /* Publish immediately -- SR shows EOCF/OFNEF
                         * before this write handler even returns. */
                        gnw_h7b0_jpeg_poll_worker(s);
                        if (gnw_jpeg_trace_enabled()) {
                            fprintf(stderr, "JPT inline len=%u epoch=%u\n",
                                    s->inbuf->len, s->job_epoch);
                        }
                        goto input_consumed;
                    }

                    if (!s->thread_started) {
                        qemu_thread_create(&s->thread, "gnw-h7b0-jpeg-worker",
                                           gnw_h7b0_jpeg_worker_thread, s,
                                           QEMU_THREAD_JOINABLE);
                        s->thread_started = true;
                    }

                    qemu_mutex_lock(&s->thread_lock);
                    if (s->job_input) {
                        /* A previous job is still queued/in flight (should
                         * not happen given firmware's own decode-then-wait
                         * sequencing, but don't leak if it somehow does). */
                        g_byte_array_free(s->job_input, TRUE);
                    }
                    s->job_input = g_byte_array_sized_new(s->inbuf->len);
                    g_byte_array_append(s->job_input, s->inbuf->data, s->inbuf->len);
                    s->job_input_epoch = s->job_epoch;
                    s->job_pending = true;
                    qemu_cond_signal(&s->thread_cond);
                    qemu_mutex_unlock(&s->thread_lock);
                    /*
                     * Input is complete: real hardware's FIFO stops
                     * requesting data now (and HAL's JPEG_ReadInputData()
                     * checks IFTF before every refill). Leaving these set
                     * while the worker runs let the feed loop pump its
                     * source pointer right off the end of AXISRAM --
                     * firmware passes its full buffer SIZE as InDataLength
                     * and relies on this backpressure + EOC to stop early
                     * (seen live as a BusFault/BSOD in JPEG_ReadInputData
                     * during retro-go coverflow).
                     */
                    s->decode_busy = true;
                    s->regs[GNW_H7B0_JPEG_SR_OFFSET >> 2] &=
                        ~(JPEG_SR_IFTF | JPEG_SR_IFNFF);
                    if (gnw_jpeg_trace_enabled()) {
                        fprintf(stderr, "JPT post len=%u epoch=%u\n",
                                s->job_input ? s->job_input->len : 0, s->job_epoch);
                    }
                    input_consumed: ;
                }
            }
        }
    }
}

static const MemoryRegionOps gnw_h7b0_jpeg_ops = {
    .read = gnw_h7b0_jpeg_read,
    .write = gnw_h7b0_jpeg_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static const Property gnw_h7b0_jpeg_properties[] = {
    DEFINE_PROP_BOOL("timing-mode", GnwH7B0JpegState, timing_mode, false),
};

static void gnw_h7b0_jpeg_init(Object *obj)
{
    GnwH7B0JpegState *s = GNW_H7B0_JPEG(obj);
    memory_region_init_io(&s->mmio, obj, &gnw_h7b0_jpeg_ops, s, TYPE_GNW_H7B0_JPEG, GNW_H7B0_JPEG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    s->inbuf = g_byte_array_new();
    s->encode_out = g_byte_array_new();
    global_jpeg_state = s;
}

static void gnw_h7b0_jpeg_finalize(Object *obj)
{
    GnwH7B0JpegState *s = GNW_H7B0_JPEG(obj);
    if (s->thread_started) {
        qemu_mutex_lock(&s->thread_lock);
        s->stop_thread = true;
        qemu_cond_signal(&s->thread_cond);
        qemu_mutex_unlock(&s->thread_lock);
        qemu_thread_join(&s->thread);
    }
    if (s->lock_inited) {
        qemu_mutex_destroy(&s->thread_lock);
        qemu_cond_destroy(&s->thread_cond);
        qemu_cond_destroy(&s->done_cond);
    }
    if (s->job_input) {
        g_byte_array_free(s->job_input, TRUE);
    }
    g_free(s->pending_y);
    g_free(s->pending_cb);
    g_free(s->pending_cr);
    g_free(s->y_plane);
    g_free(s->cb_plane);
    g_free(s->cr_plane);
    g_byte_array_unref(s->inbuf);
    g_byte_array_unref(s->encode_out);
}

/* All lengths are checked together before allocation, including product
 * overflow and cursor rounding for a last partially filled DOR word. */
#define JPEG_STATE_MAX_BYTES (64U * 1024 * 1024)

static bool jpeg_state_sizes(uint32_t w, uint32_t h, uint32_t cw, uint32_t ch,
                             uint32_t cursor, uint32_t enc_cursor,
                             const uint32_t lengths[5])
{
    uint64_t y = (uint64_t)w * h, c = (uint64_t)cw * ch;
    uint64_t total = 0;

    if (w > 65535 || h > 65535 || cw > w || ch > h ||
        (!!w != !!h) || (!!cw != !!ch) ||
        lengths[2] != y || lengths[3] != c || lengths[4] != c ||
        cursor > y + 2 * c + 3 || enc_cursor > (uint64_t)lengths[1] + 3) {
        return false;
    }
    for (int i = 0; i < 5; i++) {
        total += lengths[i];
    }
    return total <= JPEG_STATE_MAX_BYTES;
}

static int jpeg_pre_save(void *opaque)
{
    GnwH7B0JpegState *s = opaque;
    uint32_t lengths[5];

    gnw_h7b0_jpeg_snapshot_quiesce(s, false);
    lengths[0] = s->inbuf->len;
    lengths[1] = s->encode_out->len;
    lengths[2] = (uint64_t)s->plane_width * s->plane_height;
    lengths[3] = lengths[4] = (uint64_t)s->chroma_width * s->chroma_height;
    if (!jpeg_state_sizes(s->plane_width, s->plane_height,
                          s->chroma_width, s->chroma_height,
                          s->dor_cursor, s->enc_dor_cursor, lengths) ||
        (lengths[2] && !s->y_plane) ||
        (lengths[3] && (!s->cb_plane || !s->cr_plane))) {
        return -EINVAL;
    }
    return 0;
}

static int jpeg_pre_load(void *opaque)
{
    gnw_h7b0_jpeg_snapshot_quiesce(opaque, true);
    return 0;
}

static int jpeg_put_runtime(QEMUFile *f, void *pv, size_t size,
                             const VMStateField *field, JSONWriter *vmdesc)
{
    GnwH7B0JpegState *s = pv;
    uint32_t lengths[5] = {s->inbuf->len, s->encode_out->len,
        s->plane_width * s->plane_height,
        s->chroma_width * s->chroma_height,
        s->chroma_width * s->chroma_height};
    const uint8_t *buffers[5] = {s->inbuf->data, s->encode_out->data,
                                s->y_plane, s->cb_plane, s->cr_plane};

    qemu_put_be32(f, s->plane_width);
    qemu_put_be32(f, s->plane_height);
    qemu_put_be32(f, s->chroma_width);
    qemu_put_be32(f, s->chroma_height);
    qemu_put_be32(f, s->dor_cursor);
    qemu_put_be32(f, s->enc_dor_cursor);
    for (int i = 0; i < 5; i++) {
        qemu_put_be32(f, lengths[i]);
    }
    for (int i = 0; i < 5; i++) {
        if (lengths[i]) {
            qemu_put_buffer(f, buffers[i], lengths[i]);
        }
    }
    return qemu_file_get_error(f);
}

static int jpeg_get_runtime(QEMUFile *f, void *pv, size_t size,
                             const VMStateField *field)
{
    GnwH7B0JpegState *s = pv;
    uint32_t w = qemu_get_be32(f), h = qemu_get_be32(f);
    uint32_t cw = qemu_get_be32(f), ch = qemu_get_be32(f);
    uint32_t cursor = qemu_get_be32(f), enc_cursor = qemu_get_be32(f);
    uint32_t lengths[5];
    uint8_t *planes[3] = {NULL};

    for (int i = 0; i < 5; i++) {
        lengths[i] = qemu_get_be32(f);
    }
    if (qemu_file_get_error(f) ||
        !jpeg_state_sizes(w, h, cw, ch, cursor, enc_cursor, lengths)) {
        return -EINVAL;
    }
    for (int i = 0; i < 3; i++) {
        if (lengths[i + 2]) {
            planes[i] = g_try_malloc(lengths[i + 2]);
            if (!planes[i]) {
                for (int j = 0; j < 3; j++) {
                    g_free(planes[j]);
                }
                return -ENOMEM;
            }
        }
    }
    g_byte_array_set_size(s->inbuf, lengths[0]);
    g_byte_array_set_size(s->encode_out, lengths[1]);
    uint8_t *buffers[5] = {s->inbuf->data, s->encode_out->data,
                           planes[0], planes[1], planes[2]};
    for (int i = 0; i < 5; i++) {
        if (lengths[i] && qemu_get_buffer(f, buffers[i], lengths[i]) != lengths[i]) {
            for (int j = 0; j < 3; j++) {
                g_free(planes[j]);
            }
            return -EIO;
        }
    }
    g_free(s->y_plane);
    g_free(s->cb_plane);
    g_free(s->cr_plane);
    s->y_plane = planes[0]; s->cb_plane = planes[1]; s->cr_plane = planes[2];
    s->plane_width = w; s->plane_height = h;
    s->chroma_width = cw; s->chroma_height = ch;
    s->dor_cursor = cursor; s->enc_dor_cursor = enc_cursor;
    s->decode_busy = false;
    return 0;
}

static const VMStateInfo jpeg_runtime_info = {
    .name = "jpeg-runtime",
    .get = jpeg_get_runtime,
    .put = jpeg_put_runtime,
};

static const VMStateDescription vmstate_gnw_h7b0_jpeg = {
    .name = TYPE_GNW_H7B0_JPEG,
    .version_id = 2,
    .minimum_version_id = 1,
    .pre_save = jpeg_pre_save,
    .pre_load = jpeg_pre_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, GnwH7B0JpegState, GNW_H7B0_JPEG_SIZE / 4),
        { .name = "runtime", .version_id = 2, .size = sizeof(GnwH7B0JpegState),
          .info = &jpeg_runtime_info, .flags = VMS_SINGLE },
        VMSTATE_END_OF_LIST()
    }
};

static void gnw_h7b0_jpeg_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->vmsd = &vmstate_gnw_h7b0_jpeg;
    device_class_set_legacy_reset(dc, gnw_h7b0_jpeg_reset);
    device_class_set_props(dc, gnw_h7b0_jpeg_properties);
}

static const TypeInfo gnw_h7b0_jpeg_info = {
    .name          = TYPE_GNW_H7B0_JPEG,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GnwH7B0JpegState),
    .instance_init = gnw_h7b0_jpeg_init,
    .instance_finalize = gnw_h7b0_jpeg_finalize,
    .class_init    = gnw_h7b0_jpeg_class_init,
};

static void gnw_h7b0_jpeg_register_types(void)
{
    type_register_static(&gnw_h7b0_jpeg_info);
}
type_init(gnw_h7b0_jpeg_register_types)
