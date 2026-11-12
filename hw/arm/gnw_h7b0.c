/*
 * Nintendo Game & Watch (STM32H7B0) Machine Model
 *
 * Phase 1: instantiates gnw-h7b0-soc (real SRAM bank layout + internal/
 * external flash regions, no peripherals yet) and loads a kernel image
 * into flash bank 1 at 0x08000000, matching real hardware's boot path
 * (BOOT_ADD option-byte remap on cold boot, or a debug probe's VTOR/SP/
 * PC direct-set on the gnwmanager dev flow -- see gnw_h7b0_soc.c's
 * init-svtor comment) and where retro-go's own linker script places
 * .isr_vector. See ../../docs/roadmap.md.
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
#include "qapi/error.h"
#include "hw/core/boards.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/arm/gnw_h7b0_soc.h"
#include "hw/arm/boot.h"
#include "hw/arm/machines-qom.h"
#include "gnw_h7b0_desktop.h"
#include "gnw_h7b0_power.h"
#include "qapi/visitor.h"

/*
 * Was 280MHz (the datasheet's stated max) as a placeholder guess -- but
 * retro-go's actual PLL1 configuration (RCC->PLLCKSELR/PLLCFGR/
 * PLL1DIVR, decoded by HAL_RCC_GetSysClockFreq()) computes to
 * 340512000 Hz exactly, confirmed by reading SystemCoreClock live
 * out of guest RAM after boot. Since our RCC model is a plain
 * register shadow (no real PLL lock/frequency modeling -- see
 * gnw_h7b0_rcc.c), firmware's own clock-derived timing (SysTick reload,
 * frame-pacing math in Core/Src/porting/common.c's
 * frame_integrator/get_elapsed_time()) only comes out correct if this
 * Clock's rate actually matches what firmware computed it configured
 * PLL1 to. Using 280MHz here made every SysTick/HAL_GetTick()-based
 * measurement run at 280/340.5 =~ 82% of the real elapsed-time rate,
 * which made retro-go's frame_integrator perpetually think less time
 * had passed than really had, believing (from ITS perspective, based
 * on this now-provably-wrong clock) that emulation was running faster
 * than intended -- permanently latching common_emu_state.pause_frames
 * at 1. That's what was producing "runs too fast/stuttery" gameplay
 * and, audibly, corrupted retro-go audio: pause_frames=1 makes
 * common_emu_sound_sync() wait for two DMA half-transfer ticks per
 * call instead of one, while the per-core sound_submit() (e.g.
 * gwenesis_sound_submit()) still only refills whichever ONE audio
 * buffer half is active when it finally runs, permanently starving
 * the other -- audible as pitch/rate distortion and stuck-tone
 * "ripping" artifacts smeared into otherwise-normal audio.
 */
#define SYSCLK_FRQ 340512000ULL

#define TYPE_GNW_H7B0_MACHINE MACHINE_TYPE_NAME("gnw-h7b0")
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0MachineState, GNW_H7B0_MACHINE)

struct GnwH7B0MachineState {
    MachineState parent_obj;
    char *frame_endpoint;
    char *session_lock;
    char *sd_wiring;
    int session_lock_fd;
    bool initialized;
    GnwH7B0Power power;
};

static void gnw_h7b0_init(MachineState *machine)
{
    DeviceState *dev;
    Clock *sysclk;
    GnwH7B0MachineState *s = GNW_H7B0_MACHINE(machine);

    s->initialized = true;
    /* Acquire before the SoC can pad or mmap any writable Flash image. */
    s->session_lock_fd = gnw_h7b0_desktop_lock(s->session_lock, &error_fatal);

    sysclk = clock_new(OBJECT(machine), "SYSCLK");
    /*
     * Real silicon's actual post-reset state is HSI @ 64MHz -- PLL1 isn't
     * even running yet at this point, let alone reprogrammed to one of
     * retro-go's three OC levels (see SYSCLK_FRQ's comment above for
     * those). gnw_h7b0_rcc.c now decodes PLL1/CFGR.SWS live and pushes a
     * clock_update_hz() through this same Clock the moment firmware's
     * SystemClock_Config() runs early in boot (gnw_h7b0_soc_realize()
     * wires RCC to this Clock via gnw_h7b0_rcc_set_sysclk()), so this
     * bootstrap value only matters for the brief pre-SystemClock_Config
     * window -- 64000000 literal rather than including gnw_h7b0_rcc.h's
     * GNW_H7B0_RCC_HSI_HZ here, to avoid pulling a hw/misc/ header into
     * the board file for one constant.
     */
    clock_set_hz(sysclk, 64000000ULL);

    dev = qdev_new(TYPE_GNW_H7B0_SOC);
    qdev_prop_set_string(dev, "sd-wiring", s->sd_wiring);
    object_property_add_child(OBJECT(machine), "soc", OBJECT(dev));
    qdev_connect_clock_in(dev, "sysclk", sysclk);

    /* SAI1 (gnw_h7b0_sai1.c) opens an AUD_* voice and needs to know
     * which -audiodev to use; machine->audiodev is QEMU's usual
     * "the one -audiodev the user passed" plumbing (see e.g.
     * hw/arm/versatilepb.c's pl041 wiring). Must be set before the
     * SoC (and thus SAI1) realizes. */
    if (machine->audiodev) {
        qdev_prop_set_string(DEVICE(&GNW_H7B0_SOC(dev)->sai1), "audiodev",
                              machine->audiodev);
    }

    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    gnw_h7b0_power_start(&s->power, GNW_H7B0_SOC(dev));

    armv7m_load_kernel(ARM_CPU(first_cpu),
                        machine->kernel_filename,
                        FLASH_BANK1_BASE_ADDRESS, FLASH_BANK_SIZE);

    gnw_h7b0_desktop_start(s->frame_endpoint, &error_fatal);
}

static void gnw_h7b0_machine_init(MachineClass *mc)
{
    static const char * const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("cortex-m7"),
        NULL
    };

    mc->desc = "Nintendo Game & Watch (STM32H7B0, Cortex-M7)";
    mc->init = gnw_h7b0_init;
    mc->valid_cpu_types = valid_cpu_types;
    machine_add_audiodev_property(mc);
}

#define DESKTOP_STRING_PROPERTY(field, name) \
static char *gnw_h7b0_get_##field(Object *obj, Error **errp) \
{ \
    return g_strdup(GNW_H7B0_MACHINE(obj)->field); \
} \
static void gnw_h7b0_set_##field(Object *obj, const char *value, Error **errp) \
{ \
    GnwH7B0MachineState *s = GNW_H7B0_MACHINE(obj); \
    if (s->initialized) { \
        error_setg(errp, name " can only be set before machine initialization"); \
        return; \
    } \
    g_free(s->field); \
    s->field = g_strdup(value); \
}

DESKTOP_STRING_PROPERTY(frame_endpoint, "frame-endpoint")
DESKTOP_STRING_PROPERTY(session_lock, "session-lock")

static char *gnw_h7b0_get_sd_wiring(Object *obj, Error **errp)
{
    return g_strdup(GNW_H7B0_MACHINE(obj)->sd_wiring);
}

static void gnw_h7b0_set_sd_wiring(Object *obj, const char *value, Error **errp)
{
    GnwH7B0MachineState *s = GNW_H7B0_MACHINE(obj);

    if (s->initialized) {
        error_setg(errp, "sd-wiring can only be set before machine initialization");
        return;
    }
    if (strcmp(value, "spi1") && strcmp(value, "ospi")) {
        error_setg(errp, "sd-wiring must be spi1 or ospi");
        return;
    }
    g_free(s->sd_wiring);
    s->sd_wiring = g_strdup(value);
}

static bool gnw_h7b0_get_power_model(Object *obj, Error **errp)
{
    return GNW_H7B0_MACHINE(obj)->power.enabled;
}

static void gnw_h7b0_set_power_model(Object *obj, bool value, Error **errp)
{
    GnwH7B0MachineState *s = GNW_H7B0_MACHINE(obj);
    if (s->initialized) {
        error_setg(errp, "power-model can only be set before machine initialization");
        return;
    }
    s->power.enabled = value;
}

static bool gnw_h7b0_get_usb(Object *obj, Error **errp)
{
    return GNW_H7B0_MACHINE(obj)->power.usb;
}

static void gnw_h7b0_set_usb(Object *obj, bool value, Error **errp)
{
    GnwH7B0MachineState *s = GNW_H7B0_MACHINE(obj);
    if (s->initialized && !s->power.enabled) {
        error_setg(errp, "usb-power requires power-model=on");
        return;
    }
    gnw_h7b0_power_set_usb(&s->power, value);
}

static void gnw_h7b0_get_battery(Object *obj, Visitor *v,
                                const char *name, void *opaque, Error **errp)
{
    visit_type_uint32(v, name, &GNW_H7B0_MACHINE(obj)->power.battery, errp);
}

static void gnw_h7b0_set_battery(Object *obj, Visitor *v,
                                const char *name, void *opaque, Error **errp)
{
    GnwH7B0MachineState *s = GNW_H7B0_MACHINE(obj);
    uint32_t value;
    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (value > 100) {
        error_setg(errp, "battery must be 0..100");
        return;
    }
    if (s->initialized && !s->power.enabled) {
        error_setg(errp, "battery requires power-model=on");
        return;
    }
    gnw_h7b0_power_set_battery(&s->power, value);
}

#define POWER_PERIOD_PROPERTY(field, property) \
static void gnw_h7b0_get_##field(Object *obj, Visitor *v, \
                                 const char *name, void *opaque, Error **errp) \
{ \
    visit_type_uint32(v, name, &GNW_H7B0_MACHINE(obj)->power.field, errp); \
} \
static void gnw_h7b0_set_##field(Object *obj, Visitor *v, \
                                 const char *name, void *opaque, Error **errp) \
{ \
    GnwH7B0MachineState *s = GNW_H7B0_MACHINE(obj); \
    uint32_t value; \
    if (!visit_type_uint32(v, name, &value, errp)) { return; } \
    if (s->initialized) { \
        error_setg(errp, property " can only be set before machine initialization"); \
        return; \
    } \
    s->power.field = value; \
}

POWER_PERIOD_PROPERTY(battery_life_min, "battery-life-min")
POWER_PERIOD_PROPERTY(charge_time_min, "charge-time-min")

static void gnw_h7b0_machine_class_init(ObjectClass *oc, const void *data)
{
    gnw_h7b0_machine_init(MACHINE_CLASS(oc));
    object_class_property_add_str(oc, "frame-endpoint", gnw_h7b0_get_frame_endpoint,
                                 gnw_h7b0_set_frame_endpoint);
    object_class_property_add_str(oc, "session-lock", gnw_h7b0_get_session_lock,
                                 gnw_h7b0_set_session_lock);
    object_class_property_add_str(oc, "sd-wiring", gnw_h7b0_get_sd_wiring,
                                 gnw_h7b0_set_sd_wiring);
    object_class_property_add_bool(oc, "power-model", gnw_h7b0_get_power_model,
                                  gnw_h7b0_set_power_model);
    object_class_property_add_bool(oc, "usb-power", gnw_h7b0_get_usb,
                                  gnw_h7b0_set_usb);
    object_class_property_add(oc, "battery", "uint32", gnw_h7b0_get_battery,
                             gnw_h7b0_set_battery, NULL, NULL);
    object_class_property_add(oc, "battery-life-min", "uint32",
                             gnw_h7b0_get_battery_life_min,
                             gnw_h7b0_set_battery_life_min, NULL, NULL);
    object_class_property_add(oc, "charge-time-min", "uint32",
                             gnw_h7b0_get_charge_time_min,
                             gnw_h7b0_set_charge_time_min, NULL, NULL);
}

static void gnw_h7b0_machine_instance_init(Object *obj)
{
    GNW_H7B0_MACHINE(obj)->session_lock_fd = -1;
    GNW_H7B0_MACHINE(obj)->sd_wiring = g_strdup("spi1");
    GNW_H7B0_MACHINE(obj)->power.battery = 100;
    GNW_H7B0_MACHINE(obj)->power.level = 100;
    GNW_H7B0_MACHINE(obj)->power.battery_life_min = 480;
    GNW_H7B0_MACHINE(obj)->power.charge_time_min = 180;
}

static void gnw_h7b0_machine_finalize(Object *obj)
{
    GnwH7B0MachineState *s = GNW_H7B0_MACHINE(obj);
    g_free(s->frame_endpoint);
    g_free(s->session_lock);
    g_free(s->sd_wiring);
    gnw_h7b0_power_free(&s->power);
    if (s->session_lock_fd >= 0) {
        close(s->session_lock_fd);
    }
}

static const TypeInfo gnw_h7b0_machine_info = {
    .name = TYPE_GNW_H7B0_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(GnwH7B0MachineState),
    .instance_init = gnw_h7b0_machine_instance_init,
    .instance_finalize = gnw_h7b0_machine_finalize,
    .class_init = gnw_h7b0_machine_class_init,
    .interfaces = arm_machine_interfaces,
};

static void gnw_h7b0_register_machine(void)
{
    type_register_static(&gnw_h7b0_machine_info);
}

type_init(gnw_h7b0_register_machine)
