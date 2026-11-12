/* Board charger inputs for the gnw-emu desktop. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef GNW_H7B0_POWER_H
#define GNW_H7B0_POWER_H

#include "qemu/timer.h"
#include "hw/arm/gnw_h7b0_soc.h"

typedef struct GnwH7B0Power {
    bool enabled;
    bool usb;
    uint32_t battery;
    uint32_t battery_life_min;
    uint32_t charge_time_min;
    double level;
    int64_t last_update_ns;
    GnwH7B0State *soc;
    QEMUTimer *timer;
} GnwH7B0Power;

void gnw_h7b0_power_start(GnwH7B0Power *s, GnwH7B0State *soc);
void gnw_h7b0_power_set_battery(GnwH7B0Power *s, uint32_t level);
void gnw_h7b0_power_set_usb(GnwH7B0Power *s, bool usb);
void gnw_h7b0_power_free(GnwH7B0Power *s);

#endif
