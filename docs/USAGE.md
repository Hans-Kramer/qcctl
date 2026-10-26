<!-- SPDX-License-Identifier: CC-BY-4.0 -->
<!-- SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer -->
# Using `qcctl`

Everything below was run on a board. Where a thing is untested it says so.

---

## Look before you change anything

```sh
qcctl -q          # the control surface as it is now
qcctl -l          # the profiles in the config
```

Neither needs root. `-q` reports three CPU policies, the GPU, the fan state and, if any are
offline, how many cores:

```
board: Qualcomm Technologies, Inc. Monaco Monza addons
  policy0  cpus=0,1  schedutil    902400 - 2112000 kHz
  policy2  cpus=2,3  ondemand     940800 - 2361600 kHz
  policy4  cpus=4,5,6,7  schedutil    844800 - 1958400 kHz
  gpu      simple_ondemand 479000000 - 877000000 Hz
  fan      state 4/10
```

> **If `policy2` says `ondemand` there, that is the bug this tool exists to fix.** The
> vendor's own config names a `policy7` that does not exist on this board, so `policy2` —
> CPU2–3, the pair with the *highest* maximum frequency — never gets set and falls back to
> cpufreq's default.

---

## Apply a profile

```sh
sudo qcctl -m balanced
qcctl -m quiet --dry-run          # show the writes, perform none
```

`--dry-run` needs no root and touches nothing, so it is the safe way to see what a profile
would do. It is *predictive*: if a profile names an attribute this kernel does not expose,
the dry run fails and names the path, rather than reporting success and letting the real
apply discover it.

| profile | what it does |
|---|---|
| `maxperf` | all three policies pinned at their maximum, GPU pinned, fan on early |
| `balanced` | `schedutil` everywhere, including `policy2`. The default |
| `quiet` | caps frequencies, runs the `quiet` curve, and raises the fan trip to 45 °C |
| `workstation` | hands the fan to the daemon so a real curve runs |
| `held40` | `balanced` clocks, but the fan holds a temperature instead of following one |
| `lowpower` | A55-cluster emphasis, `uclamp` capped, two cores offline |

Switching is **idempotent**: anything the previous profile set that the new one does not
mention is returned to the boot state, so where you end up depends on the profile you
applied and not on the order you got there.

### Undo

```sh
sudo qcctl --reset
```

Restores the snapshot taken before the first profile was applied this boot — the values
that were *actually* there, not values believed to be stock.

---

## Write your own profile

`/etc/qcctl/profiles.conf`. Two rules, both deliberate:

- **A setting you leave out is not touched.** There is no implied default, so a partial
  profile is a legitimate thing to write.
- **Every number carries its unit.** A bare number is a parse error. CPU frequencies are
  kHz because several of this board's operating points are not whole MHz
  (2361600 kHz = 2361.6 MHz); the GPU is Hz, as `devfreq` reports it. With both units in
  play a unitless number is genuinely ambiguous.

```
< PROFILE ID=5 NAME=mine >
  CPU_POLICY 0  GOVERNOR schedutil  MAX 1497600kHz
  CPU_POLICY 2  GOVERNOR schedutil  MAX 1497600kHz
  GPU           GOVERNOR powersave
>
```

Check it before trusting it (no root needed):

```sh
qcctl --verify -c /etc/qcctl/profiles.conf
```

`--verify` does two separate things: it parses and validates the file, and then it *plans*
every profile against the running kernel. The second half is what catches a config that is
perfectly well-formed but names a knob this board no longer has — after a kernel bump, say.

The two failures get **different exit codes**, because they call for different fixes:

| exit | meaning | what to do |
|---|---|---|
| 0 | parses, validates, and every profile is applicable here | nothing |
| 2 | the file does not parse | fix the syntax at the reported line |
| 3 | it parses, but is not a usable config — a bad setting, or no profile marked `DEFAULT` | fix what the message names |
| 4 | the file is **fine**; this kernel does not expose something it names | not a config bug — see below |

Exit 4 is the one worth knowing about. Nothing is wrong with your file: the board changed
under it. `install.sh` treats it separately for exactly this reason, so an install after a
kernel bump does not send you hunting a syntax error that is not there.

A profile is applied **whole or not at all**. If a write fails partway, the writes already
made are rolled back — leaving the machine in a state no config describes is the one
outcome worse than failing.

### Change which profile is the default

`DEFAULT` is the profile the daemon applies when it starts with no selection recorded for
this boot — so it is what your board comes up on after a power cycle. It is marked in
`/etc/qcctl/profiles.conf` with one word on the `< PROFILE >` line:

```
< PROFILE ID=1 NAME=balanced DEFAULT >      # before
< PROFILE ID=2 NAME=quiet DEFAULT >         # after
```

To move it, edit the file and move the word — remove `DEFAULT` from the profile that has it,
and add it to the one you want:

```sh
sudo nano /etc/qcctl/profiles.conf          # or your editor of choice
qcctl --verify                              # no root needed; checks before you commit
sudo systemctl restart qcctld
```

**Exactly one profile may be marked `DEFAULT`.** Two is a config error and the daemon will
not start:

```
/etc/qcctl/profiles.conf:0: more than one profile marked DEFAULT
```

**Removing `DEFAULT` from every profile is caught too**, which is the mistake moving it by
hand invites — delete the word, forget to add it back, and the daemon has nothing to apply.
`qcctl --verify` fails with exit **3**:

```
/etc/qcctl/profiles.conf: no profile is marked DEFAULT
  The file is well-formed, but the daemon starts with no -m and so has nothing to apply:
  it would refuse to start. Add DEFAULT to one profile's < PROFILE ... > line.
```

`qcctl-check` reports the same thing, so `install.sh` catches it **before** enabling the
unit rather than leaving you to discover it at the next restart.

> A config with no `DEFAULT` is still legal if you always name a profile explicitly —
> `qcctld -m <name>` and `qcctl -m <name>` never consult it. It is the *installed* setup
> that needs one, because the systemd unit carries no `-m`. That is why the parser accepts
> it and the two "will this work as installed?" tools do not.

Your edit is safe across upgrades. `profiles.conf` is treated as **yours**: `install.sh`
never overwrites it, and a new release puts its own copy at `profiles.conf.dist` instead.

#### Making the new default take effect now

Restarting the daemon applies the new `DEFAULT` **only if no profile is selected for this
boot**. If you have run `qcctl -m <name>` since the last power cycle, that selection wins on
a restart — by design, so a crash cannot silently change your profile. Check which you have,
in the daemon's published status:

```sh
grep -o '"profile_source":"[a-z-]*"' /run/qcctl/status
```

- `"default"` — restarting the daemon is enough.
- `"selected"` — your choice is in force. Either select the new default explicitly with
  `sudo qcctl -m <name>`, or reboot, which clears the selection and comes up on `DEFAULT`.

`qcctl -m <name>` says this itself when the profile you applied is not the `DEFAULT` one:

```
'quiet' holds until the next power cycle. To have the board come up on it,
mark it DEFAULT in /etc/qcctl/profiles.conf
```

It names the config file it was actually given, and says nothing when you apply the
`DEFAULT` profile — there would be nothing to change.

#### `DEFAULT` and `FALLBACK` are different questions

| | applied when |
|---|---|
| `DEFAULT` | a clean start — no selection recorded this boot. Nothing has gone wrong |
| `FALLBACK` | a selection **was** recorded and cannot be honoured. Something *is* wrong |

They are independent, so one profile may carry both words if you want the same answer to
both questions:

```
< PROFILE ID=1 NAME=balanced DEFAULT FALLBACK >
```

Pointing them at different profiles is the more useful arrangement, though — it lets a
normal boot be fast and a *failed* one be cool and slow. See
[the `FALLBACK` profile](#when-a-selection-cannot-be-honoured-the-fallback-profile).

---

## Cool the board right now

```sh
qcctl --cool-max
```

**No root, no password.** It runs the fan flat out until you press Ctrl-C.

That works because the request goes to the daemon, and the daemon will only ever *raise*
cooling — there is no way to ask it for less. The socket is world-writable precisely
because the only thing a caller can cause is more airflow.

The hold lasts as long as the command runs. Ctrl-C, `kill`, even `kill -9` or a crash all
release it identically, so a dead client cannot leave your fan pinned at maximum.

With no daemon running it falls back to doing the work itself, and *that* path needs root.

---

## Fan curves

The stock fan has one trip point at 38 °C, so it only ever sits off or at 40 %, and cycles
audibly about once a minute at idle. A real curve needs the daemon to take the thermal zone
over, which is what `FAN MODE curve` in a profile does.

`/etc/qcctl/fan.config` holds two **different kinds** of strategy:

```
< CURVE NAME=desktop >             # open loop: temperature -> state
  ZONE   cpuss-0-0
  POINT  30C STATE 0
  POINT  38C STATE 4
  POINT  43C STATE 6
  HYSTERESIS 4C
  MIN_STATE 2
>

< TARGET NAME=held40 >             # closed loop: hold a temperature
  ZONE   cpuss-0-0
  TARGET 40C
  DEADBAND 1C
  STEP_INTERVAL 15s
  MIN_STATE 2
>
```

They are not one feature with a switch, and a setting used in the wrong block is a parse
error.

**Zone names: either spelling works.** The kernel's own `type` for this zone is
`cpuss-0-0-thermal`; the short form `cpuss-0-0` is what `qcctl` prints and what these
configs use. Every lookup accepts both, deliberately — a name copied out of the output must
never be rejected as missing. Use the short form in configs and in anything you type. The
full type appears in the systemd unit and in `install.sh`, which compare against the
kernel's string directly.

**`MIN_STATE 2` is worth keeping.** This fan's tachometer misreads at state 1: it reports
somewhere between roughly 3100 and 4200 rpm when the fan is barely turning, which is where
states 5 to 7 actually run — so speed cannot be trusted there. State 2 is the slowest real
speed this fan has, about 1350 rpm. State 0 stays available; the floor only forbids 1.

**Do not lower `STEP_INTERVAL` without re-measuring.** Stepping once per poll made the fan
oscillate between off and maximum, because the board takes far longer than one poll to
respond. The rate limit is what stops that.

An **85 °C ceiling** overrides every block here and is not configurable. It is a
conservative user-facing limit, not a hardware number: this zone's own passive trips are at
130 °C and 135 °C, and the lowest protective trip anywhere on this board is 105 °C, on the
two PMIC zones. Nothing is in danger at 85 °C.

### What to expect

With `held40`, all eight cores loaded and a warm start at 45 °C, the controller steps up
one state per `STEP_INTERVAL` while the zone is above target plus `DEADBAND` and **stops
adjusting** once the reading is inside the band — measured at 8 changes in 100 seconds,
settling the zone at 39–40 °C. If you obstruct the intake it goes to maximum and stays
there, but settles about 5 °C above target, because the fan runs out of authority. The
daemon says so rather than failing quietly.

---

## Airflow health in an enclosure

At a *fixed* fan state the drive signal is fixed, so any change in speed is aerodynamic.
Obstructing the inlet makes the fan spin **faster**, not slower — it moves toward the point
of least airflow, where the impeller does the least work. Measured at an identical state:
**5916–5934 rpm obstructed against 5090–5210 clear.**

That needs a reference from *your* board in *your* enclosure:

```sh
sudo qcctl --fan-baseline capture    # ~2 minutes; it drives the fan
qcctl --fan-baseline show
```

Capture it with the airflow **as it normally is** — this records your enclosure, not a
laboratory. Then add to `fan.config`:

```
< AIRFLOW >
  DELTA    8%
  DEBOUNCE 3
>
```

The comparison is **signed** and both directions mean something:

| | |
|---|---|
| faster than baseline | the intake is restricted |
| slower than baseline | the fan is not delivering — dust, or a worn bearing |

`DELTA` cannot go below 5 %, because ordinary speed variation at a fixed state reached
2.3 % on this hardware and anything tighter would report noise as a fault.

Capture **refuses** if a reading is wildly off the expected figures, unless you pass
`--force`. Recording an existing obstruction as your healthy baseline would make the whole
check permanently blind, which is the one failure worth guarding against.

**The baseline records which kernel and image it was measured under**, and both
`--fan-baseline show` and the daemon warn if that has since changed:

```
WARNING: the environment has changed since capture: kernel 6.8.0-1084-qcom -> 6.8.0-1090-qcom
```

This matters because thermal-zone and hwmon indices are assigned at probe time, so a kernel
change can move what actually gets sampled. A stale baseline does not fail loudly — it stays
wrong in one consistent direction, reporting `restricted` or `ok` steadily and plausibly,
which is the hardest kind of wrong to notice.

It is **a warning and never a refusal.** Judging continues, because a detector that disables
itself on a newer kernel turns "possibly slightly off" into "definitely nothing" — and this
tool also does emergency cooling. A baseline captured before this field existed records no
environment, and that is **not** treated as a change: nothing was observed, so nothing is
claimed.

**It cannot see everything.** At idle the fan may sit at state 0 or 1, where there is no
usable signal, so a warning is not guaranteed promptly after you change an enclosure.

---

## Reading the daemon's status

`/run/qcctl/status`, rewritten each sample and replaced atomically so you never read
a half-written file:

```sh
cat /run/qcctl/status | python3 -m json.tool
```

### `null` never means zero, and never means fine

This is the one thing to get right when consuming it:

| field | `null` means |
|---|---|
| `temp_mc` | no reading — **not** 0 °C |
| `fan_state` | the daemon is not *commanding* the fan. `fan_rpm` still says what it is doing |
| `drifted` | nothing was checked, because no profile was applied — not "nothing drifted" |
| `target_mc`, `target_unreachable` | no closed-loop strategy is running |
| `airflow` | not being judged at all — different from `"unknown"`, which means judged but no signal right now |
| `cpu[].mhz` | that policy reported no frequency — **not** 0 MHz |

`sampler` is `"down"` while the telemetry source is being restarted; every reading goes
`null` rather than freezing at its last value, because a stale number you keep displaying
is worse than an admitted gap.

### CPU frequencies are per **policy**, not per core

`cpu` is an array with one entry per cpufreq policy — **three** on this board, not eight:

```json
"cpu":[{"label":"A78C.0","mhz":1804},{"label":"A78C.1","mhz":2112},{"label":"A55","mhz":1420}]
```

Anything keyed on "cpu0's frequency" silently misses two thirds of the machine. An empty
array means no policies were reported in the last sample; the key is always present, so you
can tell that from a build that does not publish it.

These are **relayed** from the sampler, not read from the kernel by `qcctld` — which is why
you get them here for free, and why you should not run your own sampler to fetch them.

`drifted` exists because the daemon applies a profile once and **never re-asserts it** — if
you change a governor by hand, it assumes you meant to. It reports the divergence and names
the first setting that no longer matches.

---

## Machine-readable output

For anything consuming this, use `--json` and never parse the human layout:

```sh
qcctl -l --json
qcctl -q --json      # includes each policy's available governors
```

The column widths in the human output are a layout, not an interface.

### Two promises, if you are building on this

**`"v":1` is a contract.** Within a version, fields are only ever *added* — so a tool written
today keeps working. If something changes in a way that would break you, the number goes up.

**Read what the daemon publishes; do not start your own `qcstats`.** The daemon owns one
sampler and republishes what it sees. A second one costs real power and changes the very
readings you are displaying: a 20 Hz sampler measured **+384 mW**, against roughly 7.4 W at
idle. Everything a GUI needs is already in `/run/qcctl/status`, `qcctl -q --json` and
`qcctl -l --json`, with `/run/qcctl/control` for asking the daemon to cool the board.

---

## Automatic start

```sh
sudo systemctl enable --now qcctld
```

The unit is ordered after `sysfsutils.service` so a profile supersedes the vendor's
settings — it never edits the vendor's file. Verified across a cold boot: `policy2` comes
up correct.

The daemon stays resident whether or not a fan curve is configured, because it owns the
single telemetry sampler and publishes status. If that sampler dies it hands the fan back
first — it will not drive a strategy blind — then retries, and exits non-zero if it cannot
recover so `systemd` restarts it.

---

## Upgrading, and what a new release cannot change for you

`install.sh --upgrade` **never overwrites `/etc/qcctl/profiles.conf` or `fan.config`.**
They are yours; you may have tuned them, and an installer that silently replaced them would
be the worse failure. But that has a consequence worth knowing:

> **A config block added in a new release does not appear in a config you already have.**
> The feature it drives is inert until you add the block yourself.

This is not hypothetical — it is how `< AIRFLOW >` behaved when it was added, and it is why
the installer now does two things:

**1. It ships reference copies** next to your files, refreshed on every upgrade:

```
/etc/qcctl/fan.config.dist        # what this release ships
/etc/qcctl/profiles.conf.dist
```

Compare at any time with `diff /etc/qcctl/fan.config{,.dist}`. The `.dist` files are
ours, not yours: an upgrade overwrites them and an uninstall removes them.

**2. It tells you which block kinds your config is missing**, and gives you the command:

```
WARN: your /etc/qcctl/fan.config has no AIRFLOW block - features using it are inert
      until you add it
    to add the AIRFLOW block as shipped:
      sudo sh -c "sed -n '/^< AIRFLOW/,/^>/p' /etc/qcctl/fan.config.dist >> /etc/qcctl/fan.config"
    the full reference config is at /etc/qcctl/fan.config.dist
```

It compares **block kinds** (`CURVE`, `TARGET`, `AIRFLOW`), not individual named blocks. A
curve you deleted is a decision, not a gap, so it is not reported — a warning you learn to
ignore is worse than none. Deleting *every* block of a kind, or commenting one out, does read
as absent, which matches what the config file itself promises about commenting out
`< AIRFLOW >`.

Nothing here edits your config. The installer refuses to, on purpose; it only tells you what
changed and hands you the one-line command.

`profiles.conf` gets a `.dist` but **no missing-block warning**: its blocks are your profiles,
and a shipped profile you removed is a choice.

### Your profile choice survives a daemon restart — for this boot

Selecting a profile with `qcctl -m <name>` records it in `/var/lib/qcctl/selected`, tagged
with the current boot id. If the daemon restarts — a crash, a sampler that flapped past its
retry budget, `systemctl restart` — it reads that file and **re-applies your choice** rather
than reverting to `DEFAULT`.

> Before this existed, any daemon restart silently reverted the board to the `DEFAULT`
> profile. You could select `workstation`, walk away, have a sampler hiccup, and come back
> to a board running `balanced` with the fan handed back to the kernel — with nothing
> saying so.

**The scope is deliberately one boot.** A power cycle clears the selection and the board
comes up on `DEFAULT` again. That bound is the safety argument: a selection that
destabilises the machine cannot be re-applied forever by a restart loop, because one power
cycle returns it to a known-good default.

Three outcomes, and the journal always says which:

```
profile source: restored the selection recorded earlier this boot
profile source: DEFAULT - no selection was recorded this boot
profile source: FALLBACK - a selection was recorded but could not be applied.
                The board is deliberately slower and cooler than asked for.
```

The status file carries the same thing as `profile_source`, so a GUI can distinguish *what
you asked for* from *what we fell back to* — `profile` alone cannot:

| `profile_source` | meaning |
|---|---|
| `selected` | your recorded choice, restored |
| `default` | a clean start; nothing was recorded this boot |
| `explicit` | `qcctld -m` named it on the command line |
| `fallback` | your choice could not be applied, so the `FALLBACK` profile was used |
| `fallback-unusable` | your choice failed **and** no usable `FALLBACK` exists, so `DEFAULT` was applied |

### When a selection cannot be honoured: the `FALLBACK` profile

A profile marked `FALLBACK` in `profiles.conf` is applied when a recorded selection exists
but cannot be used — it was renamed or deleted, or its fan curve is no longer in
`fan.config`:

```
< PROFILE ID=3 NAME=lowpower FALLBACK >
```

It is **not** applied on a clean boot. Nothing has gone wrong there, and `DEFAULT` covers
that case — so a fresh board is not silently slowed down.

The shipped `lowpower` is the `FALLBACK` because if something is wrong and nobody is
watching, a slow cool board beats a fast hot one. It pairs with the `fallback` curve, which
reaches full fan speed by 50 °C rather than 75 °C, and it takes the fan off the kernel so
cooling does not wait for the 38 °C trip.

At most one profile may be marked `FALLBACK`, and a second is a config error.

> **`FALLBACK`, not `FAILSAFE`.** `FAILSAFE` already means something different inside a
> `CURVE` block — what happens to the fan when the daemon releases it. `FAILSAFE` is
> rejected as a profile attribute precisely so the two cannot be confused.

**The daemon checks the safety net even when it does not need it.** If the `FALLBACK`
profile is unusable, it says so at every startup:

```
WARNING the FALLBACK profile 'lowpower' is not usable: profile 'lowpower' wants
'fallback', which is in neither the CURVE nor the TARGET blocks - there is no safety
net if a selection goes bad
```

That matters because this path is only ever reached when something has *already* gone
wrong, so a guard nothing exercises is a guard that rots.

> **If you are upgrading, read this.** `fan.config` is yours and an upgrade never
> overwrites it, so a `fallback` curve added by a new release **will not appear in your
> config** — and the installer's missing-block warning compares block *kinds* (`CURVE`,
> `TARGET`, `AIRFLOW`), not names, so a missing `fallback` curve is **not** reported while
> any `CURVE` exists. Check with `diff /etc/qcctl/fan.config{,.dist}`, and add the block
> with:
>
> ```sh
> sudo sh -c "sed -n '/^< CURVE NAME=fallback/,/^>/p' \
>     /etc/qcctl/fan.config.dist >> /etc/qcctl/fan.config"
> ```

### Which build is installed

Every binary reports its own version, so an upgrade can be confirmed rather than assumed:

```console
$ qcctl -V
qcctl 0.1.0
$ qcctld -V
qcctld 0.1.0
$ qcstats -V
qcstats 0.1.0
json-schema 1
```

Each answers with **its own name**, because four binaries ship from one tree and "which one
did I just replace" is the question being asked. `qcctl-check` and `qcctl-surface-dump`
answer too.

`qcstats` prints two lines, because the two move independently: its version changes whenever
its code does, while `json-schema` is the **consumer contract** and is bumped only for a
breaking change. A version bump is therefore not a schema bump, and the second line is the
one to compare when the daemon reports a schema it cannot read.

A binary that answers `unknown` was compiled by hand rather than through `make`, which is
where the version comes from. That is deliberately not a plausible-looking number.

## Troubleshooting

**"needs root"** — the writing commands say so before attempting anything, rather than
failing halfway. `-q`, `-l`, `-V`, `--verify` and `--dry-run` work unprivileged, and
`--cool-max` needs no privilege while the daemon runs.

**The fan is off and the board feels warm.** Below 38 °C the kernel wants it off; that is
correct. If you want air now, `qcctl --cool-max`.

**`airflow` says `unknown`.** Either no baseline is captured, no `< AIRFLOW >` block is in
`fan.config`, or the fan is below state 2 where there is no signal. None of those is a
fault.

**The journal says "zone37 does not drive cooling device 7".** The kernel's own binding
links say the thermal zone this tool disables is not the one driving your fan. Fan commands
still apply — you will see `fan N -> M` as usual — but the zone that *does* drive it can
override them, so a curve may not hold. This has never been seen on a Ventuno Q, where the
fan is bound to exactly one zone; it would mean a different board or a changed device tree.
Nothing is refused on account of it, deliberately: a tool that also does emergency cooling
must not decline to cool.

A related, quieter message — "this kernel exposes no zone-to-cooling-device links" — means
the check could not run at all. That is not a mismatch and nothing is wrong.

**The journal says "stream is schema vN, this build reads v1".** `qcstats` was upgraded to
a telemetry format this `qcctl` does not read. Every such line counts as **no reading**,
and after three the daemon fails safe — deliberately, because the alternative is acting on a
temperature that may have been misread. Upgrade `qcctl` to match — `qcstats -V` prints the
schema it emits, and the message states the one this build reads. A related message,
"the stream carries no schema version", means the `--stats-cmd` is not producing `qcstats`
output at all.

**A profile seems not to have applied.** Check `drifted` in the status. Something changed
the surface after the daemon applied it — possibly you, possibly another tool.

**Suspend.** `qcctl --reapply` re-asserts the active profile and a sleep hook calls
it, but **this is unverified**: the board does not resume from either sleep state, and
every attempt needed a power cycle. Do not rely on it.
