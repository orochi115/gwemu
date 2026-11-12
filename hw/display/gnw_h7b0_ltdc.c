/*
 * STM32H7B0 LTDC minimal stub (Nintendo Game & Watch)
 *
 * See gnw_h7b0_ltdc.h for scope/rationale. Structural template:
 * hw/display/pl110.c.
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
#include "qapi/error.h"
#include "migration/vmstate.h"
#include "migration/qemu-file.h"
#include "exec/cpu-common.h"
#include "ui/console.h"
#include "ui/pixel_ops.h"
#include "hw/display/gnw_h7b0_ltdc.h"
#include "hw/misc/gnw_timeline.h"
#include "hw/display/gnw_h7b0_recorder.h"
#include "hw/display/gnw_h7b0_regs_ltdc.h"
#include "hw/misc/gnw_env.h"
#include "framebuffer.h"
#include "system/address-spaces.h"

/* getenv() is a locked linear scan on Windows (msvcrt) -- never call it
 * per-event in emulation-hot paths; resolve once and cache. */
static bool gnw_ltdc_trace_enabled(void)
{
    static int v = -1;
    if (v < 0) {
        v = gnw_env_enabled("GNW_LTDC_TRACE");
    }
    return v;
}

/* getenv() is a locked linear scan on Windows (msvcrt) -- never call it
 * per-event in emulation-hot paths; resolve once and cache. */
static bool gnw_timer_late_enabled(void)
{
    static int v = -1;
    if (v < 0) {
        v = gnw_env_enabled("GNW_TIMER_LATE");
    }
    return v;
}

static int gnw_h7b0_ltdc_capture_setup(GnwH7B0LtdcState *s);
static void gnw_h7b0_ltdc_composite_from_job(GnwH7B0LtdcState *s,
                                              GnwH7B0LtdcCaptureJob *job,
                                              uint32_t *out);
static void gnw_h7b0_ltdc_wait_compositor_idle(GnwH7B0LtdcState *s);
static bool gnw_h7b0_ltdc_fb_dirty_check_and_clear(GnwH7B0LtdcState *s);
static int gnw_h7b0_ltdc_layer_stride_bytes(uint32_t cfblr);
static void gnw_h7b0_ltdc_fb_track_range(MemoryRegionSection *section,
                                          hwaddr *cur_base, hwaddr *cur_len,
                                          hwaddr base, hwaddr len);

static bool gnw_h7b0_ltdc_enabled(GnwH7B0LtdcState *s)
{
    return (s->active_gcr & LTDC_GCR_LTDCEN) &&
           ((s->active_l1cr & LTDC_LxCR_LEN) ||
            (s->active_l2cr & LTDC_LxCR_LEN));
}

static void gnw_h7b0_ltdc_snapshot_and_dispatch(GnwH7B0LtdcState *s, int rows);
static void gnw_h7b0_ltdc_snapshot_into(GnwH7B0LtdcState *s,
                                         GnwH7B0LtdcCaptureJob *job, int rows);
static void gnw_h7b0_ltdc_promote_staged(GnwH7B0LtdcState *s);

/*
 * Cheap peek at job_busy, used by vblank_tick()'s non-VBR fallback to
 * avoid calling gnw_h7b0_ltdc_fb_dirty_check_and_clear() (which
 * *consumes* the RAM dirty bitmap -- see that function's doc comment)
 * for a capture we already know gnw_h7b0_ltdc_capture_if_enabled() would
 * just drop for being busy. Without this, the dirty-bitmap signal would
 * be lost even though no capture actually happened -- a real gap
 * capture_if_enabled()'s own return-value checks alone don't cover,
 * since by the time it runs the dirty bitmap would already be cleared.
 */
static bool gnw_h7b0_ltdc_compositor_busy(GnwH7B0LtdcState *s)
{
    bool busy;

    qemu_mutex_lock(&s->job_lock);
    busy = s->job_busy;
    qemu_mutex_unlock(&s->job_lock);
    return busy;
}

/*
 * Return true when a capture job was actually dispatched to the
 * compositor worker thread this call (NOT when the resulting frame has
 * finished compositing -- that happens asynchronously; see
 * gnw_h7b0_ltdc_composite_from_job() and the worker thread function).
 * Returns false if LTDC/Layer1 is disabled/unsupported, or if the worker
 * is still busy with a previous frame (job_busy) -- every call site
 * that needs eventual delivery of a dropped request (SRCR.VBR's
 * vbr_deferred_capture, the non-VBR fallback's fb_reg_dirty) already
 * checks this return value and retries on the next vblank tick instead
 * of assuming a call here always succeeds, which is the one behavior
 * change this device's existing capture-gating logic (all of which
 * predates and is unrelated to the compositor thread -- vbr_active,
 * structural_transition_pending, the RAM-dirty-bitmap fallback -- see
 * gnw_h7b0_ltdc_vblank_tick()) needed to keep working correctly now that
 * a "capture" is no longer instantaneous.
 */
static bool gnw_h7b0_ltdc_capture_if_enabled(GnwH7B0LtdcState *s)
{
    int rows;

    if (!gnw_h7b0_ltdc_enabled(s)) {
        return false;
    }

    /*
     * Check job_busy BEFORE calling capture_setup(): capture_setup() may
     * resize shadow_buffer/compositor_back_buffer, which is only safe
     * while the compositor worker thread is provably not touching either
     * buffer. The producer side (this function) always runs with the
     * BQL held, so it can never race a *concurrent* producer -- only the
     * worker matters here, and job_busy==false is exactly "the worker
     * already published its last frame and is asleep waiting for work".
     */
    qemu_mutex_lock(&s->job_lock);
    if (s->job_busy) {
        uint32_t pfcr = s->active_l1pfcr & LTDC_LxPFCR_PF_MASK;
        int src_width = gnw_h7b0_ltdc_layer_stride_bytes(s->active_l1cfblr);
        int cols = src_width / (pfcr == LTDC_PF_L8 ? 1 : 2);
        int brows = (int)(s->active_l1cfblnr & 0x7FFU);
        qemu_mutex_unlock(&s->job_lock);

        /*
         * Worker still busy: snapshot NOW into the staged slot (guest
         * RAM must be read at the reload instant -- see staged_job's
         * doc comment), dispatch later. Only possible while the frame
         * geometry matches the current shadow buffer: a resize needs
         * the worker provably idle, so geometry changes keep the old
         * defer-and-retry behavior (rare: screen/mode transitions).
         */
        if (cols != s->shadow_width || brows != s->shadow_height ||
            (pfcr != LTDC_PF_RGB565 && pfcr != LTDC_PF_L8) ||
            !(s->active_l1cr & LTDC_LxCR_LEN)) {
            return false;
        }
        gnw_h7b0_ltdc_snapshot_into(s, &s->staged_job, brows);
        s->staged_valid = true;
        return true;
    }
    qemu_mutex_unlock(&s->job_lock);

    rows = gnw_h7b0_ltdc_capture_setup(s);
    if (rows <= 0) {
        return false;
    }

    /* A fresh capture supersedes any older staged frame. */
    s->staged_valid = false;
    gnw_h7b0_ltdc_snapshot_and_dispatch(s, rows);
    return true;
}

/*
 * Integer nearest-neighbor upscale factor for the host window. The G&W
 * LCD is a tiny 320x240 panel; displayed 1:1 that's uncomfortably small
 * on a modern monitor, so every frame is blitted at SCALE x its native
 * size instead of relying on host-side window scaling (not uniformly
 * supported/discoverable across QEMU's -display backends).
 */
#define GNW_H7B0_LTDC_SCALE 2

/*
 * How many consecutive vblank ticks with no SRCR write (VBR or IMR) at
 * all count as "VBR mode has gone idle" -- see srcr_idle_ticks' doc
 * comment in gnw_h7b0_ltdc.h. ~130ms at the nominal 60Hz vblank rate:
 * comfortably longer than a single dropped/late frame, but much shorter
 * than the multi-hundred-ms real gaps normal in-game stalls can produce
 * (safe specifically because this is additionally gated on the
 * framebuffer having genuinely new content, which a stalled game isn't
 * producing during its stall). _MAX just bounds the counter itself so
 * it can't silently wrap on a long-running static screen.
 */
#define GNW_H7B0_LTDC_SRCR_IDLE_TICKS_THRESHOLD 8

/*
 * Bounded wait (in vblank ticks) for the non-VBR fallback's draw-
 * quiescence debounce -- see fb_quiesce_pending in gnw_h7b0_ltdc.h.
 * Large enough to ride out a multi-tick menu redraw (JPEG cover decodes
 * included), small enough that a continuously-dirty animation still
 * refreshes at a live (if reduced) rate (60/16 = ~4fps floor). Raised
 * from 4 after live testing: a full carousel redraw (5 JPEG cover
 * decodes) exceeds 4 ticks, so the bounded capture still published
 * mid-redraw frames on every periodic menu refresh.
 */
#define GNW_H7B0_LTDC_QUIESCE_TICKS_MAX 16

/* Env-gated capture-path tracing (GNW_LTDC_TRACE) -- which code path
 * published each frame, with the virtual timestamp and active CFBAR. */
#define GNW_LTDC_TRACE_CAP(s, tag) do { \
    if (gnw_ltdc_trace_enabled()) { \
        fprintf(stderr, "LTC cap %s %" PRId64 " cfbar=%08x\n", (tag), \
                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), \
                (s)->regs[GNW_H7B0_LTDC_L1CFBAR >> 2]); \
    } \
} while (0)
#define GNW_H7B0_LTDC_SRCR_IDLE_TICKS_MAX 1000000

static void gnw_h7b0_ltdc_update_irq(GnwH7B0LtdcState *s)
{
    uint32_t pending = s->regs[GNW_H7B0_LTDC_ISR >> 2] &
                       s->regs[GNW_H7B0_LTDC_IER >> 2];

    qemu_set_irq(s->irq, pending != 0);
}

/*
 * Whether this reload represents a genuine layer/screen transition
 * (format, layer-enable, or window geometry differs from the
 * currently-active config) as opposed to routine same-screen double
 * buffering, which only ever changes CFBAR (the buffer address) --
 * deliberately excluded from this comparison since it flips every
 * frame during completely normal steady-state VBR-paced gameplay. See
 * srcr_idle_ticks'/structural_transition_pending's doc comments in
 * gnw_h7b0_ltdc.h for why this distinction matters.
 */
static bool gnw_h7b0_ltdc_reload_is_structural_change(GnwH7B0LtdcState *s)
{
    return s->active_l1pfcr != s->regs[GNW_H7B0_LTDC_L1PFCR >> 2] ||
           (s->active_l1cr & LTDC_LxCR_LEN) !=
               (s->regs[GNW_H7B0_LTDC_L1CR >> 2] & LTDC_LxCR_LEN) ||
           s->active_l2pfcr != s->regs[GNW_H7B0_LTDC_L2PFCR >> 2] ||
           (s->active_l2cr & LTDC_LxCR_LEN) !=
               (s->regs[GNW_H7B0_LTDC_L2CR >> 2] & LTDC_LxCR_LEN) ||
           s->active_l1whpcr != s->regs[GNW_H7B0_LTDC_L1WHPCR >> 2] ||
           s->active_l1wvpcr != s->regs[GNW_H7B0_LTDC_L1WVPCR >> 2] ||
           s->active_l2whpcr != s->regs[GNW_H7B0_LTDC_L2WHPCR >> 2] ||
           s->active_l2wvpcr != s->regs[GNW_H7B0_LTDC_L2WVPCR >> 2];
}

/*
 * True if any register gnw_h7b0_ltdc_reload_active() is about to commit
 * actually differs from what's already active -- i.e. this reload has
 * nothing new to contribute to the composited frame. Covers the exact
 * same register set reload_active() copies (both layers' CR/CFBAR/
 * CFBLR/CFBLNR/PFCR/CACR/CKCR/DCCR/BFCR/WHPCR/WVPCR plus GCR/BPCR/BCCR),
 * so "unchanged here" really does mean "the next capture would produce a
 * bit-identical frame to the last one," not just "the specific fields
 * gnw_h7b0_ltdc_reload_is_structural_change() happens to check."
 *
 * Added per stm32h7b0-diag's docs/qemu-reports/ltdc.md: any SRCR.IMR
 * write anywhere in the system -- including one that only touches an
 * always-disabled scratch Layer 2, as case_ltdc_frame_composite.c/
 * case_ltdc_clut_correct.c both deliberately do -- unconditionally
 * triggered a full 320x240 Layer-1 framebuffer
 * cpu_physical_memory_read() snapshot-and-dispatch, because the capture
 * is gated on "is LTDC enabled" (Layer 1 always is, for the real menu)
 * rather than "did anything that would change the composited frame
 * actually happen." Measured ~22x QEMU-vs-hardware gap, ~18.7us/reload
 * of real host work for a shadow-register latch that's near-instant on
 * real silicon. Skipping the capture when nothing composition-relevant
 * changed removes that cost for exactly this class of no-op reload
 * without touching real gameplay: an ordinary game's IMR-triggered
 * format/geometry reconfiguration (the case this reload path exists
 * for) always changes at least one of these registers by definition, so
 * the capture still fires for those, unchanged. If Layer 1's RAM
 * content changes without any register write at all (direct-paint
 * firmware), that's already independently covered by
 * gnw_h7b0_ltdc_vblank_tick()'s separate RAM-dirty-bitmap fallback --
 * this only skips the *register-triggered* capture, it doesn't disable
 * or interact with that other path.
 */
static bool gnw_h7b0_ltdc_reload_is_composition_change(GnwH7B0LtdcState *s)
{
    return s->active_l1cr     != s->regs[GNW_H7B0_LTDC_L1CR >> 2] ||
           s->active_l1cfbar  != s->regs[GNW_H7B0_LTDC_L1CFBAR >> 2] ||
           s->active_l1cfblr  != s->regs[GNW_H7B0_LTDC_L1CFBLR >> 2] ||
           s->active_l1cfblnr != s->regs[GNW_H7B0_LTDC_L1CFBLNR >> 2] ||
           s->active_l1pfcr   != s->regs[GNW_H7B0_LTDC_L1PFCR >> 2] ||
           s->active_l2cr     != s->regs[GNW_H7B0_LTDC_L2CR >> 2] ||
           s->active_l2cfbar  != s->regs[GNW_H7B0_LTDC_L2CFBAR >> 2] ||
           s->active_l2cfblr  != s->regs[GNW_H7B0_LTDC_L2CFBLR >> 2] ||
           s->active_l2cfblnr != s->regs[GNW_H7B0_LTDC_L2CFBLNR >> 2] ||
           s->active_l2pfcr   != s->regs[GNW_H7B0_LTDC_L2PFCR >> 2] ||
           s->active_l2cacr   != s->regs[GNW_H7B0_LTDC_L2CACR >> 2] ||
           s->active_l1ckcr   != s->regs[GNW_H7B0_LTDC_L1CKCR >> 2] ||
           s->active_l2ckcr   != s->regs[GNW_H7B0_LTDC_L2CKCR >> 2] ||
           s->active_l1dccr   != s->regs[GNW_H7B0_LTDC_L1DCCR >> 2] ||
           s->active_l2dccr   != s->regs[GNW_H7B0_LTDC_L2DCCR >> 2] ||
           s->active_bccr     != s->regs[GNW_H7B0_LTDC_BCCR >> 2] ||
           s->active_l1cacr   != s->regs[GNW_H7B0_LTDC_L1CACR >> 2] ||
           s->active_l1bfcr   != s->regs[GNW_H7B0_LTDC_L1BFCR >> 2] ||
           s->active_l2bfcr   != s->regs[GNW_H7B0_LTDC_L2BFCR >> 2] ||
           s->active_l1whpcr  != s->regs[GNW_H7B0_LTDC_L1WHPCR >> 2] ||
           s->active_l1wvpcr  != s->regs[GNW_H7B0_LTDC_L1WVPCR >> 2] ||
           s->active_l2whpcr  != s->regs[GNW_H7B0_LTDC_L2WHPCR >> 2] ||
           s->active_l2wvpcr  != s->regs[GNW_H7B0_LTDC_L2WVPCR >> 2] ||
           s->active_gcr      != s->regs[GNW_H7B0_LTDC_GCR >> 2] ||
           s->active_bpcr     != s->regs[GNW_H7B0_LTDC_BPCR >> 2];
}

static void gnw_h7b0_ltdc_reload_active(GnwH7B0LtdcState *s)
{
    /*
     * Composition-affecting registers are about to change -- see
     * fb_reg_dirty's comment in gnw_h7b0_ltdc.h for why this is an
     * additional signal alongside (not instead of) the RAM-dirty
     * check in gnw_h7b0_ltdc_fb_dirty_check_and_clear().
     */
    s->fb_reg_dirty = true;

    if (gnw_h7b0_ltdc_reload_is_structural_change(s)) {
        s->structural_transition_pending = true;
    }

    s->active_l1cr = s->regs[GNW_H7B0_LTDC_L1CR >> 2];
    s->active_l1cfbar = s->regs[GNW_H7B0_LTDC_L1CFBAR >> 2];
    s->active_l1cfblr = s->regs[GNW_H7B0_LTDC_L1CFBLR >> 2];
    s->active_l1cfblnr = s->regs[GNW_H7B0_LTDC_L1CFBLNR >> 2];
    s->active_l1pfcr = s->regs[GNW_H7B0_LTDC_L1PFCR >> 2];

    s->active_l2cr = s->regs[GNW_H7B0_LTDC_L2CR >> 2];
    s->active_l2cfbar = s->regs[GNW_H7B0_LTDC_L2CFBAR >> 2];
    s->active_l2cfblr = s->regs[GNW_H7B0_LTDC_L2CFBLR >> 2];
    s->active_l2cfblnr = s->regs[GNW_H7B0_LTDC_L2CFBLNR >> 2];
    s->active_l2pfcr = s->regs[GNW_H7B0_LTDC_L2PFCR >> 2];
    s->active_l2cacr = s->regs[GNW_H7B0_LTDC_L2CACR >> 2];

    s->active_l1ckcr = s->regs[GNW_H7B0_LTDC_L1CKCR >> 2];
    s->active_l2ckcr = s->regs[GNW_H7B0_LTDC_L2CKCR >> 2];
    s->active_l1dccr = s->regs[GNW_H7B0_LTDC_L1DCCR >> 2];
    s->active_l2dccr = s->regs[GNW_H7B0_LTDC_L2DCCR >> 2];
    s->active_bccr = s->regs[GNW_H7B0_LTDC_BCCR >> 2];
    s->active_l1cacr = s->regs[GNW_H7B0_LTDC_L1CACR >> 2];
    s->active_l1bfcr = s->regs[GNW_H7B0_LTDC_L1BFCR >> 2];
    s->active_l2bfcr = s->regs[GNW_H7B0_LTDC_L2BFCR >> 2];
    s->active_l1whpcr = s->regs[GNW_H7B0_LTDC_L1WHPCR >> 2];
    s->active_l1wvpcr = s->regs[GNW_H7B0_LTDC_L1WVPCR >> 2];
    s->active_l2whpcr = s->regs[GNW_H7B0_LTDC_L2WHPCR >> 2];
    s->active_l2wvpcr = s->regs[GNW_H7B0_LTDC_L2WVPCR >> 2];
    s->active_gcr = s->regs[GNW_H7B0_LTDC_GCR >> 2];
    s->active_bpcr = s->regs[GNW_H7B0_LTDC_BPCR >> 2];
}

static void gnw_h7b0_ltdc_timing(GnwH7B0LtdcState *s, int64_t *frame_ns_out,
                                  int64_t *line_ns_out, uint32_t *lipcr_out,
                                  uint32_t *totalh_out);

/*
 * Re-derives the frame period from live clock/timing registers and
 * re-arms the timers -- PHASE-PRESERVING. The vblank deadline grid is
 * kept if it is still sane (strictly in the future, no further than one
 * freshly computed frame away) and only re-anchored at "now" when it is
 * stale, unarmed, or wedged against a transiently-huge period (the
 * mid-PLL3-programming case the SRCR-write call site exists for).
 *
 * Phase preservation is load-bearing for audio, not cosmetic: firmware
 * writes SRCR.VBR once per frame ~1.4ms after taking the vblank IRQ,
 * and the previous unconditional "deadline = now + frame_ns" here moved
 * the next vblank a full period past *that write* -- stretching every
 * frame to (guest work + 16.65ms) ~= 18.1ms. Stock Mario's NES-emulator
 * audio producer is paced by this exact vsync chain while its audio DMA
 * drains at a metronomic 48kHz, so those stretched frames were a
 * structural ~8% sample shortfall: the long-standing "crunchy audio"
 * bug (measured: 55.15fps, ~20% of DMA chunks handed off as ring-
 * underrun silence; see CHANGELOG 2026-07-22).
 */
static void gnw_h7b0_ltdc_recalc_timers(GnwH7B0LtdcState *s, int64_t now)
{
    int64_t frame_ns, line_ns;
    uint32_t lipcr, totalh;
    int64_t next;

    gnw_h7b0_ltdc_timing(s, &frame_ns, &line_ns, &lipcr, &totalh);

    next = s->vblank_deadline_ns;
    if (next <= now || next > now + frame_ns) {
        next = now + frame_ns;
    }
    s->vblank_deadline_ns = next;
    timer_mod(s->vblank_timer, next);

    if (lipcr < totalh) {
        /* The line event belongs to the in-flight frame (start = next -
         * frame_ns); if that moment already passed, arm it for the next
         * frame instead of firing a stale one immediately. */
        int64_t line_t = next - frame_ns + lipcr * line_ns;
        timer_mod(s->line_timer, line_t > now ? line_t
                                               : next + lipcr * line_ns);
    } else {
        timer_del(s->line_timer);
    }
}

static void gnw_h7b0_ltdc_timing(GnwH7B0LtdcState *s, int64_t *frame_ns_out,
                                  int64_t *line_ns_out, uint32_t *lipcr_out,
                                  uint32_t *totalh_out)
{
    uint32_t twcr = s->regs[GNW_H7B0_LTDC_TWCR >> 2];
    /* TWCR packs both fields: TOTALH bits[10:0], TOTALW bits[27:16] --
     * see STM32H7B0.svd. */
    uint32_t totalh = (twcr & 0x7FF) + 1;
    uint32_t totalw = ((twcr >> 16) & 0xFFF) + 1;
    uint32_t lipcr = s->regs[GNW_H7B0_LTDC_LIPCR >> 2] & 0x7FF;

    if (totalh <= 1) {
        totalh = 240;
    }

    /*
     * Derive the real vblank rate from the live PLL3R frequency (LTDC's
     * pixel clock is hardwired to pll3_r_ck, no clock-source mux) and the
     * real panel timing, instead of assuming the fixed
     * GNW_H7B0_LTDC_VBLANK_HZ -- real firmware's PLL3 config (M=4,N=9,R=24)
     * against the real 392x255 panel timing computes to ~66.7Hz, not 60Hz,
     * and that ~10% mismatch against a fixed 60Hz tick was a suspected
     * cause of coverflow-menu flicker/scroll roughness (see the plan doc
     * for the full derivation). Falls back to the fixed constant if RCC
     * isn't wired or the computed rate is nonsensical -- same defensive
     * clamping philosophy used for TIM2/DMA/SPI timing elsewhere in this
     * device family.
     */
    /*
     * Exact fractional frame period, not a truncated-to-integer-Hz one:
     * stock Mario's real rate is 60.05Hz (pll3r 6.042MHz / 393x256), and
     * even the 0.08% loss from rounding that down to a flat 60Hz is a
     * systematic production shortfall for the vsync-paced audio producer
     * (see vblank_deadline_ns's doc comment).
     */
    int64_t frame_ns = NANOSECONDS_PER_SECOND / GNW_H7B0_LTDC_VBLANK_HZ;
    if (s->rcc && totalw > 0 && totalh > 0) {
        uint32_t pll3r_hz = gnw_h7b0_rcc_get_pll3r_hz(s->rcc);
        uint64_t pixels = (uint64_t)totalw * totalh;

        if (pll3r_hz > 0 && pixels > 0) {
            int64_t real_ns = muldiv64(pixels, NANOSECONDS_PER_SECOND,
                                        pll3r_hz);
            /* Same 1..1000Hz sanity window as before, in period form. */
            if (real_ns >= 1000000 && real_ns <= 1000000000) {
                frame_ns = real_ns;
            }
        }
    }

    *frame_ns_out = frame_ns;
    *line_ns_out = frame_ns / totalh;
    *lipcr_out = lipcr;
    *totalh_out = totalh;
}

void gnw_h7b0_ltdc_set_rcc(GnwH7B0LtdcState *s, GnwH7B0RccState *rcc)
{
    s->rcc = rcc;
}

static void gnw_h7b0_ltdc_vblank_tick(void *opaque)
{
    GnwH7B0LtdcState *s = GNW_H7B0_LTDC(opaque);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (gnw_timer_late_enabled()) {
        int64_t host_now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        static int64_t last_host;
        fprintf(stderr, "VBL late=%0.2fms host_dt=%0.2fms\n",
                (now - s->vblank_deadline_ns) / 1e6,
                last_host ? (host_now - last_host) / 1e6 : 0.0);
        last_host = host_now;
    }

    /* Headless timeline engine's @frame addressing counts these ticks
     * (no-op when GNW_TIMELINE has no frame-addressed entries). */
    gnw_timeline_notify_vblank();

    /*
     * GNW_LTDC_STATE: once-a-second dump of the publish state machine.
     * Added for the intermittent black-screen-on-return-to-menu bug,
     * where the guest keeps swapping buffers (GUESTFPS stays live) and
     * the UI keeps uploading, but this device stops publishing frames
     * entirely -- so the host keeps re-uploading one stale buffer. Both
     * capture sites (the SRCR VBR/IMR handlers) and the deferred retry
     * below are gated on flags that are individually plausible to latch;
     * this prints all of them so the stuck one names itself instead of
     * being guessed at. Per-second only -- per-event output here would
     * distort the very timing being diagnosed.
     */
    {
        static int state_trace = -1;
        if (state_trace < 0) {
            state_trace = gnw_env_enabled("GNW_LTDC_STATE");
        }
        if (state_trace) {
            static int64_t t0;
            if (now - t0 >= 1000000000LL) {
                bool job_busy, job_pending;

                qemu_mutex_lock(&s->job_lock);
                job_busy = s->job_busy;
                job_pending = s->job_pending;
                qemu_mutex_unlock(&s->job_lock);

                fprintf(stderr, "LTC state content_dirty=%d deferred=%d "
                        "vbr_active=%d vbr_pending=%d srcr_idle=%d "
                        "job_busy=%d job_pending=%d staged_valid=%d "
                        "compositor_running=%d invalidate=%d "
                        "structural=%d quiesce=%d/%d shadow=%dx%d\n",
                        s->content_dirty, s->vbr_deferred_capture,
                        s->vbr_active, s->vbr_reload_pending,
                        s->srcr_idle_ticks, job_busy, job_pending,
                        s->staged_valid, s->compositor_running,
                        s->invalidate, s->structural_transition_pending,
                        s->fb_quiesce_pending, s->fb_quiesce_ticks,
                        s->shadow_width, s->shadow_height);
                t0 = now;
            }
        }
    }

    /*
     * Capture the fully composed frame just before VBR reload takes
     * effect and the next frame begins. This guarantees we don't catch
     * the guest in the middle of drawing overlays to the frontbuffer.
     *
     * A progressive/scanline-timed version of this (spreading the
     * capture across the whole frame period like a real raster scan,
     * instead of one atomic read here) was tried and made things
     * *worse*: it let this device's own ~60Hz capture and QEMU's
     * independent ~33Hz UI-refresh blit interleave, so the blit could
     * catch shadow_buffer mid-capture -- some rows from this frame,
     * some still from last frame -- which is a tear QEMU introduces
     * itself, on top of (not instead of) whatever the guest's own
     * redraw timing does. Reverted. The real horizontal-scroll tearing
     * this was meant to fix needs understanding retro-go's actual
     * framebuffer-swap strategy during scrolling, not a generic capture-
     * timing change here.
     */
    /* We DO NOT capture here anymore.
     * Capturing here while the CPU is still drawing (because emulated CPU is slower
     * than 60Hz) captures partially drawn frames.
     * We only capture on VBR write.
     */

    /*
     * RRIF only belongs here when a VBR reload actually applies this
     * tick -- real hardware sets it on a genuine reload event, not on
     * every vblank. Setting it unconditionally (the previous behavior)
     * made firmware's HAL_LTDC_ReloadEventCallback() (which re-stages
     * CFBAR for double buffering) fire on every single vblank instead
     * of only when a swap was actually requested, since retro-go leaves
     * LTDC_IT_RR permanently enabled.
     */
    if (s->vbr_reload_pending) {
        gnw_h7b0_ltdc_reload_active(s);
        s->vbr_reload_pending = false;
        s->regs[GNW_H7B0_LTDC_SRCR >> 2] &= ~LTDC_SRCR_VBR;
        s->regs[GNW_H7B0_LTDC_ISR >> 2] |= LTDC_ISR_RRIF;
        gnw_h7b0_ltdc_update_irq(s);
    } else {
        /*
         * Auto-capture for firmware (like gnwmanager, or retro-go's own
         * main-menu UI) that doesn't reload via VBR at all -- it just
         * paints pixels directly into an already-configured Layer1/2.
         * Gated on vbr_active, which tracks "is the *current* screen/
         * config VBR-paced" rather than "has this device ever used VBR":
         * an idle-timeout version of this gate was tried and reverted --
         * real in-game firmware can leave multi-hundred-ms real gaps
         * between VBR writes on its own (see the SMW APU-catchup-burst
         * investigation), so any timeout short enough to un-stick the
         * menu promptly was also short enough to spuriously re-arm this
         * fallback mid-game during those same bursts, reintroducing
         * mid-draw tearing exactly when the game stutters. vbr_active is
         * instead reset by IMR reloads (see the SRCR write handler),
         * which real screen/config transitions actually use -- letting a
         * genuinely non-VBR screen (the menu) re-arm the fallback
         * immediately after taking over the layers, without depending on
         * any elapsed-time guess.
         */
        if (s->srcr_idle_ticks < GNW_H7B0_LTDC_SRCR_IDLE_TICKS_MAX) {
            s->srcr_idle_ticks++;
        }
        /*
         * A transition can end on a VBR-type reload (not IMR) with no
         * further reloads ever coming, permanently latching vbr_active
         * true with nothing left to reset it -- see srcr_idle_ticks'
         * doc comment in gnw_h7b0_ltdc.h. Once VBR has gone idle for
         * GNW_H7B0_LTDC_SRCR_IDLE_TICKS_THRESHOLD ticks, allow the
         * fallback even with vbr_active still true -- but ONLY if the
         * last reload also represented a genuine structural transition
         * (structural_transition_pending), not just an idle timer:
         * confirmed live that ordinary in-game frame-skipping suppresses
         * VBR reloads for the exact same duration as a real abandoned
         * transition, so the idle timer alone (even combined with the
         * RAM-dirty check) fires just as often during normal stutter,
         * reintroducing mid-draw tearing/flicker -- see
         * structural_transition_pending's doc comment in gnw_h7b0_ltdc.h.
         */
        bool vbr_idle = s->srcr_idle_ticks >= GNW_H7B0_LTDC_SRCR_IDLE_TICKS_THRESHOLD &&
                         s->structural_transition_pending;
        bool fallback_armed = (!s->vbr_active || vbr_idle) && !s->content_dirty &&
            gnw_h7b0_ltdc_enabled(s) &&
            !gnw_h7b0_ltdc_compositor_busy(s);
        bool fb_dirty = fallback_armed &&
            gnw_h7b0_ltdc_fb_dirty_check_and_clear(s);

        /*
         * Draw-quiescence debounce -- see fb_quiesce_pending's doc
         * comment in gnw_h7b0_ltdc.h. A dirty tick arms the capture; it
         * fires on the first clean tick after the draw burst ends (or
         * after the bounded wait expires, so continuous animation still
         * gets through at a reduced-but-live rate).
         */
        if (fb_dirty) {
            if (gnw_ltdc_trace_enabled()) {
                fprintf(stderr, "LTC dirty %" PRId64 "\n", now);
            }
            s->fb_quiesce_pending = true;
            s->fb_quiesce_ticks++;
        } else if (s->fb_quiesce_pending) {
            s->fb_quiesce_ticks = GNW_H7B0_LTDC_QUIESCE_TICKS_MAX;
        }
        if (fallback_armed && s->fb_quiesce_pending &&
            s->fb_quiesce_ticks >= GNW_H7B0_LTDC_QUIESCE_TICKS_MAX) {
            /*
             * Only clear fb_reg_dirty if the dispatch actually happened
             * (it always should here, given the !compositor_busy() check
             * just above -- gnw_h7b0_ltdc_capture_if_enabled() can still
             * legitimately return false for other reasons, e.g. an
             * unsupported Layer1 pixel format, in which case there is
             * nothing to redo next tick and leaving fb_reg_dirty set
             * would just harmlessly re-check RAM dirtiness again).
             */
            if (gnw_ltdc_trace_enabled()) {
                fprintf(stderr, "LTC capture %" PRId64 " ticks=%d\n", now,
                        s->fb_quiesce_ticks);
            }
            if (gnw_h7b0_ltdc_capture_if_enabled(s)) {
                GNW_LTDC_TRACE_CAP(s, "fallback");
                s->fb_reg_dirty = false;
            }
            s->fb_quiesce_pending = false;
            s->fb_quiesce_ticks = 0;
        }
    }

    /*
     * Retried every vblank tick (not just the one where vbr_reload_pending
     * just resolved) since the compositor worker thread may still have
     * been busy with a previous frame the first time this was attempted
     * (gnw_h7b0_ltdc_capture_if_enabled() returns false, without
     * dispatching, when job_busy) -- only clear the flag once a job is
     * actually dispatched, so a deferred capture is never silently lost
     * to worker-thread backpressure the way it could be if this only
     * fired once inside the vbr_reload_pending branch above.
     */
    /* Dispatch a staged reload-instant snapshot once the worker is free
     * -- see staged_job's doc comment in gnw_h7b0_ltdc.h. */
    gnw_h7b0_ltdc_promote_staged(s);

    if (s->vbr_deferred_capture) {
        if (gnw_h7b0_ltdc_capture_if_enabled(s)) {
            GNW_LTDC_TRACE_CAP(s, "deferred");
            s->vbr_deferred_capture = false;
        }
    }

    /*
     * Re-arm on the deadline grid, not at "now + frame_ns" -- see
     * vblank_deadline_ns's doc comment (and gnw_h7b0_dma_schedule_next(),
     * which this mirrors). Clamp only when more than a full frame
     * behind (host stall, debugger attach), so ordinary dispatch
     * latency never shifts the grid.
     */
    {
        int64_t frame_ns, line_ns;
        uint32_t lipcr, totalh;
        int64_t frame_start;

        gnw_h7b0_ltdc_timing(s, &frame_ns, &line_ns, &lipcr, &totalh);

        frame_start = s->vblank_deadline_ns;
        if (frame_start < now - frame_ns) {
            frame_start = now;
        }
        s->vblank_deadline_ns = frame_start + frame_ns;
        timer_mod(s->vblank_timer, s->vblank_deadline_ns);

        if (lipcr < totalh) {
            timer_mod(s->line_timer, frame_start + lipcr * line_ns);
        } else {
            timer_del(s->line_timer);
        }
    }
}

static void gnw_h7b0_ltdc_line_tick(void *opaque)
{
    GnwH7B0LtdcState *s = GNW_H7B0_LTDC(opaque);

    s->regs[GNW_H7B0_LTDC_ISR >> 2] |= LTDC_ISR_LIF;
    gnw_h7b0_ltdc_update_irq(s);
}

static void gnw_h7b0_ltdc_reset(DeviceState *dev)
{
    GnwH7B0LtdcState *s = GNW_H7B0_LTDC(dev);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    /*
     * Reset can run at any time, including with a compositor job still
     * in flight on the worker thread -- wait for it to finish and
     * publish before tearing down shadow_buffer/compositor_back_buffer/
     * clut below, or the worker's pending pointer swap/CLUT read could
     * race a free() here (a real use-after-free, not a style nit: reset
     * is BQL-synchronous but the worker thread is not).
     */
    if (s->compositor_running) {
        gnw_h7b0_ltdc_wait_compositor_idle(s);
    }

    for (int i = 0; i < (GNW_H7B0_LTDC_SIZE / 4); i++) {
        s->regs[i] = get_ltdc_reset_value(i * 4);
    }
    s->invalidate = 1;
    s->pf_warned = false;
    s->vbr_reload_pending = false;
    s->vbr_deferred_capture = false;
    s->vbr_active = false;
    s->srcr_idle_ticks = 0;
    s->structural_transition_pending = false;
    s->fb_quiesce_pending = false;
    s->fb_quiesce_ticks = 0;
    s->staged_valid = false;

    g_free(s->shadow_buffer);
    s->shadow_buffer = NULL;
    g_free(s->compositor_back_buffer);
    s->compositor_back_buffer = NULL;
    s->shadow_width = 0;
    s->shadow_height = 0;
    memset(s->clut, 0, sizeof(s->clut));
    memset(s->clut2, 0, sizeof(s->clut2));

    gnw_h7b0_ltdc_fb_track_range(&s->fb_l1_section, &s->fb_l1_track_base,
                                  &s->fb_l1_track_len, 0, 0);
    gnw_h7b0_ltdc_fb_track_range(&s->fb_l2_section, &s->fb_l2_track_base,
                                  &s->fb_l2_track_len, 0, 0);

    gnw_h7b0_ltdc_reload_active(s);
    gnw_h7b0_ltdc_recalc_timers(s, now);
}

static int gnw_h7b0_ltdc_layer_stride_bytes(uint32_t cfblr)
{
    int stride = (int)((cfblr >> LTDC_LxCFBLR_CFBP_SHIFT) &
                        LTDC_LxCFBLR_CFBP_MASK);
    int line_bytes = (int)(cfblr & LTDC_LxCFBLR_CFBLL_MASK);

    if (stride <= 0) {
        stride = line_bytes;
    }
    return stride;
}

/*
 * Bytes-per-pixel for Layer2's supported formats -- mirrors the l2_bpp
 * switch inside gnw_h7b0_ltdc_capture_rows() (kept as a separate literal
 * copy there rather than refactored to share this helper, to avoid
 * touching that already-working per-pixel hot path while adding this
 * dirty-tracking feature). Returns 0 for an unsupported/disabled format,
 * same convention as capture_rows()'s l2_bpp.
 */
static int gnw_h7b0_ltdc_l2_bpp(uint32_t l2_pfcr)
{
    switch (l2_pfcr) {
    case 0: return 4; /* ARGB8888 */
    case 2: case 3: case 4: return 2; /* RGB565/ARGB1555/ARGB4444 */
    case 5: return 1; /* L8 */
    case 6: return 1; /* AL44 */
    default: return 0;
    }
}

/*
 * Track (and rebind, only when the underlying range actually changes)
 * one framebuffer range's MemoryRegionSection for DIRTY_MEMORY_VGA
 * dirty-bitmap tracking, same pattern as pl110.c's s->fbsection via
 * framebuffer_update_memory_section() -- see that function's doc
 * comment in hw/display/framebuffer.h. Rebinding toggles
 * memory_region_set_log() when the logging refcount transitions
 * 0<->1, which can trigger a memory-transaction/TLB update, so this
 * must only run when `base`/`len` actually change from what's already
 * tracked (checked via cur_base/cur_len), not unconditionally every
 * tick -- pl110_update_display() only calls the equivalent on
 * s->invalidate for the same reason.
 */
static void gnw_h7b0_ltdc_fb_track_range(MemoryRegionSection *section,
                                          hwaddr *cur_base, hwaddr *cur_len,
                                          hwaddr base, hwaddr len)
{
    if (len == 0) {
        /*
         * Tear down directly instead of calling
         * framebuffer_update_memory_section(..., 0, 0, 0) -- that helper
         * calls memory_region_find(root, 0, 0) for the "new" section
         * whenever the old one is non-NULL, and a zero-size lookup at
         * address 0 can still match whatever real RAM region happens to
         * sit at guest physical address 0 (e.g. this SoC's boot flash
         * alias), silently turning DIRTY_MEMORY_VGA logging back on for
         * an unrelated region instead of actually leaving nothing
         * tracked. Confirmed via profiling: this bug meant the "torn
         * down" logging was never really gone, so the intended teardown
         * fix had no effect.
         */
        if (section->mr) {
            memory_region_set_log(section->mr, false, DIRTY_MEMORY_VGA);
            memory_region_unref(section->mr);
            section->mr = NULL;
        }
        *cur_base = 0;
        *cur_len = 0;
        return;
    }

    if (section->mr && *cur_base == base && *cur_len == len) {
        return;
    }

    /*
     * GNW_LTDC_NO_DIRTY_LOG=1: skip DIRTY_MEMORY_VGA tracking entirely and
     * treat every frame as dirty (gnw_h7b0_ltdc_fb_range_dirty() already
     * returns true for a NULL section->mr, by its "cannot prove clean =>
     * dirty" rule). A/B knob for the cost of the tracking itself.
     *
     * Why it can be expensive here: framebuffer_update_memory_section()
     * enables logging on the whole MemoryRegion backing the framebuffer
     * (hw/display/framebuffer.c), and this SoC maps ALL 1MB of AXI SRAM as
     * a single region (gnw_h7b0_soc.c). So tracking the framebuffer marks
     * every AXI SRAM page VGA-clean, physical_memory_is_clean() then
     * reports the whole region clean, and tlb_set_page() sets TLB_NOTDIRTY
     * on all of it -- putting every guest store to emulator RAM through
     * notdirty_write() + page_collection_lock() (which allocates and frees
     * a GTree per store). Measured on the 8086tiny DOS core: 65% of host
     * cycles in that path.
     */
    if (gnw_env_enabled("GNW_LTDC_NO_DIRTY_LOG")) {
        if (section->mr) {
            memory_region_set_log(section->mr, false, DIRTY_MEMORY_VGA);
            memory_region_unref(section->mr);
            section->mr = NULL;
        }
        *cur_base = base;
        *cur_len = len;
        return;
    }

    framebuffer_update_memory_section(section, get_system_memory(), base, 1,
                                       len);
    *cur_base = base;
    *cur_len = len;
}

/*
 * Check (and clear, so subsequent guest writes are what's tracked for
 * next time) whether any byte in one tracked framebuffer range has been
 * written since the last check. Returns true (dirty) whenever the range
 * couldn't be resolved to plain guest RAM at all -- e.g.
 * framebuffer_update_memory_section() found no backing region, a region
 * smaller than requested, or a non-RAM region (MMIO alias, etc). Per
 * this feature's explicit design constraint: a missed dirty write would
 * freeze the screen permanently, so any case we can't positively prove
 * clean must be treated as dirty.
 */
static bool gnw_h7b0_ltdc_fb_range_dirty(MemoryRegionSection *section,
                                          hwaddr len)
{
    DirtyBitmapSnapshot *snap;
    MemoryRegion *mr;
    hwaddr addr;
    bool dirty;

    if (len == 0) {
        return false;
    }
    /*
     * memory_region_snapshot_and_clear_dirty()/_get_dirty() both do
     * assert(mr->ram_block) internally -- fine in a debug build (loud
     * abort), but assertions compile out under NDEBUG (this project's
     * release builds), silently proceeding into undefined behavior on
     * a non-RAM-backed region instead. Confirmed via a real,
     * reproducible segfault in a release/PGO build at this exact call
     * site (deterministic: identical link-time crash address across
     * multiple runs) -- check memory_region_is_ram() ourselves, a real
     * runtime check that can't be compiled away, instead of trusting
     * QEMU's own assert to catch this.
     */
    mr = section->mr;
    if (!mr || !memory_region_is_ram(mr)) {
        return true;
    }
    addr = section->offset_within_region;

    /*
     * *section must NOT be re-read across the snapshot call below.
     * memory_region_snapshot_and_clear_dirty() ends in
     * memory_global_after_dirty_log_sync() -> (TCG's log_global_after_sync
     * listener) -> do_run_on_cpu(), which *drops the BQL* while it waits
     * on qemu_work_cond for the vCPU to run the queued work item. In that
     * window the vCPU thread takes the BQL and can service a guest LTDC
     * register write; gnw_h7b0_ltdc_write()'s VBR-teardown path calls
     * gnw_h7b0_ltdc_fb_track_range(..., 0, 0), which unrefs the region
     * and sets section->mr = NULL right underneath us. Re-reading
     * section->mr afterwards then passed NULL to
     * memory_region_snapshot_get_dirty() -> SIGSEGV.
     *
     * Diagnosed on a Raspberry Pi 400 (4-core aarch64, where the vCPU
     * thread is very likely to be parked on the BQL exactly then):
     * confirmed with a hardware watchpoint on section->mr naming the
     * "CPU 0/TCG" thread as the writer, and with breakpoints proving the
     * pointer changes non-NULL -> NULL *inside* the snapshot call.
     *
     * So: keep our own reference to the region for the duration (it may
     * be unref'd to death otherwise), and if the tracking was retargeted
     * or torn down under us, report dirty -- per this function's
     * "anything we can't positively prove clean is dirty" rule.
     */
    memory_region_ref(mr);
    snap = memory_region_snapshot_and_clear_dirty(mr, addr, len,
                                                   DIRTY_MEMORY_VGA);
    if (!snap) {
        memory_region_unref(mr);
        return true;
    }
    if (section->mr != mr) {
        dirty = true;
    } else {
        dirty = memory_region_snapshot_get_dirty(mr, snap, addr, len);
    }
    g_free(snap);
    memory_region_unref(mr);
    return dirty;
}

/*
 * Primary dirty-tracking check for gnw_h7b0_ltdc_vblank_tick()'s non-VBR
 * auto-capture fallback (see that function's else-branch and
 * fb_reg_dirty's doc comment in gnw_h7b0_ltdc.h for full rationale).
 * Checks Layer1's and (if enabled) Layer2's actual guest-RAM
 * framebuffer ranges via the RAM dirty bitmap -- NOT just LTDC register
 * writes -- since firmware commonly paints new pixels directly into an
 * already-configured framebuffer without touching any LTDC register
 * again. ORs in fb_reg_dirty as an additional (not primary) signal for
 * register-only composition changes (e.g. a window move) that wouldn't
 * touch the framebuffer itself. Only called when the fallback is about
 * to actually consume a capture decision (see call site), so it's safe
 * for this call to also be the point where tracked ranges are
 * (re)bound and the dirty bitmap is cleared.
 */
static bool gnw_h7b0_ltdc_fb_dirty_check_and_clear(GnwH7B0LtdcState *s)
{
    uint32_t pfcr = s->active_l1pfcr & LTDC_LxPFCR_PF_MASK;
    bool l1_l8 = (pfcr == LTDC_PF_L8);
    int cols = s->shadow_width;
    int rows = s->shadow_height;
    bool l1_dirty, l2_dirty = false;
    hwaddr l1_len;

    /* Shadow buffer not sized yet (e.g. never captured before) -- can't
     * know the range, so force a capture attempt. */
    if (cols <= 0 || rows <= 0) {
        return true;
    }

    l1_len = (hwaddr)(cols * (l1_l8 ? 1 : 2)) * rows;
    gnw_h7b0_ltdc_fb_track_range(&s->fb_l1_section, &s->fb_l1_track_base,
                                  &s->fb_l1_track_len, s->active_l1cfbar,
                                  l1_len);
    l1_dirty = gnw_h7b0_ltdc_fb_range_dirty(&s->fb_l1_section, l1_len);

    if (s->active_l2cr & LTDC_LxCR_LEN) {
        uint32_t l2_pfcr = s->active_l2pfcr & LTDC_LxPFCR_PF_MASK;
        int l2_bpp = gnw_h7b0_ltdc_l2_bpp(l2_pfcr);

        if (l2_bpp > 0) {
            int l2_src_width =
                gnw_h7b0_ltdc_layer_stride_bytes(s->active_l2cfblr);
            hwaddr l2_len = (hwaddr)l2_src_width * rows;

            gnw_h7b0_ltdc_fb_track_range(&s->fb_l2_section,
                                          &s->fb_l2_track_base,
                                          &s->fb_l2_track_len,
                                          s->active_l2cfbar, l2_len);
            l2_dirty = gnw_h7b0_ltdc_fb_range_dirty(&s->fb_l2_section,
                                                     l2_len);
        } else {
            gnw_h7b0_ltdc_fb_track_range(&s->fb_l2_section,
                                          &s->fb_l2_track_base,
                                          &s->fb_l2_track_len, 0, 0);
        }
    } else {
        gnw_h7b0_ltdc_fb_track_range(&s->fb_l2_section, &s->fb_l2_track_base,
                                      &s->fb_l2_track_len, 0, 0);
    }

    return l1_dirty || l2_dirty || s->fb_reg_dirty;
}

static inline uint32_t gnw_h7b0_ltdc_rgb565_to_pixel32(uint16_t px)
{
    unsigned int r = ((px >> 11) & 0x1f) << 3;
    unsigned int g = ((px >> 5) & 0x3f) << 2;
    unsigned int b = (px & 0x1f) << 3;
    unsigned int r8 = r | (r >> 5);
    unsigned int g8 = g | (g >> 6);
    unsigned int b8 = b | (b >> 5);

    return 0xFF000000U | (r8 << 16) | (g8 << 8) | b8;
}

/*
 * Decodes one already-fetched pixel from `buf` (part of a row batch-read by
 * the caller) -- see gnw_h7b0_ltdc_capture_rows()'s l2_linebuf comment for
 * why this takes an in-memory buffer instead of a guest physical address
 * plus its own cpu_physical_memory_read() call per pixel.
 */
static uint32_t gnw_h7b0_ltdc_read_pixel_buf(const uint8_t *buf, uint32_t pfcr)
{
    switch (pfcr) {
    case 0: /* ARGB8888 */
        return ldl_le_p(buf);
    case 2: /* RGB565 */
        return gnw_h7b0_ltdc_rgb565_to_pixel32(lduw_le_p(buf));
    case 3: /* ARGB1555 */
    {
        uint16_t px = lduw_le_p(buf);
        unsigned int a1 = (px >> 15) & 0x1;
        unsigned int r5 = (px >> 10) & 0x1F;
        unsigned int g5 = (px >> 5) & 0x1F;
        unsigned int b5 = px & 0x1F;
        unsigned int r8 = (r5 << 3) | (r5 >> 2);
        unsigned int g8 = (g5 << 3) | (g5 >> 2);
        unsigned int b8 = (b5 << 3) | (b5 >> 2);
        return (a1 ? 0xFF000000U : 0U) | (r8 << 16) | (g8 << 8) | b8;
    }
    case 4: /* ARGB4444 */
    {
        uint16_t px = lduw_le_p(buf);
        unsigned int a4 = (px >> 12) & 0xF;
        unsigned int r4 = (px >> 8) & 0xF;
        unsigned int g4 = (px >> 4) & 0xF;
        unsigned int b4 = px & 0xF;
        return ((a4 * 0x11U) << 24) | ((r4 * 0x11U) << 16) |
               ((g4 * 0x11U) << 8) | (b4 * 0x11U);
    }
    default:
        return 0;
    }
}

/*
 * Validate Layer1's format/geometry, (re)size the shadow buffer if
 * needed, and return the row count to capture this frame (0 if
 * disabled/unsupported -- caller should skip scheduling capture
 * ticks). Layer2/Layer1 CFBAR etc. are all read from s->active_* here
 * and in gnw_h7b0_ltdc_capture_rows() below; those only change on an
 * SRCR reload (applied at the vblank tick that also calls this), so
 * they're stable for the whole frame and safe to re-read per capture
 * step without caching.
 */
static int gnw_h7b0_ltdc_capture_setup(GnwH7B0LtdcState *s)
{
    uint32_t cfblr, cfblnr, pfcr;
    int cols, rows, src_width;

    if (!gnw_h7b0_ltdc_enabled(s)) {
        return 0;
    }

    pfcr = s->active_l1pfcr & LTDC_LxPFCR_PF_MASK;
    if (pfcr != LTDC_PF_RGB565 && pfcr != LTDC_PF_L8) {
        if (!s->pf_warned) {
            qemu_log_mask(LOG_UNIMP,
                          "gnw_h7b0_ltdc: only RGB565/L8 are supported for Layer 1 PFCR "
                          "requested format %u -- nothing will be shown\n",
                          pfcr);
            s->pf_warned = true;
        }
        return 0;
    }

    if (!(s->active_l1cr & LTDC_LxCR_LEN)) {
        return 0; /* Layer 1 must be enabled for base frame */
    }

    cfblr = s->active_l1cfblr;
    cfblnr = s->active_l1cfblnr & 0x7FFU;

    src_width = gnw_h7b0_ltdc_layer_stride_bytes(cfblr);
    cols = src_width / (pfcr == LTDC_PF_L8 ? 1 : 2);
    rows = (int)cfblnr;

    if (cols <= 0 || rows <= 0) {
        return 0;
    }

    if (cols != s->shadow_width || rows != s->shadow_height) {
        /*
         * Only reachable when the compositor worker thread is provably
         * idle (see gnw_h7b0_ltdc_capture_if_enabled()'s job_busy check,
         * always performed before this function is called), so neither
         * buffer can be concurrently read/written by the worker here.
         * Still take compositor_publish_lock around the shadow_buffer/
         * dimensions update, since gnw_h7b0_ltdc_update_display() (main
         * thread) may be reading the old pointer/dims right up until
         * this resize.
         */
        qemu_mutex_lock(&s->compositor_publish_lock);
        g_free(s->shadow_buffer);
        s->shadow_buffer = g_new0(uint32_t, cols * rows);
        s->shadow_width = cols;
        s->shadow_height = rows;
        qemu_mutex_unlock(&s->compositor_publish_lock);

        g_free(s->compositor_back_buffer);
        s->compositor_back_buffer = g_new0(uint32_t, cols * rows);
    }

    return rows;
}

/*
 * Generalized STM32H7 LTDC blend: applies LxBFCR's BF1/BF2 fields
 * literally rather than assuming a fixed PAxCA-with-complement formula.
 * BF1 (this layer's own weight) is either the layer's constant alpha
 * (CACR) alone (register value 0x4/0x5, LTDC_LxBFCR_MODE_PA clear) or
 * pixel-alpha x constant-alpha (0x6/0x7, MODE_PA set). BF2 is the
 * complement of that same computation applied to what's already
 * composited below -- real hardware's default reset value for both
 * fields is the PAxCA pair (BFCR=0x607), which is exactly the classic
 * "over" operator this produces: factor1 = pa*ca/255, factor2 =
 * 255-factor1. For the common case (CACR=255, pa=255, i.e. an opaque
 * RGB565/L8 source with default blend factors) factor1=255/factor2=0,
 * so the result is the foreground pixel unmodified -- matching the
 * previous hardcoded behavior exactly.
 */
static inline __attribute__((always_inline))
uint32_t gnw_h7b0_ltdc_blend_over(uint32_t fg, unsigned int fg_a,
                                          uint32_t bfcr, unsigned int ca,
                                          uint32_t bg)
{
    unsigned int bf1 = (bfcr >> LTDC_LxBFCR_BF1_SHIFT) & LTDC_LxBFCR_BF1_MASK;
    unsigned int bf2 = bfcr & LTDC_LxBFCR_BF2_MASK;

    /*
     * Fast paths for the two overwhelmingly common cases in any
     * alpha-blended overlay (anti-aliased text/UI: mostly fully
     * transparent or fully opaque pixels, with only edge pixels
     * partially blended) -- skips the divisions below entirely. Only
     * safe when both factors use PA (per-pixel-alpha) mode -- the
     * default/reset BFCR value and the only mode retro-go/stock
     * firmware's AL44 overlays configure (see the file comment above) --
     * since with constant-alpha (non-PA) mode a "fully transparent"
     * source pixel can still contribute via a fixed ca, and skipping the
     * general path would silently drop that contribution.
     */
    if ((bf1 & LTDC_LxBFCR_MODE_PA) && (bf2 & LTDC_LxBFCR_MODE_PA)) {
        if (fg_a == 0) {
            return bg;
        }
        if (fg_a == 255 && ca == 255) {
            return fg;
        }
    }

    unsigned int factor1 = (bf1 & LTDC_LxBFCR_MODE_PA) ? (fg_a * ca) / 255U : ca;
    unsigned int factor2_base = (bf2 & LTDC_LxBFCR_MODE_PA) ? (fg_a * ca) / 255U : ca;
    unsigned int factor2 = 255U - factor2_base;

    unsigned int fg_r = (fg >> 16) & 0xFF, fg_g = (fg >> 8) & 0xFF, fg_b = fg & 0xFF;
    unsigned int bg_r = (bg >> 16) & 0xFF, bg_g = (bg >> 8) & 0xFF, bg_b = bg & 0xFF;

    unsigned int r = MIN(255U, (fg_r * factor1 + bg_r * factor2) / 255U);
    unsigned int g = MIN(255U, (fg_g * factor1 + bg_g * factor2) / 255U);
    unsigned int b = MIN(255U, (fg_b * factor1 + bg_b * factor2) / 255U);

    return 0xFF000000U | (r << 16) | (g << 8) | b;
}

/*
 * Resolve one layer's raw fetched pixel (already-converted ARGB, native
 * alpha for ARGB formats or 255 for opaque RGB565/L8) against that
 * layer's window-clip and color-key configuration, returning the
 * effective ARGB pixel to hand to gnw_h7b0_ltdc_blend_over() -- outside
 * the layer's window it's DCCR's default color+alpha instead of
 * framebuffer content (DCCR's bit layout, alpha[31:24]/RGB[23:16:8:0],
 * is already a valid ARGB32 value); a color-key match forces alpha to 0
 * so the pixel is fully transparent (falls through to what's below),
 * independent of that layer's blend-factor mode.
 */
static uint32_t gnw_h7b0_ltdc_resolve_layer(uint32_t raw_px, bool in_window,
                                             uint32_t dccr, bool colken,
                                             uint32_t ckcr,
                                             unsigned int *out_alpha)
{
    uint32_t px;
    unsigned int alpha;

    if (!in_window) {
        px = dccr;
        alpha = (dccr >> 24) & 0xFF;
    } else if (colken && (raw_px & 0xFFFFFFU) == (ckcr & 0xFFFFFFU)) {
        px = raw_px;
        alpha = 0;
    } else {
        px = raw_px;
        alpha = (raw_px >> 24) & 0xFF;
    }

    *out_alpha = alpha;
    return px;
}

/*
 * Row-level SIMD fast path for the single most common per-pixel case in
 * this loop: Layer1 RGB565, fully opaque, default (PAxCA) blend factors,
 * no color-key, no dithering, and the whole row inside Layer1's window --
 * i.e. ordinary in-game rendering with no overlay active. See the gate
 * check inlined at its call site in gnw_h7b0_ltdc_capture_rows() for the
 * exact conditions; anything that doesn't match falls through to the
 * general per-pixel path unchanged (bit-for-bit -- verified against a
 * standalone host-side harness comparing every possible RGB565 value).
 *
 * Only RGB565->ARGB8888 unpack is vectorized (the L8/CLUT and Layer2
 * paths involve per-pixel gathers/branches that don't vectorize cleanly
 * with SSE2 and aren't worth the risk here) -- gated to hosts with SSE2
 * (baseline for every x86_64 target QEMU supports); every other host
 * arch/config uses the identical-math scalar fallback below.
 */
#if defined(__SSE2__)
#include <emmintrin.h>


#define GNW_H7B0_LTDC_HAVE_SSE2_ROW_CONVERT 1
#endif

#ifdef GNW_H7B0_LTDC_HAVE_SSE2_ROW_CONVERT
/*
 * Converts `cols` RGB565 pixels from `src` (2 bytes/pixel, native-endian
 * guest already batch-read into host memory) into ARGB8888 `dst`, 8
 * pixels/iteration via SSE2. Tail (< 8 remaining pixels) handled scalar.
 *
 * Deliberately keeps the whole computation in 16-bit lanes (16 pixels'
 * worth of R/G/B fit two per 32-bit slot) and only widens to bytes right
 * before interleaving into the ARGB output, instead of the more "obvious"
 * per-pixel-in-a-32-bit-lane approach (widen to 32 bits immediately, do
 * all the shift/mask math there, 4 pixels/vector). That first version was
 * actually built and benchmarked first -- it was *slower* than the plain
 * scalar loop (~0.82x) on this host: the widen-to-32/narrow-back-down
 * round trip plus only 4 pixels/vector wasn't enough work to amortize the
 * shuffle overhead against an out-of-order CPU already executing the
 * independent-per-pixel scalar loop at good IPC. This 16-bit-lane, 8-wide
 * version (measured ~1.5x) is the one actually shipped -- see this
 * session's doc for both benchmarks side by side. AVX2 was also tried
 * (16 pixels/vector) and rejected: cross-128-bit-lane shuffles needed to
 * reassemble the output made it *inconsistently* faster/slower than
 * scalar across repeated runs, not a reliable win.
 */
static void gnw_h7b0_ltdc_rgb565_row_to_argb8888_sse2(const uint8_t *src,
                                                       uint32_t *dst,
                                                       int cols)
{
    const __m128i mask5 = _mm_set1_epi16(0x1F);
    const __m128i mask6 = _mm_set1_epi16(0x3F);
    const __m128i zero = _mm_setzero_si128();
    const __m128i alpha_bytes = _mm_set1_epi8((char)0xFF);
    int x = 0;

    for (; x + 8 <= cols; x += 8) {
        __m128i px = _mm_loadu_si128((const __m128i *)(src + x * 2));

        __m128i r5 = _mm_and_si128(_mm_srli_epi16(px, 11), mask5);
        __m128i g6 = _mm_and_si128(_mm_srli_epi16(px, 5), mask6);
        __m128i b5 = _mm_and_si128(px, mask5);

        __m128i r8 = _mm_or_si128(_mm_slli_epi16(r5, 3),
                                   _mm_srli_epi16(r5, 2));
        __m128i g8 = _mm_or_si128(_mm_slli_epi16(g6, 2),
                                   _mm_srli_epi16(g6, 4));
        __m128i b8 = _mm_or_si128(_mm_slli_epi16(b5, 3),
                                   _mm_srli_epi16(b5, 2));

        /* Each of r8/g8/b8 holds one valid byte (0-255) per 16-bit lane,
         * high byte zero -- packus_epi16 against an all-zero vector packs
         * the 8 low bytes down contiguously without any saturation
         * actually occurring (values already fit in 0-255). */
        __m128i r8b = _mm_packus_epi16(r8, zero);
        __m128i g8b = _mm_packus_epi16(g8, zero);
        __m128i b8b = _mm_packus_epi16(b8, zero);

        /* Interleave to B,G,R,A byte order (little-endian ARGB8888 in
         * memory is B,G,R,A) via two rounds of unpacklo at byte then
         * 16-bit granularity -- standard planar-to-interleaved pattern. */
        __m128i bg = _mm_unpacklo_epi8(b8b, g8b);
        __m128i ra = _mm_unpacklo_epi8(r8b, alpha_bytes);
        __m128i lo = _mm_unpacklo_epi16(bg, ra);
        __m128i hi = _mm_unpackhi_epi16(bg, ra);

        _mm_storeu_si128((__m128i *)(dst + x), lo);
        _mm_storeu_si128((__m128i *)(dst + x + 4), hi);
    }

    for (; x < cols; x++) {
        dst[x] = gnw_h7b0_ltdc_rgb565_to_pixel32(lduw_le_p(src + x * 2));
    }
}
#endif

#ifndef GNW_H7B0_LTDC_HAVE_SSE2_ROW_CONVERT
/* Scalar fallback/reference, identical math to the SSE2 path above --
 * used on non-x86_64 (or SSE2-less) hosts. Only compiled in when the
 * SSE2 path itself isn't available, to avoid an unused-function error
 * on the (baseline x86_64) builds that always take the SSE2 path;
 * kept as its own standalone function (not inlined into the dispatcher
 * below) since it also serves as the correctness oracle in the
 * standalone host-side test harness used to verify the SSE2 path. */
static void gnw_h7b0_ltdc_rgb565_row_to_argb8888_scalar(const uint8_t *src,
                                                         uint32_t *dst,
                                                         int cols)
{
    for (int x = 0; x < cols; x++) {
        dst[x] = gnw_h7b0_ltdc_rgb565_to_pixel32(lduw_le_p(src + x * 2));
    }
}
#endif

/*
 * Row unpack for a CLUT-indexed (L8) Layer1: one table lookup per pixel.
 * `table` is NOT the CLUT -- it is the per-job 256-entry table of
 * *fully composited Layer1 results* built by
 * gnw_h7b0_ltdc_build_l8_row_table() below, so this loop emits the final
 * `below` value directly. Deliberately scalar: SSE2 has no gather, and a
 * manual 4-wide gather issues the same four loads as this loop plus
 * insert/shuffle overhead. The win is not SIMD, it is hoisting the
 * resolve_layer()/blend_over() pair out of the per-pixel loop and into a
 * 256-entry precompute.
 */
static void gnw_h7b0_ltdc_l8_row_to_argb8888(const uint8_t *src,
                                              const uint32_t *table,
                                              uint32_t *dst, int cols)
{
    for (int x = 0; x < cols; x++) {
        dst[x] = table[src[x]];
    }
}

static void gnw_h7b0_ltdc_rgb565_row_to_argb8888(const uint8_t *src,
                                                  uint32_t *dst, int cols)
{
#ifdef GNW_H7B0_LTDC_HAVE_SSE2_ROW_CONVERT
    gnw_h7b0_ltdc_rgb565_row_to_argb8888_sse2(src, dst, cols);
#else
    gnw_h7b0_ltdc_rgb565_row_to_argb8888_scalar(src, dst, cols);
#endif
}

/*
 * True iff BFCR's BF1/BF2 are both the PAxCA-mode encoding used by
 * default-reset firmware config (0x607) -- the mode
 * gnw_h7b0_ltdc_blend_over()'s own fast path requires (see its comment)
 * before the row fast path below can skip calling it at all.
 */
static inline bool gnw_h7b0_ltdc_bfcr_is_pa_pa(uint32_t bfcr)
{
    unsigned int bf1 = (bfcr >> LTDC_LxBFCR_BF1_SHIFT) & LTDC_LxBFCR_BF1_MASK;
    unsigned int bf2 = bfcr & LTDC_LxBFCR_BF2_MASK;

    return (bf1 & LTDC_LxBFCR_MODE_PA) && (bf2 & LTDC_LxBFCR_MODE_PA);
}

static const int gnw_h7b0_ltdc_bayer4x4[4][4] = {
    { 0,  8,  2, 10 },
    {12,  4, 14,  6 },
    { 3, 11,  1,  9 },
    {15,  7, 13,  5 },
};

/*
 * Standard 4x4 Bayer ordered dither, gated on GCR.DEN. Confirmed a true
 * no-op for this model today: shadow_buffer and the host display surface
 * are both full 32bpp/8-bit-per-channel, so nothing downstream ever
 * quantizes bit depth (unlike a real TFT panel, which this hardware
 * feature targets). Implemented anyway for fidelity/future-proofing (per
 * explicit request), using the same truncate-then-replicate-low-bits
 * re-expansion pattern as gnw_h7b0_ltdc_rgb565_to_pixel32().
 */
static inline uint8_t gnw_h7b0_ltdc_dither_channel(uint8_t value, int px, int py)
{
    const int n = 6; /* assumed panel bit depth (e.g. RGB666) */
    const int step = 1 << (8 - n);
    int d = gnw_h7b0_ltdc_bayer4x4[py & 3][px & 3];
    int adjusted = (int)value + (d * step) / 16 - step / 2;
    unsigned int truncated;
    unsigned int nbit;

    adjusted = MAX(0, MIN(255, adjusted));
    truncated = ((unsigned int)adjusted) & ~(unsigned int)(step - 1);
    nbit = truncated >> (8 - n);

    return (uint8_t)((nbit << (8 - n)) | (nbit >> (2 * n - 8)));
}

static inline uint32_t gnw_h7b0_ltdc_dither_pixel(uint32_t px, int x, int y)
{
    unsigned int r = gnw_h7b0_ltdc_dither_channel((px >> 16) & 0xFF, x, y);
    unsigned int g = gnw_h7b0_ltdc_dither_channel((px >> 8) & 0xFF, x, y);
    unsigned int b = gnw_h7b0_ltdc_dither_channel(px & 0xFF, x, y);

    return (px & 0xFF000000U) | (r << 16) | (g << 8) | b;
}

/*
 * Cheap (memcpy-only, no per-pixel conversion) synchronous snapshot of
 * everything the compositor worker thread needs, taken on the BQL-
 * holding producer thread. Fills s->compositor_job and hands it to the
 * worker; the expensive per-pixel work happens in
 * gnw_h7b0_ltdc_composite_from_job() below, entirely off this thread.
 * Only called when job_busy is already known false (see
 * gnw_h7b0_ltdc_capture_if_enabled()), so s->compositor_job is not
 * touched concurrently by the worker while this runs. Batches each
 * layer's entire row span into one cpu_physical_memory_read() call, same
 * as the synchronous capture_rows() this replaces used to (see that
 * function's git history for the perf rationale -- one read per capture
 * instead of one per scanline).
 */
static void gnw_h7b0_ltdc_snapshot_into(GnwH7B0LtdcState *s,
                                         GnwH7B0LtdcCaptureJob *job, int rows)
{
    int cols = s->shadow_width;
    uint32_t pfcr = s->active_l1pfcr & LTDC_LxPFCR_PF_MASK;
    bool l1_l8 = (pfcr == LTDC_PF_L8);
    int l1_src_width = cols * (l1_l8 ? 1 : 2);

    bool l2_en = s->active_l2cr & LTDC_LxCR_LEN;
    uint32_t l2_pfcr = s->active_l2pfcr & LTDC_LxPFCR_PF_MASK;
    int l2_src_width = gnw_h7b0_ltdc_layer_stride_bytes(s->active_l2cfblr);
    int l2_bpp = 0;
    /*
     * l2_src_width > 0 is required, not just l2_en: firmware can leave
     * Layer2 enabled (LEN set) with CFBLR still at its reset/zero value
     * mid-transition (e.g. reconfiguring Layer2 for JPEG-decoded cover
     * art), which makes l2_src_width 0. Without this check l2_bpp would
     * still come out > 0 for a recognized pixel format below, but
     * job->l2_raw is allocated as g_malloc(l2_src_width * rows) == 0
     * bytes == NULL (g_malloc(0) is valid and returns NULL), and
     * l2_linebuf derived from it in gnw_h7b0_ltdc_composite_from_job()
     * would stay NULL for every row while still gating only on
     * l2_en && l2_bpp > 0 -- the real cause of a confirmed host segfault
     * in an inlined 2-byte pixel load (lduw_he_p()) once real firmware
     * sequences started actually hitting this transient zero-stride
     * state.
     */
    if (l2_en && l2_src_width > 0) {
        switch (l2_pfcr) {
        case 0: l2_bpp = 4; break;
        case 2: case 3: case 4: l2_bpp = 2; break;
        case 5: l2_bpp = 1; break; /* L8 */
        case 6: l2_bpp = 1; break; /* AL44 */
        default: l2_bpp = 0; break;
        }
    }
    /*
     * Layer2's own column count, derived from its own stride/bpp -- NOT
     * assumed equal to Layer1's `cols` (shadow_width). Real firmware can
     * configure Layer2 with a narrower CFBLR/stride than Layer1's (e.g.
     * a smaller cover-art overlay composited over a full-width
     * background), and the compositor worker's per-pixel loop is driven
     * by Layer1's `cols`. Without this bound, `x` can run past how many
     * whole pixels job->l2_raw actually holds (l2_src_width bytes per
     * row) -- a real, confirmed second host segfault (a 4-byte ARGB8888
     * read via gnw_h7b0_ltdc_read_pixel_buf()) distinct from the
     * l2_src_width==0 NULL-pointer case the l2_bpp assignment's own
     * guard above handles. Zero when l2_bpp is 0 so it never wrongly
     * reads as "in bounds" for an unsupported/disabled format. Stored
     * into the job (see below) since gnw_h7b0_ltdc_composite_from_job()
     * runs on the worker thread and can't recompute it from live `s->`
     * register state.
     */
    int l2_cols = l2_bpp > 0 ? l2_src_width / l2_bpp : 0;

    job->cols = cols;
    job->rows = rows;

    job->active_l1cr = s->active_l1cr;
    job->active_l1pfcr = s->active_l1pfcr;
    job->active_l1cfblr = s->active_l1cfblr;
    job->active_l1ckcr = s->active_l1ckcr;
    job->active_l1dccr = s->active_l1dccr;
    job->active_l1bfcr = s->active_l1bfcr;
    job->active_l1cacr = s->active_l1cacr;
    job->active_l1whpcr = s->active_l1whpcr;
    job->active_l1wvpcr = s->active_l1wvpcr;

    job->l2_en = l2_en;
    job->active_l2cr = s->active_l2cr;
    job->active_l2pfcr = s->active_l2pfcr;
    job->active_l2cfblr = s->active_l2cfblr;
    job->active_l2ckcr = s->active_l2ckcr;
    job->active_l2dccr = s->active_l2dccr;
    job->active_l2bfcr = s->active_l2bfcr;
    job->active_l2cacr = s->active_l2cacr;
    job->active_l2whpcr = s->active_l2whpcr;
    job->active_l2wvpcr = s->active_l2wvpcr;

    job->active_bccr = s->active_bccr;
    job->active_gcr = s->active_gcr;
    job->active_bpcr = s->active_bpcr;

    job->l1_src_width = l1_src_width;
    job->l2_src_width = l2_src_width;
    job->l2_bpp = l2_bpp;
    job->l2_cols = l2_cols;

    size_t l1_needed = (size_t)l1_src_width * rows;
    if (job->l1_raw_size < l1_needed) {
        g_free(job->l1_raw);
        job->l1_raw = g_malloc(l1_needed);
        job->l1_raw_size = l1_needed;
    }
    cpu_physical_memory_read(s->active_l1cfbar, job->l1_raw, l1_needed);

    if (l2_en && l2_bpp > 0) {
        size_t l2_needed = (size_t)l2_src_width * rows;
        if (job->l2_raw_size < l2_needed) {
            g_free(job->l2_raw);
            job->l2_raw = g_malloc(l2_needed);
            job->l2_raw_size = l2_needed;
        }
        cpu_physical_memory_read(s->active_l2cfbar, job->l2_raw, l2_needed);
    }

    memcpy(job->clut, s->clut, sizeof(job->clut));
    memcpy(job->clut2, s->clut2, sizeof(job->clut2));
}

static void gnw_h7b0_ltdc_snapshot_and_dispatch(GnwH7B0LtdcState *s, int rows)
{
    gnw_h7b0_ltdc_snapshot_into(s, &s->compositor_job, rows);

    qemu_mutex_lock(&s->job_lock);
    s->job_busy = true;
    s->job_pending = true;
    qemu_cond_signal(&s->job_cond);
    qemu_mutex_unlock(&s->job_lock);
}

/*
 * Promote a staged snapshot (see staged_job's doc comment in
 * gnw_h7b0_ltdc.h) into the compositor job now that the worker is free.
 * Swaps the two job structs wholesale so the raw-buffer allocations
 * simply change owners -- no copying, no leaks.
 */
static void gnw_h7b0_ltdc_promote_staged(GnwH7B0LtdcState *s)
{
    GnwH7B0LtdcCaptureJob tmp;

    qemu_mutex_lock(&s->job_lock);
    if (!s->staged_valid || s->job_busy) {
        qemu_mutex_unlock(&s->job_lock);
        return;
    }
    qemu_mutex_unlock(&s->job_lock);

    tmp = s->compositor_job;
    s->compositor_job = s->staged_job;
    s->staged_job = tmp;
    s->staged_valid = false;

    qemu_mutex_lock(&s->job_lock);
    s->job_busy = true;
    s->job_pending = true;
    qemu_cond_signal(&s->job_cond);
    qemu_mutex_unlock(&s->job_lock);
}

/*
 * The actual per-pixel convert/blend/dither work (identical logic to
 * this device's previous synchronous gnw_h7b0_ltdc_capture_rows(), just
 * sourcing from a GnwH7B0LtdcCaptureJob snapshot instead of live guest
 * RAM/s->regs/s->clut). Runs on the compositor worker thread, without
 * the BQL held, writing into `out` (the worker's private back buffer --
 * never touched by any other thread while this runs). Preserves both of
 * capture_rows()'s existing perf optimizations: the per-column window-
 * clip precompute (l1_x_in/l2_x_in) and gnw_h7b0_ltdc_blend_over()'s
 * fully-transparent/fully-opaque fast paths.
 */
static void gnw_h7b0_ltdc_composite_from_job_inner(GnwH7B0LtdcState *s,
                                                    GnwH7B0LtdcCaptureJob *job,
                                                    uint32_t *out,
                                                    bool allow_fast)
{
    int cols = job->cols;
    uint32_t pfcr = job->active_l1pfcr & LTDC_LxPFCR_PF_MASK;
    bool l1_l8 = (pfcr == LTDC_PF_L8);
    int src_width = job->l1_src_width;

    bool l1_colken = job->active_l1cr & LTDC_LxCR_COLKEN;
    uint32_t l1_ckcr = job->active_l1ckcr;
    uint32_t l1_dccr = job->active_l1dccr;
    uint32_t l1_bfcr = job->active_l1bfcr;
    uint32_t l1_cacr = job->active_l1cacr & 0xFF;
    int l1_whstpos = job->active_l1whpcr & LTDC_LxWHPCR_WHSTPOS_MASK;
    int l1_whsppos = (job->active_l1whpcr >> LTDC_LxWHPCR_WHSPPOS_SHIFT) &
                      LTDC_LxWHPCR_WHSPPOS_MASK;
    int l1_wvstpos = job->active_l1wvpcr & LTDC_LxWVPCR_WVSTPOS_MASK;
    int l1_wvsppos = (job->active_l1wvpcr >> LTDC_LxWVPCR_WVSPPOS_SHIFT) &
                      LTDC_LxWVPCR_WVSPPOS_MASK;

    bool l2_en = job->l2_en;
    uint32_t l2_pfcr = job->active_l2pfcr & LTDC_LxPFCR_PF_MASK;
    bool l2_l8 = (l2_pfcr == LTDC_PF_L8);
    bool l2_al44 = (l2_pfcr == LTDC_PF_AL44);
    int l2_src_width = job->l2_src_width;
    int l2_bpp = job->l2_bpp;
    /* See gnw_h7b0_ltdc_snapshot_and_dispatch()'s computation of this
     * field -- required to keep the per-pixel loop below from reading
     * past job->l2_raw's per-row allocation when Layer2's stride is
     * narrower than Layer1's `cols`. */
    int l2_cols = job->l2_cols;
    uint32_t l2_cacr = job->active_l2cacr & 0xFF;
    bool l2_colken = job->active_l2cr & LTDC_LxCR_COLKEN;
    uint32_t l2_ckcr = job->active_l2ckcr;
    uint32_t l2_dccr = job->active_l2dccr;
    uint32_t l2_bfcr = job->active_l2bfcr;
    int l2_whstpos = job->active_l2whpcr & LTDC_LxWHPCR_WHSTPOS_MASK;
    int l2_whsppos = (job->active_l2whpcr >> LTDC_LxWHPCR_WHSPPOS_SHIFT) &
                      LTDC_LxWHPCR_WHSPPOS_MASK;
    int l2_wvstpos = job->active_l2wvpcr & LTDC_LxWVPCR_WVSTPOS_MASK;
    int l2_wvsppos = (job->active_l2wvpcr >> LTDC_LxWVPCR_WVSPPOS_SHIFT) &
                      LTDC_LxWVPCR_WVSPPOS_MASK;

    uint32_t bccr = 0xFF000000U | (job->active_bccr & 0xFFFFFFU);
    bool dither_en = job->active_gcr & LTDC_GCR_DEN;

    int ahbp = (job->active_bpcr >> LTDC_BPCR_AHBP_SHIFT) & LTDC_BPCR_AHBP_MASK;
    int avbp = job->active_bpcr & LTDC_BPCR_AVBP_MASK;

    g_autofree bool *l1_x_in = g_malloc(cols * sizeof(bool));
    g_autofree bool *l2_x_in = g_malloc(cols * sizeof(bool));
    for (int x = 0; x < cols; x++) {
        int abs_x = ahbp + x + 1;
        l1_x_in[x] = abs_x >= l1_whstpos && abs_x <= l1_whsppos;
        l2_x_in[x] = abs_x >= l2_whstpos && abs_x <= l2_whsppos;
    }

    /*
     * Row fast-path eligibility that doesn't depend on y. For RGB565
     * Layer1 (L8 has its own, more general gate just below): fully opaque
     * with the default PAxCA blend factors and CACR==255 (so
     * gnw_h7b0_ltdc_blend_over()'s own fast path would just return fg
     * unmodified for every pixel), no color-key (which can force
     * per-pixel transparency), no dithering, and the whole row's x-range
     * inside Layer1's window (checked once here via the endpoints --
     * l1_x_in[] is monotonic in x since WHSTPOS<=WHSPPOS bounds a single
     * contiguous span).
     *
     * NOTE: this deliberately says nothing about Layer2. Under those
     * conditions the Layer1 half of the general loop reduces exactly to
     * "out = rgb565_to_argb8888(src)" -- resolve_layer returns the raw
     * pixel with alpha 255, and blend_over's PAxCA fast path then returns
     * that foreground unmodified, discarding BCCR. So when Layer2 IS up we
     * can still vectorise the Layer1 unpack for the whole row and then run
     * a second, Layer2-only pass over [0, l2_cols) that blends the overlay
     * on top of it -- bit-identical to the fused loop, because Layer2's own
     * per-pixel work in the fused loop reads nothing but `below` (which is
     * precisely what pass 1 wrote) and columns >= l2_cols are untouched by
     * either version. dither_en is excluded from the fast gate entirely, so
     * the post-blend dither step cannot be split across the two passes.
     *
     * The gate deliberately does NOT test LxBFCR. It used to require the
     * PAxCA pair, but that was over-restrictive and cost real time: an
     * RGB565 Layer1 pixel always has pa==255, so blend_over()'s
     *   factor1     = (bf1 & PA) ? (pa * ca)/255 : ca   ==  ca
     *   factor2base = (bf2 & PA) ? (pa * ca)/255 : ca   ==  ca
     * collapse to `ca` in ALL FOUR BF1/BF2 mode combinations. With
     * ca == 255 that is factor1 == 255 / factor2 == 0, i.e. "return fg
     * unmodified", for any BFCR whatsoever. Requiring PA mode therefore
     * excluded nothing unsafe and did exclude the real retro-go launcher,
     * which programs L1BFCR = 0x405 (constant-alpha mode) and so took the
     * fully general scalar path for every pixel of an entirely opaque
     * layer. CACR == 255 is still required, and !l1_colken still is too
     * (a color-key hit forces pa to 0, breaking the collapse above).
     */
    bool l1_fast_eligible = allow_fast && !l1_l8 && !l1_colken &&
        l1_cacr == 255 &&
        !dither_en && cols > 0 && l1_x_in[0] && l1_x_in[cols - 1];

    /*
     * L8 takes a different -- and strictly more general -- route to the
     * same row fast path. The RGB565 argument above leans on pa == 255
     * being *structural* for the format; for L8 the alpha comes from the
     * palette, and this device resets the whole CLUT to zero (alpha 0),
     * so a firmware that programs only part of the palette (retro-go's
     * DOS core programs a 16-entry VGA palette and leaves 240 entries at
     * zero) genuinely has fully transparent indices whose correct result
     * is the background, not the foreground. Demanding an all-opaque
     * CLUT was tried first and rejects the real workload outright.
     *
     * What is true instead: with the row entirely inside Layer1's window
     * and dithering off, the Layer1 half of the general loop is a PURE
     * FUNCTION OF THE SOURCE BYTE. resolve_layer()'s inputs besides the
     * raw pixel (dccr, colken, ckcr) and blend_over()'s (bfcr, cacr, and
     * the bccr background) are all per-job constants, and the source
     * pixel itself is one of only 256 values. So build the composed
     * result for all 256 indices once per job and gather -- exact for
     * every alpha, every BFCR, every CACR, color-key included, with no
     * opacity, CACR or color-key precondition at all. 256 evaluations
     * per frame replace 320x240 of them.
     *
     * Dithering stays excluded (it depends on x/y, so it is not a
     * function of the pixel value), as does a row that straddles the
     * window edge (in_window varies along the row). Layer2 is untouched:
     * pass 1 writes exactly the `below` the fused loop would have, and
     * the existing pass-2 loop blends on top of it as before.
     */
    g_autofree uint32_t *l1_l8_table = NULL;
    if (allow_fast && l1_l8 && !dither_en && cols > 0 &&
        l1_x_in[0] && l1_x_in[cols - 1]) {
        l1_l8_table = g_malloc(256 * sizeof(uint32_t));
        for (int i = 0; i < 256; i++) {
            unsigned int a;
            uint32_t resolved = gnw_h7b0_ltdc_resolve_layer(
                job->clut[i], true, l1_dccr, l1_colken, l1_ckcr, &a);
            l1_l8_table[i] = gnw_h7b0_ltdc_blend_over(resolved, a, l1_bfcr,
                                                      l1_cacr, bccr);
        }
        l1_fast_eligible = true;
    }
    bool l2_active = l2_en && l2_bpp > 0;

    for (int y = 0; y < job->rows; y++) {
        int abs_y = avbp + y + 1;
        bool l1_row_in = abs_y >= l1_wvstpos && abs_y <= l1_wvsppos;
        bool l2_row_in = abs_y >= l2_wvstpos && abs_y <= l2_wvsppos;
        const uint8_t *linebuf = job->l1_raw + (size_t)y * src_width;
        const uint8_t *l2_linebuf = l2_active ?
            job->l2_raw + (size_t)y * l2_src_width : NULL;

        if (l1_fast_eligible && l1_row_in) {
            uint32_t *outrow = out + (hwaddr)y * cols;

            if (l1_l8) {
                gnw_h7b0_ltdc_l8_row_to_argb8888(linebuf, l1_l8_table,
                                                 outrow, cols);
            } else {
                gnw_h7b0_ltdc_rgb565_row_to_argb8888(linebuf, outrow, cols);
            }

            if (l2_active) {
                /* Pass 2: Layer2 only, on top of what pass 1 just wrote. */
                for (int x = 0; x < l2_cols; x++) {
                    bool l2_in = l2_row_in && l2_x_in[x];
                    uint32_t l2_raw;
                    unsigned int l2_alpha;
                    uint32_t l2_resolved;

                    if (l2_l8) {
                        l2_raw = job->clut2[l2_linebuf[x]];
                    } else if (l2_al44) {
                        uint8_t raw = l2_linebuf[x];
                        unsigned int a8 = ((raw >> 4) & 0xF) * 0x11U;
                        uint32_t clut_rgb =
                            job->clut2[(raw & 0xF) * 0x11U] & 0x00FFFFFFU;
                        l2_raw = (a8 << 24) | clut_rgb;
                    } else {
                        l2_raw = gnw_h7b0_ltdc_read_pixel_buf(
                            l2_linebuf + x * l2_bpp, l2_pfcr);
                    }

                    l2_resolved = gnw_h7b0_ltdc_resolve_layer(
                        l2_raw, l2_in, l2_dccr, l2_colken, l2_ckcr, &l2_alpha);
                    outrow[x] = gnw_h7b0_ltdc_blend_over(
                        l2_resolved, l2_alpha, l2_bfcr, l2_cacr, outrow[x]);
                }
            }
            continue;
        }

        for (int x = 0; x < cols; x++) {
            int abs_x = ahbp + x + 1;
            bool l1_in = l1_row_in && l1_x_in[x];
            uint32_t l1_raw = l1_l8 ? job->clut[linebuf[x]]
                                    : gnw_h7b0_ltdc_rgb565_to_pixel32(
                                          lduw_le_p(linebuf + x * 2));

            unsigned int l1_alpha;
            uint32_t l1_resolved = gnw_h7b0_ltdc_resolve_layer(
                l1_raw, l1_in, l1_dccr, l1_colken, l1_ckcr, &l1_alpha);
            uint32_t below = gnw_h7b0_ltdc_blend_over(l1_resolved, l1_alpha,
                                                       l1_bfcr, l1_cacr, bccr);

            if (l2_active && x < l2_cols) {
                bool l2_in = l2_row_in && l2_x_in[x];
                uint32_t l2_raw;
                unsigned int l2_alpha;
                uint32_t l2_resolved;

                if (l2_l8) {
                    uint8_t idx = l2_linebuf[x];
                    l2_raw = job->clut2[idx];
                } else if (l2_al44) {
                    uint8_t raw = l2_linebuf[x];
                    unsigned int a4 = (raw >> 4) & 0xF;
                    unsigned int l4 = raw & 0xF;
                    unsigned int a8 = a4 * 0x11U;
                    uint32_t clut_rgb = job->clut2[l4 * 0x11U] & 0x00FFFFFFU;
                    l2_raw = (a8 << 24) | clut_rgb;
                } else {
                    l2_raw = gnw_h7b0_ltdc_read_pixel_buf(
                        l2_linebuf + x * l2_bpp, l2_pfcr);
                }

                l2_resolved = gnw_h7b0_ltdc_resolve_layer(
                    l2_raw, l2_in, l2_dccr, l2_colken, l2_ckcr, &l2_alpha);
                below = gnw_h7b0_ltdc_blend_over(l2_resolved, l2_alpha,
                                                  l2_bfcr, l2_cacr, below);
            }

            uint32_t px = below;

            if (dither_en) {
                px = gnw_h7b0_ltdc_dither_pixel(px, abs_x, abs_y);
            }

            out[y * cols + x] = px;
        }
    }
}

/*
 * GNW_LTDC_VERIFY_FASTPATH=1: composite every frame twice -- once with the
 * row fast path enabled (the result that is actually published) and once
 * with it forced off (the fully general per-pixel path) -- and log any
 * mismatch. This is the differential proof that the fast path is
 * pixel-exact; it is far stronger than comparing frame hashes across two
 * builds, which the launcher scene defeats anyway (it contains a
 * wall-clock-varying element, so its frames are not reproducible run to
 * run). Off by default and roughly halves compositor throughput when on.
 */
static void gnw_h7b0_ltdc_composite_from_job(GnwH7B0LtdcState *s,
                                              GnwH7B0LtdcCaptureJob *job,
                                              uint32_t *out)
{
    static int verify = -1;

    gnw_h7b0_ltdc_composite_from_job_inner(s, job, out, true);

    if (verify < 0) {
        verify = gnw_env_enabled("GNW_LTDC_VERIFY_FASTPATH");
    }
    if (verify) {
        static unsigned long seq, bad;
        size_t n = (size_t)job->cols * job->rows;
        g_autofree uint32_t *ref = g_malloc(n * sizeof(uint32_t));

        gnw_h7b0_ltdc_composite_from_job_inner(s, job, ref, false);
        seq++;
        if (memcmp(ref, out, n * sizeof(uint32_t)) != 0) {
            size_t i;
            for (i = 0; i < n && ref[i] == out[i]; i++) {
                continue;
            }
            bad++;
            fprintf(stderr, "LTDCVERIFY MISMATCH frame %lu px %zu "
                    "fast=%08x ref=%08x (%lu bad of %lu)\n",
                    seq, i, out[i], ref[i], bad, seq);
        } else {
            fprintf(stderr, "LTDCVERIFY ok frame %lu (%lu bad)\n", seq, bad);
        }
    }
}

static void *gnw_h7b0_ltdc_compositor_thread_fn(void *opaque)
{
    GnwH7B0LtdcState *s = GNW_H7B0_LTDC(opaque);

    qemu_mutex_lock(&s->job_lock);
    while (!s->compositor_stop) {
        while (!s->job_pending && !s->compositor_stop) {
            qemu_cond_wait(&s->job_cond, &s->job_lock);
        }
        if (s->compositor_stop) {
            break;
        }
        s->job_pending = false;
        qemu_mutex_unlock(&s->job_lock);

        /*
         * s->compositor_job and s->compositor_back_buffer are safe to
         * touch here without any lock: the producer side is guaranteed
         * (by job_busy, still true) not to mutate compositor_job again,
         * and compositor_back_buffer is never read/written by anything
         * but this worker thread.
         */
        gnw_h7b0_ltdc_composite_from_job(s, &s->compositor_job,
                                          s->compositor_back_buffer);

        qemu_mutex_lock(&s->compositor_publish_lock);
        {
            uint32_t *tmp = s->shadow_buffer;
            s->shadow_buffer = s->compositor_back_buffer;
            s->compositor_back_buffer = tmp;
        }
        s->content_dirty = true;
        s->invalidate = 1;
        qemu_mutex_unlock(&s->compositor_publish_lock);

        qemu_mutex_lock(&s->job_lock);
        s->job_busy = false;
        /* Wake gnw_h7b0_ltdc_wait_compositor_idle() (reset/unrealize),
         * which waits on this same condvar for job_busy to clear. */
        qemu_cond_broadcast(&s->job_cond);
    }
    qemu_mutex_unlock(&s->job_lock);
    return NULL;
}

/*
 * Block until the compositor worker thread is idle (no job in flight).
 * Must be called before anything that frees/reassigns shadow_buffer,
 * compositor_back_buffer, or compositor_job's raw snapshot buffers
 * outside the normal job_busy-gated resize path in
 * gnw_h7b0_ltdc_capture_setup() -- currently that's device reset and
 * unrealize, both of which can race an in-flight compositor job
 * otherwise (reset in particular runs under the BQL but that says
 * nothing about the worker thread, which runs without it).
 */
static void gnw_h7b0_ltdc_wait_compositor_idle(GnwH7B0LtdcState *s)
{
    qemu_mutex_lock(&s->job_lock);
    while (s->job_busy) {
        qemu_cond_wait(&s->job_cond, &s->job_lock);
    }
    qemu_mutex_unlock(&s->job_lock);
}

static void gnw_h7b0_ltdc_update_display(void *opaque)
{
    GnwH7B0LtdcState *s = GNW_H7B0_LTDC(opaque);
    DisplaySurface *surface = qemu_console_surface(s->con);
    uint32_t *buf;
    int width, height;
    bool dirty;

    /* The boot ROM's locked-device payload deliberately disables both
     * pixel layers and leaves the LTDC enabled with Layer 1's default
     * color set. On the H7B0 this is the visible blue recovery screen.
     * There is no framebuffer to capture in this state, so publish the
     * default-color scanout directly instead of leaving QEMU's initial
     * "Guest has not initialized the display" placeholder visible.
     */
    if ((s->active_gcr & LTDC_GCR_LTDCEN) &&
        !(s->active_l1cr & LTDC_LxCR_LEN) &&
        !(s->active_l2cr & LTDC_LxCR_LEN) &&
        s->active_l1dccr == 0xff0000ff) {
        uint32_t hstart = s->active_l1whpcr & 0x0fff;
        uint32_t hend = (s->active_l1whpcr >> 16) & 0x0fff;
        uint32_t vstart = s->active_l1wvpcr & 0x07ff;
        uint32_t vend = (s->active_l1wvpcr >> 16) & 0x07ff;
        int scan_width = hend > hstart ? hend - hstart : 320;
        int scan_height = vend > vstart ? vend - vstart : 240;
        uint32_t color = s->active_l1dccr;

        if (surface_bits_per_pixel(surface) == 32) {
            if (scan_width * GNW_H7B0_LTDC_SCALE != surface_width(surface) ||
                scan_height * GNW_H7B0_LTDC_SCALE != surface_height(surface)) {
                qemu_console_resize(s->con, scan_width * GNW_H7B0_LTDC_SCALE,
                                    scan_height * GNW_H7B0_LTDC_SCALE);
                surface = qemu_console_surface(s->con);
            }
            for (int y = 0; y < scan_height; y++) {
                uint8_t *row = surface_data(surface) +
                    (hwaddr)y * GNW_H7B0_LTDC_SCALE * surface_stride(surface);
                for (int sy = 0; sy < GNW_H7B0_LTDC_SCALE; sy++) {
                    uint32_t *dst = (uint32_t *)(row +
                        (hwaddr)sy * surface_stride(surface));
                    for (int x = 0; x < scan_width * GNW_H7B0_LTDC_SCALE; x++) {
                        dst[x] = color;
                    }
                }
            }
            dpy_gfx_update(s->con, 0, 0, scan_width * GNW_H7B0_LTDC_SCALE,
                           scan_height * GNW_H7B0_LTDC_SCALE);
            s->invalidate = 0;
            return;
        }
    }

    /*
     * Snapshot the currently-published buffer pointer/dims/dirty flag
     * under compositor_publish_lock -- the ONLY thing that can change
     * concurrently with this function now that compositing runs on the
     * worker thread: a publish is a pointer swap the worker performs
     * under this same lock (see gnw_h7b0_ltdc_compositor_thread_fn()).
     * Once snapshotted, `buf` is guaranteed stable for the rest of this
     * function: the worker thread only ever writes into whichever
     * buffer is NOT currently published (i.e. never `buf` after this
     * point), until its NEXT publish, which cannot happen before this
     * function releases the lock and returns.
     */
    qemu_mutex_lock(&s->compositor_publish_lock);
    buf = s->shadow_buffer;
    width = s->shadow_width;
    height = s->shadow_height;
    dirty = s->content_dirty;
    qemu_mutex_unlock(&s->compositor_publish_lock);

    if (!buf || width <= 0 || height <= 0) {
        qemu_mutex_lock(&s->compositor_publish_lock);
        s->content_dirty = false;
        qemu_mutex_unlock(&s->compositor_publish_lock);
        return;
    }

    if (surface_bits_per_pixel(surface) != 32) {
        qemu_mutex_lock(&s->compositor_publish_lock);
        s->content_dirty = false;
        qemu_mutex_unlock(&s->compositor_publish_lock);
        return;
    }

    if (!dirty && !s->invalidate) {
        return;
    }

    if (s->invalidate ||
        width * GNW_H7B0_LTDC_SCALE != surface_width(surface) ||
        height * GNW_H7B0_LTDC_SCALE != surface_height(surface)) {
        qemu_console_resize(s->con, width * GNW_H7B0_LTDC_SCALE,
                            height * GNW_H7B0_LTDC_SCALE);
        surface = qemu_console_surface(s->con);
    }

    for (int y = 0; y < height; y++) {
        uint8_t *drow0 = surface_data(surface) +
                          (hwaddr)y * GNW_H7B0_LTDC_SCALE * surface_stride(surface);

        for (int x = 0; x < width; x++) {
            uint32_t px = buf[y * width + x];
            for (int sy = 0; sy < GNW_H7B0_LTDC_SCALE; sy++) {
                uint32_t *drow = (uint32_t *)(drow0 +
                                              (hwaddr)sy * surface_stride(surface));
                for (int sx = 0; sx < GNW_H7B0_LTDC_SCALE; sx++) {
                    drow[x * GNW_H7B0_LTDC_SCALE + sx] = px;
                }
            }
        }
    }

    dpy_gfx_update(s->con, 0, 0, width * GNW_H7B0_LTDC_SCALE,
                   height * GNW_H7B0_LTDC_SCALE);
    s->invalidate = 0;

    /*
     * Only clear content_dirty if the worker hasn't already published a
     * newer frame while we were blitting (buf would then no longer
     * match s->shadow_buffer) -- otherwise we'd silently drop that
     * newer frame's dirty flag and it would never get drawn until the
     * next unrelated invalidate.
     */
    qemu_mutex_lock(&s->compositor_publish_lock);
    if (s->shadow_buffer == buf) {
        s->content_dirty = false;
    }
    qemu_mutex_unlock(&s->compositor_publish_lock);
}

static void gnw_h7b0_ltdc_invalidate_display(void *opaque)
{
    GnwH7B0LtdcState *s = GNW_H7B0_LTDC(opaque);

    s->invalidate = 1;
}

static uint64_t gnw_h7b0_ltdc_read(void *opaque, hwaddr addr,
                                    unsigned int size)
{
    GnwH7B0LtdcState *s = GNW_H7B0_LTDC(opaque);

    if (addr >= GNW_H7B0_LTDC_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return 0;
    }
    return s->regs[addr >> 2];
}

static void gnw_h7b0_ltdc_write(void *opaque, hwaddr addr,
                                 uint64_t val64, unsigned int size)
{
    GnwH7B0LtdcState *s = GNW_H7B0_LTDC(opaque);
    uint32_t value = val64;

    if (addr >= GNW_H7B0_LTDC_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return;
    }

    uint32_t mask = get_ltdc_write_mask(addr);
    value = (s->regs[addr >> 2] & ~mask) | (value & mask);

    switch (addr) {
    case GNW_H7B0_LTDC_SRCR:
        /*
         * Real hardware copies shadow registers (CFBAR/CFBLR/CFBLNR/
         * PFCR) into the active set on a reload event, clearing IMR/VBR
         * once done. IMR ("immediate") really is instant on real
         * hardware too; VBR ("vertical blanking") is real hardware's
         * deferred-to-next-vblank path and is now modeled that way
         * here (gnw_h7b0_ltdc_vblank_tick() applies it) instead of also
         * being instant -- that instant-VBR approximation was the
         * documented cause of gnw-chainloader/retro-go's menu tearing:
         * firmware's fb1/fb2 double-buffer swap (a VBR reload) used to
         * take effect immediately, so a redraw could land mid-swap
         * relative to what firmware had actually finished drawing.
         *
         * Critically, VBR must also *stay set* in the register until
         * the real vblank tick clears it -- gnw-chainloader's
         * gui_refresh() (src/chainloader/gui.c) does exactly what real
         * hardware expects: request the reload, then busy-wait on
         * `SRCR & (VBR|IMR)` clearing as its actual frame-rate throttle.
         * Clearing VBR synchronously here (the previous behavior) made
         * that wait a no-op, so gui_refresh() looped at whatever rate
         * the CPU could manage (measured ~1000x/sec) instead of ~60Hz
         * -- the real root cause of the flicker (foreground redrawn/
         * cleared far faster than any real display could show) and,
         * via the identical poll-on-SRCR/ISR pattern in retro-go's
         * lcd_wait_for_vblank(), very likely also the "runs too fast"
         * bug. IMR keeps clearing instantly (real immediate reload
         * really is instant on real hardware too).
         *
         * RRIF must be raised here too: real hardware sets it for any
         * reload that actually takes effect, immediate or deferred, and
         * an immediate reload here is exactly that -- it was previously
         * only ever set (unconditionally, wrongly) from the vblank tick,
         * so IMR reloads never raised it at all.
         */
        s->srcr_idle_ticks = 0;
        /*
         * Reset alongside srcr_idle_ticks, not just when the idle-
         * fallback consumes it -- confirmed via live profiling that
         * leaving this "sticky" (as originally designed) meant a single
         * genuine transition anywhere in a session left it permanently
         * armed, so EVERY later ordinary stutter burst (which also
         * crosses the srcr_idle_ticks threshold, see that field's doc
         * comment) re-triggered the fallback for the rest of the
         * session, reintroducing the self-inflicted tlb_reset_dirty/
         * notdirty churn this was meant to avoid. reload_active() below
         * re-arms it if THIS write's reload is itself a genuine
         * structural change.
         */
        s->structural_transition_pending = false;
        if (value & LTDC_SRCR_IMR) {
            /*
             * An IMR reload means firmware is applying a fresh layer/
             * format config -- the natural checkpoint for "a new screen
             * just took over the layers, prove it uses VBR before we
             * disable the no-VBR fallback for it again" (see the
             * vblank_tick() comment for why this isn't a permanent
             * latch or a timeout).
             */
            bool composition_changed = gnw_h7b0_ltdc_reload_is_composition_change(s);
            /*
             * True when a SRCR.VBR reload has happened since the *last*
             * IMR reload -- vbr_active is set only by the VBR path below
             * and cleared unconditionally three lines down, so reading it
             * here is exactly that predicate, and needs no extra state.
             *
             * That is the signature of the IMR-then-VBR frame-swap
             * sequence stock firmware uses (traced on Super Mario World:
             * SRCR=IMR then SRCR=VBR ~5us later, once per guest frame,
             * with L1CFBAR already flipped to the other buffer at the
             * IMR write) -- HAL_LTDC_SetAddress() does an immediate
             * reload, and the vblank reload request follows. See the
             * capture comment below for why it matters.
             */
            bool vbr_paced = s->vbr_active;
            bool structural = gnw_h7b0_ltdc_reload_is_structural_change(s);
            s->vbr_active = false;
            gnw_h7b0_ltdc_reload_active(s);
            s->regs[GNW_H7B0_LTDC_ISR >> 2] |= LTDC_ISR_RRIF;
            gnw_h7b0_ltdc_update_irq(s);
            /*
             * HAL_LTDC_SetPixelFormat() and friends use IMR reloads for
             * on-the-fly format/geometry changes. Unlike VBR (which
             * captures the outgoing frame *before* the reload for double
             * buffering), IMR takes effect instantly -- capture right away
             * with the new active configuration or the host display never
             * picks up the new framebuffer layout (lcd_setup_framebuffers()
             * calls SetPixelFormat/SetAddress then a separate VBR reload,
             * but intermediate IMR reloads from the HAL must still refresh).
             *
             * Skip the (expensive, full-framebuffer) capture entirely when
             * this reload didn't actually change anything composition-
             * affecting (composition_changed, computed above BEFORE
             * reload_active() overwrote the active_* fields) AND there
             * isn't already a stale captured-but-unpublished frame
             * pending (content_dirty) -- see
             * gnw_h7b0_ltdc_reload_is_composition_change()'s doc comment.
             * A real format/geometry change always flips at least one of
             * the compared registers, so this only ever skips truly
             * redundant reloads (e.g. an always-disabled scratch layer's
             * own reload, which can't affect what's on screen).
             *
             * If the compositor worker thread is still busy with a
             * previous frame, this dispatch is dropped (see
             * gnw_h7b0_ltdc_capture_if_enabled()'s doc comment) -- fall
             * back to the same vbr_deferred_capture retry-on-next-
             * vblank-tick mechanism the VBR path below uses, since an
             * IMR-triggered geometry/format change is exactly the kind
             * of update that must not silently get lost.
             */
            /*
             * Also skip it when this immediate reload is just the first
             * half of a VBR-paced frame swap (vbr_paced, and nothing
             * structural changed -- i.e. at most a framebuffer-address
             * flip and friends): the SRCR.VBR write that follows a few
             * microseconds later captures the very same active
             * configuration, so capturing here as well composited and
             * published every guest frame twice. Measured on stock Super
             * Mario World: 944 "imr" + 941 "deferred" captures for ~870
             * guest frames over 29s, i.e. two full 320x240 composites,
             * two surface blits and two dpy_gfx_update() calls per frame
             * for two bit-identical images -- which is also what drove
             * the host UI to render at ~45Hz for a 30fps guest.
             *
             * Real hardware does not show two frames here either: the
             * shadow registers are unchanged between the immediate
             * reload and the vblank reload, so the second reload latches
             * nothing new and the panel keeps scanning out one image.
             * RM0455's SRCR description says only that a reload copies
             * the shadow registers; it is silent on a reload that copies
             * identical values, and there is nothing in it that would
             * make the second one visible.
             *
             * Deliberately gated on vbr_paced rather than "address-only
             * change", so a guest that double-buffers with IMR reloads
             * *alone* (no VBR at all) keeps capturing here exactly as
             * before -- vbr_active can only be true if a VBR reload
             * really did arrive since the previous IMR.
             */
            if ((composition_changed || s->content_dirty) &&
                !(vbr_paced && !structural)) {
                if (gnw_h7b0_ltdc_capture_if_enabled(s)) {
                    GNW_LTDC_TRACE_CAP(s, "imr");
                } else {
                    s->vbr_deferred_capture = true;
                }
            }
        }
        if (value & LTDC_SRCR_VBR) {
            /*
             * VBR reload = the guest swapping display buffers, i.e. ONE
             * emulated frame. Counting these per wall-clock second is the
             * only guest-frame-rate measurement that is neither
             * guest-relative (the firmware's own fps counter is derived
             * from emulated time, and reads 30fps even when the guest is
             * running at 65% of realtime) nor disk-bound (the frame
             * recorder writes 36MB/s at 30fps, which perturbs what it
             * measures). getenv() is resolved once -- it is a locked
             * linear scan on Windows msvcrt.
             */
            {
                static int trace = -1;
                if (trace < 0) {
                    const char *e = getenv("GNW_UI_FRAME_TRACE");
                    trace = (e && *e && strcmp(e, "0") != 0);
                }
                if (trace) {
                    static int64_t t0;
                    static uint64_t frames;
                    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
                    frames++;
                    if (now - t0 >= 1000000000LL) {
                        fprintf(stderr, "GUESTFPS %.1f\n",
                                frames * 1e9 / (double)(now - t0));
                        t0 = now;
                        frames = 0;
                    }
                }
            }
            s->vbr_reload_pending = true;
            s->vbr_active = true;

            /*
             * Tear down the RAM dirty-bitmap tracking bound for the
             * non-VBR fallback (see gnw_h7b0_ltdc_fb_dirty_check_and_clear()
             * and fb_reg_dirty's doc comment in gnw_h7b0_ltdc.h) now that
             * VBR-paced double buffering is active again -- leaving
             * DIRTY_MEMORY_VGA logging bound to the framebuffer taxes
             * *every* guest write into it via QEMU's slow notdirty TLB
             * path (profiled: >50% of total CPU cycles during active
             * gameplay), and once bound it otherwise stays bound
             * indefinitely (gnw_h7b0_ltdc_fb_track_range() only rebinds
             * when the tracked range changes, which a game's fixed
             * framebuffer address usually never does). The fallback lazily
             * re-binds it the next time it's actually needed, so this is
             * safe to tear down unconditionally here.
             */
            gnw_h7b0_ltdc_fb_track_range(&s->fb_l1_section,
                                          &s->fb_l1_track_base,
                                          &s->fb_l1_track_len, 0, 0);
            gnw_h7b0_ltdc_fb_track_range(&s->fb_l2_section,
                                          &s->fb_l2_track_base,
                                          &s->fb_l2_track_len, 0, 0);

            /* The guest has finished drawing the frame and requested a swap.
             * Capture it NOW to avoid capturing it mid-draw during the next
             * frame. Only capture if the UI thread has finished blitting the
             * previous capture. Also retry on the next vblank tick if the
             * dispatch itself was dropped because the compositor worker
             * thread was still busy with a previous frame (see
             * gnw_h7b0_ltdc_capture_if_enabled()'s doc comment) -- not just
             * when content_dirty was already true. */
            /*
             * ...but not when this VBR reload has nothing left to apply
             * and a capture of exactly that configuration is already
             * staged and unpublished (content_dirty). Stock Super Mario
             * World -- and, from the HAL, anything that goes through
             * HAL_LTDC_SetAddress() (which does an IMR reload) before
             * requesting the vblank swap -- writes SRCR twice per frame,
             * a few microseconds apart: SRCR=IMR first (which already
             * copied the new CFBAR into the active set and captured the
             * frame right above), then SRCR=VBR. On real hardware the
             * second reload is a no-op: the shadow registers are
             * unchanged since the immediate reload, so nothing new is
             * latched and the panel keeps scanning out the same image.
             * Modelling it as another capture made the device publish
             * every guest frame twice (measured: 944 "imr" + 941
             * "deferred" captures for ~870 guest frames over 29s), which
             * dragged the whole host render path -- composite, blit,
             * dpy_gfx_update(), texture upload and present -- to ~45Hz
             * for a 30fps guest, for two byte-identical images.
             *
             * The composition-change test is what keeps this safe: a
             * guest that genuinely double-buffers has a fresh CFBAR
             * pending at its VBR write (composition_changed true) and
             * still captures here, and a guest that never uses VBR at
             * all is served by the RAM-dirty fallback in
             * gnw_h7b0_ltdc_vblank_tick(). Only the "reload the exact
             * same active configuration a second time" case is dropped,
             * and only while an unpublished capture of it already
             * exists.
             */
            if (!s->content_dirty && gnw_h7b0_ltdc_capture_if_enabled(s)) {
                GNW_LTDC_TRACE_CAP(s, "vbr");
                s->vbr_deferred_capture = false;
            } else {
                s->vbr_deferred_capture = true;
            }
        }
        s->regs[addr >> 2] = value & ~LTDC_SRCR_IMR;
        /*
         * Re-derive the vblank/line timer rate on every reload request:
         * the timers otherwise only re-arm at tick time, so a single
         * recalculation made against a transient (mid-PLL3-programming)
         * clock config can arm a pathologically long period that wedges
         * the whole frame-tick chain (stock's main loop is paced by the
         * reload-interrupt -> frame-flag path) until it finally fires.
         * Firmware writes SRCR once per frame, making this the natural
         * self-healing point.
         */
        gnw_h7b0_ltdc_recalc_timers(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        return;
    case GNW_H7B0_LTDC_IER:
        s->regs[addr >> 2] = value;
        gnw_h7b0_ltdc_update_irq(s);
        return;
    case GNW_H7B0_LTDC_ICR:
        /* Write-1-to-clear: ICR bit positions mirror ISR's exactly. */
        s->regs[GNW_H7B0_LTDC_ISR >> 2] &= ~value;
        gnw_h7b0_ltdc_update_irq(s);
        return;
    case GNW_H7B0_LTDC_ISR:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: ISR is read-only on real hardware (use ICR to "
                      "clear flags)\n", __func__);
        return;
    case GNW_H7B0_LTDC_L1CLUTWR:
    {
        /*
         * Write-only on real hardware (L1CLUTWR reads back as 0, hence
         * no shadow storage needed beyond s->clut[] itself): CLUTADD is
         * the palette index, RED/GREEN/BLUE pack the entry -- see the
         * SVD's L1CLUTWR fields. Converted to this device's host
         * pixel32 format up front so capture_rows() can use L8 entries
         * exactly like RGB565-converted pixels.
         */
        unsigned int idx = (value >> 24) & 0xFF;
        unsigned int r = (value >> 16) & 0xFF;
        unsigned int g = (value >> 8) & 0xFF;
        unsigned int b = value & 0xFF;
        s->clut[idx] = 0xFF000000U | (r << 16) | (g << 8) | b;
        return;
    }
    case GNW_H7B0_LTDC_L2CLUTWR:
    {
        /* Mirrors L1CLUTWR above, but into Layer2's own independent
         * CLUT -- real hardware has separate CLUTs per layer. */
        unsigned int idx = (value >> 24) & 0xFF;
        unsigned int r = (value >> 16) & 0xFF;
        unsigned int g = (value >> 8) & 0xFF;
        unsigned int b = value & 0xFF;
        s->clut2[idx] = 0xFF000000U | (r << 16) | (g << 8) | b;
        return;
    }
    default:
        s->regs[addr >> 2] = value;
    }
}

static const MemoryRegionOps gnw_h7b0_ltdc_ops = {
    .read = gnw_h7b0_ltdc_read,
    .write = gnw_h7b0_ltdc_write,
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

static const GraphicHwOps gnw_h7b0_ltdc_gfx_ops = {
    .invalidate  = gnw_h7b0_ltdc_invalidate_display,
    .gfx_update  = gnw_h7b0_ltdc_update_display,
};

static void gnw_h7b0_ltdc_realize(DeviceState *dev, Error **errp)
{
    GnwH7B0LtdcState *s = GNW_H7B0_LTDC(dev);

    memory_region_init_io(&s->mmio, OBJECT(s), &gnw_h7b0_ltdc_ops, s,
                           TYPE_GNW_H7B0_LTDC, GNW_H7B0_LTDC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
    s->con = graphic_console_init(dev, 0, &gnw_h7b0_ltdc_gfx_ops, s);
    gnw_h7b0_recorder_init(s->con);
    s->vblank_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                    gnw_h7b0_ltdc_vblank_tick, s);
    s->line_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                  gnw_h7b0_ltdc_line_tick, s);

    qemu_mutex_init(&s->compositor_publish_lock);
    qemu_mutex_init(&s->job_lock);
    qemu_cond_init(&s->job_cond);
    s->compositor_stop = false;
    qemu_thread_create(&s->compositor_thread, "gnw-h7b0-ltdc-compositor",
                        gnw_h7b0_ltdc_compositor_thread_fn, s,
                        QEMU_THREAD_JOINABLE);
    s->compositor_running = true;
}

static void gnw_h7b0_ltdc_unrealize(DeviceState *dev)
{
    GnwH7B0LtdcState *s = GNW_H7B0_LTDC(dev);

    /*
     * Stop the compositor worker thread and join it before tearing down
     * anything it might still touch. Signalled the same way a real job
     * is (job_lock held, job_cond signalled) so the worker's existing
     * wait loop picks up compositor_stop without needing a second wakeup
     * path.
     */
    qemu_mutex_lock(&s->job_lock);
    s->compositor_stop = true;
    qemu_cond_broadcast(&s->job_cond);
    qemu_mutex_unlock(&s->job_lock);
    qemu_thread_join(&s->compositor_thread);
    s->compositor_running = false;

    qemu_mutex_destroy(&s->job_lock);
    qemu_cond_destroy(&s->job_cond);
    qemu_mutex_destroy(&s->compositor_publish_lock);

    g_free(s->shadow_buffer);
    s->shadow_buffer = NULL;
    g_free(s->compositor_back_buffer);
    s->compositor_back_buffer = NULL;
    g_free(s->compositor_job.l1_raw);
    s->compositor_job.l1_raw = NULL;
    g_free(s->compositor_job.l2_raw);
    s->compositor_job.l2_raw = NULL;
    g_free(s->staged_job.l1_raw);
    s->staged_job.l1_raw = NULL;
    g_free(s->staged_job.l2_raw);
    s->staged_job.l2_raw = NULL;

    gnw_h7b0_ltdc_fb_track_range(&s->fb_l1_section, &s->fb_l1_track_base,
                                  &s->fb_l1_track_len, 0, 0);
    gnw_h7b0_ltdc_fb_track_range(&s->fb_l2_section, &s->fb_l2_track_base,
                                  &s->fb_l2_track_len, 0, 0);
}

/* Settle host-only work without a virtual tick, reload, IRQ or new RAM
 * capture. A staged job owns the pixels read at the original reload instant. */
static int ltdc_pre_save(void *opaque)
{
    GnwH7B0LtdcState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    gnw_h7b0_ltdc_wait_compositor_idle(s);
    gnw_h7b0_ltdc_promote_staged(s);
    gnw_h7b0_ltdc_wait_compositor_idle(s);
    s->snapshot_vblank_remaining_ns = timer_pending(s->vblank_timer) ?
        MAX(timer_expire_time_ns(s->vblank_timer) - now, 0) : -1;
    s->snapshot_line_remaining_ns = timer_pending(s->line_timer) ?
        MAX(timer_expire_time_ns(s->line_timer) - now, 0) : -1;
    s->snapshot_grid_remaining_ns = s->vblank_deadline_ns - now;
    if (!s->shadow_buffer) {
        DisplaySurface *surface = qemu_console_surface(s->con);
        if (!surface || surface_bits_per_pixel(surface) != 32 ||
            (uint64_t)surface_width(surface) * surface_height(surface) >
                16U * 1024 * 1024) {
            return -EINVAL;
        }
    }
    return 0;
}

static int ltdc_pre_load(void *opaque)
{
    GnwH7B0LtdcState *s = opaque;

    gnw_h7b0_ltdc_wait_compositor_idle(s);
    s->staged_valid = false;
    return 0;
}

static int ltdc_put_frame(QEMUFile *f, void *pv, size_t size,
                           const VMStateField *field, JSONWriter *vmdesc)
{
    GnwH7B0LtdcState *s = pv;

    qemu_put_be32(f, s->shadow_width);
    qemu_put_be32(f, s->shadow_height);
    for (size_t i = 0; i < (size_t)s->shadow_width * s->shadow_height; i++) {
        qemu_put_be32(f, s->shadow_buffer[i]);
    }
    /* With no captured framebuffer, the console may show its placeholder
     * or retain the last scanout after a cold reset. Preserve that paused
     * output too; re-reading guest RAM cannot reconstruct it. */
    DisplaySurface *surface = qemu_console_surface(s->con);
    uint32_t w = s->shadow_buffer ? 0 : surface_width(surface);
    uint32_t h = s->shadow_buffer ? 0 : surface_height(surface);

    qemu_put_be32(f, w);
    qemu_put_be32(f, h);
    for (uint32_t y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)(surface_data(surface) +
                                    (size_t)y * surface_stride(surface));
        for (uint32_t x = 0; x < w; x++) {
            qemu_put_be32(f, row[x]);
        }
    }
    return qemu_file_get_error(f);
}

static int ltdc_get_frame(QEMUFile *f, void *pv, size_t size,
                           const VMStateField *field)
{
    GnwH7B0LtdcState *s = pv;
    uint32_t w = qemu_get_be32(f), h = qemu_get_be32(f);
    uint32_t *front = NULL, *back = NULL;
    size_t pixels;

    /* Register-limited geometry, <=64MiB per buffer; reject before allocating. */
    if (qemu_file_get_error(f) || w > 8191 || h > 2047 || (!!w != !!h)) {
        return -EINVAL;
    }
    pixels = (size_t)w * h;
    if (pixels) {
        front = g_try_new(uint32_t, pixels);
        back = g_try_new0(uint32_t, pixels);
        if (!front || !back) {
            g_free(front);
            g_free(back);
            return -ENOMEM;
        }
        for (size_t i = 0; i < pixels; i++) {
            front[i] = qemu_get_be32(f);
        }
        if (qemu_file_get_error(f)) {
            g_free(front);
            g_free(back);
            return -EIO;
        }
    }
    uint32_t surface_w = qemu_get_be32(f), surface_h = qemu_get_be32(f);
    if (qemu_file_get_error(f) || surface_w > 16384 || surface_h > 16384 ||
        (!!surface_w != !!surface_h) || (pixels && surface_w) ||
        (!pixels && !surface_w) ||
        (uint64_t)surface_w * surface_h > 16U * 1024 * 1024) {
        g_free(front);
        g_free(back);
        return -EINVAL;
    }
    if (surface_w) {
        DisplaySurface *surface = qemu_create_displaysurface(surface_w, surface_h);
        for (uint32_t y = 0; y < surface_h; y++) {
            uint32_t *row = (uint32_t *)(surface_data(surface) +
                                        (size_t)y * surface_stride(surface));
            for (uint32_t x = 0; x < surface_w; x++) {
                row[x] = qemu_get_be32(f);
            }
        }
        if (qemu_file_get_error(f)) {
            qemu_free_displaysurface(surface);
            return -EIO;
        }
        /* Load executes synchronously under BQL: no UI update can run
         * between this field and completion of the wrapper restore. */
        dpy_gfx_replace_surface(s->con, surface);
    }
    qemu_mutex_lock(&s->compositor_publish_lock);
    g_free(s->shadow_buffer);
    g_free(s->compositor_back_buffer);
    s->shadow_buffer = front;
    s->compositor_back_buffer = back;
    s->shadow_width = w;
    s->shadow_height = h;
    qemu_mutex_unlock(&s->compositor_publish_lock);
    return 0;
}

static const VMStateInfo ltdc_frame_info = {
    .name = "ltdc-frame",
    .get = ltdc_get_frame,
    .put = ltdc_put_frame,
};

static int ltdc_post_load(void *opaque, int version_id)
{
    GnwH7B0LtdcState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    gnw_h7b0_ltdc_fb_track_range(&s->fb_l1_section, &s->fb_l1_track_base,
                                  &s->fb_l1_track_len, 0, 0);
    gnw_h7b0_ltdc_fb_track_range(&s->fb_l2_section, &s->fb_l2_track_base,
                                  &s->fb_l2_track_len, 0, 0);
    s->invalidate = 1;
    if (version_id == 1) {
        gnw_h7b0_ltdc_reload_active(s);
        gnw_h7b0_ltdc_recalc_timers(s, now);
        return 0;
    }
    if (s->snapshot_vblank_remaining_ns < -1 ||
        s->snapshot_line_remaining_ns < -1 ||
        s->snapshot_vblank_remaining_ns > INT64_MAX - now ||
        s->snapshot_line_remaining_ns > INT64_MAX - now ||
        s->snapshot_grid_remaining_ns > INT64_MAX - now ||
        s->snapshot_grid_remaining_ns < -now ||
        s->srcr_idle_ticks < 0 ||
        s->srcr_idle_ticks > GNW_H7B0_LTDC_SRCR_IDLE_TICKS_MAX ||
        s->fb_quiesce_ticks < 0 ||
        s->fb_quiesce_ticks > GNW_H7B0_LTDC_QUIESCE_TICKS_MAX) {
        return -EINVAL;
    }
    timer_del(s->vblank_timer);
    timer_del(s->line_timer);
    if (s->snapshot_vblank_remaining_ns >= 0) {
        timer_mod(s->vblank_timer, now + s->snapshot_vblank_remaining_ns);
    }
    if (s->snapshot_line_remaining_ns >= 0) {
        timer_mod(s->line_timer, now + s->snapshot_line_remaining_ns);
    }
    s->vblank_deadline_ns = now + s->snapshot_grid_remaining_ns;
    return 0;
}

void gnw_h7b0_ltdc_snapshot_restore(GnwH7B0LtdcState *s)
{
    /* Full-load hook: CPU interrupt controller/RAM are already restored. */
    gnw_h7b0_ltdc_update_irq(s);
    gnw_h7b0_ltdc_update_display(s);
}

static const VMStateDescription vmstate_gnw_h7b0_ltdc = {
    .name = TYPE_GNW_H7B0_LTDC,
    .version_id = 2,
    .minimum_version_id = 1,
    .pre_save = ltdc_pre_save,
    .pre_load = ltdc_pre_load,
    .post_load = ltdc_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, GnwH7B0LtdcState, GNW_H7B0_LTDC_SIZE / 4),
        VMSTATE_UINT32_V(active_l1cr, GnwH7B0LtdcState, 2),
        VMSTATE_UINT32_V(active_l1cfbar, GnwH7B0LtdcState, 2),
        VMSTATE_UINT32_V(active_l1cfblr, GnwH7B0LtdcState, 2),
        VMSTATE_UINT32_V(active_l1cfblnr, GnwH7B0LtdcState, 2),
        VMSTATE_UINT32_V(active_l1pfcr, GnwH7B0LtdcState, 2),
        VMSTATE_UINT32_V(active_l2cr, GnwH7B0LtdcState, 2),
        VMSTATE_UINT32_V(active_l2cfbar, GnwH7B0LtdcState, 2),
        VMSTATE_UINT32_V(active_l2cfblr, GnwH7B0LtdcState, 2),
        VMSTATE_UINT32_V(active_l2cfblnr, GnwH7B0LtdcState, 2),
        VMSTATE_UINT32_V(active_l2pfcr, GnwH7B0LtdcState, 2),
        VMSTATE_UINT32_V(active_l2cacr, GnwH7B0LtdcState, 2),
        VMSTATE_UINT32_V(active_l1ckcr, GnwH7B0LtdcState, 2),
        VMSTATE_UINT32_V(active_l2ckcr, GnwH7B0LtdcState, 2),
        VMSTATE_UINT32_V(active_l1dccr, GnwH7B0LtdcState, 2),
        VMSTATE_UINT32_V(active_l2dccr, GnwH7B0LtdcState, 2),
        VMSTATE_UINT32_V(active_bccr, GnwH7B0LtdcState, 2),
        VMSTATE_UINT32_V(active_l1cacr, GnwH7B0LtdcState, 2),
        VMSTATE_UINT32_V(active_l1bfcr, GnwH7B0LtdcState, 2),
        VMSTATE_UINT32_V(active_l2bfcr, GnwH7B0LtdcState, 2),
        VMSTATE_UINT32_V(active_l1whpcr, GnwH7B0LtdcState, 2),
        VMSTATE_UINT32_V(active_l1wvpcr, GnwH7B0LtdcState, 2),
        VMSTATE_UINT32_V(active_l2whpcr, GnwH7B0LtdcState, 2),
        VMSTATE_UINT32_V(active_l2wvpcr, GnwH7B0LtdcState, 2),
        VMSTATE_UINT32_V(active_gcr, GnwH7B0LtdcState, 2),
        VMSTATE_UINT32_V(active_bpcr, GnwH7B0LtdcState, 2),
        VMSTATE_UINT32_ARRAY_V(clut, GnwH7B0LtdcState, 256, 2),
        VMSTATE_UINT32_ARRAY_V(clut2, GnwH7B0LtdcState, 256, 2),
        VMSTATE_BOOL_V(vbr_reload_pending, GnwH7B0LtdcState, 2),
        VMSTATE_BOOL_V(vbr_deferred_capture, GnwH7B0LtdcState, 2),
        VMSTATE_BOOL_V(vbr_active, GnwH7B0LtdcState, 2),
        VMSTATE_BOOL_V(content_dirty, GnwH7B0LtdcState, 2),
        VMSTATE_BOOL_V(fb_reg_dirty, GnwH7B0LtdcState, 2),
        VMSTATE_BOOL_V(fb_quiesce_pending, GnwH7B0LtdcState, 2),
        VMSTATE_BOOL_V(structural_transition_pending, GnwH7B0LtdcState, 2),
        VMSTATE_INT32_V(fb_quiesce_ticks, GnwH7B0LtdcState, 2),
        VMSTATE_INT32_V(srcr_idle_ticks, GnwH7B0LtdcState, 2),
        VMSTATE_INT64_V(snapshot_vblank_remaining_ns, GnwH7B0LtdcState, 2),
        VMSTATE_INT64_V(snapshot_line_remaining_ns, GnwH7B0LtdcState, 2),
        VMSTATE_INT64_V(snapshot_grid_remaining_ns, GnwH7B0LtdcState, 2),
        { .name = "frame", .version_id = 2, .size = sizeof(GnwH7B0LtdcState),
          .info = &ltdc_frame_info, .flags = VMS_SINGLE },
        VMSTATE_END_OF_LIST()
    }
};

static void gnw_h7b0_ltdc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = gnw_h7b0_ltdc_realize;
    dc->unrealize = gnw_h7b0_ltdc_unrealize;
    dc->vmsd = &vmstate_gnw_h7b0_ltdc;
    device_class_set_legacy_reset(dc, gnw_h7b0_ltdc_reset);
}

static const TypeInfo gnw_h7b0_ltdc_info = {
    .name          = TYPE_GNW_H7B0_LTDC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GnwH7B0LtdcState),
    .class_init    = gnw_h7b0_ltdc_class_init,
};

static void gnw_h7b0_ltdc_register_types(void)
{
    type_register_static(&gnw_h7b0_ltdc_info);
}

type_init(gnw_h7b0_ltdc_register_types)
