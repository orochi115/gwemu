/*
 * STM32H7B0 ADC1/ADC2 minimal stub (Nintendo Game & Watch)
 *
 * See gnw_h7b0_adc.h for scope/rationale.
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
#include "qemu/timer.h"
#include "migration/vmstate.h"
#include "exec/cpu-common.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/gnw_h7b0_adc.h"

#define ADC_HALF_CYCLE_NS_NUMERATOR 500000000ULL
#define ADC_CLOCK_CCR_CKMODE_SHIFT 16
#define ADC_CLOCK_CCR_PRESC_SHIFT 18
#define ADC_CFGR_RESOLUTION_SHIFT 2
#define ADC_CFGR_RESOLUTION_MASK  (0x7U << ADC_CFGR_RESOLUTION_SHIFT)
#define ADC_CFGR_CONT_BIT         (1U << 13)
#define ADC_CFGR_DISCEN_BIT       (1U << 16)
#define ADC_CFGR_EXTEN_BITS       (0x3U << 10)
#define ADC_CR_ADSTP_BIT          (1U << 4)
#define ADC_ISR_EOS_BIT           (1U << 3)
#define ADC_IER_EOSIE_BIT         (1U << 3)
#define ADC_MAX_CONVERSION_HALF_CYCLES (1621U + 17U)

static void gnw_h7b0_adc_dma_state_changed(GnwH7B0AdcState *s);

static int gnw_h7b0_adc_instance_index(hwaddr base)
{
    return base == 0 ? 0 : 1;
}

static uint32_t gnw_h7b0_adc_common_ccr(GnwH7B0AdcState *s)
{
    return s->regs[GNW_H7B0_ADC_COMMON_CCR >> 2];
}

static uint32_t gnw_h7b0_adc_kernel_hz(GnwH7B0AdcState *s)
{
    return s->rcc ? gnw_h7b0_rcc_get_adc_kernel_hz(
                        s->rcc, gnw_h7b0_adc_common_ccr(s)) : 0;
}

static bool gnw_h7b0_adc_rank_channel(GnwH7B0AdcState *s, hwaddr base,
                                      unsigned rank, unsigned *channel)
{
    static const hwaddr sqr_offsets[] = {
        GNW_H7B0_ADC_SQR1, GNW_H7B0_ADC_SQR2,
        GNW_H7B0_ADC_SQR3, GNW_H7B0_ADC_SQR4,
    };
    static const uint8_t first_ranks[] = { 1, 5, 10, 15 };
    static const uint8_t rank_counts[] = { 4, 5, 5, 2 };

    for (unsigned i = 0; i < ARRAY_SIZE(sqr_offsets); i++) {
        if (rank >= first_ranks[i] &&
            rank < first_ranks[i] + rank_counts[i]) {
            unsigned slot = rank - first_ranks[i];
            uint32_t reg = s->regs[(base + sqr_offsets[i]) >> 2];
            unsigned shift = slot * 6 + (i == 0 ? 6 : 0);

            *channel = (reg >> shift) & 0x1f;
            return true;
        }
    }
    return false;
}

static bool gnw_h7b0_adc_conversion_half_cycles(GnwH7B0AdcState *s,
                                                hwaddr base, unsigned rank,
                                                uint32_t *half_cycles)
{
    static const uint16_t sample_half_cycles[8] = {
        3, 5, 17, 33, 65, 129, 775, 1621,
    };
    /* RM0455 H7B0 Table 201 TSAR: 16/14/12/10/8-bit respectively. */
    static const uint8_t sar_half_cycles[5] = { 17, 15, 13, 11, 9 };
    unsigned channel;
    uint32_t cfgr = s->regs[(base + GNW_H7B0_ADC_CFGR) >> 2];
    unsigned resolution = (cfgr & ADC_CFGR_RESOLUTION_MASK) >>
                          ADC_CFGR_RESOLUTION_SHIFT;
    unsigned sampling;

    if (resolution >= ARRAY_SIZE(sar_half_cycles) ||
        !gnw_h7b0_adc_rank_channel(s, base, rank, &channel) ||
        channel >= 20) {
        return false;
    }

    unsigned sample_reg = channel < 10 ? GNW_H7B0_ADC_SMPR1 :
                                         GNW_H7B0_ADC_SMPR2;
    unsigned sample_slot = channel < 10 ? channel : channel - 10;
    sampling = (s->regs[(base + sample_reg) >> 2] >> (sample_slot * 3)) & 7;
    *half_cycles = sample_half_cycles[sampling] + sar_half_cycles[resolution];
    return true;
}

static bool gnw_h7b0_adc_sequence_supported(GnwH7B0AdcState *s,
                                            hwaddr base, unsigned length)
{
    uint32_t cfgr = s->regs[(base + GNW_H7B0_ADC_CFGR) >> 2];
    uint32_t cfgr2 = s->regs[(base + GNW_H7B0_ADC_CFGR2) >> 2];
    uint32_t common_ccr = gnw_h7b0_adc_common_ccr(s);

    if (!length || length > 16 || (cfgr & (ADC_CFGR_EXTEN_BITS |
                                             ADC_CFGR_DISCEN_BIT)) ||
        (cfgr2 & (ADC_CFGR2_JOVSE | ADC_CFGR2_ROVSE)) ||
        (common_ccr & ADC_CCR_DUAL_MASK)) {
        return false;
    }

    for (unsigned rank = 1; rank <= length; rank++) {
        uint32_t half_cycles;

        if (!gnw_h7b0_adc_conversion_half_cycles(s, base, rank,
                                                 &half_cycles) ||
            half_cycles > ADC_MAX_CONVERSION_HALF_CYCLES) {
            return false;
        }
    }
    return true;
}

static uint64_t gnw_h7b0_adc_conversion_duration_ns(
    uint32_t remaining_half_cycles, uint64_t phase_remainder,
    uint32_t kernel_hz)
{
    if (!remaining_half_cycles || !kernel_hz) {
        return 0;
    }

    uint64_t numerator = (uint64_t)remaining_half_cycles *
                         ADC_HALF_CYCLE_NS_NUMERATOR;
    if (numerator <= phase_remainder) {
        return 1;
    }
    numerator -= phase_remainder;
    return MAX((numerator + kernel_hz - 1) / kernel_hz, 1ULL);
}

/* Settle a running conversion with exact fractional half-cycle phase. The
 * timer deadline is rounded up to integer nanoseconds, so preserve its small
 * overshoot as phase for the next conversion instead of accumulating drift. */
static void gnw_h7b0_adc_settle(GnwH7B0AdcState *s, unsigned instance,
                                 int64_t now_ns)
{
    uint32_t hz = s->active_kernel_hz[instance];
    int64_t previous = s->last_update_ns[instance];
    uint32_t remaining = s->remaining_half_cycles[instance];
    uint64_t phase = s->phase_remainder[instance];
    uint64_t elapsed, deadline_ns, numerator, target;

    if (!s->conversion_active[instance] || now_ns <= previous || !hz ||
        !remaining) {
        s->last_update_ns[instance] = now_ns;
        return;
    }

    deadline_ns = gnw_h7b0_adc_conversion_duration_ns(remaining, phase, hz);
    elapsed = MIN((uint64_t)(now_ns - previous), deadline_ns);
    numerator = elapsed * hz + phase;
    target = (uint64_t)remaining * ADC_HALF_CYCLE_NS_NUMERATOR;
    if (numerator >= target) {
        s->remaining_half_cycles[instance] = 0;
        s->phase_remainder[instance] = numerator - target;
    } else {
        uint64_t progressed = numerator / ADC_HALF_CYCLE_NS_NUMERATOR;
        s->remaining_half_cycles[instance] -= progressed;
        s->phase_remainder[instance] =
            numerator % ADC_HALF_CYCLE_NS_NUMERATOR;
    }
    s->last_update_ns[instance] = now_ns;
}

static void gnw_h7b0_adc_arm(GnwH7B0AdcState *s, unsigned instance,
                              int64_t now_ns)
{
    uint32_t hz = gnw_h7b0_adc_kernel_hz(s);
    uint64_t delay_ns;

    s->active_kernel_hz[instance] = hz;
    s->last_update_ns[instance] = now_ns;
    if (!s->conversion_active[instance] || !hz ||
        !s->remaining_half_cycles[instance]) {
        timer_del(s->conversion_timer[instance]);
        return;
    }

    delay_ns = gnw_h7b0_adc_conversion_duration_ns(
        s->remaining_half_cycles[instance],
        s->phase_remainder[instance], hz);
    int64_t deadline = delay_ns > INT64_MAX - now_ns ? INT64_MAX :
                                                       now_ns + delay_ns;
    timer_mod(s->conversion_timer[instance], deadline);
}

static void gnw_h7b0_adc_complete_rank(GnwH7B0AdcState *s,
                                       unsigned instance);

/* ADC1 and ADC2 share one NVIC input. In timing mode the peripheral drives
 * that input as a level derived from its sticky status and enable bits; the
 * legacy instantaneous compatibility path intentionally retains its pulse. */
static void gnw_h7b0_adc_update_irq(GnwH7B0AdcState *s)
{
    bool pending = false;

    if (s->timing_mode) {
        for (unsigned i = 0; i < GNW_H7B0_ADC_INSTANCE_COUNT; i++) {
            hwaddr base = (hwaddr)i * GNW_H7B0_ADC_INSTANCE_STRIDE;
            uint32_t isr = s->regs[(base + GNW_H7B0_ADC_ISR) >> 2];
            uint32_t ier = s->regs[(base + GNW_H7B0_ADC_IER) >> 2];

            pending |= ((isr & ADC_ISR_EOC) && (ier & ADC_IER_EOCIE)) ||
                       ((isr & ADC_ISR_EOS) && (ier & ADC_IER_EOSIE));
        }
        qemu_set_irq(s->irq, pending);
    }
}

static void gnw_h7b0_adc_update_kernel(GnwH7B0AdcState *s)
{
    int64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    for (unsigned i = 0; i < GNW_H7B0_ADC_INSTANCE_COUNT; i++) {
        if (s->conversion_active[i]) {
            gnw_h7b0_adc_settle(s, i, now_ns);
            if (!s->remaining_half_cycles[i]) {
                /* A rate change may arrive at the same virtual instant as
                 * the timer deadline. Complete the elapsed rank before
                 * arming against the new rate. */
                gnw_h7b0_adc_complete_rank(s, i);
            } else {
                gnw_h7b0_adc_arm(s, i, now_ns);
            }
        }
    }
}

void gnw_h7b0_adc_set_battery(GnwH7B0AdcState *s, uint16_t raw)
{
    s->battery_input = true;
    s->battery_raw = raw;
}

static uint32_t gnw_h7b0_adc_result(GnwH7B0AdcState *s, hwaddr base,
                                     unsigned rank)
{
    unsigned channel;

    if (gnw_h7b0_adc_rank_channel(s, base, rank, &channel)) {
        if (base == GNW_H7B0_ADC_INSTANCE_STRIDE &&
            channel == ADC_CHANNEL_VREFINT) {
            return GNW_H7B0_ADC_VREFINT_RAW;
        }
        if (s->battery_input && channel == 4) {
            return s->battery_raw;
        }
    }
    return GNW_H7B0_ADC_FULL_BATTERY_RAW;
}

static void gnw_h7b0_adc_complete_rank(GnwH7B0AdcState *s,
                                       unsigned instance)
{
    hwaddr base = (hwaddr)instance * GNW_H7B0_ADC_INSTANCE_STRIDE;
    uint32_t isr_off = base + GNW_H7B0_ADC_ISR;
    uint32_t cr_off = base + GNW_H7B0_ADC_CR;
    uint32_t ier = s->regs[(base + GNW_H7B0_ADC_IER) >> 2];
    uint32_t cfgr = s->regs[(base + GNW_H7B0_ADC_CFGR) >> 2];
    bool last = s->sequence_rank[instance] + 1 >=
                s->sequence_length[instance];

    s->regs[(base + GNW_H7B0_ADC_DR) >> 2] =
        gnw_h7b0_adc_result(s, base, s->sequence_rank[instance] + 1);
    s->regs[isr_off >> 2] |= ADC_ISR_EOC;
    if (last) {
        s->regs[isr_off >> 2] |= ADC_ISR_EOS;
    }
    if ((ier & ADC_IER_EOCIE) || (last && (ier & ADC_IER_EOSIE_BIT))) {
        if (s->timing_mode) {
            gnw_h7b0_adc_update_irq(s);
        } else {
            qemu_irq_pulse(s->irq);
        }
    }

    if (!last) {
        s->sequence_rank[instance]++;
    } else if (cfgr & ADC_CFGR_CONT_BIT) {
        s->sequence_rank[instance] = 0;
    } else {
        s->conversion_active[instance] = false;
        s->remaining_half_cycles[instance] = 0;
        s->regs[cr_off >> 2] &= ~ADC_CR_ADSTART;
        gnw_h7b0_adc_dma_state_changed(s);
        return;
    }

    uint32_t next_half_cycles;
    if (!gnw_h7b0_adc_conversion_half_cycles(
            s, base, s->sequence_rank[instance] + 1, &next_half_cycles)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "gnw-h7b0-adc: unsupported rank configuration while "
                      "sequencing ADC%u\n", instance + 1);
        s->conversion_active[instance] = false;
        s->regs[cr_off >> 2] &= ~ADC_CR_ADSTART;
        gnw_h7b0_adc_dma_state_changed(s);
        return;
    }
    s->remaining_half_cycles[instance] = next_half_cycles;
    gnw_h7b0_adc_arm(s, instance,
                     qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

static void gnw_h7b0_adc_conversion_tick(void *opaque)
{
    GnwH7B0AdcTimerContext *ctx = opaque;
    GnwH7B0AdcState *s = ctx->adc;
    unsigned instance = ctx->instance;
    int64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    gnw_h7b0_adc_settle(s, instance, now_ns);
    if (s->remaining_half_cycles[instance]) {
        gnw_h7b0_adc_arm(s, instance, now_ns);
        return;
    }
    gnw_h7b0_adc_complete_rank(s, instance);
}

static bool gnw_h7b0_adc_start_timing_conversion(GnwH7B0AdcState *s,
                                                  hwaddr base)
{
    unsigned instance = gnw_h7b0_adc_instance_index(base);
    unsigned count = ((s->regs[(base + GNW_H7B0_ADC_SQR1) >> 2] &
                       ADC_SQR1_L_MASK) >> ADC_SQR1_L_SHIFT) + 1;
    uint32_t first_half_cycles;

    if (!gnw_h7b0_adc_sequence_supported(s, base, count)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "gnw-h7b0-adc: unsupported regular-sequence "
                      "configuration (trigger, mode, channel, resolution, "
                      "or oversampling)\n");
        return false;
    }
    if (!(s->regs[(base + GNW_H7B0_ADC_CR) >> 2] & ADC_CR_ADEN)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "gnw-h7b0-adc: ADSTART while ADC%u is disabled\n",
                      instance + 1);
        return false;
    }
    if (!gnw_h7b0_adc_kernel_hz(s)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "gnw-h7b0-adc: timing mode has unsupported or stopped "
                      "ADC clock configuration\n");
        return false;
    }
    g_assert(gnw_h7b0_adc_conversion_half_cycles(s, base, 1,
                                                 &first_half_cycles));

    s->sequence_rank[instance] = 0;
    s->sequence_length[instance] = count;
    s->remaining_half_cycles[instance] = first_half_cycles;
    s->phase_remainder[instance] = 0;
    s->conversion_active[instance] = true;
    gnw_h7b0_adc_arm(s, instance,
                     qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    return true;
}

static void gnw_h7b0_adc_rcc_clock_changed(void *opaque)
{
    GnwH7B0AdcState *s = GNW_H7B0_ADC(opaque);

    gnw_h7b0_adc_update_kernel(s);
    gnw_h7b0_adc_dma_state_changed(s);
}

void gnw_h7b0_adc_set_rcc(GnwH7B0AdcState *s, GnwH7B0RccState *rcc)
{
    s->rcc = rcc;
    if (rcc) {
        gnw_h7b0_rcc_add_adc_clock_observer(
            rcc, gnw_h7b0_adc_rcc_clock_changed, s);
    }
    gnw_h7b0_adc_dma_state_changed(s);
}

/*
 * DMA request notifier (see gnw_h7b0_adc.h): fills the destination
 * buffer's half/full-transfer portion with GNW_H7B0_ADC_VREFINT_RAW,
 * one 32-bit word per item (matches case_adc_vrefint_throughput.c's
 * DMA_PDATAALIGN_WORD/DMA_MDATAALIGN_WORD config -- the only ADC-DMA
 * consumer that exists in this codebase so far). Real hardware would
 * deliver each stream's actual per-channel conversion result per item;
 * this stub has only one fixed regular-conversion channel value at a
 * time (see gnw_h7b0_adc_write()'s ADSTART handling), so every item in
 * the transfer gets the same value -- sufficient for what any known
 * consumer actually checks (non-poison, non-rail, real DMA motion
 * happened), not a claim of per-item conversion fidelity.
 */
static void gnw_h7b0_adc_dma_notify(void *opaque, bool half,
                                     uint32_t m0ar, uint32_t ndtr)
{
    uint32_t half_items = ndtr / 2;
    uint32_t addr = m0ar + (half ? 0 : half_items * sizeof(uint32_t));

    if (half_items == 0) {
        return;
    }

    for (uint32_t i = 0; i < half_items; i++) {
        uint32_t val = GNW_H7B0_ADC_VREFINT_RAW;
        cpu_physical_memory_write(addr + (hwaddr)i * sizeof(uint32_t),
                                   &val, sizeof(val));
    }
}

/* The DMA model consumes an item-rate callback to pace its half/full-buffer
 * notifications. In timing mode return the sequence-average conversion
 * rate; the notifier still writes a completed half-buffer in bulk, so this
 * does not claim one DMA bus request or IRQ per ADC sample. */
static uint32_t gnw_h7b0_adc_dma_rate(void *opaque)
{
    GnwH7B0AdcState *s = opaque;
    hwaddr base = GNW_H7B0_ADC_INSTANCE_STRIDE;
    unsigned count;
    uint64_t total_half_cycles = 0;
    uint32_t hz;

    if (!s->timing_mode) {
        return 0;
    }
    count = ((s->regs[(base + GNW_H7B0_ADC_SQR1) >> 2] & ADC_SQR1_L_MASK) >>
             ADC_SQR1_L_SHIFT) + 1;
    hz = gnw_h7b0_adc_kernel_hz(s);
    if (!hz || !gnw_h7b0_adc_sequence_supported(s, base, count)) {
        return 0;
    }
    for (unsigned rank = 1; rank <= count; rank++) {
        uint32_t half_cycles;
        if (!gnw_h7b0_adc_conversion_half_cycles(s, base, rank,
                                                  &half_cycles)) {
            return 0;
        }
        total_half_cycles += half_cycles;
    }
    if (!total_half_cycles) {
        return 0;
    }
    return MIN(((uint64_t)hz * 2 * count) / total_half_cycles, UINT32_MAX);
}

static uint32_t gnw_h7b0_adc_gcd(uint32_t a, uint32_t b)
{
    while (b) {
        uint32_t r = a % b;
        a = b;
        b = r;
    }
    return a;
}

/* The typed DMA provider reports an exact per-conversion cadence only for
 * uniform rank durations. A sequence average would shift HT/TC boundaries
 * across rank changes and is therefore left UNKNOWN. */
static GnwH7B0DmaRequestState gnw_h7b0_adc_request_state(void *opaque)
{
    GnwH7B0AdcState *s = opaque;
    hwaddr base = GNW_H7B0_ADC_INSTANCE_STRIDE;
    unsigned count = ((s->regs[(base + GNW_H7B0_ADC_SQR1) >> 2] &
                       ADC_SQR1_L_MASK) >> ADC_SQR1_L_SHIFT) + 1;
    uint32_t common_ccr = gnw_h7b0_adc_common_ccr(s);
    uint32_t kernel_hz;
    uint32_t first_half_cycles = 0;
    bool known;

    GnwH7B0DmaRequestState state = {
        .rate_state = GNW_H7B0_DMA_RATE_UNKNOWN,
        .request_active = s->conversion_active[1] &&
                          !!(s->regs[(base + GNW_H7B0_ADC_CFGR) >> 2] &
                             (1U << 0)),
        .provider_identity = GNW_H7B0_ADC_DMA_PROVIDER_ID,
    };

    if (!s->rcc) {
        return state;
    }
    known = gnw_h7b0_rcc_get_adc_clock_info(s->rcc, common_ccr,
                                             &kernel_hz);
    if (!known) {
        return state;
    }
    if (!kernel_hz) {
        state.rate_state = GNW_H7B0_DMA_RATE_STOPPED;
        return state;
    }
    if (!gnw_h7b0_adc_sequence_supported(s, base, count)) {
        return state;
    }
    for (unsigned rank = 1; rank <= count; rank++) {
        uint32_t half_cycles;

        if (!gnw_h7b0_adc_conversion_half_cycles(s, base, rank,
                                                 &half_cycles)) {
            return state;
        }
        if (rank == 1) {
            first_half_cycles = half_cycles;
        } else if (half_cycles != first_half_cycles) {
            return state;
        }
    }

    uint64_t numerator64 = (uint64_t)kernel_hz * 2;
    if (numerator64 > UINT32_MAX || !first_half_cycles) {
        return state;
    }
    uint32_t numerator = numerator64;
    uint32_t divisor = gnw_h7b0_adc_gcd(numerator, first_half_cycles);
    state.rate_state = GNW_H7B0_DMA_RATE_RUNNING;
    state.item_rate_num = numerator / divisor;
    state.item_rate_den = first_half_cycles / divisor;
    return state;
}

static void gnw_h7b0_adc_dma_state_changed(GnwH7B0AdcState *s)
{
    if (s->dma) {
        gnw_h7b0_dma_request_state_changed(
            s->dma, GNW_H7B0_ADC_DMA_REQUEST_ADC2);
    }
}

void gnw_h7b0_adc_set_dma(GnwH7B0AdcState *s, GnwH7B0DmaState *dma)
{
    s->dma = dma;
    if (s->dma) {
        /*
         * low_latency=false (NOT true, unlike HASH_IN's registration):
         * HASH_IN is a genuine one-shot bulk transfer (CR.EN clears
         * after the first full-transfer tick, so a fixed ~0ns delay
         * fires at most twice total). ADC2's real firmware usage
         * (case_adc_vrefint_throughput.c) is CIRCULAR mode, which
         * restarts automatically forever until firmware explicitly
         * disables the stream -- see gnw_h7b0_dma_stream_tick()'s CIRC
         * handling. Combined with a fixed 1ns low_latency re-arm delay,
         * that produces a virtual timer that keeps re-firing
         * essentially continuously in real wall-clock time (each fire
         * has real host-side processing cost) until firmware's polling
         * loop finally gets enough real CPU time to observe TCIF and
         * call HAL_ADC_Stop_DMA() -- confirmed via a live repro to
         * inflate this case from an expected sub-millisecond completion
         * to ~28 real seconds (right at the harness's 30s timeout,
         * flaky pass/TIMEOUT depending on host load). Using the normal
         * rate-derived delay here instead (same mechanism SAI1's own
         * circular audio DMA already relies on) gives a bounded,
         * realistic half-transfer cadence instead of a runaway loop.
         */
        gnw_h7b0_dma_set_request_notifier(s->dma,
            GNW_H7B0_ADC_DMA_REQUEST_ADC2, gnw_h7b0_adc_dma_notify, s,
            gnw_h7b0_adc_dma_rate, s, false);
        gnw_h7b0_dma_set_request_state_fn(
            s->dma, GNW_H7B0_ADC_DMA_REQUEST_ADC2,
            gnw_h7b0_adc_request_state, s);
    }
}

static void gnw_h7b0_adc_reset(DeviceState *dev)
{
    GnwH7B0AdcState *s = GNW_H7B0_ADC(dev);

    for (unsigned i = 0; i < GNW_H7B0_ADC_INSTANCE_COUNT; i++) {
        timer_del(s->conversion_timer[i]);
        s->conversion_active[i] = false;
        s->sequence_rank[i] = 0;
        s->sequence_length[i] = 0;
        s->remaining_half_cycles[i] = 0;
        s->phase_remainder[i] = 0;
        s->active_kernel_hz[i] = 0;
        s->last_update_ns[i] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    }
    memset(s->regs, 0, sizeof(s->regs));
    qemu_set_irq(s->irq, 0);
    gnw_h7b0_adc_dma_state_changed(s);
}

static uint64_t gnw_h7b0_adc_read(void *opaque, hwaddr addr, unsigned int size)
{
    GnwH7B0AdcState *s = GNW_H7B0_ADC(opaque);

    if (addr >= GNW_H7B0_ADC_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return 0;
    }
    uint32_t value = s->regs[addr >> 2];
    if (s->timing_mode &&
        (addr == GNW_H7B0_ADC_DR ||
         addr == GNW_H7B0_ADC_INSTANCE_STRIDE + GNW_H7B0_ADC_DR)) {
        hwaddr base = addr - (addr % GNW_H7B0_ADC_INSTANCE_STRIDE);
        s->regs[(base + GNW_H7B0_ADC_ISR) >> 2] &= ~ADC_ISR_EOC;
        gnw_h7b0_adc_update_irq(s);
    }
    return value;
}

static void gnw_h7b0_adc_write(void *opaque, hwaddr addr,
                                uint64_t val64, unsigned int size)
{
    GnwH7B0AdcState *s = GNW_H7B0_ADC(opaque);
    uint32_t value = val64;
    hwaddr instance_base;

    if (addr >= GNW_H7B0_ADC_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return;
    }

    if (addr == GNW_H7B0_ADC_COMMON_CCR && s->timing_mode) {
        int64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        for (unsigned i = 0; i < GNW_H7B0_ADC_INSTANCE_COUNT; i++) {
            if (s->conversion_active[i]) {
                gnw_h7b0_adc_settle(s, i, now_ns);
                timer_del(s->conversion_timer[i]);
            }
        }
        s->regs[addr >> 2] = value;
        for (unsigned i = 0; i < GNW_H7B0_ADC_INSTANCE_COUNT; i++) {
            if (s->conversion_active[i]) {
                if (!s->remaining_half_cycles[i]) {
                    gnw_h7b0_adc_complete_rank(s, i);
                } else {
                    gnw_h7b0_adc_arm(s, i, now_ns);
                }
            }
        }
        gnw_h7b0_adc_dma_state_changed(s);
        return;
    }

    if (s->timing_mode &&
        (addr == GNW_H7B0_ADC_ISR ||
         addr == GNW_H7B0_ADC_INSTANCE_STRIDE + GNW_H7B0_ADC_ISR)) {
        /* EOC/EOS/ADRDY are write-one-to-clear status bits on H7B0. */
        uint32_t clearable = ADC_ISR_ADRDY | ADC_ISR_EOC | ADC_ISR_EOS_BIT;
        s->regs[addr >> 2] &= ~(value & clearable);
        gnw_h7b0_adc_update_irq(s);
        return;
    }

    if (s->timing_mode &&
        (addr == GNW_H7B0_ADC_IER ||
         addr == GNW_H7B0_ADC_INSTANCE_STRIDE + GNW_H7B0_ADC_IER)) {
        s->regs[addr >> 2] = value;
        gnw_h7b0_adc_update_irq(s);
        return;
    }

    instance_base = addr - (addr % GNW_H7B0_ADC_INSTANCE_STRIDE);
    if ((addr % GNW_H7B0_ADC_INSTANCE_STRIDE) == GNW_H7B0_ADC_CR &&
        (instance_base / GNW_H7B0_ADC_INSTANCE_STRIDE) <
        GNW_H7B0_ADC_INSTANCE_COUNT) {
        /*
         * Mirror ADEN into ADRDY instantly (not cycle-accurate -- real
         * hardware takes a stabilization delay). Good enough to unblock
         * the polling loop that always follows ADC enable.
         */
        s->regs[addr >> 2] = value & ~ADC_CR_ADCAL;
        if (value & ADC_CR_ADEN) {
            s->regs[(instance_base + GNW_H7B0_ADC_ISR) >> 2] |= ADC_ISR_ADRDY;
        } else {
            s->regs[(instance_base + GNW_H7B0_ADC_ISR) >> 2] &= ~ADC_ISR_ADRDY;
        }

        if (s->timing_mode) {
            unsigned instance = gnw_h7b0_adc_instance_index(instance_base);
            if ((value & ADC_CR_ADSTP_BIT) || !(value & ADC_CR_ADEN) ||
                (!(value & ADC_CR_ADSTART) && s->conversion_active[instance])) {
                timer_del(s->conversion_timer[instance]);
                s->conversion_active[instance] = false;
                s->remaining_half_cycles[instance] = 0;
                s->phase_remainder[instance] = 0;
                s->regs[addr >> 2] &= ~(ADC_CR_ADSTART | ADC_CR_ADSTP_BIT);
                gnw_h7b0_adc_dma_state_changed(s);
                return;
            }
            if ((value & ADC_CR_ADSTART) &&
                !s->conversion_active[instance]) {
                if (!gnw_h7b0_adc_start_timing_conversion(s, instance_base)) {
                    /* Keep ADSTART asserted: unsupported settings must not
                     * turn into a guessed or instantaneous completion. */
                    return;
                }
            }
            gnw_h7b0_adc_dma_state_changed(s);
            return;
        }

        /*
         * A regular conversion (ADSTART) always "reads" a fixed full-battery
         * value into DR and completes instantly. See gnw_h7b0_adc.h for why
         * this specific value/path: gnw-chainloader's board_get_battery_raw()
         * busy-polls EOC right after this write, and retro-go-sd's
         * bq24072_poll() expects an EOC interrupt (via HAL_ADC_Start_IT) to
         * eventually call HAL_ADC_ConvCpltCallback().
         */
        if (value & ADC_CR_ADSTART) {
            uint32_t sqr1 = s->regs[(instance_base + GNW_H7B0_ADC_SQR1) >> 2];
            uint32_t sq1 = (sqr1 & ADC_SQR1_SQ1_MASK) >> ADC_SQR1_SQ1_SHIFT;
            uint32_t dr_value = (sq1 == ADC_CHANNEL_VREFINT)
                                     ? GNW_H7B0_ADC_VREFINT_RAW
                                     : (s->battery_input && sq1 == 4)
                                       ? s->battery_raw
                                       : GNW_H7B0_ADC_FULL_BATTERY_RAW;

            s->regs[(instance_base + GNW_H7B0_ADC_DR) >> 2] = dr_value;
            s->regs[(instance_base + GNW_H7B0_ADC_ISR) >> 2] |= ADC_ISR_EOC;
            s->regs[addr >> 2] &= ~ADC_CR_ADSTART;

            if (s->regs[(instance_base + GNW_H7B0_ADC_IER) >> 2] &
                ADC_IER_EOCIE) {
                qemu_irq_pulse(s->irq);
            }
        }
        return;
    }

    if (s->timing_mode &&
        (addr == GNW_H7B0_ADC_SMPR1 || addr == GNW_H7B0_ADC_SMPR2 ||
         addr == GNW_H7B0_ADC_SQR1 || addr == GNW_H7B0_ADC_SQR2 ||
         addr == GNW_H7B0_ADC_SQR3 || addr == GNW_H7B0_ADC_SQR4 ||
         addr == GNW_H7B0_ADC_CFGR ||
         addr == GNW_H7B0_ADC_CFGR2 ||
         addr == GNW_H7B0_ADC_INSTANCE_STRIDE + GNW_H7B0_ADC_SMPR1 ||
         addr == GNW_H7B0_ADC_INSTANCE_STRIDE + GNW_H7B0_ADC_SMPR2 ||
         addr == GNW_H7B0_ADC_INSTANCE_STRIDE + GNW_H7B0_ADC_SQR1 ||
         addr == GNW_H7B0_ADC_INSTANCE_STRIDE + GNW_H7B0_ADC_SQR2 ||
         addr == GNW_H7B0_ADC_INSTANCE_STRIDE + GNW_H7B0_ADC_SQR3 ||
         addr == GNW_H7B0_ADC_INSTANCE_STRIDE + GNW_H7B0_ADC_SQR4 ||
         addr == GNW_H7B0_ADC_INSTANCE_STRIDE + GNW_H7B0_ADC_CFGR ||
         addr == GNW_H7B0_ADC_INSTANCE_STRIDE + GNW_H7B0_ADC_CFGR2)) {
        s->regs[addr >> 2] = value;
        gnw_h7b0_adc_dma_state_changed(s);
        return;
    }

    qemu_log_mask(LOG_UNIMP,
                  "%s: offset 0x%"HWADDR_PRIx" is a plain read/write "
                  "shadow, no real conversion modeled (reads always 0)\n",
                  __func__, addr);
    s->regs[addr >> 2] = value;
}

static const MemoryRegionOps gnw_h7b0_adc_ops = {
    .read = gnw_h7b0_adc_read,
    .write = gnw_h7b0_adc_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void gnw_h7b0_adc_init(Object *obj)
{
    GnwH7B0AdcState *s = GNW_H7B0_ADC(obj);

    for (unsigned i = 0; i < GNW_H7B0_ADC_INSTANCE_COUNT; i++) {
        s->conversion_timer_ctx[i].adc = s;
        s->conversion_timer_ctx[i].instance = i;
        s->conversion_timer[i] = timer_new_ns(
            QEMU_CLOCK_VIRTUAL, gnw_h7b0_adc_conversion_tick,
            &s->conversion_timer_ctx[i]);
    }
    memory_region_init_io(&s->mmio, obj, &gnw_h7b0_adc_ops, s,
                           TYPE_GNW_H7B0_ADC, GNW_H7B0_ADC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static int gnw_h7b0_adc_pre_save(void *opaque)
{
    GnwH7B0AdcState *s = opaque;
    int64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    s->migrated_timing_mode = s->timing_mode;
    for (unsigned i = 0; i < GNW_H7B0_ADC_INSTANCE_COUNT; i++) {
        if (!s->conversion_active[i]) {
            continue;
        }
        gnw_h7b0_adc_settle(s, i, now_ns);
        if (!s->remaining_half_cycles[i]) {
            gnw_h7b0_adc_complete_rank(s, i);
        } else {
            gnw_h7b0_adc_arm(s, i, now_ns);
        }
    }
    return 0;
}

static int gnw_h7b0_adc_post_load(void *opaque, int version_id)
{
    GnwH7B0AdcState *s = opaque;
    int64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (version_id < 2) {
        /* Legacy v1 had only synchronous, instantaneous conversions. */
        if (s->timing_mode) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "gnw-h7b0-adc: cannot load legacy immediate-mode "
                          "migration state in timing mode\n");
            return -EINVAL;
        }
        memset(s->conversion_active, 0, sizeof(s->conversion_active));
        memset(s->remaining_half_cycles, 0,
               sizeof(s->remaining_half_cycles));
        memset(s->phase_remainder, 0, sizeof(s->phase_remainder));
        gnw_h7b0_adc_dma_state_changed(s);
        return 0;
    }
    if (s->migrated_timing_mode != s->timing_mode) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "gnw-h7b0-adc: timing-mode mismatch on migration\n");
        return -EINVAL;
    }

    for (unsigned i = 0; i < GNW_H7B0_ADC_INSTANCE_COUNT; i++) {
        if (!s->conversion_active[i]) {
            continue;
        }
        hwaddr base = (hwaddr)i * GNW_H7B0_ADC_INSTANCE_STRIDE;
        uint32_t configured_length =
            ((s->regs[(base + GNW_H7B0_ADC_SQR1) >> 2] &
              ADC_SQR1_L_MASK) >> ADC_SQR1_L_SHIFT) + 1;

        if (!s->timing_mode || !s->remaining_half_cycles[i] ||
            s->remaining_half_cycles[i] > ADC_MAX_CONVERSION_HALF_CYCLES ||
            s->phase_remainder[i] >= ADC_HALF_CYCLE_NS_NUMERATOR ||
            !s->sequence_length[i] || s->sequence_length[i] > 16 ||
            s->sequence_length[i] != configured_length ||
            s->sequence_rank[i] >= s->sequence_length[i] ||
            !(s->regs[(base + GNW_H7B0_ADC_CR) >> 2] & ADC_CR_ADSTART) ||
            !gnw_h7b0_adc_sequence_supported(s, base,
                                              s->sequence_length[i])) {
            return -EINVAL;
        }
        s->last_update_ns[i] = now_ns;
        /* A stopped RCC source is a valid paused conversion. arm() removes
         * its timer while retaining the residual phase; the RCC observer
         * will resume it if a source later becomes available. */
        gnw_h7b0_adc_arm(s, i, now_ns);
    }
    gnw_h7b0_adc_update_irq(s);
    gnw_h7b0_adc_dma_state_changed(s);
    return 0;
}

static const VMStateDescription vmstate_gnw_h7b0_adc = {
    .name = TYPE_GNW_H7B0_ADC,
    .version_id = 2,
    .minimum_version_id = 1,
    .pre_save = gnw_h7b0_adc_pre_save,
    .post_load = gnw_h7b0_adc_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, GnwH7B0AdcState, GNW_H7B0_ADC_SIZE / 4),
        VMSTATE_BOOL_ARRAY_V(conversion_active, GnwH7B0AdcState,
                             GNW_H7B0_ADC_INSTANCE_COUNT, 2),
        VMSTATE_UINT8_ARRAY_V(sequence_rank, GnwH7B0AdcState,
                              GNW_H7B0_ADC_INSTANCE_COUNT, 2),
        VMSTATE_UINT8_ARRAY_V(sequence_length, GnwH7B0AdcState,
                              GNW_H7B0_ADC_INSTANCE_COUNT, 2),
        VMSTATE_UINT32_ARRAY_V(remaining_half_cycles, GnwH7B0AdcState,
                               GNW_H7B0_ADC_INSTANCE_COUNT, 2),
        VMSTATE_UINT64_ARRAY_V(phase_remainder, GnwH7B0AdcState,
                               GNW_H7B0_ADC_INSTANCE_COUNT, 2),
        VMSTATE_BOOL_V(migrated_timing_mode, GnwH7B0AdcState, 2),
        VMSTATE_END_OF_LIST()
    }
};

static const Property gnw_h7b0_adc_properties[] = {
    DEFINE_PROP_BOOL("timing-mode", GnwH7B0AdcState, timing_mode, false),
};

static void gnw_h7b0_adc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_gnw_h7b0_adc;
    device_class_set_props(dc, gnw_h7b0_adc_properties);
    device_class_set_legacy_reset(dc, gnw_h7b0_adc_reset);
}

static const TypeInfo gnw_h7b0_adc_info = {
    .name          = TYPE_GNW_H7B0_ADC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GnwH7B0AdcState),
    .instance_init = gnw_h7b0_adc_init,
    .class_init    = gnw_h7b0_adc_class_init,
};

static void gnw_h7b0_adc_register_types(void)
{
    type_register_static(&gnw_h7b0_adc_info);
}

type_init(gnw_h7b0_adc_register_types)
