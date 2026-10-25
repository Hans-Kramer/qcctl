<!-- SPDX-License-Identifier: CC-BY-4.0 -->
<!-- SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer -->
# qcctl

Power, clock and fan profiles for the **Arduino Ventuno Q** (Qualcomm QCS8275, board
`monza`) — a command-line tool and a daemon for setting CPU, GPU and thermal policy on a
board that ships with no supported way to do it.

Its sibling [`qcstats`](https://github.com/Hans-Kramer/qcstats) samples and reports board
telemetry. The division is strict: **`qcstats` observes, `qcctl` acts.** `qcctl` contains no
monitoring code of its own — it consumes one `qcstats` stream and never reads a sensor
directly.

> **Legal Disclaimer**
> 
> This project is an independent personal project. It is in no way affiliated with, 
> sponsored by, or endorsed by Arduino&reg; or Qualcomm&reg;.
>
> "Arduino" and "VENTUNO" are registered trademarks of Arduino. "Qualcomm" and 
> "Dragonwing" are trademarks of Qualcomm Incorporated.
> All trademarks are the property of their respective owners. 

**[`BUILD.md`](docs/BUILD.md)** to compile it · **[`USAGE.md`](docs/USAGE.md)** to drive it.
This file is the overview.

---

## Install

`install.sh` works from a **staging directory** holding the binaries, the configs and the
units together. `make stage` assembles one, and needs its sibling `qcstats` built for the
same architecture — it is a hard dependency, not an optional one:

```sh
make stage                                # cross-built for the board, into /tmp/qcctl-stage
sudo sh /tmp/qcctl-stage/install.sh       # --upgrade over an existing install
sudo qcctl-uninstall                      # --purge also drops /etc/qcctl and the fan baseline
```

On the board itself there is no cross toolchain, so `make stage-native` is the target to
use there. [`BUILD.md`](docs/BUILD.md) has both paths in full.

---

## Shipped profiles

| profile | what it does | settings |
|---|---|---|
| `maxperf` | all three policies pinned at fmax, GPU pinned, fan on early | 14 settings |
| `balanced` *(default)* | `schedutil` everywhere — **including `policy2`** | 12 settings |
| `quiet` | caps frequencies, runs the `quiet` curve, fan trip raised to 45 °C | 11 settings |
| `workstation` | hands the fan to the daemon so a real curve can run | 9 settings |
| `held40` | `balanced` clocks, fan holds a temperature instead of following one | 14 settings |
| `lowpower` | A55 cluster emphasis, `uclamp` capped, two cores offline | 14 settings |

Switching profiles is **idempotent**: settings the previous profile owned and the new one does
not mention are reverted to the boot state, so the result depends on which profile is applied
and not on the path taken to it. Applying a profile reports what it reverted, so the step is
visible rather than implied.

---

## Fan control

`fan.config` offers two **different kinds** of strategy, not one with a flag:

- **`< CURVE >`** — open loop. A temperature-to-state lookup with hysteresis. Stateless.
- **`< TARGET >`** — closed loop. Name a temperature to hold; the controller modulates the fan.
  Stateful, and needs a clock.

They need different settings, so a setting used in the wrong block is a parse error.

---

## Airflow health

If you put this board in an enclosure, airflow becomes the thing that might fail.

It needs a reference from *your* board in *your* enclosure, not a shipped table:

```sh
sudo qcctl --fan-baseline capture         # about 162 seconds, states 2..10
```

`fan.config`'s `< AIRFLOW >` block sets the delta that judges against it; comment the block
out and nothing is judged.

---

## The daemon

`qcctld` is **always resident**, whether or not a fan strategy is configured. Applying a
profile is something it does at startup, not its reason to exist. It owns the single `qcstats`
instance — two samplers would mean two observer effects on the quantity being displayed.

---

## Documentation

- [`docs/BUILD.md`](docs/BUILD.md) — compiling, cross-compiling, the test suite, installing
- [`docs/USAGE.md`](docs/USAGE.md) — every command, both config formats, and how to read the
  status
- [`CHANGELOG.md`](CHANGELOG.md) and [`TODO.md`](TODO.md) — what changed, and what is open

---

## Scope

**This targets one board.** It writes to CPU, GPU and thermal controls, so it needs root,
and `install.sh` refuses outright on a machine with no `cpuss-0-0-thermal` zone rather than
installing something that cannot work. Past that there is no board detection: a control that
is absent is treated as absent, and nothing is guessed.

## Limitations

- **One board.** Ventuno Q only. No Windows, no other hardware.
- **No pre-sleep hook.** systemd does not stop services on suspend, so with a `curve` or
  `target` profile in force the zone stays `disabled` across sleep and the fan stays where
  the strategy left it. A profile with no fan strategy — including the shipped `balanced` —
  is unaffected: the kernel holds the fan throughout. This board does not resume from
  suspend at all, so `qcctl-resume.service` ships and has never run.
- **A silent client holds a control-socket slot.** There is no idle timeout, so eight
  connections that say nothing fill the table and a ninth `--cool-max` gets no hold. What it
  cannot do is lower cooling or release the zone; the kernel's governor runs underneath that
  path throughout.
- **A zone whose cooling map binds several cooling devices gets no floor**, and the release
  then leaves cooling at maximum. Every cooling map on the Ventuno Q binds exactly one
  device, so this is behaviour for other hardware.
- **DDR frequency cannot be controlled.** `ddr_frequency_mhz` is write-only and every write
  to it times out.
- **No GUI, and no kernel modules.** Nothing here taints the kernel.

## License

`MPL-2.0` for the source, `CC-BY-4.0` for the documentation, `MIT` for the build and CI
glue. See [`LICENSE`](LICENSE) and [`LICENSES/`](LICENSES/).
