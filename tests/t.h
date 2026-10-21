// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer
// SPDX-FileContributor: Claude Opus 5 (Anthropic), paired on the behavioural suite
//
// The whole test harness. There is no framework: a test is a main() that calls these
// macros and returns t_summary(), which keeps `make test` buildable with nothing this
// repo does not already need - the same promise the rest of the tree makes.
//
// A deliberate copy of qcstats/tests/t.h, for the same reason io.c and envid.c are copies:
// these are separate repositories and neither may depend on the other's tree. A fix here
// does not propagate there.
//
// Every macro counts a check. A test that runs and checks nothing passes silently
// otherwise, which is the one failure mode a harness must not have.

#ifndef QCCTL_T_H
#define QCCTL_T_H

#include <stdio.h>
#include <string.h>

static int t_checks, t_fails;

#define T_FAIL(fmt, ...) do {                                                   \
        t_fails++;                                                              \
        fprintf(stderr, "    FAIL %s:%d  " fmt "\n", __FILE__, __LINE__,        \
                __VA_ARGS__);                                                   \
    } while (0)

#define T_TRUE(cond, what) do {                                                 \
        t_checks++;                                                             \
        if (!(cond))                                                            \
            T_FAIL("%s", (what));                                               \
    } while (0)

#define T_EQ_INT(got, want, what) do {                                          \
        long long _g = (long long)(got), _w = (long long)(want);                 \
        t_checks++;                                                             \
        if (_g != _w)                                                           \
            T_FAIL("%s: got %lld, want %lld", (what), _g, _w);                  \
    } while (0)

#define T_EQ_U64(got, want, what) do {                                          \
        unsigned long long _g = (unsigned long long)(got);                       \
        unsigned long long _w = (unsigned long long)(want);                      \
        t_checks++;                                                             \
        if (_g != _w)                                                           \
            T_FAIL("%s: got %llu, want %llu", (what), _g, _w);                  \
    } while (0)

#define T_EQ_STR(got, want, what) do {                                          \
        const char *_g = (got), *_w = (want);                                    \
        t_checks++;                                                             \
        if (strcmp(_g, _w) != 0)                                                \
            T_FAIL("%s: got \"%s\", want \"%s\"", (what), _g, _w);              \
    } while (0)

/* A precondition the rest of the case dereferences. T_TRUE records a failure and carries
 * on, which turns a NULL return into a segfault halfway through a file: the run does fail,
 * but t_summary() never prints, the diagnosis is a bare "Segmentation fault", and every
 * case after it silently does not run. Use this wherever the next line would dereference. */
#define T_REQUIRE(cond, what) do {                                              \
        t_checks++;                                                             \
        if (!(cond)) {                                                          \
            T_FAIL("%s (cannot continue this case)", (what));                   \
            return;                                                             \
        }                                                                       \
    } while (0)

// For output a test should not pin whole: assert the part that is the contract.
#define T_HAS(hay, needle, what) do {                                           \
        const char *_h = (hay), *_n = (needle);                                  \
        t_checks++;                                                             \
        if (strstr(_h, _n) == NULL)                                             \
            T_FAIL("%s: \"%s\" is not in the output", (what), _n);              \
    } while (0)

#define T_HASNT(hay, needle, what) do {                                         \
        const char *_h = (hay), *_n = (needle);                                  \
        t_checks++;                                                             \
        if (strstr(_h, _n) != NULL)                                             \
            T_FAIL("%s: \"%s\" is in the output and must not be", (what), _n);  \
    } while (0)

static int t_summary(const char *name)
{
    if (t_fails)
        printf("    %-18s %d checks, %d FAILED\n", name, t_checks, t_fails);
    else if (t_checks)
        printf("    %-18s %d checks ok\n", name, t_checks);
    else
        printf("    %-18s CHECKED NOTHING\n", name);
    return (t_fails || !t_checks) ? 1 : 0;
}

#endif /* QCCTL_T_H */
