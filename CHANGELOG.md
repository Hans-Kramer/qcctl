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

- **A GitHub Actions gate** (`.github/workflows/gate.yml`) on every push to `main` and every
  pull request. `check` runs `make check` — gcc, clang and the aarch64 cross build at
  `-Wall -Wextra -Werror` — and asserts that every toolchain is actually present, because
  `make check` skips a compiler it cannot find rather than failing. It also checks that the
  installers' `--help` prints their header and not shell source, and that every tracked file
  carries an SPDX identifier. `stage` checks out the sibling `qcstats`, cross-builds it and
  runs `make stage`, so the packaging path is covered as its own job.

### Changed

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

### Changed

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
