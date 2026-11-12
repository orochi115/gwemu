/* Adapted from orochi115/gnw-emu qemu-gnw/hw/arm/gnw/gnw_display.c.
 * Local RGBA display listener. Only one frame may be unacknowledged.
 * Socket writes never block the emulator and own snapshots, not live surfaces.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/cutils.h"
#include "qemu/notify.h"
#include "qemu/timer.h"
#include "system/system.h"
#include "io/channel-socket.h"
#include "ui/console.h"
#include "gnw_h7b0_desktop.h"
#include "gnw_h7b0_frame_protocol.h"

typedef struct {
    DisplayChangeListener dcl;
    QIOChannelSocket *socket;
    DisplaySurface *surface;
    Notifier exit_notify;
    guint read_watch, write_watch;
    uint8_t control[GNW_FRAME_HEADER];
    size_t control_used, offset;
    GByteArray *sending, *pending;
    uint64_t sequence, awaited;
    uint32_t maximum;
    bool ready, waiting;
} GnwDisplay;

static void display_disconnect(GnwDisplay *s)
{
    if (s->read_watch) { g_source_remove(s->read_watch); s->read_watch = 0; }
    if (s->write_watch) { g_source_remove(s->write_watch); s->write_watch = 0; }
    if (s->socket) {
        qio_channel_close(QIO_CHANNEL(s->socket), NULL);
        object_unref(OBJECT(s->socket)); s->socket = NULL;
    }
    g_clear_pointer(&s->sending, g_byte_array_unref);
    g_clear_pointer(&s->pending, g_byte_array_unref);
}

static bool display_drain(GnwDisplay *s);
static gboolean display_writable(QIOChannel *ioc, GIOCondition condition, gpointer opaque)
{
    GnwDisplay *s = opaque;
    s->write_watch = 0;
    display_drain(s);
    return G_SOURCE_REMOVE;
}

static bool display_drain(GnwDisplay *s)
{
    Error *err = NULL;
    while (s->socket && s->sending && s->offset < s->sending->len) {
        ssize_t n = qio_channel_write(QIO_CHANNEL(s->socket),
            (char *)s->sending->data + s->offset, s->sending->len - s->offset, &err);
        if (n == QIO_CHANNEL_ERR_BLOCK) {
            if (!s->write_watch) {
                s->write_watch = qio_channel_add_watch(QIO_CHANNEL(s->socket),
                    G_IO_OUT, display_writable, s, NULL);
            }
            return true;
        }
        if (n <= 0) {
            if (err) { error_report_err(err); }
            display_disconnect(s); return false;
        }
        s->offset += n;
    }
    g_clear_pointer(&s->sending, g_byte_array_unref);
    s->offset = 0;
    return s->socket != NULL;
}

static void display_send_pending(GnwDisplay *s)
{
    if (!s->socket || !s->ready || s->waiting || s->sending || !s->pending) { return; }
    s->sending = g_steal_pointer(&s->pending);
    s->awaited = ldq_le_p(s->sending->data + 16);
    s->waiting = true;
    display_drain(s);
}

static gboolean display_readable(QIOChannel *ioc, GIOCondition condition, gpointer opaque)
{
    GnwDisplay *s = opaque;
    Error *err = NULL;
    for (;;) {
        ssize_t n = qio_channel_read(ioc, (char *)s->control + s->control_used,
                                     GNW_FRAME_HEADER - s->control_used, &err);
        if (n == QIO_CHANNEL_ERR_BLOCK) { return G_SOURCE_CONTINUE; }
        if (n <= 0) { break; }
        s->control_used += n;
        if (s->control_used == GNW_FRAME_HEADER) {
            uint8_t *p = s->control;
            uint16_t type = lduw_le_p(p + 6);
            bool valid = !memcmp(p, "GNWF", 4) && lduw_le_p(p + 4) == 1 &&
                ldl_le_p(p + 8) == 64 && !ldl_le_p(p + 12) && !ldq_le_p(p + 24) &&
                !ldl_le_p(p + 32) && !ldl_le_p(p + 36) && !ldl_le_p(p + 40) &&
                !ldl_le_p(p + 52) && !ldq_le_p(p + 56);
            if (valid && type == GNW_HELLO_ACK && !s->ready) {
                uint32_t maximum = ldl_le_p(p + 48);
                valid = !ldq_le_p(p + 16) && ldl_le_p(p + 44) == 1 &&
                    maximum && maximum <= GNW_FRAME_MAX;
                if (valid) { s->maximum = maximum; s->ready = true; }
            } else if (valid && type == GNW_FRAME_ACK && s->ready && s->waiting) {
                valid = !s->sending && ldq_le_p(p + 16) == s->awaited &&
                    !ldl_le_p(p + 44) && !ldl_le_p(p + 48);
                if (valid) { s->waiting = false; }
            } else { valid = false; }
            if (!valid) { error_report("gnw-display: invalid control/ack"); break; }
            s->control_used = 0;
            display_send_pending(s);
        }
    }
    if (err) { error_report_err(err); }
    s->read_watch = 0;
    display_disconnect(s);
    return G_SOURCE_REMOVE;
}

static void display_switch(DisplayChangeListener *dcl, DisplaySurface *surface)
{
    GnwDisplay *s = container_of(dcl, GnwDisplay, dcl);
    s->surface = surface;
}

static void display_refresh(DisplayChangeListener *dcl)
{
    GnwDisplay *s = container_of(dcl, GnwDisplay, dcl);
    DisplaySurface *surface;
    uint32_t w, h, bytes;
    uint8_t *p;
    if (!s->socket || !s->ready) { return; }
    graphic_hw_update(dcl->con);
    surface = s->surface;
    if (!surface || surface_format(surface) != PIXMAN_x8r8g8b8) { return; }
    w = surface_width(surface); h = surface_height(surface);
    if (!w || !h || w > 2048 || h > 2048) { return; }
    bytes = w * h * 4;
    if (bytes > s->maximum) { display_disconnect(s); return; }
    g_clear_pointer(&s->pending, g_byte_array_unref);
    s->pending = g_byte_array_sized_new(GNW_FRAME_HEADER + bytes);
    g_byte_array_set_size(s->pending, GNW_FRAME_HEADER + bytes);
    p = s->pending->data;
    gnw_frame_header(p, GNW_FRAME); stl_le_p(p + 12, bytes);
    stq_le_p(p + 16, ++s->sequence);
    stq_le_p(p + 24, (uint64_t)g_get_monotonic_time() * 1000);
    stl_le_p(p + 32, w); stl_le_p(p + 36, h);
    stl_le_p(p + 40, w * 4); stl_le_p(p + 44, 1);
    p += GNW_FRAME_HEADER;
    /* PIXMAN_x8r8g8b8 is native 0x00RRGGBB; convert to defined byte order. */
    for (uint32_t y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)(surface_data(surface) + y * surface_stride(surface));
        for (uint32_t x = 0; x < w; x++, p += 4) {
            uint32_t color = row[x];
            p[0] = color >> 16; p[1] = color >> 8; p[2] = color; p[3] = 255;
        }
    }
    display_send_pending(s);
}

static const DisplayChangeListenerOps display_ops = {
    .dpy_name = "gnw-frame-tcp", .dpy_refresh = display_refresh,
    .dpy_gfx_switch = display_switch,
};

static void display_exit(Notifier *notifier, void *unused)
{
    GnwDisplay *s = container_of(notifier, GnwDisplay, exit_notify);
    display_disconnect(s);
    unregister_displaychangelistener(&s->dcl);
    g_free(s);
}

void gnw_h7b0_desktop_start(const char *endpoint, Error **errp)
{
    GnwDisplay *s;
    SocketAddress *address;
    const char *port;
    unsigned int number = 0;
    if (!endpoint) { return; }
    if (!g_str_has_prefix(endpoint, "127.0.0.1:")) {
        error_setg(errp, "gnw-display: endpoint must be 127.0.0.1:PORT"); return;
    }
    /* socket_parse accepts options such as addr= that override the host. */
    port = endpoint + strlen("127.0.0.1:");
    if (!*port) {
        error_setg(errp, "gnw-display: endpoint must be 127.0.0.1:PORT"); return;
    }
    for (; *port; port++) {
        if (*port < '0' || *port > '9' || number > 6553) {
            error_setg(errp, "gnw-display: endpoint must be 127.0.0.1:PORT"); return;
        }
        number = number * 10 + (*port - '0');
        if (number > 65535) {
            error_setg(errp, "gnw-display: endpoint must be 127.0.0.1:PORT"); return;
        }
    }
    if (!number) {
        error_setg(errp, "gnw-display: endpoint must be 127.0.0.1:PORT"); return;
    }
    address = socket_parse(endpoint, errp);
    if (!address) { return; }
    s = g_new0(GnwDisplay, 1);
    s->socket = qio_channel_socket_new();
    if (qio_channel_socket_connect_sync(s->socket, address, errp) < 0) {
        qapi_free_SocketAddress(address); display_disconnect(s); g_free(s); return;
    }
    qapi_free_SocketAddress(address);
    qio_channel_set_blocking(QIO_CHANNEL(s->socket), false, NULL);
    qio_channel_set_delay(QIO_CHANNEL(s->socket), false);
    s->read_watch = qio_channel_add_watch(QIO_CHANNEL(s->socket), G_IO_IN,
                                         display_readable, s, NULL);
    s->sending = g_byte_array_sized_new(64); g_byte_array_set_size(s->sending, 64);
    gnw_frame_header(s->sending->data, GNW_HELLO);
    stl_le_p(s->sending->data + 44, 1); stl_le_p(s->sending->data + 48, GNW_FRAME_MAX);
    display_drain(s);
    s->dcl.con = qemu_console_lookup_by_index(0);
    s->dcl.ops = &display_ops;
    register_displaychangelistener(&s->dcl);
    update_displaychangelistener(&s->dcl, 16);
    s->exit_notify.notify = display_exit;
    qemu_add_exit_notifier(&s->exit_notify);
}

/* Adapted from gnw-emu gnw_session_lock.c. The child owns the lock. */
int gnw_h7b0_desktop_lock(const char *path, Error **errp)
{
    int fd;
    if (!path) { return -1; }
    fd = open(path, O_RDWR | O_CREAT | O_BINARY, 0600);
    if (fd < 0) { error_setg_errno(errp, errno, "gnw: cannot open session lock"); return -1; }
#ifdef _WIN32
    OVERLAPPED ov = { 0 };
    if (!LockFileEx((HANDLE)_get_osfhandle(fd), LOCKFILE_EXCLUSIVE_LOCK |
                    LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov)) {
        error_setg(errp, "gnw: session lock occupied (Windows error %lu)", GetLastError());
        close(fd); return -1;
    }
#else
    if (qemu_lock_fd(fd, 0, 1, true) < 0) {
        error_setg(errp, "gnw: session lock occupied"); close(fd); return -1;
    }
#endif
    return fd;
}
