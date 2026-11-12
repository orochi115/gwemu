/*
 * Opt-in board inputs matching the existing desktop's battery percentage.
 * Channel4's 11000..13000 scale and active-low PA2/PE7 pins come from the
 * Retro-Go BQ24072 driver. This is a host-controlled approximation, not a
 * charger IC or battery-disconnection model.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "gnw_h7b0_power.h"

static void power_apply(GnwH7B0Power *s)
{
    s->level = MIN(MAX(s->level, 0.0), 100.0);
    s->battery = (uint32_t)(s->level + 0.5);
    if (!s->soc) {
        return;
    }
    gnw_h7b0_adc_set_battery(&s->soc->adc, 11000 + 20 * s->level);
    gnw_h7b0_gpio_set_power(&s->soc->gpio, s->usb,
                            s->usb && s->level < 100.0);
}

static void power_settle(GnwH7B0Power *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint32_t minutes = s->usb ? s->charge_time_min : s->battery_life_min;

    if (s->timer && minutes && now >= s->last_update_ns) {
        /* Double denominator avoids uint32 overflow for long configured periods. */
        double delta = (now - s->last_update_ns) / 1e9 * 100.0 /
                       (minutes * 60.0);
        s->level += s->usb ? delta : -delta;
    }
    s->last_update_ns = now;
    power_apply(s);
}

static void power_tick(void *opaque)
{
    GnwH7B0Power *s = opaque;

    power_settle(s);
    timer_mod(s->timer, s->last_update_ns + NANOSECONDS_PER_SECOND);
}

void gnw_h7b0_power_start(GnwH7B0Power *s, GnwH7B0State *soc)
{
    if (!s->enabled) {
        return;
    }
    s->soc = soc;
    s->level = s->battery;
    s->last_update_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    power_apply(s);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, power_tick, s);
    timer_mod(s->timer, s->last_update_ns + NANOSECONDS_PER_SECOND);
}

void gnw_h7b0_power_set_battery(GnwH7B0Power *s, uint32_t level)
{
    power_settle(s);
    s->level = level;
    power_apply(s);
}

void gnw_h7b0_power_set_usb(GnwH7B0Power *s, bool usb)
{
    power_settle(s);
    s->usb = usb;
    power_apply(s);
}

void gnw_h7b0_power_free(GnwH7B0Power *s)
{
    if (s->timer) {
        timer_free(s->timer);
    }
}
