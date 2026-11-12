/* GNWF v1: shared with gnw-emu desktop/src/shared/frame-protocol.ts.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef GNW_H7B0_FRAME_PROTOCOL_H
#define GNW_H7B0_FRAME_PROTOCOL_H
#include "qemu/bswap.h"
#define GNW_FRAME_HEADER 64
#define GNW_FRAME_MAX (16 * 1024 * 1024)
enum { GNW_HELLO = 1, GNW_HELLO_ACK, GNW_FRAME, GNW_FRAME_ACK };
/* Explicit byte offsets are shared with desktop/src/shared/frame-protocol.ts. */
static inline void gnw_frame_header(uint8_t *p, uint16_t type)
{
    memset(p, 0, GNW_FRAME_HEADER);
    memcpy(p, "GNWF", 4);
    stw_le_p(p + 4, 1); stw_le_p(p + 6, type);
    stl_le_p(p + 8, GNW_FRAME_HEADER);
}
#endif
