/* Board charger inputs for the gnw-emu desktop. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef GNW_H7B0_POWER_H
#define GNW_H7B0_POWER_H

#include "qemu/timer.h"
#include "hw/arm/gnw_h7b0_soc.h"

typedef struct GnwH7B0Power {
    bool enabled;
    bool usb;
    bool battery_present;
    bool transitioning;
    uint32_t battery;
    uint32_t battery_life_min;
    uint32_t charge_time_min;
    double level;
    int64_t last_update_ns;
    uint64_t saved_level_bits;
    int64_t tick_remaining_ns;
    GnwH7B0State *soc;
    QEMUTimer *timer;
} GnwH7B0Power;

void gnw_h7b0_power_start(GnwH7B0Power *s, GnwH7B0State *soc);
void gnw_h7b0_power_set_battery(GnwH7B0Power *s, uint32_t level);
bool gnw_h7b0_power_is_supplied(const GnwH7B0Power *s);
void gnw_h7b0_power_set_usb(GnwH7B0Power *s, bool usb, Error **errp);
void gnw_h7b0_power_set_present(GnwH7B0Power *s, bool present, Error **errp);
void gnw_h7b0_power_free(GnwH7B0Power *s);
void gnw_h7b0_power_snapshot_register(GnwH7B0Power *s);
void gnw_h7b0_power_snapshot_settle(GnwH7B0Power *s);
void gnw_h7b0_power_restore(GnwH7B0Power *s);

#endif
