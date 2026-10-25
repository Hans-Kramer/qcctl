<!-- SPDX-License-Identifier: CC-BY-4.0 -->
<!-- SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer -->
# qcctl

> [!CAUTION]
> **`v0.1.0` — the first release.** It writes to CPU, GPU and thermal control
> points as root, on one board and nothing else.
>
> `qcctl` and its companion [`qcstats`](https://github.com/Hans-Kramer/qcstats)
> target **one board** — the Arduino Ventuno Q (QCS8275). Nothing else is
> tested. This writes to CPU, GPU and thermal control points as root; on
> unexpected hardware the validator refuses the config and the installer
> refuses to install, but do not rely on that as a safety net.
>
> The gate in this tree compiles under `gcc`, `clang` and the `aarch64` cross
> build at `-Wall -Wextra -Werror`, and runs **740 behavioural checks** — the
> curve and ownership logic, both config parsers and their refusals, the
> apply-and-roll-back path, the flag surface, the control socket, the scripts, the
> daemon and the installer, all against a captured board — on x86-64 and on
> `aarch64`. What a
> fixture cannot show is the kernel reacting, so **37 hardware checks stay in
> `tests/board-check.sh`, run by hand on a board; the gate does not run them.**
> Every thermal and fan figure in these docs was measured by hand.
>
> Install, removal, reinstall and a cold boot have been exercised on hardware;
> removal returns the board to stock, and after a power cycle `policy2` comes up
> `schedutil` rather than the vendor's `ondemand`. **Not verified:
> suspend/resume — this board resumes from neither sleep state.** Interfaces and
> **What `0.1.0` promises, and what it does not.** Removal is the promise:
> `uninstall.sh` returns the board to what the boot-state snapshot recorded, and
> `--reset` verifies every value it restores rather than reporting what it wrote.
> The config syntax is **not** stable — it is `0.x` in the ordinary sense and may
> change in a minor release, which is why `qcctl --verify` exists and why the
> installer refuses to overwrite a config you have edited. Known gaps are in
> [`TODO.md`](TODO.md).
>
> Fit for a board you are willing to experiment with. Not for one you depend on.

Power, clock and fan profiles for the **Arduino Ventuno Q** (Qualcomm QCS8275, board
`monza`) — a command-line tool and a daemon for setting CPU, GPU and thermal policy on a
board that ships with no supported way to do it.

Its sibling [`qcstats`](https://github.com/Hans-Kramer/qcstats) samples and reports board telemetry. The division is strict and
load-bearing: **`qcstats` observes, `qcctl` acts.** `qcctl` contains no monitoring
code of its own — it consumes one `qcstats` stream and never reads a sensor directly. If a
reading is missing, that is a gap in `qcstats`, not something to fix here.

> **Legal Disclaimer**
> 
> This project is an independent personal project. It is in no way affiliated with, 
> sponsored by, or endorsed by Arduino&reg; or Qualcomm&reg;.
>
> "Arduino" and "VENTUNO" are registered trademarks of Arduino. "Qualcomm" and 
> "Dragonwing" are trademarks of Qualcomm Incorporated.
> All trademarks are the property of their respective owners. 
> **Status.** Installed, persistent across a cold boot, and running on real hardware. Every
> claim below was measured on a board; where something is inferred or untested it says so.
> There is no GUI yet — that is deliberate and will live in its own repository.

**[`BUILD.md`](docs/BUILD.md)** to compile it · **[`USAGE.md`](docs/USAGE.md)** to drive it. This file
is the overview and the rationale.

---

## What it actually fixes

The vendor ships `/etc/sysfs.d/50-dragonwing-defaults.conf`, which sets governors on
`policy0`, `policy4` and **`policy7`**. This board's cpufreq policies are **0, 2 and 4** —
there is no `policy7`. So the write silently fails and **`policy2` boots on cpufreq's
`ondemand` default**, while the other two get `schedutil`.

`policy2` is CPU2–3: the A78C pair with the **highest** maximum frequency on the chip
(2361600 kHz). The vendor leaves the fastest cores on the wrong governor.

`qcctl` supersedes that by unit ordering (`After=sysfsutils.service`) — never by editing
the vendor's file. Verified across a cold boot: `policy2` comes up `schedutil`.

Anything that keys on `cpu0` alone misses two thirds of this machine. That is the same mistake
the vendor made, and it is why everything here is addressed **per policy, never per core**.

---

## Install

Requires the cross-built binaries, the configs and the units in one directory:

```sh
sudo ./install.sh              # or --upgrade over an existing install
sudo qcctl-uninstall     # add --purge to drop /etc/qcctl and the fan baseline too
```

The installer records exactly what it created in `/var/lib/qcctl/install.manifest`,
appending **as each path is created**, so an install that dies halfway is still fully
removable. Paths that already existed are tagged `PREEXISTING` and left alone on removal —
which matters most for `qcstats`, a hard dependency belonging to a different project.

It validates the config with `qcctl-check` **before** enabling the unit, because a bad
config at boot is the one failure that needs physical access to recover from on this board.

Removal was written and tested **before** the install path. A durable change you cannot
reverse on demand is not one that should be made.

---

## Using it

```
qcctl -l                     # list profiles
qcctl -q                     # report the current control surface
qcctl -m balanced            # apply a profile
qcctl -m quiet --dry-run     # predict the writes, perform none
qcctl --reset                # restore the captured boot state
qcctl --cool-max             # fan to maximum until Ctrl-C  (no root needed)
qcctl -l --json              # machine-readable, for consumers
```

Writes need root — and the tool says so **before** attempting anything, rather than failing
partway. `--dry-run`, `-q`, `-l` and `--verify` work unprivileged. `QCCTL_SYSROOT`
relocates every path onto a fixture tree, so the whole tool can be exercised with no board
attached.

`--cool-max` is the exception that needs no privilege at all. See [Emergency cooling](#emergency-cooling).

### Shipped profiles

| | | |
|---|---|---|
| `maxperf` | all three policies pinned at fmax, GPU pinned, fan on early | 14 settings |
| `balanced` *(default)* | `schedutil` everywhere — **including `policy2`** | 12 settings |
| `quiet` | caps frequencies, runs the `quiet` curve, fan trip raised to 45 °C | 11 settings |
| `workstation` | hands the fan to the daemon so a real curve can run | 9 settings |
| `held40` | `balanced` clocks, fan holds a temperature instead of following one | 14 settings |
| `lowpower` | A55 cluster emphasis, `uclamp` capped, two cores offline | 14 settings |

Two rules in `profiles.conf`, both deliberate:

- **A setting you omit is not touched.** There is no implied default.
- **Every number carries its unit.** A bare number is a parse error. CPU frequencies are kHz
  because several OPPs are not whole MHz (2361600 kHz = 2361.6 MHz); GPU is Hz, as devfreq
  reports it.

Switching profiles is **idempotent**: settings the previous profile owned and the new one does
not mention are reverted to the boot state, so the result depends on which profile is applied
and not on the path taken to it. Applying a profile reports what it reverted, so the step is
visible rather than implied.

---

## Fan control

The board has **exactly one active trip** — 38 °C on `cpuss-0-0` — and trip points cannot be
added at runtime. One trip selects one cooling state, which is why the stock fan only ever sits
at 0 or 4 and cycles audibly about once a minute at idle.

A real curve therefore requires taking the thermal zone away from the kernel. That is not a
shortcut. The zone carries no `polling_delay`, so the kernel re-evaluates only when a trip is
crossed — and on a crossing `step_wise` re-asserts its own choice over anything userspace wrote.
Between crossings a written state does persist, which makes cooperation worse than it sounds
rather than better: the value holds until the moment it silently does not.

`fan.config` offers two **different kinds** of strategy, not one with a flag:

- **`< CURVE >`** — open loop. A temperature-to-state lookup with hysteresis. Stateless.
- **`< TARGET >`** — closed loop. Name a temperature to hold; the controller modulates the fan.
  Stateful, and needs a clock.

They need different settings, so a setting used in the wrong block is a parse error.

An unconditional **85 °C ceiling** overrides every block and is not configurable. Validation
rejects a target at or above it, *and* the controller clamps independently — so a curve built
in memory by a future GUI cannot bypass it.

### What the closed loop actually does

It hunted, badly, on first contact with hardware: bang-bang across the whole 0–10 range, 18
state changes in 160 s. The cause was **loop gain, not hysteresis** — a 2 s poll stepping one
state per poll against a slow thermal plant. Three things fixed it: a deadband, a rate limit
(`STEP_INTERVAL`) decoupled from `POLL`, and trend awareness that refuses to add cooling while
the temperature is already falling.

Measured afterwards over a 600 s saturating CPU+RAM load: **8 state changes for the whole run,
then state 6 held for ~490 s with zero corrections**, at a mean of 40.8 °C against a 40 °C
target. It finds its operating point and stops working.

With the intake deliberately obstructed it saturates at state 10 and **holds the rail for 436 s
without chattering** — but settles 5.3 °C above target, which it cannot close. That is the
measured case for auxiliary fan support, and the daemon now says so rather than failing quietly.

---

## Emergency cooling

```sh
qcctl --cool-max       # no sudo, no password
```

This needed to work without a password, because it can be a split-second decision to prevent
hardware damage. The obvious ways to grant that are all worse than the problem: a `sudoers`
rule still makes you type `sudo`; setuid hands root to a multi-purpose CLI; and a udev rule
granting write access to the thermal zone would let any local user **disable thermal
management** — the exact failure being prevented.

The asymmetry is the solution. **An unprivileged caller may ask for more cooling, never less.**
A file permission cannot express that; a request to the already-root daemon can.

So `qcctld` listens on `/run/qcctl/control`, mode **0666 on purpose** — the only
outcome a caller can cause is more cooling, so there is nothing there for permissions to
protect. The single request is the literal `COOLMAX`; there is deliberately no vocabulary for
lowering cooling or touching the zone.

**The hold is the connection.** A client asks and keeps the socket open; closing it *is* the
release — by Ctrl-C, SIGTERM, SIGKILL or a crash. A dead client cannot leave the fan pinned,
and no client can cancel another's hold. Verified on hardware by SIGKILLing the client.

With no daemon running, `--cool-max` does the work itself and **that** path needs root.

---

## Airflow health

If you put this board in an enclosure, airflow becomes the thing that fails. At a **fixed**
cooling state the pwm is fixed, so rpm variation is purely aerodynamic — and obstructing the
inlet makes the fan spin **faster**, because it moves the operating point toward shut-off where
the impeller does least work. Measured at an identical state 10: **5916–5934 rpm obstructed
against 5090–5210 clear**, 13 % apart with no overlap.

That needs a reference from *your* board in *your* enclosure, not a shipped table:

```sh
sudo qcctl --fan-baseline capture    # ~2 minutes, drives the fan
qcctl --fan-baseline show
```

Then add an `< AIRFLOW >` block to `fan.config` with a `DELTA`. The delta is **signed**, and
both directions mean something:

- **above** baseline → restricted intake, blocked airflow
- **below** baseline → the fan is not delivering: bearing wear, dust-loaded impeller

`DELTA` is floored at 5 % **by measurement, not taste**: within-state spread reached ~2.3 % in
free air, so anything lower calls ordinary noise a fault.

The capture **refuses without `--force`** if a state sits more than 25 % off our reference
figures, because recording an existing obstruction as your healthy baseline is the one failure
that makes this whole feature a silent no-op.

**Known blind spot:** at idle the fan may sit at state 0 or 1, where there is no signal — state
1's tachometer misreads on this fan. So a warning cannot be promised promptly after you change
an enclosure.

---

## The daemon

`qcctld` is **always resident**, whether or not a fan strategy is configured. Applying a
profile is something it does at startup, not its reason to exist. It owns the single `qcstats`
instance — two samplers would mean two observer effects on the quantity being displayed.

The cost was measured rather than assumed: a resident sampler at a 2 s interval is **under
~0.2 W against a 7.4 W idle, and not separable from this board's ~1.7 W idle drift**. That is a
ceiling, not a claim of zero.

It publishes `/run/qcctl/status` — written to a temp file and renamed, so a reader never
sees a partial file:

```json
{"v":1,"profile":"balanced","temp_mc":36800,
 "fan_state":null,"fan_duty_pct":0,"fan_rpm":0,"fan_rpm_valid":true,
 "drifted":false,"target_unreachable":null,"airflow":"unknown","sampler":"ok","ts":…}
```

### Read the nulls carefully

This is the part a consumer must get right. **`null` never means zero and never means "fine".**

| field | `null` means |
|---|---|
| `temp_mc` | no reading — not 0 °C |
| `fan_state` | we do not *command* the fan; `fan_rpm` still says what it is *doing* |
| `drifted` | we did not check (no profile was applied) — not "nothing drifted" |
| `target_*` | no `TARGET` strategy is running |
| `airflow` | we are not judging at all — distinct from `"unknown"`, which means we *are* judging but there is no signal right now |

`drifted` exists because the daemon applies a profile once and **never re-asserts it** — a user
who changed a governor by hand did so on purpose. So it reports the divergence instead of
fighting it, naming the first setting that no longer matches.

If the sampler dies, the daemon releases the fan first — it cannot drive a strategy blind —
then retries, publishing `sampler: "down"` with every reading `null` rather than freezing stale
numbers. If the retries fail it exits **non-zero** so systemd restarts the unit.

---

## Correctness

```sh
make check           # compiles under gcc, clang and the aarch64 cross build
make test            # everything below: 740 checks, and no board or root needed
sh tests/sh/run.sh   # 186 of them: the flag surface, scripts, daemon, installer
```

Clean under `-Wall -Wextra -Werror` at `-O2` for all three. `char` is unsigned on aarch64 and
signed on x86, among other differences, so a native-only gate cannot see a whole class of bug —
which is why the cross build is part of the gate rather than a release step.

The behavioural suite needs neither a board nor root. `QCCTL_SYSROOT` relocates every read
onto a captured tree, and the `.manifest` beside it carries the modes and owners a copied tree
cannot; `qcctld --stats-cmd` feeds the daemon a canned NDJSON stream in place of `qcstats`.
CI runs both halves on x86-64 and on `ubuntu-24.04-arm`, and a third job asserts this tree's
`STATS_SCHEMA_V` against `qcstats`' `QCSTATS_JSON_SCHEMA` — a mismatch there is otherwise
silent, because a renamed field reads as "not measured" forever.

What the gate does **not** run is `tests/board-check.sh`: 37 checks that need a real thermal
zone and a real fan — the five release paths, the watchdog, `--fan-baseline capture`'s
read-back, and an install, uninstall and reinstall cycle. It is run by hand on a board before
a release, and it leaves the board installed, the zone enabled and the fan in the kernel's own
state. See [`TODO.md`](TODO.md) for what the suite still does not cover.

---

## Limitations, stated plainly

- **No GUI.** Everything it needs exists — the status file, the control socket, a polkit action
  and `--json` output — but it will live in its own repository.
- **Suspend is untestable here, and there is no pre-sleep hook.** Two separate facts, both
  measured.

  This board comes back from nothing once suspended — not a keyboard event, not a mouse click,
  not the on-board `USER` button. Only a hard power cycle recovers it, so
  `qcctl-resume.service` ships and has never run. Its own comment says so. One untried avenue:
  the PMIC's RTC is already an enabled wakeup source with a `wakealarm`, so `rtcwake` may wake
  what the input devices cannot — it needs no backup battery, since the alarm only has to fire
  while the board is powered.

  And nothing releases the fan *before* sleep. The release path exists and is hardware-verified
  — `ExecStopPost=qcctl-fan-release` in the unit — but systemd does not stop services on
  suspend, so with a `FAN MODE curve` or `target` profile in force the zone stays `disabled`
  and the fan stays where the strategy left it. On a board that resumes, that is seconds of
  staleness; here it is until the power is pulled. A profile with no fan strategy, which
  includes the shipped `balanced`, is unaffected: the kernel holds the fan throughout.

  Two fixes were considered and neither shipped in `0.1.0`. A oneshot unit ordered
  `Before=sleep.target` would stop the daemon, letting the existing release path run — twelve
  lines, no new code, and systemd waits for it. Teaching the daemon logind's `PrepareForSleep`
  would be better still, keeping it alive across sleep with its drift baseline and airflow
  history intact; it costs a `libsystemd` link, which is no new *runtime* requirement for a
  project that already ships units and shells out to `systemctl`, but does mean an aarch64
  `libsystemd` for the cross build that the gate performs on every push. The reason for
  shipping neither is the same reason the rest of this suite exists: the resume half of either
  one cannot be demonstrated on hardware that does not resume, and this project does not ship
  behaviour it cannot show working.
- **A silent client on the control socket holds its slot.** There is no idle timeout, so
  `CONTROL_MAXCLI` (8) connections that say nothing fill the table, and a ninth `--cool-max`
  is accepted, closed without a reply, and gets no hold: one unprivileged process can stop
  another raising the fan. **Measured.** What it cannot do is lower cooling or release the
  zone — the kernel's own governor runs underneath this path throughout, and the socket has
  no vocabulary for touching it — so what is denied is the extra cooling, not the board's
  thermal protection. Deliberate: an idle timeout or a per-uid quota each adds a way for a
  hold that is *wanted* to be dropped, which is the worse failure. Pinned by
  `tests/t_control.c`, so changing it is a decision rather than a drift.
- **A zone whose cooling map binds several devices gets no floor.** Handing a zone back needs
  the state the kernel's own map asks for, and a map that binds more than one cooling device
  gives no way to tell which entry is the fan's: the entries name their devices by phandle,
  and nothing reachable from a cooling device in sysfs identifies its device-tree node.
  Rather than read the first entry and risk leaving the fan at a CPU's floor, such a zone is
  reported as having no floor, and the release leaves cooling at maximum. Every cooling map on
  the Ventuno Q binds exactly one device, so this is behaviour for other hardware.
- **DDR frequency cannot be controlled.** `ddr_frequency_mhz` is write-only and sits on an AOSS
  QMP channel where every write times out. Residency *measurement* is possible but belongs to
  `qcstats`, since there is nothing here to act on.
- **No kernel modules.** Nothing here taints the kernel.
- **One board.** Ventuno Q only. No Windows, no other hardware.

---

## License

Three licences, chosen by what the file *is*:

| | |
|---|---|
| `src/` — the tool and the daemon | **MPL-2.0** |
| `Makefile`, `scripts/`, `systemd/` — build and packaging glue | **MIT** |
| `README.md`, `docs/*.md`, `etc/*`, `polkit/*` — prose and data | **CC-BY-4.0** |

Every file carries an `SPDX-License-Identifier`. Full texts are in [`LICENSE`](LICENSE)
(MPL-2.0) and [`LICENSES/`](LICENSES/). **Code blocks inside the documentation are MIT**,
not CC-BY-4.0 — paste a snippet into your own project and you owe no attribution.

MPL-2.0 is *file-level* copyleft: patch one of these source files and publish that file.
It stops there. Linking this code into proprietary software creates no obligation to open
that software — static linking included — and new files you write alongside it stay yours.

The packaging glue is MIT on purpose: a distribution packager has to modify the installer
and the units, and should not inherit a copyleft obligation for doing so.

**No CLA.** Contributions are inbound=outbound: your patch is licensed under whatever the
file you touched already carries, and you keep your copyright.

No third-party source is in this tree, so there is nothing to attribute.

---

## Documents

| | |
|---|---|
| [`BUILD.md`](docs/BUILD.md) | compiling, cross-compiling, running the gate, installing |
| [`USAGE.md`](docs/USAGE.md) | every command, the config formats, and how to read the status |
| [`TODO.md`](TODO.md) | open items and known gaps |

Every figure quoted here was measured on this board rather than estimated.
