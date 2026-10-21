#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer
# SPDX-FileContributor: Claude Opus 5 (Anthropic), paired on the behavioural suite
#
# The flag surface, driven through argv against the built binary.
#
# Everything else in this suite calls functions. Nothing reached main(), so the exit code a
# caller actually sees, and the order in which flags win, were asserted nowhere: of 22
# options, CI ran one. A dispatch bug that returned 0 where it should return 3 would have
# passed the entire suite.
#
# QCCTL_SYSROOT stands in for root, which needs_root() accepts deliberately, so the writes
# land in a copied fixture and this needs neither a board nor privilege.
set -u

. "$(dirname "$0")/t.sh"

BIN=build/qcctl
[ -x "$BIN" ] || { echo "    t_cli: $BIN not built" >&2; exit 2; }

FXSRC=$(ls -d tests/fixtures/monza-* 2>/dev/null | head -1)
[ -n "$FXSRC" ] || { echo "    t_cli: no captured fixture" >&2; exit 2; }

RT=$(mktemp -d /tmp/t_cli.XXXXXX) || exit 2
trap 'rm -rf "$RT"' EXIT INT TERM
FX=$RT/sysroot
mkdir -p "$FX"
cp -a "$FXSRC/." "$FX/"
export QCCTL_SYSROOT=$FX

MINFREQ=$FX/sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq

# run <args...> - output in $out, status in $rc. Both streams, because half the contract
# here is which stream a refusal goes to and every refusal writes to stderr.
run() { out=$("$BIN" "$@" 2>&1); rc=$?; }

# Contents AND the listing. Contents alone miss a file that was created but left empty -
# a stray staging artifact contributes no bytes to `cat` - and miss a deletion that takes
# an already-empty file with it. The listing carries type, size and path for every entry.
# The fixture has no path with a space in it, so this stays a one-liner.
treesum() {
    {
        find "$FX" -printf '%y %s %p\n' | sort
        find "$FX" -type f | sort | xargs cat 2>/dev/null
    } | cksum
}

cat > "$RT/ok.conf" <<'EOF'
< PROFILE ID=0 NAME=p DEFAULT >
  CPU_POLICY 0  GOVERNOR schedutil
>
EOF
# 902400 is what the fixture holds, so an apply of this is visible in the tree.
cat > "$RT/min.conf" <<'EOF'
< PROFILE ID=0 NAME=p DEFAULT >
  CPU_POLICY 0  MIN 1017600kHz
>
EOF
# TWO profiles, one disruptive and one not. Every config fixture here used to hold exactly
# one, which makes "the gate is per PROFILE" and "the gate is per FILE" indistinguishable -
# and per-file is a defect this project already fixed once, so it could be reintroduced
# with nothing to notice.
cat > "$RT/disr.conf" <<'EOF'
< PROFILE ID=0 NAME=d DEFAULT >
  CPU_ONLINE 0 0
>
< PROFILE ID=1 NAME=ordinary >
  CPU_POLICY 0  GOVERNOR schedutil
>
EOF
cat > "$RT/nodefault.conf" <<'EOF'
< PROFILE ID=0 NAME=p >
  CPU_POLICY 0  GOVERNOR schedutil
>
EOF

# --- help and version short-circuit the parse loop ------------------------------------

run --help
t_eq "$rc" 0 "--help exits 0"
t_has "$out" "usage: qcctl" "and prints the usage"

run -V
t_eq "$rc" 0 "-V exits 0"
t_has "$out" "qcctl " "and names the binary"

# They return from inside the loop, so a flag after one is never acted on. Worth pinning:
# a --help that ran the reset first would be a destructive help.
run --reset --help
t_eq "$rc" 0 "--reset --help exits 0"
t_hasnt "$out" "restored" "and nothing was reset"

# --- refusals, which are all exit 2 ---------------------------------------------------

run --bogus
t_eq "$rc" 2 "an unknown option exits 2"
t_has "$out" "unknown option" "and says so"

run -m
t_eq "$rc" 2 "-m without a profile exits 2"
run -c
t_eq "$rc" 2 "-c without a path exits 2"
run --state
t_eq "$rc" 2 "--state without a path exits 2"
run --fan-baseline
t_eq "$rc" 2 "--fan-baseline without a verb exits 2"

run --fan-baseline sideways
t_eq "$rc" 2 "--fan-baseline with a verb it does not have exits 2"
t_has "$out" "capture" "and names the verbs it does have"

# --cool-max raises cooling and nothing else; combining it with an apply would mean two
# answers to what the fan should do.
run --cool-max -m p
t_eq "$rc" 2 "--cool-max with -m exits 2"
run --cool-max --reset
t_eq "$rc" 2 "--cool-max with --reset exits 2"

# --- the exit codes a caller sees, through argv ----------------------------------------

run -c /no/such.conf -m p
t_eq "$rc" 3 "an unreadable config exits 3"

run -c "$RT/ok.conf" -m nosuch
t_eq "$rc" 3 "a profile that is not in the file exits 3"
t_has "$out" "no profile" "and says which"

run -c "$RT/nodefault.conf" --verify
t_eq "$rc" 3 "a config with no DEFAULT exits 3"

run -c "$RT/ok.conf" --verify
t_eq "$rc" 0 "a good config verifies"
t_has "$out" "validated" "and says so"

# 4 is "nothing is wrong with the file" - both routes to it.
run -c "$RT/disr.conf" -m d
t_eq "$rc" 4 "a disruptive profile without the flag exits 4"
t_has "$out" "--allow-disruptive" "and names the flag that would allow it"

run -c "$RT/disr.conf" -m d --allow-disruptive --dry-run
t_eq "$rc" 0 "with the flag it is allowed"

# The other profile in the SAME file is not disruptive, so it must apply without the flag.
run -c "$RT/disr.conf" -m ordinary --dry-run
t_eq "$rc" 0 "a profile that is not disruptive needs no flag, in the same file"
t_hasnt "$out" "allow-disruptive" "and is not asked for one"
t_has "$out" "would apply" "it just applies"

# A path this kernel does not expose: the config is valid, the board cannot take it.
mv "$MINFREQ" "$RT/saved_min"
run -c "$RT/min.conf" --verify
t_eq "$rc" 4 "a profile that cannot be planned exits 4"
t_has "$out" "would fail on this board" "and says it is the board, not the file"
mv "$RT/saved_min" "$MINFREQ"

run -c "$RT/min.conf" --verify
t_eq "$rc" 0 "and the same config verifies once the path is back"

# --- dispatch precedence --------------------------------------------------------------

run --query --list
t_has "$out" "board:" "--query wins over --list"
t_hasnt "$out" "setting" "so the profile list never printed"

# --query asked together with a change is REFUSED, not silently resolved by dispatch order.
# This used to exit 0, report, and drop the apply with nothing said about it; the suite
# pinned that as the behaviour it was, and TODO.md carried it as a defect. Fixed 2026-10-21.
run -c "$RT/ok.conf" -m p --query
t_eq "$rc" 2 "-m with --query is refused"
t_has "$out" "cannot be combined" "and says why"
t_hasnt "$out" "applying" "applying nothing"
t_hasnt "$out" "board:" "and reporting nothing"

run --query --reset
t_eq "$rc" 2 "--query with --reset likewise"
run --query --save-state
t_eq "$rc" 2 "and with --save-state"

# Each of them alone still works, so the refusal is about the COMBINATION.
run -c "$RT/ok.conf" --query
t_eq "$rc" 0 "--query on its own reports"
t_has "$out" "board:" "the board's state"

run -c "$RT/ok.conf" --verify --list
t_has "$out" "validated" "--verify wins over --list"

run --fan-baseline show --fan-baseline-file "$RT/nope" --query
t_eq "$rc" 1 "--fan-baseline wins over --query"
t_hasnt "$out" "board:" "so no report printed"

run --reapply --active-file "$RT/no-active"
t_eq "$rc" 0 "--reapply with no recorded profile is not an error"
t_has "$out" "nothing to re-apply" "and says why"

# --reapply used to ANNOUNCE a re-apply before dispatch, so with --query it said
# "re-applying" and then only reported. The combination is refused now, and the
# announcement has moved to where the apply happens. Fixed 2026-10-21.
echo p > "$RT/active"
run -c "$RT/ok.conf" --active-file "$RT/active" --reapply --query
t_eq "$rc" 2 "--reapply with --query is refused"
t_hasnt "$out" "re-applying" "and announces nothing it is not doing"

# A real --reapply still announces, because there it is true.
run -c "$RT/ok.conf" --active-file "$RT/active" --reapply --dry-run
t_eq "$rc" 0 "a --reapply that will act exits 0"
t_has "$out" "re-applying" "and says so"
t_has "$out" "would apply" "before doing it"

# --- --dry-run writes nothing ---------------------------------------------------------

before=$(treesum)
run -c "$RT/min.conf" -m p --dry-run
t_eq "$rc" 0 "a dry run exits 0"
t_has "$out" "would apply" "and says it would apply"
t_eq "$(treesum)" "$before" "and the tree is byte-identical afterwards"

# The positive control: the same apply for real must change the tree, or the check above
# would pass for a binary that cannot write at all.
run -c "$RT/min.conf" -m p
t_eq "$rc" 0 "the same apply for real exits 0"
t_has "$out" "applying" "and says applying, not would apply"
t_file_is "$MINFREQ" 1017600 "and the value is in the tree"

# --- output shapes --------------------------------------------------------------------

run --query --json
case "$out" in '{"v":1,"board":'*) t_ok 0 "--query --json is the JSON report" ;;
    *) t_ok 1 "--query --json did not start with the schema and board" ;; esac

run -c "$RT/ok.conf" --list --json
case "$out" in '{"v":1,"profiles":['*) t_ok 0 "--list --json is the JSON profile list" ;;
    *) t_ok 1 "--list --json did not start with the schema and profiles" ;; esac

run -c "$RT/ok.conf" --list
t_has "$out" "setting" "--list on its own is the human list"
t_hasnt "$out" '{"v"' "with no JSON in it"

# --- the file-path flags are honoured -------------------------------------------------

run --state "$RT/s" --save-state
t_eq "$rc" 0 "--save-state exits 0"
t_ok "$([ -s "$RT/s" ] && echo 0 || echo 1)" "and wrote the snapshot where --state said"

run --state "$RT/s" --reset
t_eq "$rc" 0 "--reset of that snapshot exits 0"
t_has "$out" "verified" "and read the restored values back"

run --fan-baseline show --fan-baseline-file "$RT/nope"
t_eq "$rc" 1 "--fan-baseline show of a missing file exits 1"

t_summary
