/* STM32H7B0 DAC holding-register aliases and output transfers. */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "migration/vmstate.h"
#include "hw/misc/gnw_h7b0_dac.h"
#include "hw/misc/gnw_h7b0_regs_dac.h"

#define DAC_CR_EN1  (1U << 0)
#define DAC_CR_TEN1 (1U << 1)

static void gnw_h7b0_dac_transfer(GnwH7B0DacState *s, int channel,
                                  bool software)
{
    uint32_t control = s->regs[0] >> (16 * channel);
    bool triggered = control & DAC_CR_TEN1;

    /* TSEL=0 selects software. Disabled channels retain their DOR. */
    if (!(control & DAC_CR_EN1) ||
        (software ? !triggered || (control & 0x3c) : triggered)) {
        return;
    }
    s->regs[(GNW_H7B0_DAC1_DOR1_OFFSET >> 2) + channel] =
        s->regs[(GNW_H7B0_DAC1_DHR12R1_OFFSET + channel * 12) >> 2];
}

static void gnw_h7b0_dac_set_holding(GnwH7B0DacState *s, int channel,
                                     uint32_t value)
{
    hwaddr base = GNW_H7B0_DAC1_DHR12R1_OFFSET + channel * 12;
    uint32_t shift = channel * 16;
    uint32_t shift8 = channel * 8;

    value &= 0xfff;
    s->regs[base >> 2] = value;
    s->regs[(base + 4) >> 2] = value << 4;
    s->regs[(base + 8) >> 2] = value >> 4;
    s->regs[GNW_H7B0_DAC1_DHR12RD_OFFSET >> 2] = deposit32(
        s->regs[GNW_H7B0_DAC1_DHR12RD_OFFSET >> 2], shift, 12, value);
    s->regs[GNW_H7B0_DAC1_DHR12LD_OFFSET >> 2] = deposit32(
        s->regs[GNW_H7B0_DAC1_DHR12LD_OFFSET >> 2], shift + 4, 12, value);
    s->regs[GNW_H7B0_DAC1_DHR8RD_OFFSET >> 2] = deposit32(
        s->regs[GNW_H7B0_DAC1_DHR8RD_OFFSET >> 2], shift8, 8, value >> 4);
    gnw_h7b0_dac_transfer(s, channel, false);
}

static void gnw_h7b0_dac_reset(DeviceState *dev)
{
    GnwH7B0DacState *s = GNW_H7B0_DAC(dev);
    for (int i = 0; i < (GNW_H7B0_DAC_SIZE / 4); i++) {
        s->regs[i] = get_dac_reset_value(i * 4);
    }
}

static uint64_t gnw_h7b0_dac_read(void *opaque, hwaddr addr, unsigned int size)
{
    GnwH7B0DacState *s = GNW_H7B0_DAC(opaque);
    if (addr >= GNW_H7B0_DAC_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n", __func__, addr);
        return 0;
    }
    return s->regs[addr >> 2] >> ((addr & 3) * 8);
}

static void gnw_h7b0_dac_write(void *opaque, hwaddr addr, uint64_t val64, unsigned int size)
{
    GnwH7B0DacState *s = GNW_H7B0_DAC(opaque);
    if (addr >= GNW_H7B0_DAC_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n", __func__, addr);
        return;
    }
    unsigned int lane = (addr & 3) * 8;
    uint32_t value, mask;

    addr &= ~3ULL;
    value = deposit32(s->regs[addr >> 2], lane, size * 8, val64);
    mask = get_dac_write_mask(addr);
    s->regs[addr >> 2] = (s->regs[addr >> 2] & ~mask) | (value & mask);
    value = s->regs[addr >> 2];
    if (addr == GNW_H7B0_DAC1_CR_OFFSET) {
        gnw_h7b0_dac_transfer(s, 0, false);
        gnw_h7b0_dac_transfer(s, 1, false);
    } else if (addr == GNW_H7B0_DAC1_SWTRGR_OFFSET) {
        for (int channel = 0; channel < 2; channel++) {
            if (value & (1u << channel)) {
                gnw_h7b0_dac_transfer(s, channel, true);
            }
        }
        s->regs[addr >> 2] = 0;
    } else if (addr >= GNW_H7B0_DAC1_DHR12R1_OFFSET &&
               addr <= GNW_H7B0_DAC1_DHR8R2_OFFSET) {
        int channel = (addr - GNW_H7B0_DAC1_DHR12R1_OFFSET) / 12;
        int format = (addr - GNW_H7B0_DAC1_DHR12R1_OFFSET) % 12;
        gnw_h7b0_dac_set_holding(s, channel,
            format == 4 ? value >> 4 : format == 8 ? value << 4 : value);
    } else if (addr >= GNW_H7B0_DAC1_DHR12RD_OFFSET &&
               addr <= GNW_H7B0_DAC1_DHR8RD_OFFSET) {
        for (int channel = 0; channel < 2; channel++) {
            uint32_t code = value >> (channel * (addr ==
                GNW_H7B0_DAC1_DHR8RD_OFFSET ? 8 : 16));
            gnw_h7b0_dac_set_holding(s, channel,
                addr == GNW_H7B0_DAC1_DHR12LD_OFFSET ? code >> 4 :
                addr == GNW_H7B0_DAC1_DHR8RD_OFFSET ? (code & 0xff) << 4 : code);
        }
    }
}

static const MemoryRegionOps gnw_h7b0_dac_ops = {
    .read = gnw_h7b0_dac_read,
    .write = gnw_h7b0_dac_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void gnw_h7b0_dac_init(Object *obj)
{
    GnwH7B0DacState *s = GNW_H7B0_DAC(obj);
    memory_region_init_io(&s->mmio, obj, &gnw_h7b0_dac_ops, s, TYPE_GNW_H7B0_DAC, GNW_H7B0_DAC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static const VMStateDescription vmstate_gnw_h7b0_dac = {
    .name = TYPE_GNW_H7B0_DAC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, GnwH7B0DacState, GNW_H7B0_DAC_SIZE / 4),
        VMSTATE_END_OF_LIST()
    }
};

static void gnw_h7b0_dac_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->vmsd = &vmstate_gnw_h7b0_dac;
    device_class_set_legacy_reset(dc, gnw_h7b0_dac_reset);
}

static const TypeInfo gnw_h7b0_dac_info = {
    .name          = TYPE_GNW_H7B0_DAC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GnwH7B0DacState),
    .instance_init = gnw_h7b0_dac_init,
    .class_init    = gnw_h7b0_dac_class_init,
};

static void gnw_h7b0_dac_register_types(void)
{
    type_register_static(&gnw_h7b0_dac_info);
}
type_init(gnw_h7b0_dac_register_types)
