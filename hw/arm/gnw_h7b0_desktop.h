/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef GNW_H7B0_DESKTOP_H
#define GNW_H7B0_DESKTOP_H
#include "qapi/error.h"
void gnw_h7b0_desktop_start(const char *endpoint, Error **errp);
int gnw_h7b0_desktop_lock(const char *path, Error **errp);
#endif
