#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer
#
# Install qcctl on an Arduino Ventuno Q. Run as root from a staging directory
# holding the cross-built binaries, the configs and the units.
#
# /var/lib/qcctl/install.manifest is appended to as each path is created, so an install
# that dies halfway is still fully removable. Paths that already existed are recorded
# PREEXISTING and removal leaves them alone. An upgrade APPENDS to what earlier installs
# recorded rather than replacing it, and the file is de-duplicated once the install has
# succeeded - so there is no moment at which a path on disk is unrecorded.
#
# Usage: install.sh [--dry-run] [--upgrade]
set -u

DRY=0; UPGRADE=0
for a in "$@"; do
    case "$a" in
        --dry-run) DRY=1 ;;
        --upgrade) UPGRADE=1 ;;
        -h|--help) sed -n '5,14p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "install.sh: unknown argument '$a'" >&2; exit 2 ;;
    esac
done

SRC=$(cd "$(dirname "$0")" && pwd)
STATE=/var/lib/qcctl
MANIFEST=$STATE/install.manifest
ETC=/etc/qcctl
UNITDIR=/etc/systemd/system
FANZONE=cpuss-0-0-thermal
# Where this script LOOKS, as opposed to where it installs. QCCTL_SYSROOT relocates the
# former onto a captured tree - the same hook the binaries and fan-release.sh honour - so the
# file bookkeeping can be exercised off a board. Destinations stay absolute: an install that
# could be redirected is a different contract, and not one this project offers.
SYSFS="${QCCTL_SYSROOT:-}/sys"

note() { echo "  $*"; }
warn() { echo "  !! $*"; }
die()  { echo "  ** $*"; echo "  ** Run scripts/uninstall.sh to undo whatever was done."; exit 1; }
run()  { if [ "$DRY" = 1 ]; then echo "    would: $*"; else "$@" || die "failed: $*"; fi; }

# record <OWNED|PREEXISTING> <path>   - append immediately, before/at creation time
record() {
    [ "$DRY" = 1 ] && { echo "    would record: $1 $2"; return 0; }
    printf '%s %s\n' "$1" "$2" >> "$MANIFEST"
}

# ours <path> - was this path recorded as ours BEFORE this run started?
# "It exists" and "it is not ours" are different facts, and only the second is PREEXISTING.
# Everything above the last #RUN marker is what earlier installs recorded, which is why
# this needs no copy of the file: the marker is the copy.
ours() {
    [ -f "$MANIFEST" ] || return 1
    _mark=$(grep -n '^#RUN' "$MANIFEST" 2>/dev/null | tail -1 | cut -d: -f1)
    if [ -n "${_mark:-}" ]; then
        head -n "$((_mark - 1))" "$MANIFEST"
    else
        cat "$MANIFEST"
    fi | awk -v p="$1" '($1 == "OWNED" || $1 == "CONFIG") && $2 == p { found = 1 }
                        END { exit !found }'
}

# put <mode> <src> <dst> [tag]   - install a file, recording ownership honestly.
# tag defaults to OWNED. CONFIG files survive an uninstall unless --purge is given.
put() {
    _m=$1; _s=$2; _d=$3; _t=${4:-OWNED}
    [ -f "$_s" ] || die "staging file missing: $_s"
    if [ -e "$_d" ] && ours "$_d"; then
        record "$_t" "$_d"
        if [ "$_t" = CONFIG ]; then
            note "   keeping your $_d (ours, but config - not overwritten)"
        else
            run install -o root -g root -m "$_m" "$_s" "$_d"
            [ "$DRY" = 0 ] && note "   upgraded $_d ($_m)"
        fi
    elif [ -e "$_d" ]; then
        record PREEXISTING "$_d"
        note "   exists and is NOT ours, leaving alone: $_d"
    else
        record "$_t" "$_d"
        run install -o root -g root -m "$_m" "$_s" "$_d"
        [ "$DRY" = 0 ] && note "   installed $_d ($_m)"
    fi
}

[ "$(id -u)" = 0 ] || { echo "install.sh: must run as root" >&2; exit 1; }
[ "$DRY" = 1 ] && echo "== DRY RUN - nothing will be changed =="

# -------------------------------------------------------------------- pre-flight
note "1. pre-flight"
command -v systemctl >/dev/null 2>&1 || die "systemd not present"
for f in qcctld qcctl qcstats qcctl-surface-dump profiles.conf fan.config \
         qcctld.service qcctl-resume.service fan-release.sh uninstall.sh \
         io.github.hans_kramer.qcctl.policy USAGE.md qcctl.8 qcctld.8; do
    [ -f "$SRC/$f" ] || die "staging incomplete: $SRC/$f is missing"
done
# The binaries must be built for THIS machine. A silently x86 binary installs cleanly and
# then fails at boot, which is the worst place to find out - and it has happened: a staging
# directory left behind by a failed `make stage` put host binaries on the board.
#
# ELF magic cannot detect that: 7f454c46 is identical on x86-64 and aarch64. e_machine can,
# and /bin/sh is this machine's architecture by definition, so it is the reference. Same
# test `make stage` applies before deploying; repeated here because a hand-assembled
# staging directory never goes through the Makefile, and that is the path that failed.
#
# One byte at offset 18 is enough for the two architectures in play (aarch64 0xb7, x86-64
# 0x3e) and matches what the Makefile reads, so the two checks cannot disagree.
host_em=$(od -An -tx1 -j18 -N1 /bin/sh | tr -d ' \n')
[ -n "$host_em" ] || die "cannot read e_machine from /bin/sh - refusing to guess"
for b in qcctld qcctl qcstats qcctl-surface-dump qcctl-check; do
    # qcctl-check is optional in a staging directory; the rest are checked above.
    [ -f "$SRC/$b" ] || continue
    case "$(head -c 20 "$SRC/$b" | od -An -tx1 | tr -d ' \n')" in
        7f454c46*) ;; *) die "$b is not an ELF binary" ;;
    esac
    em=$(od -An -tx1 -j18 -N1 "$SRC/$b" | tr -d ' \n')
    if [ "$em" != "$host_em" ]; then
        warn "$b is e_machine=0x$em; this machine is 0x$host_em"
        warn "that is a binary for another architecture - it would install and then not run"
        die "wrong-architecture binary in the staging directory. Rebuild with 'make stage'."
    fi
done
# Execution, now that the architecture is known good: --help exits 0 on all five, so a
# failure here is a real one (a missing shared library, a truncated copy) rather than the
# unconditional success the previous `|| true` chain always reported. qcctl-check was
# excluded from this because it exited 2 for --help; it answers properly now, so the one
# binary this install trusts to validate the config is tested like the rest of them.
for b in qcctld qcctl qcstats qcctl-surface-dump qcctl-check; do
    # Only qcctl-check may legitimately be absent: step 1 requires the others outright, and
    # put() installs this one only when it was staged.
    [ -f "$SRC/$b" ] || continue
    "$SRC/$b" --help >/dev/null 2>&1 || die "$b is the right architecture but will not run"
done
note "   binaries: all e_machine=0x$host_em and executable here"
arch=$(uname -m)
[ "$arch" = aarch64 ] || warn "this board reports $arch, not aarch64 - unexpected for this board"
# Discovery by name: if the fan's zone is not here, this is not the board we built for.
zfound=0
for z in "$SYSFS"/class/thermal/thermal_zone*; do
    [ "$(cat "$z/type" 2>/dev/null)" = "$FANZONE" ] && { zfound=1; note "   fan zone: ${z##*/} ($FANZONE)"; break; }
done
[ "$zfound" = 1 ] || die "no thermal zone named '$FANZONE' - wrong board?"
if [ -e "$UNITDIR/qcctld.service" ] && [ "$UPGRADE" = 0 ]; then
    die "already installed. Use --upgrade, or uninstall first."
fi
note "   ok"

# --------------------------------------------------- state dir and the manifest
note "2. state directory and manifest"
[ -f "$MANIFEST" ] && note "   previous manifest: $(grep -c '^[A-Z]' "$MANIFEST") entries"
if [ -d "$STATE" ]; then note "   exists: $STATE"
elif [ "$DRY" = 1 ]; then note "   would create $STATE"
else
    install -o root -g root -m 0755 -d "$STATE" || die "cannot create $STATE"
    note "   created $STATE"
fi
# Emptying this file was the one moment where a path on disk had nothing claiming it: the
# previous install's entries were gone and this run had not re-recorded them yet, so an
# install that died in between left files no uninstall would ever find. The old entries
# stay, this run's are appended below a marker, and step 9 de-duplicates once it is safe.
if [ "$DRY" = 0 ]; then
    [ -f "$MANIFEST" ] || : > "$MANIFEST"
    chmod 0644 "$MANIFEST"
    printf '#RUN %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || echo unknown)" \
        >> "$MANIFEST"
fi
record OWNED "$STATE"

# --------------------------------------------------------------------- binaries
note "3. binaries"
put 0755 "$SRC/qcctld"   /usr/sbin/qcctld
put 0755 "$SRC/qcctl"    /usr/sbin/qcctl
put 0755 "$SRC/qcctl-surface-dump" /usr/sbin/qcctl-surface-dump
put 0755 "$SRC/fan-release.sh" /usr/sbin/qcctl-fan-release
# Install the uninstaller itself, so removal does not depend on the staging directory
# surviving in /tmp.
put 0755 "$SRC/uninstall.sh" /usr/sbin/qcctl-uninstall
[ -f "$SRC/qcctl-check" ] && put 0755 "$SRC/qcctl-check" /usr/sbin/qcctl-check
# qcstats is a hard dependency from a separate project, invoked as a bare 'qcstats' on
# root's PATH. Consults ours() so an --upgrade refreshes one we installed rather than
# demoting it to PREEXISTING.
if existing=$(command -v qcstats 2>/dev/null) && [ -n "$existing" ]; then
    if ours "$existing"; then
        record OWNED "$existing"
        run install -o root -g root -m 0755 "$SRC/qcstats" "$existing"
        [ "$DRY" = 0 ] && note "   upgraded $existing (ours)"
    else
        record PREEXISTING "$existing"
        note "   qcstats already present at $existing and NOT ours - left alone"
    fi
else
    put 0755 "$SRC/qcstats" /usr/bin/qcstats
fi

# ----------------------------------------------------------------------- config
note "4. config"
if [ -d "$ETC" ] && ours "$ETC"; then record CONFIG "$ETC"; note "   exists and is ours: $ETC"
elif [ -d "$ETC" ]; then record PREEXISTING "$ETC"; note "   exists and is NOT ours: $ETC"
elif [ "$DRY" = 1 ]; then record CONFIG "$ETC"; note "   would create $ETC"
else
    install -o root -g root -m 0755 -d "$ETC" || die "cannot create $ETC"
    record CONFIG "$ETC"; note "   created $ETC"
fi
put 0644 "$SRC/profiles.conf" "$ETC/profiles.conf" CONFIG
put 0644 "$SRC/fan.config"    "$ETC/fan.config"    CONFIG
# The REFERENCE copies, tagged OWNED so every upgrade refreshes them. This is the whole fix
# for a real defect: the config files above are CONFIG-tagged and correctly never
# overwritten, which means a block we add in a new release is INVISIBLE to anyone who
# already installed. The < AIRFLOW > block had to be appended by hand to test it.
#
# Shipping .dist alongside puts the diff in the user's hands - the way dpkg leaves
# .dpkg-dist - instead of either editing a file they may have tuned or saying nothing.
put 0644 "$SRC/fan.config"    "$ETC/fan.config.dist"
put 0644 "$SRC/profiles.conf" "$ETC/profiles.conf.dist"

# ------------------------------------------------------------------------ units
note "5. units"
put 0644 "$SRC/qcctld.service"       "$UNITDIR/qcctld.service"
put 0644 "$SRC/qcctl-resume.service" "$UNITDIR/qcctl-resume.service"
run systemctl daemon-reload

# polkit action: lets an unprivileged GUI run `pkexec qcctl -m <name>`. Not required
# Both units point Documentation= here, so installing it makes `systemctl help qcctld` work.
note "5b. documentation"
run install -d -o root -g root -m 0755 /usr/share/doc/qcctl
# Record the directory too, or an uninstall leaves an empty one behind. The uninstaller
# removes OWNED paths longest-first, so the file goes before the directory holding it.
record OWNED /usr/share/doc/qcctl
put 0644 "$SRC/USAGE.md" /usr/share/doc/qcctl/USAGE.md
# Man pages. Section 8 for both: they are in /usr/sbin. The man1/man8 DIRECTORIES are
# deliberately NOT recorded - they belong to the filesystem, not to us, and an uninstall
# that removed them would take every other package's pages with them.
run install -d -o root -g root -m 0755 /usr/share/man/man8
put 0644 "$SRC/qcctl.8"  /usr/share/man/man8/qcctl.8
put 0644 "$SRC/qcctld.8" /usr/share/man/man8/qcctld.8

# for the daemon or the CLI to work, and deliberately NOT used by --cool-max, which needs
# no privilege at all.
note "5a. polkit action"
if [ -d /usr/share/polkit-1/actions ]; then
    put 0644 "$SRC/io.github.hans_kramer.qcctl.policy" \
        /usr/share/polkit-1/actions/io.github.hans_kramer.qcctl.policy
    if command -v pkexec >/dev/null 2>&1; then
        note "   pkexec present: a GUI can switch profiles unprivileged"
    else
        warn "no pkexec found - the action is installed but nothing can use it yet"
    fi
else
    warn "no /usr/share/polkit-1/actions - skipping the polkit action (polkit absent?)"
fi

# ------------------------------------- what the user's config is missing that we now ship
# Compared by block KIND, not by named block: a renamed or deleted CURVE is the user's
# choice, but a missing kind means a shipped feature their config cannot reach.
# --- BEGIN TESTABLE: config block comparison -------------------------------------------
# The installer test suite sources everything between these markers, so these two
# functions can be exercised without running an installer.
block_kinds() {   # $1 = file; prints the sorted unique set of block kinds
    [ -f "$1" ] || return 0
    grep -oE '^[[:space:]]*<[[:space:]]*[A-Z_]+' "$1" 2>/dev/null \
        | tr -d ' <' | sort -u
}

report_missing_blocks() {   # $1 = user's config, $2 = our reference, $3 = label
    _u=$(block_kinds "$1"); _d=$(block_kinds "$2")
    # No explicit guard for an empty reference - an injection proved one would be dead code.
    # If $_d has no kinds the loop below never runs, $_miss stays empty, and the check after
    # it returns. A guard with no behavioural effect implies a risk it does not address.
    # A plain loop, not comm with process substitution: this script is /bin/sh and <(...)
    # is a bashism that fails at parse time, taking the whole installer with it.
    _miss=""
    for _k in $_d; do
        printf '%s\n' "$_u" | grep -qx "$_k" || _miss="${_miss} $_k"
    done
    [ -n "$_miss" ] || return 0
    warn "your $1 has no${_miss} block - $3"
    for _k in $_miss; do
        echo "    to add the $_k block as shipped:"
        # sudo sh -c, not a bare sudo: the >> redirection happens in the CALLER's shell,
        # so `sudo sed ... >> file` writes as the user and fails on a root-owned config.
        echo "      sudo sh -c \"sed -n '/^< $_k/,/^>/p' $2 >> $1\""
    done
    echo "    the full reference config is at $2"
}
# --- END TESTABLE ---------------------------------------------------------------------

note "6. comparing your config against what this release ships"
if [ "$DRY" = 1 ]; then
    echo "    would compare $ETC/fan.config against $ETC/fan.config.dist"
else
    report_missing_blocks "$ETC/fan.config" "$ETC/fan.config.dist" \
        "features using it are inert until you add it"
    # profiles.conf deliberately gets NO missing-block warning: its blocks are the user's
    # own profiles, and a shipped profile they deleted is a choice, not a gap. The .dist is
    # there as a reference only.
    if ! cmp -s "$ETC/profiles.conf" "$ETC/profiles.conf.dist"; then
        note "   your profiles.conf differs from the shipped one (expected if you edited it);"
        note "   the shipped version is at $ETC/profiles.conf.dist"
    fi
fi

# ------------------------------------------------ validate BEFORE enabling anything
# A unit that starts at boot with a bad config needs a physical power cycle to recover.
note "7. validating the installed config before enabling"
if [ "$DRY" = 1 ]; then echo "    would: /usr/sbin/qcctl-check $ETC/profiles.conf"
elif [ -x /usr/sbin/qcctl-check ]; then
    PCOUT=$(/usr/sbin/qcctl-check "$ETC/profiles.conf" 2>&1); PCRC=$?
    if [ "$PCRC" = 4 ]; then
        # The file is fine; this kernel does not expose something it names. Saying
        # "rejected" here would send the user hunting a syntax error that is not there.
        printf '%s\n' "$PCOUT" | sed -n '/NOT APPLICABLE/,$p' | sed 's/^/    /'
        die "the config is well-formed but does not fit this kernel - refusing to enable the unit"
    elif [ "$PCRC" != 0 ]; then
        printf '%s\n' "$PCOUT" | tail -5 | sed 's/^/    /'
        die "$ETC/profiles.conf is REJECTED by the validator - refusing to enable the unit"
    fi
    note "   config validates, and every profile is applicable here"
else
    warn "qcctl-check not installed - config not validated. Proceeding is a risk."
fi

# ------------------------------------------------------------- enable and start
note "8. enabling and starting"
run systemctl enable qcctld.service
run systemctl enable qcctl-resume.service
# restart, not start: 'start' is a no-op on an active unit, which on --upgrade would leave
# the old daemon running against the new binary on disk.
run systemctl restart qcctld.service
[ "$DRY" = 0 ] && sleep 3

# ----------------------------------------------------------------------- verify
note "9. verifying"
if [ "$DRY" = 1 ]; then echo "  == DRY RUN complete - nothing was changed =="; exit 0; fi
bad=0
systemctl is-enabled --quiet qcctld.service || { warn "unit is NOT enabled"; bad=1; }
# The daemon is always resident, with or without a fan strategy, so 'inactive' is a failure.
if systemctl is-active --quiet qcctld.service; then
    note "   unit: active and resident"
else
    _st=$(systemctl show -p ExecMainStatus --value qcctld.service 2>/dev/null)
    _rs=$(systemctl show -p Result --value qcctld.service 2>/dev/null)
    warn "unit is NOT resident (ExecMainStatus=${_st:-?} Result=${_rs:-?})."
    warn "It is expected to stay running even with FAN MODE kernel. Check the journal."
    bad=1
fi
# These do not appear at the same moment: active is written at startup, status on the
# daemon's first sampling tick a few seconds later. Waited for, not sampled once.
for _f in /run/qcctl/active /run/qcctl/status; do
    _w=0
    while [ ! -f "$_f" ] && [ "$_w" -lt 20 ]; do
        sleep 1
        _w=$((_w + 1))
    done
    if [ -f "$_f" ]; then
        if [ "$_w" -gt 0 ]; then
            note "   $_f published after ${_w}s ($(wc -c < "$_f") bytes)"
        else
            note "   $_f published ($(wc -c < "$_f") bytes)"
        fi
    else
        warn "$_f still missing ${_w}s after the daemon started - check the journal"
        bad=1
    fi
done
note "   enabled: $(systemctl is-enabled qcctld.service)"
for p in 0 2 4; do
    g=$SYSFS/devices/system/cpu/cpufreq/policy$p/scaling_governor
    [ -r "$g" ] && note "   policy$p governor: $(cat "$g")"
done
# Now that nothing can fail and lose a path, keep the last line for each one and drop the
# markers. A failed rewrite leaves the appended superset in place, which is the safe
# direction: more recorded than exists is recoverable, less is not.
if [ "$DRY" = 0 ] && [ -f "$MANIFEST" ]; then
    _tidy=$MANIFEST.tidy
    if awk '/^[A-Z]/ { last[$2] = $0 } END { for (p in last) print last[p] }' "$MANIFEST" \
         | sort -k2 > "$_tidy" && [ -s "$_tidy" ] && mv "$_tidy" "$MANIFEST"; then
        chmod 0644 "$MANIFEST"
    else
        rm -f "$_tidy"
        warn "could not de-duplicate $MANIFEST; it lists everything, possibly twice"
    fi
fi
note "   manifest: $(grep -c '^OWNED' "$MANIFEST") owned, $(grep -c '^PREEXISTING' "$MANIFEST") pre-existing"
# The boot-state snapshot is what --reapply and the resume hook depend on.
if [ -f /var/lib/qcctl/boot-state ]; then
    note "   boot-state snapshot written ($(wc -c < /var/lib/qcctl/boot-state) bytes)"
else
    warn "no boot-state snapshot at /var/lib/qcctl/boot-state - --reapply cannot work"; bad=1
fi
# Scoped to THIS invocation: 'journalctl -b' spans the boot and would report a warning
# from a previous install.
_inv=$(systemctl show -p InvocationID --value qcctld.service 2>/dev/null)
if [ -n "$_inv" ] && journalctl _SYSTEMD_INVOCATION_ID="$_inv" --no-pager 2>/dev/null |
   grep -q "cannot record the active profile"; then
    warn "the daemon could not record the active profile - the resume hook has nothing to read"
    bad=1
fi

echo ""
if [ "$bad" = 0 ]; then
    echo "  INSTALLED. Removal: sudo qcctl-uninstall   (add --purge to drop config)"
    echo "  Enabled at boot. Coming up across a power cycle is verified on this"
    echo "  hardware, though THIS install has not been through one yet."
else
    echo "  INSTALL INCOMPLETE - see above. scripts/uninstall.sh will undo it."
    exit 1
fi
