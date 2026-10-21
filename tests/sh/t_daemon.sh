#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer
# SPDX-FileContributor: Claude Opus 5 (Anthropic), paired on the behavioural suite
#
# The daemon's whole loop, with no board. `--stats-cmd` exists for this - its own --help says
# it is for the tests - and with a fixture sysroot the fan writes land in the captured tree
# instead of on hardware. Five of the defects fixed this week live in this path, and every
# one of them was verified by hand on a board.
#
# The stream replayed here is the REAL captured qcstats output from tests/fixtures/stream,
# with only the temperature substituted, so the parser is exercised against a line a producer
# actually emitted rather than one written to suit the consumer.
set -u

. "$(dirname "$0")/t.sh"

BIN=build/qcctld
[ -x "$BIN" ] || { echo "    t_daemon: $BIN not built" >&2; exit 2; }

FXSRC=$(ls -d tests/fixtures/monza-* 2>/dev/null | head -1)
[ -n "$FXSRC" ] || { echo "    t_daemon: no captured fixture" >&2; exit 2; }
SAMPLE=tests/fixtures/stream/sample.ndjson
[ -f "$SAMPLE" ] || { echo "    t_daemon: no captured stream" >&2; exit 2; }

RT=$(mktemp -d /tmp/t_daemon.XXXXXX) || exit 2
# A bare `kill 0` signals the whole process group, this test included, so every signal
# here is guarded by a set pid rather than defaulted to one.
kill_if() { [ -n "${1:-}" ] && kill "$2" "$1" 2>/dev/null; return 0; }
trap 'kill_if "${DPID:-}" -KILL; kill_if "${CMPID:-}" -KILL; rm -rf "$RT"' EXIT INT TERM
FX=$RT/sysroot
mkdir -p "$FX"
cp -a "$FXSRC/." "$FX/"
export QCCTL_SYSROOT=$FX

ZONE=$FX/sys/class/thermal/thermal_zone37
FAN=$FX/sys/class/thermal/cooling_device7

# One real sample line with the zone temperature replaced.
line_at() {
    grep '"thermal_c"' "$SAMPLE" | tail -1 |
        sed "s/\"cpuss-0-0\":[0-9.]*/\"cpuss-0-0\":$1/"
}

# stream_of <celsius...> - a command that replays those temperatures, then stays alive so the
# daemon does not see EOF and start its sampler-recovery path.
stream_of() {
    : > "$RT/stream"
    for t in "$@"; do line_at "$t" >> "$RT/stream"; done
    echo "while IFS= read -r l; do printf '%s\n' \"\$l\"; sleep 0.4; done < $RT/stream; sleep 30"
}

conf_with_curve() {
    cat > "$RT/profiles.conf" <<EOF
< PROFILE ID=0 NAME=p DEFAULT >
  FAN  MODE  curve
  FAN  CURVE t
>
EOF
    cat > "$RT/fan.config" <<EOF
< CURVE NAME=t >
  ZONE cpuss-0-0
  POINT 30C STATE 0
  POINT 38C STATE 4
  POINT 43C STATE 6
  HYSTERESIS 4C
  MIN_STATE 2
  POLL 1s
  FAILSAFE kernel
>
EOF
}

conf_without_strategy() {
    cat > "$RT/profiles.conf" <<EOF
< PROFILE ID=0 NAME=p DEFAULT >
  CPU_POLICY 0  GOVERNOR schedutil
>
EOF
    : > "$RT/fan.config"
}

start() {   # start <stats-cmd> [extra args...]
    # Callers may prefix this with environment settings - QCCTL_FANWD_FORGET_MS=4000 start ...
    # - which reach the daemon because this is a function and not a subshell.
    _cmd=$1; shift
    rm -f "$RT/pid" "$RT/log"
    "$BIN" --foreground --no-apply -c "$RT/profiles.conf" --fan-config "$RT/fan.config" \
        --pidfile "$RT/pid" --status-file "$RT/status" --control-socket "$RT/sock" \
        --active-file "$RT/active" --selection-file "$RT/selected" \
        --stats-cmd "$_cmd" "$@" > "$RT/log" 2>&1 &
    DPID=$!
}

stop() {
    [ -n "${DPID:-}" ] || return 1
    kill -TERM "$DPID" 2>/dev/null
    _i=0
    while [ $_i -lt 40 ]; do
        kill -0 "$DPID" 2>/dev/null || break
        sleep 0.25; _i=$((_i + 1))
    done
    if kill -0 "$DPID" 2>/dev/null; then
        kill -KILL "$DPID" 2>/dev/null
        return 1
    fi
    return 0
}

# ---- A: a curve drives the fan, and the release hands back the kernel's own state --------
conf_with_curve
echo 0 > "$FAN/cur_state"
echo enabled > "$ZONE/mode"
start "$(stream_of 36.0 39.0 44.0 44.0)"
sleep 3
t_file_is "$ZONE/mode" disabled "the daemon takes the zone to drive a curve"
t_file_is "$FAN/cur_state" 6 "and the curve has driven the fan to the 43C point"
log=$(cat "$RT/log")
t_has "$log" "took zone37" "the take is logged"
t_has "$log" "fan 0 -> 4" "and each step"
stop
t_ok "$?" "it stops on SIGTERM"
t_file_is "$ZONE/mode" enabled "the zone is handed back"
t_file_is "$FAN/cur_state" 4 "with the fan at the cooling map's floor, because 44C is hot"
t_has "$(cat "$RT/log")" "what the kernel holds at 44.0 C" "and the release says so"

# ---- B: the same release, cool - which is where the never-lower rule used to pin the fan --
echo 0 > "$FAN/cur_state"
echo enabled > "$ZONE/mode"
start "$(stream_of 39.0 44.0 34.0 34.0)"
sleep 3
stop
t_ok "$?" "a second run stops too"
t_file_is "$FAN/cur_state" 0 "released below the trip, the fan goes to 0, not to max(driven)"
t_file_is "$ZONE/mode" enabled "zone handed back"

# ---- C: the watchdog corrects a state the kernel would never hold ------------------------
conf_without_strategy
echo 8 > "$FAN/cur_state"
echo enabled > "$ZONE/mode"
start "$(stream_of 33.0 33.0 33.0 33.0)"
sleep 3
t_file_is "$FAN/cur_state" 0 "a stale state below the clear point is corrected"
t_has "$(cat "$RT/log")" "below the 35.0C its trip clears at" "and the correction is logged"
stop
t_ok "$?" "stops"

# ---- C2: the watchdog is bounded, and the bound is forgotten --------------------------
# The cap exists because "a state that keeps coming back is someone's decision rather than a
# leftover". Two of its three mechanisms are reachable in seconds; the third is not.
#
#   the 60s gate between corrections - provoked below by asking for a second one at once
#   the forget window, via QCCTL_FANWD_FORGET_MS, a hook built "for the tests" and until
#     now used by none
#   the cap at exactly three - NOT covered: three corrections need two 60s waits, and
#     FANWD_INTERVAL_MS has no override. Written down rather than faked.
conf_without_strategy
echo 8 > "$FAN/cur_state"
echo enabled > "$ZONE/mode"
QCCTL_FANWD_FORGET_MS=4000 start "$(stream_of 33.0 33.0 33.0 33.0 33.0 33.0 33.0 33.0 \
                                             33.0 33.0 33.0 33.0 33.0 33.0 33.0 33.0 \
                                             33.0 33.0 33.0 33.0 33.0 33.0 33.0 33.0)"
sleep 3
t_file_is "$FAN/cur_state" 0 "the first stale state is corrected"
n=$(grep -c "fan was state" "$RT/log")
t_eq "$n" 1 "once"

# A second stale state inside the 60s gate must be LEFT: the daemon reports drift rather
# than fighting whoever is writing it.
echo 8 > "$FAN/cur_state"
sleep 4
t_file_is "$FAN/cur_state" 8 "a second stale state within the interval is left alone"
t_eq "$(grep -c 'fan was state' "$RT/log")" 1 "with no second correction logged"

# With the forget window set short, the count is dropped and the daemon says so. The gate
# is still 60s, so this asserts the FORGETTING, not another correction.
sleep 3
t_has "$(cat "$RT/log")" "correction(s) are forgotten" "the cap is forgotten after its window"
stop
t_ok "$?" "stops"
echo enabled > "$ZONE/mode"

# ---- D: and stands down for whoever else holds the zone ----------------------------------
echo 8 > "$FAN/cur_state"
echo disabled > "$ZONE/mode"
start "$(stream_of 33.0 33.0 33.0 33.0)"
sleep 3
t_file_is "$FAN/cur_state" 8 "a DISABLED zone belongs to someone else: the fan is left alone"
t_hasnt "$(cat "$RT/log")" "fan was state" "and nothing is claimed"
stop
t_ok "$?" "stops"
echo enabled > "$ZONE/mode"

# ---- E: a sampler that ignores SIGTERM must not hold the exit open -----------------------
start 'trap "" TERM; while :; do sleep 1; done'
sleep 2
t0=$(date +%s)
stop
rc=$?
t1=$(date +%s)
t_ok "$rc" "a TERM-ignoring sampler does not hold the daemon open"
t_ok "$([ $((t1 - t0)) -le 3 ] && echo 0 || echo 1)" "and the exit takes a second, not a timeout"
t_has "$(cat "$RT/log")" "did not exit on SIGTERM and was killed" "the escalation is logged"

# ---- F: a status file that cannot be written is reported once, not per poll ---------------
# The status file is published once per SAMPLE, so the stream has to keep arriving across
# both halves of this case - the failure and the recovery.
start "$(stream_of 36.0 36.0 36.0 36.0 36.0 36.0 36.0 36.0 36.0 36.0 36.0 36.0 36.0 36.0 \
                   36.0 36.0 36.0 36.0 36.0 36.0)" --status-file "$RT/nodir/status"
sleep 3
n=$(grep -c "cannot publish the status file" "$RT/log" 2>/dev/null || echo 0)
t_eq "$n" 1 "one warning across many polls, not one per poll"
mkdir -p "$RT/nodir"
sleep 2
t_eq "$(grep -c 'cannot publish the status file' "$RT/log")" 1 \
     "and still only one after it starts working"
t_ok "$([ -s "$RT/nodir/status" ] && echo 0 || echo 1)" "the status file is written"

t_has "$(cat "$RT/log")" "is being written again" "and the recovery is reported too"
stop
t_ok "$?" "stops"

# ---- F2: a REBASELINE from a real client ------------------------------------------------
# The other word the socket accepts, and no shell test sent one - though this file already
# drives the same socket for --cool-max. It is what an unprivileged GUI uses after
# `pkexec qcctl -m <name>` so the daemon's status and drift baseline stop describing the
# profile that is no longer in force. The request cannot CHOOSE a profile: the daemon reads
# the name from the root-owned active file, which is the whole of its privilege argument.
cat > "$RT/two.conf" <<'EOF'
< PROFILE ID=0 NAME=first DEFAULT >
  CPU_POLICY 0  GOVERNOR schedutil
>
< PROFILE ID=1 NAME=second >
  CPU_POLICY 0  GOVERNOR performance
>
EOF
: > "$RT/fan.config"
echo first > "$RT/active"
rm -f "$RT/pid" "$RT/log"
"$BIN" --foreground --no-apply -c "$RT/two.conf" --fan-config "$RT/fan.config" \
    --pidfile "$RT/pid" --status-file "$RT/status" --control-socket "$RT/sock" \
    --active-file "$RT/active" --selection-file "$RT/selected" \
    --stats-cmd "$(stream_of 33.0 33.0 33.0 33.0 33.0 33.0 33.0 33.0 33.0 33.0 33.0 33.0)" \
    > "$RT/log" 2>&1 &
DPID=$!
sleep 3
t_has "$(cat "$RT/status")" '"profile":"first"' "the daemon reports the profile in force"

# A real apply through the CLI, which notifies the daemon on its way out.
out=$(build/qcctl -c "$RT/two.conf" -m second --active-file "$RT/active" \
        --control-socket "$RT/sock" --selection-file "$RT/selected" \
        --state "$RT/state" 2>&1)
t_has "$out" "qcctld notified" "the CLI says it notified the daemon"
sleep 3
t_has "$(cat "$RT/log")" "rebaseline" "and the daemon logs the request"
t_has "$(cat "$RT/log")" "second" "naming the profile it read from the active file"
t_has "$(cat "$RT/status")" '"profile":"second"' "its published status follows"
stop
t_ok "$?" "stops"

# ---- G: --cool-max, over the socket, with a real client holding ---------------------------
# The privileged half of cool-max: an unprivileged caller asks the daemon, which is already
# root. The hold IS the connection, so this needs a second process that stays alive - which
# is why it lives here and not in t_cli.sh.
conf_with_curve
echo 0 > "$FAN/cur_state"
echo enabled > "$ZONE/mode"
start "$(stream_of 33.0 33.0 33.0 33.0 33.0 33.0 33.0 33.0 33.0 33.0 33.0 33.0 33.0 33.0 \
                   33.0 33.0 33.0 33.0 33.0 33.0 33.0 33.0 33.0 33.0 33.0 33.0 33.0 33.0)"
sleep 3
# 0 and not MIN_STATE: the minimum applies to a fan the curve wants ON, and off stays a
# legal answer below the first point. t_curve.c pins the same rule.
t_file_is "$FAN/cur_state" 0 "the curve holds the fan off while the board is cool"

build/qcctl --cool-max --control-socket "$RT/sock" > "$RT/cm.log" 2>&1 &
CMPID=$!
sleep 3
t_file_is "$FAN/cur_state" 10 "a cool-max hold forces the fan to maximum"
t_has "$(cat "$RT/status")" '"cool_max_holds":1' "and the status file reports the hold"
t_has "$(cat "$RT/log")" "cool-max: fan forced to state 10" "the daemon logs the force"

# The client's own account, read WHILE it is still holding. stdout to a file used to be
# block-buffered, so none of this appeared until the process exited - backwards for a
# command whose purpose is to report a hold while it lasts. Fixed 2026-10-21.
live=$(cat "$RT/cm.log")
t_has "$live" "cooling at maximum" "the client says so while the hold is still active"
t_hasnt "$live" "hold released" "and has not claimed to have released it"

# However the client exits. SIGTERM here; the board check covers the kill.
kill -TERM "$CMPID" 2>/dev/null
wait "$CMPID" 2>/dev/null
CMPID=""
sleep 3
t_has "$(cat "$RT/status")" '"cool_max_holds":0' "the hold ends with the client"
t_file_is "$FAN/cur_state" 0 "and the curve has the fan back"
cm=$(cat "$RT/cm.log")
t_has "$cm" "cooling at maximum" "the client was told the daemon did the work"
t_hasnt "$cm" "needs root" "and was never asked for a password"
t_has "$cm" "hold released" "and, once it ends, that the hold ended"
stop
t_ok "$?" "stops"

t_summary
