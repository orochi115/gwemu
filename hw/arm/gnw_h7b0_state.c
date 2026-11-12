/* Native snapshots wrap the entire QEMU VMState, including raw Flash RAM.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu-version.h"
#include "qapi/error.h"
#include "qemu/units.h"
#include "qemu/config-file.h"
#include "qemu/option.h"
#include "qemu/yank.h"
#include "io/channel-file.h"
#include "io/channel-buffer.h"
#include "migration/qemu-file.h"
#include "migration/savevm.h"
#include "migration/migration.h"
#include "migration/global_state.h"
#include "migration/vmstate.h"
#include "system/blockdev.h"
#include "system/runstate.h"
#include "system/reset.h"
#include "system/system.h"
#include "gnw_h7b0_state.h"
#ifdef _WIN32
#include <windows.h>
#endif

#define STATE_MAGIC 0x4757454d55535431ULL /* GWEMUST1, distinct from GNWSTAT1 */
#define STATE_VERSION 1
#define IDENTITY_SIZE 228
#define HEADER_SIZE (IDENTITY_SIZE + 8 + 32)
#define MAX_PAYLOAD (256 * MiB)

static int state_clock_pre_save(void *opaque)
{
    GnwH7B0Snapshot *s = opaque;
    s->rtc_clock_at_save = qemu_clock_get_ns(s->soc->rtc.clock);
    return 0;
}

static int state_clock_post_load(void *opaque, int version_id)
{
    GnwH7B0Snapshot *s = opaque;
    GnwH7B0RtcState *rtc = &s->soc->rtc;
    int64_t delta = qemu_clock_get_ns(rtc->clock) - s->rtc_clock_at_save;

    /* Host clocks advance while paused and across process restarts. QTest's
     * clock also lives outside the CPU timers VMState. Rebase calendar and
     * coherent read anchors to the restored instant; RTC post_load already
     * schedules alarm/wakeup timers from their saved remaining intervals. */
    rtc->rtc_base_vclock_ns += delta;
    if (rtc->shadow_locked) {
        rtc->shadow_vclock_ns += delta;
    }
    return 0;
}

static const VMStateDescription vmstate_snapshot_clock = {
    .name = "gnw-h7b0-snapshot-clock",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = state_clock_pre_save,
    .post_load = state_clock_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_INT64(rtc_clock_at_save, GnwH7B0Snapshot),
        VMSTATE_END_OF_LIST()
    },
};

void gnw_h7b0_state_init(GnwH7B0Snapshot *s, GnwH7B0State *soc)
{
    s->soc = soc;
    if (s->enabled) {
        /* Register after the SoC so RTC has loaded before anchor rebasing. */
        vmstate_register(NULL, 0, &vmstate_snapshot_clock, s);
    }
}

const char *gnw_h7b0_state_support(GnwH7B0Snapshot *s)
{
    if (!s->enabled) {
        return "disabled";
    }
    if (!s->soc) {
        return "starting";
    }
    if (!s->id || !*s->id) {
        return "state-id";
    }
    if (drive_get(IF_SD, 0, 0)) {
        return "sd";
    }
    if (s->soc->extflash_size > 64 * MiB) {
        return "flash-size";
    }
    GnwH7B0State *soc = s->soc;
    /* -global may enable a child independently of the SoC's timing flags. */
    if (soc->timing_mode || soc->x_modeled_timing ||
        soc->adc.timing_mode || soc->flash_r.timing_mode ||
        soc->flash_r.test_erase_timing || soc->flash_r.conservative_erase_timing ||
        soc->dma.timing_mode || soc->dwt.timing_mode ||
        soc->octospi1.timing_mode || soc->octospi2.timing_mode ||
        soc->mdma.timing_mode || soc->jpeg.timing_mode ||
        soc->tim1.timing_mode || soc->tim2.timing_mode || soc->dma2d.timing_mode) {
        return "timing";
    }
    return "";
}

static void digest(const void *data, size_t size, uint8_t out[32])
{
    GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256);
    size_t length = 32;
    g_checksum_update(sum, data, size);
    g_checksum_get_digest(sum, out, &length);
    g_checksum_free(sum);
}

static void put_digest(QEMUFile *f, const void *data, size_t size)
{
    uint8_t sha[32];
    digest(data, size, sha);
    qemu_put_buffer(f, sha, sizeof(sha));
}

/* Closing a QEMUFile closes its channel; a buffer channel frees its data.
 * Transfer ownership while closing, then retain the completed bytes for the
 * validated envelope. No second copy of a potentially large Flash payload. */
static int finish_buffer(QEMUFile *f, QIOChannelBuffer *buf)
{
    qemu_fflush(f);
    uint8_t *data = buf->data;
    size_t usage = buf->usage, capacity = buf->capacity;
    buf->data = NULL;
    int ret = qemu_fclose(f);
    buf->data = data;
    buf->usage = usage;
    buf->capacity = capacity;
    buf->offset = 0;
    return ret;
}

/* Deterministic header, no C struct padding or native-endian integers.
 * Runtime anchors/Flash contents belong to the payload; cold-start seeds and
 * RTC configuration remain identities of the session into which we restore. */
static QIOChannelBuffer *identity(GnwH7B0Snapshot *state)
{
    GnwH7B0State *s = state->soc;
    GnwH7B0RtcState *rtc = &s->rtc;
    QIOChannelBuffer *buf = qio_channel_buffer_new(IDENTITY_SIZE);
    QEMUFile *f = qemu_file_new_output(QIO_CHANNEL(buf));
    QemuOpts *opts = qemu_find_opts_singleton("rtc");
    const char *base = opts ? qemu_opt_get(opts, "base") : NULL;
    const char *drift = opts ? qemu_opt_get(opts, "driftfix") : NULL;
    g_autofree char *policy = g_strdup_printf("base=%s;driftfix=%s",
                                             base ? base : "utc",
                                             drift ? drift : "none");

    qemu_put_be64(f, STATE_MAGIC);
    qemu_put_be32(f, STATE_VERSION);
    qemu_put_be32(f, HEADER_SIZE);
    put_digest(f, QEMU_FULL_VERSION, strlen(QEMU_FULL_VERSION));
    put_digest(f, state->id, strlen(state->id));
    qemu_put_be64(f, memory_region_size(&s->flash_bank1));
    qemu_put_be64(f, memory_region_size(&s->flash_bank2));
    qemu_put_be64(f, s->extflash_size);
    qemu_put_buffer(f, memory_region_get_ram_ptr(&s->uid), 12);
    qemu_put_be32(f, rtc->use_rtc_clock | rtc->preserve_calendar << 1 |
                    rtc->sync_host << 2);
    qemu_put_be32(f, rtc->use_rtc_clock ? rtc_clock : QEMU_CLOCK_VIRTUAL);
    qemu_put_be64(f, rtc->initial_epoch);
    put_digest(f, policy, strlen(policy));
    /* This immutable decoder baseline is never replaced by guest Flash. */
    put_digest(f, s->extflash_unlock_baseline, GNW_UNLOCK_BASELINE_SIZE);
    put_digest(f, s->cold_itcm_seed, memory_region_size(&s->itcm));
    finish_buffer(f, buf);
    assert(buf->usage == IDENTITY_SIZE);
    return buf;
}

static bool sync_flash(MemoryRegion *mr, const char *path, Error **errp)
{
    int fd;
    if (!path || !*path) {
        return true;
    }
#ifdef _WIN32
    /* init_ram_ptr has no QEMU fd. Flush the existing view, then its file;
     * never replace the pathname and detach the live mapped inode. */
    if (!FlushViewOfFile(memory_region_get_ram_ptr(mr), memory_region_size(mr))) {
        error_setg_win32(errp, GetLastError(), "Could not sync restored Flash");
        return false;
    }
    fd = qemu_open_old(path, O_RDWR | O_BINARY);
#else
    fd = memory_region_get_fd(mr);
    if (qemu_msync(memory_region_get_ram_ptr(mr), memory_region_size(mr), fd)) {
        error_setg_errno(errp, errno, "Could not sync restored Flash '%s'", path);
        return false;
    }
#endif
    int ret = fd < 0 ? -1 : qemu_fdatasync(fd);
    int saved_errno = errno;
#ifdef _WIN32
    if (fd >= 0) {
        close(fd);
    }
#endif
    if (ret) {
        error_setg_errno(errp, saved_errno, "Could not sync restored Flash '%s'", path);
        return false;
    }
    return true;
}

void gnw_h7b0_state_file(GnwH7B0Snapshot *s, const char *path, bool load,
                        Error **errp)
{
    ERRP_GUARD();
    const char *unsupported = gnw_h7b0_state_support(s);
    QIOChannelFile *ioc = NULL;
    QIOChannelBuffer *payload = NULL;
    QIOChannelBuffer *expected = NULL;
    QEMUFile *f = NULL;
    int ret = 0;

    if (*unsupported) {
        error_setg(errp, "Instant states unsupported: %s", unsupported);
        return;
    }
    if (!g_path_is_absolute(path)) {
        error_setg(errp, "State path must be absolute");
        return;
    }
    if (runstate_is_running()) {
        error_setg(errp, "Pause the VM before saving or loading state");
        return;
    }
    expected = identity(s);
    if (load) {
        struct stat st;
        uint8_t header[IDENTITY_SIZE], checksum[32], actual[32];
        uint64_t size;

        ioc = qio_channel_file_new_path(path, O_RDONLY | O_BINARY, 0, errp);
        if (!ioc) {
            goto out;
        }
        if (fstat(ioc->fd, &st) || !S_ISREG(st.st_mode) ||
            st.st_size < HEADER_SIZE || st.st_size > HEADER_SIZE + MAX_PAYLOAD) {
            error_setg(errp, "Invalid snapshot file length or type");
            goto out;
        }
        f = qemu_file_new_input(QIO_CHANNEL(ioc));
        if (qemu_get_buffer(f, header, sizeof(header)) != sizeof(header) ||
            memcmp(header, expected->data, sizeof(header))) {
            error_setg(errp, "Incompatible state identity or configuration");
            goto out;
        }
        size = qemu_get_be64(f);
        if (!size || size != st.st_size - HEADER_SIZE) {
            error_setg(errp, "Invalid snapshot payload length");
            goto out;
        }
        qemu_get_buffer(f, checksum, sizeof(checksum));
        payload = qio_channel_buffer_new(size);
        if (qemu_get_buffer(f, payload->data, size) != size || qemu_file_get_error(f)) {
            error_setg(errp, "Truncated snapshot payload");
            goto out;
        }
        payload->usage = size;
        digest(payload->data, size, actual);
        if (memcmp(checksum, actual, sizeof(actual))) {
            error_setg(errp, "Invalid snapshot payload checksum");
            goto out;
        }
        ret = qemu_fclose(f);
        f = NULL;
        if (ret) {
            error_setg_errno(errp, -ret, "State file I/O failed");
            goto out;
        }
        /* Validate everything above before touching VM state. Reading once
         * into memory also prevents a concurrent replacement changing what
         * we restore after checksum validation. */
        if (!yank_register_instance(MIGRATION_YANK_INSTANCE, errp)) {
            goto out;
        }
        vm_stop(RUN_STATE_RESTORE_VM);
        qemu_system_reset(SHUTDOWN_CAUSE_SNAPSHOT_LOAD);
        f = qemu_file_new_input(QIO_CHANNEL(payload));
        ret = qemu_loadvm_state(f, errp);
        migration_incoming_state_destroy();
        vm_stop(RUN_STATE_PAUSED);
        if (!ret &&
            (!sync_flash(&s->soc->flash_bank1, s->soc->bank1_image, errp) ||
             !sync_flash(&s->soc->flash_bank2, s->soc->bank2_image, errp) ||
             !sync_flash(&s->soc->extflash, s->soc->extflash_image, errp) ||
             !gnw_h7b0_flash_r_flush_rdp(&s->soc->flash_r, errp))) {
            ret = -EIO;
        }
    } else {
        payload = qio_channel_buffer_new(MiB);
        f = qemu_file_new_output(QIO_CHANNEL(payload));
        global_state_store();
        ret = qemu_savevm_state(f, errp);
        int closed = finish_buffer(f, payload);
        f = NULL;
        if (ret || closed) {
            ret = ret ? ret : closed;
            goto out;
        }
        ioc = qio_channel_file_new_path(path, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY,
                                       0600, errp);
        if (!ioc) {
            goto out;
        }
        f = qemu_file_new_output(QIO_CHANNEL(ioc));
        qemu_put_buffer(f, expected->data, expected->usage);
        qemu_put_be64(f, payload->usage);
        put_digest(f, payload->data, payload->usage);
        qemu_put_buffer(f, payload->data, payload->usage);
        qemu_fflush(f);
        ret = qemu_file_get_error(f);
        if (!ret && qemu_fdatasync(ioc->fd)) {
            ret = -errno;
        }
    }
out:
    if (f) {
        int closed = qemu_fclose(f);
        if (!ret) {
            ret = closed;
        }
    }
    if (ret && !*errp) {
        error_setg_errno(errp, ret < 0 ? -ret : EIO, "State operation failed");
    }
    if (ioc) {
        object_unref(OBJECT(ioc));
    }
    if (payload) {
        object_unref(OBJECT(payload));
    }
    object_unref(OBJECT(expected));
}
