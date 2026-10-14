<!-- SPDX-License-Identifier: CC-BY-4.0 -->
<!-- SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer -->
# TODO

Open items for `qcctl`. Items marked **[measured]** were checked against the board;
anything else is read from the source and not yet confirmed on hardware.

---

## Defects — removal path

Removal matters more than install here, because a durable change you cannot reverse is the
thing this project refuses to make.

- **An uninstall destroys the fan baseline.** `/var/lib/qcctl/fan-baseline` sits inside an
  `OWNED` directory, so a plain uninstall removes it while `--purge` is still being offered
  as the destructive option. It is measured data that costs a ~2-minute fan-driving capture
  to replace, and `airflow` reports `null` until it is back — the same "user's work, not
  ours" argument that makes `profiles.conf` `CONFIG`. Observed during the read-back test on
  2026-10-13: a verified-clean uninstall and reinstall left the baseline gone. **[measured]**

- **The `--force` path leaks `OWNED` paths.** The fallback list omits
  `/usr/share/doc/qcctl/USAGE.md`, that directory, and both `/etc/qcctl/*.dist` files.

- **`/tmp` leak, and an upgrade hazard.** The previous manifest is copied to a `mktemp` file
  that is never removed, so each `--upgrade` leaves one behind. More seriously, an upgrade
  that dies between truncating the manifest and re-recording it loses all prior ownership
  information, and a later uninstall will leave formerly-`OWNED` paths behind.

---

## Defects — fan and thermal

- **`fan-release.sh` restores the zone but not the fan state.** After a `SIGKILL` with the fan
  left low on a hot board, re-enabling the zone does not by itself force a re-evaluation: the
  zone has no `polling_delay`, so the kernel waits for a trip crossing that may already have
  passed. Hardening would be a `cur_state` write before `mode`. Strongly indicated — a written
  state was observed holding for 12 s with the zone enabled and no crossing — but the decisive
  above-the-trip test is still outstanding. **[measured, partially]**

- **`fanown_take()` can disable a zone it will never re-enable.** An unreadable `mode` at take
  time leaves the captured value empty, and the restore is then skipped entirely. For `qcctld`
  the unit's `ExecStopPost` covers it; the `--cool-max` and `--fan-baseline capture` CLI paths
  have no such backstop.

---

## Defects — other

- **`--dry-run` claims a systemd-managed daemon is "outside systemd".** Step 1 does not
  actually stop the unit in a dry run, so the `pgrep` that follows finds the running daemon
  and narrates it as hand-launched. Real runs are correct — the stop has happened by then.
  **[measured]**

- **Pidfile path mismatch.** `daemon.c` defaults to `/run/qcctld.pid`; the unit passes
  `--pidfile /run/qcctl/pid`. A hand-started daemon with no flag uses a different pidfile, so
  the two-writer guard does not see the other process.

- **The disruptive gate is file-wide but checked per-apply.** `config_validate` collects
  disruptive actions across every profile, so adding `CPU_ONLINE 0 0` to an *unused* profile
  would make every `-m` demand `--allow-disruptive`.

- **No read-back verification on any write.** `write_path_str` checks the byte count and
  `close()` only — `uninstall.sh` now reads back after `--reset`, but nothing else does. A
  value the kernel silently clamps commits successfully, and the drift baseline then
  records what was *accepted* rather than what was requested — so clamping is
  invisible to the drift watch, which is the one failure the staged min/max writes go to such
  lengths to avoid.

- **`qcctld --foreground` is accepted and ignored.** There is no fork path at all, so the flag
  promises a choice that does not exist. Either implement it or drop it.

---

## Config and text

- **`Makefile` defines `CONF` and `FANCONF` and never uses them.**

- **Unreferenced `fan.config` blocks.** Curves `quiet` and `sustained` and target `held40` are
  named by no profile. The `quiet` *profile* uses `FAN MODE kernel`, so the similarly-named
  `quiet` *curve* is unreachable — a name collision across two files that invites exactly the
  wrong inference.

- **Most `desktop` curve points cannot be reached.** This zone sits ~34–35 °C at idle and peaks
  near 42 °C under full CPU load, so the points at 50, 65 and 75 °C are decorative; the curve
  correctly held state 4 across a 56 s full-load run. Either trim the curve to the real
  envelope or say in `fan.config` that the upper points are headroom. **[measured]**

- **An existing `/var/lib/qcctl/fan-baseline` may carry a `pwr_profile` header** from before
  the project was renamed, advising re-capture with a binary of that name. The string is absent
  from the source and from the built binary, so re-capturing rewrites the header correctly.
  **[measured]**

---

## Gaps

- **No automated behavioural gate.** `make check` is a compilation gate — gcc, clang and the
  aarch64 cross build, `-Wall -Wextra -Werror` at `-O2` — and passes; CI runs it, and stages
  a cross-built package, on every push. `QCCTL_SYSROOT` plus the `.manifest` mechanism
  already carry real modes and owners for a copied tree, so a suite would need no board and
  no root. The hook is built and unused.

- **`surface-dump` is staged and architecture-checked but never installed.** Decide whether it
  is a shipped diagnostic or a development-only tool, and make the Makefile and the installer
  agree.

- **`qcctl-check` has no `--help`** and exits 2 for it, which `install.sh` works around by
  excluding it from the binary smoke test. A broken `qcctl-check` is therefore not detected
  before it is trusted to gate the install.

- **Three paths nothing verifies:** suspend/resume (the board resumes from neither `freeze` nor
  `deep`, so the hook cannot be exercised here — stated in the unit itself), the `SIGKILL`
  fan-release backstop, and a cold-boot install (`install.sh` says so in its closing line).

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
