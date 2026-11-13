/*
 * STM32H7B0 CRYP (AES crypto processor) model (Nintendo Game & Watch)
 *
 * See gnw_h7b0_cryp.h for scope/rationale.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/host-utils.h"
#include "migration/vmstate.h"
#include "hw/core/irq.h"
#include "hw/misc/gnw_h7b0_cryp.h"

/* ------------------------------------------------------------------ */
/* Minimal standalone AES-128/192/256 (encrypt + decrypt) core.        */
/* ------------------------------------------------------------------ */

static const uint8_t aes_sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

static const uint8_t aes_rsbox[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d,
};

static const uint8_t aes_rcon[15] = {
    0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36,0x6c,0xd8,0xab,0x4d,0x9a,
};

static uint8_t gmul(uint8_t a, uint8_t b)
{
    uint8_t p = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1) {
            p ^= a;
        }
        bool hi = a & 0x80;
        a <<= 1;
        if (hi) {
            a ^= 0x1b;
        }
        b >>= 1;
    }
    return p;
}

/* key: Nk*4 bytes; fills round_keys[0..Nr][16] and *num_rounds. */
static void aes_key_expansion(const uint8_t *key, int key_bytes,
                               uint8_t round_keys[15][16], int *num_rounds)
{
    int nk = key_bytes / 4;
    int nr = nk + 6;
    uint32_t w[60]; /* max 4*(14+1) = 60 words */
    *num_rounds = nr;

    for (int i = 0; i < nk; i++) {
        w[i] = (key[4 * i] << 24) | (key[4 * i + 1] << 16) |
               (key[4 * i + 2] << 8) | key[4 * i + 3];
    }
    for (int i = nk; i < 4 * (nr + 1); i++) {
        uint32_t temp = w[i - 1];
        if (i % nk == 0) {
            temp = (temp << 8) | (temp >> 24); /* RotWord */
            temp = (aes_sbox[(temp >> 24) & 0xFF] << 24) |
                   (aes_sbox[(temp >> 16) & 0xFF] << 16) |
                   (aes_sbox[(temp >> 8) & 0xFF] << 8) |
                   aes_sbox[temp & 0xFF];
            temp ^= (uint32_t)aes_rcon[i / nk - 1] << 24;
        } else if (nk > 6 && i % nk == 4) {
            temp = (aes_sbox[(temp >> 24) & 0xFF] << 24) |
                   (aes_sbox[(temp >> 16) & 0xFF] << 16) |
                   (aes_sbox[(temp >> 8) & 0xFF] << 8) |
                   aes_sbox[temp & 0xFF];
        }
        w[i] = w[i - nk] ^ temp;
    }
    for (int r = 0; r <= nr; r++) {
        for (int c = 0; c < 4; c++) {
            uint32_t word = w[4 * r + c];
            round_keys[r][4 * c] = (word >> 24) & 0xFF;
            round_keys[r][4 * c + 1] = (word >> 16) & 0xFF;
            round_keys[r][4 * c + 2] = (word >> 8) & 0xFF;
            round_keys[r][4 * c + 3] = word & 0xFF;
        }
    }
}

static void aes_add_round_key(uint8_t s[16], const uint8_t rk[16])
{
    for (int i = 0; i < 16; i++) {
        s[i] ^= rk[i];
    }
}

static void aes_sub_bytes(uint8_t s[16])
{
    for (int i = 0; i < 16; i++) {
        s[i] = aes_sbox[s[i]];
    }
}

static void aes_inv_sub_bytes(uint8_t s[16])
{
    for (int i = 0; i < 16; i++) {
        s[i] = aes_rsbox[s[i]];
    }
}

/* State is column-major: byte index = col*4 + row. */
static void aes_shift_rows(uint8_t s[16])
{
    uint8_t t;
    /* row 1: shift left 1 */
    t = s[1]; s[1] = s[5]; s[5] = s[9]; s[9] = s[13]; s[13] = t;
    /* row 2: shift left 2 */
    t = s[2]; s[2] = s[10]; s[10] = t;
    t = s[6]; s[6] = s[14]; s[14] = t;
    /* row 3: shift left 3 (== right 1) */
    t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;
}

static void aes_inv_shift_rows(uint8_t s[16])
{
    uint8_t t;
    t = s[13]; s[13] = s[9]; s[9] = s[5]; s[5] = s[1]; s[1] = t;
    t = s[2]; s[2] = s[10]; s[10] = t;
    t = s[6]; s[6] = s[14]; s[14] = t;
    t = s[3]; s[3] = s[7]; s[7] = s[11]; s[11] = s[15]; s[15] = t;
}

static void aes_mix_columns(uint8_t s[16])
{
    for (int c = 0; c < 4; c++) {
        uint8_t *p = &s[4 * c];
        uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
        p[0] = gmul(a0, 2) ^ gmul(a1, 3) ^ a2 ^ a3;
        p[1] = a0 ^ gmul(a1, 2) ^ gmul(a2, 3) ^ a3;
        p[2] = a0 ^ a1 ^ gmul(a2, 2) ^ gmul(a3, 3);
        p[3] = gmul(a0, 3) ^ a1 ^ a2 ^ gmul(a3, 2);
    }
}

static void aes_inv_mix_columns(uint8_t s[16])
{
    for (int c = 0; c < 4; c++) {
        uint8_t *p = &s[4 * c];
        uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
        p[0] = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9);
        p[1] = gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13);
        p[2] = gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11);
        p[3] = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14);
    }
}

static void aes_encrypt_block(const uint8_t round_keys[15][16], int nr,
                               const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16];
    memcpy(s, in, 16);
    aes_add_round_key(s, round_keys[0]);
    for (int r = 1; r < nr; r++) {
        aes_sub_bytes(s);
        aes_shift_rows(s);
        aes_mix_columns(s);
        aes_add_round_key(s, round_keys[r]);
    }
    aes_sub_bytes(s);
    aes_shift_rows(s);
    aes_add_round_key(s, round_keys[nr]);
    memcpy(out, s, 16);
}

static void aes_decrypt_block(const uint8_t round_keys[15][16], int nr,
                               const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16];
    memcpy(s, in, 16);
    aes_add_round_key(s, round_keys[nr]);
    for (int r = nr - 1; r >= 1; r--) {
        aes_inv_shift_rows(s);
        aes_inv_sub_bytes(s);
        aes_add_round_key(s, round_keys[r]);
        aes_inv_mix_columns(s);
    }
    aes_inv_shift_rows(s);
    aes_inv_sub_bytes(s);
    aes_add_round_key(s, round_keys[0]);
    memcpy(out, s, 16);
}

/* ------------------------------------------------------------------ */
/* GHASH (NIST SP800-38D Algorithm 1: GF(2^128) multiply-with-reduce). */
/* ------------------------------------------------------------------ */

static void ghash_mult(const uint8_t x[16], const uint8_t h[16], uint8_t out[16])
{
    uint8_t z[16] = {0};
    uint8_t v[16];
    memcpy(v, h, 16);

    for (int i = 0; i < 128; i++) {
        if ((x[i / 8] >> (7 - (i % 8))) & 1) {
            for (int j = 0; j < 16; j++) {
                z[j] ^= v[j];
            }
        }
        bool lsb = v[15] & 1;
        for (int j = 15; j > 0; j--) {
            v[j] = (v[j] >> 1) | ((v[j - 1] & 1) << 7);
        }
        v[0] >>= 1;
        if (lsb) {
            v[0] ^= 0xe1;
        }
    }
    memcpy(out, z, 16);
}

static void block_xor(const uint8_t a[16], const uint8_t b[16], uint8_t out[16])
{
    for (int i = 0; i < 16; i++) {
        out[i] = a[i] ^ b[i];
    }
}

static void ctr_inc32(uint8_t block[16])
{
    /* Increment only the last 32 bits (big-endian), per NIST SP800-38D's
     * inc32(): the rest of the counter block (nonce) is untouched. */
    for (int i = 15; i >= 12; i--) {
        if (++block[i] != 0) {
            break;
        }
    }
}

/* Symmetric decrement, same last-32-bits-only scope as ctr_inc32() --
 * used to derive GCM's J0 from the J0+1 value firmware actually loads
 * into the IV registers (see gnw_h7b0_cryp_gcm_init_phase()'s comment). */
static void ctr_dec32(uint8_t block[16])
{
    for (int i = 15; i >= 12; i--) {
        if (block[i]-- != 0) {
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Register-level word <-> byte-block helpers.                        */
/* ------------------------------------------------------------------ */

static uint32_t be_bytes_to_word(const uint8_t b[4])
{
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8) | b[3];
}

static void word_to_be_bytes(uint32_t w, uint8_t b[4])
{
    b[0] = (w >> 24) & 0xFF;
    b[1] = (w >> 16) & 0xFF;
    b[2] = (w >> 8) & 0xFF;
    b[3] = w & 0xFF;
}

static void words4_to_block(const uint32_t w[4], uint8_t block[16])
{
    for (int i = 0; i < 4; i++) {
        word_to_be_bytes(w[i], &block[4 * i]);
    }
}

static void block_to_words4(const uint8_t block[16], uint32_t w[4])
{
    for (int i = 0; i < 4; i++) {
        w[i] = be_bytes_to_word(&block[4 * i]);
    }
}

/* ------------------------------------------------------------------ */
/* Device model.                                                      */
/* ------------------------------------------------------------------ */

/* DATATYPE transforms the data FIFO words, never the key or IV file.
 * Stock Zelda selects byte swapping; treating those words as no-swap
 * decrypts valid settings into garbage and forces first-boot setup. */
static uint32_t gnw_h7b0_cryp_swap_data(GnwH7B0CrypState *s, uint32_t word)
{
    uint32_t cr = s->regs[GNW_H7B0_CRYP_CR >> 2];

    switch ((cr & CRYP_CR_DATATYPE_MASK) >> CRYP_CR_DATATYPE_SHIFT) {
    case 1:
        return (word << 16) | (word >> 16);
    case 2:
        return bswap32(word);
    case 3:
        return revbit32(word);
    default:
        return word;
    }
}

static void gnw_h7b0_cryp_update_irq(GnwH7B0CrypState *s)
{
    uint32_t risr = s->regs[GNW_H7B0_CRYP_RISR >> 2];
    uint32_t imscr = s->regs[GNW_H7B0_CRYP_IMSCR >> 2];
    uint32_t misr = risr & imscr & 0x3;

    s->regs[GNW_H7B0_CRYP_MISR >> 2] = misr;
    qemu_set_irq(s->irq, misr != 0);
}

static void gnw_h7b0_cryp_update_flags(GnwH7B0CrypState *s)
{
    uint32_t sr = CRYP_SR_IFEM | CRYP_SR_IFNF; /* input always ready -- see .h */
    uint32_t risr = CRYP_RISR_INRIS;

    if (s->dout_count > 0) {
        sr |= CRYP_SR_OFNE;
        risr |= CRYP_RISR_OUTRIS;
    }
    s->regs[GNW_H7B0_CRYP_SR >> 2] = sr;
    s->regs[GNW_H7B0_CRYP_RISR >> 2] = risr;
    gnw_h7b0_cryp_update_irq(s);
}

static int gnw_h7b0_cryp_keysize_bytes(uint32_t cr)
{
    switch ((cr & CRYP_CR_KEYSIZE_MASK) >> CRYP_CR_KEYSIZE_SHIFT) {
    case 0: return 16;
    case 1: return 24;
    case 2: return 32;
    default: return 16;
    }
}

/* Loads the AES key from K0..K3 LR/RR per KEYSIZE's real-hardware
 * right-alignment (128-bit keys occupy K2/K3 only, 192-bit K1-K3,
 * 256-bit all of K0-K3 -- see stm32h7xx_hal_cryp.c's CRYP_SetKey()),
 * and expands it into round_keys/num_rounds. */
static void gnw_h7b0_cryp_load_key(GnwH7B0CrypState *s)
{
    int key_bytes = gnw_h7b0_cryp_keysize_bytes(s->regs[GNW_H7B0_CRYP_CR >> 2]);
    uint8_t key[32];
    static const int all_offsets[8] = {
        GNW_H7B0_CRYP_K0LR, GNW_H7B0_CRYP_K0RR, GNW_H7B0_CRYP_K1LR, GNW_H7B0_CRYP_K1RR,
        GNW_H7B0_CRYP_K2LR, GNW_H7B0_CRYP_K2RR, GNW_H7B0_CRYP_K3LR, GNW_H7B0_CRYP_K3RR,
    };
    int nwords = key_bytes / 4;
    int start = 8 - nwords; /* right-aligned in the 8-word K file */

    for (int i = 0; i < nwords; i++) {
        uint32_t w = s->regs[all_offsets[start + i] >> 2];
        word_to_be_bytes(w, &key[4 * i]);
    }
    aes_key_expansion(key, key_bytes, s->round_keys, &s->num_rounds);
}

static void gnw_h7b0_cryp_gcm_init_phase(GnwH7B0CrypState *s)
{
    uint8_t zero[16] = {0};
    uint32_t iv_words[4] = {
        s->regs[GNW_H7B0_CRYP_IV0LR >> 2], s->regs[GNW_H7B0_CRYP_IV0RR >> 2],
        s->regs[GNW_H7B0_CRYP_IV1LR >> 2], s->regs[GNW_H7B0_CRYP_IV1RR >> 2],
    };

    gnw_h7b0_cryp_load_key(s);
    aes_encrypt_block(s->round_keys, s->num_rounds, zero, s->hash_subkey);
    /* This firmware family's HAL config always supplies J0+1 (not J0!)
     * directly via IV0LR..IV1RR -- confirmed against real hardware, see
     * case_cryp_aes_gcm_correct.c's header comment: loading J0 itself or
     * the bare IV both produced hardware behavior matching "the loaded
     * value used completely literally, unincremented, as the first
     * payload block's keystream input", with no internal J0-vs-J0+1
     * offset logic at all. `counter` therefore starts at the literal
     * loaded value (J0+1) and is what PAYLOAD blocks consume; `j0` is
     * separately derived (loaded value minus one) and preserved
     * unchanged for FINAL's tag mask, since real hardware independently
     * re-derives AES_K(J0) there rather than reusing whatever `counter`
     * has advanced to by then (also confirmed against real hardware,
     * same header comment). */
    words4_to_block(iv_words, s->counter);
    memcpy(s->j0, s->counter, 16);
    ctr_dec32(s->j0);
    memset(s->ghash, 0, 16);
    s->din_count = 0;
    s->dout_count = 0;
    s->dout_total = 0;
}

/* Processes one completed 4-word DIN block per the current GCM phase.
 * See gnw_h7b0_cryp.h's file comment for the phase semantics this
 * mirrors (CRYP_AESGCM_Process()/CRYP_GCMCCM_SetHeaderPhase()/
 * HAL_CRYPEx_AESGCM_GenerateAuthTAG()). */
static void gnw_h7b0_cryp_gcm_process_block(GnwH7B0CrypState *s)
{
    uint32_t cr = s->regs[GNW_H7B0_CRYP_CR >> 2];
    uint32_t phase = (cr & CRYP_CR_GCM_CCMPH_MASK) >> CRYP_CR_GCM_CCMPH_SHIFT;
    bool decrypt = (cr & CRYP_CR_ALGODIR) != 0;
    uint32_t npblb = (cr & CRYP_CR_NPBLB_MASK) >> CRYP_CR_NPBLB_SHIFT;
    uint8_t block[16], tmp[16];

    words4_to_block(s->din_words, block);

    switch (phase) {
    case CRYP_GCM_CCMPH_HEADER:
        block_xor(s->ghash, block, tmp);
        ghash_mult(tmp, s->hash_subkey, s->ghash);
        s->dout_count = 0;
        s->dout_total = 0;
        break;

    case CRYP_GCM_CCMPH_PAYLOAD: {
        uint8_t keystream[16], outblock[16], ghash_input[16];

        /* Use the current counter AS-IS for this block's keystream --
         * NOT pre-incremented. The first PAYLOAD block must consume the
         * literal J0+1 value INIT loaded (confirmed against real
         * hardware, see gnw_h7b0_cryp_gcm_init_phase()'s comment); only
         * advance afterward, so a hypothetical next block would get
         * J0+2. Previously this incremented before use, which fed the
         * first block J0+2 instead of J0+1 -- a real, confirmed bug. */
        aes_encrypt_block(s->round_keys, s->num_rounds, s->counter, keystream);
        block_xor(block, keystream, outblock);
        ctr_inc32(s->counter);

        if (npblb > 0 && npblb <= 16) {
            memset(&outblock[16 - npblb], 0, npblb);
        }
        /* GHASH always accumulates over the ciphertext: for encryption
         * that's our output, for decryption that's the (already
         * zero-padded-by-firmware, per CRYP_AESGCM_Process()) input. */
        memcpy(ghash_input, decrypt ? block : outblock, 16);
        block_xor(s->ghash, ghash_input, tmp);
        ghash_mult(tmp, s->hash_subkey, s->ghash);

        block_to_words4(outblock, s->dout_words);
        s->dout_count = 4;
        s->dout_total = 4;
        break;
    }

    case CRYP_GCM_CCMPH_FINAL: {
        uint8_t tag_mask[16], tag[16];

        block_xor(s->ghash, block, tmp);
        ghash_mult(tmp, s->hash_subkey, s->ghash);
        /* Must use the preserved J0 (see gnw_h7b0_cryp_gcm_init_phase()'s
         * comment), NOT s->counter -- by the time FINAL runs, s->counter
         * has been advanced past J0+1 by any PAYLOAD blocks that ran. */
        aes_encrypt_block(s->round_keys, s->num_rounds, s->j0, tag_mask);
        block_xor(s->ghash, tag_mask, tag);
        block_to_words4(tag, s->dout_words);
        s->dout_count = 4;
        s->dout_total = 4;
        break;
    }

    case CRYP_GCM_CCMPH_INIT:
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: DIN write completed a block during GCM INIT "
                      "phase -- unexpected, ignoring\n", __func__);
        s->dout_count = 0;
        s->dout_total = 0;
        break;
    }
}

/*
 * CCM's CRYPEN 0->1 edge (ALGOMODE=CCM, phase=INIT): unlike GCM, real
 * hardware does NOT auto-derive anything here -- firmware has already
 * written IV0LR..IV1RR with CTR1 (masked B0, counter forced to 1, see
 * CRYP_AESCCM_Process()) *before* this edge, and is about to write the
 * raw B0 block via DIN right after (handled by
 * gnw_h7b0_cryp_ccm_process_block()'s INIT case below, not here --
 * CRYPEN must stay set across those DIN writes, only self-clearing once
 * B0 has actually been consumed, matching real firmware's "write B0,
 * then poll for CRYPEN to clear" sequence). This function only loads
 * the key, latches CTR1 into `counter`, and resets the running CBC-MAC
 * (reuses `ghash`) to zero.
 */
static void gnw_h7b0_cryp_ccm_init_phase(GnwH7B0CrypState *s)
{
    uint32_t iv_words[4] = {
        s->regs[GNW_H7B0_CRYP_IV0LR >> 2], s->regs[GNW_H7B0_CRYP_IV0RR >> 2],
        s->regs[GNW_H7B0_CRYP_IV1LR >> 2], s->regs[GNW_H7B0_CRYP_IV1RR >> 2],
    };

    gnw_h7b0_cryp_load_key(s);
    words4_to_block(iv_words, s->counter);
    memset(s->ghash, 0, 16);
    s->din_count = 0;
    s->dout_count = 0;
    s->dout_total = 0;
}

/* Processes one completed 4-word DIN block per the current CCM phase.
 * See case_cryp_aes_ccm_correct.c's header comment and
 * stm32h7xx_hal_cryp.c's CRYP_AESCCM_Process()/HAL_CRYPEx_AESCCM_
 * GenerateAuthTAG() for the real sequencing this mirrors. CBC-MAC
 * (reusing the `ghash` field) always accumulates over the PLAINTEXT --
 * the opposite of GCM's GHASH, which always accumulates ciphertext --
 * so PAYLOAD's direction-dependent selection below is deliberately the
 * mirror image of gnw_h7b0_cryp_gcm_process_block()'s. */
static void gnw_h7b0_cryp_ccm_process_block(GnwH7B0CrypState *s)
{
    uint32_t cr = s->regs[GNW_H7B0_CRYP_CR >> 2];
    uint32_t phase = (cr & CRYP_CR_GCM_CCMPH_MASK) >> CRYP_CR_GCM_CCMPH_SHIFT;
    bool decrypt = (cr & CRYP_CR_ALGODIR) != 0;
    uint32_t npblb = (cr & CRYP_CR_NPBLB_MASK) >> CRYP_CR_NPBLB_SHIFT;
    uint8_t block[16], tmp[16];

    words4_to_block(s->din_words, block);

    switch (phase) {
    case CRYP_GCM_CCMPH_INIT:
        /* B0, written raw (not masked like the IV registers were) --
         * first CBC-MAC step: MAC = AES_K(0^128 XOR B0) = AES_K(B0).
         * Real hardware self-clears CRYPEN once this block is consumed
         * (firmware's CRYP_AESCCM_Process() polls exactly that, right
         * after its 4 B0 DIN writes) -- done here, not at the CR-write
         * edge, since the edge happens *before* B0 arrives. */
        block_xor(s->ghash, block, tmp);
        aes_encrypt_block(s->round_keys, s->num_rounds, tmp, s->ghash);
        s->regs[GNW_H7B0_CRYP_CR >> 2] &= ~CRYP_CR_CRYPEN;
        s->dout_count = 0;
        s->dout_total = 0;
        break;

    case CRYP_GCM_CCMPH_HEADER:
        /* Standard CBC-MAC step over one AAD block: MAC = AES_K(MAC XOR block). */
        block_xor(s->ghash, block, tmp);
        aes_encrypt_block(s->round_keys, s->num_rounds, tmp, s->ghash);
        s->dout_count = 0;
        s->dout_total = 0;
        break;

    case CRYP_GCM_CCMPH_PAYLOAD: {
        uint8_t keystream[16], outblock[16], mac_input[16];

        /* Same as GCM: use `counter` (CTR1, then CTR2, ...) as-is for
         * this block, advance only after -- see gnw_h7b0_cryp_gcm_
         * process_block()'s PAYLOAD comment for why pre-incrementing
         * would be wrong. */
        aes_encrypt_block(s->round_keys, s->num_rounds, s->counter, keystream);
        block_xor(block, keystream, outblock);
        ctr_inc32(s->counter);

        if (npblb > 0 && npblb <= 16) {
            memset(&outblock[16 - npblb], 0, npblb);
        }
        /* CBC-MAC always accumulates the PLAINTEXT: for encryption
         * that's our input (block), for decryption it's our output
         * (outblock, the just-decrypted plaintext) -- the mirror image
         * of GCM's GHASH-always-ciphertext rule. */
        memcpy(mac_input, decrypt ? outblock : block, 16);
        block_xor(s->ghash, mac_input, tmp);
        aes_encrypt_block(s->round_keys, s->num_rounds, tmp, s->ghash);

        block_to_words4(outblock, s->dout_words);
        s->dout_count = 4;
        s->dout_total = 4;
        break;
    }

    case CRYP_GCM_CCMPH_FINAL: {
        /* DIN here carries CTR0 (masked B0, counter forced to 0 -- see
         * HAL_CRYPEx_AESCCM_GenerateAuthTAG()), not a lengths block like
         * GCM's FINAL. Tag = MAC XOR AES_K(CTR0). */
        uint8_t tag_mask[16], tag[16];

        aes_encrypt_block(s->round_keys, s->num_rounds, block, tag_mask);
        block_xor(s->ghash, tag_mask, tag);
        block_to_words4(tag, s->dout_words);
        s->dout_count = 4;
        s->dout_total = 4;
        break;
    }

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: DIN write with unknown GCM_CCMPH -- ignoring\n",
                      __func__);
        s->dout_count = 0;
        s->dout_total = 0;
        break;
    }
}

/* ECB/CBC/CTR (non-GCM) block processing -- see .h file comment: not
 * observed live on this boot path (Mario's CRYP use is GCM-only so
 * far) but implemented for completeness/future-proofing since it
 * reuses the same AES core with no new investigation needed. IV0LR/
 * IV0RR/IV1LR/IV1RR double as the running CBC IV / CTR counter here,
 * same registers GCM uses for its ICB -- real hardware overloads them
 * the same way (they're simply "the 128 bits the algorithm currently
 * needs combined with the block", whatever that means per mode). */
static void gnw_h7b0_cryp_plain_process_block(GnwH7B0CrypState *s,
                                               uint32_t algomode)
{
    uint32_t cr = s->regs[GNW_H7B0_CRYP_CR >> 2];
    bool decrypt = (cr & CRYP_CR_ALGODIR) != 0;
    uint8_t block[16], out[16];

    words4_to_block(s->din_words, block);

    if (algomode == CRYP_CR_ALGOMODE_AES_ECB) {
        if (decrypt) {
            aes_decrypt_block(s->round_keys, s->num_rounds, block, out);
        } else {
            aes_encrypt_block(s->round_keys, s->num_rounds, block, out);
        }
    } else if (algomode == CRYP_CR_ALGOMODE_AES_CBC) {
        if (decrypt) {
            aes_decrypt_block(s->round_keys, s->num_rounds, block, out);
            block_xor(out, s->counter, out);
            memcpy(s->counter, block, 16); /* IV_next = ciphertext just read */
        } else {
            uint8_t xored[16];
            block_xor(block, s->counter, xored);
            aes_encrypt_block(s->round_keys, s->num_rounds, xored, out);
            memcpy(s->counter, out, 16); /* IV_next = ciphertext just produced */
        }
    } else { /* AES_CTR */
        uint8_t keystream[16];
        aes_encrypt_block(s->round_keys, s->num_rounds, s->counter, keystream);
        block_xor(block, keystream, out);
        ctr_inc32(s->counter);
    }

    block_to_words4(out, s->dout_words);
    s->dout_count = 4;
    s->dout_total = 4;
}

static void gnw_h7b0_cryp_reset(DeviceState *dev)
{
    GnwH7B0CrypState *s = GNW_H7B0_CRYP(dev);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->round_keys, 0, sizeof(s->round_keys));
    s->num_rounds = 0;
    memset(s->hash_subkey, 0, sizeof(s->hash_subkey));
    memset(s->ghash, 0, sizeof(s->ghash));
    memset(s->counter, 0, sizeof(s->counter));
    memset(s->j0, 0, sizeof(s->j0));
    memset(s->din_words, 0, sizeof(s->din_words));
    s->din_count = 0;
    memset(s->dout_words, 0, sizeof(s->dout_words));
    s->dout_count = 0;
    s->dout_total = 0;
    qemu_irq_lower(s->irq);
    gnw_h7b0_cryp_update_flags(s);
}

static uint64_t gnw_h7b0_cryp_read(void *opaque, hwaddr addr, unsigned int size)
{
    GnwH7B0CrypState *s = GNW_H7B0_CRYP(opaque);

    if (addr >= GNW_H7B0_CRYP_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return 0;
    }

    if (addr == GNW_H7B0_CRYP_DOUT) {
        uint32_t val;

        if (s->dout_count > 0) {
            int idx = s->dout_total - s->dout_count;
            val = s->dout_words[idx];
            s->dout_count--;
            gnw_h7b0_cryp_update_flags(s);
        } else {
            val = 0;
        }
        return gnw_h7b0_cryp_swap_data(s, val);
    }

    return s->regs[addr >> 2];
}

static void gnw_h7b0_cryp_write(void *opaque, hwaddr addr,
                                 uint64_t val64, unsigned int size)
{
    GnwH7B0CrypState *s = GNW_H7B0_CRYP(opaque);
    uint32_t value = val64;

    if (addr >= GNW_H7B0_CRYP_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return;
    }

    switch (addr) {
    case GNW_H7B0_CRYP_CR: {
        uint32_t old_cr = s->regs[GNW_H7B0_CRYP_CR >> 2];
        bool old_en = (old_cr & CRYP_CR_CRYPEN) != 0;
        uint32_t old_algomode = old_cr & CRYP_CR_ALGOMODE_MASK;
        bool flush = (value & CRYP_CR_FFLUSH) != 0;
        bool new_en;
        uint32_t algomode;

        value &= ~CRYP_CR_FFLUSH; /* self-clearing pulse, not stored */
        s->regs[GNW_H7B0_CRYP_CR >> 2] = value;
        new_en = (value & CRYP_CR_CRYPEN) != 0;

        if (flush) {
            s->din_count = 0;
            s->dout_count = 0;
            s->dout_total = 0;
        }

        algomode = value & CRYP_CR_ALGOMODE_MASK;

        /*
         * Real HAL AES decrypt for ECB/CBC (CRYP_AES_Decrypt(), see
         * stm32h7xx_hal_cryp.c) does a "key preparation" phase first:
         * ALGOMODE is set to CRYP_CR_ALGOMODE_AES_KEY (0x38) *at* the
         * CRYPEN 0->1 edge, then ALGOMODE is switched back to the real
         * mode (CBC/ECB) via a SEPARATE register write that leaves
         * CRYPEN already set -- no second edge. The real key/IV setup
         * this model does below (gnw_h7b0_cryp_load_key() + counter
         * reload from IV0..1LR/RR) must therefore also run on that
         * algomode-changes-while-still-enabled transition, not only on
         * a fresh CRYPEN edge -- otherwise CBC decrypt silently reuses
         * whatever stale IV/counter state a prior encrypt operation
         * left behind (confirmed live: this made case_cryp_aes_cbc_
         * correct.c's decrypt phase decrypt against the wrong "IV",
         * since s->counter still held the last CBC ciphertext block
         * from the preceding encrypt call). ECB/CTR happened to dodge
         * this: ECB doesn't consume s->counter at all, and CTR's own
         * key-prep path (see CRYP_AES_Decrypt()) never touches ALGOMODE
         * at all, so its one real edge already lands on CRYP_CR_
         * ALGOMODE_AES_CTR directly.
         */
        if (new_en && (!old_en || algomode != old_algomode)) {
            if (algomode == CRYP_CR_ALGOMODE_AES_GCM) {
                uint32_t phase = (value & CRYP_CR_GCM_CCMPH_MASK)
                                  >> CRYP_CR_GCM_CCMPH_SHIFT;
                if (phase == CRYP_GCM_CCMPH_INIT) {
                    gnw_h7b0_cryp_gcm_init_phase(s);
                    /* Real hardware autonomously completes H/J0 setup
                     * and self-clears CRYPEN; firmware polls for this
                     * (see CRYP_AESGCM_Process()'s "Wait for the
                     * CRYPEN bit to be cleared" loop). Instant in this
                     * model, same convention as this project's other
                     * synchronous-completion peripherals. */
                    s->regs[GNW_H7B0_CRYP_CR >> 2] &= ~CRYP_CR_CRYPEN;
                }
                /* HEADER/PAYLOAD/FINAL: no action needed at the
                 * enable edge itself -- processing happens as DIN
                 * words arrive (see gnw_h7b0_cryp_gcm_process_block()).
                 * Counter/GHASH state persists across the CRYPEN
                 * toggles firmware does between phases. */
            } else if (algomode == CRYP_CR_ALGOMODE_AES_CCM) {
                uint32_t phase = (value & CRYP_CR_GCM_CCMPH_MASK)
                                  >> CRYP_CR_GCM_CCMPH_SHIFT;
                if (phase == CRYP_GCM_CCMPH_INIT) {
                    /* Unlike GCM, do NOT self-clear CRYPEN here -- CCM's
                     * INIT phase needs firmware to write the raw B0
                     * block via DIN first (see gnw_h7b0_cryp_ccm_
                     * process_block()'s INIT case, which self-clears
                     * once that arrives). */
                    gnw_h7b0_cryp_ccm_init_phase(s);
                }
                /* HEADER/PAYLOAD/FINAL: same as GCM, no action at the
                 * edge itself. */
            } else if (algomode == CRYP_CR_ALGOMODE_AES_ECB ||
                       algomode == CRYP_CR_ALGOMODE_AES_CBC ||
                       algomode == CRYP_CR_ALGOMODE_AES_CTR) {
                gnw_h7b0_cryp_load_key(s);
                if (algomode != CRYP_CR_ALGOMODE_AES_ECB) {
                    uint32_t iv_words[4] = {
                        s->regs[GNW_H7B0_CRYP_IV0LR >> 2],
                        s->regs[GNW_H7B0_CRYP_IV0RR >> 2],
                        s->regs[GNW_H7B0_CRYP_IV1LR >> 2],
                        s->regs[GNW_H7B0_CRYP_IV1RR >> 2],
                    };
                    words4_to_block(iv_words, s->counter);
                }
                s->din_count = 0;
            } else if (algomode == CRYP_CR_ALGOMODE_AES_KEY) {
                /* Key-preparation phase for ECB/CBC decrypt (see this
                 * `if`'s doc comment above) -- (re)compute round_keys
                 * from the current K0-3 registers now, though the
                 * subsequent algomode-change-while-enabled transition
                 * back to the real mode already does this again too
                 * (harmless/idempotent given unchanged K0-3). Recognized
                 * explicitly so it doesn't fall into the "unimplemented"
                 * branch below and log a misleading warning. */
                gnw_h7b0_cryp_load_key(s);
            } else {
                qemu_log_mask(LOG_UNIMP,
                              "%s: unimplemented ALGOMODE (CR=%#x) -- "
                              "DES/TDES not modeled\n", __func__, value);
            }
        }

        gnw_h7b0_cryp_update_flags(s);
        return;
    }

    case GNW_H7B0_CRYP_DIN: {
        uint32_t cr = s->regs[GNW_H7B0_CRYP_CR >> 2];
        uint32_t algomode = cr & CRYP_CR_ALGOMODE_MASK;

        if (!(cr & CRYP_CR_CRYPEN)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: DIN write while CRYPEN=0, ignoring\n", __func__);
            return;
        }
        /* H7 rev.B+ consumes the GCM FINAL length block in native word
         * order regardless of DATATYPE. HAL_CRYPEx_AESGCM_GenerateAuthTAG
         * explicitly submits unswapped lengths for this revision; applying
         * the payload swap here produces an invalid authentication tag. */
        if (algomode == CRYP_CR_ALGOMODE_AES_GCM &&
            ((cr & CRYP_CR_GCM_CCMPH_MASK) >> CRYP_CR_GCM_CCMPH_SHIFT) ==
            CRYP_GCM_CCMPH_FINAL) {
            s->din_words[s->din_count++] = value;
        } else {
            s->din_words[s->din_count++] = gnw_h7b0_cryp_swap_data(s, value);
        }
        if (s->din_count < 4) {
            return;
        }
        s->din_count = 0;

        if (algomode == CRYP_CR_ALGOMODE_AES_GCM) {
            gnw_h7b0_cryp_gcm_process_block(s);
        } else if (algomode == CRYP_CR_ALGOMODE_AES_CCM) {
            gnw_h7b0_cryp_ccm_process_block(s);
        } else if (algomode == CRYP_CR_ALGOMODE_AES_ECB ||
                   algomode == CRYP_CR_ALGOMODE_AES_CBC ||
                   algomode == CRYP_CR_ALGOMODE_AES_CTR) {
            gnw_h7b0_cryp_plain_process_block(s, algomode);
        } else {
            s->dout_count = 0;
            s->dout_total = 0;
        }
        gnw_h7b0_cryp_update_flags(s);
        return;
    }

    case GNW_H7B0_CRYP_IMSCR:
        s->regs[GNW_H7B0_CRYP_IMSCR >> 2] = value & 0x3;
        gnw_h7b0_cryp_update_irq(s);
        return;

    case GNW_H7B0_CRYP_SR:
    case GNW_H7B0_CRYP_RISR:
    case GNW_H7B0_CRYP_MISR:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: offset %#"HWADDR_PRIx" is read-only\n",
                      __func__, addr);
        return;

    default:
        /* K0-3LR/RR, IV0-1LR/RR, DMACR, CSGCM/CSGCMCCM regs: plain shadow. */
        s->regs[addr >> 2] = value;
        return;
    }
}

static const MemoryRegionOps gnw_h7b0_cryp_ops = {
    .read = gnw_h7b0_cryp_read,
    .write = gnw_h7b0_cryp_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void gnw_h7b0_cryp_init(Object *obj)
{
    GnwH7B0CrypState *s = GNW_H7B0_CRYP(obj);

    memory_region_init_io(&s->mmio, obj, &gnw_h7b0_cryp_ops, s,
                           TYPE_GNW_H7B0_CRYP, GNW_H7B0_CRYP_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const VMStateDescription vmstate_gnw_h7b0_cryp = {
    .name = TYPE_GNW_H7B0_CRYP,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, GnwH7B0CrypState, GNW_H7B0_CRYP_SIZE / 4),
        VMSTATE_UINT8_2DARRAY(round_keys, GnwH7B0CrypState, 15, 16),
        VMSTATE_INT32(num_rounds, GnwH7B0CrypState),
        VMSTATE_UINT8_ARRAY(hash_subkey, GnwH7B0CrypState, 16),
        VMSTATE_UINT8_ARRAY(ghash, GnwH7B0CrypState, 16),
        VMSTATE_UINT8_ARRAY(counter, GnwH7B0CrypState, 16),
        VMSTATE_UINT8_ARRAY(j0, GnwH7B0CrypState, 16),
        VMSTATE_UINT32_ARRAY(din_words, GnwH7B0CrypState, 4),
        VMSTATE_INT32(din_count, GnwH7B0CrypState),
        VMSTATE_UINT32_ARRAY(dout_words, GnwH7B0CrypState, 4),
        VMSTATE_INT32(dout_count, GnwH7B0CrypState),
        VMSTATE_INT32(dout_total, GnwH7B0CrypState),
        VMSTATE_END_OF_LIST()
    }
};

static void gnw_h7b0_cryp_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_gnw_h7b0_cryp;
    device_class_set_legacy_reset(dc, gnw_h7b0_cryp_reset);
}

static const TypeInfo gnw_h7b0_cryp_info = {
    .name          = TYPE_GNW_H7B0_CRYP,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GnwH7B0CrypState),
    .instance_init = gnw_h7b0_cryp_init,
    .class_init    = gnw_h7b0_cryp_class_init,
};

static void gnw_h7b0_cryp_register_types(void)
{
    type_register_static(&gnw_h7b0_cryp_info);
}
type_init(gnw_h7b0_cryp_register_types)
