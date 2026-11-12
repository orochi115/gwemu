/*
 * STM32H7B0 OTFDEC (on-the-fly decryption) minimal model
 * (Nintendo Game & Watch)
 *
 * Scope: enough for stock firmware's HAL_OTFDEC_RegionSetKey() to
 * succeed -- registers are plain read/write shadows, plus the one real
 * behavior firmware checks: writing a region's KEYR3 recomputes the
 * region's key CRC (HAL_OTFDEC_KeyCRCComputation()'s exact algorithm,
 * from sdk/stm32h7xx-hal-driver/Src/stm32h7xx_hal_otfdec.c) into
 * CONFIGR[15:8] (KEYCRC), which HAL_OTFDEC_RegionSetKey() reads back
 * and compares against its own software-computed CRC before reporting
 * success. Found via 2026-07-12 lockstep tracing: stock Zelda's boot
 * traps in its own "spin forever on HAL error" loop when SetKey fails.
 *
 * NOT modeled: actual decryption of memory-mapped OCTOSPI reads.
 * Stock extflash dumps used with this machine are the raw (encrypted
 * where applicable) images; if firmware later depends on OTFDEC
 * actually transforming fetched data, that needs a real AES-CTR
 * implementation hooked into the OSPI memory-mapped path.
 *
 * Region layout (sdk/cmsis-device-h7/Include/stm32h7b0xx.h): 4 regions
 * at base+0x20, stride 0x30; per region: CONFIGR, START_ADDR, END_ADDR,
 * NONCER0, NONCER1, KEYR0..KEYR3.
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
#ifndef HW_MISC_GNW_H7B0_OTFDEC_H
#define HW_MISC_GNW_H7B0_OTFDEC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "system/memory.h"

#define TYPE_GNW_H7B0_OTFDEC "gnw-h7b0-otfdec"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0OtfdecState, GNW_H7B0_OTFDEC)

void gnw_h7b0_otfdec_snapshot_restore(GnwH7B0OtfdecState *s);

#define GNW_H7B0_OTFDEC_SIZE 0x400

#define OTFDEC_REGION_BASE   0x20
#define OTFDEC_REGION_STRIDE 0x30
#define OTFDEC_NUM_REGIONS   4

/* Per-region register offsets (from the region's own base). */
#define OTFDEC_REG_CONFIGR    0x00
#define OTFDEC_REG_START_ADDR 0x04
#define OTFDEC_REG_END_ADDR   0x08
#define OTFDEC_REG_NONCER0    0x0c
#define OTFDEC_REG_NONCER1    0x10
#define OTFDEC_REG_KEYR0      0x14
#define OTFDEC_REG_KEYR1      0x18
#define OTFDEC_REG_KEYR2      0x1c
#define OTFDEC_REG_KEYR3      0x20

#define OTFDEC_CONFIGR_KEYCRC_SHIFT 8
#define OTFDEC_CONFIGR_KEYCRC_MASK  (0xFFU << OTFDEC_CONFIGR_KEYCRC_SHIFT)

struct GnwH7B0OtfdecState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t regs[GNW_H7B0_OTFDEC_SIZE / 4];

    /*
     * Real-decryption overlay: one high-priority RAM subregion per region
     * slot (x=1..4), installed over the guest's extflash address range
     * whenever that region's REG_EN gets set. Unlike an IO-callback
     * MemoryRegion (the first working implementation, replaced here),
     * this is decrypted EAGERLY, all at once, into a plain host RAM
     * buffer at enable time -- real AES-128-CTR decryption per RM0455
     * 41.3.4, see gnw_h7b0_otfdec.c's module doc comment for the full
     * algorithm -- then mapped as ordinary RAM.
     *
     * This isn't just an optimization: an IO-type region forces every
     * single CPU access through it (crucially, every INSTRUCTION FETCH,
     * if code executes from the decrypted range -- exactly what OTFDEC
     * is for) into TCG's slow interpreted MMIO-dispatch path, with no
     * direct-execution/TLB fast path at all. Real hardware's OTFDEC is a
     * transparent hardware pipeline in front of a normal flash read --
     * the CPU sees ordinary memory-read latency, not a trap per access.
     * A one-time bulk decrypt into real RAM is the emulation equivalent:
     * a small fixed cost when a region is enabled, full-speed TCG
     * execution/access for everything after. Measured as the actual
     * fix for a severe, real slowdown the original IO-overlay design
     * had (confirmed via `perf record` against a live QEMU session --
     * the AES/decrypt code itself was under 1% of total CPU time even
     * in the slow version; the cost was entirely the IO-region dispatch
     * path itself, not the cryptography).
     */
    MemoryRegion decrypt_overlay[OTFDEC_NUM_REGIONS];
    uint8_t *decrypted_buf[OTFDEC_NUM_REGIONS];
    bool overlay_mapped[OTFDEC_NUM_REGIONS];
    hwaddr overlay_mapped_base[OTFDEC_NUM_REGIONS];
    MemoryRegion *system_memory;
    uint8_t *extflash_host_ptr;
    hwaddr extflash_base;
    hwaddr extflash_size;
};

void gnw_h7b0_otfdec_set_extflash(GnwH7B0OtfdecState *s,
                                   MemoryRegion *system_memory,
                                   MemoryRegion *extflash,
                                   hwaddr extflash_base,
                                   hwaddr extflash_size);

#endif
