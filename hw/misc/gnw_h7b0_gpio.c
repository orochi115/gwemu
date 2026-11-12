/*
 * STM32H7B0 GPIO minimal stub (Nintendo Game & Watch)
 *
 * See gnw_h7b0_gpio.h for scope/rationale.
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
#include "hw/core/cpu.h"
#include "qemu/timer.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "qapi/util.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "ui/console.h"
#include "ui/input.h"
#include "system/system.h"
#include "hw/misc/gnw_h7b0_gpio.h"
#include "hw/misc/gnw_timeline.h"
#include "hw/misc/gnw_h7b0_regs_gpio.h"
#include "hw/misc/gnw_h7b0_exti.h"
#include "hw/misc/gnw_h7b0_syscfg.h"
#include "hw/misc/gnw_h7b0_regs_syscfg.h"

/*
 * Button -> port/pin table, transcribed from gnw-chainloader's own
 * board.h pin definitions (ground truth for what real firmware wires
 * up), not re-derived from the SVD. Port letters map to the alphabetical
 * port index baked into GNW_H7B0_GPIO_SIZE's layout (A=0, B=1, C=2, D=3).
 */
enum {
    GNW_BTN_PAUSE,
    GNW_BTN_GAME,
    GNW_BTN_TIME,
    GNW_BTN_A,
    GNW_BTN_B,
    GNW_BTN_LEFT,
    GNW_BTN_DOWN,
    GNW_BTN_RIGHT,
    GNW_BTN_UP,
    GNW_BTN_PWR,
    GNW_BTN_START,
    GNW_BTN_SELECT,
    GNW_BTN__COUNT,
};

/*
 * One physical board pinout, shared by every firmware. The working
 * Retro-Go board definitions and charger driver identify GAME=PC1,
 * PAUSE=PC13, TIME=PC5, and charger PGOOD=PA2. Inputs are active-low;
 * the emulator keeps PGOOD asserted as the device has no battery/charger
 * UI. In particular, TIME must never drive PA2: that is the charger's
 * power-good input, and doing so makes a TIME press look like a power
 * connection. pin2 == 0 means "no second wiring".
 *
 * PWR on PA0/WKUP1 is wakeup-capable and single-wired.
 */
typedef struct GnwButtonPin {
    int port;
    uint32_t pin;
    int port2;
    uint32_t pin2;
} GnwButtonPin;

static const GnwButtonPin gnw_h7b0_button_pins[GNW_BTN__COUNT] = {
    [GNW_BTN_PAUSE]  = { 2, 1U << 13 },
    [GNW_BTN_GAME]   = { 2, 1U << 1  },
    [GNW_BTN_TIME]   = { 2, 1U << 5 },
    [GNW_BTN_A]      = { 3, 1U << 9  },
    [GNW_BTN_B]      = { 3, 1U << 5  },
    [GNW_BTN_LEFT]   = { 3, 1U << 11 },
    [GNW_BTN_DOWN]   = { 3, 1U << 14 },
    [GNW_BTN_RIGHT]  = { 3, 1U << 15 },
    [GNW_BTN_UP]     = { 3, 1U << 0  },
    [GNW_BTN_PWR]    = { 0, 1U << 0  },
    [GNW_BTN_START]  = { 2, 1U << 11 },
    [GNW_BTN_SELECT] = { 2, 1U << 12 },
};

#define GNW_UNLOCK_PAYLOAD_SIZE 1288
#define GNW_UNLOCK_ENTRY_OFFSET 0x4a0

typedef struct GnwUnlockHandoff {
    GnwH7B0GpioState *gpio;
    uint64_t epoch;
    uint8_t *itcm;
    size_t itcm_offset;
    uint8_t payload[GNW_UNLOCK_PAYLOAD_SIZE];
} GnwUnlockHandoff;

static void gnw_h7b0_gpio_run_unlock_payload(CPUState *cpu,
                                             run_on_cpu_data data)
{
    GnwUnlockHandoff *handoff = data.host_ptr;

    if (handoff->epoch == handoff->gpio->handoff_epoch) {
        memcpy(handoff->itcm + handoff->itcm_offset, handoff->payload,
               sizeof(handoff->payload));
        cpu_set_pc(cpu, (handoff->itcm_offset + GNW_UNLOCK_ENTRY_OFFSET) | 1);
        cpu_exit(cpu);
    }
    handoff->gpio->pending_handoffs--;
    g_free(handoff);
}

static void handoff_barrier(CPUState *cpu, run_on_cpu_data data)
{
    /* FIFO CPU work queue: all earlier handoffs have completed. */
}

void gnw_h7b0_gpio_snapshot_quiesce(GnwH7B0GpioState *s, bool discard)
{
    if (discard) {
        s->handoff_epoch++;
    }
    if (s->pending_handoffs) {
        run_on_cpu(s->cpu, handoff_barrier, RUN_ON_CPU_NULL);
    }
}

/*
 * Keyboard mapping. Arrow keys for the d-pad are the obvious choice;
 * A/B follow the Z/X convention most emulators default to (Z=B, X=A,
 * left-to-right mirroring the physical button layout on real
 * controllers). Enter/right-shift for Start/Select mirror the same
 * emulator convention. GAME/TIME/PWR get first-letter mnemonics since
 * they're G&W-specific and have no existing convention to borrow;
 * Escape for Pause reads naturally as "pause/menu" on any keyboard.
 */
static const int gnw_h7b0_key_map[GNW_BTN__COUNT] = {
    [GNW_BTN_PAUSE]  = Q_KEY_CODE_ESC,
    [GNW_BTN_GAME]   = Q_KEY_CODE_G,
    [GNW_BTN_TIME]   = Q_KEY_CODE_T,
    [GNW_BTN_A]      = Q_KEY_CODE_X,
    [GNW_BTN_B]      = Q_KEY_CODE_Z,
    [GNW_BTN_LEFT]   = Q_KEY_CODE_LEFT,
    [GNW_BTN_DOWN]   = Q_KEY_CODE_DOWN,
    [GNW_BTN_RIGHT]  = Q_KEY_CODE_RIGHT,
    [GNW_BTN_UP]     = Q_KEY_CODE_UP,
    [GNW_BTN_PWR]    = Q_KEY_CODE_P,
    [GNW_BTN_START]  = Q_KEY_CODE_RET,
    [GNW_BTN_SELECT] = Q_KEY_CODE_SHIFT_R,
};

/*
 * Deliberately no pointer/mouse-button mapping. An earlier best-effort
 * mapping (left/middle/wheel -> A/START/d-pad, intended as a
 * placeholder for a real joystick backend this QEMU base doesn't have)
 * turned out to be actively harmful with the SDL display: ordinary
 * window interaction silently pressed game buttons (clicking to focus
 * = A, wheel = d-pad), and a click whose release was swallowed by a
 * focus change left a button latched pressed forever -- observed live
 * as START stuck low (game word 0x10) making the OFW menu ignore all
 * further input. Keyboard (INPUT_EVENT_MASK_KEY) is the only input
 * source; real gamepads need host-side key translation.
 */
static void gnw_h7b0_gpio_set_pin(GnwH7B0GpioState *s, int port,
                                   uint32_t pin, bool pressed)
{
    hwaddr idr = port * GNW_H7B0_GPIO_PORT_SIZE + GNW_H7B0_GPIO_IDR_OFFSET;

    /* Active-low: pressed clears the bit, released sets it back. */
    if (pressed) {
        s->regs[idr >> 2] &= ~pin;
    } else {
        s->regs[idr >> 2] |= pin;
    }

    /*
     * Real hardware feeds every GPIO pin's level into its EXTI line
     * (line N = pin N), gated by SYSCFG_EXTICRx's per-line port mux.
     * Stock/CFW firmware waits for button presses via EXTI interrupts
     * (e.g. the boot screen's "press POWER" wait), not just IDR polling,
     * so the level change must be forwarded or interrupt-driven button
     * waits never wake. Only forward when EXTICR actually routes this
     * line to this button's port, since several buttons share a line
     * number across ports.
     */
    if (s->exti && s->syscfg) {
        int line = ctz32(pin);
        uint32_t exticr = s->syscfg->regs[(GNW_H7B0_SYSCFG_EXTICR1_OFFSET
                                            + (line / 4) * 4) >> 2];
        int port_sel = (exticr >> ((line % 4) * 4)) & 0xF;

        if (port_sel == port) {
            /* EXTI line level follows the (active-low) pin level. */
            gnw_h7b0_exti_set_line(s->exti, line, !pressed);
        }
    }
}

void gnw_h7b0_gpio_set_power(GnwH7B0GpioState *s, bool usb, bool charging)
{
    s->power_input = true;
    s->usb_power = usb;
    s->charging = charging;
    gnw_h7b0_gpio_set_pin(s, 0, 1u << 2, usb);
    gnw_h7b0_gpio_set_pin(s, 4, 1u << 7, charging);
}

static bool gnw_h7b0_gpio_try_unlock_handoff(GnwH7B0GpioState *s,
                                             bool power_edge,
                                             bool *stock_itcm_ready)
{
    static const uint8_t unlock_start[] = {
        0x72, 0xb6, 0x12, 0x48, 0x00, 0x21, 0x01, 0x60,
        0x11, 0x49, 0x81, 0x61, 0x11, 0x48, 0x00, 0x21,
    };
    static const struct {
        size_t itcm_offset;
        size_t extflash_offset;
        const char *stock_itcm_sha1;
    } models[] = {
        { 0, 0, "ca71a54c0a22cca5c6ee129faee9f99f3a346ca0" },
        { 0x20, 0x30c3a8, "2f70156235ffd871599facf64457040d549353b4" },
    };
    uint8_t decoded[GNW_UNLOCK_PAYLOAD_SIZE];

    *stock_itcm_ready = false;
    if (!s->cpu || !s->itcm || !s->rdp_value || *s->rdp_value != 0x55) {
        *stock_itcm_ready = true;
        return false;
    }

    for (size_t model = 0; model < ARRAY_SIZE(models); model++) {
        size_t itcm_offset = models[model].itcm_offset;
        size_t extflash_offset = models[model].extflash_offset;
        const uint8_t *payload = s->itcm + itcm_offset;
        bool payload_in_itcm;

        if (itcm_offset + GNW_UNLOCK_PAYLOAD_SIZE > s->itcm_size) {
            continue;
        }
        payload_in_itcm = memcmp(payload + GNW_UNLOCK_ENTRY_OFFSET,
                                 unlock_start, sizeof(unlock_start)) == 0;
        if (!payload_in_itcm) {
            char *sha1 = g_compute_checksum_for_data(
                G_CHECKSUM_SHA1, payload, 1300);
            bool matches = g_str_equal(sha1, models[model].stock_itcm_sha1);
            g_free(sha1);
            if (!matches) {
                continue;
            }
            *stock_itcm_ready = true;
            if (!s->extflash || !s->extflash_baseline ||
                extflash_offset + sizeof(decoded) > s->extflash_size ||
                extflash_offset + sizeof(decoded) >
                    s->extflash_baseline_size) {
                return false;
            }
            for (size_t i = 0; i < sizeof(decoded); i++) {
                decoded[i] = s->extflash[extflash_offset + i] ^
                             s->itcm[itcm_offset + i] ^
                             s->extflash_baseline[extflash_offset + i];
            }
            payload = decoded;
        } else {
            *stock_itcm_ready = true;
        }

        if (memcmp(payload + GNW_UNLOCK_ENTRY_OFFSET, unlock_start,
                   sizeof(unlock_start)) == 0) {
            GnwUnlockHandoff *handoff = g_new(GnwUnlockHandoff, 1);

            handoff->itcm = s->itcm;
            handoff->gpio = s;
            handoff->epoch = s->handoff_epoch;
            s->pending_handoffs++;
            handoff->itcm_offset = itcm_offset;
            memcpy(handoff->payload, payload, sizeof(handoff->payload));
            s->unlock_handoff_active = power_edge;
            async_run_on_cpu(s->cpu, gnw_h7b0_gpio_run_unlock_payload,
                             RUN_ON_CPU_HOST_PTR(handoff));
            return true;
        }
    }
    return false;
}

bool gnw_h7b0_gpio_try_boot_handoff(GnwH7B0GpioState *s,
                                    bool *stock_itcm_ready)
{
    return gnw_h7b0_gpio_try_unlock_handoff(s, false, stock_itcm_ready);
}

static const char *const gnw_h7b0_button_names[GNW_BTN__COUNT] = {
    [GNW_BTN_PAUSE]  = "pause",
    [GNW_BTN_GAME]   = "game",
    [GNW_BTN_TIME]   = "time",
    [GNW_BTN_A]      = "a",
    [GNW_BTN_B]      = "b",
    [GNW_BTN_LEFT]   = "left",
    [GNW_BTN_DOWN]   = "down",
    [GNW_BTN_RIGHT]  = "right",
    [GNW_BTN_UP]     = "up",
    [GNW_BTN_PWR]    = "pwr",
    [GNW_BTN_START]  = "start",
    [GNW_BTN_SELECT] = "select",
};

int gnw_h7b0_gpio_button_from_name(const char *name)
{
    for (int i = 0; i < GNW_BTN__COUNT; i++) {
        if (gnw_h7b0_button_names[i] && !g_ascii_strcasecmp(name, gnw_h7b0_button_names[i])) {
            return i;
        }
    }
    return -1;
}

static void gnw_h7b0_gpio_set_button(GnwH7B0GpioState *s, int button,
                                      bool pressed)
{
    const GnwButtonPin *bp = &gnw_h7b0_button_pins[button];

    if (button == GNW_BTN_PWR && s->unlock_handoff_active) {
        /* The release edge belongs to the cold-start control and must not
         * interrupt the payload after the handoff has begun. */
        if (!pressed) {
            s->unlock_handoff_active = false;
        }
        return;
    }

    /* The physical POWER edge enters the same verified boot-ROM handoff
     * model used during initial VM startup. */
    if (button == GNW_BTN_PWR && pressed) {
        bool stock_itcm_ready;

        if (gnw_h7b0_gpio_try_unlock_handoff(s, true,
                                              &stock_itcm_ready)) {
            return;
        }
    }

    gnw_h7b0_gpio_set_pin(s, bp->port, bp->pin, pressed);
    if (bp->pin2) {
        gnw_h7b0_gpio_set_pin(s, bp->port2, bp->pin2, pressed);
    }
}

/* Non-static entry point for the timeline engine (gnw_timeline.c). */
void gnw_h7b0_gpio_inject_button(GnwH7B0GpioState *s, int button,
                                 bool pressed)
{
    if (button >= 0 && button < GNW_BTN__COUNT) {
        gnw_h7b0_gpio_set_button(s, button, pressed);
    }
}

void gnw_h7b0_gpio_set_unlock_handoff(GnwH7B0GpioState *s, CPUState *cpu,
                                      uint8_t *itcm, size_t itcm_size,
                                      uint8_t *extflash, size_t extflash_size,
                                      uint8_t *extflash_baseline,
                                      size_t extflash_baseline_size,
                                      uint8_t *rdp_value)
{
    s->cpu = cpu;
    s->itcm = itcm;
    s->itcm_size = itcm_size;
    s->extflash = extflash;
    s->extflash_size = extflash_size;
    s->extflash_baseline = extflash_baseline;
    s->extflash_baseline_size = extflash_baseline_size;
    s->rdp_value = rdp_value;
}

static FILE *rec_file = NULL;
static bool rec_inited = false;

static bool G_GNUC_PRINTF(1, 2)
gnw_timeline_record_line(const char *format, ...)
{
    va_list ap;
    int written;

    if (!rec_file) {
        return false;
    }

    va_start(ap, format);
    written = vfprintf(rec_file, format, ap);
    va_end(ap);
    if (written < 0 || fflush(rec_file) != 0 ||
        qemu_fdatasync(fileno(rec_file)) < 0) {
        error_report("gnw-gpio: timeline recording write failed: %s",
                     g_strerror(errno));
        fclose(rec_file);
        rec_file = NULL;
        return false;
    }
    return true;
}

void gnw_timeline_record_quit(void)
{
    if (rec_file) {
        double t = (double)qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000000.0;
        gnw_timeline_record_line("%.3f quit\n", t + 0.5);
    }
    if (rec_file) {
        fclose(rec_file);
        rec_file = NULL;
    }
}

static void gnw_h7b0_gpio_input_event(DeviceState *dev, QemuConsole *src,
                                       InputEvent *evt)
{
    GnwH7B0GpioState *s = GNW_H7B0_GPIO(dev);

    if (!rec_inited) {
        const char *rec_path = getenv("GNW_TIMELINE_RECORD");
        if (rec_path) {
            rec_file = fopen(rec_path, "w");
            if (!rec_file) {
                error_report("gnw-gpio: cannot open GNW_TIMELINE_RECORD=%s", rec_path);
            } else if (setvbuf(rec_file, NULL, _IONBF, 0) != 0) {
                error_report("gnw-gpio: cannot disable buffering for "
                             "GNW_TIMELINE_RECORD=%s", rec_path);
                fclose(rec_file);
                rec_file = NULL;
            } else {
                /* Keep every write visible even if QEMU crashes. */
                gnw_timeline_record_line(
                    "# Automatically recorded timeline\n");
            }
        }
        rec_inited = true;
    }

    switch (evt->type) {
    case INPUT_EVENT_KIND_KEY: {
        InputKeyEvent *key = evt->u.key.data;
        int qcode = qemu_input_key_value_to_qcode(key->key);

        for (int i = 0; i < GNW_BTN__COUNT; i++) {
            if (s->key_map[i] == qcode) {
                if (s->host_buttons[i] == key->down) {
                    /* Ignore key repeat */
                    break;
                }
                s->host_buttons[i] = key->down;
                gnw_h7b0_gpio_set_button(s, i, key->down);
                if (rec_file) {
                    double t = (double)qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000000.0;
                    gnw_timeline_record_line(
                        "%.3f %s %s\n", t,
                        key->down ? "down" : "release",
                        gnw_h7b0_button_names[i]);
                }
                break;
            }
        }
        break;
    }
    default:
        break;
    }
}

static const QemuInputHandler gnw_h7b0_gpio_input_handler = {
    .name = "gnw-h7b0 buttons",
    .mask = INPUT_EVENT_MASK_KEY,
    .event = gnw_h7b0_gpio_input_event,
};

static void gnw_h7b0_gpio_reset(DeviceState *dev)
{
    GnwH7B0GpioState *s = GNW_H7B0_GPIO(dev);

    s->handoff_epoch++;
    memset(s->host_buttons, 0, sizeof(s->host_buttons));

    gnw_h7b0_softspi_reset(&s->softspi);

    for (int i = 0; i < (GNW_H7B0_GPIO_SIZE / 4); i++) {
        uint32_t port_offset = (i * 4) % GNW_H7B0_GPIO_PORT_SIZE;
        s->regs[i] = get_gpio_reset_value(port_offset);
    }
    for (int port = 0; port < GNW_H7B0_GPIO_NUM_PORTS; port++) {
        hwaddr idr = port * GNW_H7B0_GPIO_PORT_SIZE + GNW_H7B0_GPIO_IDR_OFFSET;
        s->regs[idr >> 2] = 0xFFFFU;
    }

    /*
     * Arm gnw_h7b0_gpio_pa0_release() (see its own doc comment) -- this
     * timer existed since this device's introduction but was never
     * actually scheduled anywhere, so the EXTI0 rising edge it's meant to
     * generate never happened under QEMU. Real-hardware live tracing
     * (2026-07-19, live watchpoint+breakpoint capture against stock Mario
     * firmware) confirmed the boot-stall this was meant to unblock is a
     * genuine EXTI0-IRQn(6)-triggered handler (xPSR active-exception
     * field decoded live as 22 = 16+6 = EXTI0), landing in a function
     * that arms the main superloop's countdown-enable flag -- i.e. real
     * firmware's boot sequence genuinely waits on this exact interrupt,
     * confirmed by the IPSR value captured at the live breakpoint, not
     * inferred from static analysis alone.
     *
     * Does NOT force PA0's IDR bit low here to manufacture the edge from
     * a level change (that would repeat the PC8/PC13/PD0 mistake this
     * comment block already warns about -- an unverified guess about a
     * pin's real electrical state). Unnecessary anyway:
     * gnw_h7b0_exti_set_line()'s edge detection compares against EXTI's
     * own line_level[] state, which is independently memset to all-false
     * in gnw_h7b0_exti_reset() -- so the first call from
     * gnw_h7b0_gpio_pa0_release() (level=true) is already a genuine
     * false->true rising edge from EXTI's point of view, regardless of
     * what GPIOA_IDR itself reads.
     *
     * Delay value is an unverified-but-reasonable default (long enough
     * for firmware to get through early clock/GPIO/EXTI-trigger-config
     * init before the edge arrives, short enough not to meaningfully
     * delay boot) -- not a real-hardware-measured button-hold duration.
     * Revisit with a real measurement if boot timing ever turns out to
     * be sensitive to the exact value.
     */
    timer_mod(s->pa0_release_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + GNW_H7B0_GPIO_PA0_RELEASE_DELAY_MS);

    /*
     * PC8, PC13, and PD0 were previously forced low here as unverified
     * guesses to unblock various boot-hang hypotheses (see CHANGELOG.md /
     * git blame for the full charger-retry-counter and write-storm
     * history). Confirmed wrong by direct real-hardware register reads
     * during the 2026-07-12 breakpoint-lockstep-tracing session:
     * GPIOC_IDR and GPIOD_IDR both read 0xFFFFFFFF on real hardware at
     * reset, i.e. every pin including these three is genuinely high, not
     * low. Real hardware's own stock-firmware boot state machine
     * (FUN_0800ec7a's `(*GPIOC_IDR bit13) && ...` gate, checkpoint-traced
     * this session) depends on PC13 reading high to proceed past state 6
     * into LTDC/graphics init -- forcing it low was actively blocking the
     * exact boot progress this project needs. No longer forced low at
     * reset. If removing these reintroduces the previously-seen
     * "massive-write-storm hang", that is a separate, real bug to find
     * and fix on its own terms, not a reason to reintroduce readings that
     * contradict measured real hardware.
     */

    /* BQ24072 PGOOD is active-low PA2 on the shared board pinout. */
    s->regs[(0 * GNW_H7B0_GPIO_PORT_SIZE + GNW_H7B0_GPIO_IDR_OFFSET) >> 2] &= ~(1u << 2);
    if (s->power_input) {
        gnw_h7b0_gpio_set_power(s, s->usb_power, s->charging);
    }
}

static uint64_t gnw_h7b0_gpio_read(void *opaque, hwaddr addr,
                                    unsigned int size)
{
    GnwH7B0GpioState *s = GNW_H7B0_GPIO(opaque);
    hwaddr reg = addr & ~3ULL;
    unsigned int shift = (addr & 3) * 8;
    uint32_t value;

    if (addr + size > GNW_H7B0_GPIO_SIZE || shift + size * 8 > 32) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return 0;
    }
    value = s->regs[reg >> 2];

    if (s->softspi.bus &&
        reg == 3 * GNW_H7B0_GPIO_PORT_SIZE + GNW_H7B0_GPIO_IDR_OFFSET &&
        ((s->regs[(3 * GNW_H7B0_GPIO_PORT_SIZE +
                    GNW_H7B0_GPIO_MODER_OFFSET) >> 2] >> 24) & 3) == 0) {
        value = deposit32(value, 12, 1, s->softspi.miso);
    }

    /* PC8 senses the shared board revision. With its digital input enabled,
     * the PC5-TIME board presents a low level. Keep the existing reset/analog
     * readback and general-purpose output loopback intact. This is a board
     * input, independent of the guest firmware and the button mapping. */
    if (reg == 2 * GNW_H7B0_GPIO_PORT_SIZE + GNW_H7B0_GPIO_IDR_OFFSET) {
        uint32_t moder = s->regs[(2 * GNW_H7B0_GPIO_PORT_SIZE +
                                  GNW_H7B0_GPIO_MODER_OFFSET) >> 2];

        if (((moder >> 16) & 3U) == 0) {
            value &= ~(1U << 8);
        }
    }
    return extract32(value, shift, size * 8);
}

/*
 * Push-pull GPIO output loopback: for every pin in this port currently
 * configured as general-purpose output (MODER == 01), IDR reads back
 * exactly what ODR is driving -- real hardware behavior for a push-pull
 * output stage, confirmed against stm32h7b0-diag's gpio_output_readback
 * case (writes a pin's own current level back via BSRR, expects IDR to
 * still read that same level afterward). Pins in any other mode
 * (input/AF/analog) are left untouched -- their IDR bits are driven by
 * gnw_h7b0_gpio_set_pin()'s button-input events instead, not by this
 * function.
 */
static void gnw_h7b0_gpio_sync_output_idr(GnwH7B0GpioState *s, int port)
{
    hwaddr moder_addr = (hwaddr)port * GNW_H7B0_GPIO_PORT_SIZE + GNW_H7B0_GPIO_MODER_OFFSET;
    hwaddr odr_addr = (hwaddr)port * GNW_H7B0_GPIO_PORT_SIZE + GNW_H7B0_GPIO_ODR_OFFSET;
    hwaddr idr_addr = (hwaddr)port * GNW_H7B0_GPIO_PORT_SIZE + GNW_H7B0_GPIO_IDR_OFFSET;
    uint32_t moder = s->regs[moder_addr >> 2];
    uint32_t odr = s->regs[odr_addr >> 2];
    uint32_t idr = s->regs[idr_addr >> 2];

    for (int pin = 0; pin < 16; pin++) {
        if (((moder >> (pin * 2)) & 0x3u) != 0x1u) {
            continue; /* not general-purpose output */
        }
        if (odr & (1u << pin)) {
            idr |= (1u << pin);
        } else {
            idr &= ~(1u << pin);
        }
    }
    s->regs[idr_addr >> 2] = idr;
}

static void gnw_h7b0_gpio_write(void *opaque, hwaddr addr,
                                 uint64_t val64, unsigned int size)
{
    GnwH7B0GpioState *s = GNW_H7B0_GPIO(opaque);
    hwaddr reg = addr & ~3ULL;
    unsigned int shift = (addr & 3) * 8;
    uint32_t value;

    if (addr + size > GNW_H7B0_GPIO_SIZE || shift + size * 8 > 32) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return;
    }

    uint32_t port_offset = reg % GNW_H7B0_GPIO_PORT_SIZE;
    uint32_t mask = get_gpio_write_mask(port_offset);
    value = size == 4 ? val64 :
        deposit32(s->regs[reg >> 2], shift, size * 8, val64);
    s->regs[reg >> 2] = (s->regs[reg >> 2] & ~mask) | (value & mask);
    if (port_offset == GNW_H7B0_GPIO_BSRR_OFFSET) {
        /*
         * BUG FIX: BSRR was stored into its own register word and
         * immediately zeroed (correctly emulating "write-only, reads as
         * 0"), but the set/reset semantics were never translated into
         * ODR at all -- the write was simply discarded. Real hardware:
         * bits[15:0] set the corresponding ODR bit, bits[31:16] reset
         * it, with set taking priority over reset for the same pin (RM
         * BSRR semantics). Found via stm32h7b0-diag's
         * gpio_output_readback case, which writes a pin's own current
         * level back through BSRR and expects ODR (and IDR, see
         * gnw_h7b0_gpio_sync_output_idr()) to still reflect it
         * afterward -- previously ODR just never changed.
         */
        uint32_t command = (uint32_t)val64 << shift;
        uint32_t set_bits = command & 0xFFFFu;
        uint32_t reset_bits = (command >> 16) & 0xFFFFu;
        hwaddr odr_addr = (reg - port_offset) + GNW_H7B0_GPIO_ODR_OFFSET;
        s->regs[odr_addr >> 2] = (s->regs[odr_addr >> 2] | set_bits) & ~reset_bits;
        s->regs[reg >> 2] = 0;
    }
    if (port_offset == GNW_H7B0_GPIO_MODER_OFFSET ||
        port_offset == GNW_H7B0_GPIO_ODR_OFFSET ||
        port_offset == GNW_H7B0_GPIO_BSRR_OFFSET) {
        int port = (int)(reg / GNW_H7B0_GPIO_PORT_SIZE);
        gnw_h7b0_gpio_sync_output_idr(s, port);
        if (port == 1 || port == 4) {
            gnw_h7b0_softspi_update(&s->softspi, s->regs);
        }
    }
}

static const MemoryRegionOps gnw_h7b0_gpio_ops = {
    .read = gnw_h7b0_gpio_read,
    .write = gnw_h7b0_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* See PA0's comment in gnw_h7b0_gpio_reset() -- releases PA0 (WKUP1/power
 * button, best guess) back to its normal-high idle level a short while
 * after reset, generating a real EXTI0 rising-edge interrupt in the
 * process (if firmware has configured EXTI0 for a rising trigger by
 * then; a no-op otherwise, same as real hardware). */
static void gnw_h7b0_gpio_pa0_release(void *opaque)
{
    GnwH7B0GpioState *s = GNW_H7B0_GPIO(opaque);

    s->regs[(0 * GNW_H7B0_GPIO_PORT_SIZE + GNW_H7B0_GPIO_IDR_OFFSET) >> 2] |= (1u << 0);
    if (s->exti) {
        gnw_h7b0_exti_set_line(s->exti, 0, true);
    }
}

static void gnw_h7b0_gpio_init(Object *obj)
{
    GnwH7B0GpioState *s = GNW_H7B0_GPIO(obj);

    memory_region_init_io(&s->mmio, obj, &gnw_h7b0_gpio_ops, s,
                           TYPE_GNW_H7B0_GPIO, GNW_H7B0_GPIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    s->pa0_release_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                         gnw_h7b0_gpio_pa0_release, s);
}


/*
 * Env-gated scripted input for automated testing: GNW_AUTO_INPUT=
 * "10:time,14:game,22:a" presses each named button at t seconds
 * (virtual clock) for 200ms; an optional third field ("0:game:3")
 * overrides the hold duration in seconds (for boot-time bank-select
 * combos that need a button held down through early firmware polls). Lets an unattended run drive stock
 * firmware from the "PRESS TIME BUTTON" splash into gameplay (note:
 * the Mario unit has no START button -- use "a" to start a game).
 * Inert unless the env var is set.
 */
typedef struct GnwAutoEvent {
    int64_t ns;
    int btn;
    bool down;
} GnwAutoEvent;

static GnwAutoEvent auto_events[64];
static int auto_nevents, auto_next;
static QEMUTimer *auto_timer;
static void *auto_gpio;

static void gnw_h7b0_gpio_auto_cb(void *opaque)
{
    GnwH7B0GpioState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    while (auto_next < auto_nevents && auto_events[auto_next].ns <= now) {
        fprintf(stderr, "AUTOBTN %d %d %" PRId64 "\n",
                auto_events[auto_next].btn, auto_events[auto_next].down, now);
        gnw_h7b0_gpio_set_button(s, auto_events[auto_next].btn,
                                  auto_events[auto_next].down);
        auto_next++;
    }
    if (auto_next < auto_nevents) {
        timer_mod(auto_timer, auto_events[auto_next].ns);
    }
}

static void gnw_h7b0_gpio_realize(DeviceState *dev, Error **errp)
{
    GnwH7B0GpioState *s = GNW_H7B0_GPIO(dev);

    const char *auto_input = getenv("GNW_AUTO_INPUT");
    if (auto_input && !auto_timer) {
        g_auto(GStrv) evs = g_strsplit(auto_input, ",", -1);
        for (char **p = evs; *p && auto_nevents < 62; p++) {
            double t;
            double hold = 0.2;
            char name[16];
            if (sscanf(*p, "%lf:%15[^:]:%lf", &t, name, &hold) >= 2) {
                int btn = gnw_h7b0_gpio_button_from_name(name);
                if (btn >= 0) {
                    auto_events[auto_nevents++] = (GnwAutoEvent){
                        (int64_t)(t * 1e9), btn, true };
                    auto_events[auto_nevents++] = (GnwAutoEvent){
                        (int64_t)((t + hold) * 1e9), btn, false };
                }
            }
        }
        if (auto_nevents) {
            /* Long holds can make press/release events interleave out of
             * order across buttons; the dispatch walk needs ascending ns. */
            for (int i = 1; i < auto_nevents; i++) {
                GnwAutoEvent e = auto_events[i];
                int j = i - 1;
                while (j >= 0 && auto_events[j].ns > e.ns) {
                    auto_events[j + 1] = auto_events[j];
                    j--;
                }
                auto_events[j + 1] = e;
            }
            auto_gpio = s;
            auto_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                       gnw_h7b0_gpio_auto_cb, s);
            timer_mod(auto_timer, auto_events[0].ns);
        }
    }

    for (int i = 0; i < GNW_BTN__COUNT; i++) {
        s->key_map[i] = gnw_h7b0_key_map[i];
    }

    if (s->keymap) {
        g_auto(GStrv) pairs = g_strsplit(s->keymap, ",", -1);

        for (char **p = pairs; *p; p++) {
            g_auto(GStrv) kv = g_strsplit(g_strstrip(*p), "=", 2);
            int btn = -1, qcode;

            if (!kv[0] || !kv[1]) {
                error_setg(errp, "keymap: bad entry '%s' "
                           "(want button=key)", *p);
                return;
            }
            for (int i = 0; i < GNW_BTN__COUNT; i++) {
                if (!g_ascii_strcasecmp(kv[0], gnw_h7b0_button_names[i])) {
                    btn = i;
                    break;
                }
            }
            if (btn < 0) {
                error_setg(errp, "keymap: unknown button '%s'", kv[0]);
                return;
            }
            qcode = qapi_enum_parse(&QKeyCode_lookup, kv[1], -1, NULL);
            if (qcode < 0) {
                error_setg(errp, "keymap: unknown key '%s' "
                           "(use QKeyCode names, e.g. ret, spc, shift_r)",
                           kv[1]);
                return;
            }
            s->key_map[btn] = qcode;
        }
    }

    qemu_input_handler_register(dev, &gnw_h7b0_gpio_input_handler);

    gnw_timeline_init(s);
}

static int gpio_pre_save(void *opaque)
{
    GnwH7B0GpioState *s = opaque;

    s->pa0_remaining_ns = timer_pending(s->pa0_release_timer) ?
        MAX(timer_expire_time_ns(s->pa0_release_timer) -
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), 0) : -1;
    return 0;
}

static int gpio_post_load(void *opaque, int version_id)
{
    GnwH7B0GpioState *s = opaque;

    memset(s->host_buttons, 0, sizeof(s->host_buttons));
    if (version_id >= 3) {
        if (s->pa0_remaining_ns < -1) {
            return -EINVAL;
        }
        timer_del(s->pa0_release_timer);
        if (s->pa0_remaining_ns >= 0) {
            timer_mod(s->pa0_release_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + s->pa0_remaining_ns);
        }
    }
    return 0;
}

static const VMStateDescription vmstate_gnw_h7b0_gpio = {
    .name = TYPE_GNW_H7B0_GPIO,
    .version_id = 3,
    .minimum_version_id = 1,
    .pre_save = gpio_pre_save,
    .post_load = gpio_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, GnwH7B0GpioState, GNW_H7B0_GPIO_SIZE / 4),
        VMSTATE_STRUCT(softspi, GnwH7B0GpioState, 2,
                         vmstate_gnw_h7b0_softspi, GnwH7B0SoftSpi),
        VMSTATE_BOOL_V(power_input, GnwH7B0GpioState, 3),
        VMSTATE_BOOL_V(usb_power, GnwH7B0GpioState, 3),
        VMSTATE_BOOL_V(charging, GnwH7B0GpioState, 3),
        VMSTATE_BOOL_V(unlock_handoff_active, GnwH7B0GpioState, 3),
        VMSTATE_INT64_V(pa0_remaining_ns, GnwH7B0GpioState, 3),
        VMSTATE_END_OF_LIST()
    }
};

static const Property gnw_h7b0_gpio_properties[] = {
    DEFINE_PROP_STRING("keymap", GnwH7B0GpioState, keymap),
};

static void gnw_h7b0_gpio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_gnw_h7b0_gpio;
    dc->realize = gnw_h7b0_gpio_realize;
    device_class_set_props(dc, gnw_h7b0_gpio_properties);
    device_class_set_legacy_reset(dc, gnw_h7b0_gpio_reset);
}

static const TypeInfo gnw_h7b0_gpio_info = {
    .name          = TYPE_GNW_H7B0_GPIO,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GnwH7B0GpioState),
    .instance_init = gnw_h7b0_gpio_init,
    .class_init    = gnw_h7b0_gpio_class_init,
};

static void gnw_h7b0_gpio_register_types(void)
{
    type_register_static(&gnw_h7b0_gpio_info);
}

type_init(gnw_h7b0_gpio_register_types)
