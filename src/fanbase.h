// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer
//
// The captured fan baseline, and judging live rpm against it.
//
// A CAPTURED BASELINE AND NOT A SHIPPED TABLE.

#ifndef FANBASE_H
#define FANBASE_H

#include <stddef.h>

#include "envid.h"

#define FANBASE_MAX_STATE 10
// State 0 is 0 rpm and state 1's tachometer misreads on this fan, so neither
// carries a usable signal. Capture and judgement both start at 2.
#define FANBASE_MIN_STATE 2
#define FANBASE_DEF_DELTA_PCT 8
#define FANBASE_MIN_DELTA_PCT 5

struct fanbase {
    int   have[FANBASE_MAX_STATE + 1];      // a reading exists for this state
    long  mean[FANBASE_MAX_STATE + 1];
    long  lo[FANBASE_MAX_STATE + 1];        // observed min, kept so spread is visible
    long  hi[FANBASE_MAX_STATE + 1];
    int   n[FANBASE_MAX_STATE + 1];         // samples averaged
    int   max_state;
    char  captured[40];
    char  board[96];
    char  cdev[64];
    struct envid env;
};

enum fanbase_verdict {
    FANBASE_UNKNOWN = 0,   // no baseline for this state, or the state carries no signal
    FANBASE_OK,
    FANBASE_RESTRICTED,    // rpm ABOVE baseline: the inlet is obstructed
    FANBASE_DEGRADED,      // rpm BELOW baseline: the fan is not delivering
};

// Read and write the baseline file. 0 on success, -1 with a reason in err.
int fanbase_write(const char *path, const struct fanbase *fb, char *err, size_t errlen);
int fanbase_read(const char *path, struct fanbase *fb, char *err, size_t errlen);

enum fanbase_verdict fanbase_judge(const struct fanbase *fb, int state, long rpm,
                                   int delta_pct, long *dev_pct);

const char *fanbase_verdict_str(enum fanbase_verdict v);

#endif
