<!-- SPDX-License-Identifier: CC-BY-4.0 -->
<!-- SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer -->
# Changelog

All notable changes to `qcctl` will be summarized and documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [Unreleased]

### Added

- **A GitHub Actions gate** (`.github/workflows/gate.yml`), on every push to `main`, every
  pull request, and on demand. Two jobs: `check` is self-contained; `stage` exercises the
  packaging path, which has a hard dependency on a cross-built `qcstats` from the sibling
  repository.

  What it asserts that `make check` alone does not:

  - **Every toolchain is present.** `make check` *skips* a compiler it cannot find, with a
    notice rather than a failure. That is right on a developer's machine and wrong on a
    runner, where a missing clang would report a green gate having checked half of it.
  - **The packaging path works across repositories.** `stage` checks out `Hans-Kramer/qcstats`
    alongside, cross-builds it, and runs `make stage`, which refuses to proceed until every
    staged binary is aarch64. Its own job, so a failure there reads as a packaging failure
    rather than a compilation one.
  - **`install.sh --help` and `uninstall.sh --help` print the header, not shell source.**
    Both print a line range of their own comment block, and a range that drifts past the
    header leaks `set -u` and the SPDX line to the user.
  - **Every tracked file carries an SPDX identifier**, `LICENSE` and `LICENSES/*` excepted.
    What a REUSE lint would check, done inline to keep the zero-dependency promise.

### Fixed

- **`uninstall.sh` now verifies that `qcctl --reset` worked, and removes nothing until it
  has.** The reset was called through a helper that discards every error
  (`"$@" >/dev/null 2>&1 || true`), so a failed reset still ended in `REMOVED cleanly`
  with governors, frequency caps, `uclamp` values and offline cores left modified — and
  with the binary and the boot-state snapshot both deleted, which is the one outcome an
  operator cannot recover from. The fan path in step 2 was already verified; the clock
  path was not.

  Two checks now stand between the reset and step 5:

  - **`--reset`'s exit status is read.** A non-zero exit stops the uninstall while
    `/usr/sbin/qcctl` and `/var/lib/qcctl/boot-state` are both still present, which is
    the only point at which the error is still fixable.
  - **Every restored value is read back** and compared against the snapshot it came from
    — 312 of them on a Ventuno Q. `--reset` reports what it *wrote*, and
    `write_path_str()` checks the byte count and `close()` only, so a value the kernel
    accepts and then clamps — a `scaling_min_freq` above the current `scaling_max_freq`,
    say — commits "successfully" and is reported good. Only a read-back sees that.
    `cooling_device*/cur_state` lines are skipped using the same two substrings
    `is_governor_output()` tests, so the check cannot drift into verifying a path
    `--reset` deliberately never writes.

  A read-back that matched nothing at all is treated as a failure, not a pass.

  Verified on hardware. A full uninstall reports `restored 312 values from
  /var/lib/qcctl/boot-state` followed by `verified: all 312 restored value(s) match the
  snapshot`, and `policy2` is independently confirmed back to the vendor's `ondemand`.
  Run against a live board with a profile in force, the same check reports exactly one
  mismatch — `policy2/scaling_governor now="schedutil" boot="ondemand"` — so it detects
  real drift rather than passing vacuously. 312 values read in 53 ms.

### Changed

- **`--force` also means "remove the files even though the board is not back to stock".**
  A refusal on that ground exits **4**. Two cases deliberately do not refuse: no snapshot
  at all is no evidence of a durable change, so an install that died before writing one
  stays removable; and a snapshot with no binary left to replay it is reported along with
  the shell loop that replays it by hand.

- **The closing verdict no longer asserts stock state unconditionally.** "Removed" and
  "reverted" are two claims, and the second is made only when the read-back made it.

- **`uninstall.sh --help` lists its exit codes** (0, 1, 2, 3, 4), which were undocumented.
