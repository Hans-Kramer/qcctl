<!-- SPDX-License-Identifier: CC-BY-4.0 -->
<!-- SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer -->
<!-- SPDX-FileContributor: Claude Opus 5 (Anthropic), paired on the behavioural suite -->
# Captured fixtures

`monza-*` is a photograph of one board's **control surface**: every sysfs attribute this
project discovers, the device-tree cells sysfs does not expose, and a `.manifest` of the real
modes and owners. `QCCTL_SYSROOT` points at it and every read is relocated, so discovery, the
apply planner, the release rules and the fan strategies all run on a machine that is not a
Ventuno Q.

Captured by [`../capture-fixture.sh`](../capture-fixture.sh), which is also the authoritative
list of what this project reads.

| fixture | board image | kernel | files | manifest | content |
|---|---|---|---|---|---|
| `monza-20260710-235-6.8.0-1084` | `BUILD_ID=20260710-235` | `6.8.0-1084-qcom` | 1051 + 21 links | 1054 | 71 KB |

## Why this is tracked and not ignored

The test for `.gitignore` is whether a clone can regenerate it. A build artifact can; this
cannot — it takes **this** board. Ignored, `make test` on a fresh clone would have nothing to
read, the fixture-driven checks (most of the suite) could not run at all, and CI's "no test
may report SKIPPED" step would be the only thing left standing. The capture is a test vector
with its provenance in its name, not output.

The cost is paid in **file count**, not in size: 1074 of this repo's 1141 tracked files are
fixture, while `.git` is under a megabyte and `git status` takes ten milliseconds. A single
`tar.gz` would be 28 KB against 69 KB of loose text — no saving worth having — and it would
cost the thing that makes a loose tree worth keeping: when the board is re-captured, the diff
*shows what changed underneath this project*. A kernel that renumbers a zone or moves an
attribute's mode is exactly what a reader needs to see, and an archive hides it.

Nothing here identifies the machine it came from: `etc/` holds only the published `BUILD_ID`,
and there is no hostname, machine-id, cmdline or MAC in the tree. `capture-fixture.sh` takes
only the paths this project reads, which is why that script is the authoritative list.

## The manifest, and why a plain copy is not enough

A copied tree cannot carry root-owned `0644`: the copy belongs to whoever ran the capture, and
git records nothing beyond the execute bit. `knob_access()` answers from the **owner** mode
bits — what root may do with a knob — so without the real modes a fixture would report every
control point differently from the board.

`.manifest` is one line per attribute, `<octal mode> <owner> <board-absolute path>`, read by
`io.c` and consulted by `path_mode()` and `path_owner()` ahead of any `stat()`.

## How faithful it is

`qcctl-surface-dump` was run on the board and against this fixture, and the two outputs were
compared: **73 lines each, and the only differences were the `sysroot:` line, the hwmon paths
(which are printed with the sysroot prefix by design) and one fan state that had moved between
the capture and the comparison.** Governors, operating points, every trip, every mode and
owner, the cooling-device bindings and the model string were identical.

That comparison is worth repeating whenever the fixture is re-captured; it is the cheapest
check that a capture is complete.

## What a fixture proves, and what it does not

It proves this code turns **that** surface into **those** decisions. It cannot prove the
surface is still what a kernel presents, and it says nothing about what the hardware then
does: `step_wise`'s reactions, a written `cur_state` persisting because the zone has no
`polling_delay`, the fan's actual rpm per state. Those live in `tests/board-check.sh`, which
needs a real zone and a real fan, is run by hand on a board, and is not part of the gate.

## Licensing

The captured values and device-tree cells are machine output — numbers a kernel printed — and
are not authored content. This file is CC-BY-4.0 like the project's other documentation, and
the gate's SPDX assertion skips the captured trees and points here instead.
