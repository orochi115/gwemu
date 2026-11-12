/*
 * STM32H7B0 RTC minimal stub (Nintendo Game & Watch)
 *
 * See gnw_h7b0_rtc.h for scope/rationale.
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
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "system/system.h"
#include "qapi/error.h"
#include "migration/vmstate.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/gnw_h7b0_rtc.h"
#include "hw/misc/gnw_h7b0_regs_rtc.h"

static inline uint32_t rtc_bcd(unsigned int n)
{
    return ((n / 10) << 4) | (n % 10);
}

static inline unsigned int rtc_unbcd(uint32_t v)
{
    return ((v >> 4) & 0xf) * 10 + (v & 0xf);
}

static void gnw_h7b0_rtc_schedule_alarm_a(GnwH7B0RtcState *s);
static void gnw_h7b0_rtc_schedule_alarm_b(GnwH7B0RtcState *s);
static void gnw_h7b0_rtc_schedule_wakeup(GnwH7B0RtcState *s);

static bool gnw_h7b0_rtc_in_init(GnwH7B0RtcState *s)
{
    return s->use_rtc_clock && !s->sync_host &&
           (s->regs[GNW_H7B0_RTC_ICSR >> 2] & RTC_ICSR_INIT);
}

/* Recompute SSR/TR/DR from the running calendar (reset epoch plus
 * elapsed virtual-clock time since the last read/write) as of
 * virtual-clock time at_ns, and store the encoded fields back into
 * regs, so gnw_h7b0_rtc_read() just returns whatever's already there.
 *
 * at_ns is passed in rather than sampled here so the shadow-register
 * lock (see gnw_h7b0_rtc_read()) can re-derive all three registers
 * from one single instant. */
static void gnw_h7b0_rtc_sync_calendar(GnwH7B0RtcState *s, int64_t at_ns)
{
    int64_t elapsed_ns = at_ns - s->rtc_base_vclock_ns;
    int64_t now = s->rtc_base_epoch + elapsed_ns / NANOSECONDS_PER_SECOND;
    int64_t frac_ns = elapsed_ns % NANOSECONDS_PER_SECOND;
    time_t now_t = now;
    struct tm tm;
    uint32_t tr, dr;
    unsigned int rtc_wday;
    uint32_t prediv_s;

    if (frac_ns < 0) {
        /* Only reachable if the calendar was re-anchored into the
         * future; keep the sub-second field in range regardless. */
        frac_ns += NANOSECONDS_PER_SECOND;
        now--;
        now_t = now;
    }

    /*
     * SSR is the synchronous prescaler's *down*-counter: it reloads to
     * PREDIV_S at each calendar-second boundary and counts down to 0
     * over the following second, which is why the HAL computes the
     * fraction as (PREDIV_S - SSR) / (PREDIV_S + 1) rather than
     * SSR / PREDIV_S.
     *
     * This was a permanently-zero read-only shadow until 2026-07-30
     * (SSR's generated reset value is 0 and nothing ever wrote it), and
     * that is *not* an inert stub: with the reset PREDIV_S of 255 it
     * makes the HAL's fraction a constant 255/256, so every
     * GW_GetCurrentMillis()/_gettimeofday() caller sees tv_usec pinned
     * at 996000 and any sub-second delta comes out zero. Reported from
     * a retro-go session where gettimeofday()-based timing worked on
     * real hardware and silently measured nothing here.
     *
     * Resolution is 1/(PREDIV_S+1) s -- 3.9ms at the reset prescaler,
     * same ceiling as real silicon. This is not a sub-frame timer on
     * either; that's what TIM2 is for.
     */
    prediv_s = s->regs[GNW_H7B0_RTC_PRER_OFFSET >> 2] & 0x7fff;
    s->regs[GNW_H7B0_RTC_SSR_OFFSET >> 2] =
        prediv_s - (uint32_t)((frac_ns * (prediv_s + 1))
                              / NANOSECONDS_PER_SECOND);

    if (s->rtc_calendar_utc) {
        gmtime_r(&now_t, &tm);
    } else {
        localtime_r(&now_t, &tm);
    }

    tr = (rtc_bcd(tm.tm_sec) & 0x7f)
       | ((rtc_bcd(tm.tm_min) & 0x7f) << 8)
       | ((rtc_bcd(tm.tm_hour) & 0x3f) << 16);

    rtc_wday = tm.tm_wday == 0 ? 7 : (unsigned int)tm.tm_wday;
    unsigned int year = tm.tm_year % 100;
    dr = (rtc_bcd(tm.tm_mday) & 0x3f)
       | ((rtc_bcd(tm.tm_mon + 1) & 0x1f) << 8)
       | ((rtc_wday & 0x7) << 13)
       | ((rtc_bcd(year) & 0xff) << 16);

    s->regs[GNW_H7B0_RTC_TR_OFFSET >> 2] = tr;
    s->regs[GNW_H7B0_RTC_DR_OFFSET >> 2] = dr;
}

/* Inverse of gnw_h7b0_rtc_sync_calendar(): re-derive the epoch base
 * from whatever's currently in TR/DR, so a firmware write to either
 * (setting the clock) re-anchors the running calendar instead of
 * being instantly overwritten by the next read's sync. */
static void gnw_h7b0_rtc_reanchor_calendar(GnwH7B0RtcState *s)
{
    uint32_t tr = s->regs[GNW_H7B0_RTC_TR_OFFSET >> 2];
    uint32_t dr = s->regs[GNW_H7B0_RTC_DR_OFFSET >> 2];
    struct tm tm = { 0 };

    tm.tm_isdst = -1;
    tm.tm_sec = rtc_unbcd(tr & 0x7f);
    tm.tm_min = rtc_unbcd((tr >> 8) & 0x7f);
    tm.tm_hour = rtc_unbcd((tr >> 16) & 0x3f);
    tm.tm_mday = rtc_unbcd(dr & 0x3f);
    tm.tm_mon = rtc_unbcd((dr >> 8) & 0x1f) - 1;
    tm.tm_year = rtc_unbcd((dr >> 16) & 0xff) + 100;

    s->rtc_base_epoch = s->rtc_calendar_utc ? mktimegm(&tm) : mktime(&tm);
    s->rtc_base_vclock_ns = qemu_clock_get_ns(s->clock);
    /* A clock-set invalidates any in-progress shadow snapshot. */
    s->shadow_locked = false;
    gnw_h7b0_rtc_schedule_alarm_a(s);
    gnw_h7b0_rtc_schedule_alarm_b(s);
}

static bool gnw_h7b0_rtc_alarm_matches(GnwH7B0RtcState *s, uint32_t alarm,
                                       const struct tm *tm)
{
    unsigned int wday = tm->tm_wday == 0 ? 7 : tm->tm_wday;
    unsigned int hour = rtc_unbcd((alarm >> 16) & 0x3f);

    if (!(alarm & (1U << 7)) && rtc_unbcd(alarm & 0x7f) != tm->tm_sec) {
        return false;
    }
    if (!(alarm & (1U << 15)) &&
        rtc_unbcd((alarm >> 8) & 0x7f) != tm->tm_min) {
        return false;
    }
    if (!(alarm & (1U << 23))) {
        /* In 12-hour mode, 12 AM is hour zero and PM adds twelve. */
        if (s->regs[GNW_H7B0_RTC_CR >> 2] & (1U << 6)) {
            hour %= 12;
            if (alarm & (1U << 22)) {
                hour += 12;
            }
        }
        if (hour != tm->tm_hour) {
            return false;
        }
    }
    if (!(alarm & (1U << 31))) {
        unsigned int day = rtc_unbcd((alarm >> 24) & 0x3f);

        if (day != ((alarm & (1U << 30)) ? wday : tm->tm_mday)) {
            return false;
        }
    }
    return true;
}

static int64_t gnw_h7b0_rtc_alarm_subsecond_ns(GnwH7B0RtcState *s,
                                                uint32_t alarm_ss,
                                                int64_t second_ns,
                                                int64_t now_ns)
{
    uint32_t prediv_s = s->regs[GNW_H7B0_RTC_PRER_OFFSET >> 2] & 0x7fff;
    uint32_t compared_bits = (alarm_ss >> 24) & 0xf;
    uint32_t target = alarm_ss & 0x7fff;
    uint32_t compare_mask;
    int ss;

    /* MASKSS=0 masks every subsecond bit, retaining the historical
     * whole-second alarm behavior.  Values 1..15 compare SS[n-1:0]. */
    if (compared_bits == 0) {
        return second_ns > now_ns ? second_ns : -1;
    }
    compare_mask = (1U << compared_bits) - 1;

    /* SSR counts down from PREDIV_S.  Return the first matching counter
     * value strictly after now.  Round its tick boundary upward so an alarm
     * can never become visible before the modeled prescaler reaches it. */
    for (ss = prediv_s; ss >= 0; ss--) {
        int64_t ticks = prediv_s - ss;
        int64_t offset_ns =
            (ticks * NANOSECONDS_PER_SECOND + prediv_s) / (prediv_s + 1);
        int64_t deadline_ns = second_ns + offset_ns;

        if (((ss ^ target) & compare_mask) == 0 && deadline_ns > now_ns) {
            return deadline_ns;
        }
    }
    return -1;
}

static int64_t gnw_h7b0_rtc_next_alarm_ns(GnwH7B0RtcState *s, uint32_t alarm,
                                           uint32_t alarm_ss, int64_t now_ns)
{
    int64_t now_epoch = s->rtc_base_epoch +
        (now_ns - s->rtc_base_vclock_ns) / NANOSECONDS_PER_SECOND;
    uint32_t compared_bits = (alarm_ss >> 24) & 0xf;
    time_t candidate = now_epoch + (compared_bits == 0);
    unsigned int target_second = rtc_unbcd(alarm & 0x7f);
    int step = 1;
    /* A date-of-month match can be 61 days away (31st after February). */
    const time_t limit = candidate + 62 * 86400;

    if (!(alarm & (1U << 7))) {
        candidate += (target_second + 60 - candidate % 60) % 60;
        step = 60;
    }

    for (; candidate <= limit; candidate += step) {
        struct tm tm;

        if (s->rtc_calendar_utc) {
            gmtime_r(&candidate, &tm);
        } else {
            localtime_r(&candidate, &tm);
        }
        if (gnw_h7b0_rtc_alarm_matches(s, alarm, &tm)) {
            int64_t second_ns = s->rtc_base_vclock_ns +
                (candidate - s->rtc_base_epoch) * NANOSECONDS_PER_SECOND;
            int64_t deadline_ns = gnw_h7b0_rtc_alarm_subsecond_ns(
                s, alarm_ss, second_ns, now_ns);

            if (deadline_ns >= 0) {
                return deadline_ns;
            }
        }
    }
    return -1;
}

static void gnw_h7b0_rtc_update_irq(GnwH7B0RtcState *s);

static void gnw_h7b0_rtc_alarm_a_tick(void *opaque)
{
    GnwH7B0RtcState *s = opaque;

    s->regs[GNW_H7B0_RTC_SR >> 2] |= RTC_SR_ALRAF;
    gnw_h7b0_rtc_update_irq(s);
    gnw_h7b0_rtc_schedule_alarm_a(s);
}

static void gnw_h7b0_rtc_schedule_alarm_a(GnwH7B0RtcState *s)
{
    int64_t deadline;

    timer_del(s->alarm_a_timer);
    s->alarm_a_deadline_ns = -1;
    s->alarm_a_remaining_ns = -1;
    if (gnw_h7b0_rtc_in_init(s) ||
        !(s->regs[GNW_H7B0_RTC_CR >> 2] & RTC_CR_ALRAE)) {
        return;
    }
    deadline = gnw_h7b0_rtc_next_alarm_ns(
        s, s->regs[GNW_H7B0_RTC_ALRMAR_OFFSET >> 2],
        s->regs[GNW_H7B0_RTC_ALRMASSR_OFFSET >> 2],
        qemu_clock_get_ns(s->clock));
    if (deadline >= 0) {
        s->alarm_a_deadline_ns = deadline;
        timer_mod_ns(s->alarm_a_timer, deadline);
    }
}

static void gnw_h7b0_rtc_alarm_b_tick(void *opaque)
{
    GnwH7B0RtcState *s = opaque;

    s->regs[GNW_H7B0_RTC_SR >> 2] |= RTC_SR_ALRBF;
    gnw_h7b0_rtc_update_irq(s);
    gnw_h7b0_rtc_schedule_alarm_b(s);
}

static void gnw_h7b0_rtc_schedule_alarm_b(GnwH7B0RtcState *s)
{
    int64_t deadline;

    timer_del(s->alarm_b_timer);
    s->alarm_b_deadline_ns = -1;
    s->alarm_b_remaining_ns = -1;
    if (gnw_h7b0_rtc_in_init(s) ||
        !(s->regs[GNW_H7B0_RTC_CR >> 2] & RTC_CR_ALRBE)) {
        return;
    }
    deadline = gnw_h7b0_rtc_next_alarm_ns(
        s, s->regs[GNW_H7B0_RTC_ALRMBR_OFFSET >> 2],
        s->regs[GNW_H7B0_RTC_ALRMBSSR_OFFSET >> 2],
        qemu_clock_get_ns(s->clock));
    if (deadline >= 0) {
        s->alarm_b_deadline_ns = deadline;
        timer_mod_ns(s->alarm_b_timer, deadline);
    }
}

static int64_t gnw_h7b0_rtc_wakeup_period_ns(GnwH7B0RtcState *s)
{
    uint32_t cr = s->regs[GNW_H7B0_RTC_CR >> 2];
    uint64_t count = (s->regs[GNW_H7B0_RTC_WUTR_OFFSET >> 2] & 0xffff) + 1;
    uint32_t selector = cr & 7;

    if (selector <= 3) {
        uint32_t prer = s->regs[GNW_H7B0_RTC_PRER_OFFSET >> 2];
        uint64_t rtclk_hz = ((uint64_t)((prer >> 16) & 0x7f) + 1) *
                            ((prer & 0x7fff) + 1);
        uint64_t divisor = 16 >> selector;

        return muldiv64(count * divisor, NANOSECONDS_PER_SECOND, rtclk_hz);
    }
    if (selector == 4) {
        return count * NANOSECONDS_PER_SECOND;
    }
    if (selector == 6) {
        return (count + 0x10000) * NANOSECONDS_PER_SECOND;
    }
    return -1;
}

static void gnw_h7b0_rtc_wakeup_tick(void *opaque)
{
    GnwH7B0RtcState *s = opaque;

    s->regs[GNW_H7B0_RTC_SR >> 2] |= RTC_SR_WUTF;
    gnw_h7b0_rtc_update_irq(s);
    gnw_h7b0_rtc_schedule_wakeup(s);
}

static void gnw_h7b0_rtc_schedule_wakeup(GnwH7B0RtcState *s)
{
    int64_t period;

    timer_del(s->wakeup_timer);
    s->wakeup_deadline_ns = -1;
    s->wakeup_remaining_ns = -1;
    if (!(s->regs[GNW_H7B0_RTC_CR >> 2] & RTC_CR_WUTE)) {
        return;
    }
    period = gnw_h7b0_rtc_wakeup_period_ns(s);
    if (period > 0) {
        s->wakeup_deadline_ns =
            qemu_clock_get_ns(s->clock) + period;
        timer_mod_ns(s->wakeup_timer, s->wakeup_deadline_ns);
    }
}

static void gnw_h7b0_rtc_reset(DeviceState *dev)
{
    GnwH7B0RtcState *s = GNW_H7B0_RTC(dev);

    for (int i = 0; i < (GNW_H7B0_RTC_SIZE / 4); i++) {
        s->regs[i] = get_rtc_reset_value(i * 4);
    }
    /* Calendar reads as already-initialized/synchronized from reset,
     * same rationale as RCC_RSR's non-zero reset value: a plain-zero
     * reset reads as "never initialized", which is not what real
     * firmware expects to see on any boot after the very first.
     *
     * WUTWF/ALRBWF/ALRAWF also need to be set here, not just on an
     * explicit ICSR write (see the ICSR case below): CR is disabled
     * (WUTE/ALRAE/ALRBE all 0) at reset, and real hardware reports the
     * corresponding write-allowed flag as 1 whenever the timer/alarm is
     * disabled. A stock-firmware boot was found hanging in
     * HAL_RTCEx_DeactivateWakeUpTimer(), which clears CR.WUTE then
     * polls ICSR.WUTWF directly -- without ever writing ICSR first -- so
     * leaving these bits zeroed at reset made that wait spin until its
     * timeout, and the caller retried the whole RTC bring-up forever. */
    s->regs[GNW_H7B0_RTC_ICSR >> 2] = RTC_ICSR_RSF | RTC_ICSR_INITS
                                       | RTC_ICSR_WUTWF | RTC_ICSR_ALRBWF
                                       | RTC_ICSR_ALRAWF;

    /* Upstream defaults reseed a virtual-time calendar at every reset.
     * Desktop can select rtc_clock and retain the anchor on warm reset. */
    if (!s->preserve_calendar || !s->calendar_initialized) {
        s->rtc_calendar_utc = s->initial_epoch >= 0;
        s->rtc_base_epoch = s->rtc_calendar_utc ? s->initial_epoch : time(NULL);
        s->rtc_base_vclock_ns = qemu_clock_get_ns(s->clock);
        s->calendar_initialized = true;
    }
    s->shadow_locked = false;
    timer_del(s->alarm_a_timer);
    timer_del(s->alarm_b_timer);
    timer_del(s->wakeup_timer);
    s->alarm_a_deadline_ns = -1;
    s->alarm_a_remaining_ns = -1;
    s->alarm_b_deadline_ns = -1;
    s->alarm_b_remaining_ns = -1;
    s->wakeup_deadline_ns = -1;
    s->wakeup_remaining_ns = -1;
    gnw_h7b0_rtc_sync_calendar(s, qemu_clock_get_ns(s->clock));
    qemu_irq_lower(s->irq);
    qemu_irq_lower(s->wakeup_irq);
}

void gnw_h7b0_rtc_backup_domain_reset(GnwH7B0RtcState *s)
{
    /* Host-clock timers can fire while the CPU is paused: cancel now. */
    s->calendar_initialized = false;
    gnw_h7b0_rtc_reset(DEVICE(s));
}

/*
 * Real RTC_Alarm_IRQn covers Alarm A and B only (not the wakeup timer,
 * a separate IRQ line not modeled here -- see .h file comment). MISR
 * already holds SR gated by the *IE enable bits (computed in the CR/SCR
 * write handlers below), so this just ORs the two Alarm bits onto the
 * one output line. Previously nothing ever called qemu_set_irq() at
 * all for this device -- MISR/SR tracked correctly, but no firmware
 * interrupt-driven wait (as opposed to a polling loop) could ever
 * observe it, since the NVIC never saw the line assert (confirmed via
 * case_clock_rtc_alarm.c: HAL_RTC_AlarmIRQHandler() never ran, isr
 * count stayed 0 for the full timeout, despite ALRAF/MISR being set
 * correctly the whole time).
 */
static void gnw_h7b0_rtc_update_irq(GnwH7B0RtcState *s)
{
    uint32_t cr = s->regs[GNW_H7B0_RTC_CR >> 2];
    uint32_t sr = s->regs[GNW_H7B0_RTC_SR >> 2];
    uint32_t misr =
        (sr & RTC_SR_ALRAF & ((cr & RTC_CR_ALRAIE) ? ~0u : 0)) |
        (sr & RTC_SR_ALRBF & ((cr & RTC_CR_ALRBIE) ? ~0u : 0)) |
        (sr & RTC_SR_WUTF & ((cr & RTC_CR_WUTIE) ? ~0u : 0));

    s->regs[GNW_H7B0_RTC_MISR >> 2] = misr;
    qemu_set_irq(s->irq, (misr & (RTC_SR_ALRAF | RTC_SR_ALRBF)) != 0);
    qemu_set_irq(s->wakeup_irq, (misr & RTC_SR_WUTF) != 0);
}

static uint64_t gnw_h7b0_rtc_read(void *opaque, hwaddr addr, unsigned int size)
{
    GnwH7B0RtcState *s = GNW_H7B0_RTC(opaque);

    if (addr >= GNW_H7B0_RTC_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return 0;
    }
    /*
     * Shadow-register lock. On real hardware the calendar registers are
     * a coherent snapshot: reading SSR or TR freezes SSR/TR/DR until DR
     * is read, which is exactly why HAL_RTC_GetTime() reads SSR -> TR ->
     * DR in that order. Without the lock, each read here re-derives
     * independently from the virtual clock, so a second boundary falling
     * between the SSR and TR reads yields an inconsistent triple (a
     * sub-second field from one second paired with a seconds field from
     * the next -- i.e. an occasional one-second jump backwards or
     * forwards in whatever the caller computes). Latch the instant on
     * the first of SSR/TR and release on DR, mirroring the hardware
     * guarantee the HAL is written against.
     */
    if (gnw_h7b0_rtc_in_init(s) &&
        (addr == GNW_H7B0_RTC_SSR_OFFSET ||
         addr == GNW_H7B0_RTC_TR_OFFSET || addr == GNW_H7B0_RTC_DR_OFFSET)) {
        return s->regs[addr >> 2];
    }
    switch (addr) {
    case GNW_H7B0_RTC_SSR_OFFSET:
    case GNW_H7B0_RTC_TR_OFFSET:
        if (!s->shadow_locked) {
            s->shadow_locked = true;
            s->shadow_vclock_ns = qemu_clock_get_ns(s->clock);
        }
        gnw_h7b0_rtc_sync_calendar(s, s->shadow_vclock_ns);
        break;
    case GNW_H7B0_RTC_DR_OFFSET:
        gnw_h7b0_rtc_sync_calendar(s, s->shadow_locked
                                       ? s->shadow_vclock_ns
                                       : qemu_clock_get_ns(s->clock));
        s->shadow_locked = false;
        break;
    default:
        break;
    }
    return s->regs[addr >> 2];
}

static void gnw_h7b0_rtc_write(void *opaque, hwaddr addr,
                                uint64_t val64, unsigned int size)
{
    GnwH7B0RtcState *s = GNW_H7B0_RTC(opaque);
    uint32_t value = val64;


    if (addr >= GNW_H7B0_RTC_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return;
    }

    uint32_t mask = get_rtc_write_mask(addr);
    value = (s->regs[addr >> 2] & ~mask) | (value & mask);

    switch (addr) {
    case GNW_H7B0_RTC_ICSR: {
        bool was_init = gnw_h7b0_rtc_in_init(s);
        bool new_init = s->use_rtc_clock && !s->sync_host &&
                        (value & RTC_ICSR_INIT);

        if (!was_init && new_init) {
            gnw_h7b0_rtc_sync_calendar(s, qemu_clock_get_ns(s->clock));
            s->shadow_locked = false;
        }
        /*
         * HAL_RTC_WaitForSynchro() writes ICSR to clear RSF before
         * polling it back set; RTC_EnterInitMode() sets INIT and
         * polls INITF. Real hardware sets INITF a few RTCCLK cycles
         * after INIT is requested, and RSF once shadow registers
         * resync -- approximated here as instant, same pattern as
         * RCC/PWR/OSPI/ADC's ON->RDY bits. INITS (calendar has been
         * initialized at least once) stays set once seen.
         *
         * WUTWF/ALRBWF/ALRAWF (bits 2/1/0) are also part of ICSR's reset
         * value (0x7, all set) but were getting silently zeroed by this
         * handler on the very first ICSR write, since only INIT/INITF/
         * RSF/INITS were ever preserved. HAL_RTC_SetAlarm_IT() polls
         * ALRAWF before reprogramming ALRMAR; once zeroed, that wait
         * never ends. Found via a silent hang in a patched stock-firmware
         * image (no debug symbols -- disassembly-only, an endless WPR/
         * CR/SCR unlock-wait-retry alarm-set sequence with LTDC never
         * getting configured). These write-allowed flags are true
         * whenever the corresponding alarm/wakeup-timer is disabled;
         * approximated as always true, same "instant ready" simplification
         * as the rest of this register.
         */
        s->regs[addr >> 2] = (value & RTC_ICSR_INIT)
                              | RTC_ICSR_INITF | RTC_ICSR_RSF | RTC_ICSR_INITS
                              | RTC_ICSR_WUTWF | RTC_ICSR_ALRBWF
                              | RTC_ICSR_ALRAWF;
        if (s->use_rtc_clock && !s->sync_host) {
            if (!new_init) {
                s->regs[addr >> 2] &= ~RTC_ICSR_INITF;
            }
            if (was_init && !new_init) {
                gnw_h7b0_rtc_reanchor_calendar(s);
            } else if (!was_init && new_init) {
                gnw_h7b0_rtc_schedule_alarm_a(s);
                gnw_h7b0_rtc_schedule_alarm_b(s);
            }
        }
        return;
    }
    case GNW_H7B0_RTC_WPR_OFFSET:
        /* WPR is write-only; real hardware/SVD always read it back as 0. */
        s->regs[addr >> 2] = 0;
        return;
    case GNW_H7B0_RTC_CR:
    {
        uint32_t old_cr = s->regs[addr >> 2];
        uint32_t sr = s->regs[GNW_H7B0_RTC_SR >> 2];

        s->regs[addr >> 2] = value;

        s->regs[GNW_H7B0_RTC_SR >> 2] = sr;

        /* Keep ICSR's write-allowed flags in sync with CR here too, not
         * just on an explicit ICSR write -- HAL_RTCEx_DeactivateWakeUpTimer()
         * clears WUTE then polls WUTWF straight from CR without an
         * intervening ICSR write (see reset's comment above). */
        {
            uint32_t icsr = s->regs[GNW_H7B0_RTC_ICSR >> 2];
            icsr = (value & RTC_CR_WUTE) ? (icsr & ~RTC_ICSR_WUTWF)
                                          : (icsr | RTC_ICSR_WUTWF);
            icsr = (value & RTC_CR_ALRAE) ? (icsr & ~RTC_ICSR_ALRAWF)
                                           : (icsr | RTC_ICSR_ALRAWF);
            icsr = (value & RTC_CR_ALRBE) ? (icsr & ~RTC_ICSR_ALRBWF)
                                           : (icsr | RTC_ICSR_ALRBWF);
            s->regs[GNW_H7B0_RTC_ICSR >> 2] = icsr;
        }
        if ((old_cr ^ value) & RTC_CR_ALRAE) {
            gnw_h7b0_rtc_schedule_alarm_a(s);
        }
        if ((old_cr ^ value) & RTC_CR_ALRBE) {
            gnw_h7b0_rtc_schedule_alarm_b(s);
        }
        if ((old_cr ^ value) & (RTC_CR_WUTE | 7)) {
            gnw_h7b0_rtc_schedule_wakeup(s);
        }
        gnw_h7b0_rtc_update_irq(s);
        return;
    }
    case GNW_H7B0_RTC_SCR:
        s->regs[GNW_H7B0_RTC_SR >> 2] &= ~value;
        s->regs[GNW_H7B0_RTC_MISR >> 2] &= ~value;
        gnw_h7b0_rtc_update_irq(s);
        return;
    case GNW_H7B0_RTC_TR_OFFSET:
    case GNW_H7B0_RTC_DR_OFFSET:
        if (s->sync_host) {
            /*
             * Ignore writes to TR and DR to prevent stock firmware from
             * resetting the clock to 12:00:00 on every cold boot. The calendar
             * keeps advancing from its reset epoch on virtual time; it does
             * not continuously mirror host wall time.
             */
            return;
        }
        s->regs[addr >> 2] = value;
        if (!gnw_h7b0_rtc_in_init(s)) {
            gnw_h7b0_rtc_reanchor_calendar(s);
        }
        return;
    case GNW_H7B0_RTC_ALRMAR_OFFSET:
        s->regs[addr >> 2] = value;
        gnw_h7b0_rtc_schedule_alarm_a(s);
        return;
    case GNW_H7B0_RTC_ALRMASSR_OFFSET:
        s->regs[addr >> 2] = value;
        gnw_h7b0_rtc_schedule_alarm_a(s);
        return;
    case GNW_H7B0_RTC_ALRMBR_OFFSET:
        s->regs[addr >> 2] = value;
        gnw_h7b0_rtc_schedule_alarm_b(s);
        return;
    case GNW_H7B0_RTC_ALRMBSSR_OFFSET:
        s->regs[addr >> 2] = value;
        gnw_h7b0_rtc_schedule_alarm_b(s);
        return;
    case GNW_H7B0_RTC_WUTR_OFFSET:
        s->regs[addr >> 2] = value;
        gnw_h7b0_rtc_schedule_wakeup(s);
        return;
    case GNW_H7B0_RTC_PRER_OFFSET:
        s->regs[addr >> 2] = value;
        gnw_h7b0_rtc_schedule_alarm_a(s);
        gnw_h7b0_rtc_schedule_alarm_b(s);
        gnw_h7b0_rtc_schedule_wakeup(s);
        return;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: offset 0x%"HWADDR_PRIx" is a plain read/write "
                      "shadow, no real calendar/clock modeled\n",
                      __func__, addr);
        s->regs[addr >> 2] = value;
    }
}

static const MemoryRegionOps gnw_h7b0_rtc_ops = {
    .read = gnw_h7b0_rtc_read,
    .write = gnw_h7b0_rtc_write,
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

static GnwH7B0RtcState *g_gnw_rtc = NULL;

void gnw_h7b0_rtc_set_sync_host(bool sync_host)
{
    if (g_gnw_rtc) {
        g_gnw_rtc->sync_host = sync_host;
    }
}

static void gnw_h7b0_rtc_init(Object *obj)
{
    GnwH7B0RtcState *s = GNW_H7B0_RTC(obj);
    g_gnw_rtc = s;

    memory_region_init_io(&s->mmio, obj, &gnw_h7b0_rtc_ops, s,
                           TYPE_GNW_H7B0_RTC, GNW_H7B0_RTC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->wakeup_irq);
}

static void gnw_h7b0_rtc_realize(DeviceState *dev, Error **errp)
{
    GnwH7B0RtcState *s = GNW_H7B0_RTC(dev);

    if (s->initial_epoch < -1) {
        error_setg(errp, "initial-epoch must be -1 (host-local default) or "
                   "a nonnegative Unix epoch");
        return;
    }
    s->clock = s->use_rtc_clock ? rtc_clock : QEMU_CLOCK_VIRTUAL;
    s->alarm_a_timer = timer_new_ns(s->clock,
                                    gnw_h7b0_rtc_alarm_a_tick, s);
    s->alarm_b_timer = timer_new_ns(s->clock,
                                    gnw_h7b0_rtc_alarm_b_tick, s);
    s->wakeup_timer = timer_new_ns(s->clock,
                                   gnw_h7b0_rtc_wakeup_tick, s);
}

static int gnw_h7b0_rtc_post_load(void *opaque, int version_id)
{
    GnwH7B0RtcState *s = opaque;
    int64_t now_ns = qemu_clock_get_ns(s->clock);

    if (version_id < 6) {
        s->calendar_initialized = true;
    }
    if (version_id < 3) {
        /* Older snapshots only stored the host-local calendar convention. */
        s->rtc_calendar_utc = false;
    }
    timer_del(s->alarm_a_timer);
    timer_del(s->alarm_b_timer);
    timer_del(s->wakeup_timer);
    if (!gnw_h7b0_rtc_in_init(s) &&
        (s->regs[GNW_H7B0_RTC_CR >> 2] & RTC_CR_ALRAE)) {
        if (version_id >= 4 && s->alarm_a_remaining_ns > 0) {
            s->alarm_a_deadline_ns = now_ns + s->alarm_a_remaining_ns;
            timer_mod_ns(s->alarm_a_timer, s->alarm_a_deadline_ns);
        } else if (version_id >= 4 && s->alarm_a_remaining_ns == 0) {
            s->regs[GNW_H7B0_RTC_SR >> 2] |= RTC_SR_ALRAF;
            gnw_h7b0_rtc_schedule_alarm_a(s);
        } else {
            gnw_h7b0_rtc_schedule_alarm_a(s);
        }
    } else {
        s->alarm_a_deadline_ns = -1;
    }
    if (!gnw_h7b0_rtc_in_init(s) &&
        (s->regs[GNW_H7B0_RTC_CR >> 2] & RTC_CR_ALRBE)) {
        if (version_id >= 5 && s->alarm_b_remaining_ns > 0) {
            s->alarm_b_deadline_ns = now_ns + s->alarm_b_remaining_ns;
            timer_mod_ns(s->alarm_b_timer, s->alarm_b_deadline_ns);
        } else if (version_id >= 5 && s->alarm_b_remaining_ns == 0) {
            s->regs[GNW_H7B0_RTC_SR >> 2] |= RTC_SR_ALRBF;
            gnw_h7b0_rtc_schedule_alarm_b(s);
        } else {
            gnw_h7b0_rtc_schedule_alarm_b(s);
        }
    } else {
        s->alarm_b_deadline_ns = -1;
    }
    if (s->regs[GNW_H7B0_RTC_CR >> 2] & RTC_CR_WUTE) {
        if (version_id >= 5 && s->wakeup_remaining_ns > 0) {
            s->wakeup_deadline_ns = now_ns + s->wakeup_remaining_ns;
            timer_mod_ns(s->wakeup_timer, s->wakeup_deadline_ns);
        } else if (version_id >= 5 && s->wakeup_remaining_ns == 0) {
            s->regs[GNW_H7B0_RTC_SR >> 2] |= RTC_SR_WUTF;
            gnw_h7b0_rtc_schedule_wakeup(s);
        } else {
            gnw_h7b0_rtc_schedule_wakeup(s);
        }
    } else {
        s->wakeup_deadline_ns = -1;
    }
    gnw_h7b0_rtc_update_irq(s);
    return 0;
}

static int gnw_h7b0_rtc_pre_save(void *opaque)
{
    GnwH7B0RtcState *s = opaque;

    if (timer_pending(s->alarm_a_timer)) {
        s->alarm_a_remaining_ns = MAX(
            s->alarm_a_deadline_ns -
                qemu_clock_get_ns(s->clock),
            0);
    } else {
        s->alarm_a_remaining_ns = -1;
    }
    if (timer_pending(s->alarm_b_timer)) {
        s->alarm_b_remaining_ns = MAX(
            s->alarm_b_deadline_ns - qemu_clock_get_ns(s->clock), 0);
    } else {
        s->alarm_b_remaining_ns = -1;
    }
    if (timer_pending(s->wakeup_timer)) {
        s->wakeup_remaining_ns = MAX(
            s->wakeup_deadline_ns - qemu_clock_get_ns(s->clock), 0);
    } else {
        s->wakeup_remaining_ns = -1;
    }
    return 0;
}

static const VMStateDescription vmstate_gnw_h7b0_rtc = {
    .name = TYPE_GNW_H7B0_RTC,
    .version_id = 6,
    .minimum_version_id = 1,
    .pre_save = gnw_h7b0_rtc_pre_save,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, GnwH7B0RtcState, GNW_H7B0_RTC_SIZE / 4),
        /* The calendar anchor was missing from v1 entirely, so a v1
         * snapshot restored the encoded TR/DR but kept whatever base the
         * destination's own reset() had seeded -- the calendar jumped on
         * the next read. Migrating it (v2) is what makes the restored
         * clock continue from where it was saved. */
        VMSTATE_INT64_V(rtc_base_epoch, GnwH7B0RtcState, 2),
        VMSTATE_INT64_V(rtc_base_vclock_ns, GnwH7B0RtcState, 2),
        VMSTATE_BOOL_V(shadow_locked, GnwH7B0RtcState, 2),
        VMSTATE_INT64_V(shadow_vclock_ns, GnwH7B0RtcState, 2),
        VMSTATE_BOOL_V(rtc_calendar_utc, GnwH7B0RtcState, 3),
        VMSTATE_INT64_V(alarm_a_remaining_ns, GnwH7B0RtcState, 4),
        VMSTATE_INT64_V(alarm_b_remaining_ns, GnwH7B0RtcState, 5),
        VMSTATE_INT64_V(wakeup_remaining_ns, GnwH7B0RtcState, 5),
        VMSTATE_BOOL_V(calendar_initialized, GnwH7B0RtcState, 6),
        VMSTATE_END_OF_LIST()
    },
    .post_load = gnw_h7b0_rtc_post_load,
};

static const Property gnw_h7b0_rtc_properties[] = {
    DEFINE_PROP_BOOL("use-rtc-clock", GnwH7B0RtcState, use_rtc_clock, false),
    DEFINE_PROP_BOOL("preserve-calendar", GnwH7B0RtcState, preserve_calendar, false),
    DEFINE_PROP_BOOL("sync-host", GnwH7B0RtcState, sync_host, true),
    DEFINE_PROP_INT64("initial-epoch", GnwH7B0RtcState, initial_epoch, -1),
};

static void gnw_h7b0_rtc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_gnw_h7b0_rtc;
    dc->realize = gnw_h7b0_rtc_realize;
    device_class_set_legacy_reset(dc, gnw_h7b0_rtc_reset);
    device_class_set_props(dc, gnw_h7b0_rtc_properties);
}

static void gnw_h7b0_rtc_finalize(Object *obj)
{
    GnwH7B0RtcState *s = GNW_H7B0_RTC(obj);

    timer_free(s->alarm_a_timer);
    timer_free(s->alarm_b_timer);
    timer_free(s->wakeup_timer);
}

static const TypeInfo gnw_h7b0_rtc_info = {
    .name          = TYPE_GNW_H7B0_RTC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GnwH7B0RtcState),
    .instance_init = gnw_h7b0_rtc_init,
    .instance_finalize = gnw_h7b0_rtc_finalize,
    .class_init    = gnw_h7b0_rtc_class_init,
};

static void gnw_h7b0_rtc_register_types(void)
{
    type_register_static(&gnw_h7b0_rtc_info);
}

type_init(gnw_h7b0_rtc_register_types)
