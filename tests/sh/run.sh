#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer
# SPDX-FileContributor: Claude Opus 5 (Anthropic), paired on the behavioural suite
#
# Run every tests/sh/t_*.sh. Each is a separate process with its own temp fixture, so one
# test cannot leave state behind that another depends on - the mistake that made a C test
# in this suite pass for the wrong reason on its first day.
#
# Run from the repository root: `make test` does, and the tests resolve build/ and
# tests/fixtures/ relative to it.
set -u

cd "$(dirname "$0")/../.." || exit 2
here=tests/sh
found=0
fail=0
skipped=0

for t in "$here"/t_*.sh; do
    [ -f "$t" ] || continue
    found=$((found + 1))
    # Output flows through: each test prints its own line, including its skip notice.
    sh "$t"
    case "$?" in
        0)  ;;
        77) skipped=$((skipped + 1)) ;;
        *)  fail=1 ;;
    esac
done

[ "$found" -gt 0 ] || { echo "== no shell tests found in $here"; exit 1; }
[ "$fail" = 0 ] || exit 1
exit 0
