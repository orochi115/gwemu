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
parent desktop and uses independent working images. Power controls, complete
instant states, supply-domain cold boot/software SPI, and full firmware/audio acceptance are
still pending. Track them in the parent migration plan.

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
