<!-- SPDX-License-Identifier: CC-BY-4.0 -->
<!-- SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer -->
# Building `qcctl`

No dependencies beyond a C compiler and `make`. Nothing is fetched, nothing is vendored,
and no kernel module is built or loaded.

---

## On the board

Everything can be built and installed on the Ventuno Q itself. The board ships no compiler,
so that comes first:

```sh
sudo apt update && sudo apt install -y build-essential git
```

`qcstats` is a **hard dependency** and a separate project — the daemon reads its telemetry
stream and contains no monitoring code of its own. Clone both side by side, so that
`qcctl` finds it where it expects:

```sh
git clone https://github.com/Hans-Kramer/qcstats
git clone https://github.com/Hans-Kramer/qcctl
make -C qcstats
cd qcctl && make
```

Try it before installing anything — neither of these needs root, and neither changes a
setting:

```sh
./build/qcctl -q          # the control surface as it is now
./build/qcctl -l          # the profiles in the config
```

Then assemble and install:

```sh
make stage-native
sudo sh /tmp/qcctl-stage/install.sh
```

`make stage-native` builds the same flat directory the installer wants, from a native
build. It checks every staged binary against `/bin/sh`'s own architecture, and when it
finishes that directory is either complete or absent — a half-staged one is a thing someone
would deploy.

The installer validates your config before it enables anything, and tells you what it
touched. To undo it:

```sh
sudo qcctl-uninstall          # --purge also removes /etc/qcctl
```

**Everything the build produces lands in `build/`.** Nothing is written to the source tree,
so `make clean` is one `rm -rf` and a stale binary cannot sit beside a source file
pretending to be current.

| | |
|---|---|
| `build/qcctl` | the command-line tool |
| `build/qcctld` | the daemon |
| `build/qcctl-check` | validates a config and exits; used by the installer |
| `build/surface-dump` | prints the discovered control surface, for debugging |
| `build/src/*.o`, `*.d` | objects and header dependencies |

`polkit/`, `systemd/` and `etc/` are **inputs**, not outputs: the installer stages and ships
them, so they stay in the source tree and are tracked.

---

## Cross-compiling for the board

The board is `aarch64` and ships no toolchain, so binaries are normally built on a
workstation:

```sh
sudo apt install gcc-aarch64-linux-gnu    # Debian/Ubuntu
make cross
```

`make cross` leaves the **board** binaries in `build/` and removes the `aarch64` objects,
which a native build cannot link — without that, a later `make` dies with `file in wrong
format`. Run `make` before testing on the workstation again: the binaries in `build/` are
for the board until you do.

> **`char` is unsigned on `aarch64` and signed on x86.** That is one of several reasons
> both builds are gated: a workstation-only build cannot see a whole class of bug.

---

## Tests

```sh
make check
```

Builds under both `gcc` and `clang` and cross-builds for `aarch64`, all at
`-Wall -Wextra -Werror -O2`. That is the whole gate in this tree.

There is **no behavioural suite yet.** `QCCTL_SYSROOT` relocates every path onto a sysfs
tree captured from hardware, so one can run with no board and no root — that is an open
item, not a finished claim. See [`TODO.md`](../TODO.md).

---

## Installing from a workstation

The installer works from a **staging directory** holding everything flat — it checks for each
file by name before touching the system, and dies if one is missing. Build output lives in
`build/` and the inputs do not, so assembling it pulls from four places plus a separately
built `qcstats`. One target does all of it:

```sh
make -C ../qcstats cross      # qcstats is a separate project; board binary first
make stage                    # assembles /tmp/qcctl-stage, all of it aarch64
```

`make stage` cross-builds, copies, checks that **every** staged binary really is an aarch64
ELF, and then **restores the native tree** before it finishes. When it ends, the staging
directory is either complete and verified or absent: a half-staged directory is one someone
will deploy.

It refuses before building anything if `qcstats` is missing or is a host binary. Override
either path:

```sh
make stage STAGE=/tmp/elsewhere QCSTATS=/path/to/qcstats
```

Then copy it over and install:

```sh
scp /tmp/qcctl-stage/* <board>:/tmp/stage/
ssh <board> 'sudo sh /tmp/stage/install.sh'    # --upgrade over an existing install
ssh <board> 'sudo qcctl-uninstall'             # --purge also removes /etc/qcctl
```

Run `install.sh --dry-run` first if you want to see what it would touch; it reports every
path and changes nothing.

`qcstats` is a **hard dependency** — the daemon reads its stream and contains no
monitoring code of its own. The installer places it only if it is not already present,
and removal leaves it alone if it was.

---

## Other targets

| | |
|---|---|
| `make stage-native` | assemble the installer's staging directory from a native build |
| `make clean` | remove binaries and objects |
| `make clean-objs` | remove objects only, keeping binaries |

---

## Layout

```
src/        14 C files
etc/        profiles.conf, fan.config
systemd/    the unit and the resume hook
polkit/     the action that lets an unprivileged GUI switch profiles
scripts/    install.sh, uninstall.sh, fan-release.sh
docs/       BUILD.md, USAGE.md
```
