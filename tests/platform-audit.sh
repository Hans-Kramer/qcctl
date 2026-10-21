#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer
# SPDX-FileContributor: Claude Opus 5 (Anthropic), paired on the behavioural suite
#
# Does the platform still provide what this project is built on?
#
# Nothing here tests the code. It watches the two things that move underneath it, neither of
# which any suite can notice:
#
#   The kernel. Every control point this project writes is a sysfs attribute that exists only
#   because a config option is set. A bump that drops one breaks the project on hardware, and
#   the only warning would be a board. The config ships in linux-buildinfo-*-qcom, which is
#   1.3 MB - three orders of magnitude smaller than linux-modules, which also carries it.
#
#   The image. etc/profiles.conf records the BUILD_ID it was written against. A newer image
#   is not breakage, but it is something nobody has run this on.
#
# Run it by hand or from .github/workflows/platform.yml. A missing config flag FAILS; drift is
# a warning, because a new publication is news rather than a fault.
#
#   sh tests/platform-audit.sh [--kernels] [--images]
set -u

POOL=https://ports.ubuntu.com/ubuntu-ports/pool/universe/l/linux-qcom
# What is PUBLISHED, which the pool cannot tell us: it keeps every version ever uploaded, and
# nine of them were in there when this was written. Launchpad's API answers the real question
# in a few hundred bytes.
LP="https://api.launchpad.net/1.0/ubuntu/+archive/primary?ws.op=getPublishedSources\
&source_name=linux-qcom&exact_match=true&status=Published"
IMAGES=https://people.canonical.com/~platform/images/qualcomm-iot/ventunoq/ubuntu-desktop-24.04
CONF=etc/profiles.conf

# Every flag this project depends on, and what depends on it. This list is the authority:
# add a flag here when a new control point starts depending on one.
FLAGS="CONFIG_THERMAL_WRITABLE_TRIPS:THERMAL_ZONE_TRIP_TEMP_writes
CONFIG_THERMAL_GOV_STEP_WISE:the_whole_release_model
CONFIG_CPU_FREQ_GOV_SCHEDUTIL:every_shipped_profile
CONFIG_CPU_FREQ_GOV_ONDEMAND:the_vendor_boot_governor_profiles_supersede
CONFIG_UCLAMP_TASK:UCLAMP_MIN_MAX
CONFIG_SENSORS_EMC2305:the_fan_and_its_rpm
CONFIG_DEVFREQ_GOV_SIMPLE_ONDEMAND:the_GPU_governor_profiles_name
CONFIG_QCOM_TSENS:every_temperature_either_project_reports"

rc=0
do_kernels=0
do_images=0
for a in "$@"; do
    case "$a" in
        --kernels) do_kernels=1 ;;
        --images)  do_images=1 ;;
        *) echo "$0: unknown argument '$a'" >&2; exit 2 ;;
    esac
done
[ "$do_kernels" = 0 ] && [ "$do_images" = 0 ] && { do_kernels=1; do_images=1; }

say()  { echo "$*"; }
warn() { echo "  !! $*" >&2; [ "${GITHUB_ACTIONS:-}" = true ] && echo "::warning::$*"; }
err()  { echo "  ** $*" >&2; [ "${GITHUB_ACTIONS:-}" = true ] && echo "::error::$*"; rc=1; }
summary() { [ -n "${GITHUB_STEP_SUMMARY:-}" ] && echo "$*" >> "$GITHUB_STEP_SUMMARY"; return 0; }

fetch() { curl -sSfL --retry 2 --max-time 120 "$1"; }

# ----------------------------------------------------------------- kernels
audit_kernels() {
    tmp=$(mktemp -d) || return 1
    lp=$(fetch "$LP") || { err "cannot reach launchpad to ask what is published"; rm -rf "$tmp"; return 1; }
    vers=$(printf '%s' "$lp" | grep -o '"source_package_version": "[^"]*"' |
           sed 's/.*: "//; s/"//' | sort -u)
    [ -n "$vers" ] || { err "no published linux-qcom versions found - the extractor is broken,
    not the archive"; rm -rf "$tmp"; return 1; }

    say "published linux-qcom versions: $(printf '%s' "$vers" | tr '\n' ' ')"
    say ""
    summary "## Kernel config"
    summary ""
    summary "| kernel | $(echo "$FLAGS" | cut -d: -f1 | sed 's/CONFIG_//' | paste -sd'|') |"
    summary "|---|$(echo "$FLAGS" | sed 's/.*/---/' | paste -sd'|')|"

    for full in $vers; do
        # 6.8.0-1084.89 is the package version; 6.8.0-1084 is the ABI the binary packages
        # are named for.
        ver=${full%.*}
        deb="linux-buildinfo-${ver}-qcom_${full}_arm64.deb"
        rm -rf "$tmp/x"; mkdir -p "$tmp/x"
        if ! fetch "$POOL/$deb" > "$tmp/x/b.deb"; then
            err "cannot download $deb"
            continue
        fi
        ( cd "$tmp/x" && ar x b.deb && tar xf data.tar.* ) >/dev/null 2>&1 || {
            err "cannot unpack $deb"
            continue
        }
        # It lives at usr/lib/linux/<ver>/config, not /boot/config-* as on a running system.
        cfg=$(find "$tmp/x" -type f \( -name config -o -name 'config-*' \) | head -1)
        [ -n "$cfg" ] || { err "$deb carries no config file"; continue; }

        say "kernel $ver  ($(wc -c < "$tmp/x/b.deb") bytes, config $(wc -l < "$cfg") lines)"
        row="| $ver |"
        for entry in $FLAGS; do
            flag=${entry%%:*}
            why=${entry#*:}
            val=$(sed -n "s/^$flag=//p" "$cfg" | head -1)
            case "${val:-}" in
                y|m) printf '  %-38s %s\n' "$flag" "=$val" ; row="$row $val |" ;;
                "")  err "$ver does NOT set $flag - $why depends on it"
                     row="$row **unset** |" ;;
                *)   warn "$ver sets $flag=$val, which is neither y nor m"
                     row="$row $val |" ;;
            esac
        done
        summary "$row"
        say ""
    done
    rm -rf "$tmp"
}

# ----------------------------------------------------------------- images
audit_images() {
    index=$(fetch "$IMAGES/") || { err "cannot read the image index"; return 1; }
    # A build id is YYYYMMDD-NNN. Matched by SHAPE, not by year: a literal 2026 here would
    # have made the first 2027 publication read as "the extractor is broken" - a wrong
    # diagnosis of a working script, from a job that runs unattended every week.
    builds=$(printf '%s\n' "$index" | grep -oE 'href="[0-9]{8}-[0-9]+/"' |
             sed 's/href="//; s|/"||' | sort -u)
    [ -n "$builds" ] || {
        err "no image builds found at $IMAGES/"
        err "  either the archive moved, or the directory names are no longer YYYYMMDD-NNN"
        return 1
    }

    recorded=$(sed -n 's/^# *BUILD_ID=//p' "$CONF" | head -1)
    [ -n "$recorded" ] || { err "no BUILD_ID recorded in $CONF"; return 1; }

    newest=$(printf '%s\n' "$builds" | sort | tail -1)
    say "published images: $(printf '%s' "$builds" | tr '\n' ' ')"
    say "recorded in $CONF: $recorded"
    summary ""
    summary "## Images"
    summary ""
    summary "- published: \`$(printf '%s' "$builds" | tr '\n' ' ')\`"
    summary "- recorded in \`$CONF\`: \`$recorded\`"

    if [ "$newest" = "$recorded" ]; then
        say "  the recorded build is the newest published one"
        summary "- the recorded build is the newest published one"
    else
        warn "image $newest is newer than the recorded $recorded - nothing has been run on it"
        summary "- **$newest is newer than the recorded $recorded** - nothing has been run on it"
    fi
}

[ "$do_kernels" = 1 ] && audit_kernels
[ "$do_images" = 1 ] && audit_images

if [ "$rc" = 0 ]; then
    say "platform audit: nothing this project depends on has gone missing"
else
    say "platform audit: FAILED - see the errors above"
fi
exit $rc
