<!-- SPDX-License-Identifier: CC-BY-4.0 -->
<!-- SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer -->
<!-- SPDX-FileContributor: Claude Opus 5 (Anthropic), paired on the behavioural suite -->
# A captured qcstats stream

`sample.ndjson` is real output from `qcstats`, kept here because `qcctl` parses that stream
with `strstr` and must be testable without the sibling repository present. It is the meta
line followed by one sample line, which is what the daemon actually reads at startup.

Produced by running the sibling project against **its** committed fixture, so no board was
involved:

```sh
cd ../qcstats
QCSTATS_SYSROOT=tests/fixtures/monza-20260710-235-6.8.0-1084 \
  ./qcstats --json --count 2 > ../qcctl/tests/fixtures/stream/sample.ndjson
```

`tests/t_contract.c` asserts against this copy, and CI's `contract` job - which checks out both
repositories - asserts again against a **freshly generated** line. The copy proves the parser
handles the documented shape; the fresh line is what catches the producer drifting away from
it. A field renamed on the qcstats side makes `qcctl` read "not measured" forever, which is a
valid answer in this schema and therefore silent.

The values are machine output and not authored content; the gate's SPDX assertion skips the
data and points here, as it does for the sysfs fixtures.
