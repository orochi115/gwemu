/*
 * SAI1 -- see gnw_h7b0_sai1.h for the overall approach (DMA1 Stream0
 * transfer-complete snoop -> Fifo8 queue -> AUD_write pulled by
 * QEMU's own audio timer, no per-sample SAI FIFO modeling).
 *
 * Earlier versions called AUD_write() directly from the DMA tick
 * (gnw_h7b0_sai1_dma_notify), i.e. push-model delivery paced by our
 * approximated DMA timing. That's the wrong architecture: QEMU's
 * audio_run() drains a SW voice's *own* buffer at the real backend
 * rate on its own timer, entirely independent of when we happen to
 * call AUD_write -- so any drift between our DMA-tick cadence and
 * real playback rate was audible directly as pitch/speed distortion,
 * not just occasional under/overrun. Queuing into a Fifo8 and only
 * calling AUD_write from gnw_h7b0_sai1_voice_cb (invoked by
 * audio_run() itself, telling us exactly how many bytes it can accept
 * right now) lets QEMU's audio core govern the real-time rate like it
 * does for every other AUD_* device -- our job becomes only "don't
 * starve the queue", not "get the timing exactly right ourselves".
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "migration/vmstate.h"
#include "hw/core/qdev-properties.h"
#include "system/system.h"
#include "hw/misc/gnw_h7b0_sai1.h"
#include "hw/misc/gnw_h7b0_regs_sai1.h"
#include "exec/cpu-common.h"
#include "hw/misc/gnw_env.h"

/* getenv() is a locked linear scan on Windows (msvcrt) -- never call it
 * per-event in emulation-hot paths; resolve once and cache. */
static bool gnw_audio_trace_enabled(void)
{
    static int v = -1;
    if (v < 0) {
        v = gnw_env_enabled("GNW_AUDIO_TRACE");
    }
    return v;
}

int64_t gnw_h7b0_audio_first_sample_ns = -1;
static GnwH7B0Sai1State *gnw_sai1_instance;

/*
 * Per-event (per-DMA-half) tracing only. Measured on Windows: printing
 * one line per half -- 60/s for a 22kHz retro-go core -- slowed the
 * emulated guest to a THIRD of realtime, i.e. the trace changed the
 * thing it was measuring (83% of the host audio buffer was silence
 * purely because of it). Keep the once-a-second summaries on
 * GNW_AUDIO_TRACE=1 and put anything per-event behind =2.
 */
static bool gnw_audio_trace_verbose(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("GNW_AUDIO_TRACE");
        v = (e && *e && atoi(e) >= 2);
    }
    return v;
}

#define SAI_xCR1_SAIEN  (1U << 16)
#define SAI_xCR1_DMAEN  (1U << 17)
#define SAI_xCR1_MCKDIV_SHIFT 20
#define SAI_xCR1_MCKDIV_MASK  (0x3fU << SAI_xCR1_MCKDIV_SHIFT)
#define SAI_xCR1_OSR          (1U << 26)
#define SAI_xCR1_NODIV        (1U << 19)
#define SAI_xCR1_MODE_MASK    0x3U
#define SAI_xCR1_PRTCFG_MASK  (0x3U << 2)
#define SAI_xCR1_DS_SHIFT     5
#define SAI_xCR1_DS_MASK      (0x7U << SAI_xCR1_DS_SHIFT)
#define SAI_xCR1_MONO         (1U << 12)
#define SAI_xCR1_SYNCEN_MASK  (0x3U << 10)
#define SAI_xFRCR_FRL_MASK    0xffU
#define SAI_xSLOTR_FBOFF_MASK 0x1fU
#define SAI_xSLOTR_SLOTSZ_SHIFT 6
#define SAI_xSLOTR_SLOTSZ_MASK  (0x3U << SAI_xSLOTR_SLOTSZ_SHIFT)
#define SAI_xSLOTR_NBSLOT_SHIFT 8
#define SAI_xSLOTR_NBSLOT_MASK  (0xfU << SAI_xSLOTR_NBSLOT_SHIFT)
#define SAI_xSLOTR_SLOTEN_SHIFT 16
#define SAI_xSLOTR_SLOTEN_MASK  (0xffffU << SAI_xSLOTR_SLOTEN_SHIFT)

static void gnw_h7b0_sai1_dma_state_changed(GnwH7B0Sai1State *s);
static void gnw_h7b0_sai1_update_voice(GnwH7B0Sai1State *s);

static void gnw_h7b0_sai1_close_voice(GnwH7B0Sai1State *s)
{
    if (s->voice) {
        if (s->voice_open) {
            audio_be_set_active_out(s->audio_be, s->voice, false);
        }
        audio_be_close_out(s->audio_be, s->voice);
        s->voice = NULL;
    }
    s->voice_open = false;
    s->voice_rate_hz = 0;
    if (s->fifo_inited) {
        fifo8_reset(&s->fifo);
    }
}

/* audio_cleanup() unparents audiodevs before QEMU's exit notifiers run.
 * An open SWVoiceOut holds the backend alive across that unparenting, so
 * close it here to release backend resources such as a WAV file header. */
static void gnw_h7b0_sai1_exit(Notifier *notifier, void *data)
{
    GnwH7B0Sai1State *s = container_of(notifier, GnwH7B0Sai1State,
                                       exit_notifier);

    gnw_h7b0_sai1_close_voice(s);
}

static void gnw_h7b0_sai1_unrealize(DeviceState *dev)
{
    GnwH7B0Sai1State *s = GNW_H7B0_SAI1(dev);

    if (s->exit_notifier_registered) {
        qemu_remove_exit_notifier(&s->exit_notifier);
        s->exit_notifier_registered = false;
    }
    gnw_h7b0_sai1_close_voice(s);
    if (s->fifo_inited) {
        fifo8_destroy(&s->fifo);
        s->fifo_inited = false;
    }
    if (gnw_sai1_instance == s) {
        gnw_sai1_instance = NULL;
    }
}

/* gw_audio.c only ever configures Block A for 16-bit I2S at
 * AUDIO_SAMPLE_RATE (gw_audio.h) -- see task summary; no need to
 * decode AFRCR/ASLOTR/ACR1 DS bits, this is the only mode used.
 *
 * Content is mono, not stereo-interleaved: main.c sets
 * hsai_BlockA1.Init.MonoStereoMode = SAI_MONOMODE (the SAI hardware
 * itself duplicates one data slot to both output channels), and every
 * porting mixer (main_*.c) confirms this by writing one sample per
 * audiobuffer_dma[] index (e.g. gwenesis main_gwenesis.c line 242,
 * sound_buffer[i] = ...), not interleaved L/R pairs. Opening the
 * AUD_* voice as stereo and feeding it these mono samples directly
 * scrambled consecutive samples across L/R -- audible as distortion,
 * not just the buffer-timing stutter from gnw_h7b0_dma.c's
 * approximated pacing. */
#define GNW_H7B0_SAI1_RATE 48000
#define GNW_H7B0_SAI1_CHANNELS 1

/*
 * Decode the actual configured sample rate from ACR1's MCKDIV/OSR bits
 * combined with RCC's live PLL2 state, instead of assuming a fixed
 * 48kHz -- see gnw_h7b0_rcc.h and the audio-jank investigation this
 * fixes: game-and-watch-retro-go-sd's odroid_audio.c reprograms PLL2
 * and MCKDIV at runtime per game core (e.g. 22050/32768/53050Hz), and
 * without this our DMA pacing and host audio backend voice both stayed
 * silently stuck at 48kHz for every core except NES (whose 48kHz
 * request happens to match the boot-time default).
 *
 * Formula (matches HAL_SAI_InitFrameClock's NODIV=0 case, which is the
 * only NoDivider setting this firmware ever uses -- see main.c's
 * hsai_BlockA1.Init.NoDivider = SAI_MASTERDIVIDER_ENABLE, meaning the
 * ACR1 NODIV bit is clear):
 *   rate = SAI1_kernel_clk / (MCKDIV * (OSR ? 512 : 256))
 * Verified bit-exact against HAL_RCCEx_GetPLL2ClockFreq's actual compiled
 * float-math disassembly (retro-go-temp/elf/gw_retro_go_bank1.elf) for the
 * PLL2 VCO/PLL2P computation itself.
 *
 * Known imprecise case: some cores' set_audio_frequency() (odroid_audio.c)
 * skip HAL's auto-MCKDIV computation and instead assign hsai->Init.Mckdiv
 * directly with a hand-picked constant (AudioFrequency left at the
 * SAI_AUDIO_FREQUENCY_MCKDIV sentinel) -- confirmed via live disassembly
 * that this build's Celeste core does this for its 22050Hz target
 * (Mckdiv=8, not the ~3 the auto-formula would derive from this PLL2
 * config), same as the already-known G&W-native/Genesis manual cases.
 * Those constants were apparently tuned empirically against real hardware
 * output, not from this formula, so decoding them back through this
 * formula won't always reproduce the exact intended Hz for those specific
 * cores -- but DMA pacing and the host audio voice both derive their rate
 * from this same decode, so they stay internally consistent with each
 * other either way: the dma_counter race this whole change targets (game
 * logic vs. audio DMA completion falling out of sync, see gnw_h7b0_dma.c)
 * is fixed regardless of whether the absolute Hz is spot-on, since that
 * race only depends on both sides agreeing on the same rate, not on the
 * rate being perfectly correct.
 *
 * Falls back to GNW_H7B0_SAI1_RATE if RCC isn't wired or the kernel clock
 * cannot be determined (for example, an external source) -- preserving the
 * working NES behavior rather than producing garbage. RM0455 defines an
 * MCKDIV field value of zero as divide-by-one.
 */
static uint32_t gnw_h7b0_sai1_get_rate_hz(GnwH7B0Sai1State *s)
{
    uint32_t acr1 = s->regs[GNW_H7B0_SAI1_SAI_ACR1_OFFSET >> 2];
    uint32_t mckdiv = (acr1 & SAI_xCR1_MCKDIV_MASK) >> SAI_xCR1_MCKDIV_SHIFT;
    uint32_t kernel_hz;

    if (!s->rcc) {
        return GNW_H7B0_SAI1_RATE;
    }

    kernel_hz = gnw_h7b0_rcc_get_sai1_kernel_hz(s->rcc);
    if (kernel_hz == 0) {
        return GNW_H7B0_SAI1_RATE;
    }

    mckdiv = MAX(mckdiv, 1U);

    /*
     * NODIV=1 (stock Zelda's config: PLL2P ~= 12.288MHz, MCKDIV=4,
     * FRL+1=64 -> exactly 48000Hz) bypasses the /256 master divider:
     * SCK = sai_ker_ck / MCKDIV, and the frame rate is SCK divided by
     * the frame length (FRCR.FRL+1). The original NODIV=0-only formula
     * decoded this config as 12kHz -- audibly "deep and slow", and,
     * since DMA pacing derives from this same rate, ran the entire
     * audio-paced firmware at quarter speed.
     */
    if (acr1 & SAI_xCR1_NODIV) {
        uint32_t frl1 = (s->regs[GNW_H7B0_SAI1_SAI_AFRCR_OFFSET >> 2]
                          & 0xFF) + 1;
        return kernel_hz / (mckdiv * frl1);
    }

    return kernel_hz / (mckdiv * ((acr1 & SAI_xCR1_OSR) ? 512 : 256));
}

/* Return the exact clock divider for configurations whose DMA request is
 * one 16-bit sample per audio frame. Unknown SAI protocols remain on the
 * legacy pacing path rather than acquiring a guessed cadence. */
static bool gnw_h7b0_sai1_frame_divider(GnwH7B0Sai1State *s,
                                         uint32_t *divider)
{
    uint32_t acr1 = s->regs[GNW_H7B0_SAI1_SAI_ACR1_OFFSET >> 2];
    uint32_t afrcr = s->regs[GNW_H7B0_SAI1_SAI_AFRCR_OFFSET >> 2];
    uint32_t slotr = s->regs[GNW_H7B0_SAI1_SAI_ASLOTR_OFFSET >> 2];
    uint32_t mckdiv = (acr1 & SAI_xCR1_MCKDIV_MASK) >>
                      SAI_xCR1_MCKDIV_SHIFT;
    uint32_t frame_length = (afrcr & SAI_xFRCR_FRL_MASK) + 1;
    uint32_t slots = ((slotr & SAI_xSLOTR_NBSLOT_MASK) >>
                      SAI_xSLOTR_NBSLOT_SHIFT) + 1;
    uint32_t slotsz = (slotr & SAI_xSLOTR_SLOTSZ_MASK) >>
                      SAI_xSLOTR_SLOTSZ_SHIFT;
    uint32_t slot_width;
    uint32_t enabled_slots = (slotr & SAI_xSLOTR_SLOTEN_MASK) >>
                             SAI_xSLOTR_SLOTEN_SHIFT;

    /* MODE=master transmitter, PRTCFG=free protocol, DS=16-bit, MONO=1.
     * Slot sizes 0 and 1 both carry 16-bit samples; size 2 pads each slot
     * to 32 bits. One mono sample per frame is supported only for the
     * documented two-slot configuration. */
    if ((acr1 & SAI_xCR1_MODE_MASK) != 0 ||
        (acr1 & SAI_xCR1_PRTCFG_MASK) != 0 ||
        ((acr1 & SAI_xCR1_DS_MASK) >> SAI_xCR1_DS_SHIFT) != 4 ||
        !(acr1 & SAI_xCR1_MONO) ||
        (acr1 & SAI_xCR1_SYNCEN_MASK) != 0 ||
        (slotr & SAI_xSLOTR_FBOFF_MASK) != 0 ||
        slots != 2 || slotsz > 2 ||
        (enabled_slots & 0x3U) != 0x3U) {
        return false;
    }
    slot_width = slotsz == 2 ? 32 : 16;
    if (frame_length != slots * slot_width || frame_length < 8 ||
        frame_length > 256) {
        return false;
    }
    if (!(acr1 & SAI_xCR1_NODIV) &&
        (frame_length & (frame_length - 1)) != 0) {
        return false;
    }

    /* RM0455 specifies MCKDIV=0 as equivalent to one. */
    mckdiv = MAX(mckdiv, 1U);
    if (acr1 & SAI_xCR1_NODIV) {
        *divider = mckdiv * frame_length;
    } else {
        *divider = mckdiv * ((acr1 & SAI_xCR1_OSR) ? 512U : 256U);
    }
    return *divider != 0;
}

static uint32_t gnw_h7b0_sai1_gcd(uint32_t a, uint32_t b)
{
    while (b) {
        uint32_t r = a % b;
        a = b;
        b = r;
    }
    return a;
}

static GnwH7B0DmaRequestState gnw_h7b0_sai1_request_state(void *opaque)
{
    GnwH7B0Sai1State *s = opaque;
    uint32_t acr1 = s->regs[GNW_H7B0_SAI1_SAI_ACR1_OFFSET >> 2];
    uint32_t kernel_hz;
    uint32_t divider;
    bool active = (acr1 & (SAI_xCR1_SAIEN | SAI_xCR1_DMAEN)) ==
                  (SAI_xCR1_SAIEN | SAI_xCR1_DMAEN);
    GnwH7B0DmaRequestState state = {
        .rate_state = GNW_H7B0_DMA_RATE_UNKNOWN,
        .request_active = active,
        .provider_identity = GNW_H7B0_SAI1_DMA_PROVIDER_ID,
    };

    if (!s->rcc) {
        return state;
    }
    if (!gnw_h7b0_rcc_sai1_clock_enabled(s->rcc)) {
        state.rate_state = GNW_H7B0_DMA_RATE_STOPPED;
        return state;
    }
    if (!gnw_h7b0_rcc_get_sai1_clock_info(s->rcc, &kernel_hz)) {
        return state;
    }
    if (!kernel_hz) {
        state.rate_state = GNW_H7B0_DMA_RATE_STOPPED;
        return state;
    }
    if (!gnw_h7b0_sai1_frame_divider(s, &divider)) {
        return state;
    }

    uint32_t divisor = gnw_h7b0_sai1_gcd(kernel_hz, divider);
    state.rate_state = GNW_H7B0_DMA_RATE_RUNNING;
    state.item_rate_num = kernel_hz / divisor;
    state.item_rate_den = divider / divisor;
    return state;
}

static void gnw_h7b0_sai1_dma_state_changed(GnwH7B0Sai1State *s)
{
    if (s->dma) {
        gnw_h7b0_dma_request_state_changed(
            s->dma, GNW_H7B0_SAI1_DMA_REQUEST);
    }
}

static void gnw_h7b0_sai1_rcc_clock_changed(void *opaque)
{
    GnwH7B0Sai1State *s = GNW_H7B0_SAI1(opaque);

    gnw_h7b0_sai1_update_voice(s);
    gnw_h7b0_sai1_dma_state_changed(s);
}

static void gnw_h7b0_sai1_dma_notify(void *opaque, bool half,
                                      uint32_t m0ar, uint32_t ndtr)
{
    GnwH7B0Sai1State *s = opaque;
    uint32_t regs_acr1 = s->regs[GNW_H7B0_SAI1_SAI_ACR1_OFFSET >> 2];

    if (!(regs_acr1 & SAI_xCR1_SAIEN) || !s->voice_open) {
        return;
    }

    /* ndtr is the stream's configured item count (halfwords, per
     * HAL_DMA_Init's PeriphDataAlignment=HALFWORD); half-transfer
     * covers the first ndtr/2 items, full-transfer the second half.
     * Each item is one int16_t sample (stereo-interleaved), so bytes
     * transferred this half = (ndtr/2) * 2. */
    uint32_t half_items = ndtr / 2;
    uint32_t half_bytes = half_items * sizeof(int16_t);
    uint32_t addr = m0ar + (half ? 0 : half_bytes);

    if (half_bytes == 0 || !s->fifo_inited) {
        return;
    }

    if (half_bytes > GNW_H7B0_SAI1_FIFO_CAPACITY) {
        /*
         * Pathological NDTR (bigger than this device's whole queue) --
         * clamp rather than let fifo8_drop()/push_all() below violate
         * their preconditions. Real firmware never configures a single
         * DMA half anywhere near this size; this is a defensive bound,
         * not an expected path.
         */
        half_bytes = GNW_H7B0_SAI1_FIFO_CAPACITY;
    }

    /* Keep a hard latency bound as a last defense. Normal run-ahead is
     * stopped earlier by gnw_h7b0_sai1_dma_request_active(); this drop path
     * only handles a single DMA half-buffer that cannot fit in the bound. */
    uint32_t rate = gnw_h7b0_sai1_get_rate_hz(s);
    uint32_t max_queued = rate * sizeof(int16_t) *
                          GNW_H7B0_SAI1_MAX_QUEUE_MS / 1000;
    if (max_queued > GNW_H7B0_SAI1_FIFO_CAPACITY) {
        max_queued = GNW_H7B0_SAI1_FIFO_CAPACITY;
    }
    if (max_queued < half_bytes * 2) {
        max_queued = half_bytes * 2;   /* never below one full DMA cycle */
    }

    uint32_t queued = fifo8_num_used(&s->fifo);
    if (queued + half_bytes > max_queued) {
        uint32_t excess = queued + half_bytes - max_queued;
        if (excess > queued) {
            excess = queued;
        }
        if (gnw_audio_trace_verbose()) {
            fprintf(stderr, "DROP %u %" PRId64 "\n", excess,
                    qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        }
        fifo8_drop(&s->fifo, excess);
        s->dropped_bytes += excess;
    }

    if (half_bytes > fifo8_num_free(&s->fifo)) {
        /* Defensive: keep fifo8_push_all()'s precondition regardless. */
        uint32_t drop = half_bytes - fifo8_num_free(&s->fifo);
        fifo8_drop(&s->fifo, drop);
        s->dropped_bytes += drop;
    }

    if (gnw_audio_trace_verbose()) {
        fprintf(stderr, "DN %d %" PRId64 "\n", half,
                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }

    g_autofree uint8_t *buf = g_malloc(half_bytes);
    cpu_physical_memory_read(addr, buf, half_bytes);
    {
        const char *tap = getenv("GNW_AUDIO_TAP");
        if (tap) {
            static FILE *tapf;
            if (!tapf) {
                tapf = fopen(tap, "wb");
            }
            if (tapf) {
                fwrite(buf, 1, half_bytes, tapf);
                fflush(tapf);
            }
        }
    }
    fifo8_push_all(&s->fifo, buf, half_bytes);
    s->produced_bytes += half_bytes;
}

/*
 * Called by QEMU's own audio_run() on its real-time timer, telling us
 * exactly how many bytes the backend can accept right now -- this
 * (not the DMA tick) is what should govern actual playback rate. See
 * file header for why pushing from the DMA tick directly caused
 * pitch/speed distortion instead of just occasional glitches.
 */
static void gnw_h7b0_sai1_voice_cb(void *opaque, int avail)
{
    GnwH7B0Sai1State *s = opaque;
    int wanted = avail;

    while (avail > 0 && !fifo8_is_empty(&s->fifo)) {
        uint32_t chunk;
        /*
         * fifo8_peek_bufptr()'s max must not exceed what's actually
         * queued (fifo8.h's documented precondition) -- avail is what
         * the *backend* can accept, which is routinely more than our
         * queue currently holds (e.g. right after SAIEN, before the
         * queue has filled up), so it has to be clamped here or this
         * aborts.
         */
        uint32_t max = MIN((uint32_t)avail, fifo8_num_used(&s->fifo));
        const uint8_t *ptr = fifo8_peek_bufptr(&s->fifo, max, &chunk);
        int n = audio_be_write(s->audio_be, s->voice, (void *)ptr, chunk);

        if (n > 0 && gnw_h7b0_audio_first_sample_ns < 0) {
            gnw_h7b0_audio_first_sample_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        }

        if (n <= 0) {
            break;
        }
        /* A short or blocked backend write leaves the remaining PCM queued. */
        fifo8_drop(&s->fifo, n);
        s->consumed_bytes += n;
        avail -= n;
    }
    if (wanted > 0 && avail > 0) {
        s->starvation_callbacks++;
    }

    /*
     * GNW_AUDIO_TRACE: how often QEMU's audio_run() actually pulls from
     * us, and how much it could take. `cb` is that call rate -- if it is
     * far below the backend's own callback rate, the mixer buffer starves
     * and audio/sdl3audio.c ends up injecting silence regardless of how
     * perfectly the guest produced its samples.
     */
    if (gnw_audio_trace_enabled() && s->voice_open) {
        static int64_t t0;
        static uint64_t cb, taken, empty;
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        cb++;
        taken += (uint64_t)(wanted - avail);
        if (avail > 0) {
            empty++;
        }
        if (now - t0 >= 1000000000LL) {
            fprintf(stderr, "SAIPULL cb=%" PRIu64 " took=%" PRIu64 "B "
                    "ranout=%" PRIu64 " fifo=%u\n",
                    cb, taken, empty, fifo8_num_used(&s->fifo));
            t0 = now;
            cb = taken = empty = 0;
        }
    }
}

/* Wrapper matching GnwH7B0DmaStreamRateFn's signature, registered with
 * the DMA controller so its half/full-transfer pacing tracks the real
 * configured rate instead of assuming a flat item rate -- see
 * gnw_h7b0_sai1_get_rate_hz() above and gnw_h7b0_dma.c's use of it. */
static uint32_t gnw_h7b0_sai1_dma_rate_fn(void *opaque)
{
    return gnw_h7b0_sai1_get_rate_hz(opaque);
}

/*
 * GnwH7B0DmaStreamActiveFn for the SAI1_A audio stream: is the SAI block
 * actually pulling data out of the DMA right now?
 *
 * Three things must hold on real silicon for SAI_A to assert its DMA
 * request line (RM0455 SAI chapter: the audio block shifts data on
 * sai_a_ck derived from sai_x_ker_ck, and only requests DMA service
 * when enabled with DMA output enabled):
 *   - ACR1.SAIEN: the audio block is enabled,
 *   - ACR1.DMAEN: it is served by DMA rather than by CPU writes,
 *   - a live kernel clock: with sai_x_ker_ck stopped (RCC gating the
 *     peripheral, or PLL2 -- the SAI1SEL source retro-go uses -- turned
 *     off), the block's bit clock stops, no FIFO slot ever drains, and
 *     no request is ever made.
 *
 * The last one is the one that matters here: retro-go's "quit to main
 * menu" tears the audio clock down BEFORE it disables DMA1 Stream0 and
 * clears its flags, and in that window its DMA1_Stream0 ISR no longer
 * acknowledges anything. Real hardware raises no interrupt there
 * because the stream has quietly stalled; we used to raise one anyway
 * and wedge the guest in a permanent exception-27 storm.
 */
static bool gnw_h7b0_sai1_dma_request_active(void *opaque)
{
    GnwH7B0Sai1State *s = opaque;
    uint32_t acr1 = s->regs[GNW_H7B0_SAI1_SAI_ACR1_OFFSET >> 2];

    if (!(acr1 & SAI_xCR1_SAIEN) || !(acr1 & SAI_xCR1_DMAEN)) {
        s->host_audio_backpressured = false;
        return false;
    }

    /* Use the same clock-state decoder as the precise DMA provider. The
     * compatibility clock getter recognizes only PLL2 and cannot distinguish
     * an external/unknown source from a stopped source. Host queue pressure
     * is presentation state and must never gate timing-mode DMA. */
    if (s->dma && s->dma->timing_mode) {
        GnwH7B0DmaRequestState state = gnw_h7b0_sai1_request_state(s);

        s->host_audio_backpressured = false;
        return state.request_active &&
               state.rate_state != GNW_H7B0_DMA_RATE_STOPPED;
    }
    /* No RCC wired up at all: can't tell, assume clocked (the old
     * behavior) rather than silently killing audio. */
    if (s->rcc && gnw_h7b0_rcc_get_sai1_kernel_hz(s->rcc) == 0) {
        s->host_audio_backpressured = false;
        return false;
    }

    /* The host audio backend consumes samples at wall-clock rate.  If the
     * guest produces PCM faster, stop asserting SAI's DMA request until the
     * queue has drained.  The DMA model then stalls exactly as it does when
     * the peripheral stops requesting data; firmware waiting on its audio
     * DMA edge cannot run the game faster or force the host to drop PCM. */
    uint32_t rate = gnw_h7b0_sai1_get_rate_hz(s);
    if (s->voice_open && s->fifo_inited &&
        rate >= GNW_H7B0_SAI1_MIN_BACKPRESSURE_RATE_HZ) {
        uint32_t high_water = rate * sizeof(int16_t) *
                              GNW_H7B0_SAI1_BACKPRESSURE_MS / 1000;
        uint32_t low_water = high_water / 2;
        uint32_t queued = fifo8_num_used(&s->fifo);

        if (s->host_audio_backpressured) {
            if (queued <= low_water) {
                s->host_audio_backpressured = false;
            }
        } else if (queued >= high_water) {
            s->host_audio_backpressured = true;
        }
        if (s->host_audio_backpressured) {
            return false;
        }
    } else {
        s->host_audio_backpressured = false;
    }
    return true;
}

/* Registration and its activity predicate always travel together --
 * clearing a registration frees the whole slot, so the predicate has to
 * be (re)attached after every set_request_notifier() call. */
static void gnw_h7b0_sai1_bind_dma(GnwH7B0Sai1State *s)
{
    if (!s->dma) {
        return;
    }
    gnw_h7b0_dma_set_request_notifier(s->dma, GNW_H7B0_SAI1_DMA_REQUEST,
                                       gnw_h7b0_sai1_dma_notify, s,
                                       gnw_h7b0_sai1_dma_rate_fn, s, false);
    gnw_h7b0_dma_set_request_active_fn(s->dma, GNW_H7B0_SAI1_DMA_REQUEST,
                                        gnw_h7b0_sai1_dma_request_active, s);
    gnw_h7b0_dma_set_request_state_fn(s->dma, GNW_H7B0_SAI1_DMA_REQUEST,
                                       gnw_h7b0_sai1_request_state, s);
}

static void gnw_h7b0_sai1_update_voice(GnwH7B0Sai1State *s)
{
    uint32_t regs_acr1 = s->regs[GNW_H7B0_SAI1_SAI_ACR1_OFFSET >> 2];
    bool want_enabled = (regs_acr1 & SAI_xCR1_SAIEN) != 0;
    uint32_t rate = gnw_h7b0_sai1_get_rate_hz(s);

    /* RCC can retune the selected kernel clock while SAIEN remains set.
     * Keep the backend voice on the same live rate used by DMA pacing. */
    if (want_enabled && (!s->voice_open || s->voice_rate_hz != rate)) {
        struct audsettings as = {
            .freq = rate,
            .nchannels = GNW_H7B0_SAI1_CHANNELS,
            .fmt = AUDIO_FORMAT_S16,
            .big_endian = false,
        };

        s->voice_open = false;
        s->voice_rate_hz = 0;
        s->voice = audio_be_open_out(s->audio_be, s->voice, "gnw-h7b0-sai1",
                                      s, gnw_h7b0_sai1_voice_cb, &as);
        if (s->voice) {
            audio_be_set_volume_out_lr(s->audio_be, s->voice,
                                      s->audio_muted, 255, 255);
            audio_be_set_active_out(s->audio_be, s->voice, true);
            s->voice_open = true;
            s->voice_rate_hz = rate;
        }
    } else if (!want_enabled && s->voice_open) {
        audio_be_set_active_out(s->audio_be, s->voice, false);
        s->voice_open = false;
        s->voice_rate_hz = 0;
        fifo8_reset(&s->fifo);
    }
}

void gnw_h7b0_sai1_set_dma(GnwH7B0Sai1State *s, GnwH7B0DmaState *dma)
{
    s->dma = dma;
    /*
     * Bind once, for the life of the machine -- NOT tied to SAIEN.
     *
     * This registration is what tells the DMA controller "DMAMUX request
     * 87 is me": it supplies the transfer notifier, the real sample-rate
     * fn, and the request-activity predicate. It used to be registered
     * on SAIEN 0->1 and *cleared* on SAIEN 1->0, which conflated the
     * host-side audio voice with firmware's DMAMUX routing, and left the
     * DMA stream with no predicate and no rate fn during exactly the
     * teardown window this all matters in: the stream then free-ran at
     * the generic 48kHz fallback, firing half/full-transfer interrupts
     * at a SAI that was already switched off. All three callbacks
     * already no-op or report inactive when SAIEN is clear, so there is
     * nothing to gain by unbinding.
     */
    gnw_h7b0_sai1_bind_dma(s);
}

void gnw_h7b0_sai1_set_rcc(GnwH7B0Sai1State *s, GnwH7B0RccState *rcc)
{
    s->rcc = rcc;
    if (rcc) {
        gnw_h7b0_rcc_add_sai1_clock_observer(
            rcc, gnw_h7b0_sai1_rcc_clock_changed, s);
    }
    gnw_h7b0_sai1_dma_state_changed(s);
}

static void gnw_h7b0_sai1_reset(DeviceState *dev)
{
    GnwH7B0Sai1State *s = GNW_H7B0_SAI1(dev);
    for (int i = 0; i < (GNW_H7B0_SAI1_SIZE / 4); i++) {
        s->regs[i] = get_sai1_reset_value(i * 4);
    }
    if (s->voice_open) {
        audio_be_set_active_out(s->audio_be, s->voice, false);
        s->voice_open = false;
        s->voice_rate_hz = 0;
        fifo8_reset(&s->fifo);
    }
    s->produced_bytes = 0;
    s->consumed_bytes = 0;
    s->starvation_callbacks = 0;
    s->dropped_bytes = 0;
    gnw_h7b0_sai1_dma_state_changed(s);
}

#define SAI_xSR_FLVL_FULL (4U << 16)

static uint64_t gnw_h7b0_sai1_read(void *opaque, hwaddr addr, unsigned int size)
{
    GnwH7B0Sai1State *s = GNW_H7B0_SAI1(opaque);
    if (addr >= GNW_H7B0_SAI1_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n", __func__, addr);
        return 0;
    }

    /* HAL_SAI_Transmit_DMA busy-waits on ASR/BSR's FLVL field going
     * non-empty right after setting CR1.DMAEN, before it ever enables
     * SAIEN (see stm32h7xx_hal_sai.c's HAL_SAI_Transmit_DMA) -- same
     * "firmware polls a status flag our stub never sets" pattern as
     * SPI1 RXP/JPEG EOCF/etc (see docs/session-2026-07-10-part3).
     * With FLVL permanently 0 (reset value), that wait never ends and
     * SAIEN -- and therefore all audio output -- never happens. Once
     * DMAEN is set, report the FIFO as full; real hardware would have
     * it filling continuously off the DMA request line we don't model
     * at that granularity. */
    if (addr == GNW_H7B0_SAI1_SAI_ASR_OFFSET &&
        (s->regs[GNW_H7B0_SAI1_SAI_ACR1_OFFSET >> 2] & SAI_xCR1_DMAEN)) {
        return s->regs[addr >> 2] | SAI_xSR_FLVL_FULL;
    }
    if (addr == GNW_H7B0_SAI1_SAI_BSR_OFFSET &&
        (s->regs[GNW_H7B0_SAI1_SAI_BCR1_OFFSET >> 2] & SAI_xCR1_DMAEN)) {
        return s->regs[addr >> 2] | SAI_xSR_FLVL_FULL;
    }

    return s->regs[addr >> 2];
}

static void gnw_h7b0_sai1_write(void *opaque, hwaddr addr, uint64_t val64, unsigned int size)
{
    GnwH7B0Sai1State *s = GNW_H7B0_SAI1(opaque);
    if (addr >= GNW_H7B0_SAI1_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n", __func__, addr);
        return;
    }
    uint32_t mask = get_sai1_write_mask(addr);
    s->regs[addr >> 2] = (s->regs[addr >> 2] & ~mask) | ((uint32_t)val64 & mask);

    if (addr == GNW_H7B0_SAI1_SAI_ACR1_OFFSET) {
        if (gnw_dma_trace_level()) {
            uint32_t nv = s->regs[addr >> 2];
            gnw_dma_trace_event("ACR1", "0x%08x SAIEN=%d DMAEN=%d", nv,
                                !!(nv & SAI_xCR1_SAIEN),
                                !!(nv & SAI_xCR1_DMAEN));
        }
        gnw_h7b0_sai1_update_voice(s);
    }
    if (addr == GNW_H7B0_SAI1_SAI_AFRCR_OFFSET) {
        gnw_h7b0_sai1_update_voice(s);
    }
    if (addr == GNW_H7B0_SAI1_SAI_ACR1_OFFSET ||
        addr == GNW_H7B0_SAI1_SAI_AFRCR_OFFSET ||
        addr == GNW_H7B0_SAI1_SAI_ASLOTR_OFFSET) {
        gnw_h7b0_sai1_dma_state_changed(s);
    }
}

static const MemoryRegionOps gnw_h7b0_sai1_ops = {
    .read = gnw_h7b0_sai1_read,
    .write = gnw_h7b0_sai1_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void gnw_h7b0_sai1_init(Object *obj)
{
    GnwH7B0Sai1State *s = GNW_H7B0_SAI1(obj);
    memory_region_init_io(&s->mmio, obj, &gnw_h7b0_sai1_ops, s, TYPE_GNW_H7B0_SAI1, GNW_H7B0_SAI1_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static void gnw_h7b0_sai1_realize(DeviceState *dev, Error **errp)
{
    GnwH7B0Sai1State *s = GNW_H7B0_SAI1(dev);

    if (!audio_be_check(&s->audio_be, errp)) {
        return;
    }
    fifo8_create(&s->fifo, GNW_H7B0_SAI1_FIFO_CAPACITY);
    s->fifo_inited = true;
    gnw_sai1_instance = s;
    s->exit_notifier.notify = gnw_h7b0_sai1_exit;
    qemu_add_exit_notifier(&s->exit_notifier);
    s->exit_notifier_registered = true;
}

bool gnw_h7b0_sai1_get_telemetry(GnwH7B0Sai1Telemetry *out)
{
    GnwH7B0Sai1State *s = gnw_sai1_instance;

    if (!s || !out) {
        return false;
    }
    *out = (GnwH7B0Sai1Telemetry) {
        .configured_rate_hz = gnw_h7b0_sai1_get_rate_hz(s),
        .queued_bytes = s->fifo_inited ? fifo8_num_used(&s->fifo) : 0,
        .produced_bytes = s->produced_bytes,
        .consumed_bytes = s->consumed_bytes,
        .starvation_callbacks = s->starvation_callbacks,
        .dropped_bytes = s->dropped_bytes,
    };
    return true;
}

static const VMStateDescription vmstate_gnw_h7b0_sai1 = {
    .name = TYPE_GNW_H7B0_SAI1,
    .version_id = 2,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, GnwH7B0Sai1State, GNW_H7B0_SAI1_SIZE / 4),
        VMSTATE_UINT64_V(produced_bytes, GnwH7B0Sai1State, 2),
        VMSTATE_UINT64_V(consumed_bytes, GnwH7B0Sai1State, 2),
        VMSTATE_UINT64_V(starvation_callbacks, GnwH7B0Sai1State, 2),
        VMSTATE_UINT64_V(dropped_bytes, GnwH7B0Sai1State, 2),
        VMSTATE_END_OF_LIST()
    }
};

static const Property gnw_h7b0_sai1_properties[] = {
    DEFINE_AUDIO_PROPERTIES(GnwH7B0Sai1State, audio_be),
};

static bool gnw_h7b0_sai1_get_audio_muted(Object *obj, Error **errp)
{
    return GNW_H7B0_SAI1(obj)->audio_muted;
}

static void gnw_h7b0_sai1_set_audio_muted(Object *obj, bool muted, Error **errp)
{
    GnwH7B0Sai1State *s = GNW_H7B0_SAI1(obj);
    s->audio_muted = muted;
    if (s->voice) {
        audio_be_set_volume_out_lr(s->audio_be, s->voice, muted, 255, 255);
    }
}

static void gnw_h7b0_sai1_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->vmsd = &vmstate_gnw_h7b0_sai1;
    dc->realize = gnw_h7b0_sai1_realize;
    dc->unrealize = gnw_h7b0_sai1_unrealize;
    device_class_set_legacy_reset(dc, gnw_h7b0_sai1_reset);
    device_class_set_props(dc, gnw_h7b0_sai1_properties);
    object_class_property_add_bool(klass, "audio-muted",
                                   gnw_h7b0_sai1_get_audio_muted,
                                   gnw_h7b0_sai1_set_audio_muted);
}

static const TypeInfo gnw_h7b0_sai1_info = {
    .name          = TYPE_GNW_H7B0_SAI1,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GnwH7B0Sai1State),
    .instance_init = gnw_h7b0_sai1_init,
    .class_init    = gnw_h7b0_sai1_class_init,
};

static void gnw_h7b0_sai1_register_types(void)
{
    type_register_static(&gnw_h7b0_sai1_info);
}
type_init(gnw_h7b0_sai1_register_types)
