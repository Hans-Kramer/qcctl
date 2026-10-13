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

found=0
for z in "$THERMAL"/thermal_zone*; do
    [ -r "$z/type" ] || continue
    t=$(cat "$z/type" 2>/dev/null)
    [ "$t" = "$NAME" ] || [ "$t" = "$NAME-thermal" ] || continue
    found=1
    # Only act if it is actually disabled, so an already-enabled zone is a silent no-op.
    if [ "$(cat "$z/mode" 2>/dev/null)" = disabled ]; then
        echo enabled > "$z/mode" 2>/dev/null || true
        if [ "$(cat "$z/mode" 2>/dev/null)" = enabled ]; then
            echo "qcctl-fan-release: ${z##*/} ($NAME) returned to the kernel"
        else
            # Nothing more can be done here, so say loudly that the fan is unmanaged.
            echo "qcctl-fan-release: FAILED to enable ${z##*/} ($NAME) - fan is NOT" >&2
            echo "qcctl-fan-release: under kernel control. Run: echo enabled > $z/mode" >&2
        fi
    fi
done

[ "$found" = 1 ] || echo "qcctl-fan-release: no thermal zone named '$NAME' or '$NAME-thermal' under $THERMAL" >&2
exit 0
