<!-- SPDX-License-Identifier: CC-BY-4.0 -->
<!-- SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer -->
# TODO

Open items for `qcctl`. Items marked **[measured]** were checked against the board;
anything else is read from the source and not yet confirmed on hardware.

---

## Defects — removal path

None open. Removal matters more than install here, because a durable change you cannot
reverse is the thing this project refuses to make: the reset is verified against the
snapshot, the manifest is append-only so nothing an install creates goes unrecorded, and
neither script keeps the only copy of anything in `/tmp`.

---

## Defects — fan and thermal

- **The cooling-map floor is read from only the first `cooling-device` entry of each map.**
  One map with one cooling device is all this board has, so matching entries by phandle to
  the cdev's own node was not needed. A zone binding several devices would need that.

---

## Defects — other

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

- **One path nothing verifies:** suspend/resume — the board resumes from neither `freeze`
  nor `deep`, so the hook cannot be exercised here, which the unit itself states. The
  `SIGKILL` fan-release backstop is covered now: the script was run on the board in the
  state a kill leaves behind, and both corrected the fan and handed the zone back. A cold
  boot is covered too — on a power cycle the daemon snapshots `policy2` as the vendor's
  `ondemand` and applies `schedutil` over it, so the `After=sysfsutils.service` ordering
  holds. **[measured]**

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
