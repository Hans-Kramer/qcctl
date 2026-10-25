<!-- SPDX-License-Identifier: CC-BY-4.0 -->
<!-- SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer -->
# Changelog

All notable changes to `qcctl` will be summarized and documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [Unreleased]

### Fixed

- **`tests/sh/t_install.sh` could run its sandbox half against a live system, and did.**
  That half installs `qcctl` for real, writes `/run`, and puts a stub `systemctl` on
  `PATH`; its only protection was the `QCCTL_TEST_SANDBOX` variable the container run sets,
  plus a uid check that *permits* root - the one user able to do the damage. During this
  work it was run as root on the development board with that variable exported, and it did
  what it does in a throwaway root: installed, then uninstalled. It removed every `qcctl`
  binary, `/etc/qcctl`, both unit files and `/var/lib/qcctl` - boot-state snapshot and fan
  baseline included - and left the stub `systemctl` shadowing the real one. The board was
  never thermally at risk: the uninstaller left the zone `enabled` with the kernel holding
  the fan, which is the behaviour `tests/board-check.sh` asserts.

  It now refuses unless `/run/systemd/system` is absent. The container has no systemd as
  PID 1; every machine this could wreck does. An exported variable is not a safety
  mechanism, and a uid check that permits root is not one either.

- **The suite had never been run as the uid the daemon runs as, and failed when it was.**
  `qcctld` runs as root. Run as root, `tests/t_commit.c` reported seven failures against
  correct code: both of its refusal cases induce the refusal with `chmod` - `0444` for a
  write that must fail, `0200` for a prior value that cannot be read - and a caller holding
  `CAP_DAC_OVERRIDE` ignores mode bits entirely, so the write succeeded, nothing rolled
  back, and the "unreadable" node read fine.

  The premise was wrong rather than the numbers: real sysfs refuses a write to a `0444`
  attribute *whatever the uid*, because kernfs checks the mode itself, while a plain file in
  a fixture obeys DAC and lets root override it. A fixture cannot imitate that with mode
  bits alone. So the two cases now drop `CAP_DAC_OVERRIDE` and `CAP_DAC_READ_SEARCH` before
  they begin (`bind_to_mode_bits()`, 25 lines of `capget`/`capset`, no new dependency), and
  the kernel refuses them for the same reason the board would. Where the drop cannot be
  made the cases fail with that as the reason instead of asserting nothing.

  Proven on the board at both uids, and proven still able to catch the defect it exists for:
  with `apply_rollback()`'s call deleted, "the write BEFORE the failure is undone" reddens
  **as root**, where before the fix that mutation changed nothing because the write it was
  meant to roll back had succeeded.

- **A cooling map that binds several cooling devices yields no floor, instead of the first
  entry's.** Handing a zone back to the kernel needs the state its own cooling map asks for,
  and `cooling_map_lower()` read the first `<phandle, min, max>` entry of each map. With one
  device per map — every map on this board — that is the fan's floor. With two it is a coin
  toss: the entries name their devices by phandle, and on this kernel nothing reachable from
  a cooling device identifies its device-tree node. Measured while closing this: the phandles
  themselves are all there (851 of them under `/sys/firmware/devicetree/base`, and the
  fixture's `0x1d4` resolves to the fan), but `/sys/class/thermal/cooling_device7` holds only
  `cur_state`, `max_state`, `type`, `uevent`, `power/` and `stats/` — no `device` link, no
  `of_node`. So the comparison the old comment proposed has nothing to compare against.

  The floor is now reported unknown for such a zone, which the caller already reads as *leave
  cooling at maximum* — the conservative end, costing a fan that keeps running. One
  unattributable map decides the whole zone, even beside maps that read cleanly: the highest
  of the legible floors is not the zone's floor. A property whose length completes no entry
  is treated the same way rather than having its first 12 bytes read as a floor.

  This was the only defect left open at `0.1.0`, parked because there was no way to exercise
  a fix on hardware that cannot be in that shape. A synthetic device tree is the way: the
  cells are 12 bytes each and a test writes as many as it likes.

- **A check that passed for the wrong reason, found while writing the above.** The first
  version rejected a ragged property with a modulo test and multi-entry maps with a length
  test, and the test for the ragged case passed under a mutation that removed the modulo
  test — because 17 bytes is also *longer than one entry*, so the other guard caught it. The
  modulo test could never decide anything. One rule replaced both: exactly one entry, or no
  floor.

### Added

- **The gate runs `make test` as root, on x86-64 and on `aarch64`.** Root is the uid that
  ships and it was the one uid nothing ever executed; a suite that passes only for
  unprivileged callers is not covering the daemon. The root pass runs second and hands the
  build directory back afterwards, so nothing it rebuilds is left root-owned for the steps
  that follow.

- **`QCCTL_FANWD_INTERVAL_MS`**, beside the `QCCTL_FANWD_FORGET_MS` that was already there
  for the tests. The fan watchdog's three mechanisms are a 60-second gate between
  corrections, a cap at three, and an hour-long window after which the count is forgotten.
  The gate and the window were covered; the cap was not, because reaching it meant two
  60-second waits, and it was written down as untested rather than faked. With the gate
  overridable it has a section of its own: three corrections, the third announcing itself as
  the last, and a fourth stale state deliberately left where it is.

- **33 behavioural checks** — 8 in `t_fanown.c`, 11 in `t_daemon.sh` (section C3), 14 in
  `tests/sh/t_cli.sh`. Totals: **740** off hardware, 37 on it.

  The `t_cli.sh` additions close `--status-file` as *this* binary's flag rather than the
  daemon's: a capture with no published rpm, and one the sampler marks invalid, each have to
  name the path they were given and not the compiled-in default. Their positive control is
  indirect, and had to be — past that gate the capture takes the zone and dwells eight
  seconds per fan state, so it is killed partway and judged by the `mode` it left behind.
  Without that control, both refusals would pass for a binary that refuses every capture.

### Changed

- **A refusal test no longer depends on the refusal to terminate.** The two capture checks
  above run under `timeout`, because the thing being refused is a two-minute action: when the
  refusal was mutated away the test waited for the capture instead of failing. Same rule as
  the `--zones` refusal in `qcstats` and the socket capacity tests — a check that hangs the
  day it regresses is a check that will be deleted rather than read.

---

## [0.1.0] - 2026-10-21

### Added

- **A behavioural suite: 275 checks off hardware, 35 on it.** `make check` compiled this tree;
  nothing executed it. Every behavioural claim in these docs came from a measurement made by
  hand, once, on one board — and going back over the fourteen defects fixed this week, most
  needed no board to find. Three of them were found by a test I ran on hardware that would
  have run just as well in CI.

  The harness is `tests/t.h`: counting macros, no framework, so the zero-dependency promise
  holds. Each `tests/t_*.c` is a `main()` linked against every object but `main.o`, built into
  `build/tests/`. Seven files, run by `make test` alongside the shell suite:

  - `t_curve.c` — `curve_next()` steps up at once and down only after the full hysteresis;
    `MIN_STATE` leaves 0 legal; `target_next()`'s deadband and step interval; the 85 °C
    safety clamp, which is not configurable.
  - `t_fanown.c` — `fanown_release_mode()` returns 0 below the active trip, the cooling map's
    floor at or above it, and `LEAVE_MAX` with no temperature to judge by; `fanown_clear_point()`
    is trip minus hysteresis and is *not* the release threshold. Both of this week's fan
    defects lived in these two functions. Each case now states the device tree it needs,
    because the first version let one case's cooling map leak into the next.
  - `t_config.c` — units are mandatory, an off-OPP frequency is refused, `FAN MODE curve`
    without a `FAN CURVE` is a config error, `config_disruptive()` is per profile and not
    per file, and the exit codes are 0/2/3/4.
  - `t_apply.c` — `apply_verify()` and `state_verify()` divergence counts, including the guard
    that returns −1 when candidates existed and nothing could be read. That guard was
    untested at first: breaking it deliberately changed no result, which is how the missing
    case was found.
  - `t_surface.c` — the board probe against a captured tree: three policies, 48 zones, the
    cooling device bound by name rather than by number.
  - `t_contract.c` — the cross-project contract, fed a line `qcstats` produced from its own
    committed fixture: all seven fields come back through `stats_extract_fan()`,
    `stats_extract_cpu()` and the thermal lookups. Field spelling and order are load-bearing,
    because the parser is `strstr`, and a renamed field reads as "not measured" forever.

  `tests/sh/` is the other 66, with `t_eq`/`t_has`/`t_file_is` helpers and exit 77 for a skip:
  `t_fan_release.sh` drives the release script through the seven cases written by hand this
  week, `t_daemon.sh` runs the whole daemon loop against a fixture — a curve drives
  `cur_state`, the watchdog corrects a stale state and stands down when the zone reads
  `disabled`, a sampler that ignores `SIGTERM` is killed rather than holding the exit open —
  and `t_install.sh` runs the installer as root in a container.

  Three hooks that had been built and never used now carry it: `io_set_sysroot()` with the
  `.manifest` that emulates the modes and owners a copied tree cannot hold, `qcctld
  --stats-cmd` for a canned NDJSON stream in place of `qcstats`, and
  `QCCTL_SAMPLER_RETRY_MS` so a test need not wait out a retry. `install.sh` and
  `uninstall.sh` now honour `QCCTL_SYSROOT` for their `/sys` **probes** only; destinations
  stay absolute, because a test that could redirect them would prove nothing about an install.

  The fixture is `tests/capture-fixture.sh`, run on a board: 1053 files and 21 symlinks, 70 KB,
  of which the `.manifest` is 59 KB. Fidelity was checked by diffing `qcctl-surface-dump` on
  the board against the same binary on the fixture — 73 lines each, differing only in the
  sysroot line, the hwmon prefixes and one fan state that moved between the two runs.

  A container has no systemd as PID 1, so `install.sh` runs steps 1–7 and dies at step 8. That
  makes the container test the mid-run failure case by construction, which is exactly where
  this week's manifest defect lived.

- **Both config parsers' refusals, every curve edge, and the harness's own failure mode.**

  `tests/t_fanconfig.c` is new: about 270 lines of `fancurve.c` - the whole path from a
  hand-written file to a validated strategy - had no caller in any test. `t_curve.c` builds
  its curves in C, so every refusal in between was unreached: descending POINTs, a deadband
  of zero, a step interval that outruns the board, a poll interval of 10ms, a setting in the
  wrong kind of block, an airflow delta below the fan's measured noise floor. These are the
  OPERATOR's mistakes rather than the programmer's, and a refusal naming a line number is the
  difference between a config corrected in a minute and a fan that behaves oddly for a week.
  It also validates against a surface carrying a SECOND cooling device with four states,
  because a ceiling check that only ever sees the ten-state fan proves nothing about the
  others - and this board really has maxima from 3 to 100.

  `config_validate`'s cross-field refusals are covered too. The UCLAMP pair carries its own
  history in the source: *"this check existed for the cpufreq pair above and not for this
  one, which is how 'lowpower' shipped unappliable while --verify called it applicable"*.
  Disabling it, or the trip-ceiling rule that keeps a profile from raising a thermal
  protection, or the cpufreq MIN>MAX pair, each passed the whole suite.

  **Every curve boundary is now probed AT the boundary.** Six independent off-by-one
  mutations survived before: the hysteresis WIDTH (direction was pinned, size was not), the
  deadband becoming an open interval, the step interval off by a millisecond, the safety
  release point, and `MIN_STATE` on the falling path - which, dropped, let the fan sit at a
  state the operator had declared untrustworthy.

  Smaller things, each a way the suite could have misled: `t_contract.c`'s "written in two
  halves" never exercised line reassembly, because both halves arrived before the first
  sample; the overflow flag guarded a branch no input could reach; `duty_pct` was bounded
  only from above, so a negative percentage passed. `t_surface.c` asserted the GPU and the
  hwmon resolution only on an EMPTY tree, where absence is the right answer. `t_apply.c` had
  a case reading a file an earlier case created, a comment-line rule that was never the
  reason a line was skipped, and a `fopen` failure that returned silently and still printed
  "ok". The cooling-device exclusion is now pinned as NARROW - `cur_state` alone, not every
  attribute of a cooling device.

  And the harness itself: `T_TRUE` records a failure and carries on, so a NULL return
  segfaulted mid-file - the run failed, but with a bare "Segmentation fault", no summary, and
  every later case silently skipped. `T_REQUIRE` stops the case instead. The same mutation
  now gives eight named failures and a summary.

  Shell side: a `REBASELINE` is driven end to end by a real `qcctl -m`, which notifies the
  daemon on its way out; `t_cli.sh` uses a two-profile config, so "the disruptive gate is per
  PROFILE" is finally distinguishable from "per FILE" - a defect this project already fixed
  once; and `t_install.sh`'s `systemctl` stub answers per verb instead of saying yes to
  everything, so step 9's unit checks are exercised in both directions rather than masked.

- **The socket's limits, the watchdog's bound, and the drift CI could not see.** Five gaps
  where a mutation survived, closed together because they are the ones with consequences.

  **The socket is now tested as a CLOSED set.** Eight hand-picked refusals proved nothing
  about what else might be accepted; a fixed specification - hold only on `COOLMAX\n` or
  exactly `COOLMAX`, re-baseline only on the same two forms of `REBASELINE`, refuse the rest
  - is now written in the test once and 29 candidates are measured against it. Making
  `REBASELINE` case-insensitive reddens four checks; adding a `FANOFF` verb reddens one.
  **And its capacity is pinned**: `CONTROL_MAXCLI` clients can hold at once, a further client
  gains no slot and is told by end-of-file, the existing holds are undisturbed, and a freed
  slot is reusable. Both forms of that bug - evicting an existing client, and writing at
  slot -1 where `cli[-1]` aliases `listen_fd` - previously HUNG the test rather than failing
  it, so every client read in that file now has a deadline: a server that goes silent
  produces a named failure in half a second instead of a job that times out saying nothing.

  **And one availability limit, measured rather than argued about.** Two of the audits
  disagreed over whether a client that connects and says nothing matters: the socket is 0666
  on the argument that a request can only ever ask for MORE cooling, which is about what a
  client can make the daemon do and says nothing about taking up room. Measured: there is no
  idle timeout, so `CONTROL_MAXCLI` silent clients fill the slot table and a further
  `--cool-max` is accepted, closed without a reply, and gets no hold. An unprivileged process
  can therefore stop another unprivileged process raising the fan. It is stated in the
  README's limitations and pinned by a test rather than fixed: what is denied is the extra cooling, the kernel's
  own governor runs underneath this path throughout and the socket cannot touch it, and every
  fix — an idle timeout, a per-uid quota — adds a way for a hold that is wanted to be dropped.

  **The watchdog's bound.** Its 60-second gate between corrections is asserted (a second
  stale state inside the window is left alone, with nothing logged), and so is the forget
  window - using `QCCTL_FANWD_FORGET_MS`, a hook whose own comment said "for the tests" and
  which no test had ever set. The cap at exactly three corrections is still uncovered: three
  of them need two 60-second waits and `FANWD_INTERVAL_MS` has no override. The test says so
  rather than implying otherwise.

  **Producer drift the cross-repo check could not see.** `stats_extract_cpu` searches forward
  from a label to find `"mhz"`, so emitting them the other way round makes every policy read
  its neighbour's clock - policy 0 gets 940, policy 1 gets 844, policy 2 reads nothing - and
  `qcstats.h` declares adding a field *not* a schema bump, so that can ship. Every assertion
  held: the labels were right, the count was right, and a 100-4000 MHz plausibility band
  accepts another policy's number. Each frequency is now compared against the bytes inside
  its own policy object, read out of the raw line independently of the parser. Both forms of
  reorder redden four checks.

  **And the two hand-synced constants are compared in the test**, not only by a `sed` step in
  the workflow. The default run reads a frozen fixture, so it agreed with itself forever and a
  developer who bumped one side locally still got a green `make test`. The test now reads the
  sibling's `QCSTATS_JSON_SCHEMA` at run time; a one-sided bump fails, and when the sibling
  is not checked out the run prints that the comparison did not happen rather than passing
  quietly.

- **Three tests that would have failed on correct code.** A test that reddens when nothing
  is wrong is worse than one that misses something: it teaches a reader to ignore the suite.

  `t_contract.c`'s schema-bump probe built its forged line with `p + 5`, assuming the version
  was one character. At version 10 it spliced from the second digit, forged `{"v":990,…}` and
  failed - on a schema bump that had been made correctly on both sides. It now skips the
  digits. Confirmed by bumping both constants to 10, regenerating a stream and running
  against it.

  `t_surface.c` pinned `nzone` at a literal 48, which contradicted its own header: every
  other count in that file is read out of the capture *so that a re-capture cannot invalidate
  the test*. It now counts the capture's `thermal_zone*` entries, and asserts `ncdev` the
  same way. Verified in both directions - an extra zone in the fixture no longer fails, and a
  discovery that skips one still does. Its `fixture_tokens()` helper also counted without a
  cap and across every line, while the code stores at most `MAX_OPP`/`MAX_GOVERNORS` from the
  first line only; it now takes the cap as an argument.

  `tests/platform-audit.sh` matched image builds with a literal `href="2026…`. The first 2027
  publication would have made a weekly unattended job report "the extractor is broken" - the
  exact false diagnosis the rest of that script was written to avoid. It matches the
  `YYYYMMDD-NNN` shape now, and says what it means when nothing matches.

- **The apply path's own guarantee is tested now: `tests/t_commit.c`, 57 checks.** `apply.h`
  opens by promising two things — every path is captured before it is written, and a write
  that fails mid-apply rolls back the writes already made, because "leaving the machine in a
  state no config describes is the one outcome worse than failing". Neither was tested. The
  success path ran through the CLI test, but nothing anywhere made a write FAIL, so
  `apply_rollback` could be deleted outright and every check in both repositories still
  passed. `apply_orphans` — the revert-on-switch mechanism, added after a real board was left
  with a fan trip 7 °C late — had no coverage either, nor did `selection_save`/`_load` or
  `state_save_tagged`.

  A write is made to fail the way a kernel fails one: the target exists, was readable when
  the plan was built, and refuses the open. `chmod 0444` produces exactly that. Three writes
  with the second refused now assert that the first is undone, the second never landed, and
  the third was never attempted. Alongside: the capture holds PRIOR values and not new ones;
  a write-only node captures nothing and that is not a rollback failure; a dry commit still
  captures; the orphan sweep reverts what the new profile does not mention and leaves what it
  does; a dry sweep counts and reports without writing, with its stdout captured and
  asserted; no previous profile and a previous profile since deleted from the config are both
  no-ops; a selection is scoped to its boot id and one forged from another boot is refused;
  and a tagged snapshot carries its id.

  One thing is deliberately not asserted: rollback walks its captures newest-first, which
  matters for the min/max dance, but plain files in a fixture cannot observe the order — so
  the test pins that nothing is left behind rather than pretending to see the sequence.

  Six mutations were run to confirm the checks can fail: deleting the rollback call, making
  every capture look unreadable, making the orphan sweep forget the new profile owns a path,
  making it count without writing, making `selection_load` ignore the boot id, and dropping
  the snapshot's tag. Each reddened between one and five checks.

- **The flag surface, and the socket that guards privilege.** Two files, 102 checks, both
  for layers the rest of the suite reached around.

  `tests/sh/t_cli.sh` (55) drives the built binary through `argv`. Of 22 options CI ran one
  — `--help`, for its exit status alone — and three more were exercised only by hand on a
  board. Dispatch precedence was a documented contract with no test, and the exit codes
  0/2/3/4 were asserted below the CLI, through `config_*()`, never through it. The order is
  now pinned where it is load-bearing: `--fan-baseline` over `--query`, `--query` over
  `--list`, `--verify` over `--list`, and `--help` short-circuiting the parse loop so a
  `--reset --help` resets nothing. Both routes to 4 are covered — a disruptive profile
  without `--allow-disruptive`, and `--verify` on a config that is valid but names a path the
  kernel does not expose, which the test produces by moving one attribute out of its copied
  fixture. `--dry-run` is checked against a checksum of the entire tree before and after,
  with a real apply as the positive control, because a dry-run assertion passes trivially
  against a binary that cannot write at all.

  `tests/t_control.c` (47) covers the control socket directly. It is mode 0666 — any user on
  the board can connect — and the only thing making that safe is that the vocabulary has no
  word for *less* cooling. That boundary rested on two string comparisons and nothing
  exercised it. Eight plausible requests are refused and dropped, including `COOLMIN`, a
  lowercase `coolmax`, and `COOLMA` as a prefix of the real word; `COOLMAX` holds for exactly
  as long as the connection lives; two clients hold independently, so one leaving does not
  release the fan under the other; and `control_drop_holds()` closes both, which is how a
  client learns the daemon said OK and then could not take the zone. Breaking the refusal
  branch reddens eight checks.

  `--cool-max` end to end is in `t_daemon.sh` (now 35), where it belongs: the hold **is** the
  connection, so it needs a real second process. A client raises the fan from the curve's 0
  to the cooling device's maximum, the status file reports the hold, the daemon logs the
  force, and when the client is signalled the hold ends and the curve has the fan back. One
  thing that test had to account for: the client's stdout is block-buffered when redirected,
  so its whole account arrived at exit. That is the defect fixed under *Fixed* below: a
  command whose purpose is to report a hold *while it lasts* should not be silent for the
  duration.

  Both files were shown to fail against mutated source rather than edited expectations: an
  unknown option returning 1 instead of 2, `--dry-run` performing its writes, `--csv`
  selecting the human format, and the socket accepting an unknown request.

  **These tests found three defects on their first run**, all of them in what the CLI says
  rather than what it does, and all fixed under *Fixed* below: `--reapply --query` announcing
  a re-apply it does not perform, `-m <profile> --query` ignoring the apply with no message
  at all, and `--cool-max`'s buffering. None had been
  found by hand in the preceding week of hardware testing — the flags were exercised, but
  never in combination, and never with the output read back by something that cared.

  The suite was written with AI assistance (Claude Opus 5, paired), which the
  `SPDX-FileContributor` line in each new test file records. The three findings above are the
  suite's first contribution that is not itself a test — and the distinction worth keeping is
  the suite rather than the assistance: this week's earlier defects were found the same way,
  by scripts written for one question and then discarded. These came from checks that run
  again on every push.

- **`tests/board-check.sh`, the acceptance script the gate cannot run.** 37 checks in six
  sections — `surface`, `reset`, `watchdog`, `capture`, `install`, `release`, plus an
  `envelope` measurement that is not a test — resolving the zone and cooling device **by name**
  and restoring the board through a trap however it exits. Last run green on 2026-10-21: 37
  checks, 7m43s, board left installed with the zone enabled and its trip as it shipped.

  Four of its own flaws were found by running it, and each is the same mistake in a different
  place: a check that could pass without testing anything.

  Its stand-down check — "a disabled zone belongs to someone else" — ran twelve seconds after
  a correction, inside the watchdog's own sixty-second gate, so nothing was due to fire and
  the check passed whether or not the ownership test existed. The daemon is restarted first
  now. **Proven by cross-building a `qcctld` with that check removed and installing it on the
  board: the section fails with two named failures.** (Replacing a running daemon's binary
  needs the unit stopped first — the copy otherwise fails with `ETXTBSY` and leaves the
  mutant in place, which is what happened on the first attempt.)

  Its journal checks read the last N *lines*, which may be minutes old: an assertion that the
  watchdog did NOT act was satisfied by its own correction from an earlier section. There is a
  time-bounded read now, and the checks that ask "did this happen just now" use it.

  The watchdog section needed the board below its clear point — 35.0 °C against an idle
  temperature of about 36 °C — so whether it could run at all depended on the room, and it
  failed on a warm day and called that a defect. The fan's active trip is raised for the
  duration instead, which lifts the clear point with it; only the active trip is touched, and
  it is put back below and by the restore trap. That also surfaced something worth knowing:
  the surface, trip temperatures included, is discovered once at startup, so a trip moved
  underneath a running daemon leaves its watchdog comparing against the old clear point until
  it restarts.

  And both release cases were landing on the same side of the trip, which left "a release
  below the trip decides on 0" — the branch the 2026-10-17 defect was in — untested on
  hardware. Each branch is forced now, and the decision is checked against the temperature
  **the daemon itself measured and logged**, not one the script read seconds earlier: that was
  a race, and it reported a defect that was not there.

  Two of its assertions had to be rewritten before they were honest, and the reason is worth
  recording. They pinned `cur_state` a few seconds after a release and failed: the fan read 4
  where 0 was expected, then 6. Neither was a defect. Writing `mode=enabled` is itself one
  governor evaluation, and this zone has no `polling_delay`, so it is evaluated on tsens
  threshold interrupts and can step up more than once while the board cools. Any ceiling on
  the resulting state asserts `step_wise`'s behaviour, not this project's. What is ours is the
  **decision** — now read from the journal — and the **shape** of what is left behind: with a
  map floor of 4, the governor's own output is 0 or 4 and up, never 1 to 3, so a state in that
  band can only be a manual write nobody handed back. That predicate was confirmed to fail
  against a hand-written state 2 before it was trusted.

- **Two more CI jobs, and a weekly platform audit.** `aarch64` runs `make all`, `make test`
  and the shell suite natively on `ubuntu-24.04-arm`, closing a gap the `Makefile`'s own
  comment names — `char` is unsigned there and signed on x86, compiled on both and until now
  executed on neither. `contract` asserts `STATS_SCHEMA_V` against `qcstats`'
  `QCSTATS_JSON_SCHEMA`, generates a stream from the freshly built sampler, and fails if any
  shell test reports SKIPPED, because a suite that skips silently is a suite that passes
  silently. `platform.yml` is weekly and manually dispatchable, deliberately outside the push
  gate so a network fetch cannot make a push flaky: it asks launchpad for every published
  `linux-qcom` version, fetches the 1.3 MB `linux-buildinfo` package rather than the 180 MB
  `linux-modules`, and asserts the eight kernel config flags every control point here depends
  on. The published image build ids are compared against the recorded `BUILD_ID` as a warning,
  not a failure.

  The 6.5 GB image itself is never fetched. No runner can boot it — there is no QEMU machine
  model for a QCS8275, and no firmware or device tree to give one — so the only part of it this
  project cares about, the kernel, cannot run in CI at any price. What remains inside it is
  Ubuntu 24.04 arm64 userspace, which the `aarch64` job reproduces exactly in seconds; against
  that, the download alone exceeds a runner's free disk once decompressed, and GitHub caps a
  repository's cache at 10 GB. The build id carries the information; the bytes do not.

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

- **The gate's first run failed, in the gate rather than the code.** Three faults, all mine,
  all found by running it:

  The `check` and `aarch64` jobs asserted that no test may report SKIPPED. One legitimately
  does: the installer test stages a NATIVE build, which needs the sibling repository, and
  those jobs check out one repo. The `contract` job has both and runs it for real - its own
  comment said so - so the assertion belonged there and not everywhere. It now names the one
  skip it will accept and fails on any other, which is the signal without the false alarm.

  The `contract` job built `tests` and then ran the shell suite, which drives the real
  binaries through argv - and `tests` builds only the test programs. So two shell tests exited
  "not built" and failed the job with no annotation saying why. It builds `all` too now. The
  same dependency is already in `make test`; running the halves separately is what skipped it.

  And the runner label is pinned to `ubuntu-24.04`. It was `ubuntu-latest`, on the reasoning
  that this is Ubuntu 24.04 like the board - which stopped being true two days before the
  first run, when GitHub began migrating the label to Ubuntu 26. Matching the board's
  userspace is the whole reason the runner choice matters: these scripts are POSIX shell run
  by dash and lean on awk, sed and od. `actions/checkout` moves to v5 in the same change,
  which is what the Node 20 deprecation warnings were about.

- **A fan TARGET with no `MAX_STATE` was validated against nothing.** `fanconfig_validate`
  skipped its ceiling check entirely when `MAX_STATE` was absent, and `target_next()` then
  bounded at a literal 10 — this board's FAN maximum, and wrong for every smaller cooling
  device on it, of which there are several with maxima from 3 to 100. A `TARGET` naming the
  four-state `cdsp:cdsp_sw` validated cleanly and the daemon then commanded states the
  kernel refuses with EINVAL: a write that failed at every tick, visible only in the journal
  and only to someone looking. A CURVE was never affected — every POINT is checked against
  the named device.

  Two changes, because the two answers belong in different places. `fanconfig_validate`
  refuses a target with no `MAX_STATE` on a device with fewer states than the default and
  asks for an explicit ceiling, which is where an operator should find out; the number
  decides how hard a device is driven, so picking one for them would be worse than asking.
  And `fanown_set()` — the single point every fan write in this project passes through —
  clamps to the device's own maximum, as a backstop for anything that does not come from a
  validated config. The literal is now `TARGET_TOP_DEFAULT`, named and documented.

  Found by the behavioural suite's audit on 2026-10-20, and the consequence measured on the
  board: writing 9 to `cdsp:cdsp_sw`, which has four states, is refused and `cur_state` stays
  where it was. So the unclamped version did not drive a device too hard - it issued a write
  the kernel rejected on every tick, which is the quieter failure of the two.

- **`--query` asked together with a change did the change silently not at all.** `-m`,
  `--reset`, `--save-state` and `--reapply` all lost to `--query` by dispatch order, which
  reported the board and exited 0. A caller scripting an apply saw success and no effect.
  The combination is refused now, exit 2, the same way `--cool-max` already refuses the same
  conflict — two answers to "what should happen" is not something to resolve by the order
  the flags happen to be checked in.

- **`--reapply` announced a re-apply before it was dispatched,** so `--reapply --query`
  printed "re-applying the recorded profile" and then only reported. The announcement has
  moved to where the apply happens.

- **`--cool-max` reported nothing while the hold was active, when redirected.** stdout to a
  file or a pipe is block-buffered, so the whole account — including the fact that the fan
  was being held at maximum — arrived only when the process exited. For a command whose
  purpose is to report a hold *while it lasts*, that is backwards; a terminal is
  line-buffered, which is why it had never shown by hand. stdout is line-buffered for the
  duration of the hold now.

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
