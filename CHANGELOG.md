<!-- SPDX-License-Identifier: CC-BY-4.0 -->
<!-- SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer -->
# Changelog

All notable changes to `qcctl` will be summarized and documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [Unreleased]

### Added

- **`apply` now reads its plan back and says whether the hardware kept it.** A successful
  write proves only that the store handler took the bytes; it may have clamped a value,
  rounded it, or been superseded moments later by another writer — on this board
  `sysfsutils` writes cpufreq governors at boot and the thermal governor writes
  `cur_state`. `apply_verify()` walks the plan after the commit and reports **requested vs
  holding**, on both the CLI and the daemon:

  ```
  verified 15 settings read back as asked
  ```

  Divergence is a report, not a failure. A refused write already fails and rolls back in
  `apply_commit()`; a value the kernel accepted and modified needs the operator told, not
  undone — rolling a profile back because a frequency was rounded to a valid operating
  point would be worse than the silence this replaces. It reports positively because a
  success indistinguishable from the check not running is not evidence that anything was
  checked, and it refuses to report success when there were settings to read and none could
  be read. Cooling-device states are skipped, as `state_restore()` skips them.

  Expect zero divergences on a healthy board: `config_validate` already rejects off-OPP
  frequencies, out-of-range `UCLAMP`, min above max and cooling states above `max_state`,
  so the normalisations this would otherwise catch are prevented before any write. This is
  a backstop for what validation cannot model.

- **`qcctl-check` answers `--help`, and the installer tests it like every other binary.** It
  read `--help` as a config path, failed to parse a file by that name and exited 2 — so
  `install.sh` excluded it from the "does this binary actually run here" check, leaving the
  one binary the install trusts to validate the config as the only one not tested before it
  was relied on. It prints usage on stdout and exits 0 now, and an unknown option goes to
  stderr with exit 2 instead of being read as a filename.

  The exclusion is gone from the installer, and CI asserts that every binary answers `--help`
  with 0 on every push — a regression there used to surface as a failed install on the board,
  after the files had been copied. The usage text carries the exit codes it already had: 0
  valid and applicable here, 2 does not parse, 3 parses but is not usable, 4 the file is fine
  and this kernel does not expose something it names.

  Measured on the board through a real `--upgrade`: the previous binary exits 2 for `--help`,
  the installed one exits 0 and the installer's own check covers it, an unknown option exits
  2, and a real config still exits 0 with the per-profile report unchanged.

- **`surface-dump` ships, as `qcctl-surface-dump`.** It was built, staged and
  architecture-checked but never installed, so the one tool that answers "what does `qcctl`
  think this board looks like" existed only in a build tree — and it is a diagnostic an
  operator wants when a profile will not apply, which is exactly when the build tree is not
  at hand.

  It installs under this project's own prefix, like `qcctl-check` and `qcctl-fan-release`:
  `surface-dump` in `/usr/sbin` is a name this project has no claim to. It is built under
  that name too, so the staged file, the installed command and `-V` all agree — one tool
  with two names is how a staged-but-never-installed binary went unnoticed.

  It answers `--help` on stdout and exits 0, which earns it a place in the installer's
  execution test; an unknown argument now goes to stderr and exits 2 rather than being
  ignored. It is recorded `OWNED`, so an uninstall removes it, and CI asserts the
  uninstaller's fallback list covers it.

- **A GitHub Actions gate** (`.github/workflows/gate.yml`) on every push to `main` and every
  pull request. `check` runs `make check` — gcc, clang and the aarch64 cross build at
  `-Wall -Wextra -Werror` — and asserts that every toolchain is actually present, because
  `make check` skips a compiler it cannot find rather than failing. It also checks that the
  installers' `--help` prints their header and not shell source, and that every tracked file
  carries an SPDX identifier. `stage` checks out the sibling `qcstats`, cross-builds it and
  runs `make stage`, so the packaging path is covered as its own job.

### Changed

- **`qcctld` detaches by default, and `--foreground` now means something.** The flag was
  accepted and ignored — there was no fork path at all, so the only mode was the foreground
  one the unit asks for. `--foreground` is still what the unit passes, because `Type=simple`
  tracks the process it starts.

  The fork happens after everything that reports to stderr and before the pidfile is
  created, so startup errors reach the caller directly and the pid recorded is the resident
  process. The parent waits for the child to apply the profile and resolve its fan strategy,
  then exits with the *child's* status: `qcctld` still answers **4** for "another qcctld is
  running" rather than losing that verdict to a detached process's closed stderr. Measured
  on a Ventuno Q: exit 4 both detached and in the foreground, with the running unit's
  pidfile untouched, and a pidfile that fails for any other reason still does not claim a
  second daemon.

  Once detached there is no terminal, so the log goes to syslog under the `qcctld`
  identifier — the same journal `systemctl` reads. Startup messages still reach the caller's
  stdout, before the prompt comes back. `--oneshot` implies `--foreground`: its exit status
  is its answer, so detaching would return a prompt and no verdict.

- **`--cool-max` and `--fan-baseline capture` now hand the fan back in the state the kernel
  itself would hold:** 0 below the zone's active trip, and the cooling map's lower limit at
  or above it. Previously the fan was left at `max_state` whenever the board was at or above
  the trip *minus its hysteresis*, which on a Ventuno Q meant the whole 35–38 °C band — below
  the trip, and where the board idles. The fan could then stay near maximum indefinitely: the
  thermal governor rewrites a cooling device only on a trip transition, and a downward one
  lowers it by a single state, so a value it would not have chosen sheds one state per
  heat/cool cycle. Leaving its own state needs no transitions at all.

  The limit is read from the device tree (`cooling-maps/*/cooling-device` is
  `<phandle, min, max>`), because sysfs exposes no thermal instance bounds. Where it cannot
  be read the fan is still left at maximum, which is the safe direction.

- **The three unreferenced `fan.config` blocks are wired up, and the `quiet` name collision
  is resolved.** Curves `quiet` and `sustained` and target `held40` were named by no profile,
  so nothing could select them and nothing exercised them — the `TARGET` engine had no
  shipped configuration at all. The collision was the worst of it: the `quiet` *profile* ran
  `FAN MODE kernel`, which left the `quiet` *curve* beside it unreachable while the shared
  name invited exactly the wrong inference.

  - `quiet` runs the curve of the same name. Its raised 45 °C trip is kept and now has a
    job: the curve's `FAILSAFE kernel` hands the zone back, and 45 °C is where the kernel
    then starts the fan.
  - `maxperf` runs `sustained`, whose `FAILSAFE full` is the right answer for a board pinned
    at fmax — losing the daemon there must not hand a hot board back to a single 38 °C trip.
  - `held40` is a new profile, ID 5: a `TARGET` block no profile names cannot be selected at
    all. `balanced`'s clocks, with the fan holding a temperature rather than following one.

  Their points are trimmed to the same measured envelope as `desktop`, and `held40`'s
  deadband goes from 1 °C to 2 °C — no fan state holds a loaded board below 40.9 °C, so a
  1 °C band had the controller stepping to maximum for the last 0.2 °C: measured, state 2 to
  7 in 90 seconds with the temperature flat at 41.2 °C. A 2 °C band is satisfied anywhere
  between 38 and 42 °C, which is where a loaded board sits.

  Verified on the board: all six profiles validate and apply, the daemon takes the zone for
  `sustained`, `quiet` and `held40` in turn, the `quiet` curve holds the fan off through the
  whole unaided climb and engages at 44.6 °C exactly as its first point asks, and `held40`
  steps one state per interval and stops once the zone is inside the deadband.

- **The `desktop` fan curve is trimmed to the temperatures this board actually reaches.** Its
  upper points sat at 50, 65 and 75 °C on a zone that idles at 36 °C and reaches 47 °C under
  full load with the fan switched off, so three of its five points could not be entered — and
  a curve whose top half is unreachable is a curve that silently stops at state 4. They are
  now 43, 45 and 47 °C.

  The envelope is measured, and recorded in `fan.config` itself because it is what anyone
  editing a curve needs: with all eight cores loaded this zone holds **41 °C at fan state 4**
  and **41 °C at state 10** — past state 4 the fan buys nothing measurable on an open board.
  With the fan off it reaches 47.2 °C after two minutes and is still creeping; with the intake
  obstructed it settles about 5 °C above whatever it holds clear.

  That changes what the upper points are *for*. Above ~41 °C this board is not reporting load,
  it is reporting airflow that is not delivering — the condition the `AIRFLOW` block exists to
  detect — so the points are spaced to that: 43 °C is above anything load alone produced here,
  45 °C is the obstructed-intake measurement, and 47 °C is the no-fan ceiling. Verified with
  the trimmed curve installed: the daemon takes the zone, holds state 4 through a 140-second
  full-load run at 41.2 °C, and the 43 °C step is never entered by load alone.

  The envelope is this *cooler's*, not the board's — it was measured on the OE heatsink and
  fan with nothing around them. A different heatsink, a slower or faster fan, an enclosure or
  extra case fans all move it, and none of that is measurable from here, so points above the
  measured band are kept as **headroom rather than trimmed away**: every shipped curve still
  has a path to its top state, and `quiet` carries 55 and 65 °C points for exactly that
  reason. What this release removed is the *gap* — `desktop` had nothing at all between 38
  and 50 °C, which is the whole band this board lives in.

- **An uninstall keeps the captured fan baseline, and `--purge` takes it.**
  `/var/lib/qcctl/fan-baseline` sat inside an `OWNED` directory, so a plain uninstall deleted
  it while `--purge` was still being offered as the destructive option. It is not ours: it
  measures *this* board in *this* enclosure, it costs a two-minute fan-driving capture to
  replace, and `airflow` reports `null` until it is back — the argument that makes
  `profiles.conf` `CONFIG` rather than `OWNED`. It is now stashed across the removal and put
  back, the way the boot-state snapshot and the kept-config record already are, and `--purge`
  removes it alongside the config and says so.

  The step-7 residue check now judges each file left in `/var/lib/qcctl` on its own instead
  of testing one reason at a time: it used to name the first thing that legitimately survived
  and stop looking, so a kept snapshot would have hidden anything else left beside it.

  Measured: capture, uninstall, and the file is byte-identical afterwards, with the
  reinstalled daemon judging airflow against it again; `--purge` removes it; and both dry
  runs say which of the two is about to happen.

- **`uninstall.sh --force` also means "remove the files even though the board is not back to
  stock".** A refusal on that ground exits **4**. No snapshot at all is not evidence of a
  durable change, so an install that died before writing one stays removable.

- **`uninstall.sh` no longer calls the unit's own daemon a stray, and kills by pid.** It
  treated every running `qcctld` as hand-launched, which in a dry run — where the service is
  not really stopped — meant reporting the unit's daemon as one. It now asks systemd for the
  unit's `MainPID` and excludes it, so a real stray is still found and named. The termination
  is `kill` on those pids rather than `pkill -x qcctld`, which would have taken the unit's
  daemon with it.

- **`uninstall.sh`'s closing verdict states "removed" and "reverted" separately,** and claims
  the second only when the read-back proved it. `--help` now lists its exit codes.

- **A cold boot is now verified, and the docs no longer contradict each other on it.** The
  README's status line and `docs/USAGE.md` both claimed `policy2` was verified to come up
  `schedutil` across a power cycle, while the README's own caution block and the installer's
  closing line said a cold boot was unverified. It is now measured: on a power cycle the
  daemon snapshots `policy2` as the vendor's `ondemand` and applies `schedutil` over it, so
  the unit's `After=sysfsutils.service` ordering holds. Suspend/resume remains unverified and
  is still stated as such — this board resumes from neither sleep state.

### Removed

- **`fanown_release_point()`'s release-point output, and the name.** All three callers
  passed `NULL` for it. It returned the lowest `active` trip *minus its hysteresis*, and
  comparing against that rather than against the trip is what left the fan pinned through
  the hysteresis band — so the interface was still offering the threshold whose misuse was
  the defect. Renamed `fanown_active_trip()` for what it actually returns.

- **`CONF` and `FANCONF` from the `Makefile`.** They sat beside `STAGE` and `QCSTATS` in the
  same `?=` form that makes those two overridable, but nothing read them: both stage targets
  copy `etc/profiles.conf` and `etc/fan.config` literally. Wiring them up would have been
  worse than deleting them — `install.sh` never overwrites an existing config, so an override
  would land on a fresh install but on an upgrade replace only the `.dist` reference copy,
  which is what the installer diffs the live config against when it warns about blocks a
  release ships and the config lacks.

### Fixed

- **A disruptive setting in one profile demanded `--allow-disruptive` from every other.**
  `config_validate()` collected them across the whole file, so `CPU_ONLINE 0 0` in a profile
  nobody applies made `qcctl -m balanced` refuse until the flag was given. Measured against
  the previous build: *profile 'balanced' needs --allow-disruptive: line 77: cpu0 online = 0*,
  naming a line in `lowpower`.

  The question is per profile now — `config_disruptive()` answers it for the profile being
  applied, and the file-wide collector is gone from `config_validate()` rather than left for
  someone to reach for again. The predicate has one definition, so the validator and the gate
  cannot disagree about what counts. `--verify` and `qcctl-check` attribute each one to its
  profile, instead of printing a bare line number and leaving the reader to find out whose
  line it was.

  `qcctl-check` also said these *"require an explicit opt-in before the daemon will apply
  them"*, which was false: `qcctld` passes no list and applies the config it is given. That is
  the right behaviour — a daemon that refused to start for want of a flag nobody can pass it
  would be worse than the setting — but it is not what the text claimed. It names `qcctl -m`
  now. Measured: `-m balanced` proceeds, `-m lowpower` exits **4** naming the profile, and the
  same with `--allow-disruptive` proceeds.

- **`publish_status()` failed silently, four ways.** A path too long for its `.new` sibling,
  a `fopen` that failed, buffered writes that only surface at `fclose` — a full filesystem
  arrives there, not earlier — and a failed `rename`. Each simply returned, so a consumer saw
  a stale status file or none at all and the journal said nothing about why.

  It now reports the first failure of a run and the recovery after it, which is the shape the
  drift check already uses: at a two-second poll a per-pass complaint would bury everything
  else, and the fact does not change between passes. Measured on the board: one warning
  across 14 s of polls with the directory missing, *status file … is being written again* when
  it appeared, and a second warning when it was removed again — the silence is bounded by the
  fact rather than by the first occurrence. `logmsg()` moved above its first caller for this,
  which is where a log primitive belongs anyway.

- **Two write paths confirmed nothing.** `apply` reads its plan back; the two commands whose
  whole purpose is a particular state did not.

  `qcctl --reset` replayed the boot-state snapshot and then reported what it had *written*.
  `uninstall.sh` read the values back itself in awk, which left a bare `qcctl --reset`
  proving nothing — and this is the one command whose claim is that the board is as the
  snapshot found it. It reads the snapshot back against the live surface now, names the first
  divergence and **exits 1** when the board is not back. Same report as `apply_verify()`, with
  the opposite verdict: after an apply a divergence is news, here it is a failure, because
  "restored" is the claim being made. The restore and the read-back share one snapshot
  parser, so the set that gets verified cannot drift from the set that was written.

  Measured: a clean reset verifies **312 of 312**. Against a crafted snapshot asking for a
  `scaling_min_freq` above the live `scaling_max_freq`, the write is **accepted and ignored** —
  one of the failures the old note could only name in theory — and the reset says so and exits
  1. The reported *holding* value can be the pre-clamp one, since the read-back is immediate:
  it reported 902400 where the kernel settled on 1497600 a moment later. The divergence is
  right either way.

  `--fan-baseline capture` drove the fan to a state and dwelled 8 s before measuring. A write
  accepted but not held would have put ten seconds of rpm against the **wrong state** into the
  one file every later airflow judgement is measured against, with nothing afterwards able to
  reveal it. It reads the state back before the dwell now and aborts rather than record a
  figure nothing confirms. A clean capture records every state exactly as before.

- **An install that died halfway could leave files no uninstall would ever find.** The
  manifest is the only record of what is on disk, and step 2 emptied it before step 3 started
  re-recording — so between those two points every path the previous install had created was
  unaccounted for. The installer's own header promises the opposite: *appended to as each path
  is created, so an install that dies halfway is still fully removable*. It is append-only
  now, with this run's lines below a `#RUN` marker that `ours()` reads back to, and a
  de-duplication once the install has succeeded and nothing can be lost by rewriting it. The
  copy of the previous manifest in `/tmp` went with it: it was never removed, so every
  `--upgrade` left one behind, and `/tmp` is the wrong place for the only record of what a
  board owns.

  Demonstrated on the board by making step 5b fail — a regular file where its directory goes.
  The previous installer lost three entries, the polkit policy among them while the file was
  still on disk; this one loses nothing, writes no temp file, and leaves nothing on disk
  unaccounted for.

  Losing an entry is **permanent**, which that same test showed by accident: with no record
  that the polkit policy was ours, the next install recorded it `PREEXISTING` — the honest
  answer on the evidence it had — and `qcctl-uninstall` then correctly left it on the board.
  Ownership, once lost, cannot be recovered by any later run, and that is what made this
  worth fixing ahead of louder things.

  **`uninstall.sh` had the same shape, and is fixed with it.** Three `mktemp` files and no
  `trap`, two of which — the boot-state snapshot and the captured fan baseline — are the
  *only* copy of the operator's own data between the removal in step 5 and the restore after
  it. A trap now puts those two back rather than deleting them, and says where the copy is
  if it cannot; only the manifest snapshot, a copy of a file the run is removing anyway, is
  discarded. Both the normal restores and the trap go through one `restore_stash()`, because
  two copies of a restore is how the fan release ended up with three. Measured inside that
  window, with the baseline's only copy in `/tmp`: a simulated failure exits **7** and a
  `SIGTERM` to the process group exits **143**, each putting the baseline back
  byte-identical and leaving no orphan behind. The signal exits are in `--help` now.

- **A fan state nothing would ever correct.** The kernel rewrites a cooling device only on a
  trip transition, and a downward one lowers it by a single state, so a value it did not
  choose is never cleaned up — on an idle board, never at all. Measured: a fan forced to 10
  held through 15 s of idling, 8 `emul_temp` writes and 10 zone `mode` toggles. Releases are
  correct now, but two paths still ended there: a `SIGKILL`, which runs no release at all,
  and a `FAILSAFE full` release, which leaves the fan at maximum *by design* and had nothing
  to walk it back once a profile with no fan strategy took over — measured, state 10 at
  32 °C. Both are covered now, each in the only place that can see it:

  - **`qcctl-fan-release`**, the unit's `ExecStopPost`, sets the fan as well as re-enabling
    the zone. It acts only where it finds the zone *disabled*, which is what says no release
    ran: a clean release leaves it enabled, and `FAILSAFE full`'s maximum is not this
    script's to undo.
  - **`qcctld` watches the fan whenever it is not driving it**, and lowers a state the kernel
    would never hold.

  "Would never hold" is the trip *minus its hysteresis*. Below that the governor's own answer
  is 0; above it the governor is entitled to a throttled state, and a correction would be a
  fight it wins. That is deliberately **not** the threshold a release uses — a release asks
  what the kernel would hold *here*, and inside the band that is the cooling map's floor.
  Same number, opposite question, which is why `fanown_clear_point()` says so at the one
  place someone would otherwise merge the two.

  The watchdog is bounded: one correction a minute at most, three at most before it says
  something else is driving the fan and stops, and the count is forgotten after an hour
  without one — so "three strikes" means three in quick succession rather than three in a
  fortnight of uptime. It acts only where the zone drives exactly one cooling device, because
  with several, which one is the fan is a guess. And only on a zone the kernel actually
  holds: the zone's `mode` is the lock, so while `--fan-baseline capture` or `--cool-max`
  has it, the fan is theirs until they hand it back.

  Measured on the board: a `FAILSAFE full` leftover is cleaned up by the next daemon,
  state 9 → 0 at 34.9 °C; hand-written states are corrected at 33–35 °C and left alone at
  35.3, 36.4, 36.8 and 43.5 °C; the rate limit holds; the third correction announces the cap
  and a fourth write stays at 10; the forget window resets the count. And the backstop, in
  exactly the state a `SIGKILL` leaves behind: *fan was state 6 at 34200mC, below the
  35000mC its trip clears at - set to 0*. Its restraint is measured too: it does not undo
  a failsafe, and it will not guess between two cooling devices. Every decision it makes is
  reachable through `QCCTL_SYSROOT` on a copied tree, with no board and no root.

- **A sampler could outlive the stop meant to end it, and hold the daemon's exit open until
  systemd resorted to `SIGKILL`.** Seen once as a 90-second `systemctl stop`. Three things
  were wrong in how `qcctld` starts and ends its one `qcstats` child, and all three are fixed:

  - **`SIG_IGN` survives `execve`.** The daemon ignores `SIGPIPE` — a dying sampler must not
    kill it — and the sampler inherited that: measured on the running board, `SigIgn 0x1000`.
    Closing its pipe therefore could not end it either, so a sampler that missed its signal
    was beyond every means the daemon had short of `SIGKILL`. The child now resets the
    dispositions it must not keep before `exec`, and the sampler's `SigIgn` reads `0`.
  - **Between `fork` and `exec` a child still carries the parent's handlers.** A `SIGTERM` to
    the process group in that window was caught by the daemon's own handler *inside the
    child* and dropped; after `exec` the signal was already spent. Microseconds wide, which
    is why the original resisted sixteen reproduction attempts. The same reset closes it.
  - **The wait for the child was unbounded.** `waitpid(pid, &status, 0)`, with handlers
    installed through `signal()` and therefore `SA_RESTART`, so a `SIGTERM` arriving there set
    `stop_flag` and the wait simply resumed. Demonstrated against the previous binary with a
    sampler that ignores `SIGTERM`: twenty seconds after the signal it was still in `do_wait`
    (`wait4`) and would have stayed there. It now polls with `WNOHANG` for 500 ms, escalates
    to `SIGKILL` on the group, and logs that it had to — the same case exits in a second.

  The signal is sent *before* the pipe is closed, so a sampler with teardown of its own still
  runs it: verified with `qcstats --gpu-busy`, whose private trace instance is gone and whose
  tracepoint is back to 0 after the stop. A well-behaved sampler is still reaped rather than
  killed, which keeps the exit status the log depends on to tell "qcstats is not installed"
  (127) from "something went wrong".

- **The daemon's own release left the fan where the strategy had it.** `fan_release()` handed
  the zone back under a never-lower rule — `max(the state the strategy drove it to, the state
  found when the zone was taken)` — so switching off a curve or target profile returned an
  *enabled* zone with the fan still high. The kernel lowers a cooling device only on a trip
  transition and only by one state, so on an idle board that value stayed: measured, state
  **8** held at 34 °C after a switch from `held40` to `balanced`, and **7** after an earlier
  one.

  It now makes the same decision `--cool-max` and `--fan-baseline capture` already make —
  `fanown_release_mode()`: 0 below the zone's active trip, the cooling map's lower limit at or
  above it. The daemon was the third copy of this logic and the one that was never converted.
  `struct fanown` also records the zone it took, so a release reads the trip of the zone it
  actually holds rather than of whichever strategy the caller passed it.

  Two paths are deliberately unchanged. `FAILSAFE full` still leaves the fan at maximum,
  because that is what losing the daemon is meant to mean for a board that asked for it. And a
  *blind* release — the sampler has died, so there is no temperature — keeps the never-lower
  handback: no reading is not a licence to lower the fan, and not one to raise it either.

  Measured on the board, all five paths: a switch at idle writes 0; a switch while hot writes
  the map floor; a `systemctl stop` at idle writes 0 and it stays 0 with no daemon left to
  correct anything; `failsafe=full` still leaves maximum; a killed sampler leaves the fan where
  it was and says so. The handback also shows from the other side — once released, the kernel
  starts moving the state on its own again (4 → 5 while hot, 0 → its floor near the trip),
  which it never did with a value the daemon had left behind.

- **The gate asserting the `--force` path checked less than it reported.** It matched each
  installed path as a substring anywhere in `uninstall.sh`, so any path that is a prefix of
  a sibling — `/usr/share/doc/qcctl` inside `…/USAGE.md`, `/usr/sbin/qcctl` inside
  `qcctl-uninstall` — or that is merely named in a comment, passed whether the fallback list
  held it or not. It also skipped a `put` whose destination sits on a continuation line,
  leaving the polkit policy unchecked, and it passed when the extraction matched nothing, so
  a change to the installer's idiom would have turned it green rather than red.

  It now joins continuations before extracting, matches whole lines against the `KNOWN`
  block alone, and refuses to report success unless both sets are populated and a path it
  must always find is present. Removing each of the fourteen entries in turn is caught;
  previously six of thirteen were.

- **`qcctld` requested a runtime-directory mode instead of setting it.** `mkdir`'s mode is
  masked by umask, so a daemon started by hand under `umask 027` or stricter created
  `/run/qcctl` at 0750 or 0700 — without the world-traversable bit the 0666 control socket
  inside depends on, silently disabling unprivileged `--cool-max`. That is the failure the
  comment above the line warns against. The mode is now set explicitly, as `install.sh` and
  `RuntimeDirectoryMode=` both do. Verified under `umask 077`: the directory comes out 0755.

- **A pidfile diagnostic depended on `errno` surviving two file reads.** `pidfile_is_stale()`
  opens and reads two files between the failed `open` and the caller's inspection of
  `errno`, and a library call is permitted to set `errno` even when it succeeds — so the
  "another qcctld is running" message rested on an unguaranteed property, on exactly the
  path it exists for. The value is now restored where nothing after the `open` sets it
  meaningfully; where `unlink` or the retry has set it, that remains the more useful cause.

- **A release reported a fan state it had failed to set.** When the write was refused,
  stderr said the fan was left where it was and stdout then asserted it had been set —
  including "nothing is left for it to walk back", which is precisely the claim the refusal
  makes false. Both the CLI and the daemon now report the refusal in the verdict. Verified
  by making `cur_state` unwritable mid-hold: the release says the fan could not be set, and
  the fan is indeed left at maximum.

- **The comment above that release forbade what the code deliberately does.** It described
  restoring the state found at take time, and warned that lowering a warm board is the one
  move that can do harm — while the code writes 0 below the active trip and the cooling
  map's floor at or above it, the floor being the governor's own answer there. A reader
  following the comment would have reverted the fix and reintroduced the pinned fan. The
  part that is still true — do not lower *below* what the governor wants — is kept.

- **The uninstaller deleted the snapshot its own message told the operator to replay.**
  When `qcctl` is gone but the boot-state snapshot remains, step 3 reports that the control
  surface was changed with nothing left to revert it, and prints the loop to replay the
  snapshot by hand. Step 5 then removed `/var/lib/qcctl`, which holds it — so the operator
  acted on the instruction after the run and found the file gone.

  The snapshot is now stashed across the removal and put back, the way the kept-config
  record already is, and the residue check knows it is legitimately kept rather than
  reporting `INCOMPLETE`. `--purge` still removes it, and says so instead of printing a
  replay instruction it is about to invalidate.

- **`--force` left four files behind for good.** `uninstall.sh` falls back to a built-in
  path list when no manifest survives, which is exactly what `--force` is for, and that list
  omitted four paths the installer records `OWNED`: `/usr/share/doc/qcctl/USAGE.md`, the
  directory holding it, and both `/etc/qcctl/*.dist` reference copies. Because `--force` is
  the case where no manifest exists to correct the omission from, those four had no route off
  the board at all. The `.dist` copies belong in that list rather than with the config —
  `profiles.conf` and `fan.config` are the user's and survive without `--purge`, but the
  reference copies beside them are ours and every upgrade refreshes them.

  CI now asserts the list against `install.sh`, since two hand-maintained lists that must
  agree is how this happened: an `OWNED` destination the uninstaller never names fails the
  gate.

- **The guard against two daemons sharing the fan never fired.** `qcctld` defaulted its
  pidfile to `/run/qcctld.pid` while the unit passed `--pidfile /run/qcctl/pid`, so a
  hand-started daemon and the unit's each created a pidfile the other could not see, and
  both ran. The default is now `/run/qcctl/pid` — inside the unit's `RuntimeDirectory`,
  alongside the control socket and the status file — and the unit no longer restates it, so
  one declaration can no longer drift from another. A daemon started outside systemd creates
  that directory itself, which also gets it a working `active` and `status` file instead of
  two write failures.

  A pidfile that cannot be created for some other reason — a missing directory, a permission
  failure — no longer reports that another daemon is running.

- **`uninstall.sh` never checked that `qcctl --reset` worked.** It called it through a helper
  that discards every error, so a failed reset still ended in `REMOVED cleanly` with
  governors, frequency caps, `uclamp` values and offline cores left modified — and with the
  binary and the boot-state snapshot both deleted, which is the one outcome an operator
  cannot recover from. The reset's exit status is now read, and every restored value is read
  back and compared against the snapshot it came from. A failure stops the uninstall while
  the binary and the snapshot are still present and the error is still fixable.

  The read-back matters on its own: `--reset` reports what it *wrote*, and `write_path_str()`
  checks the byte count and `close()` only, so a value the kernel accepts and then clamps
  commits successfully.

- **`fanown_take()` could disable a thermal zone it would never re-enable.** An unreadable
  `mode` was swallowed into an empty string, which defeated two things: the guard that
  refuses a zone another process already owns, and the restore, which is skipped when nothing
  was captured. It wrote `disabled`, took ownership and handed nothing back — leaving nothing
  managing the fan, since the zone carries no `polling_delay`. It now refuses to take a zone
  unless it has read a mode it understands.

- **`--cool-max` told the user "nothing is left pinned" while leaving the fan pinned.** Both
  release messages are rewritten to state which state the fan was left in and why.
