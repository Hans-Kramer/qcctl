#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer
#
# Remove qcctl from a board. Manifest-driven: install.sh records what it created in
# /var/lib/qcctl/install.manifest, one line per path:
#
#     OWNED       <path>     created by the installer   -> removed
#     CONFIG      <path>     config we created          -> removed only with --purge
#     PREEXISTING <path>     already on the board       -> left alone
#
# PREEXISTING matters most for qcstats: a hard dependency from another project, so it is
# removed only if this installer put it there.
#
# Nothing is removed until the board is verified back to stock: the fan's zone and every
# value `qcctl --reset` restores are read back and compared against the snapshot.
#
# Usage: uninstall.sh [--dry-run] [--purge] [--force]
#   --dry-run  print what would happen, change nothing
#   --purge    also remove /etc/qcctl and the captured fan baseline. Both are kept by
#              default: one is the user's config, the other is the user's measurements
#   --force    proceed past a refusal: use the built-in path list when no manifest
#              exists, and remove the files even when the board is not back to stock
#
# Exit: 0 removed   1 incomplete, or unsafe to continue   2 bad argument
#       3 no manifest and no --force   4 the board is not back to stock
#
# Installed as /usr/sbin/qcctl-uninstall, and removes itself while running.
set -u

DRY=0; PURGE=0; FORCE=0
for a in "$@"; do
    case "$a" in
        --dry-run) DRY=1 ;;
        --purge)   PURGE=1 ;;
        --force)   FORCE=1 ;;
        # --help prints the header block above; this range tracks it.
        -h|--help) sed -n '5,28p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "uninstall.sh: unknown argument '$a'" >&2; exit 2 ;;
    esac
done

MANIFEST=/var/lib/qcctl/install.manifest
STATEF=/var/lib/qcctl/boot-state   # what --reset restores from; step 5 removes it
FANBASE=/var/lib/qcctl/fan-baseline  # measured on this board; kept unless --purge
VENDOR=/etc/sysfs.d/50-dragonwing-defaults.conf
FANZONE=cpuss-0-0-thermal

# Resolve the fan's zone BY NAME, never by index - same rule the rest of the project
# follows. Falls back to empty, which the step-2 check reports rather than ignores.
ZONE=
for z in /sys/class/thermal/thermal_zone*; do
    [ -r "$z/type" ] || continue
    [ "$(cat "$z/type" 2>/dev/null)" = "$FANZONE" ] && { ZONE="$z/mode"; break; }
done

# Every path install.sh records OWNED. Used only with --force, when no manifest survives,
# so anything missing here is a file --force leaves behind for good. The user's config is
# deliberately absent - the /etc/qcctl block below keeps or purges that - but the .dist
# reference copies beside it are ours, and are removed with or without --purge.
# CI asserts this list against install.sh; see .github/workflows/gate.yml.
KNOWN="/etc/systemd/system/qcctld.service
/etc/systemd/system/qcctl-resume.service
/usr/sbin/qcctld
/usr/sbin/qcctl
/usr/sbin/qcctl-surface-dump
/usr/sbin/qcctl-fan-release
/usr/sbin/qcctl-uninstall
/usr/sbin/qcctl-check
/usr/bin/qcstats
/usr/share/polkit-1/actions/io.github.hans_kramer.qcctl.policy
/usr/share/doc/qcctl/USAGE.md
/usr/share/doc/qcctl
/etc/qcctl/fan.config.dist
/etc/qcctl/profiles.conf.dist
/var/lib/qcctl"

run() { if [ "$DRY" = 1 ]; then echo "    would: $*"; else "$@" >/dev/null 2>&1 || true; fi; }
# All narration goes to ONE stream. stderr and stdout flush independently (markedly so
# over ssh), and a tool reporting what it deleted must never present those events out of
# order. Failures are marked, and the exit status carries the machine-readable verdict.
note() { echo "  $*"; }
warn() { echo "  !! $*"; }
die()  { echo "  ** $*"; exit "${2:-1}"; }

[ "$(id -u)" = 0 ] || { echo "uninstall.sh: must run as root" >&2; exit 1; }
[ "$DRY" = 1 ] && echo "== DRY RUN - nothing will be changed =="

# ---------------------------------------------------------------- 1. stop the daemon
# Stopping first is what lets the unit's ExecStopPost hand the fan back. Everything
# else can wait; an unmanaged fan cannot.
note "1. stopping the service"
if systemctl list-unit-files qcctld.service >/dev/null 2>&1 &&
   systemctl is-active --quiet qcctld.service 2>/dev/null; then
    run systemctl stop qcctld.service
else
    note "   not running"
fi
# Anything left outside systemd (a hand-launched daemon from testing). The unit's own
# pid comes from systemd rather than being assumed dead: a dry run does not really stop
# the service, so without this it reported the unit's daemon as a stray.
MAINPID=$(systemctl show -p MainPID --value qcctld.service 2>/dev/null)
stray=
for p in $(pgrep -x qcctld 2>/dev/null); do
    [ "$p" = "${MAINPID:-0}" ] || stray="$stray $p"
done
if [ -n "$stray" ]; then
    note "   a qcctld outside systemd is still alive (pid$stray) - terminating"
    run kill -TERM $stray
    i=0; while [ "$DRY" = 0 ] && pgrep -x qcctld >/dev/null 2>&1 && [ "$i" -lt 6 ]; do
        sleep 1; i=$((i+1)); done
    run kill -KILL $stray
fi

# ------------------------------------------------------- 2. hand the fan back, verify
# Independent of the unit's ExecStopPost. A SIGKILL cannot run a handler, so this must
# not be the only defence - and it must be VERIFIED, not assumed.
note "2. returning the fan's thermal zone to the kernel"
if [ -z "$ZONE" ]; then
    warn "no thermal zone named '$FANZONE' on this board - cannot verify fan ownership."
    warn "If a daemon was running, release the fan by hand before trusting this result."
elif [ "$DRY" = 1 ]; then
    echo "    would: echo enabled > $ZONE   (resolved by name)"
else
    note "   zone resolved by name: ${ZONE%/mode} ($FANZONE)"
    echo enabled > "$ZONE" 2>/dev/null || true
fi
if [ "$DRY" = 0 ] && [ -n "$ZONE" ]; then
    m=$(cat "$ZONE" 2>/dev/null || echo "unreadable")
    if [ "$m" = enabled ]; then note "   verified: mode=enabled"
    else warn "FAILED: zone mode is '$m', not 'enabled'. The fan is NOT under kernel"
         die  "control. Fix this before anything else: echo enabled > $ZONE"
    fi
fi

# --------------------------------------------- 3. reset the control surface to stock
# Must run while the binary and the snapshot both exist - step 5 removes them. Restores
# the boot defaults, vendor policy2 bug included. Not through run(), which swallows
# errors: a failed reset here must not become "REMOVED cleanly".
note "3. resetting clocks and governors to stock"
RESET_OK=0
RESET_NOTE="not attempted"
KEEP_SNAPSHOT=0

# refuse <message> - stop before anything is removed, unless the operator insisted.
refuse() {
    if [ "$FORCE" = 1 ]; then
        warn "--force given: removing the files anyway, with the board NOT back to stock."
        return 0
    fi
    warn "Nothing has been removed: /usr/sbin/qcctl and $STATEF are both still here,"
    warn "so this is fixable. Deal with the above and re-run, or pass --force to remove"
    warn "the files anyway and leave the board modified."
    die "$1" 4
}

if [ ! -x /usr/sbin/qcctl ]; then
    if [ -f "$STATEF" ]; then
        warn "/usr/sbin/qcctl is gone but $STATEF is not, so the control surface WAS"
        warn "changed and nothing left on this board can revert it. Replay it by hand:"
        warn "  grep -v '^#' $STATEF | grep -v cooling_device |"
        warn "    while IFS=\"\$(printf '\\t')\" read -r p v; do echo \"\$v\" > \"\$p\"; done"
        warn "(cooling_device lines are skipped, as --reset skips them: a fan state is"
        warn "the governor's output, and replaying one can leave the board uncooled.)"
        if [ "$PURGE" = 1 ]; then
            warn "--purge REMOVES that snapshot with everything else. Copy it elsewhere"
            warn "first if you intend to replay it."
        else
            KEEP_SNAPSHOT=1
            warn "The snapshot is kept for that purpose: it lives under an OWNED directory,"
            warn "so this run puts it back after removing the rest."
        fi
        RESET_NOTE="qcctl binary absent, snapshot left unreplayed"
    else
        note "   /usr/sbin/qcctl absent and no snapshot - nothing to reset with"
        RESET_NOTE="nothing was ever recorded to reset"
    fi
elif [ ! -f "$STATEF" ]; then
    # An install that died before step 9 leaves this, and it must not block removal.
    note "   no snapshot at $STATEF - nothing to restore, nothing to verify against"
    RESET_NOTE="no boot-state snapshot existed"
elif [ "$DRY" = 1 ]; then
    echo "    would: /usr/sbin/qcctl --reset"
    echo "    would: read back and compare $(grep -v '^#' "$STATEF" 2>/dev/null |
                                              grep -vc cooling_device) snapshot values"
else
    if RESET_OUT=$(/usr/sbin/qcctl --reset 2>&1); then
        note "   $RESET_OUT"
        RESET_OK=1
    else
        rc=$?
        warn "qcctl --reset FAILED (exit $rc):"
        printf '%s\n' "$RESET_OUT" | sed 's/^/     /'
        RESET_NOTE="qcctl --reset failed"
        refuse "the control surface is still modified and --reset could not undo it"
    fi
fi

# --reset reports what it wrote; this reports what the board now reads, which differs
# whenever the kernel accepts a write and clamps it. One awk: a shell loop is 312 forks.
if [ "$RESET_OK" = 1 ]; then
    VOUT=$(awk -F'\t' '
        /^#/   { next }
        NF < 2 { next }
        # The substrings is_governor_output() tests, so this cannot skip a different set.
        index($1, "/cooling_device") && index($1, "/cur_state") { next }
        {
            if ((getline got < $1) <= 0) { close($1); un++; next }
            close($1)
            sub(/[ \t\r]+$/, "", got)
            if (got == $2) { ok++ }
            else { bad++; if (bad <= 10) printf "MISMATCH %s now=\"%s\" boot=\"%s\"\n", $1, got, $2 }
        }
        END { printf "SUMMARY %d %d %d\n", ok + 0, bad + 0, un + 0 }
    ' "$STATEF" 2>/dev/null)
    printf '%s\n' "$VOUT" | sed -n 's/^MISMATCH /  !! /p'
    SUM=$(printf '%s\n' "$VOUT" | sed -n 's/^SUMMARY //p')
    v_ok=${SUM%% *}; v_rest=${SUM#* }; v_bad=${v_rest%% *}; v_un=${v_rest#* }
    v_ok=${v_ok:-0}; v_bad=${v_bad:-0}; v_un=${v_un:-0}
    [ "$v_bad" -gt 10 ] && warn "($((v_bad - 10)) further mismatch(es) not listed)"
    if [ "$v_ok" = 0 ]; then
        warn "read back NOTHING from $STATEF - this verified nothing, so treat the"
        warn "reset as unproven rather than as passed."
        RESET_OK=0; RESET_NOTE="read-back verified nothing"
        refuse "the reset could not be verified"
    elif [ "$v_bad" != 0 ]; then
        warn "$v_bad of $((v_ok + v_bad)) value(s) do NOT match $STATEF after --reset."
        warn "--reset reported success, so these are writes the kernel accepted and then"
        warn "did not honour. The board is not back to stock."
        RESET_OK=0; RESET_NOTE="$v_bad value(s) did not match the snapshot"
        refuse "the board does not match its boot state"
    else
        if [ "$v_un" != 0 ]; then
            note "   verified: $v_ok value(s) match the snapshot ($v_un no longer readable)"
        else
            note "   verified: all $v_ok restored value(s) match the snapshot"
        fi
    fi
fi

# ---------------------------------------------------------------- 4. disable the units
note "4. disabling and removing the units"
run systemctl disable qcctld.service
run systemctl disable qcctl-resume.service

# ------------------------------------------------------------------- 5. remove files
note "5. removing installed files"
removed=none; kept=none; cfg=none
# The snapshot step 3 told the operator to replay lives inside an OWNED directory, so the
# removal below would delete the file that message points at. Stash and restore it, the
# same way the manifest is stashed below.
SNAPSTATE=
if [ "$KEEP_SNAPSHOT" = 1 ] && [ "$DRY" = 0 ] && [ -f "$STATEF" ]; then
    SNAPSTATE=$(mktemp) && cat "$STATEF" > "$SNAPSTATE"
fi
# The fan baseline is measured on THIS board in THIS enclosure and costs a fan-driving
# capture to replace, so it outlives a plain uninstall the way the user's config does.
BASESTATE=
if [ -f "$FANBASE" ]; then
    if [ "$PURGE" = 1 ]; then
        note "   purging $FANBASE - a reinstall will need a fresh capture"
    else
        note "   keeping $FANBASE (--purge removes it)"
        [ "$DRY" = 0 ] && { BASESTATE=$(mktemp) && cat "$FANBASE" > "$BASESTATE"; }
    fi
fi
# Recorded BEFORE the manifest is deleted: it lives under an OWNED path, so testing for
# it afterwards would fire the fallback below and purge paths it said to leave alone.
USED_MANIFEST=0
if [ -f "$MANIFEST" ]; then
    USED_MANIFEST=1
    note "   using manifest $MANIFEST"
    # Snapshot first: the manifest lives inside /var/lib/qcctl, which is itself OWNED.
    SNAP=$(mktemp) || die "cannot create a temp file"
    cat "$MANIFEST" > "$SNAP"
    removed=$(grep -c '^OWNED' "$SNAP" 2>/dev/null); removed=${removed:-0}
    kept=$(grep -c '^PREEXISTING' "$SNAP" 2>/dev/null); kept=${kept:-0}
    # Deepest paths first so directories are empty when we reach them.
    awk '$1 == "OWNED" { print length($2), $2 }' "$SNAP" | sort -rn | cut -d' ' -f2- |
    while IFS= read -r p; do
        [ -n "$p" ] || continue
        if [ -e "$p" ] || [ -L "$p" ]; then note "   remove $p"; run rm -rf "$p"
        else note "   already gone $p"; fi
    done
    awk '$1 == "PREEXISTING" { print "   KEEPING (not ours): " $2 }' "$SNAP"
    # Config last, and only when asked. An edited profiles.conf is the user's work.
    if [ "$PURGE" = 1 ]; then
        awk '$1 == "CONFIG" { print length($2), $2 }' "$SNAP" | sort -rn | cut -d' ' -f2- |
        while IFS= read -r p; do
            [ -n "$p" ] || continue
            if [ -e "$p" ]; then note "   purge $p"; run rm -rf "$p"; fi
        done
    else
        awk '$1 == "CONFIG" { print "   KEEPING (your config, --purge removes it): " $2 }' "$SNAP"
    fi
    cfg=$(grep -c '^CONFIG' "$SNAP" 2>/dev/null); cfg=${cfg:-0}
    # Kept config needs a kept record: without it a later install tags /etc/qcctl
    # PREEXISTING and a future --purge would refuse to remove it.
    if [ "$PURGE" = 0 ] && [ "$cfg" -gt 0 ] && [ "$DRY" = 0 ]; then
        mkdir -p "$(dirname "$MANIFEST")" 2>/dev/null
        grep '^CONFIG' "$SNAP" > "$MANIFEST" 2>/dev/null && chmod 0644 "$MANIFEST"
        note "   kept a $cfg-line config record at $MANIFEST so a reinstall still knows"
        note "   these files are ours (--purge removes config and this record together)"
    fi
    rm -f "$SNAP"
elif [ "$FORCE" = 1 ]; then
    warn "no manifest. Using the built-in path list, which CANNOT tell whether"
    warn "qcstats pre-existed on this board. Verify that yourself before trusting this."
    removed=unknown; kept=unknown; cfg=unknown
    echo "$KNOWN" | while IFS= read -r p; do
        [ -n "$p" ] || continue
        if [ -e "$p" ] || [ -L "$p" ]; then note "   remove $p"; run rm -rf "$p"; fi
    done
elif [ "$DRY" = 1 ]; then
    note "   no manifest at $MANIFEST"
    note "   a real run would stop here and remove nothing without --force"
else
    warn "no manifest at $MANIFEST and --force not given."
    warn "No files were removed. Steps 1-4 DID run: the service is stopped, the fan's"
    warn "zone is back under kernel control, and the control surface is reset. Only file"
    warn "removal was skipped. Re-run with --force to use the built-in path list."
    exit 3
fi

# Fallback for the --force path ONLY, which has no tags to go on. Never when a manifest
# governed the run - its tags are authoritative and must not be second-guessed here.
if [ "$USED_MANIFEST" = 0 ] && [ -d /etc/qcctl ]; then
    if [ "$PURGE" = 1 ]; then note "   purging /etc/qcctl"; run rm -rf /etc/qcctl
    else note "   KEEPING /etc/qcctl (your config; --purge removes it)"; fi
fi
run rm -rf /run/qcctl
if [ -n "$SNAPSTATE" ]; then
    mkdir -p "$(dirname "$STATEF")" 2>/dev/null
    if cat "$SNAPSTATE" > "$STATEF" 2>/dev/null; then
        chmod 0644 "$STATEF"
        note "   kept $STATEF - the only record of how to revert this board by hand"
    else
        warn "could not keep $STATEF; the board's original values are now unrecorded"
    fi
    rm -f "$SNAPSTATE"
fi
if [ -n "$BASESTATE" ]; then
    mkdir -p "$(dirname "$FANBASE")" 2>/dev/null
    if cat "$BASESTATE" > "$FANBASE" 2>/dev/null; then
        chmod 0644 "$FANBASE"
        note "   kept $FANBASE - an uninstall cannot re-measure it"
    else
        warn "could not keep $FANBASE; a reinstall will need a fresh capture"
    fi
    rm -f "$BASESTATE"
fi

# ----------------------------------------------------------------- 6. tell systemd
note "6. reloading systemd"
run systemctl daemon-reload
run systemctl reset-failed qcctld.service qcctl-resume.service

# ------------------------------------------------------------------- 7. verify
note "7. verifying removal"
if [ "$DRY" = 1 ]; then echo "  == DRY RUN complete - nothing was changed =="; exit 0; fi

left=0
for p in /usr/sbin/qcctld /usr/sbin/qcctl /etc/systemd/system/qcctld.service \
         /etc/systemd/system/qcctl-resume.service /run/qcctl; do
    if [ -e "$p" ] || [ -L "$p" ]; then warn "STILL PRESENT: $p"; left=$((left+1)); fi
done
# /var/lib/qcctl survives when it holds something this run is not entitled to remove. Each
# file is judged on its own: a chain of reasons would report the first and miss the rest.
if [ -e /var/lib/qcctl ]; then
    # Own names: `kept` and `stray` are both live elsewhere in this script, and the
    # closing verdict reads `kept`.
    vl_kept= ; vl_stray=
    for vl_f in /var/lib/qcctl/* /var/lib/qcctl/.*; do
        case "$vl_f" in
            /var/lib/qcctl/. | /var/lib/qcctl/..) continue ;;
            '/var/lib/qcctl/*' | '/var/lib/qcctl/.*') continue ;;
        esac
        [ -e "$vl_f" ] || continue
        case "$vl_f" in
            "$STATEF")
                if [ "$KEEP_SNAPSHOT" = 1 ]; then vl_kept="$vl_kept the boot-state snapshot,"
                else vl_stray="$vl_stray $vl_f"; fi ;;
            "$FANBASE")
                if [ "$PURGE" = 0 ]; then vl_kept="$vl_kept the fan baseline,"
                else vl_stray="$vl_stray $vl_f"; fi ;;
            "$MANIFEST")
                if grep -qv '^CONFIG' "$MANIFEST" 2>/dev/null; then vl_stray="$vl_stray $vl_f"
                else vl_kept="$vl_kept the config record,"; fi ;;
            *) vl_stray="$vl_stray $vl_f" ;;
        esac
    done
    if [ -n "$vl_stray" ]; then
        warn "STILL PRESENT in /var/lib/qcctl:$vl_stray"
        left=$((left+1))
    elif [ -n "$vl_kept" ]; then
        note "   /var/lib/qcctl kept, holding${vl_kept%,}"
    else
        note "   /var/lib/qcctl kept but empty"
    fi
fi
pgrep -x qcctld >/dev/null 2>&1 && { warn "STILL RUNNING: qcctld"; left=$((left+1)); }
u=$(systemctl list-unit-files 2>/dev/null | grep -c qcctl || true)
[ "${u:-0}" = 0 ] || { warn "systemd still knows $u qcctl unit(s)"; left=$((left+1)); }

# The vendor file is superseded by unit ORDERING, never edited. If it changed, something
# did more than it should have.
if [ -f "$VENDOR" ]; then
    if grep -q "policy7/scaling_governor" "$VENDOR"; then
        note "   vendor file intact (still names the non-existent policy7 - its own bug)"
    else
        warn "$VENDOR no longer names policy7. We never edit this file; check what did."
    fi
fi
cd_state=
for c in /sys/class/thermal/cooling_device*; do
    [ "$(cat "$c/type" 2>/dev/null)" = emc2305_fan ] && cd_state=$(cat "$c/cur_state" 2>/dev/null)
done
note "   zone mode: $(cat "$ZONE" 2>/dev/null)   fan state: ${cd_state:-unknown}"

echo ""
if [ "$left" = 0 ]; then
    echo "  REMOVED cleanly. owned=$removed config=$cfg preexisting=$kept"
    # Removed and reverted are two claims, not one.
    if [ "$RESET_OK" = 1 ]; then
        echo "  The board is back to stock and was read back to prove it, vendor policy2"
        echo "  bug included - that is correct."
    else
        echo "  NOT verified back to stock: $RESET_NOTE."
        echo "  The files are gone; the control surface may still be modified."
    fi
else
    echo "  INCOMPLETE: $left item(s) remain. See above."
    exit 1
fi
