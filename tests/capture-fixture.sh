#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer
# SPDX-FileContributor: Claude Opus 5 (Anthropic), paired on the behavioural suite
#
# Capture the control surface this project discovers, as a tree for QCCTL_SYSROOT. Run it ON
# a board. The result is small - the whole surface is a few kilobytes of text - and committing
# it is what lets the suite run with no board and no root.
#
#   sh tests/capture-fixture.sh tests/fixtures/monza-20260710-235-6.8.0-1084
#
# Two things make this more than a copy:
#
#   The .manifest. A copied tree cannot carry root-owned 0644: a non-root copy is owned by
#   whoever ran this, and git records nothing beyond the execute bit. io.c reads a manifest of
#   `<octal mode> <owner> <board-absolute path>` lines from the fixture root and answers
#   path_mode()/path_owner() from it, so knob_access() still reports what ROOT may do with a
#   knob rather than what the file happens to allow here.
#
#   The file list below IS this project's read surface. A path missing from it is a path no
#   fixture can satisfy, and a test against it would fail for the wrong reason.
#
# Unreadable attributes are left out rather than written empty: absent and empty mean the
# same thing to a reader, and absent is the honest record.
set -u

OUT=${1:-}
[ -n "$OUT" ] || { echo "usage: $0 <output-dir>" >&2; exit 2; }
[ -d /sys/class/thermal ] || { echo "$0: no /sys/class/thermal here" >&2; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT" || exit 1
MANIFEST=$OUT/.manifest
: > "$MANIFEST"
n=0
links=0
skipped=0

# grab <absolute-path> - one regular file, plus its real mode and owner in the manifest.
# Symlinks are skipped here; sysfs is full of plumbing links whose targets are nowhere near
# the captured set, and copying them leaves a tree full of dangling links.
grab() {
    [ -e "$1" ] || return 0
    [ -L "$1" ] && return 0
    [ -f "$1" ] || return 0
    mkdir -p "$OUT$(dirname "$1")"
    if cat "$1" > "$OUT$1" 2>/dev/null; then
        # %a is the octal mode, %U the owner's name - what io.c's manifest expects.
        stat -c '%a %U %n' "$1" >> "$MANIFEST" 2>/dev/null
        n=$((n + 1))
    else
        rm -f "$OUT$1"
        # A write-only or erroring attribute still has a mode worth recording: it is how
        # knob_access() reports `wo` rather than `absent`.
        stat -c '%a %U %n' "$1" >> "$MANIFEST" 2>/dev/null
        skipped=$((skipped + 1))
    fi
}

grab_glob() {
    for p in "$@"; do grab "$p"; done
}

# grab_link <absolute-path> - a RELATIVE symlink, which still resolves inside the tree. A
# zone's cdevN links are how its cooling devices are discovered.
grab_link() {
    [ -L "$1" ] || return 0
    case "$(readlink "$1")" in
        /*) return 0 ;;
    esac
    mkdir -p "$OUT$(dirname "$1")"
    ln -sfn "$(readlink "$1")" "$OUT$1"
    links=$((links + 1))
}

echo "capturing into $OUT"

# ---- thermal: the zones, their trips, and the devices they drive ----
for z in /sys/class/thermal/thermal_zone*; do
    [ -d "$z" ] || continue
    grab_glob "$z"/type "$z"/mode "$z"/policy "$z"/temp "$z"/available_policies \
              "$z"/sustainable_power
    grab_glob "$z"/trip_point_*_type "$z"/trip_point_*_temp "$z"/trip_point_*_hyst
    grab_glob "$z"/cdev*_trip_point "$z"/cdev*_weight
    for l in "$z"/cdev*; do grab_link "$l"; done
done
for c in /sys/class/thermal/cooling_device*; do
    [ -d "$c" ] || continue
    grab_glob "$c"/type "$c"/cur_state "$c"/max_state
done

# ---- cpufreq, per policy ----
for p in /sys/devices/system/cpu/cpufreq/policy*; do
    [ -d "$p" ] || continue
    for f in "$p"/*; do grab "$f"; done
done
# ---- per-cpu: online, capacity, cpuidle states ----
for c in /sys/devices/system/cpu/cpu[0-9]*; do
    [ -d "$c" ] || continue
    grab_glob "$c"/online "$c"/cpu_capacity
    for s in "$c"/cpuidle/state*; do
        [ -d "$s" ] || continue
        grab_glob "$s"/name "$s"/desc "$s"/disable "$s"/latency
    done
done

# ---- the GPU and anything else on devfreq ----
for d in /sys/class/devfreq/*; do
    [ -d "$d" ] || continue
    for f in "$d"/*; do grab "$f"; done
done

# ---- hwmon, resolved by name at runtime ----
for h in /sys/class/hwmon/hwmon*; do
    [ -d "$h" ] || continue
    grab "$h"/name
    grab_glob "$h"/pwm* "$h"/fan* "$h"/in[0-9]* "$h"/curr[0-9]* "$h"/power[0-9]* \
              "$h"/temp[0-9]*
done

# ---- the system-wide uclamp pair, which is a sysctl rather than a sysfs knob ----
grab_glob /proc/sys/kernel/sched_util_clamp_min /proc/sys/kernel/sched_util_clamp_max

# ---- the device tree: cooling-map limits, which sysfs does not expose at all ----
grab /proc/device-tree/model
for z in /proc/device-tree/thermal-zones/*; do
    [ -d "$z" ] || continue
    for m in "$z"/cooling-maps/*; do
        [ -d "$m" ] || continue
        grab "$m"/cooling-device
    done
done

# ---- identity ----
grab /etc/buildinfo

# ---- provenance, so a stale fixture is recognisable as one ----
{
    echo "# qcctl fixture capture"
    echo "captured   $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "host       $(uname -n)"
    echo "kernel     $(uname -r)"
    echo "arch       $(uname -m)"
    echo "build_id   $(sed -n 's/^BUILD_ID=//p' /etc/buildinfo 2>/dev/null || echo unknown)"
    echo "model      $(tr -d '\0' < /proc/device-tree/model 2>/dev/null || echo unknown)"
    echo "files      $n"
    echo "links      $links"
    echo "manifest   $(wc -l < "$MANIFEST") entries"
} > "$OUT/.capture"

echo "  $n files, $links cdev links, $skipped unreadable (mode still recorded)"
echo "  $(wc -l < "$MANIFEST") manifest entries"
echo "  provenance in $OUT/.capture"
