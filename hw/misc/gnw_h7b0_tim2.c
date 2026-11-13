/* Auto-generated stub for TIM2, extended with a real counter for CR1.CEN */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "migration/vmstate.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "hw/misc/gnw_h7b0_tim2.h"
#include "hw/misc/gnw_h7b0_regs_tim2.h"
#include "hw/misc/gnw_h7b0_timer_clock.h"
#include "hw/misc/gnw_env.h"

/*
 * TIM2-TIM14's real kernel clock, not a guess: game-and-watch-retro-go-sd's
 * main.c SystemClock_Config sets AHBCLKDivider=RCC_HCLK_DIV1 (HCLK=SYSCLK,
 * no division) and APB1CLKDivider=RCC_APB1_DIV2 (PCLK1=HCLK/2) -- but
 * STM32H7's timer kernel clock uses the "x2" rule whenever its APBx
 * prescaler isn't /1: timer_clk = 2 x PCLK1 when APB1 prescaler > 1. That
 * doubling exactly cancels the /2, so TIM2's real kernel clock equals HCLK,
 * which (AHB_DIV1) equals SYSCLK.
 *
 * The real, LIVE value now comes from gnw_h7b0_rcc_get_hclk_hz() (see
 * gnw_h7b0_tim2_period_ns()) once gnw_h7b0_tim2_set_rcc() has wired an RCC
 * pointer in -- RCC decodes it from actual PLL1/SYSCLK register state, so
 * it tracks retro-go's runtime CPU overclock feature (NORMAL~280MHz/
 * BOOST1~312MHz/BOOST2~340MHz, switchable at runtime), rather than being
 * frozen at whichever level happened to be sampled once.
 *
 * The 340512000 constant below is now only a defensive fallback for when
 * RCC isn't wired (e.g. a standalone unit test) or returns 0 -- it's the
 * BOOST2-level SYSCLK value previously reverse-engineered from a live
 * SystemCoreClock read (see gnw_h7b0.c's SYSCLK_FRQ comment), kept as the
 * least-wrong single guess since it matches the already-validated ARMv7M
 * clock constant. Before this live wiring, this file's own guess had
 * predated that finding and silently mismatched it (a flat 200MHz), and
 * even after matching it, remained correct only at that one OC level --
 * any firmware game-speed pacing derived from a TIM2-TIM14 instance (not
 * just audio-DMA-paced code) would run at the wrong real-time rate at
 * NORMAL/BOOST1 or mid-switch otherwise.
 */
#define GNW_H7B0_TIM2_APPROX_CLK_HZ 340512000ULL
/* Clamp so a firmware-chosen PSC/ARR can't freeze the emulator for a
 * real-world eternity (e.g. ARR=0xffffffff at a slow prescaler) or
 * spin the host CPU rescheduling a timer for an ~instant period. */
#define GNW_H7B0_TIM2_MIN_PERIOD_NS 1000ULL
#define GNW_H7B0_TIM2_MAX_PERIOD_NS (2 * NANOSECONDS_PER_SECOND)

static int gnw_h7b0_tim2_instance_index(hwaddr addr)
{
    hwaddr instance_base = addr - (addr % 0x400);
    unsigned int idx = instance_base / 0x400;

    return idx < GNW_H7B0_TIM2_BLOCK_INSTANCE_COUNT ? (int)idx : -1;
}

static QEMUTimer *gnw_h7b0_tim2_count_timer(GnwH7B0Tim2State *s, int idx)
{
    return idx < GNW_H7B0_TIM2_LEGACY_INSTANCE_COUNT ?
        s->count_timer[idx] : s->late_count_timer[
            idx - GNW_H7B0_TIM2_LEGACY_INSTANCE_COUNT];
}

static uint32_t gnw_h7b0_tim2_counter_mask(int idx)
{
    /* Only TIM2 and TIM5 have 32-bit counters (RM0455). */
    return idx == 0 || idx == 3 ? UINT32_MAX : 0xffffu;
}

static uint32_t gnw_h7b0_tim2_write_mask(int idx, uint32_t offset)
{
    if (idx < GNW_H7B0_TIM2_LEGACY_INSTANCE_COUNT) {
        uint32_t mask = get_tim2_write_mask(offset);

        if (offset == GNW_H7B0_TIM2_CNT_OFFSET ||
            offset == GNW_H7B0_TIM2_ARR_OFFSET) {
            mask &= gnw_h7b0_tim2_counter_mask(idx);
        }
        return mask;
    }

    /* TIM12/13/14 only expose the base timer controls modeled below. */
    switch (offset) {
    case GNW_H7B0_TIM2_CR1_OFFSET:
        return 0x000003ffu;
    case GNW_H7B0_TIM2_DIER_OFFSET:
    case GNW_H7B0_TIM2_SR_OFFSET:
    case GNW_H7B0_TIM2_EGR_OFFSET:
        return 0x1u;
    case GNW_H7B0_TIM2_CNT_OFFSET:
    case GNW_H7B0_TIM2_ARR_OFFSET:
    case GNW_H7B0_TIM2_PSC_OFFSET:
        return 0xffffu;
    default:
        return 0;
    }
}

static void gnw_h7b0_tim2_update_irq(GnwH7B0Tim2State *s, int idx)
{
    hwaddr base = (hwaddr)idx * 0x400;
    uint32_t dier = s->regs[(base + GNW_H7B0_TIM2_DIER_OFFSET) >> 2];
    uint32_t sr = s->regs[(base + GNW_H7B0_TIM2_SR_OFFSET) >> 2];

    qemu_set_irq(s->irq[idx], (dier & sr & 1u) != 0);
}

/* Kernel clock used by both update timing and the live CNT calculation. */
static uint32_t gnw_h7b0_tim2_kernel_hz(GnwH7B0Tim2State *s)
{
    if (s->rcc) {
        /* A wired zero is a stopped clock, not an unavailable clock model. */
        return gnw_h7b0_rcc_get_timer_ker_hz(s->rcc);
    }
    return GNW_H7B0_TIM2_APPROX_CLK_HZ;
}

static void gnw_h7b0_tim2_settle(GnwH7B0Tim2State *s, int idx,
                                  uint64_t now, uint32_t kernel_hz)
{
    hwaddr base = (hwaddr)idx * 0x400;
    uint32_t cr1 = s->regs[(base + GNW_H7B0_TIM2_CR1_OFFSET) >> 2];
    uint32_t psc = s->regs[(base + GNW_H7B0_TIM2_PSC_OFFSET) >> 2] &
                   GNW_H7B0_TIM2_PRESCALER_MASK;
    uint32_t arr = s->regs[(base + GNW_H7B0_TIM2_ARR_OFFSET) >> 2] &
                   gnw_h7b0_tim2_counter_mask(idx);
    uint64_t modulus = (uint64_t)arr + 1;
    uint64_t elapsed_ns, kernel_cycles, ticks, remainder;

    if (s->timing_mode && icount_enabled()) {
        IcountCycleStamp stamp;

        icount_get_cycle_stamp(&stamp);
        if (!gnw_h7b0_cycle_phase_settle(
                &s->cycle_phase[idx], stamp, (cr1 & 1u) ? kernel_hz : 0,
                &ticks)) {
            g_assert_not_reached();
        }
        if (cr1 & 1u) {
            uint64_t prescale = (uint64_t)psc + 1;
            uint64_t total = ticks + s->psc_remainder[idx];

            s->psc_remainder[idx] = total % prescale;
            s->cnt_base[idx] = ((uint64_t)s->cnt_base[idx] +
                (total / prescale) % modulus) % modulus;
        }
        s->regs[(base + GNW_H7B0_TIM2_CNT_OFFSET) >> 2] =
            s->cnt_base[idx];
        s->cnt_ref_ns[idx] = now;
        return;
    }

    if (!(cr1 & 0x1u) || now <= s->cnt_ref_ns[idx]) {
        return;
    }
    elapsed_ns = now - s->cnt_ref_ns[idx];
    kernel_cycles = gnw_h7b0_timer_elapsed_kernel_cycles(
        elapsed_ns, kernel_hz, &s->kernel_fraction[idx]);
    ticks = kernel_cycles / ((uint64_t)psc + 1);
    remainder = kernel_cycles % ((uint64_t)psc + 1);
    remainder += s->psc_remainder[idx];
    if (remainder >= (uint64_t)psc + 1) {
        remainder -= (uint64_t)psc + 1;
        ticks++;
    }
    s->psc_remainder[idx] = remainder;
    s->cnt_base[idx] = ((uint64_t)s->cnt_base[idx] + ticks % modulus) %
                       modulus;
    s->regs[(base + GNW_H7B0_TIM2_CNT_OFFSET) >> 2] = s->cnt_base[idx];
    s->cnt_ref_ns[idx] = now;
}

static void gnw_h7b0_tim2_latch_cnt(GnwH7B0Tim2State *s, int idx)
{
    gnw_h7b0_tim2_settle(s, idx,
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
        gnw_h7b0_tim2_kernel_hz(s));
}

static uint64_t gnw_h7b0_tim2_remaining_ns_exact(GnwH7B0Tim2State *s,
                                                  int idx,
                                                  uint32_t kernel_hz)
{
    hwaddr base = (hwaddr)idx * 0x400;
    uint32_t psc = s->regs[(base + GNW_H7B0_TIM2_PSC_OFFSET) >> 2] &
                   GNW_H7B0_TIM2_PRESCALER_MASK;
    uint32_t arr = s->regs[(base + GNW_H7B0_TIM2_ARR_OFFSET) >> 2] &
                   gnw_h7b0_tim2_counter_mask(idx);
    uint64_t modulus = (uint64_t)arr + 1;
    uint64_t remaining_ticks = modulus - (s->cnt_base[idx] % modulus);
    uint64_t kernel_cycles = remaining_ticks * ((uint64_t)psc + 1) -
                             s->psc_remainder[idx];
    if (s->timing_mode && icount_enabled()) {
        uint64_t interval = gnw_h7b0_timer_kernel_q64_to_ns_ceil(
            kernel_cycles, s->cycle_phase[idx].fraction_q64, kernel_hz);

        return interval ? interval : 1;
    }
    return gnw_h7b0_timer_kernel_cycles_to_ns_ceil(
        kernel_cycles, s->kernel_fraction[idx], kernel_hz);
}

static uint64_t gnw_h7b0_tim2_remaining_ns(GnwH7B0Tim2State *s, int idx,
                                           uint32_t kernel_hz)
{
    uint64_t interval = gnw_h7b0_tim2_remaining_ns_exact(s, idx, kernel_hz);

    return gnw_h7b0_timer_apply_period_limits(
        interval, s->timing_mode, GNW_H7B0_TIM2_MIN_PERIOD_NS,
        GNW_H7B0_TIM2_MAX_PERIOD_NS);
}

static void gnw_h7b0_tim2_schedule(GnwH7B0Tim2State *s, int idx,
                                    uint32_t kernel_hz)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (!kernel_hz) {
        timer_del(gnw_h7b0_tim2_count_timer(s, idx));
        return;
    }
    timer_mod(gnw_h7b0_tim2_count_timer(s, idx), gnw_h7b0_timer_deadline_sat(
                  now, gnw_h7b0_tim2_remaining_ns(s, idx, kernel_hz)));
}

static void gnw_h7b0_tim2_start_counting(GnwH7B0Tim2State *s, int idx)
{
    s->cnt_ref_ns[idx] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (s->timing_mode && icount_enabled()) {
        IcountCycleStamp stamp;

        icount_get_cycle_stamp(&stamp);
        g_assert(gnw_h7b0_cycle_phase_init(&s->cycle_phase[idx], stamp));
    }
    gnw_h7b0_tim2_schedule(s, idx, gnw_h7b0_tim2_kernel_hz(s));
}

static bool gnw_h7b0_tim2_timing_prepare(
    void *opaque, const IcountCycleStamp *before, uint32_t old_hz)
{
    GnwH7B0Tim2State *s = opaque;

    for (int idx = 0; idx < GNW_H7B0_TIM2_BLOCK_INSTANCE_COUNT; idx++) {
        hwaddr base = (hwaddr)idx * 0x400;
        bool counting = s->regs[(base + GNW_H7B0_TIM2_CR1_OFFSET) >> 2] & 1u;
        uint64_t ticks;

        s->pending_cycle_phase[idx] = s->cycle_phase[idx];
        s->pending_cnt_base[idx] = s->cnt_base[idx];
        s->pending_psc_remainder[idx] = s->psc_remainder[idx];
        if (!gnw_h7b0_cycle_phase_settle(&s->pending_cycle_phase[idx],
                                          *before, counting ? old_hz : 0,
                                          &ticks)) {
            s->pending_phase_valid = false;
            return false;
        }
        if (counting) {
            uint32_t psc = s->regs[(base + GNW_H7B0_TIM2_PSC_OFFSET) >> 2] &
                           GNW_H7B0_TIM2_PRESCALER_MASK;
            uint32_t arr = s->regs[(base + GNW_H7B0_TIM2_ARR_OFFSET) >> 2] &
                           gnw_h7b0_tim2_counter_mask(idx);
            uint64_t prescale = (uint64_t)psc + 1;
            uint64_t modulus = (uint64_t)arr + 1;
            uint64_t total = ticks + s->pending_psc_remainder[idx];

            s->pending_psc_remainder[idx] = total % prescale;
            s->pending_cnt_base[idx] =
                (s->pending_cnt_base[idx] + (total / prescale) % modulus) %
                modulus;
        }
    }
    s->pending_phase_valid = true;
    return true;
}

static void gnw_h7b0_tim2_timing_commit(
    void *opaque, const IcountCycleStamp *before,
    const IcountCycleStamp *after, uint32_t new_hz)
{
    GnwH7B0Tim2State *s = opaque;

    g_assert(s->pending_phase_valid);
    for (int idx = 0; idx < GNW_H7B0_TIM2_BLOCK_INSTANCE_COUNT; idx++) {
        hwaddr base = (hwaddr)idx * 0x400;

        g_assert(gnw_h7b0_cycle_phase_rebase(&s->pending_cycle_phase[idx],
                                              *before, *after));
        s->cycle_phase[idx] = s->pending_cycle_phase[idx];
        s->cnt_base[idx] = s->pending_cnt_base[idx];
        s->psc_remainder[idx] = s->pending_psc_remainder[idx];
        s->regs[(base + GNW_H7B0_TIM2_CNT_OFFSET) >> 2] = s->cnt_base[idx];
        if (s->regs[(base + GNW_H7B0_TIM2_CR1_OFFSET) >> 2] & 1u) {
            gnw_h7b0_tim2_schedule(s, idx, new_hz);
        }
    }
    s->pending_phase_valid = false;
}

static void gnw_h7b0_tim2_timing_abort(void *opaque)
{
    GnwH7B0Tim2State *s = opaque;

    s->pending_phase_valid = false;
}

/* Env-gated (GNW_TIMER_LATE): per-instance update-expiry counter,
 * reported once per virtual second. The 2026-07-22 perf bug was exactly
 * an expiry flood here; this makes any recurrence measurable at a
 * glance instead of needing a host profiler. */
static void gnw_h7b0_tim2_count_expiry(int idx)
{
    static int enabled = -1;
    static int64_t window_start;
    static uint32_t counts[GNW_H7B0_TIM2_BLOCK_INSTANCE_COUNT];

    if (enabled < 0) {
        enabled = gnw_env_enabled("GNW_TIMER_LATE");
    }
    if (!enabled) {
        return;
    }
    counts[idx]++;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (now - window_start >= NANOSECONDS_PER_SECOND) {
        if (window_start) {
            fprintf(stderr, "TIMEXP");
            for (int i = 0; i < GNW_H7B0_TIM2_BLOCK_INSTANCE_COUNT; i++) {
                if (counts[i]) {
                    fprintf(stderr, " tim[%d]=%u/s", i, counts[i]);
                }
            }
            fprintf(stderr, "\n");
        }
        memset(counts, 0, sizeof(counts));
        window_start = now;
    }
}

static void gnw_h7b0_tim2_timer_tick(void *opaque)
{
    GnwH7B0Tim2TimerCtx *ctx = opaque;
    GnwH7B0Tim2State *s = ctx->s;
    int idx = ctx->idx;

    gnw_h7b0_tim2_count_expiry(idx);
    hwaddr base = (hwaddr)idx * 0x400;
    uint32_t cr1 = s->regs[(base + GNW_H7B0_TIM2_CR1_OFFSET) >> 2];
    uint32_t kernel_hz = gnw_h7b0_tim2_kernel_hz(s);
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    bool elapsed_update = !s->timing_mode && now > s->cnt_ref_ns[idx] &&
        now - s->cnt_ref_ns[idx] >=
            gnw_h7b0_tim2_remaining_ns_exact(s, idx, kernel_hz);

    /* A capped default-mode deadline is a scheduling checkpoint when the
     * actual timer period is longer than the cap. Settle elapsed cycles and
     * reschedule without manufacturing UIF or an update interrupt. */
    if (!s->timing_mode && !elapsed_update &&
        gnw_h7b0_tim2_remaining_ns_exact(s, idx, kernel_hz) >
            GNW_H7B0_TIM2_MAX_PERIOD_NS) {
        gnw_h7b0_tim2_settle(s, idx,
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), kernel_hz);
        gnw_h7b0_tim2_schedule(s, idx, kernel_hz);
        return;
    }

    if (s->timing_mode || elapsed_update) {
        gnw_h7b0_tim2_settle(s, idx,
            now,
            kernel_hz);
    } else {
        s->cnt_base[idx] = 0;
        s->regs[(base + GNW_H7B0_TIM2_CNT_OFFSET) >> 2] = 0;
        s->kernel_fraction[idx] = 0;
        s->psc_remainder[idx] = 0;
        s->cnt_ref_ns[idx] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    }
    s->regs[(base + GNW_H7B0_TIM2_SR_OFFSET) >> 2] |= 0x1u; /* UIF */
    gnw_h7b0_tim2_update_irq(s, idx);
    if ((s->regs[(base + GNW_H7B0_TIM2_DIER_OFFSET) >> 2] & (1u << 8)) &&
        s->update_notifier) {
        s->update_notifier(s->update_notifier_opaque, idx);
    }

    if (cr1 & (1u << 3)) {
        /* OPM (one-pulse mode): counter stops itself after the update
         * event, same as real hardware. */
        if (elapsed_update) {
            s->cnt_base[idx] = 0;
            s->regs[(base + GNW_H7B0_TIM2_CNT_OFFSET) >> 2] = 0;
            s->kernel_fraction[idx] = 0;
            s->psc_remainder[idx] = 0;
        }
        s->regs[(base + GNW_H7B0_TIM2_CR1_OFFSET) >> 2] = cr1 & ~0x1u;
        return;
    }

    if (cr1 & 0x1u) {
        gnw_h7b0_tim2_schedule(s, idx, kernel_hz);
    }
}

void gnw_h7b0_tim2_set_update_notifier(GnwH7B0Tim2State *s,
                                        GnwH7B0Tim2UpdateNotifier notifier,
                                        void *opaque)
{
    s->update_notifier = notifier;
    s->update_notifier_opaque = opaque;
}

static void gnw_h7b0_tim2_clock_changed(void *opaque, uint32_t old_hz,
                                         uint32_t new_hz)
{
    GnwH7B0Tim2State *s = opaque;
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    for (int idx = 0; idx < GNW_H7B0_TIM2_BLOCK_INSTANCE_COUNT; idx++) {
        hwaddr base = (hwaddr)idx * 0x400;

        if (!(s->regs[(base + GNW_H7B0_TIM2_CR1_OFFSET) >> 2] & 0x1u)) {
            continue;
        }
        gnw_h7b0_tim2_settle(s, idx, now, old_hz);
        gnw_h7b0_tim2_schedule(s, idx, new_hz);
    }
}

void gnw_h7b0_tim2_set_rcc(GnwH7B0Tim2State *s, GnwH7B0RccState *rcc)
{
    s->rcc = rcc;
    gnw_h7b0_rcc_add_timer_clock_observer(
        rcc, GNW_H7B0_RCC_TIMER_APB1, gnw_h7b0_tim2_clock_changed, s);
    if (s->timing_mode && icount_enabled()) {
        gnw_h7b0_rcc_add_timing_observer(
            rcc, GNW_H7B0_RCC_TIMING_APB1,
            gnw_h7b0_tim2_timing_prepare,
            gnw_h7b0_tim2_timing_commit,
            gnw_h7b0_tim2_timing_abort, s);
    }
}

static void gnw_h7b0_tim2_reset(DeviceState *dev)
{
    GnwH7B0Tim2State *s = GNW_H7B0_TIM2(dev);
    for (int i = 0; i < (GNW_H7B0_TIM2_SIZE / 4); i++) {
        s->regs[i] = get_tim2_reset_value((i * 4) & 0x3ffu);
    }
    for (int i = 0; i < GNW_H7B0_TIM2_BLOCK_INSTANCE_COUNT; i++) {
        timer_del(gnw_h7b0_tim2_count_timer(s, i));
        s->cnt_base[i] = 0;
        s->cnt_ref_ns[i] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        s->kernel_fraction[i] = 0;
        s->psc_remainder[i] = 0;
        if (s->timing_mode && icount_enabled()) {
            IcountCycleStamp stamp;

            icount_get_cycle_stamp(&stamp);
            if (stamp.core_hz) {
                g_assert(gnw_h7b0_cycle_phase_init(&s->cycle_phase[i],
                                                    stamp));
            }
        }
        gnw_h7b0_tim2_update_irq(s, i);
    }
}

static uint64_t gnw_h7b0_tim2_read(void *opaque, hwaddr addr, unsigned int size)
{
    GnwH7B0Tim2State *s = GNW_H7B0_TIM2(opaque);
    if (addr >= GNW_H7B0_TIM2_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n", __func__, addr);
        return 0;
    }

    /*
     * CNT must be derived from elapsed time, not read back as a shadow.
     * Firmware uses a free-running instance (ARR=0xffffffff) as its
     * microsecond time base and polls this register hundreds of times a
     * second; a shadow answers "no time has passed" every time. See
     * gnw_h7b0_tim2.h's cnt_base/cnt_ref_ns comment for the measured
     * audio symptom this caused.
     */
    if ((addr & 0x3ffu) == GNW_H7B0_TIM2_CNT_OFFSET) {
        int idx = gnw_h7b0_tim2_instance_index(addr);
        if (idx >= 0) {
            uint32_t v;

            gnw_h7b0_tim2_latch_cnt(s, idx);
            v = s->cnt_base[idx] & gnw_h7b0_tim2_counter_mask(idx);
            /* Cached: firmware polls CNT at busy-wait rates, and
             * msvcrt's getenv() is a locked linear scan -- calling it
             * per-read measurably slowed the whole guest on Windows. */
            static int trace_env = -1;
            if (trace_env < 0) {
                trace_env = gnw_env_enabled("GNW_AUDIO_TRACE");
            }
            if (trace_env) {
                fprintf(stderr, "TR %d %" PRId64 " %u\n", idx,
                        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), v);
            }
            return v;
        }
    }

    return s->regs[addr >> 2];
}

static void gnw_h7b0_tim2_write(void *opaque, hwaddr addr, uint64_t val64, unsigned int size)
{
    GnwH7B0Tim2State *s = GNW_H7B0_TIM2(opaque);
    if (addr >= GNW_H7B0_TIM2_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n", __func__, addr);
        return;
    }
    /*
     * get_tim2_write_mask()/get_tim2_reset_value() (auto-generated from
     * a single TIM2 instance's own register layout) only recognize
     * offsets within the first 0x400 window -- BUG FIX (2026-07-16):
     * using the raw block-wide `addr` here meant every instance past
     * TIM2 itself (TIM3-TIM7, offsets 0x400+) hit those functions'
     * default case (mask 0) for literally every register, silently
     * turning every write to TIM3-TIM7 (CR1, PSC, ARR, SR, ...) into a
     * no-op -- found via stm32h7b0-diag's timer_tim6_update case: with
     * CR1.CEN never actually taking effect, gnw_h7b0_tim2_start_counting()
     * never ran, and (more subtly) firmware's own `TIM6->SR = 0` clear
     * -- meant to clear the UIF that the EGR.UG side effect below had
     * just set -- was *also* a no-op, so the busy-wait loop saw UIF
     * already set and returned almost instantly. Fold to the
     * per-instance local offset before consulting either table, same as
     * the EGR/CR1 side-effect logic below already correctly does.
     */
    uint32_t local_addr = addr & 0x3ffu;
    uint32_t old_value = s->regs[addr >> 2];
    int cnt_idx = gnw_h7b0_tim2_instance_index(addr);
    uint32_t mask = cnt_idx >= 0 ?
        gnw_h7b0_tim2_write_mask(cnt_idx, local_addr) :
        get_tim2_write_mask(local_addr);

    /*
     * Anything that changes how CNT extrapolates (its own value, the
     * prescaler, or the wrap point) has to settle the elapsed time so far
     * against the OLD parameters first -- hence latching before the store
     * below, not after.
     */
    if (cnt_idx >= 0 &&
        (local_addr == GNW_H7B0_TIM2_CNT_OFFSET ||
         local_addr == GNW_H7B0_TIM2_PSC_OFFSET ||
         local_addr == GNW_H7B0_TIM2_ARR_OFFSET ||
         local_addr == GNW_H7B0_TIM2_CR1_OFFSET ||
         (local_addr == GNW_H7B0_TIM2_EGR_OFFSET && (val64 & 1u))) &&
        (s->regs[(addr - local_addr + GNW_H7B0_TIM2_CR1_OFFSET) >> 2] &
         0x1u)) {
        gnw_h7b0_tim2_latch_cnt(s, cnt_idx);
    }

    if (local_addr == GNW_H7B0_TIM2_SR_OFFSET) {
        /* STM32 timer status flags are rc_w0: writing 0 clears a set
         * flag, while writing 1 leaves it unchanged. */
        s->regs[addr >> 2] = old_value & ((uint32_t)val64 | ~mask);
    } else {
        s->regs[addr >> 2] = (old_value & ~mask) | ((uint32_t)val64 & mask);
    }

    /* A write to CNT sets the counter outright. */
    if (cnt_idx >= 0 && local_addr == GNW_H7B0_TIM2_CNT_OFFSET) {
        s->cnt_base[cnt_idx] = s->regs[addr >> 2];
        s->cnt_ref_ns[cnt_idx] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    } else if (cnt_idx >= 0 &&
               local_addr == GNW_H7B0_TIM2_PSC_OFFSET) {
        s->psc_remainder[cnt_idx] = 0;
    } else if (cnt_idx >= 0 &&
               local_addr == GNW_H7B0_TIM2_ARR_OFFSET) {
        uint64_t modulus = (uint64_t)s->regs[addr >> 2] + 1;

        s->cnt_base[cnt_idx] %= modulus;
        s->regs[(addr - local_addr + GNW_H7B0_TIM2_CNT_OFFSET) >> 2] =
            s->cnt_base[cnt_idx];
    }

    /*
     * This device models the whole TIM2-TIM14 block (see soc.h's
     * TIM2_BLOCK_BASE_ADDRESS/SIZE) as one contiguous region, but is
     * otherwise a plain shadow with no side effects -- fine for most
     * firmware, but a real hardware-timer busy-wait idiom (write EGR.UG=1
     * to force an update event, then spin on SR.UIF until hardware sets
     * it) never terminates here, since nothing ever sets UIF. Found via a
     * silent black-screen hang tracing a patched stock-firmware image
     * with no debug symbols (disassembly-only: r0=0x40000C00 == TIM5,
     * spinning on SR bit0 after an EGR.UG write). TIM2/3/4/5/6/7 share a
     * uniform 0x400-per-instance layout with EGR at local offset 0x14 and
     * SR at local offset 0x10 (general-purpose/basic timer register map);
     * apply the EGR.UG -> SR.UIF side effect generically at that stride so
     * it covers every timer in the block, not just whichever one this was
     * found on. (TIM12/13/14 don't sit on that same 0x400 grid within
     * this block; not covered by this generic rule -- fix those the same
     * way if a similar hang is found on one of them.)
     */
    if ((addr & 0x3ffu) == GNW_H7B0_TIM2_EGR_OFFSET && (val64 & 0x1u)) {
        uint32_t sr_addr = (addr & ~0x3ffu) + GNW_H7B0_TIM2_SR_OFFSET;
        s->regs[sr_addr >> 2] |= 0x1u; /* UIF */
        if (cnt_idx >= 0) {
            gnw_h7b0_tim2_update_irq(s, cnt_idx);
        }
        /* UG re-initializes the counter (and reloads the prescaler) on
         * real hardware, so restart the extrapolation from zero. */
        if (cnt_idx >= 0) {
            s->cnt_base[cnt_idx] = 0;
            s->cnt_ref_ns[cnt_idx] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            s->psc_remainder[cnt_idx] = 0;
            s->regs[(addr - local_addr + GNW_H7B0_TIM2_CNT_OFFSET) >> 2] = 0;
            if (s->timing_mode && icount_enabled()) {
                IcountCycleStamp stamp;

                icount_get_cycle_stamp(&stamp);
                g_assert(gnw_h7b0_cycle_phase_init(
                    &s->cycle_phase[cnt_idx], stamp));
            }
            if (s->regs[(addr - local_addr +
                         GNW_H7B0_TIM2_CR1_OFFSET) >> 2] & 0x1u) {
                gnw_h7b0_tim2_schedule(
                    s, cnt_idx, gnw_h7b0_tim2_kernel_hz(s));
            }
        }
    }

    /*
     * CR1.CEN (bit 0): real hardware starts counting from CNT toward
     * ARR (at PSC's rate) and sets SR.UIF on overflow, repeating
     * unless one-pulse mode (CR1.OPM, bit 3) is set. Previously a
     * plain shadow -- any firmware busy-waiting on SR.UIF after
     * enabling CEN (rather than forcing an EGR.UG update, the only
     * path with a side effect) would hang forever. Approximated via
     * gnw_h7b0_tim2_start_counting()'s QEMUTimer instead of instant
     * completion, so timing-dependent pacing (e.g. a real per-frame
     * delay) gets a plausible real duration rather than either
     * hanging or running unbounded-fast.
     */
    if ((addr & 0x3ffu) == GNW_H7B0_TIM2_CR1_OFFSET) {
        int idx = gnw_h7b0_tim2_instance_index(addr);
        uint32_t new_cr1 = s->regs[addr >> 2];

        if (idx >= 0) {
            if ((new_cr1 & 0x1u) && !(old_value & 0x1u)) {
                /* Counting resumes from the frozen value, as of now. */
                s->cnt_ref_ns[idx] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
                gnw_h7b0_tim2_start_counting(s, idx);
            } else if (!(new_cr1 & 0x1u)) {
                timer_del(gnw_h7b0_tim2_count_timer(s, idx));
            }
        }
    } else if (cnt_idx >= 0 &&
               (local_addr == GNW_H7B0_TIM2_CNT_OFFSET ||
                local_addr == GNW_H7B0_TIM2_PSC_OFFSET ||
                local_addr == GNW_H7B0_TIM2_ARR_OFFSET) &&
               (s->regs[(addr - local_addr +
                         GNW_H7B0_TIM2_CR1_OFFSET) >> 2] & 0x1u)) {
        gnw_h7b0_tim2_schedule(s, cnt_idx, gnw_h7b0_tim2_kernel_hz(s));
    }

    if (cnt_idx >= 0 &&
        (local_addr == GNW_H7B0_TIM2_DIER_OFFSET ||
         local_addr == GNW_H7B0_TIM2_SR_OFFSET)) {
        gnw_h7b0_tim2_update_irq(s, cnt_idx);
    }
}

static const MemoryRegionOps gnw_h7b0_tim2_ops = {
    .read = gnw_h7b0_tim2_read,
    .write = gnw_h7b0_tim2_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void gnw_h7b0_tim2_init(Object *obj)
{
    GnwH7B0Tim2State *s = GNW_H7B0_TIM2(obj);
    memory_region_init_io(&s->mmio, obj, &gnw_h7b0_tim2_ops, s, TYPE_GNW_H7B0_TIM2, GNW_H7B0_TIM2_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);

    for (int i = 0; i < GNW_H7B0_TIM2_BLOCK_INSTANCE_COUNT; i++) {
        s->timer_ctx[i].s = s;
        s->timer_ctx[i].idx = i;
        QEMUTimer **timer = i < GNW_H7B0_TIM2_LEGACY_INSTANCE_COUNT ?
            &s->count_timer[i] : &s->late_count_timer[
                i - GNW_H7B0_TIM2_LEGACY_INSTANCE_COUNT];

        *timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                              gnw_h7b0_tim2_timer_tick,
                              &s->timer_ctx[i]);
        sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq[i]);
    }
}

static const Property gnw_h7b0_tim2_properties[] = {
    DEFINE_PROP_BOOL("timing-mode", GnwH7B0Tim2State, timing_mode, false),
};

static int gnw_h7b0_tim2_pre_save(void *opaque)
{
    GnwH7B0Tim2State *s = opaque;
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint32_t kernel_hz = gnw_h7b0_tim2_kernel_hz(s);

    for (int idx = 0; idx < GNW_H7B0_TIM2_BLOCK_INSTANCE_COUNT; idx++) {
        gnw_h7b0_tim2_settle(s, idx, now, kernel_hz);
    }
    if (s->pending_phase_valid) {
        return -EINVAL;
    }
    return 0;
}

static int gnw_h7b0_tim2_post_load(void *opaque, int version_id)
{
    GnwH7B0Tim2State *s = opaque;
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    for (int idx = 0; idx < GNW_H7B0_TIM2_BLOCK_INSTANCE_COUNT; idx++) {
        hwaddr base = (hwaddr)idx * 0x400;
        uint32_t psc = s->regs[(base + GNW_H7B0_TIM2_PSC_OFFSET) >> 2] &
                       GNW_H7B0_TIM2_PRESCALER_MASK;

        if (idx >= GNW_H7B0_TIM2_LEGACY_INSTANCE_COUNT && version_id < 5) {
            /* v2-v4 did not carry TIM12/13/14 runtime state. Start from the
             * migrated CNT shadow at the destination clock epoch. */
            s->cnt_base[idx] = s->regs[(base + GNW_H7B0_TIM2_CNT_OFFSET) >> 2] &
                               gnw_h7b0_tim2_counter_mask(idx);
            s->kernel_fraction[idx] = 0;
            s->psc_remainder[idx] = 0;
            s->cnt_ref_ns[idx] = now;
            if (s->timing_mode && icount_enabled()) {
                IcountCycleStamp stamp;

                icount_get_cycle_stamp(&stamp);
                if (!gnw_h7b0_cycle_phase_init(&s->cycle_phase[idx], stamp)) {
                    return -EINVAL;
                }
            }
            if (s->regs[(base + GNW_H7B0_TIM2_CR1_OFFSET) >> 2] & 1u) {
                gnw_h7b0_tim2_schedule(s, idx, gnw_h7b0_tim2_kernel_hz(s));
            }
        } else if (version_id < 3) {
            s->kernel_fraction[idx] = 0;
            s->psc_remainder[idx] = 0;
            s->cnt_ref_ns[idx] = now;
            if (s->regs[(base + GNW_H7B0_TIM2_CR1_OFFSET) >> 2] & 0x1u) {
                gnw_h7b0_tim2_schedule(
                    s, idx, gnw_h7b0_tim2_kernel_hz(s));
            }
        } else if (s->kernel_fraction[idx] >= NANOSECONDS_PER_SECOND ||
                   s->psc_remainder[idx] > psc) {
            return -EINVAL;
        } else {
            s->cnt_ref_ns[idx] = now;
        }
        if (s->timing_mode && icount_enabled()) {
            IcountCycleStamp stamp;

            icount_get_cycle_stamp(&stamp);
            if (version_id >= 4) {
                if (!gnw_h7b0_cycle_phase_stamp_equal(
                        &s->cycle_phase[idx].stamp, &stamp)) {
                    return -EINVAL;
                }
            } else if (!gnw_h7b0_cycle_phase_init(
                           &s->cycle_phase[idx], stamp)) {
                return -EINVAL;
            }
        }
        if (s->timing_mode &&
            (s->regs[(base + GNW_H7B0_TIM2_CR1_OFFSET) >> 2] & 1u)) {
            gnw_h7b0_tim2_schedule(s, idx, gnw_h7b0_tim2_kernel_hz(s));
        }
        gnw_h7b0_tim2_update_irq(s, idx);
    }
    return 0;
}

static const VMStateDescription vmstate_gnw_h7b0_cycle_phase = {
    .name = "gnw-h7b0-cycle-phase",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_INT64(stamp.modeled_cycles, GnwH7B0CyclePhase),
        VMSTATE_INT64(stamp.idle_ns, GnwH7B0CyclePhase),
        VMSTATE_UINT64(stamp.core_hz, GnwH7B0CyclePhase),
        VMSTATE_UINT64(stamp.generation, GnwH7B0CyclePhase),
        VMSTATE_UINT64(fraction_q64, GnwH7B0CyclePhase),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_gnw_h7b0_tim2 = {
    .name = TYPE_GNW_H7B0_TIM2,
    .version_id = 5,
    .minimum_version_id = 2,
    .pre_save = gnw_h7b0_tim2_pre_save,
    .post_load = gnw_h7b0_tim2_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, GnwH7B0Tim2State, GNW_H7B0_TIM2_SIZE / 4),
        /* CNT is derived, so its reference point has to travel with the
         * snapshot or the counter jumps on restore. */
        VMSTATE_SUB_ARRAY(cnt_ref_ns, GnwH7B0Tim2State, 0,
                          GNW_H7B0_TIM2_LEGACY_INSTANCE_COUNT, 0,
                          vmstate_info_uint64, uint64_t),
        VMSTATE_SUB_ARRAY(cnt_base, GnwH7B0Tim2State, 0,
                          GNW_H7B0_TIM2_LEGACY_INSTANCE_COUNT, 0,
                          vmstate_info_uint32, uint32_t),
        VMSTATE_ARRAY_OF_POINTER(count_timer, GnwH7B0Tim2State,
                                 GNW_H7B0_TIM2_LEGACY_INSTANCE_COUNT, 3,
                                 vmstate_info_timer, QEMUTimer *),
        VMSTATE_SUB_ARRAY(kernel_fraction, GnwH7B0Tim2State, 0,
                          GNW_H7B0_TIM2_LEGACY_INSTANCE_COUNT, 3,
                          vmstate_info_uint64, uint64_t),
        VMSTATE_SUB_ARRAY(psc_remainder, GnwH7B0Tim2State, 0,
                          GNW_H7B0_TIM2_LEGACY_INSTANCE_COUNT, 3,
                          vmstate_info_uint32, uint32_t),
        VMSTATE_STRUCT_SUB_ARRAY(cycle_phase, GnwH7B0Tim2State, 0,
                                 GNW_H7B0_TIM2_LEGACY_INSTANCE_COUNT, 4,
                                 vmstate_gnw_h7b0_cycle_phase,
                                 GnwH7B0CyclePhase),
        VMSTATE_SUB_ARRAY(cnt_ref_ns, GnwH7B0Tim2State,
                          GNW_H7B0_TIM2_LEGACY_INSTANCE_COUNT,
                          GNW_H7B0_TIM2_LATE_INSTANCE_COUNT, 5,
                          vmstate_info_uint64, uint64_t),
        VMSTATE_SUB_ARRAY(cnt_base, GnwH7B0Tim2State,
                          GNW_H7B0_TIM2_LEGACY_INSTANCE_COUNT,
                          GNW_H7B0_TIM2_LATE_INSTANCE_COUNT, 5,
                          vmstate_info_uint32, uint32_t),
        VMSTATE_ARRAY_OF_POINTER(late_count_timer, GnwH7B0Tim2State,
                                 GNW_H7B0_TIM2_LATE_INSTANCE_COUNT, 5,
                                 vmstate_info_timer, QEMUTimer *),
        VMSTATE_SUB_ARRAY(kernel_fraction, GnwH7B0Tim2State,
                          GNW_H7B0_TIM2_LEGACY_INSTANCE_COUNT,
                          GNW_H7B0_TIM2_LATE_INSTANCE_COUNT, 5,
                          vmstate_info_uint64, uint64_t),
        VMSTATE_SUB_ARRAY(psc_remainder, GnwH7B0Tim2State,
                          GNW_H7B0_TIM2_LEGACY_INSTANCE_COUNT,
                          GNW_H7B0_TIM2_LATE_INSTANCE_COUNT, 5,
                          vmstate_info_uint32, uint32_t),
        VMSTATE_STRUCT_SUB_ARRAY(cycle_phase, GnwH7B0Tim2State,
                                 GNW_H7B0_TIM2_LEGACY_INSTANCE_COUNT,
                                 GNW_H7B0_TIM2_LATE_INSTANCE_COUNT, 5,
                                 vmstate_gnw_h7b0_cycle_phase,
                                 GnwH7B0CyclePhase),
        VMSTATE_END_OF_LIST()
    }
};

static void gnw_h7b0_tim2_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_props(dc, gnw_h7b0_tim2_properties);
    dc->vmsd = &vmstate_gnw_h7b0_tim2;
    device_class_set_legacy_reset(dc, gnw_h7b0_tim2_reset);
}

static const TypeInfo gnw_h7b0_tim2_info = {
    .name          = TYPE_GNW_H7B0_TIM2,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GnwH7B0Tim2State),
    .instance_init = gnw_h7b0_tim2_init,
    .class_init    = gnw_h7b0_tim2_class_init,
};

static void gnw_h7b0_tim2_register_types(void)
{
    type_register_static(&gnw_h7b0_tim2_info);
}
type_init(gnw_h7b0_tim2_register_types)
