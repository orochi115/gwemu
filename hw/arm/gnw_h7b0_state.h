/* Opt-in, same-build native snapshot foundation. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef GNW_H7B0_STATE_H
#define GNW_H7B0_STATE_H

#include "hw/arm/gnw_h7b0_soc.h"
#include "gnw_h7b0_power.h"

typedef struct GnwH7B0Snapshot {
    bool enabled;
    char *id;
    GnwH7B0State *soc;
    GnwH7B0Power *power;
    int64_t rtc_clock_at_save;
} GnwH7B0Snapshot;

void gnw_h7b0_state_init(GnwH7B0Snapshot *s, GnwH7B0State *soc);
const char *gnw_h7b0_state_support(GnwH7B0Snapshot *s);
void gnw_h7b0_state_file(GnwH7B0Snapshot *s, const char *path, bool load,
                        Error **errp);

#endif
