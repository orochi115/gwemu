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

This fork does not yet provide gnw-emu's complete desktop transport or
toolchain acceptance. Track that work in the parent project's migration plan.
