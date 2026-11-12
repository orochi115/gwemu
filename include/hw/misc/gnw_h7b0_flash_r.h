/*
 * STM32H7B0 FLASH_R (embedded flash controller registers)
 *
 * Started as an auto-generated register-only stub; upgraded to a real
 * device for sector-erase side effects -- see gnw_h7b0_flash_r.c for why:
 * gnwmanager's `erase` command finished suspiciously instantly and left
 * flash content completely unchanged. The register stub had no
 * connection at all to the actual flash_bank1/flash_bank2 RAM regions, so
 * writing FLASH_CR1/CR2's SER+SNB+START bits (what HAL_FLASHEx_Erase()
 * actually does to erase a sector on real hardware) only ever changed the
 * register's own stored value -- the guest-visible flash memory itself
 * never got touched. Real hardware's flash controller autonomously wipes
 * the target sector to 0xFF as a hardware side effect of that bit
 * sequence; program (HAL_FLASH_Program) doesn't have this problem since
 * it does a direct CPU write to the memory-mapped flash address, bypassing
 * FLASH_R registers for the actual data movement entirely.
 */
#ifndef HW_MISC_GNW_H7B0_FLASH_R_H
#define HW_MISC_GNW_H7B0_FLASH_R_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_GNW_H7B0_FLASH_R "gnw-h7b0-flash_r"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0FlashRState, GNW_H7B0_FLASH_R)

#define GNW_H7B0_FLASH_R_SIZE 0x1000

bool gnw_h7b0_flash_r_flush_rdp(GnwH7B0FlashRState *s, Error **errp);

struct GnwH7B0FlashRState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t regs[GNW_H7B0_FLASH_R_SIZE / 4];

    /* Set post-realize by gnw_h7b0_soc.c via gnw_h7b0_flash_r_set_banks() --
     * the actual RAM-backed flash content a CR-triggered erase must modify.
     * NULL until wired up (matches instantiation order: flash_bank1/2 are
     * created before flash_r in the SoC's realize()). */
    MemoryRegion *bank1_mr;
    MemoryRegion *bank2_mr;

    /* Opt-in command-erase timing. The test profile is arbitrary; the
     * conservative profile uses datasheet maxima and is not calibrated. */
    bool timing_mode;
    bool test_erase_timing;
    bool conservative_erase_timing;
    QEMUTimer *erase_timer;
    bool erase_pending;
    bool erase_bank;
    uint8_t erase_bank_index;
    uint32_t erase_sector;
    uint32_t erase_profile_id;

    bool initial_locked;
    char *rdp_image;
    bool rdp_initialized;
    uint8_t rdp_value;
    uint8_t optkey_stage;
};

/* Wire the FLASH_R device to the actual bank memory it controls. Must be
 * called after both `bank1`/`bank2` are realized (memory_region_get_ram_ptr
 * requires a realized RAM region) and before the guest can plausibly reach
 * an erase-triggering CR write (i.e. any time before CPU reset/resume). */
void gnw_h7b0_flash_r_set_banks(GnwH7B0FlashRState *s, MemoryRegion *bank1,
                                 MemoryRegion *bank2);
void gnw_h7b0_flash_r_set_initial_locked(GnwH7B0FlashRState *s, bool locked);
void gnw_h7b0_flash_r_set_rdp_image(GnwH7B0FlashRState *s,
                                    const char *image);

#endif
