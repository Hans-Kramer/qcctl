# SPDX-License-Identifier: MPL-2.0
# SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer
# SPDX-FileContributor: Claude Opus 5 (Anthropic), paired on the behavioural suite
#
# The shell half of the harness, sourced by each tests/sh/t_*.sh. Same contract as
# tests/t.h: every assertion counts, and a test that checks nothing fails - because a
# shell test that silently stops running is the easiest kind to write by accident.
#
# Not executable and not a test itself; `run.sh` skips it by name.

T_CHECKS=0
T_FAILS=0
T_NAME=${T_NAME:-$(basename "$0" .sh)}

t_fail() {
    T_FAILS=$((T_FAILS + 1))
    echo "    FAIL $T_NAME: $1" >&2
}

# t_eq <got> <want> <what>
t_eq() {
    T_CHECKS=$((T_CHECKS + 1))
    [ "$1" = "$2" ] && return 0
    t_fail "$3: got '$1', want '$2'"
}

# t_true <condition-description> <what>, used as: t_true "[ -f x ]" ...
t_ok() {
    T_CHECKS=$((T_CHECKS + 1))
    [ "$1" = 0 ] && return 0
    t_fail "$2"
}

# t_has <text> <needle> <what>
t_has() {
    T_CHECKS=$((T_CHECKS + 1))
    case "$1" in
        *"$2"*) return 0 ;;
    esac
    t_fail "$3: '$2' is not in the output"
}

# t_hasnt <text> <needle> <what>
t_hasnt() {
    T_CHECKS=$((T_CHECKS + 1))
    case "$1" in
        *"$2"*) t_fail "$3: '$2' is in the output and must not be"; return ;;
    esac
}

# t_file_is <path> <want> <what> - the contents of a one-line file
t_file_is() {
    T_CHECKS=$((T_CHECKS + 1))
    _got=$(cat "$1" 2>/dev/null)
    [ "$_got" = "$2" ] && return 0
    t_fail "$3: $1 holds '$_got', want '$2'"
}

# t_skip <reason> - something this machine does not have. Exit 77 is the runner's signal
# for "skipped", which it reports without failing; CI asserts the word never appears, because
# there the only honest outcome is to run.
t_skip() {
    printf '    %-18s SKIPPED: %s\n' "$T_NAME" "$1"
    exit 77
}

t_summary() {
    if [ "$T_FAILS" -gt 0 ]; then
        printf '    %-18s %s checks, %s FAILED\n' "$T_NAME" "$T_CHECKS" "$T_FAILS"
        exit 1
    fi
    if [ "$T_CHECKS" = 0 ]; then
        printf '    %-18s CHECKED NOTHING\n' "$T_NAME"
        exit 1
    fi
    printf '    %-18s %s checks ok\n' "$T_NAME" "$T_CHECKS"
    exit 0
}
