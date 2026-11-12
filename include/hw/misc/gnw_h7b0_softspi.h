/* GPIO mode-0 bridge for the G&W SD mod on the external-Flash pins. */
#ifndef HW_MISC_GNW_H7B0_SOFTSPI_H
#define HW_MISC_GNW_H7B0_SOFTSPI_H

#include "hw/ssi/ssi.h"
#include "migration/vmstate.h"

typedef struct GnwH7B0SoftSpi {
    SSIBus *bus;
    bool selected;
    bool clock;
    bool miso;
    uint8_t bits;
    uint8_t tx;
    uint8_t rx;
} GnwH7B0SoftSpi;

void gnw_h7b0_softspi_reset(GnwH7B0SoftSpi *s);
void gnw_h7b0_softspi_update(GnwH7B0SoftSpi *s, const uint32_t *gpio);
extern const VMStateDescription vmstate_gnw_h7b0_softspi;

#endif
