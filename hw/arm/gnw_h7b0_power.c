/*
 * Opt-in board inputs matching the existing desktop's battery percentage.
 * Channel4's 11000..13000 scale and active-low PA2/PE7 pins come from the
 * Retro-Go BQ24072 driver. This is a host-controlled approximation, not a
 * charger IC or battery chemistry model.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include <math.h>
#include "gnw_h7b0_power.h"
#include "qapi/error.h"
#include "system/runstate.h"
#include "migration/vmstate.h"

bool gnw_h7b0_power_is_supplied(const GnwH7B0Power *s)
{
    return !s->enabled || s->battery_present || s->usb;
}

static void power_apply(GnwH7B0Power *s)
{
    s->level = MIN(MAX(s->level, 0.0), 100.0);
    s->battery = (uint32_t)(s->level + 0.5);
    if (!s->soc) {
        return;
    }
    gnw_h7b0_adc_set_battery(&s->soc->adc,
                            s->battery_present ? 11000 + 20 * s->level : 0);
    gnw_h7b0_gpio_set_power(&s->soc->gpio, s->usb,
                            s->usb && s->battery_present && s->level < 100.0);
}

static void power_settle(GnwH7B0Power *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint32_t minutes = s->usb ? s->charge_time_min : s->battery_life_min;

    if (s->timer && s->battery_present && minutes && now >= s->last_update_ns) {
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
    gnw_h7b0_soc_set_supply(soc, gnw_h7b0_power_is_supplied(s));
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

static void power_transition(GnwH7B0Power *s, bool was_supplied, Error **errp)
{
    bool supplied = gnw_h7b0_power_is_supplied(s);
    int ret;

    if (!s->soc || was_supplied == supplied) {
        return;
    }

    /* Source fields already reflect the new state. Close the execution gate
     * for both falling and rising transitions before vm_stop can drop BQL.
     * It stays closed until the synchronous cold reset has completed. */
    s->transitioning = true;
    ret = vm_stop(RUN_STATE_PAUSED);
    gnw_h7b0_soc_set_supply(s->soc, supplied);
    s->transitioning = false;
    if (ret) {
        /* vm_stop has stopped the vCPUs even if a device/flush failed. The
         * physical source change and cold reset remain applied; report it
         * rather than rolling back to an executing, unpowered machine. */
        error_setg(errp, "supply change applied, but VM stop/flush failed (%d)",
                   ret);
    }
}

void gnw_h7b0_power_set_usb(GnwH7B0Power *s, bool usb, Error **errp)
{
    bool was_supplied = gnw_h7b0_power_is_supplied(s);

    power_settle(s);
    s->usb = usb;
    power_apply(s);
    power_transition(s, was_supplied, errp);
}

void gnw_h7b0_power_set_present(GnwH7B0Power *s, bool present, Error **errp)
{
    bool was_supplied = gnw_h7b0_power_is_supplied(s);

    power_settle(s);
    s->battery_present = present;
    power_apply(s);
    power_transition(s, was_supplied, errp);
}

void gnw_h7b0_power_free(GnwH7B0Power *s)
{
    if (s->timer) {
        timer_free(s->timer);
    }
}

static int power_pre_save(void *opaque)
{
    GnwH7B0Power *s = opaque;

    power_settle(s);
    memcpy(&s->saved_level_bits, &s->level, sizeof(s->level));
    s->tick_remaining_ns = s->timer && timer_pending(s->timer) ?
        MAX(timer_expire_time_ns(s->timer) - s->last_update_ns, 0) : -1;
    return 0;
}

static int power_post_load(void *opaque, int version_id)
{
    GnwH7B0Power *s = opaque;

    memcpy(&s->level, &s->saved_level_bits, sizeof(s->level));
    if (!isfinite(s->level) || s->level < 0 || s->level > 100 ||
        s->battery != (uint32_t)(s->level + 0.5) || s->tick_remaining_ns < -1) {
        return -EINVAL;
    }
    s->transitioning = false;
    s->last_update_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (s->timer) {
        timer_del(s->timer);
        if (s->tick_remaining_ns >= 0) {
            timer_mod(s->timer, s->last_update_ns + s->tick_remaining_ns);
        }
    }
    /* GPIO/ADC/SYSCFG/EXTI must all be loaded before reapplying inputs. */
    return 0;
}

static const VMStateDescription vmstate_power = {
    .name = "gnw-h7b0-board-power",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = power_pre_save,
    .post_load = power_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(usb, GnwH7B0Power),
        VMSTATE_BOOL(battery_present, GnwH7B0Power),
        VMSTATE_UINT32(battery, GnwH7B0Power),
        VMSTATE_UINT64(saved_level_bits, GnwH7B0Power),
        VMSTATE_INT64(tick_remaining_ns, GnwH7B0Power),
        VMSTATE_END_OF_LIST()
    },
};

void gnw_h7b0_power_snapshot_register(GnwH7B0Power *s)
{
    vmstate_register(NULL, 0, &vmstate_power, s);
}

void gnw_h7b0_power_snapshot_settle(GnwH7B0Power *s)
{
    if (s->enabled) {
        power_settle(s);
    }
}

void gnw_h7b0_power_restore(GnwH7B0Power *s)
{
    if (s->enabled) {
        /* Restore the execution gate directly, never invoke a cold transition. */
        s->soc->supply_present = gnw_h7b0_power_is_supplied(s);
        power_apply(s);
    }
}
