/*
 * STM32H7B0 LTDC minimal stub (Nintendo Game & Watch)
 *
 * Real device, not a full LTDC model, but composites both layers:
 * Layer1 (bottom/background) and Layer2 (top/foreground -- real STM32
 * LTDC hardware always shows Layer2 above Layer1; had this backwards
 * until 2026-07-13, which combined with the AL44 gap below to make
 * Mario/Zelda's GAME/PAUSE overlay menus fully invisible even once
 * their pixel data was being decoded correctly), with blending,
 * color-keying, and independent per-layer CLUTs all modeled (see
 * gnw_h7b0_ltdc_capture_rows()). RGB565/888, ARGB8888/1555/4444, AL88, L8
 * (flat 256-entry CLUT-indexed), and AL44 (4-bit alpha applied
 * directly + 4-bit luminance indexing a 16-entry CLUT sub-palette at
 * n*17 -- NOT flat-256-indexed like L8; used by Mario/Zelda's stock
 * firmware for anti-aliased overlay text, e.g. the GAME/PAUSE menus)
 * are drawn on both layers. PA4 digital/DAC1 output controls the
 * panel backlight at scanout publication. QEMU's own display-refresh timer drives redraws, not a
 * modeled per-pixel/per-line VSYNC scan.
 *
 * Line interrupt IS modeled, though, via a QEMU timer now paced by the
 * live PLL3R pixel clock and TWCR panel timing (see
 * gnw_h7b0_ltdc_recalc_timers()) rather than a plain fixed ~60Hz -- LTDC's
 * pixel clock is hardwired to pll3_r_ck (no clock-source mux like SAI1/
 * ADC have), and real firmware's PLL3 config computes to ~66.7Hz against
 * the real 392x255 panel timing, not 60Hz. GNW_H7B0_LTDC_VBLANK_HZ is now
 * only the defensive fallback for when RCC isn't wired or the computed
 * rate is nonsensical. Both
 * gnw-chainloader's gui_refresh() and retro-go-sd's
 * lcd_wait_for_vblank() (Core/Src/gw_lcd.c) __WFI()-busy-wait on a
 * frame counter that only HAL_LTDC_LineEventCallback() (the real LI
 * IRQ handler) increments -- which never fired without a real
 * interrupt. Found once gnw_h7b0_gpio.c's button fix got
 * gnw-chainloader far enough to reach its real menu (the flicker
 * reported there was a symptom of this same gap: gui_refresh()
 * grabbing an in-progress-redraw frame because it never actually
 * waited on anything) and once retro-go-sd's boot got far enough
 * (WWDG/TIM2-block fixes) to hang in lcd_wait_for_vblank() outright.
 * IER.LIE/RRIE gate the interrupt like real hardware; ISR/ICR are real
 * (not shadow) with W1C-on-ICR semantics. LIPCR is still a plain
 * shadow -- the timer fires unconditionally at a fixed rate once
 * enabled, not at the configured line position.
 *
 * SRCR's VBR (vertical-blank reload) now really defers the
 * CFBAR/CFBLR/CFBLNR/PFCR shadow->active copy to the next vblank tick
 * (see gnw_h7b0_ltdc_reload_active()/_vblank_tick()) instead of being
 * instant like IMR. That was the root cause of the menu-tearing/
 * flicker documented above: firmware's double-buffer flip (a VBR
 * reload) used to take effect the instant SRCR was written, so a host
 * redraw could catch a CFBAR pointing at a buffer firmware hadn't
 * actually finished drawing into yet.
 *
 * Purpose: get pixels from guest RAM onto a QEMU display surface via
 * the same MemoryRegionSection + framebuffer_update_display() pattern
 * used by hw/display/pl110.c (used as a structural template), not a
 * cycle/timing-accurate LCD-TFT controller. See docs/roadmap.md
 * (Phase 2/3 pulled forward -- LTDC was where a real gnw-chainloader
 * boot stopped after Phase 1's other peripheral gaps were closed).
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

#ifndef HW_DISPLAY_GNW_H7B0_LTDC_H
#define HW_DISPLAY_GNW_H7B0_LTDC_H

#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "system/memory.h"
#include "qemu/timer.h"
#include "qemu/thread.h"
#include "qom/object.h"
#include "hw/misc/gnw_h7b0_rcc.h"
#include "hw/misc/gnw_h7b0_gpio.h"
#include "hw/misc/gnw_h7b0_dac.h"

#define TYPE_GNW_H7B0_LTDC "gnw-h7b0-ltdc"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0LtdcState, GNW_H7B0_LTDC)

/*
 * Register offsets/bits below are from
 * sdk/cmsis-device-h7/Include/stm32h7b0xx.h's LTDC_TypeDef /
 * LTDC_Layer_TypeDef and LTDC_xxx_yyy bit definitions, cross-checked
 * against STM32H7B0.svd.
 */
#define GNW_H7B0_LTDC_SIZE   0x1000

#define GNW_H7B0_LTDC_GCR    0x18
#define LTDC_GCR_LTDCEN      (1U << 0)
/* DEN (dither enable): confirmed via STM32H7B0.svd this session, not
 * previously defined in this codebase. */
#define LTDC_GCR_DEN         (1U << 16)

#define GNW_H7B0_LTDC_SRCR   0x24
#define LTDC_SRCR_IMR        (1U << 0)
#define LTDC_SRCR_VBR        (1U << 1)

#define GNW_H7B0_LTDC_TWCR      0x14

#define GNW_H7B0_LTDC_BPCR      0xC
/* AVBP: bits[10:0], AHBP: bits[27:16] -- accumulated back porch, needed to
 * convert a shadow-buffer row/col index into an absolute panel coordinate
 * for window-clip checks against WHPCR/WVPCR (which are themselves
 * absolute-panel-coordinate registers, per real HAL_LTDC_Init()). */
#define LTDC_BPCR_AVBP_MASK     0x7FFU
#define LTDC_BPCR_AHBP_SHIFT    16
#define LTDC_BPCR_AHBP_MASK     0xFFFU

#define GNW_H7B0_LTDC_BCCR      0x2C

/* Layer1 registers, offsets relative to the LTDC base (0x84 + sub-offset). */
#define GNW_H7B0_LTDC_L1CR      0x84
#define LTDC_LxCR_LEN           (1U << 0)
/* COLKEN (color-key enable): confirmed via STM32H7B0.svd this session, not
 * previously defined in this codebase. */
#define LTDC_LxCR_COLKEN        (1U << 1)
#define GNW_H7B0_LTDC_L1WHPCR   0x88
#define GNW_H7B0_LTDC_L1WVPCR   0x8C
/* WHPCR: WHSTPOS bits[11:0], WHSPPOS bits[27:16]. WVPCR: WVSTPOS
 * bits[10:0], WVSPPOS bits[26:16]. Both absolute panel coordinates. */
#define LTDC_LxWHPCR_WHSTPOS_MASK  0xFFFU
#define LTDC_LxWHPCR_WHSPPOS_SHIFT 16
#define LTDC_LxWHPCR_WHSPPOS_MASK  0xFFFU
#define LTDC_LxWVPCR_WVSTPOS_MASK  0x7FFU
#define LTDC_LxWVPCR_WVSPPOS_SHIFT 16
#define LTDC_LxWVPCR_WVSPPOS_MASK  0x7FFU
#define GNW_H7B0_LTDC_L1CKCR    0x90
#define GNW_H7B0_LTDC_L1PFCR    0x94
#define LTDC_LxPFCR_PF_MASK     0x7U
#define LTDC_PF_RGB565          2U
#define LTDC_PF_L8              5U
#define LTDC_PF_AL44            6U
#define GNW_H7B0_LTDC_L1CACR    0x98
#define GNW_H7B0_LTDC_L1DCCR    0x9C
#define GNW_H7B0_LTDC_L1BFCR    0xA0
/* BF1: bits[10:8], BF2: bits[2:0]. Values 0x4/0x5 select constant-alpha
 * only (that layer's CACR); 0x6/0x7 select pixel-alpha x constant-alpha --
 * see gnw_h7b0_ltdc_blend_over()'s comment for the full formula. */
#define LTDC_LxBFCR_BF1_SHIFT   8
#define LTDC_LxBFCR_BF1_MASK    0x7U
#define LTDC_LxBFCR_BF2_MASK    0x7U
#define LTDC_LxBFCR_MODE_PA     (1U << 1) /* set in both BF1/BF2 PAxCA values */
#define GNW_H7B0_LTDC_L1CLUTWR  0xC4
#define GNW_H7B0_LTDC_L1CFBAR   0xAC
#define GNW_H7B0_LTDC_L1CFBLR   0xB0
#define LTDC_LxCFBLR_CFBLL_MASK 0x1FFFU
#define LTDC_LxCFBLR_CFBP_SHIFT 16
#define LTDC_LxCFBLR_CFBP_MASK  0x1FFFU
#define GNW_H7B0_LTDC_L1CFBLNR  0xB4

/* Layer2 registers */
#define GNW_H7B0_LTDC_L2CR      0x104
#define GNW_H7B0_LTDC_L2WHPCR   0x108
#define GNW_H7B0_LTDC_L2WVPCR   0x10C
#define GNW_H7B0_LTDC_L2CKCR    0x110
#define GNW_H7B0_LTDC_L2PFCR    0x114
#define GNW_H7B0_LTDC_L2CACR    0x118
#define GNW_H7B0_LTDC_L2DCCR    0x11C
#define GNW_H7B0_LTDC_L2BFCR    0x120
#define GNW_H7B0_LTDC_L2CFBAR   0x12C
#define GNW_H7B0_LTDC_L2CFBLR   0x130
#define GNW_H7B0_LTDC_L2CFBLNR  0x134
#define GNW_H7B0_LTDC_L2CLUTWR  0x144

#define GNW_H7B0_LTDC_IER    0x34
#define LTDC_IER_LIE         (1U << 0)
#define LTDC_IER_FUIE        (1U << 1)
#define LTDC_IER_TERRIE      (1U << 2)
#define LTDC_IER_RRIE        (1U << 3)

#define GNW_H7B0_LTDC_ISR    0x38
#define LTDC_ISR_LIF         (1U << 0)
#define LTDC_ISR_FUIF        (1U << 1)
#define LTDC_ISR_TERRIF      (1U << 2)
#define LTDC_ISR_RRIF        (1U << 3)

#define GNW_H7B0_LTDC_ICR    0x3C
/* ICR bit positions mirror ISR's LIF/FUIF/TERRIF/RRIF exactly. */

#define GNW_H7B0_LTDC_LIPCR  0x40

/*
 * Fallback vblank rate, used only when RCC isn't wired (gnw_h7b0_ltdc_set_rcc()
 * never called) or the live PLL3R-derived rate computes to something
 * nonsensical -- see gnw_h7b0_ltdc_recalc_timers() in gnw_h7b0_ltdc.c. Real
 * firmware's PLL3 config (M=4,N=9,R=24) against the real 392x255 panel
 * timing computes to ~66.7Hz, not this; kept only as a defensive default,
 * not a claim about the real refresh rate.
 */
#define GNW_H7B0_LTDC_VBLANK_HZ 60

/*
 * A raw, self-contained snapshot of everything
 * gnw_h7b0_ltdc_composite_from_job() (the compositor worker thread's
 * per-pixel convert/blend/dither work, moved out of the old synchronous
 * gnw_h7b0_ltdc_capture_rows()) needs, taken cheaply (memcpy only, no
 * per-pixel conversion) on the BQL-holding producer thread so the worker
 * thread never touches live guest RAM, s->regs, s->clut/clut2, or any of
 * the RAM-dirty-bitmap tracking state again -- only this struct, which
 * the producer guarantees not to mutate again until the worker clears
 * job_busy. l1_raw/l2_raw are reallocated (by the producer, under the
 * same "compositor provably idle" guarantee) only when frame geometry
 * changes, same cadence as shadow_buffer's own resizing.
 *
 * Deliberately does NOT include anything from the RAM-dirty-bitmap
 * fallback (fb_l1_section/fb_reg_dirty/etc below) -- that machinery
 * decides *whether* to dispatch a job at all and stays entirely on the
 * producer/BQL thread (see gnw_h7b0_ltdc_vblank_tick()'s comment), since
 * it calls QEMU's dirty-bitmap API (memory_region_snapshot_and_clear_dirty()
 * and friends), which is not documented as safe to call outside the BQL.
 */
typedef struct GnwH7B0LtdcCaptureJob {
    int cols;
    int rows;

    uint32_t active_l1cr, active_l1pfcr, active_l1cfblr;
    uint32_t active_l1ckcr, active_l1dccr, active_l1bfcr, active_l1cacr;
    uint32_t active_l1whpcr, active_l1wvpcr;

    bool l2_en;
    uint32_t active_l2cr;
    uint32_t active_l2pfcr, active_l2cfblr;
    uint32_t active_l2ckcr, active_l2dccr, active_l2bfcr, active_l2cacr;
    uint32_t active_l2whpcr, active_l2wvpcr;

    uint32_t active_bccr;
    uint32_t active_gcr;
    uint32_t active_bpcr;

    int l1_src_width;
    int l2_src_width;
    int l2_bpp;
    /*
     * Layer2's own column count, derived from its own stride/bpp -- NOT
     * assumed equal to Layer1's `cols`. Real firmware can configure
     * Layer2 with a narrower CFBLR/stride than Layer1 (e.g. a smaller
     * cover-art overlay over a full-width background); without this
     * bound, the compositor worker's per-pixel loop (driven by Layer1's
     * `cols`) can read past the end of l2_raw's per-row allocation -- a
     * real, previously-confirmed host segfault (see
     * gnw_h7b0_ltdc_snapshot_and_dispatch()'s computation of this field
     * for the full history). Zero whenever l2_bpp is 0.
     */
    int l2_cols;

    uint8_t *l1_raw;
    size_t l1_raw_size;
    uint8_t *l2_raw;
    size_t l2_raw_size;

    uint32_t clut[256];
    uint32_t clut2[256];
} GnwH7B0LtdcCaptureJob;

struct GnwH7B0LtdcState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    QemuConsole *con;
    qemu_irq irq;
    QEMUTimer *vblank_timer;
    QEMUTimer *line_timer;
    /*
     * Grid deadline the vblank timer is armed against. Re-arming at
     * "dispatch time + frame_ns" instead (the old behavior) bakes every
     * tick's host-scheduling lateness into the next frame permanently,
     * which under load ran the guest-visible vsync several percent below
     * 60Hz -- and stock Mario paces its NES-emulator audio producer off
     * vsync while the audio DMA drains at a metronomic 48kHz, so that
     * deficit surfaced as ~13 ring-underrun silence gaps per second
     * (measured): the long-standing "crunchy audio" bug. Same
     * deadline-grid discipline as gnw_h7b0_dma_schedule_next().
     */
    int64_t vblank_deadline_ns;
    int64_t snapshot_vblank_remaining_ns;
    int64_t snapshot_line_remaining_ns;
    int64_t snapshot_grid_remaining_ns;
    int invalidate;
    bool pf_warned;

    uint32_t regs[GNW_H7B0_LTDC_SIZE / 4];

    /* Set via gnw_h7b0_ltdc_set_rcc(), same realize-order-independence
     * rationale as gnw_h7b0_sai1.h's rcc field. Lets
     * gnw_h7b0_ltdc_recalc_timers() derive the real vblank rate from
     * PLL3R (LTDC's pixel clock is hardwired to pll3_r_ck, no mux) plus
     * live TWCR timing instead of the fixed GNW_H7B0_LTDC_VBLANK_HZ
     * fallback. May be NULL (e.g. standalone instantiation) -- callers
     * must check before use. */
    GnwH7B0RccState *rcc;
    GnwH7B0GpioState *backlight_gpio;
    GnwH7B0DacState *backlight_dac;
    uint32_t last_backlight;

    /*
     * Real "active" copies of the Layer1 and Layer2 registers that actually
     * affect what gets drawn (CFBAR/CFBLR/CFBLNR/PFCR) -- see
     * gnw_h7b0_ltdc_write()'s SRCR handling. s->regs holds what
     * firmware last wrote (the "shadow" set, read back as-is); these
     * only change on an SRCR reload (IMR: immediately; VBR: deferred
     * to the next real vblank tick), same as real hardware, instead
     * of every layer-register write being instantly visible to the
     * next redraw.
     */
    uint32_t active_l1cr;
    uint32_t active_l1cfbar;
    uint32_t active_l1cfblr;
    uint32_t active_l1cfblnr;
    uint32_t active_l1pfcr;
    uint32_t active_l2cr;
    uint32_t active_l2cfbar;
    uint32_t active_l2cfblr;
    uint32_t active_l2cfblnr;
    uint32_t active_l2pfcr;
    uint32_t active_l2cacr;
    bool vbr_reload_pending;
    /*
     * Set when a SRCR.VBR write could not capture the outgoing frame
     * because content_dirty was still true (the UI thread had not yet
     * blitted the previous capture). The deferred capture is then taken
     * immediately after the vblank reload applies the new shadow
     * registers -- essential for lcd_setup_framebuffers()'s format/geometry
     * changes, which otherwise leave the shadow buffer sized/configured for
     * the old mode and the display appears frozen.
     */
    bool vbr_deferred_capture;
    /*
     * Tracks "is the *current* screen/config VBR-paced" -- set on every
     * SRCR.VBR write, cleared on every SRCR.IMR write (real screen/config
     * transitions -- e.g. retro-go leaving a game for its own main menu --
     * apply their new layer config via an immediate IMR reload). Gates
     * vblank_tick()'s no-VBR auto-capture fallback (added for gnwmanager/
     * OFW firmware, and retro-go's own main-menu UI, that paint pixels
     * directly without ever reloading via VBR).
     *
     * A permanent "has this device ever used VBR" latch made the main
     * menu screen stay black forever after returning from *any* game
     * (the menu itself never writes SRCR again to re-trigger a capture).
     * An idle-timeout version of that latch was tried next and also
     * reverted: real in-game firmware can leave multi-hundred-ms real
     * gaps between VBR writes on its own (see the SMW APU-catchup-burst
     * investigation), so any timeout short enough to un-stick the menu
     * promptly was also short enough to spuriously re-arm this fallback
     * mid-game during those same bursts -- reintroducing mid-draw
     * tearing exactly when the game stutters. Resetting on IMR instead
     * avoids guessing at a time threshold entirely.
     */
    bool vbr_active;

    /*
     * Additional active-set snapshots for the generalized per-layer
     * compositing pipeline (color key, window-clip/default-color,
     * generalized blend factors, dithering) -- same shadow->active
     * reload semantics as the fields above, just for registers that
     * previously weren't read at all.
     */
    uint32_t active_l1ckcr;
    uint32_t active_l2ckcr;
    uint32_t active_l1dccr;
    uint32_t active_l2dccr;
    uint32_t active_bccr;
    uint32_t active_l1cacr;
    uint32_t active_l1bfcr;
    uint32_t active_l2bfcr;
    uint32_t active_l1whpcr;
    uint32_t active_l1wvpcr;
    uint32_t active_l2whpcr;
    uint32_t active_l2wvpcr;
    uint32_t active_gcr;
    uint32_t active_bpcr;

    /*
     * Shadow buffer to decouple QEMU's asynchronous UI refresh from the
     * guest's rendering. The guest often draws overlays (via DMA2D) directly
     * into the active frontbuffer immediately after a VBR flip. If the UI
     * thread samples guest RAM mid-draw, overlays flicker out of existence.
     * We capture the fully finished frame at the very end of its 16ms window
     * (the instant before the next VBLANK) to guarantee a tear-free image.
     * (A progressive/scanline-timed capture was tried here and reverted --
     * see gnw_h7b0_ltdc_vblank_tick()'s comment for why.)
     *
     * As of the LTDC-compositor worker-thread change: "shadow_buffer" is
     * now the *published/front* buffer -- the one
     * gnw_h7b0_ltdc_update_display() (called from the BQL-holding UI-
     * refresh path) reads from. It is a ping-pong pair with
     * compositor_back_buffer (see below): the compositor worker thread
     * writes a freshly-composited frame into whichever of the two is NOT
     * currently published, then atomically swaps the two pointers under
     * compositor_publish_lock. Every access to shadow_buffer/
     * shadow_width/shadow_height must hold compositor_publish_lock
     * EXCEPT from gnw_h7b0_ltdc_capture_setup(), which only (re)sizes
     * both buffers while the compositor is provably idle (see
     * gnw_h7b0_ltdc_capture_if_enabled()'s job_busy gate, checked before
     * capture_setup() is ever called) -- so no concurrent writer can
     * exist at resize time either way, but the lock is still taken there
     * too since update_display() may be reading the old pointer
     * concurrently on the main thread right up until the resize.
     */
    uint32_t *shadow_buffer;
    int shadow_width;
    int shadow_height;

    /*
     * The compositor worker thread's private scratch buffer -- the other
     * half of the front/back ping-pong pair described above. Touched
     * ONLY by the compositor worker thread (never the vCPU/main/BQL
     * thread) between the moment it wakes up for a job and the moment it
     * swaps pointers with shadow_buffer under compositor_publish_lock,
     * so no lock is needed for the compositor's own reads/writes of this
     * pointer -- only the swap itself needs the lock.
     */
    uint32_t *compositor_back_buffer;

    /*
     * Guards shadow_buffer/compositor_back_buffer (pointer values only,
     * plus shadow_width/shadow_height and content_dirty) against the one
     * genuine cross-thread race this device now has: the compositor
     * worker thread publishing a finished frame (a pointer swap) versus
     * the main/BQL thread's gnw_h7b0_ltdc_update_display() reading the
     * currently-published pointer. Held only for the swap itself / a
     * short read, never across the expensive per-pixel compositing work
     * or the display blit -- see gnw_h7b0_ltdc_composite_from_job() and
     * gnw_h7b0_ltdc_update_display().
     */
    QemuMutex compositor_publish_lock;

    /*
     * Job handoff between the BQL-holding producer side (MMIO write
     * handler / vblank timer callback -- mutually exclusive with each
     * other already via the BQL, so effectively a single producer) and
     * the compositor worker thread (single consumer). job_lock/job_cond
     * implement a classic bounded (depth-1) work queue: the producer
     * snapshots raw guest RAM + register/CLUT state into compositor_job
     * (a cheap memcpy-only step, still done synchronously on the vCPU/
     * main thread, AFTER the RAM-dirty-bitmap fallback check below has
     * already decided a capture is warranted) and sets job_pending,
     * waking the worker; the worker clears job_pending, does the
     * expensive per-pixel convert/blend/dither work OUTSIDE any lock
     * (reading only compositor_job, which the producer is guaranteed not
     * to touch again until it observes job_busy false), then clears
     * job_busy once done. The producer uses job_busy (checked/set under
     * job_lock) as a "compositor still busy with the previous frame"
     * throttle -- a new capture request arriving while busy is simply
     * dropped (the existing vbr_deferred_capture/fb_reg_dirty retry-on-
     * next-vblank-tick mechanisms already handle redelivery -- see
     * gnw_h7b0_ltdc_capture_if_enabled()'s doc comment), which is the
     * same "drop a frame under load" policy real double-buffered display
     * pipelines use, not a new correctness compromise.
     */
    QemuMutex job_lock;
    QemuCond job_cond;
    bool job_pending;
    bool job_busy;
    GnwH7B0LtdcCaptureJob compositor_job;

    /*
     * Staged capture slot (BQL-only). When a reload-triggered capture
     * arrives while the compositor worker is still busy, the frame is
     * snapshotted HERE at the reload instant and dispatched later
     * (vblank tick, or displaced by a newer capture). The old behavior
     * -- deferring the whole capture and re-reading guest RAM up to a
     * vblank later -- was a confirmed source of visible mid-redraw
     * flicker: retro-go's menu draws into the displayed buffer on
     * alternating frames (its ReloadEventCallback re-stages CFBAR via
     * an immediate IMR reload), so a late re-read snapshots a
     * half-drawn carousel. Snapshot timing must follow the guest's
     * reload, not the worker's availability.
     */
    GnwH7B0LtdcCaptureJob staged_job;
    bool staged_valid;

    QemuThread compositor_thread;
    bool compositor_running;
    bool compositor_stop;

    /*
     * RAM dirty-bitmap tracking for the non-VBR auto-capture fallback
     * (see gnw_h7b0_ltdc_vblank_tick()'s else-branch): lets that path
     * skip a full recomposite when neither layer's guest-RAM
     * framebuffer nor any composition-affecting register has changed
     * since the last fallback capture, instead of recompositing
     * unconditionally on every tick. Uses the same
     * MemoryRegionSection + DIRTY_MEMORY_VGA dirty-bitmap API as
     * hw/display/vga.c and hw/display/framebuffer.c (this file's own
     * structural template, pl110.c, uses the same API) -- NOT a
     * register-write-only flag, since firmware commonly paints new
     * pixels directly into an already-configured framebuffer without
     * touching any LTDC register again (e.g. a game continuing to
     * render behind a static menu overlay). fb_l1_track_base/_len and
     * fb_l2_track_base/_len record what range each section is
     * currently bound to, so a rebind (via
     * framebuffer_update_memory_section(), which toggles
     * memory_region_set_log() and is too costly to call every tick --
     * see pl110_update_display()) only happens when the tracked range
     * actually changes, not on every fallback check.
     */
    MemoryRegionSection fb_l1_section;
    MemoryRegionSection fb_l2_section;
    hwaddr fb_l1_track_base;
    hwaddr fb_l1_track_len;
    hwaddr fb_l2_track_base;
    hwaddr fb_l2_track_len;
    /*
     * Secondary, additional signal alongside the RAM-dirty check above:
     * set whenever gnw_h7b0_ltdc_reload_active() applies a fresh set of
     * composition-affecting registers (window position, blend factors,
     * etc.), so a register-only change with an otherwise-unwritten
     * framebuffer (e.g. a window move) still forces a fallback
     * recapture. Cleared once the fallback consumes it. This is
     * deliberately NOT the primary mechanism (see the file-level
     * rejected-attempt history) -- only an additional net to catch
     * changes the RAM-dirty check structurally cannot see.
     */
    bool fb_reg_dirty;

    /*
     * Draw-quiescence debounce for the non-VBR fallback capture. Direct-
     * paint firmware (retro-go's menu) redraws its visible framebuffer
     * in place; on real hardware a full redraw (JPEG cover decodes
     * included) finishes in a couple of ms and intermediate states are
     * never visible, but under TCG the same redraw spans many vblank
     * ticks, so capturing at the first dirty tick publishes half-drawn
     * frames (seen live: a side cover composited on top of the center
     * cover, and a periodic menu flicker). Instead, a dirty tick only
     * arms fb_quiesce_pending; the capture happens on the first later
     * tick whose dirty check comes back clean (draw burst over), or
     * unconditionally after GNW_H7B0_LTDC_QUIESCE_TICKS_MAX ticks so a
     * continuously-animating screen still updates.
     */
    bool fb_quiesce_pending;
    int fb_quiesce_ticks;

    /*
     * Counts consecutive gnw_h7b0_ltdc_vblank_tick() calls since the last
     * SRCR write of any kind (VBR or IMR) -- reset to 0 on every SRCR
     * write, incremented each tick otherwise. Lets the non-VBR fallback
     * also fire once VBR mode has gone idle for a while even though
     * vbr_active is still latched true (see the vblank_tick() else-
     * branch's comment): a firmware transition can end on a VBR-type
     * reload (not IMR) with no further reloads ever coming, which
     * otherwise permanently blocks the fallback with no other signal
     * that VBR pacing has actually stopped.
     *
     * NOT sufficient alone, and not even alongside the RAM-dirty check --
     * confirmed live (2026-07-14) that ordinary in-game frame-skipping
     * (game-and-watch-retro-go-sd's common_emu_frame_loop()/
     * frame_integrator catch-up mechanism, the same one root-caused for
     * the general post-pause stutter) ALSO suppresses lcd_swap()/VBR
     * reloads for the same duration as any real stutter, and the
     * framebuffer keeps getting genuinely new content throughout (the
     * game is still trying to render, just skipping some draws) -- so
     * "VBR idle + framebuffer dirty" alone fires just as often during
     * completely normal stutter as during a real abandoned-VBR
     * transition, reintroducing mid-draw tearing/flicker during any bad
     * stutter in any game. See structural_transition_pending below for
     * the fix: also require evidence of a genuine screen/layer
     * transition, not just an idle timer.
     */
    int srcr_idle_ticks;

    /*
     * Set by gnw_h7b0_ltdc_reload_active() whenever a reload changes
     * pixel format, layer enable, or window geometry relative to the
     * currently-active config (gnw_h7b0_ltdc_reload_is_structural_change())
     * -- deliberately EXCLUDES CFBAR (buffer address), which flips every
     * frame during completely normal double buffering and so can't be
     * used to distinguish "a new screen just took over" from "same
     * screen, routine frame swap." Required, alongside srcr_idle_ticks
     * crossing its threshold, before the non-VBR fallback is allowed to
     * override a still-true vbr_active -- see srcr_idle_ticks' doc
     * comment for why the idle-timer signal alone isn't enough. Reset
     * to false on every SRCR write (alongside srcr_idle_ticks), not
     * just when consumed by the fallback -- an earlier "sticky forever"
     * design was confirmed live to be a real bug: since srcr_idle_ticks
     * alone crosses its threshold during ordinary stutter too, leaving
     * this flag permanently armed after a single genuine transition
     * anywhere in a session meant every later stutter burst re-
     * triggered the fallback, causing a real, self-inflicted
     * tlb_reset_dirty/notdirty-write cost (confirmed via profiling:
     * gnw_h7b0_ltdc_fb_dirty_check_and_clear()'s own
     * memory_region_snapshot_and_clear_dirty() call forces the
     * framebuffer's TLB entries back into slow tracking every time it
     * runs). gnw_h7b0_ltdc_reload_active() re-arms it if the write that
     * triggered this reset is itself a genuine structural change.
     */
    bool structural_transition_pending;

    /*
     * Layer1's hardware CLUT (L1CLUTWR), used only when active_l1pfcr
     * selects L8 -- retro-go switches Layer1 into L8 at runtime to save
     * framebuffer RAM (see Core/Src/gw_lcd.c's lcd_setup_framebuffers()/
     * HAL_LTDC_ConfigCLUT() in the real firmware). Entries are already
     * converted to this device's host pixel32 format (see
     * gnw_h7b0_ltdc_write()'s L1CLUTWR case), not left as raw RGB888, so
     * capture_rows() can index straight into it like the RGB565 path.
     */
    uint32_t clut[256];

    /*
     * Layer2's own hardware CLUT (L2CLUTWR) -- real hardware has fully
     * independent per-layer CLUTs, so a single shared array (as used to
     * be the case here) is only correct as long as Layer2 never uses L8,
     * which real firmware doesn't guarantee.
     */
    uint32_t clut2[256];

    /*
     * Set whenever gnw_h7b0_ltdc_vblank_tick() captures a fresh frame
     * into shadow_buffer; cleared once gnw_h7b0_ltdc_update_display()
     * actually redraws from it. QEMU's own UI refresh timer
     * (graphic_hw_update -> our gfx_update callback) fires on its own
     * cadence, independent of and often faster than our ~60Hz capture --
     * without this, update_display() was redoing its full per-pixel
     * upscale/blit (profiled at over a third of this process's total
     * CPU time) on every one of those extra calls even when the
     * frontbuffer hadn't changed at all, which is what was actually
     * behind the "one core pegged" / audio-and-video stutter reports:
     * the guest CPU emulation and the host UI-refresh work compete for
     * the same single QEMU main thread. */
    bool content_dirty;
};

void gnw_h7b0_ltdc_snapshot_restore(GnwH7B0LtdcState *s);

void gnw_h7b0_ltdc_set_rcc(GnwH7B0LtdcState *s, GnwH7B0RccState *rcc);

#endif
