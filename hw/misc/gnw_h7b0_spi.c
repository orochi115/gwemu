/*
 * STM32H7B0 SPI minimal stub (Nintendo Game & Watch)
 *
 * See gnw_h7b0_spi.h for scope/rationale.
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
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/gnw_h7b0_spi.h"
#include "hw/misc/gnw_h7b0_regs_spi.h"
#include "hw/misc/gnw_h7b0_stub_log.h"
#include "hw/sd/sd.h"
#include "system/blockdev.h"
#include "exec/cpu-common.h"
#include "hw/core/irq.h"
#include "qemu/timer.h"
#include "hw/misc/gnw_env.h"

#define GNW_H7B0_SPI_DMA_PEND_RX 0x1
#define GNW_H7B0_SPI_DMA_PEND_TX 0x2

static void gnw_h7b0_spi_update_irq(GnwH7B0SpiState *s)
{
    uint32_t sr = s->regs[GNW_H7B0_SPI_SR >> 2];
    uint32_t ier = s->regs[GNW_H7B0_SPI_IER >> 2];

    qemu_set_irq(s->irq, !!(sr & ier & GNW_H7B0_SPI_IRQ_MASK));
}

static void gnw_h7b0_spi_update_rx_level(GnwH7B0SpiState *s)
{
    uint32_t level = (s->rx_count & 3U) << 13;
    if (s->rx_count >= 4) {
        level |= 1U << 15; /* RXWNE: at least one complete word available. */
    }
    if (s->rx_count) {
        level |= SPI_SR_RXP;
    }
    s->regs[GNW_H7B0_SPI_SR >> 2] =
        (s->regs[GNW_H7B0_SPI_SR >> 2] &
         ~(SPI_SR_RXP | SPI_SR_RXLEVEL_MASK)) | level;
}

static void gnw_h7b0_spi_receive_byte(GnwH7B0SpiState *s, uint8_t byte)
{
    /* Preserve unread data on overflow. Full electrical OVR/FIFO-threshold
     * behaviour is outside this byte-oriented functional model. */
    if (s->rx_count == GNW_H7B0_SPI_RX_FIFO_SIZE) {
        return;
    }
    s->rx_fifo[(s->rx_head + s->rx_count) % GNW_H7B0_SPI_RX_FIFO_SIZE] = byte;
    s->rx_count++;
    s->regs[GNW_H7B0_SPI_RXDR >> 2] = byte;
    gnw_h7b0_spi_update_rx_level(s);
}

/*
 * TXDR completion used to be deferred by a small QEMUTimer-paced delay
 * here, meant to fix retro-go-sd's user_diskio_spi.c busy-wait loops
 * (SD_ReadyWait()/SD_RxDataBlock()/SD_SendCmd()) spinning through far
 * more iterations per wall-clock timeout window than real hardware could
 * (confirmed via live profiling: ~23,000 SPI1 register writes/sec during
 * ordinary SD-card gameplay reads, since instant completion meant each
 * "byte" cost only a few emulated instructions). That approach caused a
 * real regression -- a full guest hang, confirmed via live PC-sampling
 * (frozen in HAL_Delay, forward progress only resumed once completion
 * was reverted to synchronous) -- almost certainly because retro-go-sd's
 * poll loop here is tight enough that TCG never yields back to QEMU's
 * main loop to service the pending completion timer, so the guest spins
 * forever waiting on a flag that can only ever be set from a timer
 * callback it's preventing from running. Reverted to synchronous
 * completion (matching this file's original, working behavior) --
 * correctness/not-hanging trumps the CPU-usage optimization. If revisited,
 * needs a pacing strategy that doesn't require the guest CPU to yield
 * control on every single byte (e.g. block-level pacing instead of
 * per-byte, or a mechanism proven not to starve under a tight poll loop).
 */
/*
 * DMA path (SPI1 only, the SD card SPI).
 *
 * retro-go-sd reads every 512-byte SD data block with
 * HAL_SPI_TransmitReceive_DMA() and busy-waits for HAL_SPI_STATE_READY,
 * falling back to the polled path above only after a 100ms timeout plus
 * an abort. With no DMA modelling at all that timeout was paid on every
 * single sector (~0.5 reads/sec, nothing booted).
 *
 * The DMA controller here never moves peripheral data itself -- each
 * consumer pulls from M0AR/NDTR in its own stream notifier (same as
 * gnw_h7b0_sai1.c and gnw_h7b0_hash.c). So the RX notifier below does
 * the real SSI byte exchange for both directions, sourcing the outgoing
 * bytes from the TX stream's buffer; the TX notifier exists only so the
 * TX stream gets the same request-activity gating and its TCIF is
 * latched (HAL_DMA_IRQHandler needs it to return hdmatx to READY).
 */
static bool gnw_h7b0_spi_dma_request_active(GnwH7B0SpiState *s,
                                             uint32_t dmaen_bit)
{
    uint32_t cr1 = s->regs[GNW_H7B0_SPI_CR1 >> 2];
    uint32_t cfg1 = s->regs[GNW_H7B0_SPI_CFG1 >> 2];

    /*
     * Real hardware only clocks a DMA-fed transfer once the master has
     * been started. HAL enables both DMA streams several register writes
     * *before* it sets CR1.CSTART, so without this the transfer would
     * run at stream-enable time and retire out of order with HAL's own
     * setup sequence.
     */
    return (cr1 & SPI_CR1_SPE) && (cr1 & SPI_CR1_CSTART) && (cfg1 & dmaen_bit);
}

static bool gnw_h7b0_spi_dma_rx_active(void *opaque)
{
    return gnw_h7b0_spi_dma_request_active(GNW_H7B0_SPI(opaque),
                                            SPI_CFG1_RXDMAEN);
}

static bool gnw_h7b0_spi_dma_tx_active(void *opaque)
{
    return gnw_h7b0_spi_dma_request_active(GNW_H7B0_SPI(opaque),
                                            SPI_CFG1_TXDMAEN);
}

/* Retire one stream's contribution to the in-flight transfer; EOT is a
 * whole-transfer flag, so it waits for every stream CSTART armed. */
static void gnw_h7b0_spi_dma_stream_done(GnwH7B0SpiState *s, uint8_t which)
{
    s->dma_pending &= ~which;
    if (s->dma_pending) {
        return;
    }

    /*
     * Hardware clears CSTART once CR2.TSIZE items have moved, and sets
     * EOT/TXTF. HAL only enables IER.EOTIE from the RX stream's
     * transfer-complete callback, i.e. after this point -- the interrupt
     * line is level-driven off SR & IER (see the IER write handler), so
     * latching EOT here and letting the later IER write raise the line
     * matches the hardware ordering.
     */
    s->regs[GNW_H7B0_SPI_CR1 >> 2] &= ~SPI_CR1_CSTART;
    s->regs[GNW_H7B0_SPI_SR >> 2] |= SPI_SR_EOT | SPI_SR_TXTF;
    gnw_h7b0_spi_update_irq(s);
}

static void gnw_h7b0_spi_dma_rx_notify(void *opaque, bool half,
                                        uint32_t m0ar, uint32_t ndtr)
{
    GnwH7B0SpiState *s = GNW_H7B0_SPI(opaque);
    /* Byte items (both stream inits use {PERIPH,MEM}DATAALIGN_BYTE).
     * Half-transfer covers the first ndtr/2 items, full-transfer the
     * rest -- the remainder goes to the full half so an odd NDTR still
     * moves every byte. */
    uint32_t first = ndtr / 2;
    uint32_t off = half ? 0 : first;
    uint32_t len = half ? first : ndtr - first;
    uint32_t tx_m0ar, tx_ndtr;
    uint32_t moved = len;
    bool have_tx = false;

    if (!s->ssi || len == 0) {
        if (!half) {
            gnw_h7b0_spi_dma_stream_done(s, GNW_H7B0_SPI_DMA_PEND_RX);
        }
        return;
    }

    /*
     * Outgoing bytes come from whatever buffer the TX stream is pointed
     * at. Every current firmware DMA use is an SD block read with an
     * all-0xFF fill buffer, but reading it for real keeps a DMA-driven
     * block *write* correct instead of silently sending 0xFF.
     */
    if (s->dma) {
        have_tx = gnw_h7b0_dma_get_request_stream_regs(
            s->dma, GNW_H7B0_SPI1_DMA_REQUEST_TX, &tx_m0ar, &tx_ndtr) &&
            tx_ndtr >= ndtr;
    }

    while (len) {
        uint8_t txbuf[64], rxbuf[64];
        uint32_t chunk = MIN(len, (uint32_t)sizeof(txbuf));

        if (have_tx) {
            cpu_physical_memory_read(tx_m0ar + off, txbuf, chunk);
        } else {
            memset(txbuf, 0xff, chunk);
        }
        for (uint32_t i = 0; i < chunk; i++) {
            rxbuf[i] = s->softspi ? 0xff : ssi_transfer(s->ssi, txbuf[i]) & 0xff;
        }
        cpu_physical_memory_write(m0ar + off, rxbuf, chunk);

        /* DMA already consumed the data phase. Preserve the existing final
         * byte mirror without accumulating another FIFO for each DMA chunk. */
        s->rx_head = s->rx_count = 0;
        gnw_h7b0_spi_receive_byte(s, rxbuf[chunk - 1]);

        off += chunk;
        len -= chunk;
    }

    /*
     * GNW_SPI_DMA_TRACE=1: per-second DMA throughput, for checking that
     * SD block reads really run over DMA rather than falling back to the
     * polled path. Wall-clock (QEMU_CLOCK_REALTIME) on purpose -- a
     * guest-relative rate would read fine even while the guest crawls.
     * getenv() resolved once; unset/empty/"0" all mean off.
     */
    {
        /* getenv() resolved once -- it is a locked linear scan on
         * Windows msvcrt, and this is a per-block path. */
        static int trace = -1;
        if (trace < 0) {
            trace = gnw_env_enabled("GNW_SPI_DMA_TRACE");
        }
        if (trace) {
            static int64_t t0;
            static uint64_t bytes, blocks;
            int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

            bytes += moved;
            blocks += !half;
            if (now - t0 >= 1000000000LL) {
                fprintf(stderr, "SPIDMA %.1f blocks/s %.1f KB/s\n",
                        blocks * 1e9 / (double)(now - t0),
                        bytes * 1e9 / (double)(now - t0) / 1024.0);
                t0 = now;
                bytes = blocks = 0;
            }
        }
    }

    gnw_h7b0_spi_update_irq(s);
    if (!half) {
        gnw_h7b0_spi_dma_stream_done(s, GNW_H7B0_SPI_DMA_PEND_RX);
    }
}

static void gnw_h7b0_spi_dma_tx_notify(void *opaque, bool half,
                                        uint32_t m0ar, uint32_t ndtr)
{
    /* Byte movement is done by the RX notifier above (it needs both
     * buffers at once for the SSI exchange); this only retires the TX
     * stream's half of the transfer. */
    if (!half) {
        gnw_h7b0_spi_dma_stream_done(GNW_H7B0_SPI(opaque),
                                      GNW_H7B0_SPI_DMA_PEND_TX);
    }
}

void gnw_h7b0_spi_set_dma(GnwH7B0SpiState *s, GnwH7B0DmaState *dma)
{
    s->dma = dma;

    /*
     * low_latency: a 512-byte SD block is microseconds of real transfer,
     * and firmware bounds its wait at 100ms, so the controller's
     * 1ms-per-half audio pacing floor is the wrong model here. The
     * request-activity predicates keep the low_latency streams from
     * completing inline at stream-enable time (see
     * gnw_h7b0_dma_start_stream()).
     */
    gnw_h7b0_dma_set_request_notifier(dma, GNW_H7B0_SPI1_DMA_REQUEST_RX,
                                       gnw_h7b0_spi_dma_rx_notify, s,
                                       NULL, NULL, true);
    gnw_h7b0_dma_set_request_active_fn(dma, GNW_H7B0_SPI1_DMA_REQUEST_RX,
                                        gnw_h7b0_spi_dma_rx_active, s);
    gnw_h7b0_dma_set_request_notifier(dma, GNW_H7B0_SPI1_DMA_REQUEST_TX,
                                       gnw_h7b0_spi_dma_tx_notify, s,
                                       NULL, NULL, true);
    gnw_h7b0_dma_set_request_active_fn(dma, GNW_H7B0_SPI1_DMA_REQUEST_TX,
                                        gnw_h7b0_spi_dma_tx_active, s);
}

static void gnw_h7b0_spi_reset(DeviceState *dev)
{
    GnwH7B0SpiState *s = GNW_H7B0_SPI(dev);

    for (int i = 0; i < (GNW_H7B0_SPI_SIZE / 4); i++) {
        s->regs[i] = get_spi_reset_value(i * 4);
    }
    memset(s->logged_unimp, 0, sizeof(s->logged_unimp));
    s->dma_pending = 0;
    s->rx_head = s->rx_count = 0;
    memset(s->rx_fifo, 0, sizeof(s->rx_fifo));
    /*
     * TXP ("Tx-Packet space available") reflects TxFIFO occupancy on
     * real hardware, which is trivially available even before the
     * peripheral is enabled -- it is NOT gated on CR1.SPE. Found via
     * retro-go-sd's bare-metal user_diskio_spi.c: it polls SR.TXP
     * *before* HAL_SPI_Transmit() (the actual SPE-enabling call) ever
     * runs, so a TXP-only-set-after-SPE model left it spinning
     * forever on the very first byte. Set once at reset and never
     * cleared -- see the CR1 handler below.
     */
    s->regs[GNW_H7B0_SPI_SR >> 2] = SPI_SR_TXP;
    gnw_h7b0_spi_update_irq(s);
}

static uint64_t gnw_h7b0_spi_read(void *opaque, hwaddr addr, unsigned int size)
{
    GnwH7B0SpiState *s = GNW_H7B0_SPI(opaque);

    if (addr >= GNW_H7B0_SPI_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return 0;
    }
    uint32_t value = s->regs[addr >> 2];
    if (addr == GNW_H7B0_SPI_RXDR) {
        if (s->rx_count) {
            value = 0;
            for (unsigned i = 0; i < size && s->rx_count; i++) {
                value |= (uint32_t)s->rx_fifo[s->rx_head] << (i * 8);
                s->rx_head = (s->rx_head + 1) % GNW_H7B0_SPI_RX_FIFO_SIZE;
                s->rx_count--;
            }
        }
        gnw_h7b0_spi_update_rx_level(s);
        gnw_h7b0_spi_update_irq(s);
    }
    return value;
}

static void gnw_h7b0_spi_write(void *opaque, hwaddr addr,
                                uint64_t val64, unsigned int size)
{
    GnwH7B0SpiState *s = GNW_H7B0_SPI(opaque);
    uint32_t value = val64;

    if (addr >= GNW_H7B0_SPI_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return;
    }

    uint32_t mask = get_spi_write_mask(addr);

    /*
     * The SVD-derived CR1 mask (0xfd01) leaves CSTART (bit 9) out, so
     * every CR1.CSTART write was silently dropped -- the bit read back
     * as 0 and the DMA path below never saw a transfer start at all
     * (confirmed live: right after HAL_SPI_TransmitReceive_DMA() returned
     * HAL_OK with CFG1 = 0x1007c007, i.e. both DMAENs set, CR1 read back
     * 0x1001 with no CSTART). Firmware then ate its full 100ms
     * HAL_SPI_GetState() timeout per 512-byte SD block and fell back to
     * the polled path. CSTART is a real writable start bit on hardware
     * (RM0455 SPI CR1) -- allow it through.
     */
    if (addr == GNW_H7B0_SPI_CR1) {
        mask |= SPI_CR1_CSTART;
    }

    value = (s->regs[addr >> 2] & ~mask) | (value & mask);

    switch (addr) {
    case GNW_H7B0_SPI_CR1: {
        uint32_t prev = s->regs[addr >> 2];

        s->regs[addr >> 2] = value;
        if (!(value & SPI_CR1_SPE)) {
            /*
             * TXP is left alone (see reset's comment -- it's not
             * SPE-gated on real hardware); EOT and RXP are
             * disable-gated, matching real hardware clearing status
             * on disable.
             */
            s->regs[GNW_H7B0_SPI_SR >> 2] &=
                ~(SPI_SR_EOT | SPI_SR_RXP | SPI_SR_RXLEVEL_MASK);
            s->rx_head = s->rx_count = 0;
            /* HAL_SPI_Abort() disables the peripheral; drop any transfer
             * still armed so a later one doesn't inherit its state. */
            s->dma_pending = 0;
        } else if ((value & SPI_CR1_CSTART) && !(prev & SPI_CR1_CSTART)) {
            /* Arm the DMA-driven transfer: record which streams have to
             * report in before EOT. Nothing to do for a polled transfer
             * (neither DMAEN set) -- the TXDR path handles those. */
            uint32_t cfg1 = s->regs[GNW_H7B0_SPI_CFG1 >> 2];

            s->dma_pending =
                ((cfg1 & SPI_CFG1_RXDMAEN) ? GNW_H7B0_SPI_DMA_PEND_RX : 0) |
                ((cfg1 & SPI_CFG1_TXDMAEN) ? GNW_H7B0_SPI_DMA_PEND_TX : 0);

            /*
             * Run the transfer now rather than waiting for the DMA
             * controller's stall poll to notice the request went active
             * -- that poll interval was the whole per-block cost.
             * RX first: it sources the outgoing bytes from the TX
             * stream's registers, which are only readable while that
             * stream is still enabled.
             */
            if (s->dma) {
                gnw_h7b0_dma_kick_request(s->dma, GNW_H7B0_SPI1_DMA_REQUEST_RX);
                gnw_h7b0_dma_kick_request(s->dma, GNW_H7B0_SPI1_DMA_REQUEST_TX);
            }
        }
        gnw_h7b0_spi_update_irq(s);
        return;
    }
    case GNW_H7B0_SPI_IER:
        /*
         * Level-driven line: HAL enables EOTIE only after the DMA
         * transfer-complete callback has run, by which point SR.EOT is
         * already latched, so the line has to be re-evaluated here
         * rather than only when SR changes.
         */
        s->regs[addr >> 2] = value;
        gnw_h7b0_spi_update_irq(s);
        return;
    case GNW_H7B0_SPI_CR2:
    case GNW_H7B0_SPI_CFG1:
        /* Plain shadow, but read back by the DMA path above (TSIZE,
         * TXDMAEN/RXDMAEN) -- not an unimplemented register. */
        s->regs[addr >> 2] = value;
        return;
    case GNW_H7B0_SPI_TXDR:
        /*
         * Real hardware only sets EOT once the whole configured
         * transfer finishes; approximated here as "every write instantly
         * completes the transfer" since nothing currently tracks a
         * configured byte count -- see this file's top-of-file comment
         * for why a deferred (paced) completion was tried and reverted.
         */
        s->regs[addr >> 2] = value;
        if (s->ssi) {
            /* Real SSI byte exchange with the attached virtual SD card. */
            uint32_t rx = s->softspi ? 0xff : ssi_transfer(s->ssi, value & 0xff);
            /*
             * Receive occupancy reports both RXP and RXPLVL.
             * STM32H7 HAL variants poll either RXP or RXWNE/FRLVL before
             * reading the byte; RXWNE stays clear with less than a word.
             */
            gnw_h7b0_spi_receive_byte(s, rx);
        }
        s->regs[GNW_H7B0_SPI_SR >> 2] |= SPI_SR_EOT;
        /*
         * Hardware clears CSTART when the transfer ends, and this path
         * models a transfer that ends immediately. Leaving it set was a
         * real bug once the DMA path started gating on CSTART: a polled
         * transfer left CSTART stuck at 1 forever, so the next DMA
         * transfer's streams looked "already started" the moment
         * HAL_DMA_Start_IT() enabled them -- the TX stream then completed
         * before CSTART was written, its completion was overwritten when
         * the CSTART write armed dma_pending, and EOT never fired again.
         */
        s->regs[GNW_H7B0_SPI_CR1 >> 2] &= ~SPI_CR1_CSTART;
        gnw_h7b0_spi_update_irq(s);
        return;
    case GNW_H7B0_SPI_IFCR:
        /* Write-1-to-clear: IFCR bit positions mirror SR's exactly. */
        s->regs[GNW_H7B0_SPI_SR >> 2] &= ~value;
        gnw_h7b0_spi_update_irq(s);
        return;
    case GNW_H7B0_SPI_SR:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: SR is read-only on real hardware (use IFCR to "
                      "clear flags)\n", __func__);
        return;
    default:
        gnw_h7b0_stub_log_unimp_ratelimited(s->logged_unimp, __func__, addr);
        s->regs[addr >> 2] = value;
    }
}

static const MemoryRegionOps gnw_h7b0_spi_ops = {
    .read = gnw_h7b0_spi_read,
    .write = gnw_h7b0_spi_write,
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

static void gnw_h7b0_spi_init(Object *obj)
{
    GnwH7B0SpiState *s = GNW_H7B0_SPI(obj);

    memory_region_init_io(&s->mmio, obj, &gnw_h7b0_spi_ops, s,
                           TYPE_GNW_H7B0_SPI, GNW_H7B0_SPI_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void gnw_h7b0_spi_realize(DeviceState *dev, Error **errp)
{
    GnwH7B0SpiState *s = GNW_H7B0_SPI(dev);

    if (!s->sd_card) {
        return;
    }

    /*
     * Real SD-over-SPI card, backed by whatever -drive if=sd,... the
     * user passes (none => card model with no image => reads as
     * "ejected", same as before this device existed). See
     * gnw_h7b0_spi.h for why chip-select isn't wired up.
     */
    s->ssi = ssi_create_bus(dev, "ssi");
    DeviceState *ssi_sd_dev = ssi_create_peripheral(s->ssi, "ssi-sd");

    DriveInfo *dinfo = drive_get(IF_SD, 0, 0);
    BlockBackend *blk = dinfo ? blk_by_legacy_dinfo(dinfo) : NULL;
    DeviceState *card_dev = qdev_new(TYPE_SD_CARD_SPI);
    qdev_prop_set_drive_err(card_dev, "drive", blk, &error_fatal);
    qdev_realize_and_unref(card_dev, qdev_get_child_bus(ssi_sd_dev, "sd-bus"),
                            &error_fatal);
}

static int gnw_h7b0_spi_post_load(void *opaque, int version_id)
{
    GnwH7B0SpiState *s = GNW_H7B0_SPI(opaque);
    if (version_id < 3) {
        /* Version2 had only an unread-byte mirror in RXDR/RXP. */
        s->rx_head = 0;
        s->rx_count = !!(s->regs[GNW_H7B0_SPI_SR >> 2] & SPI_SR_RXP);
        memset(s->rx_fifo, 0, sizeof(s->rx_fifo));
        s->rx_fifo[0] = s->regs[GNW_H7B0_SPI_RXDR >> 2];
    }
    if (s->rx_head >= GNW_H7B0_SPI_RX_FIFO_SIZE ||
        s->rx_count > GNW_H7B0_SPI_RX_FIFO_SIZE) {
        /* A failed load leaves the VM paused, but MMIO diagnostics are still
         * available. Do not leave an invalid cursor accessible through RXDR. */
        s->rx_head = s->rx_count = 0;
        gnw_h7b0_spi_update_rx_level(s);
        gnw_h7b0_spi_update_irq(s);
        return -EINVAL;
    }
    gnw_h7b0_spi_update_rx_level(s);
    gnw_h7b0_spi_update_irq(s);
    return 0;
}

static const VMStateDescription vmstate_gnw_h7b0_spi = {
    .name = TYPE_GNW_H7B0_SPI,
    .version_id = 3,
    .minimum_version_id = 2,
    .post_load = gnw_h7b0_spi_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, GnwH7B0SpiState, GNW_H7B0_SPI_SIZE / 4),
        VMSTATE_UINT8(dma_pending, GnwH7B0SpiState),
        VMSTATE_UINT8_ARRAY_V(rx_fifo, GnwH7B0SpiState,
                             GNW_H7B0_SPI_RX_FIFO_SIZE, 3),
        VMSTATE_UINT8_V(rx_head, GnwH7B0SpiState, 3),
        VMSTATE_UINT8_V(rx_count, GnwH7B0SpiState, 3),
        VMSTATE_END_OF_LIST()
    }
};

static const Property gnw_h7b0_spi_properties[] = {
    DEFINE_PROP_BOOL("sd-card", GnwH7B0SpiState, sd_card, false),
};

static void gnw_h7b0_spi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_gnw_h7b0_spi;
    dc->realize = gnw_h7b0_spi_realize;
    device_class_set_legacy_reset(dc, gnw_h7b0_spi_reset);
    device_class_set_props(dc, gnw_h7b0_spi_properties);
}

static const TypeInfo gnw_h7b0_spi_info = {
    .name          = TYPE_GNW_H7B0_SPI,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GnwH7B0SpiState),
    .instance_init = gnw_h7b0_spi_init,
    .class_init    = gnw_h7b0_spi_class_init,
};

static void gnw_h7b0_spi_register_types(void)
{
    type_register_static(&gnw_h7b0_spi_info);
}

type_init(gnw_h7b0_spi_register_types)
