#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer
# SPDX-FileContributor: Claude Opus 5 (Anthropic), paired on the behavioural suite
#
# install.sh and uninstall.sh against real destinations, as real root, in a filesystem that
# can be thrown away. This is the only way to exercise them: they write to /usr/sbin and
# /var/lib by design, and an install that could be redirected somewhere harmless would be a
# different contract from the one this project offers.
#
# Run from the repository root. Outside a sandbox it stages a native build and re-runs itself
# inside a disposable container; inside, it is already root with nothing to lose.
#
# Two deliberate limits, both of which are why steps 8 and 9 remain hardware checks:
#
#   A container has no systemd as PID 1. A stub `systemctl` that fails is put on PATH, so the
#   pre-flight's `command -v systemctl` passes and step 8 then dies - which makes this test
#   the MID-RUN FAILURE case by construction. That is the case the manifest defect of
#   2026-10-18 lived in, so it is the one worth automating.
#
#   QCCTL_SYSROOT supplies the board shape the pre-flight probes for. The destinations are
#   untouched by that: they are the real ones, in a throwaway root.
set -u

. "$(dirname "$0")/t.sh"

# ---------------------------------------------------------------- outside the sandbox
if [ "${QCCTL_TEST_SANDBOX:-0}" != 1 ]; then
    rt=$(command -v docker 2>/dev/null || command -v podman 2>/dev/null || true)
    [ -n "$rt" ] || t_skip "no container runtime (docker or podman)"
    STAGE=${STAGE:-/tmp/qcctl-stage-t_install}
    rm -rf "$STAGE"
    make --no-print-directory stage-native STAGE="$STAGE" >/dev/null 2>&1 \
        || t_skip "cannot stage a native build - is ../qcstats built?"
    exec "$rt" run --rm \
        -v "$PWD:/src:ro" -v "$STAGE:/stage:ro" \
        -e QCCTL_TEST_SANDBOX=1 \
        ubuntu:24.04 \
        sh -c 'cp -a /src /w && cp -a /stage /w/stage && cd /w && sh tests/sh/t_install.sh'
fi

# ---------------------------------------------------------------- inside the sandbox
[ "$(id -u)" = 0 ] || { echo "    t_install: not root inside the sandbox" >&2; exit 2; }

FX=$(ls -d tests/fixtures/monza-* 2>/dev/null | head -1)
[ -n "$FX" ] || { echo "    t_install: no captured fixture" >&2; exit 2; }
export QCCTL_SYSROOT=$PWD/$FX

# A systemctl stub that answers per VERB, not a blanket success.
#
# A stub that exits 0 for everything is quietly dishonest about step 9: `is-enabled` and
# `is-active` then "succeed" whatever the daemon is doing, and `show -p InvocationID`
# returns nothing so the journal check never runs at all. The step's verdict came entirely
# from the separate boot-state check, and deleting its unit checks changed no result.
#
# So: the verbs the install itself needs (daemon-reload, enable, start, stop) succeed,
# because a failure there would kill the run long before the file work this test is about -
# and the INSPECTION verbs answer from a file this test controls, so both branches of step 9
# can be driven. What systemd would really have done is still a hardware check.
mkdir -p /usr/local/bin /run/qcctl-test
echo 1 > /run/qcctl-test/is-enabled        # not enabled, which is the truth in a container
echo 1 > /run/qcctl-test/is-active         # and not running
cat > /usr/local/bin/systemctl <<'STUB'
#!/bin/sh
verb=""
for a in "$@"; do
    case "$a" in --*) ;; *) verb=$a; break ;; esac
done
case "$verb" in
    is-enabled|is-active) exit "$(cat /run/qcctl-test/$verb 2>/dev/null || echo 1)" ;;
    show)                 exit 0 ;;
    *)                    exit 0 ;;
esac
STUB
chmod 0755 /usr/local/bin/systemctl

# The daemon cannot run here, so step 9 would spend 20 seconds each waiting for the two files
# it publishes. They are faked to keep this test quick; the real handoff is covered by
# tests/sh/t_daemon.sh and by scripts/board-check.sh.
mkdir -p /run/qcctl
: > /run/qcctl/active
: > /run/qcctl/status

MANIFEST=/var/lib/qcctl/install.manifest
# Every destination install.sh is expected to create, which is also what uninstall.sh must
# remove. Kept here rather than read from the manifest, because reading the manifest to check
# the manifest proves nothing.
DESTS="/usr/sbin/qcctld /usr/sbin/qcctl /usr/sbin/qcctl-surface-dump
/usr/sbin/qcctl-fan-release /usr/sbin/qcctl-uninstall /usr/sbin/qcctl-check /usr/bin/qcstats
/etc/systemd/system/qcctld.service /etc/systemd/system/qcctl-resume.service
/usr/share/doc/qcctl/USAGE.md /usr/share/polkit-1/actions/io.github.hans_kramer.qcctl.policy
/etc/qcctl/fan.config.dist /etc/qcctl/profiles.conf.dist"

in_manifest() {  # in_manifest <path>
    awk -v p="$1" '/^[A-Z]/ && $2 == p { found = 1 } END { exit !found }' "$MANIFEST"
}

# ---- 1. a full install: every file placed, every file recorded ---------------------------
sh stage/install.sh >/tmp/i1.log 2>&1
rc=$?
log=$(cat /tmp/i1.log)
t_has "$log" "9. verifying" "the install runs every step"
# It reports itself INCOMPLETE and exits non-zero, which is correct: no daemon ran here, so
# there is no boot-state snapshot. An installer that called this a success would be lying.
t_eq "$rc" 1 "and reports itself incomplete rather than claiming success"
t_has "$log" "INSTALL INCOMPLETE" "saying so"
t_has "$log" "no boot-state snapshot" "and why"

# The unit checks now report for real, instead of being masked by a stub that said yes to
# everything. Both are expected to fail here: nothing enabled or started this unit.
t_has "$log" "unit is NOT enabled" "step 9 notices the unit is not enabled"
t_has "$log" "unit is NOT resident" "and that it is not running"

# And the other way round: with the stub answering yes, those two warnings must disappear.
# Without this, a step 9 that never asked would look identical to one that asked and was
# satisfied.
echo 0 > /run/qcctl-test/is-enabled
echo 0 > /run/qcctl-test/is-active
# --upgrade, because the first install is still in place: a second plain run refuses at the
# pre-flight and never reaches step 9, which is what this is about.
sh stage/install.sh --upgrade >/tmp/i1b.log 2>&1 || true
log2=$(cat /tmp/i1b.log)
t_hasnt "$log2" "unit is NOT enabled" "with the unit enabled, that warning is gone"
t_hasnt "$log2" "unit is NOT resident" "and so is the other"
t_has "$log2" "unit: active and resident" "replaced by the positive report"
echo 1 > /run/qcctl-test/is-enabled
echo 1 > /run/qcctl-test/is-active

missing=
for p in $DESTS; do
    [ -e "$p" ] || { missing="$missing MISSING:$p"; continue; }
    in_manifest "$p" || missing="$missing UNRECORDED:$p"
done
t_eq "$missing" "" "every destination exists and is recorded"
t_ok "$(ls /tmp/tmp.* >/dev/null 2>&1 && echo 1 || echo 0)" \
     "and no temporary copy of the manifest is left in /tmp"
t_eq "$(grep -c '^#RUN' "$MANIFEST")" 0 \
     "the manifest is tidied once nothing can be lost by rewriting it"
t_eq "$(awk '/^[A-Z]/{print $2}' "$MANIFEST" | sort | uniq -d | wc -l)" 0 \
     "with no duplicate paths"

# ---- 2. an upgrade that DIES still records everything on disk ----------------------------
# The defect of 2026-10-18: the manifest was emptied before the new entries were written, so
# an install that died in between left files no uninstall would ever find. Induced the same
# way it was demonstrated on the board - a regular file where a directory has to go.
rm -rf /usr/share/doc/qcctl
touch /usr/share/doc/qcctl
sh stage/install.sh --upgrade >/tmp/i2.log 2>&1
rc=$?
t_ok "$([ "$rc" != 0 ] && echo 0 || echo 1)" "the upgrade dies on the blocked path"
t_has "$(cat /tmp/i2.log)" "failed:" "saying what failed"

missing=
for p in $DESTS; do
    [ "$p" = /usr/share/doc/qcctl/USAGE.md ] && continue   # genuinely gone: we blocked it
    [ -e "$p" ] || continue
    in_manifest "$p" || missing="$missing UNRECORDED:$p"
done
t_eq "$missing" "" "and every path still on disk is still recorded"
t_ok "$(grep -q '^#RUN' "$MANIFEST" && echo 0 || echo 1)" \
     "the run marker is there, because the tidy never got to run"
rm -f /usr/share/doc/qcctl

# ---- 3. the user's own data survives a removal, and --purge takes it ---------------------
printf '# a captured baseline\nstate 10 5252\n' > /var/lib/qcctl/fan-baseline
sum=$(md5sum /var/lib/qcctl/fan-baseline | cut -d' ' -f1)
sh /usr/sbin/qcctl-uninstall >/tmp/u1.log 2>&1
left=
for p in $DESTS; do [ -e "$p" ] && left="$left $p"; done
t_eq "$left" "" "a plain uninstall removes every path it installed"
t_ok "$([ -f /var/lib/qcctl/fan-baseline ] && echo 0 || echo 1)" \
     "the fan baseline survives, because it is the operator's measurement"
t_eq "$(md5sum /var/lib/qcctl/fan-baseline 2>/dev/null | cut -d' ' -f1)" "$sum" \
     "byte-identical, having been stashed and put back"
t_ok "$([ -f /etc/qcctl/profiles.conf ] && echo 0 || echo 1)" "and the config is kept"
t_has "$(cat /tmp/u1.log)" "REMOVED cleanly" "and it says so"
t_ok "$(ls /tmp/tmp.* >/dev/null 2>&1 && echo 1 || echo 0)" \
     "with no stash left behind in /tmp"

# ---- 4. --purge is the everything switch, and works on what a plain removal left ---------
# No reinstall: a plain uninstall leaves the config, the kept-config record and the baseline,
# which is exactly the state an operator later decides to purge from.
sh scripts/uninstall.sh --purge >/tmp/u2.log 2>&1
t_ok "$([ -e /etc/qcctl ] && echo 1 || echo 0)" "--purge removes the config directory"
t_ok "$([ -e /var/lib/qcctl/fan-baseline ] && echo 1 || echo 0)" "and the fan baseline"
t_has "$(cat /tmp/u2.log)" "purging" "saying which of the two it is doing"

t_summary
