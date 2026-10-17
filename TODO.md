<!-- SPDX-License-Identifier: CC-BY-4.0 -->
<!-- SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer -->
# TODO

Open items for `qcctl`. Items marked **[measured]** were checked against the board;
anything else is read from the source and not yet confirmed on hardware.

---

## Defects — removal path

Removal matters more than install here, because a durable change you cannot reverse is the
thing this project refuses to make.

- **`/tmp` leak, and an upgrade hazard.** The previous manifest is copied to a `mktemp` file
  that is never removed, so each `--upgrade` leaves one behind — and that copy is the only
  record of the prior install's ownership, since step 2 truncates the real manifest. An
  upgrade that dies after the truncate and before re-recording a path therefore loses it:
  the file stays on disk, no manifest claims it, and a later uninstall reports
  `REMOVED cleanly` while leaving it behind. Narrower than it first looks — the pre-flight
  checks staging completeness and binary architecture *before* the truncate, so the likely
  failures are caught first, and what remains is an I/O failure during steps 3–5. The
  consequence is still permanent and silent, and it falsifies the installer's own header:
  "an install that dies halfway is still fully removable".

---

## Defects — fan and thermal

- **The cooling-map floor is read from only the first `cooling-device` entry of each map.**
  One map with one cooling device is all this board has, so matching entries by phandle to
  the cdev's own node was not needed. A zone binding several devices would need that.

---

## Defects — other

- **`publish_status()` fails silently.** If the status file cannot be opened it simply
  returns, so a consumer sees a missing or stale file with nothing in the journal to say
  why. The default pidfile now creates `/run/qcctl` when systemd has not, so this only
  bites when `--status-file` points somewhere unwritable — but a failure that explains
  itself once would still be better than none.

- **The disruptive gate is file-wide but checked per-apply.** `config_validate` collects
  disruptive actions across every profile, so adding `CPU_ONLINE 0 0` to an *unused* profile
  would make every `-m` demand `--allow-disruptive`.

- **Two write paths still confirm nothing.** `apply` reads its plan back now, but
  `qcctl --reset` replays a snapshot without building a plan, so nothing *inside* qcctl
  checks it — `uninstall.sh` verifies it externally and a bare `qcctl --reset` does not.
  And `--fan-baseline capture` drives the fan to a state and dwells 8 s before measuring;
  a clamped write there would make the capture record the wrong state's rpm with nothing to
  reveal it. That one wants a confirm-before-proceeding read at its own call site rather
  than a change to `write_path_str`, whose eleven callers mostly have nothing to judge.

  The premise of the old entry was too strong and is corrected here: `config_validate`
  rejects off-OPP frequencies (`config.c:592`), out-of-range `UCLAMP`, min above max and
  cooling states above `max_state`, so the silent clamping it described is largely
  unreachable through a valid config. **[measured:** `qcctl -m` with `MAX 1800000kHz` is
  refused before any write.**]** What remains is supersession by another writer, a driver
  that accepts and ignores, and hardware the validator does not model.

---

## Config and text

---

## Gaps

- **No automated behavioural gate.** `make check` is a compilation gate — gcc, clang and the
  aarch64 cross build, `-Wall -Wextra -Werror` at `-O2` — and passes; CI runs it, and stages
  a cross-built package, on every push. `QCCTL_SYSROOT` plus the `.manifest` mechanism
  already carry real modes and owners for a copied tree, so a suite would need no board and
  no root. The hook is built and unused.

- **`qcctl-check` has no `--help`** and exits 2 for it, which `install.sh` works around by
  excluding it from the binary smoke test. A broken `qcctl-check` is therefore not detected
  before it is trusted to gate the install.

- **One path nothing verifies:** suspend/resume — the board resumes from neither `freeze` nor
  `deep`, so the hook cannot be exercised here, which the unit itself states. The `SIGKILL`
  fan-release backstop is now covered: the script is exercised in the state a kill leaves,
  on the board and against seven `QCCTL_SYSROOT` fixture cases. A cold boot is covered too: on a power cycle the daemon snapshots
  `policy2` as the vendor's `ondemand` and applies `schedutil` over it, so the
  `After=sysfsutils.service` ordering holds. **[measured]**

- **No `CapabilityBoundingSet=` on the unit.** `ProtectKernelTunables=` and
  `ProtectSystem=strict` would break a daemon whose job is writing `/sys`, but the capability
  set is a free restriction left on the table.

---

## Settled

- **`RuntimeDirectoryMode=0755` is declared explicitly** in `qcctld.service`, with the reason
  recorded: the 0666 control socket is only reachable while the directory is world-traversable,
  so tightening it silently breaks `--cool-max`. **[measured]**
- **Zone naming.** The short form (`cpuss-0-0`) is the user-facing spelling, used in configs,
  printed output and anything you type; the kernel's full `type` (`cpuss-0-0-thermal`) appears
  in the unit and in `install.sh`, which compare against it directly. Every lookup accepts
  both, `fan-release.sh` included. Documented in `docs/USAGE.md`.
- **The polkit action is `io.github.hans_kramer.qcctl.manage`**, in a namespace this project
  controls rather than one belonging to Arduino. A GUI will hardcode it, so it is not a
  string to change casually.
- **No `CONTRIBUTING.md`.**
