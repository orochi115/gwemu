/*
 * STM32H7B0 ADC1/ADC2 minimal stub (Nintendo Game & Watch)
 *
 * Not cycle-accurate, not a real conversion model. Purpose: real
 * firmware's ADC init sets CR's ADEN bit then polls the matching
 * ISR's ADRDY bit and must not hang forever doing so (found via a
 * gnw-chainloader boot hang in board_adc_init()) -- see
 * docs/roadmap.md Phase 1. CR's ADCAL (bit 31, self-clearing
 * "calibration in progress" flag on real hardware) is cleared
 * instantly for the same reason: patched-zelda-bank1.bin's ADC init
 * sets it and spins on it clearing, which never happened before this
 * fix.
 *
 * By default regular conversions (CR's ADSTART) are instant: ISR's EOC is
 * set and DR is loaded with GNW_H7B0_ADC_FULL_BATTERY_RAW as soon as ADSTART
 * is written, and the matching NVIC line (ADC_IRQn, shared by ADC1/ADC2) is
 * pulsed if IER's EOCIE is set. Opt-in timing mode instead sequences supported
 * regular ranks against RCC-selected virtual clock rates and drives the IRQ
 * as a level from sticky ISR/IER flags. This covers both firmware's battery
 * read paths: gnw-chainloader's board_get_battery_raw() busy-polls ISR's
 * EOC after setting ADSTART, while retro-go-sd's bq24072.c calls
 * HAL_ADC_Start_IT() and reads the result from HAL_ADC_ConvCpltCallback(),
 * which only fires on a real ADC interrupt. Only ADC1's channel-4 (PC4)
 * battery-sense reading is modeled; every other register is a plain
 * read-what-was-written shadow with no side effects.
 *
 * Covers both ADC1 (this device's own base) and ADC2 (mapped 0x100
 * higher, per sdk/cmsis-device-h7/Include/stm32h7b0xx.h's ADC2_BASE =
 * ADC1_BASE + 0x100) plus the shared ADC12_COMMON registers at +0x300,
 * as one 0x400 register file -- not two separate device instances,
 * since firmware addresses them as a single contiguous block via
 * ADC1/ADC2 pointers into the same peripheral window.
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

#ifndef HW_MISC_GNW_H7B0_ADC_H
#define HW_MISC_GNW_H7B0_ADC_H

#include "hw/core/sysbus.h"
#include "hw/misc/gnw_h7b0_rcc.h"
#include "qom/object.h"
#include "hw/misc/gnw_h7b0_dma.h"
#include "qemu/timer.h"

#define TYPE_GNW_H7B0_ADC "gnw-h7b0-adc"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0AdcState, GNW_H7B0_ADC)

/* DMAMUX1 request line for ADC2 (sdk/stm32h7xx-hal-driver/Inc/
 * stm32h7xx_hal_dma.h's DMA_REQUEST_ADC2) -- used to register this
 * device's DMA notifier the same way gnw_h7b0_sai1.c/gnw_h7b0_hash.c
 * register theirs (see gnw_h7b0_dma_set_request_notifier()). */
#define GNW_H7B0_ADC_DMA_REQUEST_ADC2 10
#define GNW_H7B0_ADC_DMA_PROVIDER_ID 0x41444332U /* "ADC2" */

#define GNW_H7B0_ADC_SIZE   0x400

/* Per-instance offsets within each ADC1/ADC2 0x100 sub-block. */
#define GNW_H7B0_ADC_ISR    0x00
#define ADC_ISR_ADRDY       (1U << 0)
#define ADC_ISR_EOC         (1U << 2)
#define GNW_H7B0_ADC_IER    0x04
#define ADC_IER_EOCIE       (1U << 2)
#define GNW_H7B0_ADC_CR     0x08
#define ADC_CR_ADEN         (1U << 0)
#define ADC_CR_ADSTART      (1U << 2)
#define ADC_CR_ADCAL        (1U << 31)
#define GNW_H7B0_ADC_DR     0x40
#define GNW_H7B0_ADC_SQR1   0x30
#define GNW_H7B0_ADC_SQR2   0x34
#define GNW_H7B0_ADC_SQR3   0x38
#define GNW_H7B0_ADC_SQR4   0x3c
#define GNW_H7B0_ADC_CFGR   0x0c
#define GNW_H7B0_ADC_CFGR2  0x10
#define GNW_H7B0_ADC_SMPR1  0x14
#define GNW_H7B0_ADC_SMPR2  0x18
#define GNW_H7B0_ADC_COMMON_CCR 0x308
#define ADC_SQR1_SQ1_SHIFT  6
#define ADC_SQR1_SQ1_MASK   (0x1FU << ADC_SQR1_SQ1_SHIFT)
#define ADC_SQR1_L_SHIFT    0
#define ADC_SQR1_L_MASK     (0xFU << ADC_SQR1_L_SHIFT)
#define ADC_CFGR_RES_SHIFT  2
#define ADC_CFGR_RES_MASK   (0x7U << ADC_CFGR_RES_SHIFT)
#define ADC_CFGR_CONT       (1U << 13)
#define ADC_CFGR_DISCEN     (1U << 16)
#define ADC_CFGR_EXTEN_MASK (0x3U << 10)
#define ADC_CFGR2_JOVSE    (1U << 1)
#define ADC_CFGR2_ROVSE    (1U << 0)
#define ADC_CR_ADSTP        (1U << 4)
#define ADC_ISR_EOS         (1U << 3)
#define ADC_IER_EOSIE       (1U << 3)
#define ADC_CCR_DUAL_MASK   (0x1FU)

/* Regular-sequence channel numbers this stub distinguishes (see DR-value
 * selection in gnw_h7b0_adc_write()). Everything else still gets
 * GNW_H7B0_ADC_FULL_BATTERY_RAW, preserving existing battery-read
 * behavior for any channel not explicitly listed here. */
#define ADC_CHANNEL_VREFINT  19

/*
 * Fixed regular-conversion result returned on every ADSTART, standing in for
 * board_get_battery_raw()'s PC4/channel-4 battery-voltage read (see
 * gnw-chainloader's board.c and retro-go-sd's bq24072.c). Both firmwares
 * treat raw >= 13000 as BQ24072_BATTERY_FULL (100%).
 *
 * Was 13500 -- calibrated only against that retro-go-sd/gnw-chainloader
 * threshold, never checked against stock firmware. Confirmed wrong via
 * 2026-07-12 breakpoint-lockstep-tracing: stock Zelda's own battery-level
 * lookup (FUN_0800320e in a Ghidra decompile) uses up to 4 different
 * threshold tables depending on a charging-state flag/GPIOE-bit combo,
 * and one of those tables' max threshold is ~41974 -- far above 13500 --
 * causing stock firmware to read battery level 0 on QEMU (vs a real
 * nonzero level on real hardware, which has an actual battery), silently
 * skipping an entire boot-progress branch (confirmed: this is the root
 * cause of stock Zelda's display/LTDC-init path never being reached in
 * QEMU, traced through ~6 layers of intermediate function calls -- see
 * docs/session-2026-07-12-breakpoint-lockstep-tracing.md part 4).
 * 0xFFFF (max a 16-bit DR register can hold) clears every threshold table
 * in both firmware families and is the semantically correct "full
 * battery" reading regardless of which table gets selected.
 */
#define GNW_H7B0_ADC_FULL_BATTERY_RAW 0xFFFF

/*
 * Fixed regular-conversion result for the VREFINT internal channel
 * (SQR1.SQ1 == ADC_CHANNEL_VREFINT), distinct from
 * GNW_H7B0_ADC_FULL_BATTERY_RAW. Added because stm32h7b0-diag's
 * case_adc_vrefint_correct.c/case_adc_vrefint_throughput.c both read
 * VREFINT specifically (never the battery channel) and independently
 * reject any reading >= 0xFFF0 as "stuck at rail" -- GNW_H7B0_ADC_
 * FULL_BATTERY_RAW's 0xFFFF was tripping that check on every sample
 * before this stub distinguished channels at all (every regular
 * conversion returned the same fixed battery value regardless of which
 * channel was actually selected). An arbitrary stable mid-scale value
 * satisfies both diag cases (consistency across samples, non-rail) --
 * no real VDDA/VREFINT calibration data exists for this board (see
 * case_adc_vrefint_correct.c's header comment), so there is no "more
 * correct" value to pick instead.
 */
#define GNW_H7B0_ADC_VREFINT_RAW 0x6000

/* ADC1/ADC2 sub-block stride and count within the combined register file. */
#define GNW_H7B0_ADC_INSTANCE_STRIDE 0x100
#define GNW_H7B0_ADC_INSTANCE_COUNT  2

typedef struct GnwH7B0AdcTimerContext {
    GnwH7B0AdcState *adc;
    unsigned int instance;
} GnwH7B0AdcTimerContext;

struct GnwH7B0AdcState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    qemu_irq irq;
    uint32_t regs[GNW_H7B0_ADC_SIZE / 4];
    GnwH7B0DmaState *dma;
    GnwH7B0RccState *rcc;
    bool timing_mode;
    /* External channel4 input survives peripheral resets. Disabled by default. */
    bool battery_input;
    uint16_t battery_raw;

    /* Timer state is indexed ADC1=0, ADC2=1. Timers themselves are rebuilt
     * from the migrated remaining conversion phase in post_load. */
    QEMUTimer *conversion_timer[GNW_H7B0_ADC_INSTANCE_COUNT];
    GnwH7B0AdcTimerContext conversion_timer_ctx[
        GNW_H7B0_ADC_INSTANCE_COUNT];
    bool conversion_active[GNW_H7B0_ADC_INSTANCE_COUNT];
    uint8_t sequence_rank[GNW_H7B0_ADC_INSTANCE_COUNT];
    uint8_t sequence_length[GNW_H7B0_ADC_INSTANCE_COUNT];
    uint32_t remaining_half_cycles[GNW_H7B0_ADC_INSTANCE_COUNT];
    uint64_t phase_remainder[GNW_H7B0_ADC_INSTANCE_COUNT];
    int64_t last_update_ns[GNW_H7B0_ADC_INSTANCE_COUNT];
    uint32_t active_kernel_hz[GNW_H7B0_ADC_INSTANCE_COUNT];
    bool migrated_timing_mode;
};

/* Wires this ADC up to the shared DMA controller, registering a request
 * notifier on GNW_H7B0_ADC_DMA_REQUEST_ADC2 so DMA2's ADC2-bound stream
 * (e.g. case_adc_vrefint_throughput.c's DMA2_Stream1) actually receives
 * real conversion data instead of the DMA controller's generic transfer-
 * timing model completing with nothing behind it (that model only
 * simulates flag/IRQ timing -- real byte movement for non-M2M streams is
 * the source peripheral's job, same as gnw_h7b0_sai1.c/gnw_h7b0_hash.c). */
void gnw_h7b0_adc_set_dma(GnwH7B0AdcState *s, GnwH7B0DmaState *dma);
/* Connect the combined ADC1/ADC2 block to RCC after both devices exist. */
void gnw_h7b0_adc_set_rcc(GnwH7B0AdcState *s, GnwH7B0RccState *rcc);
void gnw_h7b0_adc_set_battery(GnwH7B0AdcState *s, uint16_t raw);

#endif
