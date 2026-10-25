<!-- SPDX-License-Identifier: CC-BY-4.0 -->
<!-- SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer -->
# TODO

Open work on `qcctl`, and nothing else. **No defects are open.** Deliberate limits that are
not going to change are in the README's *Limitations*; what was fixed, and why, is in
`CHANGELOG.md`.

Items marked **[measured]** were checked against the board; anything else is read from the
source and not yet confirmed on hardware.

---

## Gaps

- **A second captured fixture.** `make test` runs 740 checks against one real tree, and the
  tests build the shapes a board cannot be in themselves. Neither stands in for another
  board: the value of a capture is that nobody chose what was in it. Blocked on hardware.

- **The gate does not run the hardware checks, and cannot.** `tests/board-check.sh` is 37
  checks that need a real thermal zone and a real fan. Nothing reminds anyone to run it —
  it is a release step written down, not an enforced one. Last green **2026-10-21** (37 ok,
  7m43s, staged `aarch64` build), which was before the fan watchdog and the cooling-map
  reader changed, so that run no longer describes this tree.

- **No pre-sleep hook, and no way to verify one.** Nothing releases the fan before the
  system sleeps, so a `curve` or `target` profile leaves the zone `disabled` and the fan
  where the strategy left it. The two candidate fixes, and why neither shipped, are in the
  README; the blocker is that this board resumes from nothing, so the resume half of either
  cannot be shown working. Revisit when it resumes, or when `rtcwake` is shown to wake it.
  **[measured]**

- **No `CapabilityBoundingSet=` on `qcctld.service`.** `ProtectKernelTunables=` and
  `ProtectSystem=strict` would break a daemon whose job is writing `/sys`, but the
  capability set is a free restriction left on the table. Needs an install cycle on the
  board to prove the daemon still writes, binds its socket and creates its runtime
  directory.
