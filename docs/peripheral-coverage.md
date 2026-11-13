# Peripheral coverage

What actually has a real device model in `hw/misc/`/`hw/display/` vs. a
generic unimplemented-register stub, at a glance. For per-peripheral
register-behavior detail, read the model's own file (each one documents
its scope/rationale in its header comment) — this is an index, not a
substitute for that.

Regenerate this by hand when peripherals change; it's small enough not to
need scripted upkeep.

## Real device models (`hw/misc/`, `hw/display/`)

| Peripheral | Status | Notes |
|---|---|---|
| RCC | Real | Full clock tree: HSI/HSE/PLL1-3, SYSCLK/HCLK/pixel-clock propagation, dynamic CPU-overclock support. AHB1RSTR/APB2RSTR writes really reset the registered devices on a 0->1 edge (`HAL_DeInit()` depends on it); APB3RSTR — and so LTDCRST — is deliberately still inert, see `docs/STATUS.md`. |
| NVIC | Real | Standard ARMv7-M NVIC, with two deliberate fork-local changes. First, an `MPU_RNR`/`MPU_RBAR` write naming a region the CPU does not implement (architecturally `UNPREDICTABLE`) now also suppresses the *following* `MPU_RASR` write, which would otherwise take its region number from the stale `MPU_RNR` and reprogram an unrelated region — see `tests/qtest/armv7m-mpu-test.c`. Second, v7M Lockup no longer `cpu_abort()`s the process. A guest with no valid vector table (blank internal flash, a supported state — it is what real hardware looks like before flashing, and how gnwmanager/GDB flashing works) clears the stuck exception state, halts, and re-resets every 250ms. The loop stands down under `RUN_STATE_DEBUG` so a debugger can load and run code from RAM. A machine that appears to reset in a loop on a blank bank1 is behaving correctly, not failing. |
| LTDC | Real | Per-layer compositing, all eight PFCR formats on both layers, PA4/DAC backlight, color key, blend, dithering, CLUTs, real reload/vblank timing. A frame is published once per guest frame: firmware's `SRCR.IMR` reload followed by a `SRCR.VBR` reload that latches nothing new is one frame, not two. |
| DMA2D | Real | Per-pixel fetch for every format in use, blend/fixed-color modes. |
| JPEG | Real | Polled output-register pipeline, correct chroma subsampling (decode via `stb_image.h`). |
| CRYP | Real | AES ECB/CBC/CTR/GCM/CCM. |
| HASH | Real | MD5/SHA-1/SHA-224/SHA-256, plain and HMAC, via QEMU's own `crypto/`. |
| OTFDEC | Real | Real AES-128-CTR extflash decryption (RM0455 41.3.4), eager bulk-decrypt-to-RAM for performance. |
| SAI1 | Real | Audio path: DMA1 Stream0 transfer-complete snoop → Fifo8 → QEMU audio timer, no per-sample FIFO modeling. |
| GPIO | Real | Real pin state incl. the PA0/EXTI0 power-button release-timer edge. |
| EXTI | Real | Real edge-triggered interrupt line tracking. |
| DMA | Partial | Real per-stream transfer-completion (extended from a plain register shadow for retro-go's audio DMA path). |
| MDMA | Partial | Real SW-triggered memory-to-memory transfer completion (was a full unimplemented-device stub). |
| TIM1 | Partial | Real counter/UIF for `CR1.CEN` (was a plain register shadow). |
| TIM2 | Partial | Block model with real counting/UIF and per-instance 16/32-bit CNT/ARR (TIM2/5 are 32-bit). |
| LPTIM1 | Partial | Free-running-counter model, mirrors TIM2's `CR1.CEN`/`EGR.UG` pattern. |
| DAC | Partial | Enabled-channel DHR aliases/dual writes, untriggered and software-triggered DOR transfer (LCD-backlight control path); external trigger timing is not modeled. |
| RNG | Partial | Real `RNG_SR.DRDY` completion (was a full unimplemented-device stub). |
| CRC | Partial | Real CRC-32 unit, correct reset value. |
| DWT | Partial | Real `CYCCNT` only. |
| RTC | Partial | Real IRQ wiring on `MISR & (ALRAF|ALRBF)`, backup-domain registers correctly survive CPU-only reset. TR/DR/SSR are a real virtual-clock calendar with the hardware shadow-register lock; alarms/wakeup-timer are instant-fire approximations. |
| FLASH_R | Partial | Real sector-erase side effect against actual flash memory; `OPTSR_CUR.RDP` seeded to real factory default. |
| OSPI (x2) | Partial | Real command decoding, NOR WEL/bitwise programming/page wrap/erase gating, auto-polling, IRQ lines, and a selectable JEDEC NOR catalog with capacity-dependent SFDP density, read/program/erase opcodes, and Winbond/Spansion status handling. The SoC maps both controllers to the selected chip capacity (up to 256 MiB). |
| SPI | Partial | Real `TXP`/`RXP`/`EOT` status-flag behavior and a real SSI byte exchange with the virtual SD card. SPI1 additionally models the DMA request path (DMAMUX requests 37/38 into DMA1, `CFG1.TXDMAEN`/`RXDMAEN`, `CR1.CSTART` gating, EOT interrupt on NVIC 35) so retro-go-sd's `HAL_SPI_TransmitReceive_DMA()` SD block reads complete instead of timing out. SPI2 (LCD) is polled only, no IRQ, no DMA. |
| LPUART1 | Stub | Register-array shadow, correct reset values, no real UART behavior. |
| USART1 | Partial | Transmit only: ISR reports the transmitter always ready, TDR writes go to serial port 0. No RX. Homebrew printf console. |
| ADC (1/2) | Stub+ | Register shadow, but decodes `SQR1.SQ1` and returns per-channel values (incl. VREFINT); DMA completion notify wired. |
| PWR | Stub+ | Register shadow with boot SBF/WKUPF1, CSSF status clearing and self-clearing WKUPCR W1C. Full MCU Standby/Stop domain timing remains unmodeled. |
| TAMP | Stub | Register-array shadow; backup registers correctly excluded from CPU-only reset (real hardware behavior). |
| WWDG, IWDG | Stub | Register-array shadow, correct reset values. |
| SYSCFG, OCTOSPIM, DBGMCU, CRS, FMC | Stub | Register-array shadow, correct reset values; unused for real side effects on this board (see `docs/peripheral-coverage.md`'s "not needed" note below). |

"Stub" here always means: correct register layout and reset values (audited against the SVD/HAL source), plain read/write with no side effects — not a placeholder with wrong behavior. Several of these were previously plain `create_unimplemented_device()` log-only stubs and got promoted to real register-array models specifically because real firmware polls a status bit in them; see `CHANGELOG.md` for which.

## Not modeled at all (`create_unimplemented_device()`, log-only)

Everything else in the STM32H7B0's memory map: USART2/3/6/9/10, UART4/5/7/8, I2C1-4, SPI3/4/5/6, CAN (FDCAN/TT_FDCAN/CAN_CCU), USB OTG1_HS, SDMMC1/2 (+ their delay blocks), HRTIM, DFSDM1/2, DCMI, PSSI, COMP1, VREFBUF, OPAMP, SWPMI, MDIOS, CEC, SPDIFRX, HSEM, RAMECC, BDMA1/2, DMAMUX1/2, TIM8/15/16/17. None of these are exercised by any firmware this project boots (retro-go, stock/CFW Mario and Zelda) — confirmed by what actually gets configured/polled during real boot traces, not assumed. If a future firmware image touches one of these and hangs, that's the signal to promote it out of this list, not a standing TODO.
