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
# Usage: uninstall.sh [--dry-run] [--purge] [--force]
#   --dry-run  print what would happen, change nothing
#   --purge    also remove /etc/qcctl (user-edited config is kept by default)
#   --force    proceed with the built-in path list when no manifest exists
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
        -h|--help) sed -n '5,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "uninstall.sh: unknown argument '$a'" >&2; exit 2 ;;
    esac
done

MANIFEST=/var/lib/qcctl/install.manifest
VENDOR=/etc/sysfs.d/50-dragonwing-defaults.conf
FANZONE=cpuss-0-0-thermal

# Resolve the fan's zone BY NAME, never by index - same rule the rest of the project
# follows. Falls back to empty, which the step-2 check reports rather than ignores.
ZONE=
for z in /sys/class/thermal/thermal_zone*; do
    [ -r "$z/type" ] || continue
    [ "$(cat "$z/type" 2>/dev/null)" = "$FANZONE" ] && { ZONE="$z/mode"; break; }
done

# Paths the installer can create. Used only with --force, when no manifest survives.
KNOWN="/etc/systemd/system/qcctld.service
/etc/systemd/system/qcctl-resume.service
/usr/sbin/qcctld
/usr/sbin/qcctl
/usr/sbin/qcctl-fan-release
/usr/sbin/qcctl-uninstall
/usr/sbin/qcctl-check
/usr/bin/qcstats
/usr/share/polkit-1/actions/io.github.hans_kramer.qcctl.policy
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
# Anything left outside systemd (a hand-launched daemon from testing).
if pgrep -x qcctld >/dev/null 2>&1; then
    note "   a qcctld outside systemd is still alive - terminating"
    run pkill -TERM -x qcctld
    i=0; while [ "$DRY" = 0 ] && pgrep -x qcctld >/dev/null 2>&1 && [ "$i" -lt 6 ]; do
        sleep 1; i=$((i+1)); done
    run pkill -KILL -x qcctld
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
# Must happen while the binary still exists. After this the board is back to its boot
# defaults - including the vendor's policy2 bug, which is the correct stock state.
note "3. resetting clocks and governors to stock"
if [ -x /usr/sbin/qcctl ]; then
    run /usr/sbin/qcctl --reset
else
    note "   /usr/sbin/qcctl absent - nothing to reset with"
fi

# ---------------------------------------------------------------- 4. disable the units
note "4. disabling and removing the units"
run systemctl disable qcctld.service
run systemctl disable qcctl-resume.service

# ------------------------------------------------------------------- 5. remove files
note "5. removing installed files"
removed=none; kept=none; cfg=none
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
# /var/lib/qcctl legitimately survives when it holds the kept-config record.
if [ -e /var/lib/qcctl ]; then
    if [ -f "$MANIFEST" ] && ! grep -qv '^CONFIG' "$MANIFEST" 2>/dev/null; then
        note "   /var/lib/qcctl kept, holding only the config record"
    else
        warn "STILL PRESENT: /var/lib/qcctl (and it is not just a config record)"
        left=$((left+1))
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
    echo "  The board is back to stock, vendor policy2 bug included - that is correct."
else
    echo "  INCOMPLETE: $left item(s) remain. See above."
    exit 1
fi
