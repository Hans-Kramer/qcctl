<!-- SPDX-License-Identifier: CC-BY-4.0 -->
<!-- SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer -->
# TODO

Open items for `qcctl`. Items marked **[measured]** were checked against the board;
anything else is read from the source and not yet confirmed on hardware.

---

## Defects

**One open, and it is parked.** Four were found by the behavioural suite between 2026-10-19
and 2026-10-21 and are fixed; they are described in `CHANGELOG.md`, with the one remaining
below.

Those tests were written with AI assistance — Claude Opus 5, paired on the behavioural suite,
as the `SPDX-FileContributor` line in each file records — and none of the four had been found
by hand in the week of hardware testing that preceded them. Noted because provenance belongs
with a finding: the suite's value is not the checks that pass but the ones that did not, and
a reader deciding how much to trust these files should know how they were produced.

Two limits are documented rather than fixed, both on the control socket, both pinned by
tests in `tests/t_control.c` so that changing either is a decision:

- It honours **one request per write**, so a client batching two has only its first obeyed.
  Splitting the buffer would mean either discarding a trailing fragment or buffering across
  reads, and both change what a 0666 socket accepts — so it is stated in `src/control.h`.
- **A client that connects and says nothing holds its slot.** There is no idle timeout, so
  `CONTROL_MAXCLI` silent clients fill the table and a further `--cool-max` is accepted,
  closed without a reply, and gets no hold: an unprivileged process can stop another
  unprivileged process raising the fan. **[measured]** What it cannot do is lower cooling or
  release the zone — the kernel's own governor runs underneath this path throughout and the
  socket has no vocabulary for touching it — so what is denied is the extra cooling, not the
  board's thermal protection. Left as it is deliberately: an idle timeout or a per-uid quota
  each adds a way for a hold that is *wanted* to be dropped, which is the worse failure.

Removal matters more than install here, because a durable change you cannot reverse is the
thing this project refuses to make, and that path is clear: the reset is verified against the
snapshot, the manifest is append-only so nothing an install creates goes unrecorded, and
neither script keeps the only copy of anything in `/tmp`.

- **The cooling-map floor is read from only the first `cooling-device` entry of each map.**
  One map with one cooling device is all this board has, so matching entries by phandle to
  the cdev's own node was not needed. A zone binding several devices would need that.
  **Parked:** there is no way to exercise a fix here, so it would ship unverified — which is
  worse than a limitation that is written down.

---

## Gaps

- **The suite proves one board shape.** `make test` runs 707 checks on every push, on
  x86-64 and on `aarch64`, covering the curve and ownership logic, both config parsers and
  their refusals, `apply`'s read-back and its rollback, the board probe, the control socket,
  the cross-project contract, the flag surface through `argv`, the release script, the
  daemon's whole loop through `--stats-cmd`, and the installer as root in a container.

  The flag surface is covered as of 2026-10-19: `tests/sh/t_cli.sh` drives the built binary
  through `argv` — dispatch precedence, the exit codes 0/2/3/4 end-to-end, every refusal,
  `--dry-run` writing nothing against a checksum of the whole tree, and the JSON and human
  output shapes. `--cool-max` is in `t_daemon.sh`, where a real client holds a real socket,
  and the socket's own vocabulary restriction is in `tests/t_control.c`. The apply path's
  failure half — a refused write rolling back the writes already made, and the orphan sweep
  on a profile switch — is in `tests/t_commit.c` as of 2026-10-20.

  What is left: `--control-socket` and `--status-file` are exercised only as the daemon's
  flags, not `qcctl`'s; the fan watchdog's **three-strikes cap** (three corrections need two
  60-second waits and `FANWD_INTERVAL_MS` has no override, unlike `FANWD_FORGET_MS` - the
  gate and the forget window are covered); and a **second fixture**, because one captured tree proves the code
  handles that shape and says nothing about another. Synthetic trees for shapes this board
  cannot show — a zone binding two cooling devices, a zone with no `active` trip, an attribute
  that accepts a write and holds a different value — are the cheaper half of that, because
  they need no board to write.

- **The gate does not run the hardware checks, and cannot.** `tests/board-check.sh` is 37
  checks that need a real thermal zone and a real fan; it was last run green on **2026-10-21**
  (37 ok, 7m43s, against the staged aarch64 build). Nothing reminds anyone to run it — it is a release step written down,
  not an enforced one. The things it covers are the things no fixture can: `step_wise`
  reacting to a real temperature, a written `cur_state` persisting because the zone has no
  `polling_delay`, and what the fan physically does.

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
