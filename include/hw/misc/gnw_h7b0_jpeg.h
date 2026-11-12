/* Auto-generated stub for JPEG */
#ifndef HW_MISC_GNW_H7B0_JPEG_H
#define HW_MISC_GNW_H7B0_JPEG_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "qemu/thread.h"

#define TYPE_GNW_H7B0_JPEG "gnw-h7b0-jpeg"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0JpegState, GNW_H7B0_JPEG)

#define GNW_H7B0_JPEG_SIZE 0x1000

struct GnwH7B0JpegState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t regs[GNW_H7B0_JPEG_SIZE / 4];
    /* When enabled by the SoC's opt-in timing mode, decode inline so
     * guest-visible completion does not depend on host worker scheduling.
     * This does not model the codec's hardware latency. */
    bool timing_mode;

    GByteArray *inbuf;

    /* Decoded output: a full-resolution Y plane plus Cb/Cr planes
     * subsampled per the image's real SOF0 H/V sampling factors (to
     * match real hardware/firmware's expected total output size --
     * serving full-resolution, unsubsampled chroma made our DOR output
     * roughly 2x the size firmware's own destination buffer expects,
     * which firmware's own polling loop only partially drains, leaving
     * the tail of its buffer stale). DOR reads pop 4 bytes at a time
     * from a flat cursor over y_plane||cb_plane||cr_plane. */
    uint8_t *y_plane;
    uint8_t *cb_plane;
    uint8_t *cr_plane;
    int plane_width;
    int plane_height;
    int chroma_width;
    int chroma_height;
    uint32_t dor_cursor;

    /* Encode-direction (CONFR1.DE clear) state: raw YCbCr pixel bytes
     * accumulated from DIR writes until a full image's worth has
     * arrived (per CONFR3.XSIZE/CONFR1.YSIZE), then real-encoded into
     * encode_out; DOR reads drain encode_out via enc_dor_cursor,
     * mirroring the decode side's y/cb/cr_plane + dor_cursor pattern. */
    GByteArray *encode_out;
    uint32_t enc_dor_cursor;

    /*
     * Worker-thread offload for the actual decode (native + stb_image
     * fallback) + YCbCr conversion (the expensive scalar-compute part),
     * same category of fix as the LTDC compositor worker thread: real
     * firmware's HAL_JPEG_Decode() runs a tight, uninterruptible polling
     * loop with no other work interleaved (confirmed against
     * game-and-watch-retro-go-sd's hw_jpeg_decoder.c and the HAL
     * source), so this buys firmware itself no wall-clock speedup -- it
     * still polls the same number of times. What it does buy is not
     * holding BQL (and therefore stalling every other main-loop
     * consumer: audio callback pacing, display-refresh timers, gdbstub)
     * for the full duration of a real scalar JPEG decode, which used to
     * happen entirely inside one MMIO write handler call (the DIR write
     * that completes the image, on EOI detection).
     *
     * thread_lock protects only the fields below (job handoff + result
     * publish), never s->regs[] or the y/cb/cr_plane pointers above
     * directly -- those are only ever touched from the BQL thread, by
     * gnw_h7b0_jpeg_poll_worker() copying out of the pending_* fields
     * once decode_done is observed.
     */
    QemuThread thread;
    bool thread_started;
    QemuMutex thread_lock;
    QemuCond thread_cond;
    /* Signaled by the worker when decode_done goes true, so a status
     * poll can briefly (bounded) sleep instead of busy-returning "not
     * done" -- see gnw_h7b0_jpeg_poll_worker(). */
    QemuCond done_cond;
    bool worker_active;  /* thread_lock: includes compute outside the lock */
    bool job_pending;    /* BQL thread -> worker: a job is queued */
    bool lock_inited;    /* thread_lock/conds initialized (worker may not be) */
    bool decode_done;    /* worker -> BQL thread: pending_* is ready */
    bool stop_thread;
    GByteArray *job_input; /* snapshot of inbuf at EOI, owned by worker once posted */

    uint8_t *pending_y;
    uint8_t *pending_cb;
    uint8_t *pending_cr;
    int pending_width;
    int pending_height;
    int pending_chroma_width;
    int pending_chroma_height;
    int pending_comp;
    uint32_t pending_confrn1;
    /*
     * BQL-only: a decode job for the current operation has been posted to
     * the worker and its result not yet published. While set, further DIR
     * writes are ignored (real codec: input FIFO stops requesting data
     * after EOI -- SR.IFTF/IFNFF are cleared when the job is posted), so
     * a HAL feed loop that keeps polling can neither walk its source
     * pointer off the end of RAM nor have leftover bytes EOI-scanned into
     * a spurious second job. Cleared by poll_worker() on publish/discard
     * and by a new CONFR0.START.
     */
    bool decode_busy;
    uint32_t job_epoch;       /* current device epoch, bumped on reset (and
                               * on CONFR0.START, to discard any stale
                               * in-flight job from a previous operation) */
    uint32_t job_input_epoch; /* epoch the currently-queued job was posted under */
    uint32_t pending_epoch;   /* epoch the finished pending_* result belongs to */
};

void gnw_h7b0_jpeg_snapshot_quiesce(GnwH7B0JpegState *s, bool discard);

void gnw_h7b0_jpeg_get_last_decoded_size(uint32_t *width, uint32_t *height);
void gnw_h7b0_jpeg_get_last_chroma_size(uint32_t *width, uint32_t *height);

#endif
