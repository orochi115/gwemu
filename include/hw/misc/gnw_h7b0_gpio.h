/*
 * STM32H7B0 GPIO minimal stub (Nintendo Game & Watch)
 *
 * Not a real pin/electrical model -- no MODER-driven input/output
 * behavior, no ODR->IDR loopback for pins configured as outputs, no
 * button/LCD control-line side effects. Every register in every port
 * (A-K, 0x400 each) is a plain read/write shadow, *except* that IDR
 * (offset 0x10 within each port) resets to all-1s instead of the
 * generic RAM placeholder's all-0s.
 *
 * That one bit of behavior matters a lot: real G&W buttons are
 * active-low with external pull-ups (see gnw-chainloader's
 * board_check_button(): `(port->IDR & pin) == 0` means pressed), so a
 * zero-initialized IDR reads as "every button held" forever. This was
 * the previous plain-RAM GPIO placeholder's behavior, and it silently
 * forced gnw-chainloader's main.c app_early_logic() "God Mode" path
 * (hold_left || hold_right) on every single boot -- not a real
 * button press, not a QEMU input event, just an unpressed button
 * reading as pressed. Found by the user noticing gnw-chainloader
 * jumping straight to a "FLASHING..." OFW-install screen it had no
 * business reaching un-prompted, after gnw_h7b0_crc.c's real CRC unit
 * let that code path's checksum actually pass instead of failing
 * closed. Defaulting IDR high fixes the false-positive without
 * needing any real button-input plumbing yet.
 *
 * Button input plumbing (added this session): registers a
 * QemuInputHandler covering both keyboard and pointer-button events
 * and clears/sets the mapped port's IDR bit on press/release, per
 * board_check_button()'s `(port->IDR & pin) == 0` == pressed polarity.
 * See gnw_h7b0_gpio.c for the button->pin table and the keyboard/
 * pointer-button mapping, and its rationale comment for why "gamepad"
 * support here is best-effort: QEMU v9.2 (this project's pinned base)
 * has no SDL2 game-controller/joystick backend, so InputButton in this
 * tree is a mouse-button enum only -- there is no code path by which a
 * real xpad-style controller's face buttons reach a QEMU device today.
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

#ifndef HW_MISC_GNW_H7B0_GPIO_H
#define HW_MISC_GNW_H7B0_GPIO_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "hw/misc/gnw_h7b0_softspi.h"

#define TYPE_GNW_H7B0_GPIO "gnw-h7b0-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0GpioState, GNW_H7B0_GPIO)

/* Per-port layout, per STM32H7B0.svd's GPIOx peripherals (11 ports, A-K). */
#define GNW_H7B0_GPIO_PORT_SIZE   0x400
#define GNW_H7B0_GPIO_NUM_PORTS   11
#define GNW_H7B0_GPIO_SIZE        (GNW_H7B0_GPIO_NUM_PORTS * GNW_H7B0_GPIO_PORT_SIZE)

#define GNW_H7B0_GPIO_IDR_OFFSET  0x10

/* See gnw_h7b0_gpio_reset()'s doc comment on the pa0_release_timer arm
 * site: an unverified-but-reasonable default, not a real-hardware-
 * measured button-hold duration. */
#define GNW_H7B0_GPIO_PA0_RELEASE_DELAY_MS  50

typedef struct GnwH7B0ExtiState GnwH7B0ExtiState;
typedef struct GnwH7B0SyscfgState GnwH7B0SyscfgState;
typedef struct CPUState CPUState;

struct GnwH7B0GpioState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    uint32_t regs[GNW_H7B0_GPIO_SIZE / 4];
    bool power_input;
    bool usb_power;
    bool charging;
    GnwH7B0SoftSpi softspi;
    /* Set by the SoC after both devices exist (see gnw_h7b0_soc.c). Used
     * only to notify EXTI of the PA0 (WKUP1, best guess) release edge --
     * see gnw_h7b0_gpio_pa0_release() in gnw_h7b0_gpio.c. */
    GnwH7B0ExtiState *exti;
    /* Set by the SoC alongside exti. Needed to consult SYSCFG_EXTICRx
     * (the per-line port mux) so a button press only raises its EXTI
     * line when firmware actually routed that line to the button's
     * port -- several buttons share a line number across ports
     * (PA0/PD0 both line 0, PC5/PD5 line 5, PC11/PD11 line 11). */
    GnwH7B0SyscfgState *syscfg;
    /* Optional cold-boot unlock-payload handoff. The SoC connects the
     * CPU, ITCM bytes, and current RDP option byte after realization. */
    CPUState *cpu;
    uint8_t *itcm;
    uint8_t *extflash;
    uint8_t *extflash_baseline;
    uint8_t *rdp_value;
    size_t itcm_size;
    size_t extflash_size;
    size_t extflash_baseline_size;
    bool unlock_handoff_active;
    uint64_t handoff_epoch;
    unsigned pending_handoffs;
    bool host_buttons[16];
    /* Guest virtual input survives host cleanup and is migrated separately. */
    bool nonhost_buttons[16];
    int64_t pa0_remaining_ns;
    QEMUTimer *pa0_release_timer;
    /*
     * Optional keyboard-remap property ("keymap"): comma-separated
     * button=key pairs, e.g. "a=z,b=x,start=spc". Button names:
     * pause game time a b left down right up pwr start select.
     * Key names are QEMU QKeyCode names (the same names `-k help`-era
     * keymaps and qemu monitor sendkey use: ret, spc, shift_r, kp_0,
     * ...). Unlisted buttons keep their built-in defaults. Parsed at
     * realize into key_map[].
     */
    char *keymap;
    int key_map[16];
};

/* Timeline-engine entry points (hw/misc/gnw_timeline.c): resolve a
 * button name ("a", "game", ... -- case-insensitive) to its GNW_BTN_*
 * index (-1 if unknown), and set that button's pin state. */
int gnw_h7b0_gpio_button_from_name(const char *name);
void gnw_h7b0_gpio_inject_button(GnwH7B0GpioState *s, int button,
                                 bool pressed);
/* Release only keyboard/QMP-held keys; leave timeline/cold-start input alone. */
unsigned gnw_h7b0_gpio_release_host_input(GnwH7B0GpioState *s);
void gnw_h7b0_gpio_set_unlock_handoff(GnwH7B0GpioState *s, CPUState *cpu,
                                      uint8_t *itcm, size_t itcm_size,
                                      uint8_t *extflash, size_t extflash_size,
                                      uint8_t *extflash_baseline,
                                      size_t extflash_baseline_size,
                                      uint8_t *rdp_value);
bool gnw_h7b0_gpio_try_boot_handoff(GnwH7B0GpioState *s,
                                    bool *stock_itcm_ready);
void gnw_timeline_record_quit(void);
void gnw_h7b0_gpio_set_power(GnwH7B0GpioState *s, bool usb, bool charging);
void gnw_h7b0_gpio_snapshot_quiesce(GnwH7B0GpioState *s, bool discard);

#endif
