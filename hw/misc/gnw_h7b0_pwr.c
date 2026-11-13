/*
 * STM32H7B0 PWR minimal stub (Nintendo Game & Watch)
 *
 * See gnw_h7b0_pwr.h for scope/rationale.
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

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "migration/vmstate.h"
#include "hw/misc/gnw_h7b0_pwr.h"
#include "hw/misc/gnw_h7b0_regs_pwr.h"

static void gnw_h7b0_pwr_reset(DeviceState *dev)
{
    GnwH7B0PwrState *s = GNW_H7B0_PWR(dev);

    for (int i = 0; i < (GNW_H7B0_PWR_SIZE / 4); i++) {
        s->regs[i] = get_pwr_reset_value(i * 4);
    }

    /*
     * PWR_CPUCR.SBF (bit 6, "System Standby flag... set by hardware" per
     * STM32H7B0.svd) is documented as a real hardware-set flag, not a
     * static POR default -- but on this specific product, every normal
     * power-on IS a wake-from-Standby event: the physical power button is
     * wired to PA0/WKUP1 (see hw/misc/gnw_h7b0_gpio.c's pa0_release_timer
     * and its doc comment), and this device's real "off" state is
     * silicon Standby, not a true power cut. Confirmed via live real-
     * hardware vs. QEMU comparison (2026-07-19): stock Mario firmware's
     * own boot-init code (FUN_08006030 in the decompiled binary) checks
     * this exact bit and only proceeds into normal display/graphics init
     * if it's set -- real hardware always has it set at this point in
     * boot (a live register read confirmed this), QEMU never did,
     * because nothing here ever set it. Set it unconditionally at reset
     * to match every observed real boot.
     */
    s->regs[GNW_H7B0_PWR_CPUCR_OFFSET >> 2] |= (1u << 6);
    s->regs[GNW_H7B0_PWR_WKUPFR_OFFSET >> 2] |= 1u;
}

static uint64_t gnw_h7b0_pwr_read(void *opaque, hwaddr addr, unsigned int size)
{
    GnwH7B0PwrState *s = GNW_H7B0_PWR(opaque);

    if (addr + size > GNW_H7B0_PWR_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return 0;
    }
    return s->regs[addr >> 2] >> ((addr & 3) * 8);
}

static void gnw_h7b0_pwr_write(void *opaque, hwaddr addr,
                                uint64_t val64, unsigned int size)
{
    GnwH7B0PwrState *s = GNW_H7B0_PWR(opaque);
    hwaddr reg = addr & ~3ULL;
    unsigned shift = (addr & 3) * 8;
    uint32_t command = (uint32_t)val64 << shift;
    uint32_t value;

    if (addr + size > GNW_H7B0_PWR_SIZE || shift + size * 8 > 32) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return;
    }

    uint32_t mask = get_pwr_write_mask(reg);
    value = size == 4 ? val64 :
        deposit32(s->regs[reg >> 2], shift, size * 8, val64);
    value = (s->regs[reg >> 2] & ~mask) | (value & mask);

    switch (reg) {
    case GNW_H7B0_PWR_CPUCR_OFFSET:
        /* STOPF/SBF are hardware status; CSSF is a self-clearing W1C
         * pulse. Preserve status on ordinary control writes. */
        value = (value & ~0x260u) | (s->regs[reg >> 2] & 0x60u);
        if (command & (1u << 9)) {
            value &= ~0x60u;
        }
        s->regs[reg >> 2] = value;
        return;
    case GNW_H7B0_PWR_WKUPCR_OFFSET:
        s->regs[GNW_H7B0_PWR_WKUPFR_OFFSET >> 2] &= ~(command & 0x3fu);
        s->regs[reg >> 2] = 0;
        return;
    case GNW_H7B0_PWR_WKUPFR_OFFSET:
        return; /* Read-only hardware flags. */
    case GNW_H7B0_PWR_CR3:
        /*
         * Writing the supply configuration instantly "completes" it:
         * set ACTVOSRDY in CSR1 (real hardware takes a few cycles, not
         * modeled). Good enough to unblock the polling loop that always
         * follows this write in ST HAL-derived supply-config code.
         */
        s->regs[reg >> 2] = value;
        s->regs[GNW_H7B0_PWR_CSR1 >> 2] |= PWR_CSR1_ACTVOSRDY;
        return;
    case GNW_H7B0_PWR_SRDCR:
        /* Same idea: VOS write instantly mirrors into its own VOSRDY. */
        s->regs[reg >> 2] = value | PWR_SRDCR_VOSRDY;
        return;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: offset 0x%"HWADDR_PRIx" is a plain read/write "
                      "shadow, no real power-domain behavior modeled\n",
                      __func__, addr);
        s->regs[reg >> 2] = value;
    }
}

static const MemoryRegionOps gnw_h7b0_pwr_ops = {
    .read = gnw_h7b0_pwr_read,
    .write = gnw_h7b0_pwr_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void gnw_h7b0_pwr_init(Object *obj)
{
    GnwH7B0PwrState *s = GNW_H7B0_PWR(obj);

    memory_region_init_io(&s->mmio, obj, &gnw_h7b0_pwr_ops, s,
                           TYPE_GNW_H7B0_PWR, GNW_H7B0_PWR_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static const VMStateDescription vmstate_gnw_h7b0_pwr = {
    .name = TYPE_GNW_H7B0_PWR,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, GnwH7B0PwrState, GNW_H7B0_PWR_SIZE / 4),
        VMSTATE_END_OF_LIST()
    }
};

static void gnw_h7b0_pwr_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_gnw_h7b0_pwr;
    device_class_set_legacy_reset(dc, gnw_h7b0_pwr_reset);
}

static const TypeInfo gnw_h7b0_pwr_info = {
    .name          = TYPE_GNW_H7B0_PWR,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GnwH7B0PwrState),
    .instance_init = gnw_h7b0_pwr_init,
    .class_init    = gnw_h7b0_pwr_class_init,
};

static void gnw_h7b0_pwr_register_types(void)
{
    type_register_static(&gnw_h7b0_pwr_info);
}

type_init(gnw_h7b0_pwr_register_types)
