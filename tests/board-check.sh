#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer
# SPDX-FileContributor: Claude Opus 5 (Anthropic), paired on the behavioural suite
#
# What only hardware can answer. Run this ON a Ventuno Q, as root, with qcctl installed.
#
# The gate does not run this and cannot: every check below depends on a thermal governor
# reacting to a real temperature, a fan that actually turns, or a driver that clamps. The
# suite in tests/ covers everything that does not. This script exists so the measured figures
# in docs/USAGE.md and etc/fan.config are reproducible by someone other than their author.
#
#   sh board-check.sh              # the default sections
#   sh board-check.sh --list       # what there is
#   sh board-check.sh release      # just one
#
# It restores the board on the way out, whatever happens: the default profile applied, the
# unit running, the fan where the kernel itself would hold it.
set -u

SECTIONS_DEFAULT="surface reset watchdog capture install release"
SECTIONS_ALL="$SECTIONS_DEFAULT envelope"

CHECKS=0
FAILS=0
SKIPS=0
ZONE=
CDEV=
ZONEDIR=
CDEVDIR=

ok()   { CHECKS=$((CHECKS + 1)); printf '    ok    %s\n' "$1"; }
bad()  { CHECKS=$((CHECKS + 1)); FAILS=$((FAILS + 1)); printf '    FAIL  %s\n' "$1"; }
note() { printf '          %s\n' "$1"; }
# A section whose PRECONDITION the room cannot meet is skipped, not failed - and the skip is
# counted and repeated in the summary, because a skip nobody notices is worse than a failure.
# The watchdog section needs the board below its clear point, which on this hardware is below
# the idle temperature in a warm room: see the note there.
skip_() { SKIPS=$((SKIPS + 1)); printf '    SKIP  %s\n' "$1"; }
head_() { printf '\n== %s\n' "$1"; }

# eq <got> <want> <what>
eq() { [ "$1" = "$2" ] && ok "$3" || bad "$3 (got '$1', want '$2')"; }

# Resolve the zone and the fan BY NAME, never by index - the same rule the rest of the
# project follows, and the reason a fixture can stand in for a board at all.
# First match wins, and a second is reported rather than silently preferred: without the
# break this took the LAST match, so on a board that exposed the name twice it would have
# resolved something other than what it reported - and said nothing. The release script and
# install.sh both break on first match; this is the same rule.
resolve() {
    nz=0; nc=0
    for z in /sys/class/thermal/thermal_zone*; do
        [ "$(cat "$z/type" 2>/dev/null)" = cpuss-0-0-thermal ] || continue
        nz=$((nz + 1))
        [ -n "$ZONEDIR" ] || { ZONEDIR=$z; ZONE=${z##*/}; }
    done
    for c in /sys/class/thermal/cooling_device*; do
        [ "$(cat "$c/type" 2>/dev/null)" = emc2305_fan ] || continue
        nc=$((nc + 1))
        [ -n "$CDEVDIR" ] || { CDEVDIR=$c; CDEV=${c##*/}; }
    done
    [ "$nz" -le 1 ] || note "WARNING: $nz zones report cpuss-0-0-thermal; using $ZONE"
    [ "$nc" -le 1 ] || note "WARNING: $nc cooling devices report emc2305_fan; using $CDEV"
    [ -n "$ZONEDIR" ] && [ -n "$CDEVDIR" ]
}

temp()  { cat "$ZONEDIR/temp"; }
fan()   { cat "$CDEVDIR/cur_state"; }
mode()  { cat "$ZONEDIR/mode"; }
trip()  { cat "$ZONEDIR/trip_point_2_temp"; }
hyst()  { cat "$ZONEDIR/trip_point_2_hyst"; }
clearp() { echo $(( $(trip) - $(hyst) )); }
state() { printf 'temp=%s fan=%s mode=%s' "$(temp)" "$(fan)" "$(mode)"; }

# load <seconds> - self-limiting, so an interrupted run cannot leave the board busy.
load() {
    _n=0
    while [ "$_n" -lt 8 ]; do
        timeout "$1" sh -c 'while : ; do : ; done' >/dev/null 2>&1 &
        _n=$((_n + 1))
    done
}
load_stop() { pkill -f 'while : ; do : ; done' >/dev/null 2>&1 || true; sleep 1; }

# wait_until <seconds> <shell-condition>
wait_until() {
    _d=$1; shift
    _i=0
    while [ "$_i" -lt "$_d" ]; do
        eval "$@" && return 0
        sleep 1; _i=$((_i + 1))
    done
    return 1
}

profile() { qcctl -m "$1" >/dev/null 2>&1; systemctl restart qcctld; sleep 5; }
jlog()    { journalctl -u qcctld --no-pager -n "${1:-40}" | sed 's/.*qcctld\[[0-9]*\]: //'; }
# The last N LINES may be minutes old - a previous section's, or a previous run's. Where a
# check asks "did the daemon do this JUST NOW", it has to be bounded in time instead: mark_
# takes a timestamp and jsince reads only what the daemon has said since. Without this, an
# assertion that the watchdog did NOT act was satisfied by its own correction from an earlier
# section, and reported a defect that was not there.
# Local time, because that is what --since reads; the rest of this script prints UTC, which
# is why this is the one timestamp that is not -u.
mark_()   { date '+%Y-%m-%d %H:%M:%S'; }
jsince()  { journalctl -u qcctld --no-pager --since "$1" 2>/dev/null \
                | sed 's/.*qcctld\[[0-9]*\]: //'; }

# check_release_rule <since> <what> - find the release line the daemon logged since <since>
# and check the state it chose against the temperature and trip IT reported:
#   below the trip        -> 0, the fan off
#   at or above the trip  -> MAP_FLOOR, which the kernel itself would hold there
# Reading all three numbers out of one line is what makes this race-free: the decision and
# the temperature it was made from come from the same moment.
MAP_FLOOR=4
check_release_rule() {
    _line=$(jsince "$1" | grep 'fan set to state' | tail -1)
    if [ -z "$_line" ]; then
        bad "$2: the daemon logged no release at all"
        return
    fi
    _st=$(printf '%s\n' "$_line" | sed -n 's/.*fan set to state \([0-9-]*\).*/\1/p')
    _t=$(printf '%s\n' "$_line"  | sed -n 's/.*holds at \([0-9.]*\) C.*/\1/p')
    _tr=$(printf '%s\n' "$_line" | sed -n 's/.*(trip \([0-9.]*\) C).*/\1/p')
    if [ -z "$_st" ] || [ -z "$_t" ] || [ -z "$_tr" ]; then
        bad "$2: cannot read the release line: $_line"
        return
    fi
    # Compare in milli-degrees, so this needs no floating point.
    _tmc=$(printf '%s\n' "$_t"  | awk '{printf "%d", $1 * 1000}')
    _trmc=$(printf '%s\n' "$_tr" | awk '{printf "%d", $1 * 1000}')
    if [ "$_tmc" -ge "$_trmc" ]; then
        eq "$_st" "$MAP_FLOOR" "$2 at ${_t}C (trip ${_tr}C) decides on the map's floor"
    else
        eq "$_st" 0 "$2 at ${_t}C (trip ${_tr}C) decides on state 0"
    fi
}

restore() {
    load_stop
    # The watchdog section raises the fan's active trip; put it back whatever happened.
    [ -n "${TRIP_SAVED:-}" ] && [ -n "${ZONEDIR:-}" ] \
        && printf '%s\n' "$TRIP_SAVED" > "$ZONEDIR/trip_point_2_temp" 2>/dev/null
    systemctl start qcctld >/dev/null 2>&1 || true
    qcctl -m balanced >/dev/null 2>&1 || true
    systemctl restart qcctld >/dev/null 2>&1 || true
    sleep 3
    # Whatever the run did, leave the fan where the kernel would hold it.
    if [ -n "$CDEVDIR" ] && [ -n "$ZONEDIR" ]; then
        echo enabled > "$ZONEDIR/mode" 2>/dev/null || true
        if [ "$(temp)" -lt "$(trip)" ]; then echo 0 > "$CDEVDIR/cur_state" 2>/dev/null || true
        else echo 4 > "$CDEVDIR/cur_state" 2>/dev/null || true; fi
    fi
}

# ------------------------------------------------------------------ sections
s_surface() {
    head_ "surface: what this board exposes"
    out=$(qcctl-surface-dump 2>&1)
    eq "$?" 0 "qcctl-surface-dump runs"
    case "$out" in *cpuss-0-0*) ok "it finds the fan's zone" ;; *) bad "no cpuss-0-0 in the dump" ;; esac
    case "$out" in *emc2305_fan*) ok "and the fan's cooling device" ;; *) bad "no emc2305_fan" ;; esac
    for p in policy0 policy2 policy4; do
        case "$out" in *$p*) ok "$p is present" ;; *) bad "$p is missing" ;; esac
    done
    note "zone=$ZONE cdev=$CDEV trip=$(trip) hyst=$(hyst) clear=$(clearp)"
    eq "$(mode)" enabled "the zone is under kernel control at rest"
}

s_reset() {
    head_ "reset: the board goes back to what the snapshot recorded"
    profile quiet
    note "with 'quiet' applied: policy0 max=$(cat /sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq)"
    out=$(qcctl --reset 2>&1); rc=$?
    eq "$rc" 0 "qcctl --reset succeeds"
    case "$out" in *"read back as the snapshot has them"*)
        ok "and verifies every value it restored" ;;
        *) bad "no read-back report: $out" ;;
    esac
    note "$(printf '%s' "$out" | tr '\n' ' ')"
}

s_watchdog() {
    head_ "watchdog: a state the kernel would never hold"
    profile balanced
    # THIS SECTION NEEDS THE BOARD BELOW ITS CLEAR POINT, which as shipped is 35.0 C - and
    # this board idles at about 36 C on the OE cooler. Cooling the room is not a test
    # strategy: the first version of this waited for the fan to get under 35 C, which
    # depends on the weather, failed on a warm day, and reported it as a defect.
    #
    # So the THRESHOLD is moved instead of the board. The fan's active trip is raised for the
    # duration, which lifts the clear point with it (clear = trip - hysteresis), and the
    # board's steady temperature is then comfortably below it. Only the ACTIVE trip is
    # touched - the passive ones at 130 and 135 C, which are the actual protections, are not
    # - and it is put back below and by the restore trap however this exits.
    TRIP_SAVED=$(cat "$ZONEDIR/trip_point_2_temp")
    if ! printf '%s\n' "$(( TRIP_SAVED + 8000 ))" > "$ZONEDIR/trip_point_2_temp" 2>/dev/null; then
        skip_ "this kernel will not let the active trip be moved, so the clear point is fixed"
        return
    fi
    note "active trip raised $TRIP_SAVED -> $(cat "$ZONEDIR/trip_point_2_temp"), clear point now $(clearp)"
    # AND THE DAEMON HAS TO BE RESTARTED TO SEE IT. The surface - trip temperatures included
    # - is discovered once at startup, so a trip moved underneath a running daemon leaves its
    # watchdog comparing against the old clear point. Worth knowing in its own right: after
    # `qcctl -m quiet`, which raises this very trip to 45 C, the running daemon's watchdog
    # keeps using the shipped 35 C until it is restarted. It under-acts rather than
    # over-acts, and a restart fixes it, which is why this is a note and not a defect.
    systemctl restart qcctld
    sleep 3

    if ! wait_until 60 '[ $(temp) -lt $(( $(clearp) - 500 )) ]'; then
        note "board is at $(temp), still not clear of $(clearp) - that is unexpected"
        printf '%s\n' "$TRIP_SAVED" > "$ZONEDIR/trip_point_2_temp"
        skip_ "the board will not sit below the raised clear point"
        return
    fi
    since=$(mark_)
    echo 8 > "$CDEVDIR/cur_state"
    note "wrote state 8 by hand at $(temp)"
    if wait_until 20 '[ $(fan) = 0 ]'; then
        ok "the daemon corrects it to 0"
    else
        bad "still at state $(fan) after 20s, with the board at $(temp)"
    fi
    case "$(jsince "$since")" in
        *"its trip clears at"*) ok "and says why" ;;
        *) bad "no correction in the journal since the state was written" ;;
    esac

    # And it must stand down for another owner: a disabled zone belongs to whoever took it.
    #
    # THE DAEMON IS RESTARTED FIRST. A correction just fired above, which sets the
    # watchdog's last-correction clock, and the gate between corrections is 60s - so a
    # 12-second wait proved nothing: the watchdog would not have acted either way, and this
    # check passed whether or not the ownership test existed. A fresh daemon starts with
    # that clock at zero, which is exactly why the fixture test (t_daemon.sh, section D)
    # catches the mutation this one missed.
    systemctl restart qcctld
    sleep 3
    echo disabled > "$ZONEDIR/mode"
    since=$(mark_)
    echo 8 > "$CDEVDIR/cur_state"
    sleep 12
    eq "$(fan)" 8 "a disabled zone is left alone"
    # Bounded to this window only. Read as the last N lines, this matched the daemon's own
    # correction from the half above and reported a defect that had not happened.
    case "$(jsince "$since")" in
        *"its trip clears at"*) bad "the watchdog corrected a zone it does not hold" ;;
        *) ok "and the journal shows it did not try" ;;
    esac
    printf '%s\n' "$TRIP_SAVED" > "$ZONEDIR/trip_point_2_temp"
    note "active trip restored to $(cat "$ZONEDIR/trip_point_2_temp")"
    echo enabled > "$ZONEDIR/mode"
}

s_capture() {
    head_ "capture: the fan baseline measures what it says"
    profile balanced
    out=$(qcctl --fan-baseline capture 2>&1); rc=$?
    eq "$rc" 0 "the capture completes"
    case "$out" in *"written to"*) ok "and writes the baseline" ;; *) bad "no baseline written" ;; esac
    n=$(printf '%s' "$out" | grep -c '^  state ')
    [ "$n" -ge 8 ] && ok "$n states measured" || bad "only $n states measured"
    case "$out" in *"asked for state"*) bad "a state did not hold - read-back refused it" ;;
        *) ok "every state it drove to was the state the fan held" ;; esac
    systemctl restart qcctld; sleep 6
    case "$(jlog 25)" in *"judging against"*) ok "the daemon judges airflow against it" ;;
        *) bad "the daemon is not judging airflow" ;; esac
}

s_install() {
    head_ "install: removal is reversible, and the operator's data survives it"
    [ -f /var/lib/qcctl/fan-baseline ] || { note "no baseline to protect; capturing one"; qcctl --fan-baseline capture >/dev/null 2>&1; }
    sum=$(md5sum /var/lib/qcctl/fan-baseline | cut -d' ' -f1)
    out=$(qcctl-uninstall 2>&1); rc=$?
    eq "$rc" 0 "a plain uninstall succeeds"
    case "$out" in *"REMOVED cleanly"*) ok "and reports a clean removal" ;; *) bad "not a clean removal" ;; esac
    left=0
    for p in /usr/sbin/qcctld /usr/sbin/qcctl /usr/sbin/qcctl-surface-dump /usr/bin/qcstats; do
        [ -e "$p" ] && { bad "$p is still there"; left=1; }
    done
    [ "$left" = 0 ] && ok "every binary is gone"
    eq "$(md5sum /var/lib/qcctl/fan-baseline 2>/dev/null | cut -d' ' -f1)" "$sum" \
       "the fan baseline survives, byte-identical"
    [ -f /etc/qcctl/profiles.conf ] && ok "the config is kept" || bad "the config was removed"

    [ -d "${STAGE:-/tmp/stage}" ] || { note "no staging dir at ${STAGE:-/tmp/stage}; cannot reinstall"; return; }
    sh "${STAGE:-/tmp/stage}/install.sh" >/dev/null 2>&1
    sleep 4
    eq "$(systemctl is-active qcctld)" active "the reinstall brings the unit back"
    # The SET, not a count. A count was here and it was wrong twice over: polkit is optional,
    # so an image without /usr/share/polkit-1/actions installs correctly with one path fewer,
    # and qcstats is OWNED only when this installer placed it - a board that already has one
    # from its own release gets PREEXISTING and no OWNED entry at all. Both of those are
    # properties of the machine, not of the installer, and a number that moves with the
    # machine fails a correct install. The two optional paths are checked separately below.
    manifest=/var/lib/qcctl/install.manifest
    missing=
    for p in /etc/systemd/system/qcctld.service /etc/systemd/system/qcctl-resume.service \
             /usr/sbin/qcctld /usr/sbin/qcctl /usr/sbin/qcctl-surface-dump \
             /usr/sbin/qcctl-fan-release /usr/sbin/qcctl-uninstall /usr/sbin/qcctl-check \
             /usr/share/doc/qcctl /usr/share/doc/qcctl/USAGE.md \
             /usr/share/man/man8/qcctl.8 /usr/share/man/man8/qcctld.8 \
             /etc/qcctl/fan.config.dist /etc/qcctl/profiles.conf.dist /var/lib/qcctl; do
        grep -qx "OWNED $p" "$manifest" || missing="$missing $p"
    done
    eq "$missing" "" "every path this installer always owns is recorded"
    if [ -d /usr/share/polkit-1/actions ]; then
        grep -q '^OWNED /usr/share/polkit-1/actions/' "$manifest" \
            && ok "and the polkit action, which this image has" \
            || bad "polkit actions exist here but no action is recorded"
    else
        skip_ "no /usr/share/polkit-1/actions on this image, so no action to record"
    fi
    # qcstats either way, but it must be ONE of the two and never neither: the daemon cannot
    # run without it, and a manifest that mentions it nowhere means nobody is accountable
    # for the copy on disk.
    if grep -q '/qcstats$' "$manifest"; then
        ok "qcstats is accounted for ($(awk '/\/qcstats$/{print $1, $2}' "$manifest"))"
    else
        bad "qcstats appears in the manifest neither as OWNED nor as PREEXISTING"
    fi
    # And each recorded path is really there, which a count alone does not say.
    # A manifest line is "<KIND> <path>"; only the OWNED ones are this installer's own.
    missing=0
    while read -r _kind _path; do
        [ "$_kind" = OWNED ] || continue
        [ -e "$_path" ] || { missing=$((missing + 1)); note "recorded but absent: $_path"; }
    done < /var/lib/qcctl/install.manifest
    eq "$missing" 0 "and every recorded path exists"
    eq "$(grep -c '^#RUN' /var/lib/qcctl/install.manifest)" 0 "and the manifest tidied"
}

s_release() {
    head_ "release: the fan is handed back in the state the kernel would hold"
    # What a release DECIDES is ours and deterministic; what the fan then holds is not.
    # Writing mode=enabled is itself one governor evaluation - measured 2026-10-17 - so the
    # kernel may raise the state immediately afterwards, and on this board it does: 0 becomes
    # the map's floor inside the hysteresis band, and the floor becomes floor+1 while hot.
    # Both are the kernel managing the zone again, which is the whole point of handing it
    # back, and this zone has no polling_delay, so it is evaluated on tsens threshold
    # interrupts: it can step up more than once while the board cools. A ceiling on the
    # resulting state would therefore assert step_wise's behaviour, not qcctl's.
    #
    # What IS ours: the decision, read from the journal, and the SHAPE of what is left
    # behind. With a map floor of 4 the governor's own output is 0 or 4..max and never
    # 1..3, so a state in that band can only be a manual write nobody handed back - which
    # is the release defect this section exists for. Case 1 enters it holding state 3.
    #
    # And the decision is checked against the temperature THE DAEMON measured, not one this
    # script read seconds earlier. Pinning a predicted value was a race: at 39.0 C the test
    # expected the floor, the board fell to 35.7 C before the release ran, the release
    # correctly chose 0, and the check reported a defect. The rule is what is being tested -
    # below the trip, off; at or above it, the map's floor - so the rule is what is asserted,
    # evaluated against the numbers in the daemon's own line.

    # 1. A switch away from a target profile while the board is BELOW the trip.
    #
    # The trip is raised for this case rather than the board cooled, for the same reason the
    # watchdog section does it: as shipped the trip is 38.0 C and this board sits at 36-39 C,
    # so which side of it a release lands on is the room's decision. With both cases landing
    # above the trip - which is what happened on 2026-10-21 - the "decides on 0" branch is
    # not exercised on hardware at all, and that is the branch the 2026-10-17 defect was in.
    # The daemon is restarted so it re-reads the trip: the surface is discovered once.
    TRIP_SAVED=$(cat "$ZONEDIR/trip_point_2_temp")
    if printf '%s\n' "$(( TRIP_SAVED + 8000 ))" > "$ZONEDIR/trip_point_2_temp" 2>/dev/null; then
        systemctl restart qcctld
        sleep 3
        note "active trip raised $TRIP_SAVED -> $(cat "$ZONEDIR/trip_point_2_temp") so this case is below it"
    else
        note "cannot raise the trip; this case tests whichever side the room puts us on"
    fi
    profile held40
    load 70; sleep 75; load_stop
    wait_until 60 '[ $(temp) -lt $(trip) ]' || note "board still at $(temp)"
    note "before the switch: $(state)"
    since=$(mark_)
    qcctl -m balanced >/dev/null 2>&1; sleep 6
    check_release_rule "$since" "switching away below the trip"
    eq "$(mode)" enabled "with the zone handed back"
    f=$(fan)
    { [ "$f" -eq 0 ] || [ "$f" -ge 4 ]; } \
        && ok "and state $f is one step_wise itself can hold, not a curve state" \
        || bad "the fan is at $f, in the 1..3 band only a manual write produces"

    # 2. The same switch while HOT: the cooling map's floor, never maximum and never off.
    # The trip goes back to what the board shipped with, so "hot" means hot by its own
    # definition rather than by one this script invented.
    printf '%s\n' "$TRIP_SAVED" > "$ZONEDIR/trip_point_2_temp" 2>/dev/null
    systemctl restart qcctld
    sleep 3
    note "active trip restored to $(cat "$ZONEDIR/trip_point_2_temp")"
    profile held40
    load 170
    wait_until 150 '[ $(temp) -ge $(trip) ] && [ $(fan) -ge 5 ]' \
        || note "the controller only reached state $(fan) at $(temp)"
    note "before the switch: $(state)"
    since=$(mark_)
    qcctl -m balanced >/dev/null 2>&1; sleep 6
    check_release_rule "$since" "switching away while hot"
    [ "$(fan)" -ge 4 ] && ok "and the fan is at the floor or above, as the governor steps it" \
                       || bad "the fan dropped to $(fan), below the floor, while hot"
    load_stop

    # 3. Stopping the unit at idle.
    profile workstation
    wait_until 90 '[ $(temp) -lt $(clearp) ]' || note "board at $(temp)"
    echo 6 > "$CDEVDIR/cur_state" 2>/dev/null || true
    systemctl stop qcctld; sleep 4
    eq "$(fan)" 0 "stopping the daemon at idle leaves the fan off"
    eq "$(mode)" enabled "and the zone enabled"

    # 4. FAILSAFE full is honoured, which is what makes it safe to ship.
    systemctl start qcctld; sleep 3
    profile maxperf
    note "sustained curve engaged: $(state)"
    systemctl stop qcctld; sleep 4
    [ "$(fan)" -ge 9 ] && ok "a FAILSAFE full release leaves the fan at maximum" \
                       || bad "failsafe left state $(fan), not maximum"
    systemctl start qcctld; sleep 3

    # 5. A blind release: the sampler dies, so there is no temperature to judge by.
    profile workstation
    load 60
    wait_until 60 '[ $(fan) -ge 4 ]' || note "curve did not raise the fan"
    before=$(fan)
    pkill -x qcstats
    sleep 6
    case "$(jlog 20)" in *"no temperature, so the fan was not lowered"*)
        ok "a blind release does not lower the fan" ;;
        *) bad "no blind release in the journal" ;; esac
    [ "$(fan)" -ge "$before" ] && ok "and the fan is no lower than it was" \
                               || bad "the fan dropped from $before to $(fan) while blind"
    load_stop
}

s_envelope() {
    head_ "envelope: what this cooler can actually do (a measurement, not a check)"
    systemctl stop qcctld; sleep 2
    echo disabled > "$ZONEDIR/mode"
    for st in 0 4 10; do
        echo "$st" > "$CDEVDIR/cur_state"
        load 140
        max=0; e=0
        while [ "$e" -lt 120 ]; do
            sleep 20; e=$((e + 20))
            v=$(temp); [ "$v" -gt "$max" ] && max=$v
        done
        load_stop
        note "full load at fan state $st: peak ${max}mC"
        sleep 20
    done
    echo 0 > "$CDEVDIR/cur_state"
    sleep 30
    note "idle with the fan off: $(temp)mC"
    echo enabled > "$ZONEDIR/mode"
    systemctl start qcctld
    note "these are the numbers etc/fan.config's header records; re-measure before trusting"
    note "a curve on a different heatsink, fan or enclosure"
}

# ------------------------------------------------------------------ main
[ "${1:-}" = --list ] && { echo "sections: $SECTIONS_ALL"; echo "default:  $SECTIONS_DEFAULT"; exit 0; }
[ "$(id -u)" = 0 ] || { echo "board-check: must run as root - it drives the fan" >&2; exit 2; }
command -v qcctl >/dev/null 2>&1 || { echo "board-check: qcctl is not installed" >&2; exit 2; }
resolve || { echo "board-check: no cpuss-0-0-thermal zone or emc2305_fan here - wrong board?" >&2; exit 2; }

trap 'echo; echo "restoring the board..."; restore' EXIT INT TERM

want=${*:-$SECTIONS_DEFAULT}
started=$(date -u +%H:%M:%S)
echo "board-check on $(uname -n), kernel $(uname -r), started $started"
echo "sections: $want"

for sec in $want; do
    case " $SECTIONS_ALL " in
        *" $sec "*) "s_$sec" ;;
        *) echo "board-check: no such section '$sec'" >&2; exit 2 ;;
    esac
done

echo
if [ "$FAILS" = 0 ]; then
    if [ "$SKIPS" -gt 0 ]; then
        echo "board-check: $CHECKS checks ok, $SKIPS SECTION(S) SKIPPED - not a pass of"
        echo "             those sections; see the SKIP line(s) above  ($started -> $(date -u +%H:%M:%S))"
    else
        echo "board-check: $CHECKS checks ok  ($started -> $(date -u +%H:%M:%S))"
    fi
else
    echo "board-check: $CHECKS checks, $FAILS FAILED${SKIPS:+, $SKIPS skipped}  ($started -> $(date -u +%H:%M:%S))"
fi
[ "$FAILS" = 0 ]
