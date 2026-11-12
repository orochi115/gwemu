/*
 * STM32H7B0 RTC minimal stub (Nintendo Game & Watch)
 *
 * Calendar and Alarm A behavior needed by firmware; not cycle-accurate.
 * Purpose: real
 * firmware's HAL_RTC_Init() -> RTC_EnterInitMode() writes ICSR.INIT
 * (bit 7) then polls ICSR.INITF (bit 6) for real hardware to confirm
 * entry into initialization mode, and RTC_ExitInitMode() ->
 * HAL_RTC_WaitForSynchro() then polls ICSR.RSF (bit 5) for the shadow
 * registers to resync -- both bounded-timeout HAL polls that must not
 * time out, or HAL_RTC_Init() returns HAL_TIMEOUT and firmware treats
 * that as fatal (retro-go's Error_Handler()/BSOD, traced via
 * gnw-chainloader/retro-go-sd's Core/Src/main.c MX_RTC_Init()). This
 * was a plain zero-initialized RAM placeholder through Phase 1 (see
 * STATUS.md); INITF/RSF never reading back set is exactly this
 * timeout. Every ICSR write instantly mirrors INITF|RSF|INITS set
 * (same "instant approximation" pattern as RCC/PWR/OSPI/ADC's
 * ON->RDY bits) so the poll never blocks.
 *
 * Alarms A and B run from QEMU_CLOCK_VIRTUAL. Their calendar fields and masks
 * select the next matching second. SCR clears their SR flags, MISR gates them
 * with CR.ALRAIE/ALRBIE, and the shared alarm output drives RTC_Alarm_IRQn.
 * The wakeup counter also runs from virtual time and updates WUTF/MISR. The
 * second device output carries WUTF gated by CR.WUTIE into EXTI line 19;
 * EXTI owns its mask, pending flag, and dedicated RTC_WKUP_IRQn output.
 *
 * TR/DR (time/date) are a ticking calendar, seeded from the host's
 * local time at reset by default and advanced via QEMU_CLOCK_VIRTUAL
 * (see gnw_h7b0_rtc_sync_calendar()/_reanchor_calendar() in the .c
 * file). The SoC overrides that default with Unix epoch 0 in timing mode.
 * The `initial-epoch` property can instead select a deterministic nonnegative
 * Unix epoch, displayed as UTC. The standard QEMU `-rtc base`
 * option is not consumed by this device. This was added because the
 * gnw-chainloader has a real clock display
 * that reads TR/DR and a static/stub value looked obviously wrong. A
 * write to either re-anchors the running calendar to the new value
 * (in case firmware ever sets the clock), but no tested firmware image
 * actually does that -- they only read it.
 *
 * SSR (the synchronous prescaler's sub-second down-counter) is derived
 * from the same calendar, using the programmed PRER.PREDIV_S, and
 * SSR/TR/DR reads take the hardware's shadow-register lock so the three
 * are always a coherent snapshot -- both in gnw_h7b0_rtc_read()/
 * _sync_calendar(). It has to be real, not a shadow: a constant SSR
 * makes the HAL's (PREDIV_S - SSR)/(PREDIV_S + 1) fraction constant
 * too, which silently freezes tv_usec for every
 * GW_GetCurrentMillis()/_gettimeofday() caller.
 *
 * Opt-in use-rtc-clock selects QEMU's -rtc clock for the calendar and timers;
 * preserve-calendar retains its anchor on warm reset. With use-rtc-clock and
 * sync-host=false, INIT freezes calendar/alarm A/B and commits TR/DR together
 * on exit; the independent wakeup counter continues. Backup-domain loss
 * explicitly reseeds the calendar. Defaults keep the historical behavior.
 *
 * Every other register (WPR, CALR, SHIFTR, timestamp and CFGR) is a plain
 * read/write shadow with no side effects.
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

#ifndef HW_MISC_GNW_H7B0_RTC_H
#define HW_MISC_GNW_H7B0_RTC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "qemu/timer.h"

#define TYPE_GNW_H7B0_RTC "gnw-h7b0-rtc"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0RtcState, GNW_H7B0_RTC)

/*
 * Register offsets/bits below are from
 * sdk/cmsis-device-h7/Include/stm32h7b0xx.h's RTC_TypeDef and
 * RTC_ICSR_x bit definitions (TAMP-merged variant, used on H7B0).
 */
#define GNW_H7B0_RTC_SIZE   0x400

#define GNW_H7B0_RTC_ICSR   0x0C
#define RTC_ICSR_INIT       (1U << 7)
#define RTC_ICSR_INITF      (1U << 6)
#define RTC_ICSR_RSF        (1U << 5)
#define RTC_ICSR_INITS      (1U << 4)
#define RTC_ICSR_WUTWF      (1U << 2)
#define RTC_ICSR_ALRBWF     (1U << 1)
#define RTC_ICSR_ALRAWF     (1U << 0)

#define GNW_H7B0_RTC_CR     0x18
#define RTC_CR_WUTIE        (1U << 14)
#define RTC_CR_ALRBIE       (1U << 13)
#define RTC_CR_ALRAIE       (1U << 12)
#define RTC_CR_WUTE         (1U << 10)
#define RTC_CR_ALRBE        (1U << 9)
#define RTC_CR_ALRAE        (1U << 8)

#define GNW_H7B0_RTC_SR     0x50
#define RTC_SR_WUTF         (1U << 2)
#define RTC_SR_ALRBF        (1U << 1)
#define RTC_SR_ALRAF        (1U << 0)

#define GNW_H7B0_RTC_MISR   0x54

#define GNW_H7B0_RTC_SCR    0x5C

struct GnwH7B0RtcState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    qemu_irq irq;
    qemu_irq wakeup_irq;
    QEMUTimer *alarm_a_timer;
    QEMUTimer *alarm_b_timer;
    QEMUTimer *wakeup_timer;
    int64_t alarm_a_deadline_ns;
    int64_t alarm_a_remaining_ns;
    int64_t alarm_b_deadline_ns;
    int64_t alarm_b_remaining_ns;
    int64_t wakeup_deadline_ns;
    int64_t wakeup_remaining_ns;
    uint32_t regs[GNW_H7B0_RTC_SIZE / 4];

    /*
     * Ticking calendar: rtc_base_epoch is the Unix time the
     * calendar read TR/DR as, at host virtual-clock time
     * rtc_base_vclock_ns -- current calendar time is always
     * rtc_base_epoch + (now - rtc_base_vclock_ns). Seeded from the
     * host's wall-clock time at reset by default (like a battery-backed RTC that
     * was already running) rather than 2000-01-01, since real
     * firmware (gnw-chainloader's clock display) only ever *reads*
     * this calendar -- it has no in-firmware time-set UI in the
     * images tested so far, so there's nothing to seed it from except
     * the host clock. A nonnegative `initial_epoch` property selects a
     * deterministic UTC epoch; the -1 default preserves host-local fields.
     */
    int64_t rtc_base_epoch;
    int64_t rtc_base_vclock_ns;
    /* Optional deterministic epoch; -1 keeps the historical host-local seed. */
    int64_t initial_epoch;
    /* Calendar interpretation captured at reset and included in migration. */
    bool rtc_calendar_utc;

    /*
     * Calendar shadow-register lock: virtual-clock instant that a
     * pending SSR/TR/DR read sequence is being served from, held from
     * the first SSR-or-TR read until the closing DR read (see the
     * comment in gnw_h7b0_rtc_read()).
     */
    bool shadow_locked;
    int64_t shadow_vclock_ns;

    bool sync_host;
    /* Opt-in desktop policy; defaults preserve upstream behavior. */
    bool use_rtc_clock;
    bool preserve_calendar;
    bool calendar_initialized;
    QEMUClockType clock;

};

void gnw_h7b0_rtc_set_sync_host(bool sync_host);
void gnw_h7b0_rtc_backup_domain_reset(GnwH7B0RtcState *s);

#endif
