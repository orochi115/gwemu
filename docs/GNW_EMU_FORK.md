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

The initial input-only batch did not model battery detachment, power loss at
0percent, charger CE
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

The existing native `gnw-cold-cycle` HMP command now captures its immutable
ITCM seed at the initial system reset's exit phase, after loader/ROM reset
holds and before guest execution. Capturing on the first cold-cycle request
instead retained guest working data or a decoded unlock payload as boot
state. Full removal also clears all TAMP backup registers using the existing
backup-domain reset hook; normal system reset continues to preserve them.
Internal/external Flash, option bytes and UID remain nonvolatile.

Parent power regressions cover loaded ITCM bytes, cleared unloaded ITCM,
warm/cold backup-register distinctions and repeated POWER pulses. Optional
copied-ROM acceptance is in `tests/test_gwemu_cold_firmware.py`. After backup
state loss, patched Zelda OFW requires completing its PRESS TIME screen
before the Left+GAME patch can enter Retro-Go again.

That cold-only batch remained an explicit in-process reset approximation
with a delayed POWER press. It did not provide battery presence or sustained
off-state CPU gating. Physical RTC/VBAT behavior and protection transitions
remain unverified. Existing direct SRAM memset/ITCM memcpy translation-cache
behavior remains a separate unproven concern; this narrow patch does not
change it or claim complete cold-start/savestate fidelity.

The opt-in power model now has writable QOM `/machine` `battery-present`
(default true) and read-only `powered`. Supply availability is battery
presence OR USB, independently of percentage: connected 0percent still
counts as a source. With no battery, its saved percentage does not charge or
drain, ADC channel4 reads zero and CHG is inactive; USB PGOOD retains its
existing polarity. Switching between redundant sources does not reset.

Losing the final source synchronously stops vCPUs, cancels pending startup
handoff/POWER pulses and the UI warm-fault request, clears volatile SRAM and
TAMP, and resets the CPU/peripherals. Restoring a source synchronously cold
resets and schedules the existing POWER pulse but stays paused: only an
explicit continue runs firmware. A transition latch closes the execution
gate before `vm_stop` may release BQL and keeps it closed through reset.
If VM stop/device flushing reports a failure, the source/reset change stays
applied and QMP reports the error; it never rolls back to running without
power. Initial no-supply startup suppresses handoff and clears backup state.

A default-permissive `MachineClass.can_run` check guards the central
`vm_prepare_start` before RESUME/tick/runstate changes; only this board's
enabled model vetoes execution. QMP/HMP continue returns a supply error.
GDB c/s/C/vCont handlers return E22 before arming a step, while detach returns
one OK and central silent resume denial keeps the device stopped. Debug
reads and reconnect remain usable while off. An explicit HMP cold cycle
cannot supply an unpowered device. Parent `tests/test_gwemu_supply.py`
checks actual TCG SRAM execution, protocol denial, detach/reconnect,
back-to-back restore/continue, source combinations and unrelated-machine
compatibility; copied-firmware acceptance remains in the parent.

This is a discrete source/execution model. It does not implement charger CE
or current limits, voltage/chemistry cutoffs, always-on RTC/VBAT domains,
physical protection transitions or sudden-loss SD consistency. QEMU virtual
time pauses while unpowered. Debugger memory writes remain available as
debug operations. Full VMState/Flash rollback and desktop battery controls
are separate work.

The desktop RTC integration opts in with three globals:
`gnw-h7b0-rtc.use-rtc-clock=on`, `gnw-h7b0-rtc.preserve-calendar=on`,
`gnw-h7b0-rtc.sync-host=off`. Calendar, SSR, shadow snapshots, alarms A/B
and wakeup use QEMU's selected `rtc_clock`: host mode ticks while paused;
`-rtc ...,clock=vm` advances on virtual time. `initial-epoch` still supplies
an explicit UTC starting calendar; the default seed is host-local time.
Warm reset preserves the anchor but resets control registers and cancels
timers. Explicit cold/supply-domain reset invalidates the anchor and
synchronously cancels host-clock timers. RCC BDRST also clears RTC when
preserve-calendar is enabled, keeping the default TAMP-only callback intact.

With use-rtc-clock and sync-host disabled, INIT captures/freezes the calendar
and cancels calendar alarms. Writes to TR/DR are staged and committed once
on INIT exit, then alarms are rescheduled. The independent wakeup counter
continues during INIT. Warm reset discards incomplete staged writes. Direct
TR/DR writes outside INIT retain immediate reanchoring. Host-local writes
now infer daylight saving time (`tm_isdst=-1`), fixing summer clock setting.
VMState v6 versions the initialization flag; old anchors remain readable.
This does not establish whole-board/Flash rollback or cross-policy states.

All new globals default off except the existing sync-host write suppression,
so upstream's virtual-only/reset-seeded behavior remains the default.
Parent RTC tests cover boundary/shadow/calendar setting, reset/source
arbitration and paused host timers. Copied-ROM tests compare both kernels'
startup calendar and faithful post-boot INIT setting on four image sets.
Gwemu enters Retro-Go bank2; legacy compares the existing OFW entry on the
two patched image sets, as in the parent migration baseline.
Session RTC remains startup configuration: fixed sessions reopen at their
configured epoch, host sessions reopen at current host time. Physical VBAT,
oscillator shutdown, 12-hour mode and cycle-accurate protection remain outside
this compatibility batch; an unpowered host-mode RTC is not a physical
backup-domain simulation.
