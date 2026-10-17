#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer
#
# Hand the fan's thermal zone back to the kernel. Installed as
# /usr/sbin/qcctl-fan-release and run from the unit's ExecStopPost, which is the
# backstop for a SIGKILL the daemon cannot catch.
#
# Resolves the zone by name, never by index. Idempotent, silent on success, and always
# exits 0 so it can never fail a unit stop.
#
# The short config spelling is accepted as well as the full sysfs type - 'cpuss-0-0' and
# 'cpuss-0-0-thermal' both resolve - because that is what surface_zone() does, and because
# the recovery command the daemon prints carries the config spelling.
set -u
NAME=${1:-cpuss-0-0-thermal}
NAME=${NAME%-thermal}

# QCCTL_SYSROOT relocates the paths onto a fixture tree. Unset leaves them as they are.
THERMAL="${QCCTL_SYSROOT:-}/sys/class/thermal"

# The temperature below which the kernel treats a trip as clear, from the lowest `active`
# trip and its hysteresis. Prints nothing if there is no readable active trip.
clear_point() {
    _trip= ; _hyst=0
    for _t in "$1"/trip_point_*_type; do
        [ -r "$_t" ] || continue
        [ "$(cat "$_t" 2>/dev/null)" = active ] || continue
        _n=${_t##*/trip_point_}; _n=${_n%_type}
        _tv=$(cat "$1/trip_point_${_n}_temp" 2>/dev/null)
        case "$_tv" in ''|*[!0-9]*) continue ;; esac
        if [ -z "$_trip" ] || [ "$_tv" -lt "$_trip" ]; then
            _trip=$_tv
            _hyst=$(cat "$1/trip_point_${_n}_hyst" 2>/dev/null)
            case "$_hyst" in ''|*[!0-9]*) _hyst=0 ;; esac
        fi
    done
    [ -n "$_trip" ] && echo $((_trip - _hyst))
}

# The fan, not just the zone: the kernel rewrites a cooling device only on a trip
# transition, so a state left behind by a daemon that died is one nothing will correct.
# Below the clear point the kernel's own answer is 0, which is the only state it can be
# given here without second-guessing a governor that is entitled to its own.
release_fan() {
    _cdev=$(readlink "$1/cdev0" 2>/dev/null); _cdev=${_cdev##*/}
    # One binding only: with several, which of them is the fan is a guess.
    [ -n "$_cdev" ] && [ ! -e "$1/cdev1" ] || return 0
    _temp=$(cat "$1/temp" 2>/dev/null)
    _clear=$(clear_point "$1")
    case "$_temp" in ''|*[!0-9]*) return 0 ;; esac
    [ -n "$_clear" ] || return 0
    [ "$_temp" -lt "$_clear" ] || return 0
    _state=$(cat "$THERMAL/$_cdev/cur_state" 2>/dev/null)
    case "$_state" in ''|*[!0-9]*) return 0 ;; esac
    [ "$_state" -gt 0 ] || return 0
    echo 0 > "$THERMAL/$_cdev/cur_state" 2>/dev/null || true
    FANNOTE=" (fan was state $_state at ${_temp}mC, below the ${_clear}mC its trip clears at - set to 0)"
}

found=0
FANNOTE=
for z in "$THERMAL"/thermal_zone*; do
    [ -r "$z/type" ] || continue
    t=$(cat "$z/type" 2>/dev/null)
    [ "$t" = "$NAME" ] || [ "$t" = "$NAME-thermal" ] || continue
    found=1
    # Only act if it is actually disabled, so an already-enabled zone is a silent no-op.
    # Disabled also means no release ran, which is what makes this the SIGKILL backstop -
    # a clean release leaves the zone enabled, and FAILSAFE full deliberately leaves the
    # fan at maximum. Neither is this script's to undo.
    if [ "$(cat "$z/mode" 2>/dev/null)" = disabled ]; then
        # The fan first, then the zone: the same order the daemon's own release uses.
        release_fan "$z"
        echo enabled > "$z/mode" 2>/dev/null || true
        if [ "$(cat "$z/mode" 2>/dev/null)" = enabled ]; then
            echo "qcctl-fan-release: ${z##*/} ($NAME) returned to the kernel$FANNOTE"
        else
            # Nothing more can be done here, so say loudly that the fan is unmanaged.
            echo "qcctl-fan-release: FAILED to enable ${z##*/} ($NAME) - fan is NOT" >&2
            echo "qcctl-fan-release: under kernel control. Run: echo enabled > $z/mode" >&2
        fi
    fi
done

[ "$found" = 1 ] || echo "qcctl-fan-release: no thermal zone named '$NAME' or '$NAME-thermal' under $THERMAL" >&2
exit 0
