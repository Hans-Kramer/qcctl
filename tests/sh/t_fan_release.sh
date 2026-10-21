#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer
# SPDX-FileContributor: Claude Opus 5 (Anthropic), paired on the behavioural suite
#
# scripts/fan-release.sh, which is the unit's ExecStopPost and therefore the only thing that
# runs after a SIGKILL. What it must get right is as much about restraint as about action:
# it has to correct a fan nothing else will, without undoing a release that was deliberate.
#
# Every shape here is built in a temp tree, because the board cannot be in most of them: it
# has one cooling device per zone and exactly one active trip, and the whole point of the
# cases below is the zones it does not have.
set -u

. "$(dirname "$0")/t.sh"

SCRIPT=scripts/fan-release.sh
FX=$(mktemp -d /tmp/t_fan_release.XXXXXX) || exit 2
trap 'rm -rf "$FX"' EXIT INT TERM

Z=$FX/sys/class/thermal/thermal_zone0
C0=$FX/sys/class/thermal/cooling_device0
C1=$FX/sys/class/thermal/cooling_device1

# shape <mode> <temp-mC> <state> <trip-type> <cooling-devices>
shape() {
    rm -rf "$FX/sys"
    mkdir -p "$Z" "$C0" "$C1"
    echo cpuss-0-0-thermal > "$Z/type"
    echo "$1" > "$Z/mode"
    echo "$2" > "$Z/temp"
    echo "$4" > "$Z/trip_point_0_type"
    echo 38000 > "$Z/trip_point_0_temp"
    echo 3000  > "$Z/trip_point_0_hyst"
    ln -sfn ../cooling_device0 "$Z/cdev0"
    [ "$5" = 2 ] && ln -sfn ../cooling_device1 "$Z/cdev1"
    for c in "$C0" "$C1"; do
        echo emc2305_fan > "$c/type"
        echo "$3" > "$c/cur_state"
        echo 10 > "$c/max_state"
    done
}

run() { QCCTL_SYSROOT=$FX sh "$SCRIPT" cpuss-0-0 2>&1; }

# The clear point for every shape below is 38000 - 3000 = 35000 mC.

# 1. A SIGKILLed daemon leaves the zone disabled and the fan wherever its curve had it. On a
#    board this cool the kernel's own answer is 0, and nothing but this script will write it.
shape disabled 32000 10 active 1
out=$(run)
t_file_is "$C0/cur_state" 0 "a stuck fan on a cool board is set to 0"
t_file_is "$Z/mode" enabled "and the zone is handed back"
t_has "$out" "fan was state 10" "the report names the state it found"
t_has "$out" "35000mC" "and the threshold it judged against"

# 2. Inside the hysteresis band the governor is still entitled to a throttled state, so the
#    script must not touch the fan - only re-enable the zone.
shape disabled 37000 10 active 1
out=$(run)
t_file_is "$C0/cur_state" 10 "inside the band the fan is left alone"
t_file_is "$Z/mode" enabled "while the zone is still handed back"
t_hasnt "$out" "fan was state" "and nothing is claimed about the fan"

# 3. Hot: more cooling than the floor is what the board needs, so leave it.
shape disabled 42000 8 active 1
out=$(run)
t_file_is "$C0/cur_state" 8 "above the trip the fan is left alone"
t_file_is "$Z/mode" enabled "zone handed back"

# 4. Already off, nothing to correct.
shape disabled 32000 0 active 1
out=$(run)
t_file_is "$C0/cur_state" 0 "a fan already off stays off"
t_hasnt "$out" "fan was state" "with nothing reported about it"

# 5. Two cooling devices: which of them is the fan is a guess, and a guess is not allowed.
#    The cooling-map reader has the same limit, which is an open item in TODO.md.
shape disabled 32000 10 active 2
out=$(run)
t_file_is "$C0/cur_state" 10 "with two cooling devices the script will not guess"
t_file_is "$Z/mode" enabled "but still hands the zone back"

# 6. The zone already enabled means a release ALREADY ran - a clean stop, or a FAILSAFE full
#    release that deliberately left the fan at maximum. Neither is this script's to undo.
shape enabled 32000 10 active 1
out=$(run)
t_file_is "$C0/cur_state" 10 "an enabled zone means a release ran: the fan is not touched"
t_eq "$out" "" "and the script says nothing at all"

# 7. No active trip: no clear point, so no judgement to make.
shape disabled 32000 10 passive 1
out=$(run)
t_file_is "$C0/cur_state" 10 "with no active trip the fan is left alone"
t_file_is "$Z/mode" enabled "zone handed back"

# 8. It can never fail a unit stop, whatever it finds - including a zone that is not there.
shape disabled 32000 10 active 1
QCCTL_SYSROOT=$FX sh "$SCRIPT" cpuss-0-0 >/dev/null 2>&1
t_eq "$?" 0 "a run that acts exits 0"
QCCTL_SYSROOT=$FX sh "$SCRIPT" no-such-zone >/dev/null 2>&1
t_eq "$?" 0 "and so does one that finds no zone at all"
out=$(QCCTL_SYSROOT=$FX sh "$SCRIPT" no-such-zone 2>&1)
t_has "$out" "no thermal zone named" "which it reports rather than passing silently"

t_summary
