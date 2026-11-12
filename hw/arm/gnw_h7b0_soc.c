/*
 * STM32H7B0 SoC (Nintendo Game & Watch), Phase 0 skeleton
 *
 * Bare Cortex-M7 + RAM only, no peripherals yet. See gnw_h7b0_soc.h and
 * ../../docs/roadmap.md.
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
#include <glib/gstdio.h>
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/error-report.h"
#include "exec/icount.h"
#include "exec/replay-core.h"
#include "system/address-spaces.h"
#include "system/runstate.h"
#include "system/reset.h"
#include "hw/core/resettable.h"
#include "monitor/monitor.h"
#ifdef _WIN32
#include <windows.h>
#endif
#include "hw/arm/gnw_h7b0_soc.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "system/system.h"
#include "hw/misc/unimp.h"
#include "hw/misc/gnw_h7b0_regs_rcc.h"
#include "target/arm/cpu.h"
#include "hw/misc/gnw_h7b0_nor.h"
#include "hw/misc/gnw_h7b0_tamp.h"

/* Runtime receipts are admitted only after a separate review of the retained
 * FIT/freeze/HOLDOUT/replay chain.  The DMA-engine r1 review is recorded in
 * docs/timing-calibration/dma-engine-geometry-supplement-r1/.
 */
#define GNW_MDMA_AXI1_AUTHORIZED_RECEIPT_SHA256 \
    "c1db7d250d4bd947543883fd7f9cf4f7d621ffed98b95d22e6b6807e42aac9ce"
#define GNW_MDMA_AXI1_COMPARISON_ADMISSION_SHA256 \
    "b0ec6c7d4338d1ab2f74a133570c8283094801846ab39b7ed08e08fcfd0eb1e4"
#define GNW_MDMA_V37_BWM_FIT_ADMISSION_SHA256 \
    "26b6f9369987761bc91085adf11e57aa734f4f7ea58f485dc7a6494e7861bb8b"
#define GNW_DMA_ENGINE_R1_AUTHORIZED_RECEIPT_SHA256 \
    "a4cbac646ba40c21f0e345f5f83124cac638e5c42dcfbc782ec54559bddfeb8e"

static void gnw_h7b0_bus_v6_dma_tc(void *opaque, bool half,
                                    uint32_t m0ar, uint32_t ndtr)
{
    (void)m0ar;
    (void)ndtr;
    if (!half) {
        gnw_h7b0_mdma_bus_v6_request(opaque, 7);
    }
}

static void gnw_h7b0_soc_tamp_backup_domain_reset(void *opaque)
{
    gnw_h7b0_tamp_backup_domain_reset(GNW_H7B0_TAMP(opaque));
}

static void gnw_h7b0_bus_v6_tim_update(void *opaque, int instance)
{
    GnwH7B0State *s = opaque;

    if (instance == 4 && s->mdma.bus_v6_request_path) {
        /* TIM6 update is DMAMUX1 request 69. */
        gnw_h7b0_dma_peripheral_request(&s->dma, 69);
    }
}

/* This deliberately empty allowlist makes the prospective v2 profile
 * impossible to activate until a reviewed receipt and an exact coefficient
 * ABI are compiled into the same change. */
#define GNW_CPU_CACHE_DMA2D_V2_AUTHORIZED_RECEIPT_SHA256 ""

/* GnWManager's Zelda payload is placed at 0x30c3a8. Save that span plus the
 * 1,300-byte ITCM validation window; Mario's payload is at offset zero. */
#define GNW_UNLOCK_BASELINE_SIZE (0x30c3a8 + 1300)
#define GNW_STARTUP_HANDOFF_POLL_NS (10 * 1000000LL)
#define GNW_STARTUP_HANDOFF_MAX_ATTEMPTS 500

static void gnw_h7b0_cold_power_release(void *opaque)
{
    GnwH7B0State *s = opaque;

    gnw_h7b0_gpio_inject_button(&s->gpio,
                                gnw_h7b0_gpio_button_from_name("pwr"), false);
}

static void gnw_h7b0_cold_power_on(void *opaque)
{
    GnwH7B0State *s = opaque;

    gnw_h7b0_gpio_inject_button(&s->gpio,
                                gnw_h7b0_gpio_button_from_name("pwr"), true);
    timer_mod(s->cold_power_release_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 200 * 1000000LL);
}

static void gnw_h7b0_startup_handoff(void *opaque)
{
    GnwH7B0State *s = opaque;
    bool stock_itcm_ready;

    if (gnw_h7b0_gpio_try_boot_handoff(&s->gpio, &stock_itcm_ready) ||
        stock_itcm_ready) {
        return;
    }
    if (s->startup_handoff_attempts++ < GNW_STARTUP_HANDOFF_MAX_ATTEMPTS) {
        timer_mod(s->startup_handoff_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  GNW_STARTUP_HANDOFF_POLL_NS);
    }
}

static void hmp_gnw_cold_cycle(Monitor *mon, const QDict *qdict)
{
    gnw_h7b0_soc_cold_power_cycle();
    monitor_printf(mon, "GNW cold power cycle requested\n");
}

static void hmp_gnw_ui_reset(Monitor *mon, const QDict *qdict)
{
    gnw_h7b0_soc_ui_reset();
    monitor_printf(mon, "GNW UI reset requested\n");
}

void gnw_h7b0_soc_cold_power_cycle(void)
{
    Object *obj = object_resolve_path_type("/machine/soc", TYPE_GNW_H7B0_SOC,
                                           NULL);
    GnwH7B0State *s;
    uint8_t *itcm;

    if (!obj) {
        warn_report("gnw-h7b0: cold power cycle requested without a live SoC");
        return;
    }
    s = GNW_H7B0_SOC(obj);
    itcm = memory_region_get_ram_ptr(&s->itcm);

    /* The immutable boot path supplies this original ITCM image after power
     * is restored. Preserve the real loader-provided bytes once so later
     * cold cycles do not reuse the decoded unlock payload. */
    if (!s->cold_itcm_seed) {
        s->cold_itcm_seed = g_memdup2(itcm, memory_region_size(&s->itcm));
    }

    /* Full power removal clears volatile SRAM banks. Internal/external flash,
     * option bytes, and the factory UID are nonvolatile and retained. */
#define CLEAR_VOLATILE_RAM(field) \
    memset(memory_region_get_ram_ptr(&s->field), 0, \
           memory_region_size(&s->field))
    CLEAR_VOLATILE_RAM(itcm);
    CLEAR_VOLATILE_RAM(dtcm);
    CLEAR_VOLATILE_RAM(axisram1);
    CLEAR_VOLATILE_RAM(ahbsram1);
    CLEAR_VOLATILE_RAM(ahbsram2);
    CLEAR_VOLATILE_RAM(srdsram);
    CLEAR_VOLATILE_RAM(bkpsram);
#undef CLEAR_VOLATILE_RAM
    memcpy(itcm, s->cold_itcm_seed, memory_region_size(&s->itcm));

    /* Reset the CPU and all peripheral registers, then reapply power and
     * synthesize the user's POWER press after 750 ms of guest time. */
    qemu_system_reset_request(SHUTDOWN_CAUSE_HOST_UI);
    timer_mod(s->cold_power_on_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 750 * 1000000LL);
}

static void gnw_h7b0_soc_reset_exit(Object *obj, ResetType type)
{
    GnwH7B0State *s = GNW_H7B0_SOC(obj);
    ARMCPU *cpu = s->armv7m.cpu;
    CPUARMState *env;

    (void)type;

    if (s->initial_boot_handoff_pending) {
        s->initial_boot_handoff_pending = false;
        s->startup_handoff_attempts = 0;
        timer_mod(s->startup_handoff_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1);
    }

    if (!s->ui_reset_to_fault_pending) {
        return;
    }
    s->ui_reset_to_fault_pending = false;

    /* The physical RDP1 device's reset-halt/one-step sequence was captured
     * through GnWManager/OpenOCD: reset leaves the core at the inaccessible
     * vector-table sentinel (PC=FFFFFFFE, MSP=FFFFFFFC), and the first step
     * raises IACCVIOL and escalates it to a vector-table HardFault. Preserve
     * that architectural state on the UI's warm Reset. Cold Power Cycle has
     * a separate path and must still boot the staged external-flash payload.
     */
    if (!cpu || s->flash_r.rdp_value != 0x55) {
        return;
    }

    env = &cpu->env;
    memset(env->regs, 0, sizeof(env->regs));
    env->regs[13] = 0xfffffffc;
    env->regs[14] = 0xffffffff;
    env->regs[15] = 0xfffffffe;
    env->NF = 0;
    env->ZF = 1; /* QEMU stores ZF inverted; this leaves xPSR.Z clear. */
    env->CF = 0;
    env->VF = 0;
    env->QF = 0;
    env->GE = 0;
    env->thumb = true;
    env->v7m.vecbase[M_REG_NS] = FLASH_BANK1_BASE_ADDRESS;
    env->v7m.other_sp = 0;
    env->v7m.exception = 3; /* HardFault active */
    env->v7m.cfsr[M_REG_NS] = 1; /* IACCVIOL */
    env->v7m.hfsr = (1U << 30) | (1U << 1); /* FORCED | VECTTBL */
    env->v7m.dfsr = 9; /* HALTED | VCATCH, as reported by OpenOCD */
    s->rcc.regs[GNW_H7B0_RCC_RSR >> 2] = 0x01e80000;
    cpu_reset_interrupt(CPU(cpu), CPU_INTERRUPT_EXITTB);
    cpu->parent_obj.halted = true;
}

void gnw_h7b0_soc_ui_reset(void)
{
    Object *obj = object_resolve_path_type("/machine/soc", TYPE_GNW_H7B0_SOC,
                                           NULL);

    if (obj) {
        GnwH7B0State *s = GNW_H7B0_SOC(obj);
        s->ui_reset_to_fault_pending = true;
    }
    qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
}

static void gnw_h7b0_soc_initfn(Object *obj)
{
    GnwH7B0State *s = GNW_H7B0_SOC(obj);

    object_initialize_child(obj, "armv7m", &s->armv7m, TYPE_ARMV7M);
    object_initialize_child(obj, "rcc", &s->rcc, TYPE_GNW_H7B0_RCC);
    object_initialize_child(obj, "pwr", &s->pwr, TYPE_GNW_H7B0_PWR);
    object_initialize_child(obj, "octospi1", &s->octospi1, TYPE_GNW_H7B0_OSPI);
    object_initialize_child(obj, "octospi2", &s->octospi2, TYPE_GNW_H7B0_OSPI);
    object_initialize_child(obj, "adc", &s->adc, TYPE_GNW_H7B0_ADC);
    object_initialize_child(obj, "lptim1", &s->lptim1, TYPE_GNW_H7B0_LPTIM1);
    object_initialize_child(obj, "ltdc", &s->ltdc, TYPE_GNW_H7B0_LTDC);
    object_initialize_child(obj, "dma2d", &s->dma2d, TYPE_GNW_H7B0_DMA2D);
    object_initialize_child(obj, "spi2", &s->spi2, TYPE_GNW_H7B0_SPI);
    object_initialize_child(obj, "spi1", &s->spi1, TYPE_GNW_H7B0_SPI);
    /* SPI1 is the "Tim" dedicated-pin SD-card mod path; SPI2 is not
     * (LCD-panel init commands) -- see gnw_h7b0_spi.h. */
    qdev_prop_set_bit(DEVICE(&s->spi1), "sd-card", true);
    object_initialize_child(obj, "rtc", &s->rtc, TYPE_GNW_H7B0_RTC);
    object_initialize_child(obj, "crc", &s->crc, TYPE_GNW_H7B0_CRC);
    object_initialize_child(obj, "hash", &s->hash, TYPE_GNW_H7B0_HASH);
    object_initialize_child(obj, "mdma", &s->mdma, TYPE_GNW_H7B0_MDMA);
    object_initialize_child(obj, "rng", &s->rng, TYPE_GNW_H7B0_RNG);
    object_initialize_child(obj, "gpio", &s->gpio, TYPE_GNW_H7B0_GPIO);
    object_initialize_child(obj, "dbgmcu", &s->dbgmcu, TYPE_GNW_H7B0_DBGMCU);
    object_initialize_child(obj, "dwt", &s->dwt, TYPE_GNW_H7B0_DWT);
    object_initialize_child(obj, "flash_r", &s->flash_r, TYPE_GNW_H7B0_FLASH_R);
    object_initialize_child(obj, "fmc", &s->fmc, TYPE_GNW_H7B0_FMC);
    object_initialize_child(obj, "crs", &s->crs, TYPE_GNW_H7B0_CRS);
    object_initialize_child(obj, "octospim", &s->octospim, TYPE_GNW_H7B0_OCTOSPIM);
    object_initialize_child(obj, "otfdec1", &s->otfdec1, TYPE_GNW_H7B0_OTFDEC);
    object_initialize_child(obj, "otfdec2", &s->otfdec2, TYPE_GNW_H7B0_OTFDEC);
    object_initialize_child(obj, "cryp", &s->cryp, TYPE_GNW_H7B0_CRYP);
    object_initialize_child(obj, "exti", &s->exti, TYPE_GNW_H7B0_EXTI);
    object_initialize_child(obj, "syscfg", &s->syscfg, TYPE_GNW_H7B0_SYSCFG);
    object_initialize_child(obj, "dma", &s->dma, TYPE_GNW_H7B0_DMA);
    object_initialize_child(obj, "sai1", &s->sai1, TYPE_GNW_H7B0_SAI1);
    object_initialize_child(obj, "dac1", &s->dac1, TYPE_GNW_H7B0_DAC);
    object_initialize_child(obj, "dac2", &s->dac2, TYPE_GNW_H7B0_DAC);
    object_initialize_child(obj, "tim1", &s->tim1, TYPE_GNW_H7B0_TIM1);
    object_initialize_child(obj, "jpeg", &s->jpeg, TYPE_GNW_H7B0_JPEG);
    object_initialize_child(obj, "tamp", &s->tamp, TYPE_GNW_H7B0_TAMP);
    object_initialize_child(obj, "wwdg", &s->wwdg, TYPE_GNW_H7B0_WWDG);
    object_initialize_child(obj, "tim2", &s->tim2, TYPE_GNW_H7B0_TIM2);
    object_initialize_child(obj, "iwdg", &s->iwdg, TYPE_GNW_H7B0_IWDG);
    object_initialize_child(obj, "lpuart1", &s->lpuart1, TYPE_GNW_H7B0_LPUART1);
    object_initialize_child(obj, "usart1", &s->usart1, TYPE_GNW_H7B0_USART1);


    s->sysclk = qdev_init_clock_in(DEVICE(s), "sysclk", NULL, NULL, 0);
    s->systick_refclk = clock_new(obj, "systick-refclk");
    clock_set_hz(s->systick_refclk, 8000000);
}

/* Extend a flash image explicitly with the erased NOR value, never sparse
 * zero-filled bytes. Images larger than the physical region are rejected
 * instead of silently hiding their tail behind the guest mapping.
 */
static bool gnw_h7b0_prepare_flash_image(const char *path, uint64_t size,
                                         Error **errp)
{
    FILE *f = g_fopen(path, "r+b");
    uint8_t fill[64 * 1024];
    long current;

    if (!f) {
        f = g_fopen(path, "w+b");
    }
    if (!f) {
        error_setg_errno(errp, errno, "cannot open flash image '%s'", path);
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (current = ftell(f)) < 0) {
        error_setg_errno(errp, errno, "cannot determine size of flash image '%s'",
                         path);
        fclose(f);
        return false;
    }
    if ((uint64_t)current > size) {
        error_setg(errp, "flash image '%s' is %ld bytes, larger than the "
                   "selected region (%" PRIu64 " bytes)", path, current, size);
        fclose(f);
        return false;
    }
    if ((uint64_t)current < size) {
        memset(fill, 0xFF, sizeof(fill));
        while ((uint64_t)current < size) {
            size_t n = MIN((uint64_t)sizeof(fill), size - (uint64_t)current);
            if (fwrite(fill, 1, n, f) != n) {
                error_setg_errno(errp, errno, "cannot pad flash image '%s' with 0xFF",
                                 path);
                fclose(f);
                return false;
            }
            current += n;
        }
        if (fflush(f) != 0) {
            error_setg_errno(errp, errno, "cannot flush padded flash image '%s'",
                             path);
            fclose(f);
            return false;
        }
    }
    fclose(f);
    return true;
}

/*
 * Backs `field` directly by `image_path` (created/truncated to `size` if it
 * doesn't already exist yet) when non-NULL/non-empty, so guest writes land
 * in that file live instead of anonymous memory -- see the bank1_image/
 * bank2_image/extflash_image doc comment in gnw_h7b0_soc.h. Falls back to
 * plain anonymous RAM (today's behavior, seeded only via `-device loader`)
 * otherwise.
 *
 * memory_region_init_ram_from_file() is CONFIG_POSIX-only in QEMU itself
 * (it's mmap-based) -- genuinely unavailable on Windows via QEMU's own
 * generic memory-backend machinery (backends/hostmem-file.c does the same
 * #ifndef CONFIG_POSIX: error_setg(...) refusal). On _WIN32 we get real
 * persistence anyway via a direct CreateFileMapping()/MapViewOfFile() host
 * pointer, wrapped as guest RAM with memory_region_init_ram_ptr() -- the
 * same portable "wrap an existing host pointer" primitive this codebase
 * already relies on for gnw_h7b0_otfdec.c's decrypt-overlay buffers. Only
 * a truly unknown non-POSIX, non-Windows host falls back to ephemeral RAM,
 * loudly (warn_report(), not silence): persistence quietly not working
 * would be a much worse surprise than an explicit "this won't persist".
 */
#ifdef _WIN32
static bool gnw_h7b0_init_ram_from_file_win32(MemoryRegion *mr, Object *owner,
                                               const char *name, uint64_t size,
                                               const char *image_path,
                                               Error **errp)
{
    HANDLE file = CreateFileA(image_path, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ, NULL, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        error_setg(errp, "%s: CreateFileA(\"%s\") failed (error %lu)",
                   name, image_path, (unsigned long)GetLastError());
        return false;
    }

    LARGE_INTEGER cur_size;
    if (!GetFileSizeEx(file, &cur_size)) {
        error_setg(errp, "%s: GetFileSizeEx(\"%s\") failed (error %lu)",
                   name, image_path, (unsigned long)GetLastError());
        CloseHandle(file);
        return false;
    }
    /* Created/truncated to `size` if it doesn't already exist yet or is
     * shorter -- matches memory_region_init_ram_from_file()'s documented
     * behavior on POSIX. Never shrink an existing longer file. */
    if ((uint64_t)cur_size.QuadPart < size) {
        LARGE_INTEGER new_size;
        new_size.QuadPart = (LONGLONG)size;
        if (!SetFilePointerEx(file, new_size, NULL, FILE_BEGIN) ||
            !SetEndOfFile(file)) {
            error_setg(errp, "%s: extending \"%s\" to %" PRIu64
                       " bytes failed (error %lu)", name, image_path, size,
                       (unsigned long)GetLastError());
            CloseHandle(file);
            return false;
        }
    }

    HANDLE mapping = CreateFileMappingA(file, NULL, PAGE_READWRITE,
                                         (DWORD)(size >> 32),
                                         (DWORD)(size & 0xFFFFFFFFu), NULL);
    if (mapping == NULL) {
        error_setg(errp, "%s: CreateFileMappingA(\"%s\") failed (error %lu)",
                   name, image_path, (unsigned long)GetLastError());
        CloseHandle(file);
        return false;
    }

    void *ptr = MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE,
                               0, 0, size);
    if (ptr == NULL) {
        error_setg(errp, "%s: MapViewOfFile(\"%s\") failed (error %lu)",
                   name, image_path, (unsigned long)GetLastError());
        CloseHandle(mapping);
        CloseHandle(file);
        return false;
    }

    /* The mapping/file HANDLEs are intentionally never closed here: the
     * view stays valid as long as they're open, this region needs to
     * stay live for the device's/process's whole lifetime (same "leak
     * until process exit" convention gnw_h7b0_otfdec.c already uses for
     * its own host-pointer-backed regions), and Windows keeps the
     * underlying mapping alive via the still-open view regardless. */
    memory_region_init_ram_ptr(mr, owner, name, size, ptr);
    return true;
}
#endif

static bool gnw_h7b0_init_ram_or_file(MemoryRegion *mr, Object *owner,
                                       const char *name, uint64_t size,
                                       const char *image_path, Error **errp)
{
    if (image_path != NULL && image_path[0] != '\0') {
        /* Prepare explicitly because both the POSIX mmap backend and
         * Windows file extension otherwise expose new bytes as zeros. */
        struct stat before;
        bool existed = stat(image_path, &before) == 0;
        if (!gnw_h7b0_prepare_flash_image(image_path, size, errp)) {
            return false;
        }
        if (!existed) {
            info_report("%s: created blank flash image '%s' with 0xFF contents",
                        name, image_path);
        } else if ((uint64_t)before.st_size < size) {
            info_report("%s: padded flash image '%s' from %" PRIu64
                        " to %" PRIu64 " bytes with 0xFF",
                        name, image_path, (uint64_t)before.st_size, size);
        }
#ifdef CONFIG_POSIX
        return memory_region_init_ram_from_file(mr, owner, name, size, 0,
                                                 RAM_SHARED, image_path, 0,
                                                 errp);
#elif defined(_WIN32)
        return gnw_h7b0_init_ram_from_file_win32(mr, owner, name, size,
                                                  image_path, errp);
#else
        warn_report("persistent flash-image backing isn't supported on "
                    "this host -- \"%s\" will NOT persist writes, falling "
                    "back to ephemeral RAM for %s", image_path, name);
#endif
    }
    memory_region_init_ram(mr, owner, name, size, errp);
    if (!errp || *errp == NULL) {
        memset(memory_region_get_ram_ptr(mr), 0xFF, size);
    }
    return !errp || *errp == NULL;
}

static uint8_t *gnw_h7b0_load_unlock_baseline(const char *image_path,
                                              const uint8_t *extflash,
                                              uint64_t extflash_size,
                                              bool refresh, Error **errp)
{
    uint8_t *baseline = g_malloc(GNW_UNLOCK_BASELINE_SIZE);
    /* The Zelda baseline span exceeds Mario's 1 MiB NOR mapping. Keep the
     * sidecar format stable, but never read beyond the selected chip. */
    memset(baseline, 0xFF, GNW_UNLOCK_BASELINE_SIZE);
    memcpy(baseline, extflash, MIN(extflash_size, GNW_UNLOCK_BASELINE_SIZE));

    if (!refresh && image_path && image_path[0]) {
        char *path = g_strdup_printf("%s.gnw-unlock-baseline", image_path);
        gchar *contents = NULL;
        gsize length = 0;
        GError *local_err = NULL;

        if (g_file_get_contents(path, &contents, &length, &local_err)) {
            if (length == GNW_UNLOCK_BASELINE_SIZE) {
                memcpy(baseline, contents, length);
                g_free(contents);
                g_free(path);
                return baseline;
            }
            g_free(contents);
        } else if (!g_error_matches(local_err, G_FILE_ERROR,
                                   G_FILE_ERROR_NOENT)) {
            error_setg(errp, "cannot read unlock baseline '%s': %s", path,
                       local_err->message);
            g_clear_error(&local_err);
            g_free(path);
            g_free(baseline);
            return NULL;
        }
        g_clear_error(&local_err);

        if (!g_file_set_contents(path, (const gchar *)baseline,
                                 GNW_UNLOCK_BASELINE_SIZE, &local_err)) {
            error_setg(errp, "cannot save unlock baseline '%s': %s", path,
                       local_err->message);
            g_clear_error(&local_err);
            g_free(path);
            g_free(baseline);
            return NULL;
        }
        g_free(path);
        return baseline;
    }

    return baseline;
}

static void gnw_h7b0_mdma_clock_changed(
    void *opaque, GnwH7B0RccEngineClockInfo old_info,
    GnwH7B0RccEngineClockInfo new_info)
{
    gnw_h7b0_mdma_set_clock_info(opaque, new_info.known,
                               new_info.reference_hz, new_info.enabled);
}

static void gnw_h7b0_dma2d_clock_changed(
    void *opaque, GnwH7B0RccEngineClockInfo old_info,
    GnwH7B0RccEngineClockInfo new_info)
{
    gnw_h7b0_dma2d_set_clock_info(opaque, new_info.known,
                                new_info.reference_hz, new_info.enabled);
}

static uint32_t gnw_h7b0_bus_masters_for_range(void *opaque,
                                                uint64_t address,
                                                unsigned size,
                                                int64_t guest_time_ns)
{
    GnwH7B0State *s = opaque;

    return gnw_h7b0_mdma_contends_for_range(&s->mdma, address, size) |
           gnw_h7b0_mdma_large_contends_for_range(&s->mdma, address, size) |
           gnw_h7b0_mdma_v37_bwm_contends_for_range(
               &s->mdma, address, size, guest_time_ns) |
           gnw_h7b0_dma2d_contends_for_range(&s->dma2d, address, size);
}

/* Exact scope of the retained unseen-size OSPI measurement. This callback is
 * queried on each candidate CPU byte load so clock/controller changes fail
 * closed without a duplicated shadow of RCC or OCTOSPI state. */
static bool gnw_h7b0_xip_unseen_scope_valid(void *opaque)
{
    GnwH7B0State *s = opaque;
    GnwH7B0OspiState *o = &s->octospi1;
    uint32_t cr = o->regs[GNW_H7B0_OSPI_CR >> 2];
    uint32_t dcr1 = o->regs[GNW_H7B0_OSPI_DCR1 >> 2];
    uint32_t ccr = o->regs[GNW_H7B0_OSPI_CCR >> 2];
    const GnwNorChip *chip;

    if (o->chip_index >= GNW_NOR_CHIP_COUNT) {
        return false;
    }
    chip = &gnw_nor_chips[o->chip_index];
    return gnw_h7b0_rcc_get_hclk_hz(&s->rcc) == 280000000 &&
        gnw_h7b0_rcc_get_octospi_kernel_hz(&s->rcc) == 280000000 &&
        o->flash_size == 64ULL * 1024 * 1024 &&
        chip->jedec[0] == 0xc2 && chip->jedec[1] == 0x25 &&
        chip->jedec[2] == 0x3a && cr == 0x30000301 &&
        dcr1 == 0x011b0108 &&
        o->regs[GNW_H7B0_OSPI_DCR2 >> 2] == 0 &&
        o->regs[0x10 >> 2] == 0 && o->regs[0x14 >> 2] == 0 &&
        ccr == 0x01002101 &&
        o->regs[GNW_H7B0_OSPI_TCR >> 2] == 0x8;
}

/* Read-only model-derived clock diagnostics for the frozen OSPI replay.
 * Keeping these on the SoC makes the replay verify the same RCC getters used
 * by the live profile predicate. */
static void gnw_h7b0_soc_get_hclk_hz(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp)
{
    GnwH7B0State *s = GNW_H7B0_SOC(obj);
    uint64_t value = gnw_h7b0_rcc_get_hclk_hz(&s->rcc);

    (void)opaque;
    visit_type_uint64(v, name, &value, errp);
}

static void gnw_h7b0_soc_get_octospi_kernel_hz(Object *obj, Visitor *v,
                                              const char *name, void *opaque,
                                              Error **errp)
{
    GnwH7B0State *s = GNW_H7B0_SOC(obj);
    uint64_t value = gnw_h7b0_rcc_get_octospi_kernel_hz(&s->rcc);

    (void)opaque;
    visit_type_uint64(v, name, &value, errp);
}

static bool gnw_h7b0_cpu_bwm_comparison_enabled(GnwH7B0State *s)
{
    Object *cpu = OBJECT(s->armv7m.cpu);

    return object_property_get_bool(cpu,
               "x-gnw-m7-bus-contention-v37-bwm-comparison", &error_abort) ||
           object_property_get_bool(cpu,
               "x-gnw-m7-bus-contention-v46-bwm-17over4", &error_abort);
}

static void gnw_h7b0_soc_realize(DeviceState *dev_soc, Error **errp)
{
    GnwH7B0State *s = GNW_H7B0_SOC(dev_soc);
    MemoryRegion *system_memory = get_system_memory();
    DeviceState *armv7m;
    Error *err = NULL;
    const GnwNorChip *nor_chip = gnw_nor_chip_by_key(
        s->extflash_chip ? s->extflash_chip : GNW_NOR_DEFAULT_CHIP_KEY);

    if (!nor_chip) {
        error_setg(errp, "unknown extflash chip '%s'",
                   s->extflash_chip ? s->extflash_chip : "(null)");
        return;
    }
    uint8_t uid_bytes[12];
    if (s->sd_wiring && strcmp(s->sd_wiring, "spi1") &&
        strcmp(s->sd_wiring, "ospi")) {
        error_setg(errp, "sd-wiring must be spi1 or ospi");
        return;
    }
    if (s->device_uid) {
        if (strlen(s->device_uid) != sizeof(uid_bytes) * 2) {
            error_setg(errp, "UID must contain exactly 24 hexadecimal digits");
            return;
        }
        for (size_t i = 0; i < sizeof(uid_bytes) * 2; i++) {
            if (!g_ascii_isxdigit(s->device_uid[i])) {
                error_setg(errp, "UID must contain exactly 24 hexadecimal digits");
                return;
            }
        }
        for (size_t i = 0; i < sizeof(uid_bytes); i++) {
            uid_bytes[i] = g_ascii_xdigit_value(s->device_uid[i * 2]) * 16 +
                           g_ascii_xdigit_value(s->device_uid[i * 2 + 1]);
        }
    }
    s->extflash_size = nor_chip->size_bytes;

    if (s->cpu_cache_dma2d_v2_profile) {
        if (!s->cpu_cache_dma2d_v2_runtime_receipt_sha256 ||
            !GNW_CPU_CACHE_DMA2D_V2_AUTHORIZED_RECEIPT_SHA256[0] ||
            strcmp(s->cpu_cache_dma2d_v2_runtime_receipt_sha256,
                   GNW_CPU_CACHE_DMA2D_V2_AUTHORIZED_RECEIPT_SHA256)) {
            error_setg(errp, "the CPU/memory/cache/DMA2D v2 profile requires "
                       "an exact reviewed runtime receipt; no receipt is "
                       "authorized");
            return;
        }
        if (!s->x_modeled_timing) {
            error_setg(errp, "the CPU/memory/cache/DMA2D v2 profile requires "
                       "x-modeled-timing=on");
            return;
        }
        /* No coefficient installation belongs here until the reviewed model
         * config has an exact C ABI.  The empty digest above keeps this path
         * unreachable in the meantime. */
    } else if (s->cpu_cache_dma2d_v2_runtime_receipt_sha256) {
        error_setg(errp, "x-cpu-cache-dma2d-v2-runtime-receipt-sha256 "
                   "requires x-cpu-cache-dma2d-v2-profile=on");
        return;
    }

    if (s->x_modeled_timing) {
        if (use_icount != ICOUNT_PRECISE ||
            (icount_sleep_enabled() && !icount_alignment_enabled())) {
            error_setg(errp, "gnw-h7b0 experimental modeled timing requires "
                       "precise TCG -icount with sleep disabled or "
                       "realtime alignment enabled "
                       "(-icount shift=0,sleep=off or "
                       "shift=0,align=on,sleep=on)");
            return;
        }
        if (s->x_timing_test_cycles_per_insn || s->x_timing_test_half_rate) {
            error_setg(errp, "gnw-h7b0 experimental modeled timing cannot be "
                       "combined with x-timing-test-cycle properties");
            return;
        }
        if (!icount_enable_experimental_m7_cycle_model()) {
            error_setg(errp, "gnw-h7b0 experimental modeled timing conflicts "
                       "with another icount cycle model");
            return;
        }
        s->timing_mode = true;
        qdev_prop_set_bit(DEVICE(&s->armv7m),
                          "x-gnw-m7-cycle-model", true);
    }

    if (!clock_has_source(s->sysclk)) {
        error_setg(errp, "sysclk clock must be wired up by the board code");
        return;
    }
    if (s->timing_mode && use_icount != ICOUNT_PRECISE) {
        error_setg(errp, "gnw-h7b0 timing mode requires precise TCG -icount "
                   "(for example: -icount shift=0)");
        return;
    }
    if (s->timing_mode && replay_mode != REPLAY_MODE_NONE) {
        error_setg(errp, "gnw-h7b0 timing mode does not support record/replay");
        return;
    }
    if (s->timing_mode) {
        qdev_prop_set_bit(DEVICE(&s->dwt), "timing-mode", true);
    }
    if (s->x_timing_test_cycles_per_insn && s->x_timing_test_half_rate) {
        error_setg(errp, "x-timing-test-cycle properties are mutually exclusive");
        return;
    }
    if (s->x_timing_test_cycles_per_insn || s->x_timing_test_half_rate) {
        if (!s->timing_mode) {
            error_setg(errp, "x-timing-test-cycle properties require "
                       "gnw-h7b0 timing mode");
            return;
        }
        if (s->x_timing_test_half_rate) {
            icount_set_test_half_rate();
        } else {
            icount_set_test_cycle_scale(s->x_timing_test_cycles_per_insn);
        }
    }

#define INIT_RAM_REGION(field, name, base, size) \
    do { \
        memory_region_init_ram(&s->field, OBJECT(dev_soc), name, size, &err); \
        if (err != NULL) { \
            error_propagate(errp, err); \
            return; \
        } \
        memory_region_add_subregion(system_memory, base, &s->field); \
    } while (0)

#define INIT_RAM_OR_FILE_REGION(field, name, base, size, image_path) \
    do { \
        if (!gnw_h7b0_init_ram_or_file(&s->field, OBJECT(dev_soc), name, \
                                        size, (image_path), &err)) { \
            error_propagate(errp, err); \
            return; \
        } \
        memory_region_add_subregion(system_memory, base, &s->field); \
    } while (0)

    INIT_RAM_REGION(itcm, "GNW_H7B0.itcm", ITCM_BASE_ADDRESS, ITCM_SIZE);
    INIT_RAM_REGION(dtcm, "GNW_H7B0.dtcm", DTCM_BASE_ADDRESS, DTCM_SIZE);
    /*
     * Real H7B0 AXI SRAM is one genuinely contiguous ~1MB block (RM0455);
     * AXISRAM1/2/3 are just documentation/sizing subdivisions of it, not
     * separate hardware blocks -- confirmed no gap between them
     * (AXISRAM1_BASE+SIZE == AXISRAM2_BASE, and likewise for 2->3).
     * Modeling them as three separate MemoryRegion objects made any
     * guest buffer straddling one of those artificial boundaries
     * unreachable via a single memory_region_find() call -- found via
     * live profiling when the LTDC non-VBR fallback's RAM dirty-bitmap
     * tracking (gnw_h7b0_ltdc_fb_dirty_check_and_clear()) silently
     * failed to bind on every call for a framebuffer that happened to
     * span AXISRAM1/AXISRAM2. One region for the whole span is a more
     * faithful model of the real contiguous hardware, not just a
     * workaround, and fixes this for any future code with the same
     * assumption.
     */
    INIT_RAM_REGION(axisram1, "GNW_H7B0.axisram1", AXISRAM1_BASE_ADDRESS,
                     AXISRAM1_SIZE + AXISRAM2_SIZE + AXISRAM3_SIZE);
    INIT_RAM_REGION(ahbsram1, "GNW_H7B0.ahbsram1", AHBSRAM1_BASE_ADDRESS,
                     AHBSRAM1_SIZE);
    INIT_RAM_REGION(ahbsram2, "GNW_H7B0.ahbsram2", AHBSRAM2_BASE_ADDRESS,
                     AHBSRAM2_SIZE);
    INIT_RAM_REGION(srdsram, "GNW_H7B0.srdsram", SRDSRAM_BASE_ADDRESS,
                     SRDSRAM_SIZE);
    INIT_RAM_REGION(bkpsram, "GNW_H7B0.bkpsram", BKPSRAM_BASE_ADDRESS,
                     BKPSRAM_SIZE);
    INIT_RAM_OR_FILE_REGION(flash_bank1, "GNW_H7B0.flash_bank1",
                             FLASH_BANK1_BASE_ADDRESS, FLASH_BANK_SIZE,
                             s->bank1_image);
    INIT_RAM_OR_FILE_REGION(flash_bank2, "GNW_H7B0.flash_bank2",
                             FLASH_BANK2_BASE_ADDRESS, FLASH_BANK_SIZE,
                             s->bank2_image);
    INIT_RAM_OR_FILE_REGION(extflash, "GNW_H7B0.extflash", EXTFLASH_BASE_ADDRESS,
                             s->extflash_size, s->extflash_image);
    INIT_RAM_REGION(uid, "GNW_H7B0.uid", UID_BASE_ADDRESS, UID_SIZE);

    /*
     * Unimplemented devices to prevent BusFaults on gnwmanager payload,
     * a not-yet-modeled peripheral read/write, or a reset-register
     * snapshot tool (scripts/snapshot_registers.py) walking every
     * STM32H7B0.svd peripheral. Every SVD peripheral base address not
     * already covered by a real device model below is stubbed here so
     * accesses log instead of faulting.
     */
    create_unimplemented_device("SPI3", 0x40003c00, 0x400);
    create_unimplemented_device("SPDIFRX", 0x40004000, 0x400);
    create_unimplemented_device("USART2", 0x40004400, 0x400);
    create_unimplemented_device("USART3", 0x40004800, 0x400);
    create_unimplemented_device("UART4", 0x40004c00, 0x400);
    create_unimplemented_device("UART5", 0x40005000, 0x400);
    create_unimplemented_device("I2C1", 0x40005400, 0x400);
    create_unimplemented_device("I2C2", 0x40005800, 0x400);
    create_unimplemented_device("I2C3", 0x40005c00, 0x400);
    create_unimplemented_device("CEC", 0x40006c00, 0x400);
    create_unimplemented_device("UART7", 0x40007800, 0x400);
    create_unimplemented_device("UART8", 0x40007c00, 0x400);
    create_unimplemented_device("SWPMI", 0x40008800, 0x400);
    create_unimplemented_device("OPAMP", 0x40009000, 0x400);
    create_unimplemented_device("MDIOS", 0x40009400, 0x400);
    create_unimplemented_device("TT_FDCAN", 0x4000a000, 0x400);
    create_unimplemented_device("FDCAN", 0x4000a400, 0x400);
    create_unimplemented_device("CAN_CCU", 0x4000a800, 0x400);
    create_unimplemented_device("TIM8", 0x40010400, 0x400);
    create_unimplemented_device("USART6", 0x40011400, 0x400);
    create_unimplemented_device("USART9", 0x40011800, 0x400);
    create_unimplemented_device("USART10", 0x40011c00, 0x400);
    create_unimplemented_device("SPI4", 0x40013400, 0x400);
    create_unimplemented_device("TIM15", 0x40014000, 0x400);
    create_unimplemented_device("TIM16", 0x40014400, 0x400);
    create_unimplemented_device("TIM17", 0x40014800, 0x400);
    create_unimplemented_device("SPI5", 0x40015000, 0x400);
    create_unimplemented_device("SAI2", 0x40015c00, 0x400);
    create_unimplemented_device("HRTIM_Master", 0x40017400, 0x80);
    create_unimplemented_device("HRTIM_TIMA", 0x40017480, 0x80);
    create_unimplemented_device("HRTIM_TIMB", 0x40017500, 0x80);
    create_unimplemented_device("HRTIM_TIMC", 0x40017580, 0x80);
    create_unimplemented_device("HRTIM_TIMD", 0x40017600, 0x80);
    create_unimplemented_device("HRTIM_TIME", 0x40017680, 0x80);
    create_unimplemented_device("HRTIM_Common", 0x40017780, 0x80);
    create_unimplemented_device("DFSDM1", 0x40017800, 0x4bc);
    create_unimplemented_device("DMAMUX1", 0x40020800, 0x400);
    create_unimplemented_device("OTG1_HS_GLOBAL", 0x40040000, 0x400);
    create_unimplemented_device("OTG1_HS_HOST", 0x40040400, 0x400);
    create_unimplemented_device("OTG1_HS_DEVICE", 0x40040800, 0x400);
    create_unimplemented_device("OTG1_HS_PWRCLK", 0x40040e00, 0x3f200);
    create_unimplemented_device("DCMI", 0x48020000, 0x400);
    create_unimplemented_device("PSSI", 0x48020400, 0x6b);
    create_unimplemented_device("HSEM", 0x48020800, 0x400);
    create_unimplemented_device("SDMMC2", 0x48022400, 0x400);
    create_unimplemented_device("DELAY_Block_SDMMC2", 0x48022800, 0x400);
    create_unimplemented_device("BDMA1", 0x48022c00, 0x400);
    create_unimplemented_device("AXI", 0x51000000, 0x100000);
    create_unimplemented_device("Delay_Block_OCTOSPI1", 0x52006000, 0x400);
    create_unimplemented_device("SDMMC1", 0x52007000, 0x3fd);
    create_unimplemented_device("DELAY_Block_SDMMC1", 0x52008000, 0x400);
    create_unimplemented_device("RAMECC", 0x52009000, 0x400);
    create_unimplemented_device("Delay_Block_OCTOSPI2", 0x5200b000, 0x400);
    create_unimplemented_device("SPI6", 0x58001400, 0x400);
    create_unimplemented_device("I2C4", 0x58001c00, 0x400);
    create_unimplemented_device("LPTIM2", 0x58002400, 0x400);
    create_unimplemented_device("LPTIM3", 0x58002800, 0x400);
    create_unimplemented_device("COMP1", 0x58003800, 0x400);
    create_unimplemented_device("VREFBUF", 0x58003c00, 0x400);
    create_unimplemented_device("DFSDM2", 0x58006c00, 0x400);
    create_unimplemented_device("BDMA2", 0x58025400, 0x400);
    create_unimplemented_device("DMAMUX2", 0x58025800, 0x400);

    {
        /*
         * Seed a fixed synthetic 96-bit UID (HAL_GetUIDw0/1/2() read
         * this as three consecutive words at UID_BASE) -- see
         * gnw_h7b0_soc.h's UID_BASE_ADDRESS comment for why this
         * region exists at all. Value is arbitrary; nothing depends
         * on matching real silicon.
         */
        uint32_t *uid_ptr = memory_region_get_ram_ptr(&s->uid);

        uid_ptr[0] = 0x47575145; /* "GWQE" */
        uid_ptr[1] = 0x4d55004d; /* "MU\0M" */
        uid_ptr[2] = 0x00000001;
        if (s->device_uid) {
            memcpy(uid_ptr, uid_bytes, sizeof(uid_bytes));
        }
    }
    /*
     * DBGMCU (CoreSight debug unit): not modeled as a real device, same
     * "plain RAM placeholder" treatment as extflash above. Real firmware
     * (ST HAL's DBGMCU_CR sleep/stop/standby-debug-enable helpers) reads
     * and read-modifies-writes this during early boot; without *some*
     * backing memory here those accesses BusFault, and this SoC's fault
     * handlers don't recover from that gracefully. A RAM stub is enough
     * to let boot past that point -- real semantics (e.g. IDC's fixed
     * chip-ID reset value) are future work if something depends on them.
     */
    /*
     * Flash controller registers (FLASH_ACR etc, distinct from the
     * memory-mapped flash content above) -- same plain-RAM-placeholder
     * rationale as DBGMCU just above: real firmware polls FLASH_ACR
     * wait-state-ready bits during clock init and BusFaults without
     * backing memory here.
     */
    /* FMC (external memory controller) -- same plain-RAM rationale. */
    /* CRS (HSI48 clock recovery/auto-trim) -- plain RAM placeholder. */
    /* OCTOSPI IO manager -- plain RAM placeholder (pin-mux only). */
    /* EXTI + SYSCFG -- plain RAM placeholder. */
    /* DMA1 + DMA2 + DMAMUX1 -- plain RAM placeholder. */
    /* SAI1 -- real device (gnw_h7b0_sai1.c): opens an AUD_* voice and
     * snoops DMA1 Stream0 transfer-complete ticks for PCM data. */
    /* DAC1/DAC2 -- plain RAM placeholders (previously entirely unmapped). */
    /* TIM1 -- plain RAM placeholder (previously entirely unmapped). */
    /* JPEG -- plain RAM placeholder (previously entirely unmapped). */
    /* TAMP -- plain RAM placeholder (previously entirely unmapped). */
    /* WWDG -- plain RAM placeholder (previously entirely unmapped). */
    /*
     * TIM2/3/4/5/6/7/12/13/14 -- plain RAM placeholder (previously
     * entirely unmapped).
     */
#undef INIT_RAM_REGION

    armv7m = DEVICE(&s->armv7m);
    /*
     * Was 96 -- too small for OCTOSPI2_IRQn (150), added when wiring
     * up real OCTOSPI1/2 IRQ lines (see gnw_h7b0_ospi.h). NVIC's
     * num-irq must be a multiple of 32; 160 is the smallest multiple
     * covering every IRQn this SoC model currently uses.
     */
    qdev_prop_set_uint32(armv7m, "num-irq", 160);
    qdev_prop_set_uint8(armv7m, "num-prio-bits", 4);
    qdev_prop_set_string(armv7m, "cpu-type", ARM_CPU_TYPE_NAME("cortex-m7"));
    qdev_prop_set_uint32(armv7m, "itcm-size", ITCM_SIZE);
    qdev_prop_set_bit(armv7m, "itcm-init-enable", true);
    qdev_prop_set_bit(armv7m, "itcm-init-rmw-enable", true);
    qdev_prop_set_bit(armv7m, "enable-bitband", true);
    /*
     * Real hardware boots from flash bank 1 at 0x08000000 via BOOT_ADD
     * option-byte address-0 remap (cold boot) or a debug probe directly
     * setting VTOR/SP/PC there (the gnwmanager dev-flow path retro-go's
     * own linker script targets -- see STM32H7B0VBTx_FLASH.ld, whose
     * .isr_vector lands at FLASH's origin, not ITCM's). QEMU's ARMv7M
     * container exposes exactly this indirection via init-nsvtor: it's
     * the initial value of VTOR, which is where cpu_reset() reads the
     * initial SP/PC vector table from. (Cortex-M7 has no TrustZone-M,
     * so it's the "ns" -- non-secure, i.e. only -- variant that applies;
     * "init-svtor" is a no-op property on this core, silently ignored by
     * armv7m.c's object_property_find guard -- do not use it here.)
     * Pointing it at flash bank 1 models that remap without needing a
     * fake alias memory region.
     */
    qdev_prop_set_uint32(armv7m, "init-nsvtor", FLASH_BANK1_BASE_ADDRESS);
    qdev_connect_clock_in(armv7m, "cpuclk", s->sysclk);
    /* The Cortex-M7 external SysTick reference is HCLK/8 independently of
     * whether experimental modeled timing is enabled.  CLKSOURCE=1 remains
     * connected to the processor clock above. */
    qdev_connect_clock_in(armv7m, "refclk", s->systick_refclk);
    object_property_set_link(OBJECT(&s->armv7m), "memory",
                              OBJECT(system_memory), &error_abort);
    /*
     * DWT (0xe0001000) is otherwise unconditionally RAZ/WI inside
     * armv7m.c's own container -- see gnw_h7b0_dwt.h for why real
     * firmware needs a real DWT_CYCCNT here. s->dwt's MemoryRegion is
     * already valid at this point (set up in its instance_init via
     * object_initialize_child, independent of when it's realized).
     */
    object_property_set_link(OBJECT(&s->armv7m), "dwt-mr",
                              OBJECT(&s->dwt.mmio), &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->armv7m), errp)) {
        return;
    }

    /*
     * The calibrated CPU penalty and MDMA service rate are one measured
     * contract.  Accepting either side alone would create an overlap window
     * or a CPU coefficient that was never validated as a standalone mode.
     * Check this only after the ARMv7-M container has realized its CPU.
     */
    bool mdma_axi1_profile = object_property_get_bool(
        OBJECT(s->armv7m.cpu),
        "x-gnw-m7-bus-contention-mdma-axi1-v1", &error_abort);
    bool bwm_comparison_profile = gnw_h7b0_cpu_bwm_comparison_enabled(s);
    bool mdma_axi1_runtime_authorized =
        s->mdma_axi1_runtime_receipt_sha256 &&
        GNW_MDMA_AXI1_AUTHORIZED_RECEIPT_SHA256[0] &&
        !strcmp(s->mdma_axi1_runtime_receipt_sha256,
                GNW_MDMA_AXI1_AUTHORIZED_RECEIPT_SHA256);
    bool mdma_axi1_comparison_admitted =
        s->mdma_axi1_comparison_mode &&
        s->mdma_axi1_runtime_receipt_sha256 &&
        !strcmp(s->mdma_axi1_runtime_receipt_sha256,
                GNW_MDMA_AXI1_COMPARISON_ADMISSION_SHA256);
    bool v37_bwm_comparison_admitted =
        s->mdma_v37_bwm_comparison_mode &&
        s->mdma_v37_bwm_fit_receipt_sha256 &&
        !strcmp(s->mdma_v37_bwm_fit_receipt_sha256,
                GNW_MDMA_V37_BWM_FIT_ADMISSION_SHA256);
    if (bwm_comparison_profile &&
        (!v37_bwm_comparison_admitted || !s->mdma.test_profile ||
         !s->timing_mode || mdma_axi1_profile ||
         object_property_get_bool(OBJECT(s->armv7m.cpu),
             "x-gnw-m7-bus-contention-test-profile", &error_abort))) {
        error_setg(errp, "the experimental BWM coefficient requires timing "
                   "mode, the exact BWM admission receipt, the transient "
                   "MDMA test profile, and no other contention profile");
        return;
    }
    if ((s->mdma_v37_bwm_comparison_mode ||
         s->mdma_v37_bwm_fit_receipt_sha256) &&
        (!bwm_comparison_profile || !v37_bwm_comparison_admitted)) {
        error_setg(errp, "v37 BWM comparison admission requires its exact "
                   "receipt and paired CPU candidate");
        return;
    }
    if (mdma_axi1_profile && !mdma_axi1_runtime_authorized &&
        !mdma_axi1_comparison_admitted) {
        error_setg(errp, "the MDMA AXI1 v1 coefficient requires an exact "
                   "reviewed runtime receipt or frozen comparison admission");
        return;
    }
    if (s->mdma_axi1_comparison_mode && !mdma_axi1_comparison_admitted) {
        error_setg(errp, "x-mdma-axi1-comparison-mode requires the exact "
                   "frozen comparison-only admission and paired profile");
        return;
    }
    if (!mdma_axi1_profile && s->mdma_axi1_runtime_receipt_sha256) {
        error_setg(errp, "x-mdma-axi1-runtime-receipt-sha256 requires the "
                   "paired CPU/MDMA AXI1 v1 profiles");
        return;
    }

    qdev_connect_gpio_out_named(DEVICE(&s->armv7m.nvic), "trace-enable", 0,
        qdev_get_gpio_in_named(DEVICE(&s->dwt), "trace-enable", 0));
    /* CPU stall accounting advances virtual time.  Requiring SoC timing mode
     * keeps DWT and peripheral deadlines on the same timing contract. */
    if (s->armv7m.cpu->gnw_m7_cycle_model && !s->timing_mode) {
        error_setg(errp, "x-gnw-m7-cycle-model requires "
                   "gnw-h7b0-soc.timing-mode=on");
        return;
    }

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->rcc), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->rcc), 0, RCC_BASE_ADDRESS);
    /* The board wires s->sysclk before realize; RCC updates it with the
     * core rate and updates systick_refclk independently with HCLK / 8. */
    gnw_h7b0_rcc_set_sysclk(&s->rcc, s->sysclk);
    gnw_h7b0_rcc_set_systick_refclk(&s->rcc, s->systick_refclk);
    if (s->timing_mode) {
        gnw_h7b0_rcc_init_timing_clock(&s->rcc);
    }
    gnw_h7b0_dwt_set_rcc(&s->dwt, &s->rcc);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->pwr), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->pwr), 0, PWR_BASE_ADDRESS);

    /* Keep the JEDEC/SFDP identity, command profile and XIP backing size
     * tied to the single chip selected by gwemu. */
    qdev_prop_set_uint64(DEVICE(&s->octospi1), "flash-size", s->extflash_size);
    qdev_prop_set_uint32(DEVICE(&s->octospi1), "chip-index",
                         (uint32_t)(nor_chip - gnw_nor_chips));
    if (s->timing_mode) {
        qdev_prop_set_bit(DEVICE(&s->octospi1), "timing-mode", true);
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->octospi1), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->octospi1), 0, OCTOSPI1_BASE_ADDRESS);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->octospi1), 0,
                        qdev_get_gpio_in(armv7m, OCTOSPI1_IRQn));
    /*
     * Real hardware's single external NOR is wired to OCTOSPI1; give
     * it a host pointer into the same extflash RAM region the CPU
     * sees over XIP, so indirect-mode PP/erase/READ (used by retro-go's
     * littlefs block device, gw_littlefs.c) actually persist instead
     * of being silently dropped -- see gnw_h7b0_ospi.h.
     */
    gnw_h7b0_ospi_set_backing(&s->octospi1,
                               memory_region_get_ram_ptr(&s->extflash));
    /*
     * ...and tell it which span that XIP window occupies, so it can
     * kill the window whenever the controller leaves memory-mapped
     * mode, the way real silicon's shared pins do -- see the "XIP
     * window availability" comment in gnw_h7b0_ospi.c.
     */
    gnw_h7b0_ospi_set_xip_window(&s->octospi1, system_memory,
                                  EXTFLASH_BASE_ADDRESS, s->extflash_size);

    qdev_prop_set_uint64(DEVICE(&s->octospi2), "flash-size", s->extflash_size);
    qdev_prop_set_uint32(DEVICE(&s->octospi2), "chip-index",
                         (uint32_t)(nor_chip - gnw_nor_chips));
    if (s->timing_mode) {
        qdev_prop_set_bit(DEVICE(&s->octospi2), "timing-mode", true);
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->octospi2), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->octospi2), 0, OCTOSPI2_BASE_ADDRESS);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->octospi2), 0,
                        qdev_get_gpio_in(armv7m, OCTOSPI2_IRQn));

    gnw_h7b0_ospi_set_kernel_clock_hz(&s->octospi1,
                                   gnw_h7b0_rcc_get_octospi_kernel_hz(&s->rcc));
    gnw_h7b0_rcc_add_octospi_clock_observer(
        &s->rcc, gnw_h7b0_ospi_kernel_clock_changed, &s->octospi1);
    gnw_h7b0_ospi_set_kernel_clock_hz(&s->octospi2,
                                   gnw_h7b0_rcc_get_octospi_kernel_hz(&s->rcc));
    gnw_h7b0_rcc_add_octospi_clock_observer(
        &s->rcc, gnw_h7b0_ospi_kernel_clock_changed, &s->octospi2);
    gnw_h7b0_rcc_add_reset_target(&s->rcc, GNW_H7B0_RCC_AHB3RSTR_OFFSET, 14,
                                DEVICE(&s->octospi1));
    gnw_h7b0_rcc_add_reset_target(&s->rcc, GNW_H7B0_RCC_AHB3RSTR_OFFSET, 19,
                                DEVICE(&s->octospi2));

    if (s->timing_mode) {
        qdev_prop_set_bit(DEVICE(&s->adc), "timing-mode", true);
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->adc), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->adc), 0, ADC_BASE_ADDRESS);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->adc), 0,
                        qdev_get_gpio_in(armv7m, ADC_IRQn));

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->lptim1), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->lptim1), 0, LPTIM1_BASE_ADDRESS);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->lptim1), 0,
                        qdev_get_gpio_in(armv7m, LPTIM1_IRQn));

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->ltdc), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->ltdc), 0, LTDC_BASE_ADDRESS);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->ltdc), 0,
                        qdev_get_gpio_in(armv7m, LTDC_IRQn));
    gnw_h7b0_ltdc_set_rcc(&s->ltdc, &s->rcc);

    if (s->timing_mode) {
        qdev_prop_set_bit(DEVICE(&s->dma2d), "timing-mode", true);
    }
    if (GNW_DMA_ENGINE_R1_AUTHORIZED_RECEIPT_SHA256[0] &&
        s->timing_mode && s->armv7m.cpu->gnw_m7_cycle_model &&
        !s->dma2d.test_profile && !s->dma2d.dma2d_poll_r4_profile) {
        qdev_prop_set_bit(DEVICE(&s->dma2d),
                          "x-dma-engine-successor-r1-profile", true);
        qdev_prop_set_string(DEVICE(&s->dma2d),
            "x-dma-engine-successor-r1-receipt-sha256",
            GNW_DMA_ENGINE_R1_AUTHORIZED_RECEIPT_SHA256);
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->dma2d), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->dma2d), 0, DMA2D_BASE_ADDRESS);
    {
        GnwH7B0RccEngineClockInfo info = gnw_h7b0_rcc_engine_clock_info(
            &s->rcc, GNW_H7B0_RCC_ENGINE_DMA2D);
        gnw_h7b0_dma2d_set_clock_info(&s->dma2d, info.known,
                                   info.reference_hz, info.enabled);
        gnw_h7b0_rcc_add_engine_clock_observer(
            &s->rcc, GNW_H7B0_RCC_ENGINE_DMA2D,
            gnw_h7b0_dma2d_clock_changed, &s->dma2d);
    }
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->dma2d), 0,
                        qdev_get_gpio_in(armv7m, DMA2D_IRQn));
    gnw_h7b0_rcc_add_reset_target(&s->rcc, GNW_H7B0_RCC_AHB3RSTR_OFFSET, 4,
                                DEVICE(&s->dma2d));

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->spi2), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->spi2), 0, SPI2_BASE_ADDRESS);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->spi1), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->spi1), 0, SPI1_BASE_ADDRESS);
    if (s->sd_wiring && !strcmp(s->sd_wiring, "ospi")) {
        s->spi1.softspi = true;
        s->gpio.softspi.bus = s->spi1.ssi;
    }
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->spi1), 0,
                        qdev_get_gpio_in(armv7m, SPI1_IRQn));

    /*
     * Timing mode promises a reproducible guest timeline.  Leaving the RTC's
     * default initial epoch at -1 would still sample time(NULL) here, making
     * TR/DR depend on when (and in which host timezone) QEMU was launched.
     * Keep the interactive default host-local clock, but give timing mode a
     * deterministic UTC seed unless the caller supplied an explicit epoch.
     */
    if (s->timing_mode && s->rtc.initial_epoch == -1) {
        s->rtc.initial_epoch = 0;
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->rtc), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->rtc), 0, RTC_BASE_ADDRESS);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->rtc), 0,
                        qdev_get_gpio_in(armv7m, RTC_Alarm_IRQn));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->rtc), 1,
                        qdev_get_gpio_in(DEVICE(&s->exti), 19));

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->crc), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->crc), 0, CRC_BASE_ADDRESS);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->hash), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->hash), 0, HASH_BASE_ADDRESS);
    /* HASH_RNG shared line (NVIC IRQ 80, see STM32H7B0.svd) -- RNG has no
     * error-condition IRQ source in this synchronous model (see
     * gnw_h7b0_rng.c), so this line is HASH's alone for now. */
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->hash), 0, qdev_get_gpio_in(armv7m, 80));

    if (s->timing_mode) {
        qdev_prop_set_bit(DEVICE(&s->mdma), "timing-mode", true);
    }
    if (GNW_DMA_ENGINE_R1_AUTHORIZED_RECEIPT_SHA256[0] &&
        s->timing_mode && s->armv7m.cpu->gnw_m7_cycle_model &&
        !s->mdma.test_profile && !s->mdma.mdma_axi1_v1_profile) {
        qdev_prop_set_bit(DEVICE(&s->mdma),
                          "x-dma-engine-successor-r1-profile", true);
        qdev_prop_set_string(DEVICE(&s->mdma),
            "x-dma-engine-successor-r1-receipt-sha256",
            GNW_DMA_ENGINE_R1_AUTHORIZED_RECEIPT_SHA256);
    }
    if (mdma_axi1_runtime_authorized &&
        (s->mdma.mdma_axi1_v1_profile ||
         !s->mdma.dma_engine_successor_profile)) {
        error_setg(errp, "the authorized MDMA AXI1 contention profile "
                   "requires the exact large-transfer producer profile");
        return;
    }
    if ((!mdma_axi1_profile && s->mdma.mdma_axi1_v1_profile) ||
        (mdma_axi1_profile &&
         !s->mdma.mdma_axi1_v1_profile &&
         !s->mdma.dma_engine_successor_profile) ||
        (s->mdma.mdma_axi1_v1_profile &&
         s->mdma.dma_engine_successor_profile)) {
        error_setg(errp, "the CPU contention profile requires exactly one "
                   "paired MDMA timing profile");
        return;
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->mdma), errp)) {
        return;
    }
    gnw_cycle_model_set_bus_contention_probe(
        CPU(s->armv7m.cpu), gnw_h7b0_bus_masters_for_range, s);
    if (s->armv7m.cpu->gnw_m7_xip_service_unseen_v1_profile) {
        gnw_cycle_model_set_xip_unseen_scope_probe(
            CPU(s->armv7m.cpu), gnw_h7b0_xip_unseen_scope_valid, s);
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->mdma), 0, MDMA_BASE_ADDRESS);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->mdma), 0,
                       qdev_get_gpio_in(armv7m, 122));
    {
        GnwH7B0RccEngineClockInfo info = gnw_h7b0_rcc_engine_clock_info(
            &s->rcc, GNW_H7B0_RCC_ENGINE_MDMA);
        gnw_h7b0_mdma_set_clock_info(&s->mdma, info.known,
                                   info.reference_hz, info.enabled);
        gnw_h7b0_rcc_add_engine_clock_observer(
            &s->rcc, GNW_H7B0_RCC_ENGINE_MDMA,
            gnw_h7b0_mdma_clock_changed, &s->mdma);
    }
    gnw_h7b0_rcc_add_reset_target(&s->rcc, GNW_H7B0_RCC_AHB3RSTR_OFFSET, 0,
                                DEVICE(&s->mdma));

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->rng), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->rng), 0, RNG_BASE_ADDRESS);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->gpio), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->gpio), 0, GPIO_BASE_ADDRESS);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->dbgmcu), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->dbgmcu), 0, DBGMCU_BASE_ADDRESS);

    /*
     * s->dwt is NOT mapped via sysbus_mmio_map/system_memory -- it's
     * wired directly into armv7m's own container (see the "dwt-mr"
     * link property set before armv7m's realize above), since that's
     * the only way to actually override armv7m.c's own RAZ/WI default
     * for the CPU-architected 0xe0001000 DWT address. Still needs its
     * own realize for reset/vmstate registration.
     */
    qdev_connect_clock_in(DEVICE(&s->dwt), "sysclk", s->sysclk);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->dwt), errp)) {
        return;
    }

    if (s->timing_mode) {
        qdev_prop_set_bit(DEVICE(&s->flash_r), "timing-mode", true);
    }
    gnw_h7b0_flash_r_set_initial_locked(&s->flash_r, s->rdp_locked);
    if (s->rdp_image && s->rdp_image[0]) {
        gnw_h7b0_flash_r_set_rdp_image(&s->flash_r, s->rdp_image);
    } else if (s->bank1_image && s->bank1_image[0]) {
        char *rdp_image = g_strdup_printf("%s.rdp", s->bank1_image);
    gnw_h7b0_flash_r_set_rdp_image(&s->flash_r, rdp_image);
        g_free(rdp_image);
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->flash_r), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->flash_r), 0, FLASH_R_BASE_ADDRESS);
    gnw_h7b0_flash_r_set_banks(&s->flash_r, &s->flash_bank1, &s->flash_bank2);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->fmc), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->fmc), 0, FMC_BASE_ADDRESS);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->crs), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->crs), 0, CRS_BASE_ADDRESS);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->octospim), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->octospim), 0, OCTOSPIM_BASE_ADDRESS);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->otfdec1), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->otfdec1), 0, OTFDEC1_BASE_ADDRESS);
    gnw_h7b0_otfdec_set_extflash(&s->otfdec1, system_memory, &s->extflash,
                                  EXTFLASH_BASE_ADDRESS, s->extflash_size);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->otfdec2), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->otfdec2), 0, OTFDEC2_BASE_ADDRESS);
    gnw_h7b0_otfdec_set_extflash(&s->otfdec2, system_memory, &s->extflash,
                                  EXTFLASH_BASE_ADDRESS, s->extflash_size);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->cryp), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->cryp), 0, CRYP_BASE_ADDRESS);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->cryp), 0, qdev_get_gpio_in(armv7m, 79));

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->exti), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->exti), 0, EXTI_SYSCFG_BASE_ADDRESS);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->exti), 0,
                        qdev_get_gpio_in(armv7m, EXTI0_IRQn));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->exti), 1,
                        qdev_get_gpio_in(armv7m, EXTI1_IRQn));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->exti), 2,
                        qdev_get_gpio_in(armv7m, EXTI2_IRQn));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->exti), 3,
                        qdev_get_gpio_in(armv7m, EXTI3_IRQn));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->exti), 4,
                        qdev_get_gpio_in(armv7m, EXTI4_IRQn));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->exti), 5,
                        qdev_get_gpio_in(armv7m, EXTI9_5_IRQn));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->exti), 6,
                        qdev_get_gpio_in(armv7m, EXTI15_10_IRQn));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->exti),
                        GNW_H7B0_EXTI_RTC_WKUP_OUTPUT,
                        qdev_get_gpio_in(armv7m, RTC_WKUP_IRQn));
    /* GPIO's PA0-release logic needs to notify EXTI of a real edge -- see
     * gnw_h7b0_gpio.c. Both devices already exist and are realized by
     * this point; this just wires the pointer, not a QOM property, since
     * GPIO -> EXTI isn't a real hardware bus relationship worth modeling
     * more formally for one internal notification. */
    s->gpio.exti = &s->exti;
    s->gpio.syscfg = &s->syscfg;
    s->extflash_unlock_baseline = gnw_h7b0_load_unlock_baseline(
        s->extflash_image, memory_region_get_ram_ptr(&s->extflash),
        memory_region_size(&s->extflash),
        s->flash_r.rdp_value != 0x55, errp);
    if (!s->extflash_unlock_baseline) {
        return;
    }
    gnw_h7b0_gpio_set_unlock_handoff(&s->gpio, CPU(s->armv7m.cpu),
                                     memory_region_get_ram_ptr(&s->itcm),
                                     memory_region_size(&s->itcm),
                                     memory_region_get_ram_ptr(&s->extflash),
                                     memory_region_size(&s->extflash),
                                     s->extflash_unlock_baseline,
                                     GNW_UNLOCK_BASELINE_SIZE,
                                     &s->flash_r.rdp_value);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->syscfg), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->syscfg), 0, EXTI_SYSCFG_BASE_ADDRESS + 0x400);

    if (s->timing_mode) {
        qdev_prop_set_bit(DEVICE(&s->dma), "timing-mode", true);
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->dma), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->dma), 0, DMA_BASE_ADDRESS);
    gnw_h7b0_dma_set_stream_notifier(&s->dma, 7,
                                      gnw_h7b0_bus_v6_dma_tc, &s->mdma);
    {
        /* DMA1 Stream0-6 (11-17), Stream7 (47); DMA2 Stream0-4 (56-60),
         * Stream5-7 (68-70) -- see stm32h7b0xx.h's IRQn_Type, not a
         * contiguous range. */
        static const int dma_stream_irqn[GNW_H7B0_DMA_STREAM_COUNT] = {
            11, 12, 13, 14, 15, 16, 17, 47,
            56, 57, 58, 59, 60, 68, 69, 70,
        };
        for (int i = 0; i < GNW_H7B0_DMA_STREAM_COUNT; i++) {
            sysbus_connect_irq(SYS_BUS_DEVICE(&s->dma), i,
                                qdev_get_gpio_in(armv7m, dma_stream_irqn[i]));
        }
    }

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->sai1), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->sai1), 0, SAI1_BASE_ADDRESS);
    gnw_h7b0_sai1_set_dma(&s->sai1, &s->dma);
    gnw_h7b0_sai1_set_rcc(&s->sai1, &s->rcc);

    /*
     * Peripheral resets. HAL_DeInit() force-resets every bus, and
     * retro-go calls it on the way out of a game right before jumping
     * back to the launcher -- without these the game's still-armed
     * SAI1/DMA1 audio stream survives into the launcher's fresh boot
     * and storms an interrupt it can never acknowledge.
     * RM0455: AHB1RSTR bit0 = DMA1RST, bit1 = DMA2RST;
     *         APB2RSTR bit22 = SAI1RST.
     */
    gnw_h7b0_rcc_add_reset_target(&s->rcc, GNW_H7B0_RCC_AHB1RSTR_OFFSET, 0,
                                   DEVICE(&s->dma));
    gnw_h7b0_rcc_add_reset_target(&s->rcc, GNW_H7B0_RCC_AHB1RSTR_OFFSET, 1,
                                   DEVICE(&s->dma));
    gnw_h7b0_rcc_add_reset_target(&s->rcc, GNW_H7B0_RCC_APB2RSTR_OFFSET, 22,
                                   DEVICE(&s->sai1));
    gnw_h7b0_hash_set_dma(&s->hash, &s->dma);
    gnw_h7b0_adc_set_dma(&s->adc, &s->dma);
    gnw_h7b0_adc_set_rcc(&s->adc, &s->rcc);
    gnw_h7b0_rcc_add_reset_target(&s->rcc, GNW_H7B0_RCC_AHB1RSTR_OFFSET, 5,
                                DEVICE(&s->adc));
    /* SPI1 only -- SPI2 drives the LCD panel with no DMA at all. */
    gnw_h7b0_spi_set_dma(&s->spi1, &s->dma);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->dac1), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->dac1), 0, DAC1_BASE_ADDRESS);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->dac2), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->dac2), 0, DAC2_BASE_ADDRESS);

    if (s->timing_mode) {
        qdev_prop_set_bit(DEVICE(&s->tim1), "timing-mode", true);
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->tim1), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->tim1), 0, TIM1_BASE_ADDRESS);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->tim1), 0,
                       qdev_get_gpio_in(armv7m, TIM1_UP_IRQn));
    gnw_h7b0_tim1_set_rcc(&s->tim1, &s->rcc);

    if (s->timing_mode) {
        qdev_prop_set_bit(DEVICE(&s->jpeg), "timing-mode", true);
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->jpeg), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->jpeg), 0, JPEG_BASE_ADDRESS);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->tamp), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->tamp), 0, TAMP_BASE_ADDRESS);
    gnw_h7b0_rcc_set_backup_domain_reset(
        &s->rcc, gnw_h7b0_soc_tamp_backup_domain_reset, &s->tamp);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->wwdg), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->wwdg), 0, WWDG_BASE_ADDRESS);

    if (s->timing_mode) {
        qdev_prop_set_bit(DEVICE(&s->tim2), "timing-mode", true);
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->tim2), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->tim2), 0, TIM2_BLOCK_BASE_ADDRESS);
    static const int tim_irqs[GNW_H7B0_TIM2_BLOCK_INSTANCE_COUNT] = {
        TIM2_IRQn, TIM3_IRQn, TIM4_IRQn, TIM5_IRQn, TIM6_DAC_IRQn, TIM7_IRQn,
        TIM8_BRK_TIM12_IRQn, TIM8_UP_TIM13_IRQn, TIM8_TRG_COM_TIM14_IRQn,
    };
    for (int i = 0; i < GNW_H7B0_TIM2_BLOCK_INSTANCE_COUNT; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->tim2), i,
                           qdev_get_gpio_in(armv7m, tim_irqs[i]));
    }
    gnw_h7b0_tim2_set_rcc(&s->tim2, &s->rcc);
    gnw_h7b0_tim2_set_update_notifier(&s->tim2,
                                       gnw_h7b0_bus_v6_tim_update, s);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->iwdg), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->iwdg), 0, IWDG_BASE_ADDRESS);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->lpuart1), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->lpuart1), 0, LPUART1_BASE_ADDRESS);

    /* Homebrew firmware uses USART1 as its printf console; bind it to
     * serial port 0 so -serial stdio/file: just works. */
    qdev_prop_set_chr(DEVICE(&s->usart1), "chardev", serial_hd(0));
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->usart1), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->usart1), 0, USART1_BASE_ADDRESS);

    s->startup_handoff_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                            gnw_h7b0_startup_handoff, s);
    s->cold_power_on_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                          gnw_h7b0_cold_power_on, s);
    s->cold_power_release_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                               gnw_h7b0_cold_power_release, s);
    s->initial_boot_handoff_pending = s->rdp_locked;
    monitor_register_hmp("gnw-cold-cycle", false, hmp_gnw_cold_cycle);
    monitor_register_hmp("gnw-ui-reset", false, hmp_gnw_ui_reset);

    /*
     * Remaining peripherals (DMA2D, GPIO, USART, real flash/QSPI boot)
     * are added in later phases; see ../../docs/roadmap.md.
     */
}

static const Property gnw_h7b0_soc_properties[] = {
    /*
     * Optional file-backed persistence for the flash regions -- e.g.
     * -global gnw-h7b0-soc.extflash-image=backup/qemu-images/zelda-extflash.bin
     * makes guest writes (flashing/erasing) land in that file live, instead
     * of only in anonymous RAM seeded once at boot by `-device loader`. See
     * the doc comment on GnwH7B0State's bank1_image/bank2_image/
     * extflash_image fields in gnw_h7b0_soc.h.
     */
    DEFINE_PROP_STRING("uid", GnwH7B0State, device_uid),
    DEFINE_PROP_STRING("sd-wiring", GnwH7B0State, sd_wiring),
    DEFINE_PROP_STRING("bank1-image", GnwH7B0State, bank1_image),
    DEFINE_PROP_STRING("bank2-image", GnwH7B0State, bank2_image),
    DEFINE_PROP_STRING("extflash-image", GnwH7B0State, extflash_image),
    DEFINE_PROP_STRING("extflash-chip", GnwH7B0State, extflash_chip),
    DEFINE_PROP_STRING("rdp-image", GnwH7B0State, rdp_image),
    DEFINE_PROP_BOOL("rdp-locked", GnwH7B0State, rdp_locked, false),
    DEFINE_PROP_BOOL("timing-mode", GnwH7B0State, timing_mode, false),
    DEFINE_PROP_BOOL("x-modeled-timing", GnwH7B0State,
                     x_modeled_timing, false),
    DEFINE_PROP_BOOL("x-cpu-cache-dma2d-v2-profile", GnwH7B0State,
                     cpu_cache_dma2d_v2_profile, false),
    DEFINE_PROP_STRING("x-cpu-cache-dma2d-v2-runtime-receipt-sha256",
                       GnwH7B0State,
                       cpu_cache_dma2d_v2_runtime_receipt_sha256),
    DEFINE_PROP_STRING("x-mdma-axi1-runtime-receipt-sha256", GnwH7B0State,
                       mdma_axi1_runtime_receipt_sha256),
    DEFINE_PROP_BOOL("x-mdma-axi1-comparison-mode", GnwH7B0State,
                     mdma_axi1_comparison_mode, false),
    DEFINE_PROP_STRING("x-mdma-v37-bwm-fit-receipt-sha256", GnwH7B0State,
                       mdma_v37_bwm_fit_receipt_sha256),
    DEFINE_PROP_BOOL("x-mdma-v37-bwm-comparison-mode", GnwH7B0State,
                     mdma_v37_bwm_comparison_mode, false),
    DEFINE_PROP_UINT32("x-timing-test-cycles-per-insn", GnwH7B0State,
                       x_timing_test_cycles_per_insn, 0),
    DEFINE_PROP_BOOL("x-timing-test-half-rate", GnwH7B0State,
                     x_timing_test_half_rate, false),
};

static void gnw_h7b0_soc_finalize(Object *obj)
{
    GnwH7B0State *s = GNW_H7B0_SOC(obj);

    g_free(s->extflash_unlock_baseline);
    g_free(s->cold_itcm_seed);
    if (s->cold_power_on_timer) {
        timer_free(s->cold_power_on_timer);
    }
    if (s->cold_power_release_timer) {
        timer_free(s->cold_power_release_timer);
    }
    if (s->startup_handoff_timer) {
        timer_free(s->startup_handoff_timer);
    }
}

struct GnwH7B0StateClass {
    SysBusDeviceClass parent_class;
    ResettablePhases parent_phases;
};

static void gnw_h7b0_soc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    GnwH7B0StateClass *sc = GNW_H7B0_SOC_CLASS(klass);

    resettable_class_set_parent_phases(RESETTABLE_CLASS(klass), NULL, NULL,
                                       gnw_h7b0_soc_reset_exit,
                                       &sc->parent_phases);

    dc->realize = gnw_h7b0_soc_realize;
    device_class_set_props(dc, gnw_h7b0_soc_properties);
    object_class_property_add(klass, "x-hclk-hz", "uint64",
                              gnw_h7b0_soc_get_hclk_hz, NULL, NULL, NULL);
    object_class_property_set_description(klass, "x-hclk-hz",
        "Read-only RCC model HCLK frequency in Hz");
    object_class_property_add(klass, "x-octospi-kernel-hz", "uint64",
                              gnw_h7b0_soc_get_octospi_kernel_hz,
                              NULL, NULL, NULL);
    object_class_property_set_description(klass, "x-octospi-kernel-hz",
        "Read-only RCC model OCTOSPI kernel frequency in Hz");
}

static const TypeInfo gnw_h7b0_soc_info = {
    .name          = TYPE_GNW_H7B0_SOC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GnwH7B0State),
    .class_size    = sizeof(GnwH7B0StateClass),
    .instance_init = gnw_h7b0_soc_initfn,
    .instance_finalize = gnw_h7b0_soc_finalize,
    .class_init    = gnw_h7b0_soc_class_init,
};

static void gnw_h7b0_soc_types(void)
{
    type_register_static(&gnw_h7b0_soc_info);
}

type_init(gnw_h7b0_soc_types)
