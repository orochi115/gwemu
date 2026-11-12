/*
 * GPIO mode-0 byte bridge, reusing QEMU's upstream SSI SD protocol.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/misc/gnw_h7b0_softspi.h"

/* GPIO register layout: ports are 0x400 apart, MODER=0 and ODR=0x14. */
static bool output(const uint32_t *gpio, unsigned port, unsigned pin)
{
    return ((gpio[port * 0x400 / 4] >> (pin * 2)) & 3) == 1;
}

static bool level(const uint32_t *gpio, unsigned port, unsigned pin)
{
    return (gpio[(port * 0x400 + 0x14) / 4] >> pin) & 1;
}

void gnw_h7b0_softspi_reset(GnwH7B0SoftSpi *s)
{
    s->selected = false;
    s->clock = false;
    s->miso = true;
    s->bits = 0;
    s->tx = 0;
    s->rx = 0xff;
}

void gnw_h7b0_softspi_update(GnwH7B0SoftSpi *s, const uint32_t *gpio)
{
    bool selected, clock;

    if (!s->bus) {
        return;
    }
    /* PB2 CLK, PB1 MOSI and PE11 select must actually be GPIO outputs.
     * PE11 high selects SD, while low belongs to the external NOR Flash.
     * Alternate-function OSPI traffic must never reach this bridge. */
    selected = output(gpio, 1, 2) && output(gpio, 1, 1) &&
               output(gpio, 4, 11) && level(gpio, 4, 11);
    clock = output(gpio, 1, 2) && level(gpio, 1, 2);
    if (!selected) {
        /* Do not submit an incomplete byte. Byte-aligned deselection must
         * preserve the pending response: firmware toggles select between
         * individual response bytes, including CMD8's four trailing bytes.
         * The upstream SSI SD parser keeps its byte-level state across CS. */
        if (s->bits) {
            s->bits = 0;
            s->tx = 0;
            s->rx = 0xff;
        }
        s->selected = false;
        s->clock = clock;
        s->miso = true;
        return;
    }
    if (!s->selected) {
        s->selected = true;
        s->clock = clock;
        s->miso = s->rx & 0x80;
        return; /* Selecting with CLK high is not a rising clock edge. */
    }
    if (clock && !s->clock) {
        s->tx = (s->tx << 1) | level(gpio, 1, 1);
        s->rx <<= 1;
        if (++s->bits == 8) {
            /* A byte-only SSI target cannot know outgoing MOSI before all
             * eight bits arrive. Its reply is clocked in the next byte.
             * Keep the eighth MISO bit stable until the falling edge. */
            s->rx = ssi_transfer(s->bus, s->tx) & 0xff;
            s->bits = 0;
            s->tx = 0;
        }
    } else if (!clock && s->clock) {
        s->miso = s->rx & 0x80;
    }
    s->clock = clock;
}

const VMStateDescription vmstate_gnw_h7b0_softspi = {
    .name = "gnw-h7b0-softspi",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(selected, GnwH7B0SoftSpi),
        VMSTATE_BOOL(clock, GnwH7B0SoftSpi),
        VMSTATE_BOOL(miso, GnwH7B0SoftSpi),
        VMSTATE_UINT8(bits, GnwH7B0SoftSpi),
        VMSTATE_UINT8(tx, GnwH7B0SoftSpi),
        VMSTATE_UINT8(rx, GnwH7B0SoftSpi),
        VMSTATE_END_OF_LIST()
    }
};
