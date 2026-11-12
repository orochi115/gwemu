/*
 * STM32H7B0 SoC (Nintendo Game & Watch), Phase 0 skeleton
 *
 * Bare Cortex-M7 + RAM only, no peripherals yet. Modeled on
 * hw/arm/stm32f405_soc.c's SoC-container pattern. Addresses taken from
 * ../../../minicraft-gnw/tools/retro-go-porting-toolkit/linker.ld (real
 * hardware memory map, cross-checked against STM32H7B0.svd at the repo
 * root) rather than the reference manual directly.
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

#ifndef HW_ARM_GNW_H7B0_SOC_H
#define HW_ARM_GNW_H7B0_SOC_H

#include "hw/arm/armv7m.h"
#include "hw/misc/gnw_h7b0_rcc.h"
#include "hw/misc/gnw_h7b0_pwr.h"
#include "qemu/timer.h"
#include "hw/misc/gnw_h7b0_ospi.h"
#include "hw/misc/gnw_h7b0_adc.h"
#include "hw/misc/gnw_h7b0_lptim1.h"
#include "hw/display/gnw_h7b0_ltdc.h"
#include "hw/display/gnw_h7b0_dma2d.h"
#include "hw/misc/gnw_h7b0_spi.h"
#include "hw/misc/gnw_h7b0_rtc.h"
#include "hw/misc/gnw_h7b0_crc.h"
#include "hw/misc/gnw_h7b0_hash.h"
#include "hw/misc/gnw_h7b0_mdma.h"
#include "hw/misc/gnw_h7b0_rng.h"
#include "hw/misc/gnw_h7b0_gpio.h"
#include "hw/misc/gnw_h7b0_dbgmcu.h"
#include "hw/misc/gnw_h7b0_dwt.h"
#include "hw/misc/gnw_h7b0_flash_r.h"
#include "hw/misc/gnw_h7b0_fmc.h"
#include "hw/misc/gnw_h7b0_crs.h"
#include "hw/misc/gnw_h7b0_otfdec.h"
#include "hw/misc/gnw_h7b0_cryp.h"
#include "hw/misc/gnw_h7b0_octospim.h"
#include "hw/misc/gnw_h7b0_exti.h"
#include "hw/misc/gnw_h7b0_syscfg.h"
#include "hw/misc/gnw_h7b0_dma.h"
#include "hw/misc/gnw_h7b0_sai1.h"
#include "hw/misc/gnw_h7b0_dac.h"
#include "hw/misc/gnw_h7b0_tim1.h"
#include "hw/misc/gnw_h7b0_jpeg.h"
#include "hw/misc/gnw_h7b0_tamp.h"
#include "hw/misc/gnw_h7b0_wwdg.h"
#include "hw/misc/gnw_h7b0_tim2.h"
#include "hw/misc/gnw_h7b0_iwdg.h"
#include "hw/misc/gnw_h7b0_lpuart1.h"
#include "hw/misc/gnw_h7b0_usart1.h"
#include "qom/object.h"

#define TYPE_GNW_H7B0_SOC "gnw-h7b0-soc"
OBJECT_DECLARE_TYPE(GnwH7B0State, GnwH7B0StateClass, GNW_H7B0_SOC)

/*
 * Real STM32H7B0 memory map, per RM0455 Table 6 (rm0455.pdf, repo root),
 * cross-checked against STM32H7B0.svd's RCC clock-enable bit names
 * (AXISRAM1/2/3LPEN, AHBSRAM1/2LPEN, DTCM1/2LPEN, BKPRAMLPEN, SRDSRAMLPEN)
 * confirming the bank split. EXCEPTION: internal flash size/layout is
 *2x256K dual-bank per the project owner (community-verified real-silicon
 * fact), NOT RM0455's stated 128K single-bank — see
 * ../../docs/h7b0-flash-discrepancy.md before changing FLASH_BANK*.
 */
#define ITCM_BASE_ADDRESS     0x00000000
#define ITCM_SIZE             (64 * 1024)

#define DTCM_BASE_ADDRESS     0x20000000
#define DTCM_SIZE             (128 * 1024)

#define AXISRAM1_BASE_ADDRESS 0x24000000
#define AXISRAM1_SIZE         (256 * 1024)
#define AXISRAM2_BASE_ADDRESS 0x24040000
#define AXISRAM2_SIZE         (384 * 1024)
#define AXISRAM3_BASE_ADDRESS 0x240A0000
#define AXISRAM3_SIZE         (384 * 1024)

#define AHBSRAM1_BASE_ADDRESS 0x30000000
#define AHBSRAM1_SIZE         (64 * 1024)
#define AHBSRAM2_BASE_ADDRESS 0x30010000
#define AHBSRAM2_SIZE         (64 * 1024)

#define SRDSRAM_BASE_ADDRESS  0x38000000
#define SRDSRAM_SIZE          (32 * 1024)
#define BKPSRAM_BASE_ADDRESS  0x38800000
#define BKPSRAM_SIZE          (4 * 1024)

/*
 * Internal flash: see ../../docs/h7b0-flash-discrepancy.md. 2x256K
 * dual-bank per community/project-owner knowledge, NOT RM0455's stated
 * 128K single-bank. Modeled as plain RAM for now (no program/erase
 * register semantics yet); content loading and bank-select-to-address-0
 * mirroring is future work, see docs/roadmap.md Phase 1.
 *
 * NOTE (2026-07-12 breakpoint-lockstep tracing session): a same-day
 * attempt to add a flash-bank1-at-address-0 alias to explain a
 * null-pointer-dereference divergence was reverted -- ITCM_BASE_ADDRESS
 * is *already* 0x00000000 (see below), so the alias shadowed the
 * pre-existing, legitimate ITCM region instead of fixing anything.
 * Real hardware's address-0 content at the point traced turned out to
 * be code-like bytes, not flash's raw vector table -- consistent with
 * firmware copying a startup/hot-code section into ITCM early in boot
 * (a common STM32H7 pattern), which our existing ITCM-as-RAM-at-0
 * model should already reproduce once that copy loop runs identically
 * on both targets -- investigate whether it actually does before
 * adding any address-0 remap logic.
 */
#define FLASH_BANK1_BASE_ADDRESS 0x08000000
#define FLASH_BANK2_BASE_ADDRESS 0x08100000
#define FLASH_BANK_SIZE           (256 * 1024)

/*
 * Factory-programmed 96-bit unique device ID (STM32H7xx_HAL_Driver's
 * HAL_GetUIDw0/1/2(), stm32h7b0xx.h's UID_BASE) -- real hardware has
 * this burned into a fixed system-memory address well past either
 * flash bank, not backed by any RAM/flash region we model elsewhere.
 * retro-go's extflash file-cache journal (Core/Src/gw_flash_alloc.c's
 * get_cpu_unique_id(), used whenever a ROM is too big for the RAM
 * cache and gets cached to extflash instead) reads this at runtime;
 * without backing memory here that read BusFaults. A small RAM page
 * seeded with a fixed synthetic UID (gnw_h7b0_soc.c) is enough --
 * nothing depends on this matching real silicon's actual per-chip ID.
 */
#define UID_BASE_ADDRESS 0x08FFF800
#define UID_SIZE          4096

/*
 * External OSPI flash (real G&W firmware lives here, XIP). Dual-quad
 * OCTOSPI1+OCTOSPI2 via the IO manager, since we're targeting the Tim
 * Scheuerwegen SD-card mod (SPI2/OSPI2 path), not the yota9 mod. Not yet
 * modeled as a real OCTOSPI device (no register-level emulation) --
 * reserved as plain RAM for now so an image can be loaded and executed
 * from the real address for boot-path testing. Size varies 1-256MB on
 * real hardware; default to minicraft-gnw's linker.ld default (64M).
 */
#define EXTFLASH_BASE_ADDRESS 0x90000000
#define EXTFLASH_DEFAULT_SIZE (64 * 1024 * 1024)
/* Compatibility for build-time image tools using the default board size. */
#define EXTFLASH_SIZE EXTFLASH_DEFAULT_SIZE

/*
 * RCC base address: computed as PERIPH_BASE + SRD_AHB4PERIPH_BASE offset
 * + 0x4400 from sdk/cmsis-device-h7/Include/stm32h7b0xx.h, cross-checked
 * against RM0455 Table 7's register boundary list (RCC:
 * 0x58024400-0x580247FF) -- both agree.
 */
#define RCC_BASE_ADDRESS 0x58024400

/*
 * DBGMCU (CoreSight debug unit), per RM0455 Table 7's register boundary
 * list. Modeled as plain RAM for now -- see the comment above its
 * INIT_RAM_REGION call in gnw_h7b0_soc.c realize().
 */
#define DBGMCU_BASE_ADDRESS 0x5C001000

/*
 * ARMv7-M DWT unit -- architecturally fixed for every Cortex-M
 * implementation, not SoC-specific (see gnw_h7b0_dwt.h).
 */
#define DWT_BASE_ADDRESS 0xE0001000
#define DBGMCU_SIZE          0x400

/*
 * Flash controller registers (FLASH_ACR etc. -- NOT the memory-mapped
 * flash content itself, that's FLASH_BANK1/2_BASE_ADDRESS above), per
 * RM0455/CMSIS FLASH_R_BASE = AHB3PERIPH_BASE + 0x2000. Modeled as plain
 * RAM for now -- see the DBGMCU comment in gnw_h7b0_soc.c realize() for
 * the same rationale (real firmware polls FLASH_ACR wait-state-ready
 * bits during clock init; without backing memory here that BusFaults).
 */
#define FLASH_R_BASE_ADDRESS 0x52002000
#define FLASH_R_SIZE          0x1000

/*
 * FMC (Flexible Memory Controller -- external SRAM/NAND/PSRAM/SDRAM
 * interface), per RM0455/CMSIS FMC_R_BASE = AHB3PERIPH_BASE + 0x4000.
 * Modeled as plain RAM for now -- same rationale as FLASH_R/DBGMCU
 * above.
 */
#define FMC_BASE_ADDRESS 0x52004000
#define FMC_SIZE          0x1000

/*
 * PWR (power control), per RM0455/SVD baseAddress 0x58024800, right
 * after RCC. Modeled as a real minimal device (hw/misc/gnw_h7b0_pwr.c),
 * RCC-style: found by a gnw-chainloader boot hang in SystemClock_Config
 * spinning on PWR_CSR1's ACTVOSRDY bit, which a plain-RAM stub never
 * sets (it's a hardware-set-only status bit, not something firmware
 * writes). See gnw_h7b0_pwr.h.
 */
#define PWR_BASE_ADDRESS 0x58024800

/*
 * GPIOA-K, per STM32H7B0.svd (contiguous 0x400-per-port blocks,
 * 0x58020000-0x58022FFF). Modeled as a real minimal device
 * (hw/misc/gnw_h7b0_gpio.c) -- real GPIO (pin-mux-driven input/output,
 * LCD control lines) is still Phase 4 territory (see docs/roadmap.md),
 * but a zero-initialized plain-RAM IDR made every active-low button
 * (external pull-ups, real hardware convention) read as permanently
 * pressed, silently forcing gnw-chainloader's "God Mode" boot-time
 * button-override path on every launch. See gnw_h7b0_gpio.h.
 */
#define GPIO_BASE_ADDRESS 0x58020000

/*
 * CRS (Clock Recovery System, HSI48 auto-trim -- used by USB), per
 * STM32H7B0.svd baseAddress 0x40008400. Modeled as plain RAM for now --
 * same rationale as the other not-yet-modeled peripherals above.
 */
#define CRS_BASE_ADDRESS 0x40008400
#define OTFDEC1_BASE_ADDRESS 0x5200b800
#define OTFDEC2_BASE_ADDRESS 0x5200bc00
#define CRYP_BASE_ADDRESS 0x48021000
#define CRS_SIZE          0x400

/*
 * OCTOSPI1/2 controller registers (distinct from the memory-mapped XIP
 * window at EXTFLASH_BASE_ADDRESS above), per STM32H7B0.svd. Modeled as
 * a real minimal device (hw/misc/gnw_h7b0_ospi.c), RCC/PWR-style: found
 * by a gnw-chainloader boot hitting its own "spin forever on HAL error"
 * trap in OSPI_WriteBytes, because HAL_OSPI_Command()'s post-config
 * poll of SR's TCF bit never completed against a plain-RAM stub. See
 * gnw_h7b0_ospi.h -- still no real command/address/data-phase transfer
 * semantics (no bytes actually move), just enough for HAL to see
 * "command accepted, transfer complete" and not error out.
 */
#define OCTOSPI1_BASE_ADDRESS 0x52005000
#define OCTOSPI2_BASE_ADDRESS 0x5200A000
/* Per sdk/cmsis-device-h7/Include/stm32h7b0xx.h's IRQn_Type. */
#define OCTOSPI1_IRQn 92
#define OCTOSPI2_IRQn 150
/*
 * OCTOSPI IO manager -- plain RAM for now (pin-mux config only, no
 * transfer semantics needed there).
 */
#define OCTOSPIM_BASE_ADDRESS 0x5200B400
#define OCTOSPIM_SIZE          0x400

/*
 * TAMP (tamper/backup registers), per STM32H7B0.svd baseAddress
 * 0x58004400 -- adjacent to but distinct from RTC (RTC_BASE_ADDRESS
 * above only covers 0x58004000-0x580043FF; TAMP is a separate
 * peripheral immediately after it, not merged into our RTC device).
 * Modeled as plain RAM for now -- same rationale as the other
 * not-yet-modeled peripherals above; found the same way CRC was, one
 * gap further into gnw-chainloader's boot once OSPI/CRC stopped
 * blocking it.
 */
#define TAMP_BASE_ADDRESS 0x58004400
#define TAMP_SIZE          0x400

/*
 * WWDG (window watchdog), per STM32H7B0.svd baseAddress 0x50003000.
 * Modeled as plain RAM for now -- same rationale as the other
 * not-yet-modeled peripherals above; found the same way DAC1/DAC2/
 * TIM1 were, entirely unmapped rather than even a RAM placeholder.
 * `MX_WWDG1_Init()` (retro-go) and the OEM firmware's own watchdog
 * init both touch this during boot. No real countdown/refresh
 * semantics modeled -- if something ever blocks waiting for a
 * hardware-set WWDG status bit, a dumb RAM stub won't be enough (same
 * caveat as RTC/OSPI/ADC/PWR before they got real devices).
 */
#define WWDG_BASE_ADDRESS 0x50003000
#define WWDG_SIZE          0x400

/*
 * TIM2/3/4/5 (general purpose) + TIM6/7 (basic) + TIM12/13/14 (general
 * purpose), per STM32H7B0.svd (contiguous 0x400-per-timer blocks,
 * 0x40000000-0x400023FF, stopping exactly at LPTIM1's own base of
 * 0x40002400 -- NOT 0x2800: that earlier size was off by one 0x400
 * block and silently overlapped/shadowed the real LPTIM1 device
 * (0x40002400-0x400027FF) with this plain-RAM stub, since this block
 * realizes after LPTIM1 in gnw_h7b0_soc_realize() -- confirmed live
 * (a poke to LPTIM1_ARR read back with no ISR.ARROK side effect at
 * all, i.e. plain RAM shadow behavior, not the real device) while
 * root-causing stm32h7b0-diag's case_lptim1_correct.c genuinely
 * failing (not hanging) even after the real LPTIM1 device was added.
 * Modeled as plain RAM for now -- same rationale as the other
 * not-yet-modeled peripherals above; found via patched-zelda-bank1.bin
 * (real OEM firmware) touching TIM5 (0x40000C00) during boot, entirely
 * unmapped like TIM1 was. Covering the whole contiguous block up front
 * (TIM1's own gap was found and fixed one timer at a time, single
 * instance, before this) since real OEM firmware's timer usage
 * (piezo speaker PWM, backlight, etc.) is likely to touch more than
 * one of these.
 */
#define TIM2_BLOCK_BASE_ADDRESS 0x40000000
#define TIM2_BLOCK_SIZE          0x2400

/*
 * IWDG (independent watchdog) and LPUART1, per STM32H7B0.svd. Modeled
 * as real (if minimal) devices, not create_unimplemented_device()
 * stubs -- found via the sibling stm32h7b0-diag correctness-test
 * suite's real-hardware register-dump comparison that
 * create_unimplemented_device()'s always-reads-zero behavior made
 * IWDG_RLR/IWDG_WINR (real reset value 0x00000FFF) and LPUART1_ISR
 * (real reset value 0x000000C0) read wrong at reset.
 */
#define IWDG_BASE_ADDRESS 0x58004800
#define LPUART1_BASE_ADDRESS 0x58000c00
#define USART1_BASE_ADDRESS 0x40011000

/*
 * CRC (hardware CRC-32 unit), per STM32H7B0.svd baseAddress
 * 0x40023000. Modeled as a real device (hw/misc/gnw_h7b0_crc.c) --
 * found once gnw-chainloader's OSPI_Init() stopped hanging (see
 * gnw_h7b0_ospi.h) and boot reached ofw_crc32()
 * (src/chainloader/system/ofw_verify.c), which first BusFaulted
 * (entirely unmapped) and, once given only a plain-RAM stub, made
 * every flash-content verification fail (DR always read back 0). A
 * real bit-serial CRC engine was needed, not just enough register
 * plumbing to avoid a fault -- see gnw_h7b0_crc.h.
 */
#define CRC_BASE_ADDRESS 0x40023000

/*
 * HASH (hardware MD5/SHA-1/SHA-224/SHA-256 processor), per
 * STM32H7B0.svd baseAddress 0x48021400. Modeled as a real device
 * (hw/misc/gnw_h7b0_hash.c) -- gnwmanager's RAM flash util calls
 * HAL_HASHEx_SHA256_Start(..., HAL_MAX_DELAY) after every flash write
 * to verify it, and HAL_MAX_DELAY means no timeout: an unimplemented-
 * device stub (always reads 0) left the digest-complete flag
 * permanently unset, wedging the guest CPU forever on every internal-
 * flash write. See gnw_h7b0_hash.h.
 */
#define HASH_BASE_ADDRESS 0x48021400
#define MDMA_BASE_ADDRESS 0x52000000
#define RNG_BASE_ADDRESS 0x48021800

/*
 * ADC1/ADC2 (+ common registers), per STM32H7B0.svd (0x40022000/
 * 0x40022100, 0x100 each, plus shared ADC_COMMON registers at +0x300).
 * Modeled as a real minimal device (hw/misc/gnw_h7b0_adc.c), RCC/PWR-
 * style: found by a gnw-chainloader boot hang in board_adc_init()
 * spinning on ADC1's ISR.ADRDY bit after setting CR.ADEN, which a
 * plain-RAM stub never sets. See gnw_h7b0_adc.h -- regular conversions
 * return a fixed full-battery reading; every other register is a plain
 * shadow.
 */
#define ADC_BASE_ADDRESS 0x40022000
/* Per sdk/cmsis-device-h7/Include/stm32h7b0xx.h's IRQn_Type: ADC_IRQn = 18
 * (shared by ADC1/ADC2). */
#define ADC_IRQn 18

/* LPTIM1 -- was entirely unmapped (see gnw_h7b0_lptim1.h for the real
 * bug this caused: stm32h7b0-diag's case_lptim1_correct.c HardFaulting
 * and hanging the whole diag sweep). Base/IRQn per STM32H7B0.svd. */
#define LPTIM1_BASE_ADDRESS 0x40002400
#define LPTIM1_IRQn 93

/* Per sdk/cmsis-device-h7/Include/stm32h7b0xx.h's IRQn_Type. EXTI lines
 * 0-4 each get a dedicated IRQ; 5-9 and 10-15 share one IRQ each. */
#define EXTI0_IRQn 6
#define EXTI1_IRQn 7
#define EXTI2_IRQn 8
#define EXTI3_IRQn 9
#define EXTI4_IRQn 10
#define EXTI9_5_IRQn 23
#define EXTI15_10_IRQn 40

/* Per sdk/CMSIS/Device/ST/STM32H7xx/Include/stm32h7b0xx.h. */
#define TIM1_UP_IRQn 25
#define TIM2_IRQn 28
#define TIM3_IRQn 29
#define TIM4_IRQn 30
#define TIM5_IRQn 50
#define TIM6_DAC_IRQn 54
#define TIM7_IRQn 55
#define TIM8_BRK_TIM12_IRQn 43
#define TIM8_UP_TIM13_IRQn 44
#define TIM8_TRG_COM_TIM14_IRQn 45

/*
 * SPI2, per STM32H7B0.svd baseAddress 0x40003800. This is also the
 * LCD-panel init-command path (separate from the LTDC pixel-data
 * path; see gnw-chainloader's gw_lcd_spi_tx()) as well as the Tim
 * Scheuerwegen SD-card mod's SPI path (per CLAUDE.md's repo
 * conventions -- SPI2/OSPI2, not the yota9 mod). Modeled as a real
 * minimal device (hw/misc/gnw_h7b0_spi.c): found by a gnw-chainloader
 * boot hang in gw_lcd_spi_tx() spinning on SR.TXP/SR.EOT, which a
 * plain-RAM stub never sets. No real byte transfer happens yet (no
 * LCD-panel or SD-card model attached).
 */
#define SPI2_BASE_ADDRESS 0x40003800

/*
 * SPI1, per STM32H7B0.svd baseAddress 0x40013000. This is the "Tim"
 * SD-card mod's dedicated-pin bus, tried first by gnw-chainloader's
 * sdcard_detect() -> spi1_init(). Modeled as a real minimal device
 * (hw/misc/gnw_h7b0_spi.c, same as SPI2): found by a boot hang in
 * spi1_init() spinning on SR.TXP/SR.EOT the same way SPI2's LCD path
 * did, which a plain-RAM stub never sets.
 */
#define SPI1_BASE_ADDRESS 0x40013000
/*
 * Per sdk/cmsis-device-h7/Include/stm32h7b0xx.h's IRQn_Type. Needed for
 * the DMA-driven SD block reads: HAL retires those from SPI1's EOT
 * interrupt (see gnw_h7b0_spi.c's DMA section). SPI2 has no IRQ wired --
 * its LCD path is polled.
 */
#define SPI1_IRQn 35

/*
 * EXTI + SYSCFG, per STM32H7B0.svd (contiguous 0x400-per-block,
 * 0x58000000-0x580007FF). Modeled as plain RAM for now -- same
 * rationale as the other not-yet-modeled peripherals above; covers
 * both in one region since they're adjacent and both commonly touched
 * during GPIO/interrupt pin-mux init.
 */
#define EXTI_SYSCFG_BASE_ADDRESS 0x58000000
#define EXTI_SYSCFG_SIZE          0x800

/*
 * DMA1 + DMA2 + DMAMUX1, per STM32H7B0.svd (contiguous 0x400-per-block,
 * 0x40020000-0x40020BFF). Modeled as plain RAM for now -- same
 * rationale as the other not-yet-modeled peripherals above.
 */
#define DMA_BASE_ADDRESS 0x40020000
#define DMA_SIZE          0xC00

/*
 * SAI1 (Serial Audio Interface), per STM32H7B0.svd baseAddress
 * 0x40015800. Modeled as plain RAM for now -- same rationale as the
 * other not-yet-modeled peripherals above.
 */
#define SAI1_BASE_ADDRESS 0x40015800
#define SAI1_SIZE          0x400

/*
 * DAC1, per sdk/cmsis-device-h7/Include/stm32h7b0xx.h's DAC1_BASE
 * (0x40007400) -- audio output (piezo speaker), untouched by
 * gnw-chainloader (no audio) but touched by retro-go's audio init.
 * Was previously not mapped AT ALL (not even a RAM placeholder,
 * unlike every other not-yet-modeled peripheral here) -- a genuine
 * bus fault, found because it cascaded into an unrecoverable "Lockup:
 * can't escalate" QEMU abort (a second fault inside the first fault's
 * handler that couldn't be taken at the current priority) rather than
 * a clean, debuggable single fault. Modeled as plain RAM for now --
 * same rationale as the other not-yet-modeled peripherals above.
 */
#define DAC1_BASE_ADDRESS 0x40007400
#define DAC1_SIZE          0x400

/*
 * DAC2, per sdk/cmsis-device-h7/Include/stm32h7b0xx.h's DAC2_BASE
 * (0x58003400, SRD domain -- a separate instance from DAC1, not a
 * mirror). Same "was entirely unmapped, found via the same DAC1
 * lockup" story -- see DAC1_BASE_ADDRESS above.
 */
#define DAC2_BASE_ADDRESS 0x58003400
#define DAC2_SIZE          0x400

/*
 * TIM1, per sdk/cmsis-device-h7/Include/stm32h7b0xx.h's TIM1_BASE
 * (0x40010000) -- an entire peripheral class (all timers) was
 * previously unmapped; retro-go's MX_TIM1_Init() (Core/Src/main.c,
 * likely piezo-speaker PWM) is the only timer it initializes, found
 * via the same real-BSOD-crash-screen trail as DAC1/DAC2. Modeled as
 * plain RAM for now -- same rationale as the other not-yet-modeled
 * peripherals above.
 */
#define TIM1_BASE_ADDRESS 0x40010000
#define TIM1_SIZE          0x400

/*
 * JPEG, per STM32H7B0.svd baseAddress 0x52003000 -- hardware JPEG
 * decode, used by retro-go's menu UI for game cover art/thumbnails.
 * Was entirely unmapped (not even a RAM placeholder), found via a real
 * BSOD (`Invalid read at addr 0x52003030 ... reason: rejected` in the
 * guest-error log) while booting retro-go-real-bank1.bin. SVD's
 * documented addressBlock is only 0x400, but retro-go reads as far as
 * offset 0x42C (past that into the reserved gap before FMC at
 * 0x52004000) -- sized to cover the whole gap up to FMC rather than
 * just the documented register block, since this is only a RAM
 * placeholder anyway. Modeled as plain RAM for now -- same rationale
 * as the other not-yet-modeled peripherals above.
 */
#define JPEG_BASE_ADDRESS 0x52003000
#define JPEG_SIZE          0x1000

/*
 * RTC, per STM32H7B0.svd baseAddress 0x58004000. Modeled as plain RAM
 * for now -- same rationale as the other not-yet-modeled peripherals
 * above.
 */
#define RTC_BASE_ADDRESS 0x58004000
#define GNW_UNLOCK_BASELINE_SIZE (0x30c3a8 + 1300)
#define RTC_SIZE          0x400
/* Per sdk/cmsis-device-h7/Include/stm32h7b0xx.h's IRQn_Type. */
#define RTC_WKUP_IRQn 3
#define RTC_Alarm_IRQn 41

/*
 * LTDC (LCD-TFT Display Controller), per STM32H7B0.svd baseAddress
 * 0x50001000. Modeled as a real minimal device
 * (hw/display/gnw_h7b0_ltdc.c): Layer1-only, RGB565-only, plain
 * fixed-rate vblank/line-interrupt approximation (not real per-line
 * timing). Found by a gnw-chainloader boot reaching real LTDC init
 * after all other Phase 1 boot-path gaps were closed -- see
 * STATUS.md and gnw_h7b0_ltdc.h.
 */
#define LTDC_BASE_ADDRESS 0x50001000
/* Per sdk/cmsis-device-h7/Include/stm32h7b0xx.h's IRQn_Type: LTDC_IRQn = 88. */
#define LTDC_IRQn 88

/*
 * DMA2D (Chrom-ART Accelerator), per STM32H7B0.svd baseAddress
 * 0x52001000. Modeled as a real minimal device (hw/display/gnw_h7b0_dma2d.c):
 * R2M/M2M/M2M_PFC/M2M_BLEND modes, ARGB8888/RGB565/ARGB1555/ARGB4444/L8
 * pixel formats. Added to fix a real BusFault crash in retro-go
 * firmware's HAL_DMA2D_Init() -- DMA2D was previously completely
 * unmapped.
 */
#define DMA2D_BASE_ADDRESS 0x52001000
/* Per sdk/cmsis-device-h7/Include/stm32h7b0xx.h's IRQn_Type: DMA2D_IRQn = 90. */
#define DMA2D_IRQn 90

struct GnwH7B0State {
    SysBusDevice parent_obj;

    ARMv7MState armv7m;
    GnwH7B0RccState rcc;
    GnwH7B0PwrState pwr;
    GnwH7B0OspiState octospi1;
    GnwH7B0OspiState octospi2;
    GnwH7B0AdcState adc;
    GnwH7B0Lptim1State lptim1;
    GnwH7B0LtdcState ltdc;
    GnwH7B0Dma2dState dma2d;
    GnwH7B0SpiState spi2;
    GnwH7B0SpiState spi1;
    GnwH7B0RtcState rtc;
    GnwH7B0CrcState crc;
    GnwH7B0HashState hash;
    GnwH7B0MdmaState mdma;
    GnwH7B0RngState rng;
    GnwH7B0GpioState gpio;
    GnwH7B0DbgmcuState dbgmcu;
    GnwH7B0DwtState dwt;
    GnwH7B0FlashRState flash_r;
    GnwH7B0FmcState fmc;
    GnwH7B0CrsState crs;
    GnwH7B0OctospimState octospim;
    GnwH7B0OtfdecState otfdec1;
    GnwH7B0OtfdecState otfdec2;
    GnwH7B0CrypState cryp;
    GnwH7B0ExtiState exti;
    GnwH7B0SyscfgState syscfg;
    GnwH7B0DmaState dma;
    GnwH7B0Sai1State sai1;
    GnwH7B0DacState dac1;
    GnwH7B0DacState dac2;
    GnwH7B0Tim1State tim1;
    GnwH7B0JpegState jpeg;
    GnwH7B0TampState tamp;
    GnwH7B0WwdgState wwdg;
    GnwH7B0Tim2State tim2;
    GnwH7B0IwdgState iwdg;
    GnwH7B0Lpuart1State lpuart1;
    GnwH7B0Usart1State usart1;

    MemoryRegion itcm;
    MemoryRegion dtcm;
    /*
     * Covers AXISRAM1+2+3's whole real, genuinely contiguous ~1MB span
     * (see gnw_h7b0_soc_realize()'s comment) -- one MemoryRegion, not
     * three, despite the field name matching only the first
     * subdivision's name/base.
     */
    MemoryRegion axisram1;
    MemoryRegion ahbsram1;
    MemoryRegion ahbsram2;
    MemoryRegion srdsram;
    MemoryRegion bkpsram;
    MemoryRegion flash_bank1;
    MemoryRegion flash_bank2;
    MemoryRegion extflash;
    MemoryRegion uid;

    /* A cold power cycle clears volatile silicon state without destroying
     * the emulator process. The ITCM seed is captured at initial reset exit,
     * after loader writes and before guest execution; it models the bytes
     * supplied by the device's immutable boot path after power returns. */
    uint8_t *cold_itcm_seed;
    QEMUTimer *startup_handoff_timer;
    QEMUTimer *cold_power_on_timer;
    QEMUTimer *cold_power_release_timer;
    unsigned startup_handoff_attempts;
    bool initial_boot_handoff_pending;
    bool ui_reset_to_fault_pending;
    bool supply_present;

    Clock *sysclk;
    /* SysTick's external source is HCLK / 8, distinct from cpuclk when
     * CDCPRE or HPRE divides the H7B0 clock tree. */
    Clock *systick_refclk;

    /*
     * Optional mmap-backed persistence for flash_bank1/flash_bank2/extflash
     * (qdev string properties: "bank1-image", "bank2-image",
     * "extflash-image", e.g. -global gnw-h7b0-soc.extflash-image=foo.bin).
     * When set, that region is backed directly by the named file (created/
     * truncated to the region's full size if it doesn't already exist) via
     * memory_region_init_ram_from_file(..., RAM_SHARED, ...), so guest
     * writes (flashing, erasing) land in the file itself as they happen --
     * the same file doubles as both the boot seed and the live, persistent
     * backing store. Leave unset (the default) to keep the existing
     * behavior: anonymous RAM seeded once at boot via `-device loader`,
     * with writes never touching the source file -- e.g. for repeatable
     * testing against a known-good image without needing to re-copy it
     * after every run. For a persistent extflash image, the RDP cold-boot
     * handoff also keeps IMAGE.gnw-unlock-baseline beside it: the small
     * original span needed to decode GnWManager's staged payload after the
     * emulator process is fully stopped and relaunched.
     */
    /* Optional 96-bit UID in raw memory byte order (24 hex digits). */
    char *device_uid;
    char *sd_wiring;
    char *bank1_image;
    char *bank2_image;
    char *extflash_image;
    char *extflash_chip;
    uint64_t extflash_size;
    /* Small pre-payload extflash snapshot used to model GnWManager's
     * XOR-decoded cold-boot transfer into ITCM. */
    uint8_t *extflash_unlock_baseline;
    char *rdp_image;
    /* Opt-in snapshots keep the original session decoder baseline on reopen. */
    bool preserve_state_baseline;
    /* Start with STM32 readout protection level 1 instead of level 0. */
    bool rdp_locked;
    /* Opt-in one-instruction/one-cycle baseline; requires precise -icount. */
    bool timing_mode;
    bool x_modeled_timing;
    bool cpu_cache_dma2d_v2_profile;
    char *cpu_cache_dma2d_v2_runtime_receipt_sha256;
    char *mdma_axi1_runtime_receipt_sha256;
    bool mdma_axi1_comparison_mode;
    char *mdma_v37_bwm_fit_receipt_sha256;
    bool mdma_v37_bwm_comparison_mode;
    /* Unstable validation-only producer; zero leaves production costs off. */
    uint32_t x_timing_test_cycles_per_insn;
    bool x_timing_test_half_rate;
};

/* UI action: clear volatile device state, reset the SoC, then press POWER. */
void gnw_h7b0_soc_cold_power_cycle(void);

/* Board supply transition; caller has synchronously stopped the vCPUs. */
void gnw_h7b0_soc_set_supply(GnwH7B0State *s, bool present);

/* UI Reset on an RDP-locked device reproduces the captured reset fault. */
void gnw_h7b0_soc_ui_reset(void);

#endif
