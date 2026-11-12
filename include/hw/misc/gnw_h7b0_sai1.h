/*
 * SAI1 (Serial Audio Interface) -- register shadow plus real audio
 * output for Block A (the only block game-and-watch-retro-go-sd's
 * gw_audio.c ever configures, via hsai_BlockA1). Block B's registers
 * are still just shadowed with no side effects.
 *
 * Real firmware never writes SAI1_ADR by hand: it calls
 * HAL_SAI_Transmit_DMA(&hsai_BlockA1, audiobuffer_dma, ...), which
 * hands the transfer to DMA1 Stream0 (see stm32h7xx_hal_msp.c's
 * HAL_SAI_MspInit, DMA_REQUEST_SAI1_A on DMA1_Stream0, memory-to-
 * peripheral, MINC enabled, circular, halfword-aligned). Rather than
 * model the SAI FIFO one halfword at a time, this device registers a
 * notifier (gnw_h7b0_dma_set_stream_notifier) on that fixed stream
 * and reads the just-completed half/full buffer straight out of guest
 * RAM at each transfer-complete tick DMA1 already generates, pushing
 * it to a QEMU AUD_* output voice. See gnw_h7b0_dma.c's block comment
 * for why that timing already matches real audio pacing.
 */
#ifndef HW_MISC_GNW_H7B0_SAI1_H
#define HW_MISC_GNW_H7B0_SAI1_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "qemu/audio.h"
#include "qemu/fifo8.h"
#include "qemu/notify.h"
#include "hw/misc/gnw_h7b0_dma.h"
#include "hw/misc/gnw_h7b0_rcc.h"

#define TYPE_GNW_H7B0_SAI1 "gnw-h7b0-sai1"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0Sai1State, GNW_H7B0_SAI1)

void gnw_h7b0_sai1_snapshot_prepare(GnwH7B0Sai1State *s);
void gnw_h7b0_sai1_snapshot_restore(GnwH7B0Sai1State *s);

#define GNW_H7B0_SAI1_SIZE 0x400

/* DMAMUX1 DMAREQ_ID for SAI1 Block A Tx (sai1_a_dma) -- see RM0455's
 * DMAMUX1 request table. The actual stream is resolved at runtime from
 * firmware's DMAMUX routing (retro-go: DMA1 Stream0; stock Zelda: DMA2
 * Stream6), see gnw_h7b0_dma_set_request_notifier(). */
#define GNW_H7B0_SAI1_DMA_REQUEST 87
#define GNW_H7B0_SAI1_DMA_PROVIDER_ID 0x53414901U /* "SAI1" */

/* Real wiring is fixed in hardware (not modeled via DMAMUX routing
 * here) -- see file comment above. */
#define GNW_H7B0_SAI1_DMA_STREAM 0

/*
 * ~8 DMA half-buffers' worth (assuming the ~2154-byte/1077-sample
 * half gw_audio.h's AUDIO_BUFFER_LENGTH implies) -- enough depth to
 * absorb normal DMA-tick/audio-backend timing jitter without the
 * queue running dry, without adding so much latency input lag becomes
 * noticeable.
 */
#define GNW_H7B0_SAI1_FIFO_CAPACITY (64 * 1024)

/* Maximum queue latency in milliseconds. DMA backpressure normally keeps
 * the queue below this bound; the capacity/drop guard is only a last-resort
 * safety net for a DMA half-buffer larger than the remaining space. */
#define GNW_H7B0_SAI1_MAX_QUEUE_MS 150
#define GNW_H7B0_SAI1_MIN_BACKPRESSURE_RATE_HZ 8000

/* Pause guest DMA at 75 ms and resume at half that depth, leaving a full
 * queue-cap margin for normal firmware DMA half-buffers. */
#define GNW_H7B0_SAI1_BACKPRESSURE_MS (GNW_H7B0_SAI1_MAX_QUEUE_MS / 2)

struct GnwH7B0Sai1State {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t regs[GNW_H7B0_SAI1_SIZE / 4];

    AudioBackend *audio_be;
    SWVoiceOut *voice;
    bool voice_open;
    /* Host presentation setting; guest DMA and sample timing remain active. */
    bool audio_muted;
    bool snapshot_restoring;
    Notifier exit_notifier;
    bool exit_notifier_registered;
    /* Host presentation rate currently programmed into voice. */
    uint32_t voice_rate_hz;

    /* Set via gnw_h7b0_sai1_set_dma so realize-order between SAI1 and
     * DMA1 in the SoC doesn't matter. */
    GnwH7B0DmaState *dma;

    /* Set via gnw_h7b0_sai1_set_rcc, same realize-order-independence
     * rationale as dma above. Used to decode the actual configured
     * sample rate (ACR1 MCKDIV/OSR combined with RCC's live PLL2
     * state) instead of assuming a fixed 48kHz -- see
     * gnw_h7b0_sai1_get_rate_hz() in gnw_h7b0_sai1.c. */
    GnwH7B0RccState *rcc;

    /*
     * PCM samples land here (gnw_h7b0_sai1_dma_notify) as soon as each
     * DMA half/full tick fires; gnw_h7b0_sai1_voice_cb -- called by
     * QEMU's own audio engine on ITS timer, at the real backend drain
     * rate -- pulls from this queue via AUD_write(). This decouples
     * "how fast our approximated DMA-tick cadence happens to fire"
     * from "how fast samples actually need to leave the speaker",
     * which is the actual fix for pitch/speed distortion: an earlier
     * version called AUD_write() directly from the DMA tick itself,
     * so any drift between our tick cadence and real playback rate
     * corrupted pitch instead of just building/draining queue depth.
     */
    Fifo8 fifo;
    bool fifo_inited;
    bool host_audio_backpressured;
    uint64_t produced_bytes;
    uint64_t consumed_bytes;
    uint64_t starvation_callbacks;
    uint64_t dropped_bytes;
};

typedef struct GnwH7B0Sai1Telemetry {
    uint32_t configured_rate_hz;
    uint32_t queued_bytes;
    uint64_t produced_bytes;
    uint64_t consumed_bytes;
    uint64_t starvation_callbacks;
    uint64_t dropped_bytes;
} GnwH7B0Sai1Telemetry;

void gnw_h7b0_sai1_set_dma(GnwH7B0Sai1State *s, GnwH7B0DmaState *dma);
void gnw_h7b0_sai1_set_rcc(GnwH7B0Sai1State *s, GnwH7B0RccState *rcc);
bool gnw_h7b0_sai1_get_telemetry(GnwH7B0Sai1Telemetry *out);

#endif
