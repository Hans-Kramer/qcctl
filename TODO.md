<!-- SPDX-License-Identifier: CC-BY-4.0 -->
<!-- SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer -->
# TODO

Open work on `qcctl`, and one standing fact at the end that a reader of this file would
otherwise ask about. **No defects are open.** Deliberate limits that are not going to change
are in the README's *Limitations*; what was fixed, and why, is in `CHANGELOG.md`.

Items marked **[measured]** were checked against the board; anything else is read from the
source and not yet confirmed on hardware.

---

## Gaps

- **A second captured fixture.** `make test` runs 819 checks against one real tree, and the
  tests build the shapes a board cannot be in themselves. Neither stands in for another
  board: the value of a capture is that nobody chose what was in it. Blocked on hardware.

- **No pre-sleep hook, and no way to verify one.** Nothing releases the fan before the
  system sleeps, so a `curve` or `target` profile leaves the zone `disabled` and the fan
  where the strategy left it. Two candidate fixes, neither shipped: a oneshot unit ordered
  `Before=sleep.target` to stop the daemon so the existing `ExecStopPost` release runs —
  twelve lines, no new code — or teaching the daemon logind's `PrepareForSleep`, which keeps
  it alive across sleep with its drift baseline and airflow history intact at the cost of a
  `libsystemd` link the cross build would also need. The blocker is the same for both: this
  board resumes from nothing, so the resume half cannot be shown working, and this project
  does not ship behaviour it cannot demonstrate. Revisit when it resumes, or when `rtcwake`
  is shown to wake it. **[measured]**

- **The fan watchdog does not consider `0` above the active trip to be impossible.** It
  watches for states 1–3, the band the governor cannot produce given a map floor of 4. But
  `0` while the temperature is above the active trip is equally impossible for the governor
  to *hold*, and on a zone with no `polling_delay` it persists: the board was found at
  44.9 °C with the fan off and no interrupt arriving to prompt a re-evaluation, and neither a
  manual `cur_state` write nor a `mode` toggle moved it. Widening the watchdog's condition to
  include "off while above the active trip, for longer than one evaluation" would catch it.
  The hard part is not the test but the response: correcting it means taking the zone, which
  is what the profile in force has declined to do. **[measured]**

## Known, and not a gap

- **The gate does not run the hardware checks, and cannot.** `tests/board-check.sh` is 39
  checks that need a thermal governor reacting to a real temperature, a fan that turns, and
  a driver that clamps — none of which a runner has. It is a release step run by hand, and
  nothing reminds anyone to run it. Last green **2026-10-25** (39 ok, 9m21s, native build
  on the board), which is this tree. **[measured]**
