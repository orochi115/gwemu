/*
 * STM32H7B0 OTFDEC minimal model (Nintendo Game & Watch)
 *
 * See gnw_h7b0_otfdec.h for scope/rationale.
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
#include "qemu/bswap.h"
#include "migration/vmstate.h"
#include "hw/misc/gnw_h7b0_otfdec.h"
#include "crypto/aes.h"

/*
 * Real AES-128-CTR on-the-fly decryption, per RM0455 41.3.4 "AES in
 * counter mode decryption" (STM32H7A3/7B3/7B0 reference manual). Exact
 * bit layout, transcribed directly from the manual, not guessed:
 *
 *   AES_IV[127:0]  = RxNONCER[63:0] || 16'b0 || RxCFGR[31:16] || 2'b00
 *                     || (x-1)[1:0] || ReadAddress[31:4]
 *   AES_KEY[127:0] = RxKEYR3[31:0] || RxKEYR2[31:0] || RxKEYR1[31:0]
 *                     || RxKEYR0[31:0]
 *   AES_Dx[127:0]  = AXI_word(@+0x8)[63:0] || AXI_word(@)[63:0]
 *
 * where @ is the 16-byte-aligned block address, x is the region ID
 * (1-4), and RxCFGR[31:16] is the region's REGx_VERSION field.
 *
 * The manual separately notes: "CPU memories... follow little endian
 * notation whereas AES hardware accelerator follows big endian
 * notation." This matters for exactly how the 128-bit IV/key/data
 * values above (specified MSB-first, big-endian) map onto the
 * little-endian byte layout AES_Dx's own definition implies for
 * AXI_word(@) -- see otfdec_decrypt_block()'s comment for the concrete
 * byte-ordering this produces.
 */
static void otfdec_compute_keystream(const uint32_t key_words[4],
                                      uint64_t nonce, uint16_t version,
                                      int region_id_1based, uint32_t block_addr,
                                      uint8_t keystream_out[16])
{
    uint8_t key_be[16];
    uint8_t iv_be[16];
    AES_KEY aes_key;
    uint32_t iv_word0 = (((uint32_t)(region_id_1based - 1) & 0x3u) << 28) |
                         ((block_addr >> 4) & 0x0FFFFFFFu);
    uint32_t iv_word1 = (uint32_t)version;
    uint32_t iv_word2 = (uint32_t)(nonce & 0xFFFFFFFFu);
    uint32_t iv_word3 = (uint32_t)(nonce >> 32);
    /* AES_KEY = KEYR3(MSB) || KEYR2 || KEYR1 || KEYR0(LSB); key_words[]
     * is passed in KEYR0..KEYR3 order (matches the register layout). */
    uint32_t key_word3 = key_words[3]; /* KEYR3 -> most significant */
    uint32_t key_word2 = key_words[2];
    uint32_t key_word1 = key_words[1];
    uint32_t key_word0 = key_words[0]; /* KEYR0 -> least significant */

    stl_be_p(iv_be + 0, iv_word3);
    stl_be_p(iv_be + 4, iv_word2);
    stl_be_p(iv_be + 8, iv_word1);
    stl_be_p(iv_be + 12, iv_word0);

    stl_be_p(key_be + 0, key_word3);
    stl_be_p(key_be + 4, key_word2);
    stl_be_p(key_be + 8, key_word1);
    stl_be_p(key_be + 12, key_word0);

    AES_set_encrypt_key(key_be, 128, &aes_key);
    AES_encrypt(iv_be, keystream_out, &aes_key);
}

/*
 * Decrypts one 16-byte-aligned block in place. `block` holds the raw
 * ciphertext bytes at [block_addr, block_addr+16) in normal (little-
 * endian, ascending-address) memory order on input; decrypted plaintext
 * in the same order on output.
 *
 * AES_Dx[127:0] = AXI_word(@+8)[63:0] || AXI_word(@)[63:0] means the
 * AES engine's MSB-first 128-bit view has AXI_word(@)'s bytes as the
 * LOW 64 bits and AXI_word(@+8)'s as the HIGH 64 bits -- and since
 * memory itself is little-endian, AXI_word(@)'s own LSB (bit-wise) is
 * the byte at address @ itself. So the AES engine's big-endian byte 15
 * (LSB of its 128-bit view) is memory byte @+0, and its byte 0 (MSB) is
 * memory byte @+15 -- i.e. the AES-domain byte order is the reverse of
 * normal memory order. The keystream computed above is already in that
 * same AES/big-endian byte order (byte 0 = MSB), so XOR-ing it against
 * the block requires reversing one or the other to align them.
 */
static void otfdec_decrypt_block(uint8_t block[16],
                                  const uint8_t keystream_be[16])
{
    for (int mem_off = 0; mem_off < 16; mem_off++) {
        block[mem_off] ^= keystream_be[15 - mem_off];
    }
}

/*
 * Exact reimplementation of the ROM'd key-CRC the peripheral computes
 * over a freshly loaded 128-bit key, mirrored from the HAL's software
 * twin, HAL_OTFDEC_KeyCRCComputation()
 * (sdk/stm32h7xx-hal-driver/Src/stm32h7xx_hal_otfdec.c) -- firmware
 * compares the two and refuses the key on mismatch, so this must match
 * bit-for-bit.
 */
static uint8_t otfdec_key_crc(const uint32_t *key)
{
    static const uint32_t key_strobe[4] = { 0xAA55AA55U, 0x3U, 0x18U, 0xC0U };
    uint8_t crc = 0;

    for (uint32_t j = 0; j < 4; j++) {
        uint32_t keyval = key[j];

        if (j == 0) {
            keyval ^= key_strobe[0];
        } else {
            keyval ^= (key_strobe[j] << 24) | ((uint32_t)crc << 16) |
                      (key_strobe[j] << 8) | crc;
        }

        crc = 0;
        for (uint8_t i = 0; i < 32; i++) {
            uint32_t k = (((uint32_t)crc >> 7) ^
                          ((keyval >> (31 - i)) & 0xFU)) & 1U;
            crc <<= 1;
            if (k) {
                crc ^= 0x7; /* CRC-7 polynomial */
            }
        }

        crc ^= 0x55;
    }

    return crc;
}

/*
 * Installs (or removes) region x's decrypting overlay to match its
 * current REG_EN/START_ADDR/END_ADDR configuration. Called after every
 * write that could affect any of those fields -- see the note at each
 * call site. Real hardware allows reconfiguring a region at any time
 * (subject to CONFIGLOCK, not modeled here since no known firmware path
 * needs it); this mirrors that by always fully re-deriving the mapping
 * from current register state rather than tracking edges.
 *
 * Decrypts the WHOLE region in one bulk pass right here, into a plain
 * host RAM buffer mapped as ordinary RAM (not an IO region) -- see the
 * struct's own doc comment in gnw_h7b0_otfdec.h for why this eager,
 * bulk-decrypt-then-map-as-RAM design replaced an earlier per-access
 * IO-callback version. A region is typically enabled once (at firmware
 * OTFDEC init) and stays mapped for the rest of a boot, so this upfront
 * cost is paid once, not per instruction fetch.
 */
static void otfdec_update_region_mapping(GnwH7B0OtfdecState *s, int region_id_1based)
{
    int idx = region_id_1based - 1;
    hwaddr region_base = OTFDEC_REGION_BASE + idx * OTFDEC_REGION_STRIDE;
    uint32_t cfgr = s->regs[(region_base + OTFDEC_REG_CONFIGR) >> 2];
    bool enabled = (cfgr & 0x1) != 0;

    if (!s->system_memory) {
        return; /* gnw_h7b0_otfdec_set_extflash() not called yet (e.g. early reset) */
    }

    if (s->overlay_mapped[idx]) {
        memory_region_del_subregion(s->system_memory, &s->decrypt_overlay[idx]);
        object_unparent(OBJECT(&s->decrypt_overlay[idx]));
        s->overlay_mapped[idx] = false;
        g_free(s->decrypted_buf[idx]);
        s->decrypted_buf[idx] = NULL;
    }

    if (!enabled) {
        return;
    }

    hwaddr start = s->regs[(region_base + OTFDEC_REG_START_ADDR) >> 2];
    hwaddr end = s->regs[(region_base + OTFDEC_REG_END_ADDR) >> 2];

    if (end <= start || start < s->extflash_base ||
        end >= s->extflash_base + s->extflash_size) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "gnw_h7b0_otfdec: region %d enabled with bad/out-of-"
                      "range START/END (0x%"HWADDR_PRIx"-0x%"HWADDR_PRIx")\n",
                      region_id_1based, start, end);
        return;
    }

    hwaddr size = end - start + 1;
    hwaddr aligned_size = (size + 0xF) & ~(hwaddr)0xF; /* whole 16B blocks */
    hwaddr host_off = start - s->extflash_base;

    if (!s->extflash_host_ptr || host_off + aligned_size > s->extflash_size) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "gnw_h7b0_otfdec: region %d range falls outside the "
                      "backing extflash image\n", region_id_1based);
        return;
    }

    uint32_t cfgr_now = s->regs[(region_base + OTFDEC_REG_CONFIGR) >> 2];
    uint16_t version = (uint16_t)(cfgr_now >> 16);
    uint32_t key_words[4];
    uint64_t nonce;

    for (int i = 0; i < 4; i++) {
        key_words[i] = s->regs[((region_base + OTFDEC_REG_KEYR0) >> 2) + i];
    }
    nonce = (uint64_t)s->regs[(region_base + OTFDEC_REG_NONCER1) >> 2] << 32 |
            s->regs[(region_base + OTFDEC_REG_NONCER0) >> 2];

    uint8_t *buf = g_malloc(aligned_size);
    memcpy(buf, s->extflash_host_ptr + host_off, aligned_size);
    for (hwaddr off = 0; off < aligned_size; off += 16) {
        uint8_t keystream[16];

        otfdec_compute_keystream(key_words, nonce, version, region_id_1based,
                                  (uint32_t)(start + off), keystream);
        otfdec_decrypt_block(buf + off, keystream);
    }
    s->decrypted_buf[idx] = buf;

    char name[32];
    snprintf(name, sizeof(name), "gnw-h7b0-otfdec-overlay-r%d", region_id_1based);
    memory_region_init_ram_ptr(&s->decrypt_overlay[idx], NULL, name, size, buf);
    memory_region_set_readonly(&s->decrypt_overlay[idx], true);
    /* Higher priority than the plain extflash RAM region so reads in
     * range go through the decrypted copy instead of raw ciphertext. */
    memory_region_add_subregion_overlap(s->system_memory, start,
                                         &s->decrypt_overlay[idx], 1);
    s->overlay_mapped[idx] = true;
    s->overlay_mapped_base[idx] = start;
}

void gnw_h7b0_otfdec_set_extflash(GnwH7B0OtfdecState *s,
                                   MemoryRegion *system_memory,
                                   MemoryRegion *extflash,
                                   hwaddr extflash_base,
                                   hwaddr extflash_size)
{
    s->system_memory = system_memory;
    s->extflash_host_ptr = memory_region_get_ram_ptr(extflash);
    s->extflash_base = extflash_base;
    s->extflash_size = extflash_size;
}

void gnw_h7b0_otfdec_snapshot_restore(GnwH7B0OtfdecState *s)
{
    /* Called only after raw Flash RAM and all device registers have loaded. */
    for (int region = 1; region <= OTFDEC_NUM_REGIONS; region++) {
        otfdec_update_region_mapping(s, region);
    }
}

static void gnw_h7b0_otfdec_reset(DeviceState *dev)
{
    GnwH7B0OtfdecState *s = GNW_H7B0_OTFDEC(dev);

    for (int i = 0; i < OTFDEC_NUM_REGIONS; i++) {
        if (s->overlay_mapped[i]) {
            memory_region_del_subregion(s->system_memory, &s->decrypt_overlay[i]);
            object_unparent(OBJECT(&s->decrypt_overlay[i]));
            s->overlay_mapped[i] = false;
            g_free(s->decrypted_buf[i]);
            s->decrypted_buf[i] = NULL;
        }
    }

    memset(s->regs, 0, sizeof(s->regs));
}

static uint64_t gnw_h7b0_otfdec_read(void *opaque, hwaddr addr,
                                      unsigned int size)
{
    GnwH7B0OtfdecState *s = GNW_H7B0_OTFDEC(opaque);

    if (addr >= GNW_H7B0_OTFDEC_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return 0;
    }
    return s->regs[addr >> 2];
}

static void gnw_h7b0_otfdec_write(void *opaque, hwaddr addr,
                                   uint64_t val64, unsigned int size)
{
    GnwH7B0OtfdecState *s = GNW_H7B0_OTFDEC(opaque);

    if (addr >= GNW_H7B0_OTFDEC_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return;
    }

    s->regs[addr >> 2] = (uint32_t)val64;

    /*
     * Writing a region's KEYR3 completes a key load: real hardware
     * then exposes the key's CRC in that region's CONFIGR.KEYCRC,
     * which HAL_OTFDEC_RegionSetKey() reads back to confirm the load.
     */
    if (addr >= OTFDEC_REGION_BASE &&
        addr < OTFDEC_REGION_BASE +
               OTFDEC_NUM_REGIONS * OTFDEC_REGION_STRIDE) {
        int region_id_1based = (int)((addr - OTFDEC_REGION_BASE) /
                                      OTFDEC_REGION_STRIDE) + 1;
        hwaddr region_base = OTFDEC_REGION_BASE +
            (region_id_1based - 1) * OTFDEC_REGION_STRIDE;
        hwaddr reg_off = addr - region_base;

        if (reg_off == OTFDEC_REG_KEYR3) {
            uint32_t key[4];
            uint32_t *configr = &s->regs[(region_base +
                                          OTFDEC_REG_CONFIGR) >> 2];

            for (int i = 0; i < 4; i++) {
                key[i] = s->regs[((region_base + OTFDEC_REG_KEYR0) >> 2) + i];
            }
            *configr = (*configr & ~OTFDEC_CONFIGR_KEYCRC_MASK) |
                       ((uint32_t)otfdec_key_crc(key)
                        << OTFDEC_CONFIGR_KEYCRC_SHIFT);
        }

        /*
         * A region becomes (or stops being) decryptable via a write to
         * its own CONFIGR (REG_EN, bit 0), or its content changes via a
         * write to START/END (window moves) or KEYR/NONCER (re-keying).
         * Since the whole region is bulk-decrypted eagerly into RAM (see
         * otfdec_update_region_mapping()'s own doc comment), any write
         * that could change what it decrypts to -- not just REG_EN/
         * START/END -- has to re-run that full re-derivation, or the
         * mapped RAM would keep serving stale plaintext from the old
         * key/nonce/window. Re-deriving from current register state is
         * a harmless no-op when the region isn't enabled or nothing
         * relevant actually changed.
         */
        if (reg_off == OTFDEC_REG_CONFIGR || reg_off == OTFDEC_REG_START_ADDR ||
            reg_off == OTFDEC_REG_END_ADDR || reg_off == OTFDEC_REG_NONCER0 ||
            reg_off == OTFDEC_REG_NONCER1 || reg_off == OTFDEC_REG_KEYR0 ||
            reg_off == OTFDEC_REG_KEYR1 || reg_off == OTFDEC_REG_KEYR2 ||
            reg_off == OTFDEC_REG_KEYR3) {
            otfdec_update_region_mapping(s, region_id_1based);
        }
    }
}

static const MemoryRegionOps gnw_h7b0_otfdec_ops = {
    .read = gnw_h7b0_otfdec_read,
    .write = gnw_h7b0_otfdec_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void gnw_h7b0_otfdec_init(Object *obj)
{
    GnwH7B0OtfdecState *s = GNW_H7B0_OTFDEC(obj);

    memory_region_init_io(&s->mmio, obj, &gnw_h7b0_otfdec_ops, s,
                           TYPE_GNW_H7B0_OTFDEC, GNW_H7B0_OTFDEC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static const VMStateDescription vmstate_gnw_h7b0_otfdec = {
    .name = TYPE_GNW_H7B0_OTFDEC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, GnwH7B0OtfdecState,
                             GNW_H7B0_OTFDEC_SIZE / 4),
        VMSTATE_END_OF_LIST()
    }
};

static void gnw_h7b0_otfdec_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_gnw_h7b0_otfdec;
    device_class_set_legacy_reset(dc, gnw_h7b0_otfdec_reset);
}

static const TypeInfo gnw_h7b0_otfdec_info = {
    .name          = TYPE_GNW_H7B0_OTFDEC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GnwH7B0OtfdecState),
    .instance_init = gnw_h7b0_otfdec_init,
    .class_init    = gnw_h7b0_otfdec_class_init,
};

static void gnw_h7b0_otfdec_register_types(void)
{
    type_register_static(&gnw_h7b0_otfdec_info);
}
type_init(gnw_h7b0_otfdec_register_types)
