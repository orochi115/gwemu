/*
 * STM32H7B0 OCTOSPI minimal stub (Nintendo Game & Watch)
 *
 * See gnw_h7b0_ospi.h for scope/rationale.
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
#include "qemu/units.h"
#include "qapi/visitor.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "hw/misc/gnw_h7b0_ospi.h"
#include "hw/misc/gnw_h7b0_nor.h"
#include "hw/misc/gnw_h7b0_regs_ospi.h"
#include "hw/misc/gnw_h7b0_stub_log.h"

#define OSPI_TIMING_CONFIG_ID 5U
#define OSPI_SR_FLEVEL_SHIFT 8
#define OSPI_SR_FLEVEL_MASK (0x3fU << OSPI_SR_FLEVEL_SHIFT)

/*
 * XIP window availability
 * -----------------------
 * The OCTOSPI has one set of pins. RM0455 24.4.17 ("OCTOSPI error
 * management") lists, among the conditions under which the controller
 * generates an *AXI slave error* for an access to the memory-mapped
 * window: "The memory-mapped mode is disabled and an AXI read or
 * write request occurs", "A read or write address (including the
 * address offset if ADOFFEN bit is set) exceeds the size of the
 * external memory", and "The OCTOSPI is disabled while a read or
 * write burst is requested". So on silicon the window is not a
 * passive always-readable aperture: it exists only while
 * CR.FMODE == 11 with the controller enabled, and vanishes the moment
 * firmware leaves memory-mapped mode.
 *
 * That is exactly what the documented page-program sequence does
 * (RM0455 24.4.11): abort out of memory-mapped mode, program via
 * indirect mode, auto-poll for completion, then re-enter memory-mapped
 * mode. HAL_OSPI_Abort() *is* that exit. Between it and the restore of
 * FMODE, any instruction fetch or data load from the window faults on
 * real hardware -- so firmware that runs a callback, ISR or const
 * lookup out of external flash across that gap is broken on silicon
 * even though it works fine against a model that keeps the bytes
 * readable throughout. Modeling the window as a plain RAM region made
 * that entire defect class structurally invisible; this gate makes it
 * fault here too.
 *
 * The H7B0's OCTOSPI_SR has no BERRF bit (confirmed against
 * sdk/cmsis-device-h7/Include/stm32h7b0xx.h: SR is TEF/TCF/FTF/SMF/
 * TOF/BUSY/FLEVEL only), which is RM0455's "otherwise the Slave bus
 * error response is generated at system level as transaction response
 * with no dedicated bit into the controller status register" case --
 * so a violation is a pure bus fault with no status flag to set.
 */
static bool ospi_memmap_active(GnwH7B0OspiState *s)
{
    uint32_t cr = s->regs[GNW_H7B0_OSPI_CR >> 2];

    return (cr & OSPI_CR_EN) &&
           ((cr & OSPI_CR_FMODE_MASK) >> OSPI_CR_FMODE_SHIFT)
               == OSPI_FMODE_MEMMAP;
}

static void ospi_xip_fault_log(GnwH7B0OspiState *s, hwaddr addr, bool is_write)
{
    uint32_t cr = s->regs[GNW_H7B0_OSPI_CR >> 2];

    /*
     * One-shot: a faulting fetch typically re-executes from the
     * BusFault handler, and per-access logging would drown the log.
     */
    if (s->xip_fault_logged) {
        return;
    }
    s->xip_fault_logged = true;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "%s: %s at XIP window offset 0x%" HWADDR_PRIx " while "
                  "memory-mapped mode is not active (CR=0x%08x, EN=%u, "
                  "FMODE=%u) -- real hardware answers this with an AXI "
                  "slave error (RM0455 24.4.17); raising a bus fault. "
                  "Firmware must not read or execute from the XIP window "
                  "between leaving memory-mapped mode (e.g. "
                  "HAL_OSPI_Abort()) and restoring FMODE=11.\n",
                  __func__, is_write ? "write" : "read/fetch", addr,
                  cr, !!(cr & OSPI_CR_EN),
                  (cr & OSPI_CR_FMODE_MASK) >> OSPI_CR_FMODE_SHIFT);
}

/*
 * Accesses carrying MEMTXATTRS_UNSPECIFIED are not guest AXI traffic:
 * they are host-side pokes at the address space -- `-device loader`
 * painting a flash image in at reset, the monitor's pmemsave/xp, the
 * gdbstub reading memory. Those model an external programmer or a
 * debugger touching the chip, not the CPU going through the OCTOSPI's
 * AXI port, so the window's availability doesn't apply to them; they
 * are serviced straight out of the backing store. (This also means
 * device DMA, which mostly uses unspecified attrs in QEMU, is not
 * gated -- the CPU fetch/load path is what this models, and it is the
 * one that matters for the defect class this exists to catch.)
 */
static bool ospi_xip_gate_bypass(GnwH7B0OspiState *s, MemTxAttrs attrs)
{
    return attrs.unspecified && s->backing;
}

static MemTxResult ospi_xip_gate_read(void *opaque, hwaddr addr,
                                       uint64_t *data, unsigned size,
                                       MemTxAttrs attrs)
{
    GnwH7B0OspiState *s = opaque;

    if (ospi_xip_gate_bypass(s, attrs)) {
        *data = (addr + size <= s->flash_size)
                 ? ldn_le_p(s->backing + addr, size) : 0;
        return MEMTX_OK;
    }
    ospi_xip_fault_log(s, addr, false);
    *data = 0;
    return MEMTX_ERROR;
}

static MemTxResult ospi_xip_gate_write(void *opaque, hwaddr addr,
                                        uint64_t value, unsigned size,
                                        MemTxAttrs attrs)
{
    GnwH7B0OspiState *s = opaque;

    if (ospi_xip_gate_bypass(s, attrs)) {
        if (addr + size <= s->flash_size) {
            stn_le_p(s->backing + addr, size, value);
        }
        return MEMTX_OK;
    }
    ospi_xip_fault_log(s, addr, true);
    return MEMTX_ERROR;
}

static const MemoryRegionOps ospi_xip_gate_ops = {
    .read_with_attrs = ospi_xip_gate_read,
    .write_with_attrs = ospi_xip_gate_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

/*
 * Map or unmap the faulting gate to match the controller's current
 * mode. Priority 2 puts it above both the plain extflash RAM region
 * (priority 0) and OTFDEC's decrypt overlays (priority 1) -- on real
 * hardware the OTFDEC sits downstream of the OCTOSPI, so a dead window
 * is dead through the decrypt path too.
 */
static void ospi_xip_gate_update(GnwH7B0OspiState *s)
{
    bool want_gate;

    if (!s->xip_as) {
        return;
    }
    want_gate = !ospi_memmap_active(s);
    if (want_gate == s->xip_gate_mapped) {
        return;
    }
    if (want_gate) {
        memory_region_add_subregion_overlap(s->xip_as, s->xip_base,
                                             &s->xip_gate, 2);
    } else {
        memory_region_del_subregion(s->xip_as, &s->xip_gate);
    }
    s->xip_gate_mapped = want_gate;
}

static void gnw_h7b0_ospi_reset(DeviceState *dev)
{
    GnwH7B0OspiState *s = GNW_H7B0_OSPI(dev);

    if (s->timing_timer) {
        timer_del(s->timing_timer);
    }

    for (int i = 0; i < (GNW_H7B0_OSPI_SIZE / 4); i++) {
        s->regs[i] = get_ospi_reset_value(i * 4);
    }
    memset(s->logged_unimp, 0, sizeof(s->logged_unimp));
    s->pending_instr = 0;
    s->pending_addr = 0;
    s->dr_pos = 0;
    s->wel = false;
    s->timing_active = false;
    s->timing_wire_complete = false;
    s->timing_ncs_released = false;
    s->timing_instruction_delivered = false;
    s->timing_fallback_logged = false;
    s->timing_phase = GNW_H7B0_OSPI_PHASE_IDLE;
    s->fifo_head = 0;
    s->fifo_level = 0;
    memset(s->fifo, 0, sizeof(s->fifo));
    memset(s->delivered_program, 0, sizeof(s->delivered_program));
    s->delivered_program_len = 0;
    s->timing_config_identity = s->timing_mode ? OSPI_TIMING_CONFIG_ID : 0;
    /* Peripheral reset does not stop the externally supplied RCC clock. */
    if (s->test_kernel_clock_hz) {
        s->kernel_clock_hz = s->test_kernel_clock_hz;
    }
    s->active_length = 0;
    s->active_delivered = 0;
    s->active_consumed = 0;
    s->phase_remaining_quanta = 0;
    s->phase_progress_num = 0;
    s->migrated_ncs_release_elapsed_ns = 0;
    s->phase_last_update_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->ncs_release_ns = s->phase_last_update_ns;
    s->xip_fault_logged = false;
    /*
     * CR resets with EN=0/FMODE=00, so the window comes up dead -- as
     * on real silicon, where nothing can be fetched from external
     * flash until firmware has configured memory-mapped mode.
     */
    ospi_xip_gate_update(s);
    qemu_irq_lower(s->irq);
}

/*
 * Byte generator for SFDP reads: a minimal one-parameter-header SFDP
 * table (signature + basic flash parameter table) whose only field
 * OSPI_GetFlashSizeSfdp() (gw_flash.c) actually reads is the density
 * dword at basic-table offset 4, from which it derives flash size.
 * `off` is an absolute byte offset into this synthetic SFDP address
 * space, matching how real SFDP reads are addressed (CMD_SFDP's
 * address phase carries the offset).
 */
static uint8_t ospi_sfdp_byte(GnwH7B0OspiState *s, uint32_t off)
{
    static const char sig[4] = "SFDP";
    uint32_t density_bits;

    if (off < 4) {
        return sig[off];
    }
    switch (off) {
    case 4: return 0x06; /* SFDP minor revision */
    case 5: return 0x01; /* SFDP major revision */
    case 6: return 0x00; /* NPH: 1 parameter header (0-based) */
    case 7: return 0xFF; /* unused */
    case 8: return 0x00; /* basic table JEDEC ID LSB (0xFF00) */
    case 9: return 0x06; /* basic table minor revision */
    case 10: return 0x01; /* basic table major revision */
    case 11: return 0x09; /* basic table length, in DWORDs */
    case 12: return OSPI_SFDP_TABLE_PTR & 0xFF;
    case 13: return (OSPI_SFDP_TABLE_PTR >> 8) & 0xFF;
    case 14: return (OSPI_SFDP_TABLE_PTR >> 16) & 0xFF;
    case 15: return 0xFF; /* basic table JEDEC ID MSB */
    default:
        break;
    }

    if (off >= OSPI_SFDP_TABLE_PTR && off < OSPI_SFDP_TABLE_PTR + 9 * 4) {
        uint32_t rel = off - OSPI_SFDP_TABLE_PTR;

        /* Basic Flash Parameter Table DWORD 2 = density, in bits - 1. */
        density_bits = (uint32_t)(s->flash_size * 8 - 1);
        switch (rel) {
        case 4: return density_bits & 0xFF;
        case 5: return (density_bits >> 8) & 0xFF;
        case 6: return (density_bits >> 16) & 0xFF;
        case 7: return (density_bits >> 24) & 0xFF;
        default:
            return 0;
        }
    }

    return 0;
}

static const GnwNorChip *ospi_chip(GnwH7B0OspiState *s)
{
    if (s->chip_index >= GNW_NOR_CHIP_COUNT) {
        return gnw_nor_chip_by_key(GNW_NOR_DEFAULT_CHIP_KEY);
    }
    return &gnw_nor_chips[s->chip_index];
}

static uint8_t ospi_chip_read_opcode(GnwH7B0OspiState *s)
{
    switch (ospi_chip(s)->profile) {
    case GNW_NOR_PROFILE_MX_24:
    case GNW_NOR_PROFILE_MX_32_54:
    case GNW_NOR_PROFILE_ISSI_24:
    case GNW_NOR_PROFILE_WINBOND_24:
        return 0xEB;
    case GNW_NOR_PROFILE_SPANSION_32:
    case GNW_NOR_PROFILE_MX_32:
    case GNW_NOR_PROFILE_WINBOND_32:
    default:
        return 0xEC;
    }
}

static uint8_t ospi_chip_program_opcode(GnwH7B0OspiState *s)
{
    switch (ospi_chip(s)->profile) {
    case GNW_NOR_PROFILE_MX_24: return 0x38;
    case GNW_NOR_PROFILE_MX_32: return 0x3E;
    case GNW_NOR_PROFILE_MX_32_54: return 0x38;
    case GNW_NOR_PROFILE_SPANSION_32: return 0x12;
    case GNW_NOR_PROFILE_ISSI_24: return 0x38;
    case GNW_NOR_PROFILE_WINBOND_24: return 0x32;
    case GNW_NOR_PROFILE_WINBOND_32: return 0x34;
    default: return 0;
    }
}

static uint64_t ospi_chip_erase_size(GnwH7B0OspiState *s, uint8_t instr)
{
    switch (ospi_chip(s)->profile) {
    case GNW_NOR_PROFILE_MX_24:
    case GNW_NOR_PROFILE_MX_32_54:
    case GNW_NOR_PROFILE_ISSI_24:
    case GNW_NOR_PROFILE_WINBOND_24:
        switch (instr) {
        case 0x20: return 4 * 1024;
        case 0x52: return 32 * 1024;
        case 0xD8: return 64 * 1024;
        }
        break;
    case GNW_NOR_PROFILE_MX_32:
        switch (instr) {
        case 0x21: return 4 * 1024;
        case 0x5C: return 32 * 1024;
        case 0xDC: return 64 * 1024;
        }
        break;
    case GNW_NOR_PROFILE_SPANSION_32:
        if (instr == 0xDC) return 256 * 1024;
        break;
    case GNW_NOR_PROFILE_WINBOND_32:
        if (instr == 0x21) return 4 * 1024;
        if (instr == 0xDC) return 64 * 1024;
        break;
    }
    return 0;
}

/* Byte generator for the current indirect-read command's data phase. */
static uint8_t ospi_gen_read_byte(GnwH7B0OspiState *s, uint32_t pos)
{
    switch (s->pending_instr) {
    case OSPI_CMD_RDID:
        return pos < 3 ? ospi_chip(s)->jedec[pos] : 0;
    case OSPI_CMD_RDSR:
        return (s->wel ? OSPI_FLASH_SR_WEL : 0) |
               ((ospi_chip(s)->profile == GNW_NOR_PROFILE_WINBOND_24 ||
                 ospi_chip(s)->profile == GNW_NOR_PROFILE_WINBOND_32 ||
                 ospi_chip(s)->profile == GNW_NOR_PROFILE_SPANSION_32)
                    ? 0 : OSPI_FLASH_SR_QE);
    case OSPI_CMD_RDCR:
        return ospi_chip(s)->profile == GNW_NOR_PROFILE_SPANSION_32 ? 0x02 : 0;
    case OSPI_CMD_RDAR:
        /* Spansion's init probes several volatile/nonvolatile registers. */
        return 0;
    case OSPI_CMD_RDSR2:
        return (ospi_chip(s)->profile == GNW_NOR_PROFILE_WINBOND_24 ||
                ospi_chip(s)->profile == GNW_NOR_PROFILE_WINBOND_32 ||
                ospi_chip(s)->profile == GNW_NOR_PROFILE_SPANSION_32) ? 0x02 : 0;
    case OSPI_CMD_SFDP:
        return ospi_sfdp_byte(s, s->pending_addr + pos);
    /* Stock OFW reads its authenticated settings with 0x6b (1-1-4),
     * while Retro-Go normally uses 0xeb/0xec (1-4-4). All standard
     * array-read commands share this functional byte stream. Returning
     * zero for the other bus-width variants makes valid settings appear
     * corrupt and forces PRESS TIME / 12:00 on every launch. */
    case 0x03: case 0x0B: case 0x3B: case 0x6B: case 0xBB:
    case 0x13: case 0x0C: case 0x3C: case 0x6C: case 0xBC:
    case 0xEB:
    case 0xEC: {
        uint64_t off = s->pending_addr + pos;

        if (s->backing && off < s->flash_size) {
            return s->backing[off];
        }
        return 0xFF;
    }
    default:
        qemu_log_mask(LOG_UNIMP,
                       "%s: no real data-phase model for instruction 0x%02x, "
                       "returning zero\n", __func__, s->pending_instr);
        return 0;
    }
}

/*
 * Expected CCR ADSIZE per address-phase command, following the selected
 * command profile from retro-go's gw_flash.c. SFDP and register reads use
 * 24-bit addresses; data and erase commands use the address width implied
 * by that profile. Commands without an address phase are omitted.
 */
static bool ospi_expected_adsize(GnwH7B0OspiState *s, uint8_t instr,
                                  uint32_t *adsize)
{
    switch (instr) {
    case 0x03: case 0x0B: case 0x3B: case 0x6B: case 0xBB:
    case OSPI_CMD_SFDP:
    case OSPI_CMD_RDAR:
        *adsize = OSPI_ADSIZE_24B;
        return true;
    case 0x13: case 0x0C: case 0x3C: case 0x6C: case 0xBC:
        *adsize = OSPI_ADSIZE_32B;
        return true;
    default:
        if (instr == ospi_chip_read_opcode(s) ||
            instr == ospi_chip_program_opcode(s) ||
            ospi_chip_erase_size(s, instr)) {
            switch (ospi_chip(s)->profile) {
            case GNW_NOR_PROFILE_MX_24:
            case GNW_NOR_PROFILE_ISSI_24:
            case GNW_NOR_PROFILE_WINBOND_24:
                *adsize = OSPI_ADSIZE_24B;
                break;
            default:
                *adsize = OSPI_ADSIZE_32B;
                break;
            }
            return true;
        }
        return false;
    }
}

/* Erase `size` bytes at `addr` in the XIP backing store, if wired up. */
static void ospi_backing_erase(GnwH7B0OspiState *s, uint32_t addr,
                                uint64_t size)
{
    if (!s->wel || !s->backing || !size || addr >= s->flash_size) {
        return;
    }
    addr &= ~(size - 1);
    if (size > s->flash_size - addr) {
        size = s->flash_size - addr;
    }
    memset(s->backing + addr, 0xFF, size);
}

/*
 * Side effects for the instant functional fallback. Timed transactions
 * apply erase and delivered page-program data in
 * ospi_timing_commit_transaction() when NCS is released. This path is
 * used only when the timing frame is unsupported, so preserve the
 * command-level WEL behavior without claiming timed flash completion.
 */
static void ospi_cmd_triggered(GnwH7B0OspiState *s)
{
    /* Program consumes WEL only after its data phase is delivered. */
    if (s->pending_instr == ospi_chip_program_opcode(s)) {
        return;
    }
    switch (s->pending_instr) {
    case OSPI_CMD_WREN:
        s->wel = true;
        return;
    case OSPI_CMD_RDID:
    case OSPI_CMD_RDSR:
    case OSPI_CMD_RDCR:
    case OSPI_CMD_RDAR:
    case OSPI_CMD_RDSR2:
    case OSPI_CMD_SFDP:
    case OSPI_CMD_RSTEN:
    case OSPI_CMD_RST:
    case 0x03: case 0x0B: case 0x3B: case 0x6B: case 0xBB:
    case 0x13: case 0x0C: case 0x3C: case 0x6C: case 0xBC:
    case 0xEB: case 0xEC:
        return;
    case OSPI_CMD_CE:
        ospi_backing_erase(s, 0, s->flash_size);
        s->wel = false;
        return;
    default:
        /* Erase/program/WRSR-class commands consume WEL. */
        s->wel = false;
        return;
    }
}

/*
 * Automatic status-polling mode (CR.FMODE == 2): real hardware
 * repeatedly issues the configured read command and compares the
 * received status against PSMAR under the PSMKR mask (AND or OR
 * per CR.PMM), setting SR.SMF on match. Our synthetic flash's status
 * responses are immediate and constant for a given command, so a
 * single evaluation at trigger time is exact: if the generated status
 * matches, SMF sets "instantly" (as it would on real idle flash); if
 * it doesn't, no amount of re-polling of this model would change the
 * answer, and leaving SMF clear until firmware reconfigures is also
 * the honest behavior. Found via 2026-07-12 lockstep tracing: stock
 * Zelda's OSPI reset sequence auto-polls RDSR after RSTEN/RST and
 * waits on SMF (HAL_OSPI_AutoPolling), trapping at its own
 * "HAL error" spin when SMF never set.
 */
static void ospi_autopoll_evaluate(GnwH7B0OspiState *s)
{
    uint32_t dlr = s->regs[GNW_H7B0_OSPI_DLR >> 2];
    uint32_t nbytes = (dlr >= 3) ? 4 : dlr + 1;
    uint32_t mask = s->regs[GNW_H7B0_OSPI_PSMKR >> 2];
    uint32_t match = s->regs[GNW_H7B0_OSPI_PSMAR >> 2];
    uint32_t status = 0;
    bool matched;

    for (uint32_t i = 0; i < nbytes; i++) {
        status |= (uint32_t)ospi_gen_read_byte(s, i) << (8 * i);
    }

    if (s->regs[GNW_H7B0_OSPI_CR >> 2] & OSPI_CR_PMM) {
        /* OR mode: any unmasked bit equal to its match bit. */
        matched = (~(status ^ match) & mask) != 0;
    } else {
        /* AND mode: all unmasked bits equal to their match bits. */
        matched = ((status ^ match) & mask) == 0;
    }

    if (matched) {
        s->regs[GNW_H7B0_OSPI_SR >> 2] |= OSPI_SR_SMF;
    }
}

/*
 * Level-sensitive IRQ re-evaluation: real hardware asserts the line
 * whenever any SR flag is both set and unmasked by the matching CR
 * xIE bit, and deasserts it the moment none are -- so this must be
 * called after every write that can change either SR or CR's IE bits.
 */
static void ospi_update_irq(GnwH7B0OspiState *s)
{
    uint32_t sr = s->regs[GNW_H7B0_OSPI_SR >> 2];
    uint32_t cr = s->regs[GNW_H7B0_OSPI_CR >> 2];
    bool pending = ((sr & OSPI_SR_TEF) && (cr & OSPI_CR_TEIE)) ||
                   ((sr & OSPI_SR_TCF) && (cr & OSPI_CR_TCIE)) ||
                   ((sr & OSPI_SR_FTF) && (cr & OSPI_CR_FTIE)) ||
                   ((sr & OSPI_SR_SMF) && (cr & OSPI_CR_SMIE)) ||
                   ((sr & OSPI_SR_TOF) && (cr & OSPI_CR_TOIE));

    qemu_set_irq(s->irq, pending);
}

static uint32_t ospi_timing_fmode(GnwH7B0OspiState *s)
{
    return (s->regs[GNW_H7B0_OSPI_CR >> 2] & OSPI_CR_FMODE_MASK) >>
           OSPI_CR_FMODE_SHIFT;
}

static uint32_t ospi_timing_threshold(GnwH7B0OspiState *s)
{
    return ((s->regs[GNW_H7B0_OSPI_CR >> 2] & OSPI_CR_FTHRES_MASK) >>
            OSPI_CR_FTHRES_SHIFT) + 1;
}

static void ospi_timing_update_status(GnwH7B0OspiState *s)
{
    uint32_t *sr = &s->regs[GNW_H7B0_OSPI_SR >> 2];
    uint32_t cr = s->regs[GNW_H7B0_OSPI_CR >> 2];
    uint32_t threshold = ospi_timing_threshold(s);
    uint32_t fmode = ospi_timing_fmode(s);
    bool ftf = false;

    *sr = (*sr & ~OSPI_SR_FLEVEL_MASK) |
          ((uint32_t)s->fifo_level << OSPI_SR_FLEVEL_SHIFT);
    if (cr & OSPI_CR_EN) {
        if (fmode == OSPI_FMODE_INDIRECT_WRITE) {
            ftf = GNW_H7B0_OSPI_FIFO_SIZE - s->fifo_level >= threshold;
        } else if (fmode == OSPI_FMODE_INDIRECT_READ) {
            ftf = s->fifo_level >= threshold ||
                  (s->timing_wire_complete && s->fifo_level != 0);
        }
    }
    if (ftf) {
        *sr |= OSPI_SR_FTF;
    } else {
        *sr &= ~OSPI_SR_FTF;
    }
    ospi_update_irq(s);
}

static void ospi_timing_fifo_reset(GnwH7B0OspiState *s)
{
    s->fifo_head = 0;
    s->fifo_level = 0;
    memset(s->fifo, 0, sizeof(s->fifo));
}

static void ospi_timing_fifo_push(GnwH7B0OspiState *s, uint8_t value)
{
    unsigned tail;

    g_assert(s->fifo_level < GNW_H7B0_OSPI_FIFO_SIZE);
    tail = (s->fifo_head + s->fifo_level) % GNW_H7B0_OSPI_FIFO_SIZE;
    s->fifo[tail] = value;
    s->fifo_level++;
}

static uint8_t ospi_timing_fifo_pop(GnwH7B0OspiState *s)
{
    uint8_t value;

    g_assert(s->fifo_level);
    value = s->fifo[s->fifo_head];
    s->fifo_head = (s->fifo_head + 1) % GNW_H7B0_OSPI_FIFO_SIZE;
    s->fifo_level--;
    return value;
}

static unsigned ospi_timing_lanes(uint32_t mode)
{
    switch (mode) {
    case 1: return 1;
    case 2: return 2;
    case 3: return 4;
    case 4: return 8;
    default: return 0;
    }
}

static uint64_t ospi_timing_phase_quanta(uint32_t bits, uint32_t mode,
                                         bool dtr)
{
    unsigned lanes = ospi_timing_lanes(mode);
    uint64_t transfers;

    if (!lanes) {
        return 0;
    }
    transfers = DIV_ROUND_UP(bits, lanes);
    return dtr ? transfers : transfers * 2;
}

static uint64_t ospi_timing_denominator(GnwH7B0OspiState *s)
{
    uint64_t divider = (s->active_dcr2 & OSPI_DCR2_PRESCALER_MASK) + 1;

    return divider * NANOSECONDS_PER_SECOND;
}

static uint64_t ospi_timing_phase_max_quanta(uint8_t phase)
{
    switch (phase) {
    case GNW_H7B0_OSPI_PHASE_IDLE:
        return 0;
    case GNW_H7B0_OSPI_PHASE_CS_HIGH:
        return 16;
    case GNW_H7B0_OSPI_PHASE_PREAMBLE:
    case GNW_H7B0_OSPI_PHASE_POSTAMBLE:
        return 2;
    case GNW_H7B0_OSPI_PHASE_INSTRUCTION:
    case GNW_H7B0_OSPI_PHASE_ADDRESS:
    case GNW_H7B0_OSPI_PHASE_ALTERNATE:
        return 64;
    case GNW_H7B0_OSPI_PHASE_DUMMY:
        return 62;
    case GNW_H7B0_OSPI_PHASE_DATA:
        return 16;
    default:
        return 0;
    }
}

static void ospi_timing_settle(GnwH7B0OspiState *s, int64_t now)
{
    uint64_t denominator, rate, elapsed;
    unsigned __int128 advance, phase_work;

    if (!s->timing_active || !s->phase_remaining_quanta ||
        !s->kernel_clock_hz || now <= s->phase_last_update_ns) {
        s->phase_last_update_ns = now;
        return;
    }
    denominator = ospi_timing_denominator(s);
    rate = (uint64_t)s->kernel_clock_hz * 2;
    elapsed = now - s->phase_last_update_ns;
    advance = (unsigned __int128)elapsed * rate + s->phase_progress_num;
    phase_work = (unsigned __int128)s->phase_remaining_quanta * denominator;
    if (advance >= phase_work) {
        s->phase_remaining_quanta = 0;
        advance -= phase_work;
        /*
         * QEMU invokes the timer at its integer-ns deadline, so normal
         * overshoot is below one nanosecond.  Saturation only protects an
         * unexpectedly late callback from corrupting state.
         */
        s->phase_progress_num = MIN(advance,
                                    (unsigned __int128)UINT64_MAX);
    } else {
        s->phase_remaining_quanta -= (uint64_t)(advance / denominator);
        s->phase_progress_num = advance % denominator;
    }
    s->phase_last_update_ns = now;
}

static void ospi_timing_schedule(GnwH7B0OspiState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t needed, delay;

    timer_del(s->timing_timer);
    s->phase_last_update_ns = now;
    if (!s->timing_active || !s->phase_remaining_quanta ||
        !s->kernel_clock_hz) {
        return;
    }
    needed = s->phase_remaining_quanta * ospi_timing_denominator(s) -
             s->phase_progress_num;
    delay = DIV_ROUND_UP(needed, (uint64_t)s->kernel_clock_hz * 2);
    delay = MAX(delay, 1);
    timer_mod(s->timing_timer,
              delay > INT64_MAX - now ? INT64_MAX : now + delay);
}

static void ospi_timing_phase_complete(GnwH7B0OspiState *s);

static void ospi_timing_begin_phase(GnwH7B0OspiState *s,
                                    GnwH7B0OspiPhase phase,
                                    uint64_t quanta)
{
    uint64_t denominator = ospi_timing_denominator(s);
    unsigned __int128 phase_work = (unsigned __int128)quanta * denominator;

    s->timing_phase = phase;
    s->phase_remaining_quanta = quanta;
    if (s->phase_progress_num >= phase_work) {
        s->phase_progress_num -= phase_work;
        s->phase_remaining_quanta = 0;
        ospi_timing_phase_complete(s);
        return;
    }
    s->phase_remaining_quanta -= s->phase_progress_num / denominator;
    s->phase_progress_num %= denominator;
    ospi_timing_schedule(s);
}

static bool ospi_timing_frame_supported(GnwH7B0OspiState *s)
{
    uint32_t ccr = s->regs[GNW_H7B0_OSPI_CCR >> 2];
    uint32_t dcr1 = s->regs[GNW_H7B0_OSPI_DCR1 >> 2];
    uint32_t cr = s->regs[GNW_H7B0_OSPI_CR >> 2];
    uint32_t modes[] = {
        (ccr & OSPI_CCR_IMODE_MASK) >> OSPI_CCR_IMODE_SHIFT,
        (ccr & OSPI_CCR_ADMODE_MASK) >> OSPI_CCR_ADMODE_SHIFT,
        (ccr & OSPI_CCR_ABMODE_MASK) >> OSPI_CCR_ABMODE_SHIFT,
        (ccr & OSPI_CCR_DMODE_MASK) >> OSPI_CCR_DMODE_SHIFT,
    };
    unsigned i;

    if ((cr & OSPI_CR_DQM) ||
        ((dcr1 & OSPI_DCR1_MTYP_MASK) >> OSPI_DCR1_MTYP_SHIFT) ==
            OSPI_DCR1_MTYP_HYPERBUS ||
        ospi_timing_fmode(s) > OSPI_FMODE_INDIRECT_READ) {
        return false;
    }
    for (i = 0; i < ARRAY_SIZE(modes); i++) {
        if (modes[i] > 4) {
            return false;
        }
    }
    if ((s->pending_instr == OSPI_CMD_PP) &&
        (uint64_t)s->regs[GNW_H7B0_OSPI_DLR >> 2] + 1 >
            GNW_H7B0_OSPI_PROGRAM_BUFFER_SIZE) {
        return false;
    }
    return true;
}

static void ospi_timing_log_fallback(GnwH7B0OspiState *s)
{
    if (s->timing_fallback_logged) {
        return;
    }
    s->timing_fallback_logged = true;
    qemu_log_mask(LOG_UNIMP,
                  "%s: unsupported OCTOSPI timing frame; using instant "
                  "functional fallback (no accuracy claim)\n", __func__);
}

static void ospi_timing_commit_transaction(GnwH7B0OspiState *s)
{
    switch (s->active_instr) {
    case OSPI_CMD_WREN:
        s->wel = true;
        break;
    case OSPI_CMD_RDID:
    case OSPI_CMD_RDSR:
    case OSPI_CMD_RDCR:
    case OSPI_CMD_SFDP:
    case OSPI_CMD_RSTEN:
    case OSPI_CMD_RST:
    case OSPI_CMD_READ:
        break;
    case OSPI_CMD_CE:
        ospi_backing_erase(s, 0, s->flash_size);
        s->wel = false;
        break;
    case OSPI_CMD_SE:
        ospi_backing_erase(s, s->active_addr, 4 * KiB);
        s->wel = false;
        break;
    case OSPI_CMD_BE32K:
        ospi_backing_erase(s, s->active_addr, 32 * KiB);
        s->wel = false;
        break;
    case OSPI_CMD_BE64K:
        ospi_backing_erase(s, s->active_addr, 64 * KiB);
        s->wel = false;
        break;
    case OSPI_CMD_PP:
        if (s->wel && s->backing) {
            for (unsigned i = 0; i < s->delivered_program_len; i++) {
                uint64_t off = (s->active_addr & ~0xffu) |
                               ((s->active_addr + i) & 0xffu);
                if (off < s->flash_size) {
                    s->backing[off] &= s->delivered_program[i];
                }
            }
        }
        s->wel = false;
        break;
    default:
        s->wel = false;
        break;
    }
    s->delivered_program_len = 0;
}

static void ospi_timing_finish_if_idle(GnwH7B0OspiState *s)
{
    if (!s->timing_wire_complete || !s->timing_ncs_released ||
        s->fifo_level) {
        return;
    }
    s->timing_active = false;
    s->timing_phase = GNW_H7B0_OSPI_PHASE_IDLE;
    s->regs[GNW_H7B0_OSPI_SR >> 2] &= ~OSPI_SR_BUSY;
    ospi_timing_update_status(s);
}

static void ospi_timing_advance(GnwH7B0OspiState *s);

static void ospi_timing_phase_complete(GnwH7B0OspiState *s)
{
    uint8_t value;

    switch (s->timing_phase) {
    case GNW_H7B0_OSPI_PHASE_CS_HIGH:
        break;
    case GNW_H7B0_OSPI_PHASE_INSTRUCTION:
        s->timing_instruction_delivered = true;
        break;
    case GNW_H7B0_OSPI_PHASE_DATA:
        if (ospi_timing_fmode(s) == OSPI_FMODE_INDIRECT_READ) {
            g_assert(s->fifo_level < GNW_H7B0_OSPI_FIFO_SIZE);
            value = ospi_gen_read_byte(s, s->active_delivered);
            ospi_timing_fifo_push(s, value);
        } else {
            value = ospi_timing_fifo_pop(s);
            if (s->active_instr == OSPI_CMD_PP) {
                g_assert(s->delivered_program_len <
                         GNW_H7B0_OSPI_PROGRAM_BUFFER_SIZE);
                s->delivered_program[s->delivered_program_len++] = value;
            }
        }
        s->active_delivered++;
        ospi_timing_update_status(s);
        if (s->active_delivered == s->active_length) {
            s->timing_wire_complete = true;
            s->regs[GNW_H7B0_OSPI_SR >> 2] |= OSPI_SR_TCF;
            if (ospi_timing_fmode(s) == OSPI_FMODE_INDIRECT_WRITE) {
                ospi_timing_fifo_reset(s);
            }
            ospi_timing_update_status(s);
            ospi_timing_begin_phase(s, GNW_H7B0_OSPI_PHASE_POSTAMBLE, 2);
            return;
        }
        break;
    case GNW_H7B0_OSPI_PHASE_POSTAMBLE:
        /*
         * The wire is idle after NCS release; don't pre-run a future
         * command.
         */
        s->phase_progress_num = 0;
        ospi_timing_commit_transaction(s);
        s->timing_ncs_released = true;
        s->ncs_release_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        ospi_timing_finish_if_idle(s);
        return;
    default:
        break;
    }
    ospi_timing_advance(s);
}

static void ospi_timing_advance(GnwH7B0OspiState *s)
{
    uint32_t ccr = s->active_ccr;
    uint32_t mode, size;
    uint64_t quanta;

    for (;;) {
        switch (s->timing_phase) {
        case GNW_H7B0_OSPI_PHASE_CS_HIGH:
            ospi_timing_begin_phase(s, GNW_H7B0_OSPI_PHASE_PREAMBLE, 2);
            return;
        case GNW_H7B0_OSPI_PHASE_PREAMBLE:
            mode = (ccr & OSPI_CCR_IMODE_MASK) >> OSPI_CCR_IMODE_SHIFT;
            size = ((ccr & OSPI_CCR_ISIZE_MASK) >> OSPI_CCR_ISIZE_SHIFT) + 1;
            quanta = ospi_timing_phase_quanta(size * 8, mode,
                                               ccr & OSPI_CCR_IDTR);
            s->timing_phase = GNW_H7B0_OSPI_PHASE_INSTRUCTION;
            break;
        case GNW_H7B0_OSPI_PHASE_INSTRUCTION:
            mode = (ccr & OSPI_CCR_ADMODE_MASK) >> OSPI_CCR_ADMODE_SHIFT;
            size = ((ccr & OSPI_CCR_ADSIZE_MASK) >> OSPI_CCR_ADSIZE_SHIFT) + 1;
            quanta = ospi_timing_phase_quanta(size * 8, mode,
                                               ccr & OSPI_CCR_ADDTR);
            s->timing_phase = GNW_H7B0_OSPI_PHASE_ADDRESS;
            break;
        case GNW_H7B0_OSPI_PHASE_ADDRESS:
            mode = (ccr & OSPI_CCR_ABMODE_MASK) >> OSPI_CCR_ABMODE_SHIFT;
            size = ((ccr & OSPI_CCR_ABSIZE_MASK) >> OSPI_CCR_ABSIZE_SHIFT) + 1;
            quanta = ospi_timing_phase_quanta(size * 8, mode,
                                               ccr & OSPI_CCR_ABDTR);
            s->timing_phase = GNW_H7B0_OSPI_PHASE_ALTERNATE;
            break;
        case GNW_H7B0_OSPI_PHASE_ALTERNATE:
            mode = (ccr & OSPI_CCR_DMODE_MASK) >> OSPI_CCR_DMODE_SHIFT;
            quanta = mode ? 2 * (s->active_tcr & OSPI_TCR_DCYC_MASK) : 0;
            s->timing_phase = GNW_H7B0_OSPI_PHASE_DUMMY;
            break;
        case GNW_H7B0_OSPI_PHASE_DUMMY:
            if (!s->active_length) {
                s->timing_wire_complete = true;
                s->regs[GNW_H7B0_OSPI_SR >> 2] |= OSPI_SR_TCF;
                ospi_timing_update_status(s);
                ospi_timing_begin_phase(s,
                                        GNW_H7B0_OSPI_PHASE_POSTAMBLE, 2);
                return;
            }
            s->timing_phase = GNW_H7B0_OSPI_PHASE_DATA;
            quanta = 0;
            break;
        case GNW_H7B0_OSPI_PHASE_DATA:
            if (ospi_timing_fmode(s) == OSPI_FMODE_INDIRECT_READ) {
                if (s->fifo_level == GNW_H7B0_OSPI_FIFO_SIZE) {
                    s->phase_remaining_quanta = 0;
                    s->phase_progress_num = 0;
                    timer_del(s->timing_timer);
                    return;
                }
            } else if (!s->fifo_level) {
                s->phase_remaining_quanta = 0;
                s->phase_progress_num = 0;
                timer_del(s->timing_timer);
                return;
            }
            mode = (ccr & OSPI_CCR_DMODE_MASK) >> OSPI_CCR_DMODE_SHIFT;
            quanta = ospi_timing_phase_quanta(8, mode,
                                               ccr & OSPI_CCR_DDTR);
            break;
        default:
            return;
        }
        if (quanta) {
            ospi_timing_begin_phase(s, s->timing_phase, quanta);
            return;
        }
    }
}

static void ospi_timing_timer_cb(void *opaque)
{
    GnwH7B0OspiState *s = opaque;

    ospi_timing_settle(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    if (!s->phase_remaining_quanta) {
        ospi_timing_phase_complete(s);
    } else {
        ospi_timing_schedule(s);
    }
}

static bool ospi_timing_start(GnwH7B0OspiState *s)
{
    uint32_t ccr = s->regs[GNW_H7B0_OSPI_CCR >> 2];
    uint32_t dmode = (ccr & OSPI_CCR_DMODE_MASK) >> OSPI_CCR_DMODE_SHIFT;
    uint64_t length = dmode ?
        (uint64_t)s->regs[GNW_H7B0_OSPI_DLR >> 2] + 1 : 0;

    if (!(s->regs[GNW_H7B0_OSPI_CR >> 2] & OSPI_CR_EN) ||
        !ospi_timing_frame_supported(s)) {
        ospi_timing_log_fallback(s);
        return false;
    }
    if (s->timing_active) {
        return true;
    }
    s->active_ccr = ccr;
    s->active_tcr = s->regs[GNW_H7B0_OSPI_TCR >> 2];
    s->active_dcr1 = s->regs[GNW_H7B0_OSPI_DCR1 >> 2];
    s->active_dcr2 = s->regs[GNW_H7B0_OSPI_DCR2 >> 2];
    s->active_instr = s->pending_instr;
    s->active_addr = s->pending_addr;
    s->active_length = length;
    s->active_delivered = 0;
    s->active_consumed = 0;
    s->delivered_program_len = 0;
    s->phase_progress_num = 0;
    s->timing_active = true;
    s->timing_wire_complete = false;
    s->timing_instruction_delivered = false;
    s->regs[GNW_H7B0_OSPI_SR >> 2] &= ~(OSPI_SR_TCF | OSPI_SR_TEF);
    s->regs[GNW_H7B0_OSPI_SR >> 2] |= OSPI_SR_BUSY;
    ospi_timing_update_status(s);
    if (s->timing_ncs_released) {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        uint64_t denominator = ospi_timing_denominator(s);
        uint64_t quanta =
            2 * (((s->active_dcr1 & OSPI_DCR1_CSHT_MASK) >>
                  OSPI_DCR1_CSHT_SHIFT) + 1);
        unsigned __int128 required =
            (unsigned __int128)quanta * denominator;
        unsigned __int128 idle_credit = 0;

        ospi_timing_begin_phase(s, GNW_H7B0_OSPI_PHASE_CS_HIGH, quanta);
        if (s->kernel_clock_hz && now > s->ncs_release_ns) {
            idle_credit = (unsigned __int128)(now - s->ncs_release_ns) *
                          s->kernel_clock_hz * 2;
            idle_credit = MIN(idle_credit, required);
        }
        s->phase_remaining_quanta -= idle_credit / denominator;
        s->phase_progress_num = idle_credit % denominator;
        s->phase_last_update_ns = now;
        if (idle_credit == required) {
            /*
             * Time before the trigger can satisfy CSHT, but cannot run any
             * part of the newly triggered command.
             */
            s->phase_progress_num = 0;
            s->phase_remaining_quanta = 0;
            ospi_timing_phase_complete(s);
        } else {
            ospi_timing_schedule(s);
        }
    } else {
        ospi_timing_begin_phase(s, GNW_H7B0_OSPI_PHASE_PREAMBLE, 2);
    }
    s->timing_ncs_released = false;
    return true;
}

static bool ospi_timing_should_trigger(GnwH7B0OspiState *s, hwaddr addr)
{
    uint32_t ccr = s->regs[GNW_H7B0_OSPI_CCR >> 2];
    uint32_t admode = (ccr & OSPI_CCR_ADMODE_MASK) >> OSPI_CCR_ADMODE_SHIFT;
    uint32_t dmode = (ccr & OSPI_CCR_DMODE_MASK) >> OSPI_CCR_DMODE_SHIFT;
    uint32_t fmode = ospi_timing_fmode(s);

    if (fmode > OSPI_FMODE_INDIRECT_READ) {
        return false;
    }
    if (addr == GNW_H7B0_OSPI_DR) {
        return fmode == OSPI_FMODE_INDIRECT_WRITE && dmode != 0;
    }
    if (addr == GNW_H7B0_OSPI_IR) {
        return admode == 0 &&
               (fmode == OSPI_FMODE_INDIRECT_READ || dmode == 0);
    }
    return addr == GNW_H7B0_OSPI_AR && admode != 0 &&
           (fmode == OSPI_FMODE_INDIRECT_READ || dmode == 0);
}

static void ospi_timing_abort(GnwH7B0OspiState *s)
{
    if (s->timing_active) {
        bool phase_was_running = s->phase_remaining_quanta != 0;

        ospi_timing_settle(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        if (phase_was_running && !s->phase_remaining_quanta) {
            ospi_timing_phase_complete(s);
        }
        timer_del(s->timing_timer);
        /* NCS closes only a command which actually reached the device. */
        if (s->timing_active && s->timing_instruction_delivered) {
            ospi_timing_commit_transaction(s);
        } else if (s->timing_active) {
            s->delivered_program_len = 0;
        }
    }
    ospi_timing_fifo_reset(s);
    s->timing_active = false;
    s->timing_wire_complete = false;
    s->timing_instruction_delivered = false;
    s->timing_ncs_released = true;
    s->ncs_release_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->timing_phase = GNW_H7B0_OSPI_PHASE_IDLE;
    s->phase_remaining_quanta = 0;
    s->phase_progress_num = 0;
    s->regs[GNW_H7B0_OSPI_SR >> 2] &= ~OSPI_SR_BUSY;
    s->regs[GNW_H7B0_OSPI_SR >> 2] |= OSPI_SR_TCF;
    ospi_timing_update_status(s);
}

void gnw_h7b0_ospi_set_kernel_clock_hz(GnwH7B0OspiState *s, uint32_t hz)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    bool phase_was_running = s->phase_remaining_quanta != 0;

    if (s->timing_mode) {
        ospi_timing_settle(s, now);
        if (phase_was_running && !s->phase_remaining_quanta) {
            ospi_timing_phase_complete(s);
        }
    }
    s->kernel_clock_hz = hz;
    if (s->timing_mode) {
        ospi_timing_schedule(s);
    }
}

void gnw_h7b0_ospi_kernel_clock_changed(void *opaque,
                                         uint32_t old_hz,
                                         uint32_t new_hz)
{
    GnwH7B0OspiState *s = opaque;

    if (s->kernel_clock_hz != old_hz) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: stale OCTOSPI old clock %u (device has %u)\n",
                      __func__, old_hz, s->kernel_clock_hz);
    }
    gnw_h7b0_ospi_set_kernel_clock_hz(s, new_hz);
}

static void ospi_test_clock_get(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    GnwH7B0OspiState *s = GNW_H7B0_OSPI(obj);
    uint32_t hz = s->test_kernel_clock_hz;

    visit_type_uint32(v, name, &hz, errp);
}

static void ospi_test_clock_set(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    GnwH7B0OspiState *s = GNW_H7B0_OSPI(obj);
    uint32_t hz;

    if (!visit_type_uint32(v, name, &hz, errp)) {
        return;
    }
    s->test_kernel_clock_hz = hz;
    gnw_h7b0_ospi_set_kernel_clock_hz(s, hz);
}

static uint64_t gnw_h7b0_ospi_read(void *opaque, hwaddr addr,
                                    unsigned int size)
{
    GnwH7B0OspiState *s = GNW_H7B0_OSPI(opaque);

    if (addr >= GNW_H7B0_OSPI_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return 0;
    }

    if (addr == GNW_H7B0_OSPI_DR) {
        if (s->timing_mode && s->timing_active &&
            ospi_timing_fmode(s) == OSPI_FMODE_INDIRECT_READ) {
            uint64_t value = 0;

            for (unsigned i = 0; i < size; i++) {
                uint8_t byte = 0xff;

                if (s->fifo_level) {
                    byte = ospi_timing_fifo_pop(s);
                    s->active_consumed++;
                }
                value |= (uint64_t)byte << (i * 8);
            }
            ospi_timing_update_status(s);
            if (!s->timing_wire_complete &&
                s->timing_phase == GNW_H7B0_OSPI_PHASE_DATA &&
                !s->phase_remaining_quanta) {
                ospi_timing_advance(s);
            }
            ospi_timing_finish_if_idle(s);
            return value;
        }
        /*
         * HAL_OSPI_Receive() reads DR one byte at a time (see
         * gnw_h7b0_ospi.h); real hardware auto-increments through the
         * FIFO/data-phase for each byte transferred.  Consume every byte
         * represented by a wide DR access, matching the timed FIFO path.
         *
         * DLR holds NbData-1 (see stm32h7xx_hal_ospi.c's "DLR =
         * NbData - 1U" in both HAL_OSPI_Command() and the DMA path),
         * so the transfer covers exactly DLR+1 bytes; well-behaved
         * firmware never reads past that (it polls TCF/checks
         * XferCount, both already satisfied by the AR/IR write
         * handler below setting TCF instantly). Bound dr_pos anyway
         * so a guest that ignores status and keeps reading DR past
         * its own configured length gets a flat 0xFF (idle-bus/
         * erased-flash filler) instead of the byte generator quietly
         * running off the end of whatever pending_instr's data phase
         * actually means (e.g. re-reading stale RDID/SFDP bytes).
         */
        uint32_t dlr = s->regs[GNW_H7B0_OSPI_DLR >> 2];
        uint64_t value = 0;

        for (unsigned i = 0; i < size; i++) {
            uint8_t byte = 0xff;

            if (s->dr_pos <= dlr) {
                byte = ospi_gen_read_byte(s, s->dr_pos);
                s->dr_pos++;
            }
            value |= (uint64_t)byte << (i * 8);
        }
        return value;
    }

    if (addr == GNW_H7B0_OSPI_SR && s->timing_mode &&
        ospi_timing_frame_supported(s)) {
        ospi_timing_update_status(s);
    }

    return s->regs[addr >> 2];
}

static void gnw_h7b0_ospi_write(void *opaque, hwaddr addr,
                                 uint64_t val64, unsigned int size)
{
    GnwH7B0OspiState *s = GNW_H7B0_OSPI(opaque);
    uint32_t value = val64;

    if (addr >= GNW_H7B0_OSPI_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return;
    }

    uint32_t mask = get_ospi_write_mask(addr);
    value = (s->regs[addr >> 2] & ~mask) | (value & mask);

    if (s->timing_mode && s->timing_active &&
        addr != GNW_H7B0_OSPI_CR && addr != GNW_H7B0_OSPI_FCR &&
        addr != GNW_H7B0_OSPI_DR) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: ignoring OCTOSPI register write 0x%" HWADDR_PRIx
                      " while BUSY\n", __func__, addr);
        return;
    }

    switch (addr) {
    case GNW_H7B0_OSPI_IR:
    case GNW_H7B0_OSPI_AR:
        /*
         * Real hardware starts the transaction once the command is
         * fully configured -- IR alone for HAL_OSPI_Command()'s
         * no-data / no-address path, or AR/IR (re-written by
         * HAL_OSPI_Receive()/HAL_OSPI_Transmit() themselves, after
         * HAL_OSPI_Command() already configured but didn't start the
         * transfer) for the data phase, per whichever of AR/IR
         * corresponds to the command's address mode. Latch the
         * instruction/address for the data-phase byte generator
         * below, apply any no-data-phase side effects (WREN/etc)
         * immediately, and instantly report the command as complete
         * (TCF+FTF set, BUSY clear) so every polling loop -- the
         * command's own, and each subsequent per-byte DR access --
         * succeeds right away.
         */
        s->regs[addr >> 2] = value;
        if (addr == GNW_H7B0_OSPI_IR) {
            s->pending_instr = value & 0xFF;
        }
        if (addr == GNW_H7B0_OSPI_AR) {
            s->pending_addr = value;
            /*
             * pending_addr is used as a plain integer offset below
             * regardless of address width (AR is a real 32-bit register
             * on actual hardware too -- ADSIZE only shapes how many
             * bytes get clocked out over the wire, not AR's numeric
             * contents), so there's no width-dependent decode needed
             * here. Still, verify firmware actually configured the
             * ADSIZE this device's synthetic identity is documented to
             * use, so a future flash-detection change picking a
             * different command table (e.g. 24-bit addressing) becomes
             * a visible log instead of a silent misbehavior.
             */
            uint32_t expected_adsize, actual_adsize;

            if (ospi_expected_adsize(s, s->pending_instr, &expected_adsize)) {
                actual_adsize = (s->regs[GNW_H7B0_OSPI_CCR >> 2]
                                  & OSPI_CCR_ADSIZE_MASK)
                                 >> OSPI_CCR_ADSIZE_SHIFT;
                if (actual_adsize != expected_adsize) {
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "%s: instruction 0x%02x configured CCR "
                                  "ADSIZE=%u, expected %u for this "
                                  "device's synthetic flash identity -- "
                                  "address-phase decoding may be wrong\n",
                                  __func__, s->pending_instr,
                                  actual_adsize, expected_adsize);
                }
            }
        }
        s->dr_pos = 0;
        if (s->timing_mode) {
            if (ospi_timing_fmode(s) <= OSPI_FMODE_INDIRECT_READ &&
                !ospi_timing_should_trigger(s, addr)) {
                return;
            }
            if (ospi_timing_should_trigger(s, addr) &&
                ospi_timing_start(s)) {
                return;
            }
            if (ospi_timing_fmode(s) > OSPI_FMODE_INDIRECT_READ) {
                ospi_timing_log_fallback(s);
            }
        }

        /* Addressed commands do not start at IR: WREN must survive
         * until AR completes the command, especially for sector erase. */
        if (addr == GNW_H7B0_OSPI_IR &&
            (s->regs[GNW_H7B0_OSPI_CCR >> 2] & OSPI_CCR_ADMODE_MASK)) {
            return;
        }

        if (addr == GNW_H7B0_OSPI_AR) {
            /* Sector/block erase has an address phase but no data phase. */
            uint64_t erase_size = ospi_chip_erase_size(s, s->pending_instr);
            if (erase_size) {
                ospi_backing_erase(s, s->pending_addr, erase_size);
            }
        }
        ospi_cmd_triggered(s);
        s->regs[GNW_H7B0_OSPI_SR >> 2] |= OSPI_SR_TCF | OSPI_SR_FTF;
        s->regs[GNW_H7B0_OSPI_SR >> 2] &= ~OSPI_SR_BUSY;
        if (((s->regs[GNW_H7B0_OSPI_CR >> 2] & OSPI_CR_FMODE_MASK)
              >> OSPI_CR_FMODE_SHIFT) == OSPI_FMODE_AUTOPOLL) {
            ospi_autopoll_evaluate(s);
        }
        ospi_update_irq(s);
        return;
    case GNW_H7B0_OSPI_DR:
        if (s->timing_mode &&
            (s->timing_active || ospi_timing_should_trigger(s, addr))) {
            bool handled = s->timing_active || ospi_timing_start(s);

            if (handled) {
                uint64_t remaining = s->active_length >
                                     s->active_delivered + s->fifo_level ?
                    s->active_length - s->active_delivered - s->fifo_level : 0;
                unsigned count = MIN((uint64_t)size, remaining);

                count = MIN(count,
                            GNW_H7B0_OSPI_FIFO_SIZE - s->fifo_level);
                for (unsigned i = 0; i < count; i++) {
                    ospi_timing_fifo_push(s, val64 >> (i * 8));
                }
                ospi_timing_update_status(s);
                if (s->timing_phase == GNW_H7B0_OSPI_PHASE_DATA &&
                    !s->phase_remaining_quanta) {
                    ospi_timing_advance(s);
                }
                return;
            }
        }
        /*
         * HAL_OSPI_Transmit() normally writes DR one byte at a time. For page
         * program, persist every byte represented by the access to the XIP
         * backing store at
         * pending_addr+dr_pos (mirrors ospi_gen_read_byte()'s
         * addressing for indirect reads); any other write-class
         * command (e.g. WRSR) still has its byte dropped, since only
         * PP is modeled with a real data-phase effect.
         */
        if (s->pending_instr == ospi_chip_program_opcode(s) && s->backing) {
            uint64_t length = (uint64_t)s->regs[GNW_H7B0_OSPI_DLR >> 2] + 1;

            for (unsigned i = 0; i < size && s->dr_pos < length; i++) {
                uint64_t off = (s->pending_addr & ~0xffu) |
                               ((s->pending_addr + s->dr_pos) & 0xffu);

                if (s->wel && off < s->flash_size) {
                    s->backing[off] &= val64 >> (i * 8);
                }
                s->dr_pos++;
            }
            if (s->dr_pos == length) {
                s->wel = false;
            }
            return;
        }
        s->dr_pos++;
        return;
    case GNW_H7B0_OSPI_CCR:
        s->regs[addr >> 2] = value;
        return;
    case GNW_H7B0_OSPI_FCR:
        /* Write-1-to-clear: FCR bit positions mirror SR's exactly. */
        s->regs[GNW_H7B0_OSPI_SR >> 2] &= ~value;
        if (s->timing_mode && ospi_timing_frame_supported(s)) {
            ospi_timing_update_status(s);
        } else {
            ospi_update_irq(s);
        }
        return;
    case GNW_H7B0_OSPI_CR:
    {
        bool requested_abort = value & OSPI_CR_ABORT;

        /*
         * ABORT is self-clearing: RM0455 24.4.18 says BUSY and ABORT
         * are both reset once the abort completes, and our transfers
         * are instantaneous, so it never reads back as set.
         */
        if (s->timing_mode && s->timing_active) {
            uint32_t locked = OSPI_CR_EN | OSPI_CR_DQM |
                              OSPI_CR_FTHRES_MASK | OSPI_CR_FMODE_MASK;

            value = (value & ~locked) |
                    (s->regs[addr >> 2] & locked);
        }
        s->regs[addr >> 2] = value & ~OSPI_CR_ABORT;
        if (requested_abort) {
            if (s->timing_mode && s->timing_active) {
                ospi_timing_abort(s);
            } else {
                s->regs[GNW_H7B0_OSPI_SR >> 2] &= ~OSPI_SR_BUSY;
            }
        }
        /* EN/FMODE decide whether the XIP window exists at all. */
        ospi_xip_gate_update(s);
        if (s->timing_mode && ospi_timing_frame_supported(s)) {
            ospi_timing_update_status(s);
        } else {
            ospi_update_irq(s);
        }
        return;
    }
    case GNW_H7B0_OSPI_SR:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: SR is read-only on real hardware (use FCR to "
                      "clear flags)\n", __func__);
        return;
    default:
        gnw_h7b0_stub_log_unimp_ratelimited(s->logged_unimp, __func__, addr);
        s->regs[addr >> 2] = value;
    }
}

static const MemoryRegionOps gnw_h7b0_ospi_ops = {
    .read = gnw_h7b0_ospi_read,
    .write = gnw_h7b0_ospi_write,
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

void gnw_h7b0_ospi_set_backing(GnwH7B0OspiState *s, void *backing)
{
    s->backing = backing;
}

void gnw_h7b0_ospi_set_xip_window(GnwH7B0OspiState *s,
                                   MemoryRegion *system_memory,
                                   hwaddr base, uint64_t size)
{
    char name[64];

    snprintf(name, sizeof(name), "%s-xip-gate",
             object_get_canonical_path_component(OBJECT(s)));
    memory_region_init_io(&s->xip_gate, OBJECT(s), &ospi_xip_gate_ops, s,
                           name, size);
    s->xip_as = system_memory;
    s->xip_base = base;
    s->xip_size = size;
    s->xip_gate_mapped = false;
    /* Reset already ran at realize; apply the gate for the current CR. */
    ospi_xip_gate_update(s);
}

static void gnw_h7b0_ospi_init(Object *obj)
{
    GnwH7B0OspiState *s = GNW_H7B0_OSPI(obj);

    s->timing_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                   ospi_timing_timer_cb, s);
    object_property_add(obj, "x-kernel-clock-hz", "uint32",
                        ospi_test_clock_get, ospi_test_clock_set, NULL, NULL);
    memory_region_init_io(&s->mmio, obj, &gnw_h7b0_ospi_ops, s,
                           TYPE_GNW_H7B0_OSPI, GNW_H7B0_OSPI_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

/*
 * The gate's mapped/unmapped state is a pure function of CR (EN and
 * FMODE), which is migrated, so re-derive it rather than migrating the
 * MemoryRegion mapping itself.
 */
static int gnw_h7b0_ospi_pre_save(void *opaque)
{
    GnwH7B0OspiState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (s->timing_mode && s->timing_active) {
        bool phase_was_running = s->phase_remaining_quanta != 0;

        ospi_timing_settle(s, now);
        if (phase_was_running && !s->phase_remaining_quanta) {
            ospi_timing_phase_complete(s);
        }
    }
    s->migrated_ncs_release_elapsed_ns =
        s->timing_ncs_released && now > s->ncs_release_ns ?
            now - s->ncs_release_ns : 0;
    return 0;
}

static int gnw_h7b0_ospi_post_load(void *opaque, int version_id)
{
    GnwH7B0OspiState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (version_id < 2) {
        if (s->timing_mode) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: legacy OCTOSPI snapshot has no timing-mode "
                          "identity\n", __func__);
            return -EINVAL;
        }
    } else {
        if (s->timing_config_identity !=
                (s->timing_mode ? OSPI_TIMING_CONFIG_ID : 0) ||
            s->fifo_head >= GNW_H7B0_OSPI_FIFO_SIZE ||
            s->fifo_level > GNW_H7B0_OSPI_FIFO_SIZE ||
            s->delivered_program_len > GNW_H7B0_OSPI_PROGRAM_BUFFER_SIZE ||
            s->timing_phase > GNW_H7B0_OSPI_PHASE_POSTAMBLE ||
            s->active_length > (UINT64_C(1) << 32) ||
            s->phase_remaining_quanta >
                ospi_timing_phase_max_quanta(s->timing_phase) ||
            s->active_delivered > s->active_length ||
            s->active_consumed > s->active_length ||
            (s->timing_active &&
             ospi_timing_fmode(s) == OSPI_FMODE_INDIRECT_READ &&
             (s->active_consumed > s->active_delivered ||
              s->active_delivered - s->active_consumed != s->fifo_level)) ||
            (s->timing_active &&
             ospi_timing_fmode(s) == OSPI_FMODE_INDIRECT_WRITE &&
             (s->active_delivered + s->fifo_level > s->active_length)) ||
            (s->timing_active && s->active_instr == OSPI_CMD_PP &&
             s->delivered_program_len != s->active_delivered) ||
            (s->active_instr != OSPI_CMD_PP &&
             s->delivered_program_len != 0) ||
            (!s->timing_active &&
             (s->fifo_level || s->delivered_program_len)) ||
            s->phase_progress_num >= ospi_timing_denominator(s) ||
            (s->timing_active &&
             s->timing_phase == GNW_H7B0_OSPI_PHASE_IDLE) ||
            (s->timing_active && !s->phase_remaining_quanta &&
             s->timing_phase != GNW_H7B0_OSPI_PHASE_DATA &&
             !(s->timing_phase == GNW_H7B0_OSPI_PHASE_POSTAMBLE &&
               s->timing_ncs_released && s->timing_wire_complete)) ||
            (!s->timing_active &&
             s->timing_phase != GNW_H7B0_OSPI_PHASE_IDLE) ||
            (s->timing_wire_complete &&
             s->active_delivered != s->active_length) ||
            (s->timing_active && s->timing_ncs_released &&
             (!s->timing_wire_complete ||
              s->timing_phase != GNW_H7B0_OSPI_PHASE_POSTAMBLE)) ||
            s->migrated_ncs_release_elapsed_ns > INT64_MAX) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: invalid OCTOSPI timing snapshot state\n",
                          __func__);
            return -EINVAL;
        }
        if (!s->timing_mode &&
            (s->timing_active || s->fifo_level ||
             s->timing_config_identity)) {
            return -EINVAL;
        }
    }

    timer_del(s->timing_timer);
    if (s->timing_mode) {
        s->phase_last_update_ns = now;
        s->ncs_release_ns = now -
            (int64_t)s->migrated_ncs_release_elapsed_ns;
        ospi_timing_update_status(s);
        if (s->timing_active && s->phase_remaining_quanta) {
            ospi_timing_schedule(s);
        } else if (s->timing_active &&
                   s->timing_phase == GNW_H7B0_OSPI_PHASE_DATA) {
            ospi_timing_advance(s);
        }
    }
    ospi_xip_gate_update(s);
    ospi_update_irq(s);
    return 0;
}

static const VMStateDescription vmstate_gnw_h7b0_ospi = {
    .name = TYPE_GNW_H7B0_OSPI,
    .version_id = 2,
    .minimum_version_id = 1,
    .pre_save = gnw_h7b0_ospi_pre_save,
    .post_load = gnw_h7b0_ospi_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, GnwH7B0OspiState, GNW_H7B0_OSPI_SIZE / 4),
        VMSTATE_UINT8(pending_instr, GnwH7B0OspiState),
        VMSTATE_UINT32(pending_addr, GnwH7B0OspiState),
        VMSTATE_UINT32(dr_pos, GnwH7B0OspiState),
        VMSTATE_BOOL(wel, GnwH7B0OspiState),
        VMSTATE_UINT32_EQUAL_V(timing_config_identity, GnwH7B0OspiState,
                               2, "OCTOSPI timing mode differs"),
        VMSTATE_UINT32_EQUAL_V(test_kernel_clock_hz, GnwH7B0OspiState,
                               2, "OCTOSPI test kernel clock differs"),
        VMSTATE_BOOL_V(timing_active, GnwH7B0OspiState, 2),
        VMSTATE_BOOL_V(timing_wire_complete, GnwH7B0OspiState, 2),
        VMSTATE_BOOL_V(timing_ncs_released, GnwH7B0OspiState, 2),
        VMSTATE_BOOL_V(timing_instruction_delivered, GnwH7B0OspiState, 2),
        VMSTATE_UINT8_V(timing_phase, GnwH7B0OspiState, 2),
        VMSTATE_UINT8_V(fifo_head, GnwH7B0OspiState, 2),
        VMSTATE_UINT8_V(fifo_level, GnwH7B0OspiState, 2),
        VMSTATE_UINT8_ARRAY_V(fifo, GnwH7B0OspiState,
                              GNW_H7B0_OSPI_FIFO_SIZE, 2),
        VMSTATE_UINT8_ARRAY_V(delivered_program, GnwH7B0OspiState,
                              GNW_H7B0_OSPI_PROGRAM_BUFFER_SIZE, 2),
        VMSTATE_UINT16_V(delivered_program_len, GnwH7B0OspiState, 2),
        VMSTATE_UINT32_V(kernel_clock_hz, GnwH7B0OspiState, 2),
        VMSTATE_UINT32_V(active_ccr, GnwH7B0OspiState, 2),
        VMSTATE_UINT32_V(active_tcr, GnwH7B0OspiState, 2),
        VMSTATE_UINT32_V(active_dcr1, GnwH7B0OspiState, 2),
        VMSTATE_UINT32_V(active_dcr2, GnwH7B0OspiState, 2),
        VMSTATE_UINT32_V(active_instr, GnwH7B0OspiState, 2),
        VMSTATE_UINT32_V(active_addr, GnwH7B0OspiState, 2),
        VMSTATE_UINT64_V(active_length, GnwH7B0OspiState, 2),
        VMSTATE_UINT64_V(active_delivered, GnwH7B0OspiState, 2),
        VMSTATE_UINT64_V(active_consumed, GnwH7B0OspiState, 2),
        VMSTATE_UINT64_V(phase_remaining_quanta, GnwH7B0OspiState, 2),
        VMSTATE_UINT64_V(phase_progress_num, GnwH7B0OspiState, 2),
        VMSTATE_UINT64_V(migrated_ncs_release_elapsed_ns,
                          GnwH7B0OspiState, 2),
        VMSTATE_INT64_V(phase_last_update_ns, GnwH7B0OspiState, 2),
        VMSTATE_END_OF_LIST()
    }
};

static const Property gnw_h7b0_ospi_properties[] = {
    DEFINE_PROP_UINT64("flash-size", GnwH7B0OspiState, flash_size,
                        OSPI_FLASH_DEFAULT_SIZE),
    DEFINE_PROP_UINT32("chip-index", GnwH7B0OspiState, chip_index, 6),
    DEFINE_PROP_BOOL("timing-mode", GnwH7B0OspiState, timing_mode, false),
};

static void gnw_h7b0_ospi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_gnw_h7b0_ospi;
    device_class_set_legacy_reset(dc, gnw_h7b0_ospi_reset);
    device_class_set_props(dc, gnw_h7b0_ospi_properties);
}

static const TypeInfo gnw_h7b0_ospi_info = {
    .name          = TYPE_GNW_H7B0_OSPI,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GnwH7B0OspiState),
    .instance_init = gnw_h7b0_ospi_init,
    .class_init    = gnw_h7b0_ospi_class_init,
};

static void gnw_h7b0_ospi_register_types(void)
{
    type_register_static(&gnw_h7b0_ospi_info);
}

type_init(gnw_h7b0_ospi_register_types)
