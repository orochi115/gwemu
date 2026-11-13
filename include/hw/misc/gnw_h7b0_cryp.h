/*
 * STM32H7B0 CRYP (AES crypto processor) model (Nintendo Game & Watch)
 *
 * Scope: real AES-128/192/256 GCM (encrypt and decrypt, hardware's
 * software-facing register protocol per RM0455 and
 * sdk/stm32h7xx-hal-driver/Src/stm32h7xx_hal_cryp.c's
 * CRYP_AESGCM_Process()/CRYP_GCMCCM_SetHeaderPhase()/
 * HAL_CRYPEx_AESGCM_GenerateAuthTAG()) -- init/header/payload/final
 * phase state machine, DIN/DOUT 4-word block FIFOs, INIT-phase H
 * (hash subkey) and J0 (initial counter block, supplied by firmware
 * as a full 128-bit IV, not derived from a shorter nonce -- this is
 * the only IV convention this firmware family's HAL config uses),
 * header-phase GHASH-only accumulation, payload-phase AES-CTR
 * keystream XOR + GHASH update (over ciphertext, matching real
 * hardware and firmware's own ALGODIR-dependent HAL logic), and
 * final-phase length-block processing producing the authentication
 * tag in DOUT. CRYPEN self-clears after the INIT phase (real
 * hardware autonomously completes H/J0 setup and clears CRYPEN;
 * firmware polls for this), matching this project's "instant
 * complete, no real timing model" convention used for OSPI/OTFDEC/
 * DMA2D.
 *
 * Found via 2026-07-12 lockstep tracing: stock Mario CFW's boot
 * sequence starts a CRYP AES-GCM decrypt from its CRYP1_IRQn (IRQ 79)
 * ISR and sleeps until that ISR's buffer-drain loop sets a completion
 * flag -- with CRYP a plain `create_unimplemented_device` (reads as
 * zero, never interrupts), the ISR never fires and boot hangs forever
 * in that wait.
 *
 * DATATYPE implements no-swap, halfword, byte and bit reversal on
 * DIN/DOUT; keys and IVs retain their register byte order. GCM FINAL
 * length words bypass DATATYPE as on H7 rev.B and above.
 *
 * NOT modeled: DES/TDES algorithms, DMA-driven DIN/DOUT (DMACR is a
 * plain shadow), and the CSGCMCCMxR/CSGCMxR context-swap
 * registers (plain shadow -- only needed if firmware suspends and
 * resumes a GCM operation across an unrelated CRYP use, not observed
 * in this boot path).
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
#ifndef HW_MISC_GNW_H7B0_CRYP_H
#define HW_MISC_GNW_H7B0_CRYP_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_GNW_H7B0_CRYP "gnw-h7b0-cryp"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0CrypState, GNW_H7B0_CRYP)

#define GNW_H7B0_CRYP_SIZE 0x400

/* Register offsets, per CRYP_TypeDef (sdk/cmsis-device-h7/Include/stm32h7b0xx.h). */
#define GNW_H7B0_CRYP_CR       0x00
#define GNW_H7B0_CRYP_SR       0x04
#define GNW_H7B0_CRYP_DIN      0x08
#define GNW_H7B0_CRYP_DOUT     0x0C
#define GNW_H7B0_CRYP_DMACR    0x10
#define GNW_H7B0_CRYP_IMSCR    0x14
#define GNW_H7B0_CRYP_RISR     0x18
#define GNW_H7B0_CRYP_MISR     0x1C
#define GNW_H7B0_CRYP_K0LR     0x20
#define GNW_H7B0_CRYP_K0RR     0x24
#define GNW_H7B0_CRYP_K1LR     0x28
#define GNW_H7B0_CRYP_K1RR     0x2C
#define GNW_H7B0_CRYP_K2LR     0x30
#define GNW_H7B0_CRYP_K2RR     0x34
#define GNW_H7B0_CRYP_K3LR     0x38
#define GNW_H7B0_CRYP_K3RR     0x3C
#define GNW_H7B0_CRYP_IV0LR    0x40
#define GNW_H7B0_CRYP_IV0RR    0x44
#define GNW_H7B0_CRYP_IV1LR    0x48
#define GNW_H7B0_CRYP_IV1RR    0x4C

#define CRYP_CR_ALGODIR         (1U << 2)
#define CRYP_CR_ALGOMODE_MASK   0x00080038U /* bits[3:5] folded with bit19 */
#define CRYP_CR_ALGOMODE_AES_ECB 0x00000020U
#define CRYP_CR_ALGOMODE_AES_CBC 0x00000028U
#define CRYP_CR_ALGOMODE_AES_CTR 0x00000030U
#define CRYP_CR_ALGOMODE_AES_GCM 0x00080000U
#define CRYP_CR_ALGOMODE_AES_CCM 0x00080008U
/* AES key-preparation mode: real HAL's CRYP_AES_Decrypt() briefly
 * switches ALGOMODE to this value (while CRYPEN is enabled) before an
 * ECB/CBC decrypt, to have hardware derive the decryption key schedule;
 * see the .c file's CR-write handler comment for why this model must
 * recognize it explicitly instead of falling through to "unimplemented". */
#define CRYP_CR_ALGOMODE_AES_KEY 0x00000038U
#define CRYP_CR_DATATYPE_SHIFT  6
#define CRYP_CR_DATATYPE_MASK   (0x3U << CRYP_CR_DATATYPE_SHIFT)
#define CRYP_CR_KEYSIZE_SHIFT   8
#define CRYP_CR_KEYSIZE_MASK    (0x3U << CRYP_CR_KEYSIZE_SHIFT)
#define CRYP_CR_FFLUSH          (1U << 14)
#define CRYP_CR_CRYPEN          (1U << 15)
#define CRYP_CR_GCM_CCMPH_SHIFT 16
#define CRYP_CR_GCM_CCMPH_MASK  (0x3U << CRYP_CR_GCM_CCMPH_SHIFT)
#define CRYP_CR_NPBLB_SHIFT     20
#define CRYP_CR_NPBLB_MASK      (0xFU << CRYP_CR_NPBLB_SHIFT)

#define CRYP_GCM_CCMPH_INIT     0
#define CRYP_GCM_CCMPH_HEADER   1
#define CRYP_GCM_CCMPH_PAYLOAD  2
#define CRYP_GCM_CCMPH_FINAL    3

#define CRYP_SR_IFEM  (1U << 0)
#define CRYP_SR_IFNF  (1U << 1)
#define CRYP_SR_OFNE  (1U << 2)
#define CRYP_SR_OFFU  (1U << 3)
#define CRYP_SR_BUSY  (1U << 4)

#define CRYP_RISR_INRIS  (1U << 0)
#define CRYP_RISR_OUTRIS (1U << 1)

struct GnwH7B0CrypState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    qemu_irq irq;
    uint32_t regs[GNW_H7B0_CRYP_SIZE / 4];

    /* GCM working state, latched at INIT phase (CRYPEN 0->1 while
     * GCM_CCMPH==INIT) from the current key/IV registers -- kept
     * separate from the live registers since firmware's own HAL
     * workarounds rewrite IV1RR mid-operation for partial final
     * blocks (see the .c file's payload-phase handling). */
    uint8_t round_keys[15][16];
    int num_rounds;
    uint8_t hash_subkey[16];   /* H = AES_Encrypt(key, 0^128) */
    uint8_t ghash[16];         /* Y, the running GHASH accumulator (GCM) or
                                * running CBC-MAC accumulator (CCM) -- the
                                * two modes never run concurrently on one
                                * device instance, so this field is reused
                                * rather than duplicated. */
    uint8_t counter[16];       /* current CTR block (starts at J0(GCM)/CTR1(CCM)) */
    uint8_t j0[16];            /* GCM only: the ORIGINAL loaded J0, preserved
                                * separately from `counter` (which advances
                                * during PAYLOAD) -- HAL_CRYPEx_AESGCM_
                                * GenerateAuthTAG()'s tag mask needs AES_K(J0)
                                * specifically, confirmed against real
                                * hardware (see case_cryp_aes_gcm_correct.c's
                                * header comment: "the tag generation...
                                * independently derives AES_K(J0)... despite
                                * the payload's counter register having been
                                * loaded with J0+1, not J0"). */

    /* DIN/DOUT 4-word block buffers -- real hardware's FIFOs are
     * deeper, but since processing is modeled as instant (no timing
     * emulation, same convention as this project's other "instant
     * complete" peripherals), a single pending block is sufficient:
     * a block is processed as soon as its 4th word is written. */
    uint32_t din_words[4];
    int din_count;
    uint32_t dout_words[4];
    int dout_count;   /* words remaining to be read via DOUT */
    int dout_total;   /* words originally produced (for OFNE calc) */
};

#endif
