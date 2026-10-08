<!-- SPDX-License-Identifier: CC-BY-4.0 -->
<!-- SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer -->

# qcctl

Power, performance and thermal policy for the **Arduino Ventuno Q**
a.k.a. `monza` (Qualcomm QCS8275 / Dragonwing IQ-8275): named profiles you can apply,
reverse, and trust to stay in force.

> **Legal Disclaimer**
> 
> This project is an independent personal project. It is no way affiliated with, 
> sponsored by, or endorsed by Arduino&reg; or Qualcomm&reg;.
>
> "Arduino" and "VENTUNO" are registered trademarks of Arduino. "Qualcomm" and 
> "Dragonwing" are trademarks of Qualcomm Incorporated.
> All trademarks are the property of their respective owners.

> **Pre-release.** This repository is being opened early during active development.
> Interfaces, option names and the output schema may still change.
> Nothing here is promised as stable yet.
---

## What it aims to be

A way to say *"run this board quietly"*, or *"run it flat out"*, and have that mean
something specific, repeatable and reversible — instead of a remembered list of values to
write by hand into places where a typo is indistinguishable from a decision.

The intent is a configuration file describing named profiles, a command that applies one by
name, and a resident service that keeps the chosen profile in force and can take
responsibility for a fan when asked to. Everything it changes, it should be able to put back.

## What it intends to get right

**Reversibility before capability.** Applying a profile should begin by recording what was
there, so the machine can be returned to how it booted on demand.
A change you cannot reverse is not a change this tool should be making.

**Refuse rather than guess.** A profile is validated against the running system before
anything is written, and a request that cannot be honoured should be rejected up front —
not attempted halfway and then unwound.
Where a setting cannot be applied, the goal is to say which one and why, early.

**All or nothing.** If applying a profile fails partway, the earlier writes are undone.
A machine left in a state no profile describes is worse than a machine that refused to change.

**Say it when it fails.** A write the system rejects must be reported, with the attribute
that failed, the reason, and what to do about it. Not many people running this will want to
go reading kernel interfaces to find out why their machine is behaving oddly, and they
should not have to. A silently swallowed error is treated here as a defect.

**Safety is not a mode you remember to enable.** When the service stops, crashes, or loses
the information it needs to make decisions, control should return to the kernel
automatically — not as a tidy-up step that depends on the program still working well enough
to run it. The failure path is intended to be as deliberately designed as the success path.

**Emergency cooling must not need a password.** Asking for *more* cooling can be a
split-second decision, so it should be available to an ordinary user. Asking for *less*
should not be. That asymmetry — a request that cannot reduce safety needs no gate — is meant
to be visible in the design rather than bolted on.

**The unset is never a default.** A profile that says nothing about a setting leaves it
alone, so partial profiles are useful and composable. Switching between profiles should not
depend on the order you switched in.

## Non-goals

- **Monitoring.** qcctl reads the telemetry it needs from `qcstats` rather than growing its
  own. See *The pair*.
- **Exceeding what the board allows.** The aim is to use the range the platform already
  offers, not to push past vendor limits.
- **Portability.** This targets one board. Profiles are validated against the machine in
  front of them, and a setting that does not exist here is a refusal rather than a no-op.
- **Being a graphical application.** A published machine-readable status is intended so that
  a separate interface can be built on top; this repository is not that interface.
- **Hiding what it does.** A dry run that prints every intended write, and no surprises
  outside it, is preferred over convenience.

## The pair

qcctl is one half of a deliberate split:

| | |
|---|---|
| **qcstats** | **observes** — reads and reports, writes nothing |
| **qcctl** | **acts** — applies power, performance and thermal policy |

Keeping them apart is the point. Anything that only needs to *watch* a system should not
have to be trusted with the ability to *change* it, and a tool that does both makes it hard
to separate a real change from a side effect of measuring. A missing reading is a gap for
qcstats to fill, not something for qcctl to work around.

`qcstats` is usable entirely on its own. qcctl is intended to depend on it.

## Status and scope

Early. The reversibility guarantees, the observe/act boundary and the behaviour of the
failure paths are the parts considered settled in intent; much of the surface around them is
not. Issues and discussion about the design are welcome, and more useful at this stage than
patches.

## Licensing

Source will be **MPL-2.0**; documentation will be **CC-BY-4.0**.
