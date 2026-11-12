# gnw-emu integration fork

This repository is derived from [slash-proc/gwemu](https://github.com/slash-proc/gwemu),
branch `gnw-h7b0`. The initial integration baseline is
`3bfce5fd26a402660a8526b3a7ffc92048c3966a` (gwemu v0.0.29).
Upstream history and licenses are preserved. GitHub permits only one fork
per user in a fork network; this independent repository keeps the new
gwemu integration separate from the legacy `orochi115/qemu` fork.

The `gnw-emu` branch contains small integration fixes. The first fix bounds
the unlock baseline copy by the configured NOR capacity. Mario's 1 MiB
mapping is smaller than the shared Zelda unlock span; reading the entire
span previously accessed memory outside that mapping during initialization.
The baseline sidecar retains its existing size; unavailable bytes are filled
with the erased Flash value, `0xff`.

Native integration regressions live in the parent `orochi115/gnw-emu`
repository at `tests/test_gwemu.py`.
They exercise QMP initialization with 1 MiB and 4 MiB images, the persisted
unlock baseline, and unchanged source contents. Run them against this build
with `GWEMU_BINARY=/absolute/path/to/qemu-system-arm`.

The developer desktop preview adds a standalone GNWF v1 display listener
and child-owned session lock, without copying the legacy STM32 model:

- `-M gnw-h7b0,frame-endpoint=127.0.0.1:PORT,session-lock=PATH`
  connects only to a decimal loopback port in 1..65535. The display streams
  the composed 640x480 surface as RGBA with ACK backpressure. The lock is
  acquired before writable images are initialized and held until process exit.
- `-global gnw-h7b0-soc.uid=24HEX` supplies the 12 raw UID bytes before
  realization; absent this property, the upstream synthetic UID remains.
- `-global gnw-h7b0-sai1.audio-muted=on` initializes host mute. QMP can
  update `audio-muted` at `/machine/soc/sai1`; voice recreation preserves it.
  Guest DMA continues while muted.

Parent regressions in `tests/test_gwemu_desktop.py` check real composed pixels,
partial ACKs, invalid endpoints, UID bytes, locking, and nonzero/zero WAV PCM
across mute and voice recreation. The preview is explicitly selected in the
parent desktop and uses independent working images. Complete instant states,
supply-domain cold boot, and full firmware/audio acceptance are still pending.
Track them in the parent migration plan.

The next preview batch adds an explicit board-input approximation:
`-M gnw-h7b0,power-model=on,battery=50,usb-power=off,battery-life-min=480,charge-time-min=180`.
Power modeling defaults off, retaining upstream's fixed full ADC/PGOOD input.
When enabled, QOM `/machine` `battery` (0..100) and `usb-power` are writable;
the enable flag and periods are startup-only. Channel4 on ADC1/2 reads the
Retro-Go BQ24072 driver's 11000..13000 scale, PA2 PGOOD and PE7 CHG are
active-low and propagate through existing EXTI routing. Other ADC channels,
VREFINT and the existing mode-sensitive PC8 strap keep their behavior.
GPIO/ADC reset retains these external inputs. Virtual time drives charging
and draining, with zero periods disabling the respective change.

This does not model battery detachment, power loss at 0percent, charger CE
current limits, or real battery chemistry. Parent `tests/test_gwemu_power.py`
also exercises the existing `gnw-cold-cycle` HMP entry, with explicit limits
on its SRAM/internal-Flash/UID/POWER-timing evidence. Desktop normal shutdown
freezes guest time, reads the power state, and saves it only after successful
exit and file synchronization; readback failure still terminates the child
and reports uncertain saving.

Software SPI SD is selected explicitly with startup-only machine property
`-M gnw-h7b0,sd-wiring=ospi`; `spi1` is the default and preserves the existing
dedicated SPI1 polled/DMA behavior. The `ospi` name follows the parent profile:
this is GPIO bit-banging on shared Flash pins, not an SD device attached to
the hardware OCTOSPI peripheral. PB2 is CLK, PB1 MOSI, PD12 MISO, and PE11
high selects SD (low selects NOR Flash). CLK/MOSI/select must be configured
as GPIO outputs; AF traffic is ignored. PD12 is driven only in digital-input
mode, retaining output loopback and other pin readbacks.

A small mode-0 bridge owns the existing SPI1 SSI bus in this mode; SPI1
TXDR/DMA return `0xff` without consuming card bytes. The same upstream
`ssi-sd`/`sd-card` devices and `-drive if=sd,format=raw,file=...` backend handle
the protocol and persistent image. No legacy STM32/SD parser is copied.
Since SSI exchanges complete bytes, a MOSI byte is submitted at its eighth
rising edge and its reply is shifted during the next byte. Current MISO
remains stable until the falling edge, including the eighth bit. This adds
one byte of response latency, tolerated by the firmware's response polling.

Byte-aligned PE11 deselect/reselect preserves the pending response and
upstream command/data state: Retro-Go toggles select between individual
response reads. Incomplete outgoing bytes are abandoned on deselection or
loss of output mode; a partially consumed response is discarded. This does
not promise arbitrary mid-command recovery or real card power cycling;
upstream `ssi-sd` has no CS-abort callback. System reset clears the bridge's
shift state and resets the upstream card devices without erasing the image.
The bridge state is included in GPIO VMState; this alone does not provide
the parent's complete Flash rollback required for desktop instant states.

Parent `tests/test_gwemu_softspi.py` covers default/explicit SPI1 and software
SPI initialization, patterned sector reads/writes/reopen, per-byte select
gaps, absent card, partial-byte cancellation, reset, AF/mode changes,
duplicate clock writes, eighth-bit stability, and startup validation.

The software driver's CMD18/CMD12 multi-sector sequence also exposed a
small upstream SSI SD bug: CMD12 can interrupt a partly clocked next block,
but the adapter retained its byte count and CRC. A subsequent CMD17 then
returned fewer than 512 data bytes, substituting CRC bytes at the end.
The adapter now clears that interrupted read count/CRC when accepting
CMD12. Regression tests cover both hardware and software SPI, three
patterned sectors, stop/read-again, and CMD9/CMD10 data CRCs.
