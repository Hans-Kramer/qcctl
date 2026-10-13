// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer
//
// Two fan strategies, deliberately separate TYPES rather than one type with a mode flag.
//
// The safety ceiling belongs to NEITHER.

#ifndef QCCTL_FANCURVE_H
#define QCCTL_FANCURVE_H

#include "surface.h"
#include <stddef.h>

#define MAX_CURVES      8
#define MAX_TARGETS     8
#define MAX_POINTS      16
#define CURVE_ERRLEN    256
#define SAFETY_CEILING_MC   85000
#define SAFETY_RELEASE_MC    3000   // hysteresis, or it flips at the ceiling
#define REACHABLE_LOW_MC    30000
#define REACHABLE_HIGH_MC   50000

enum failsafe_kind {
    FAILSAFE_KERNEL = 0,    // hand the zone back to the kernel governor
    FAILSAFE_FULL,          // leave the fan at maximum
};

struct cpoint {
    long temp_mc;           // threshold, milli-C
    int  state;             // cooling device state at or above that threshold
    int  line;
};

// Stateless lookup.
struct curve {
    char  name[NAME_LEN];
    char  zone[NAME_LEN];
    char  cdev[NAME_LEN];
    struct cpoint pt[MAX_POINTS];
    int   npt;
    long  hyst_mc;          // fall this far below a point before stepping down
    int   min_state;        // lowest NON-ZERO state allowed; 0 stays legal
    int   poll_ms;
    enum failsafe_kind failsafe;
    int   line;
};

// Closed-loop controller. Needs the two things a curve does not: history and a clock.
struct target {
    char  name[NAME_LEN];
    char  zone[NAME_LEN];
    char  cdev[NAME_LEN];
    long  target_mc;
    long  deadband_mc;      // do nothing inside +/- this
    int   min_state;
    int   max_state;        // 0 = use the device's own max
    int   poll_ms;
    int   step_ms;
    enum failsafe_kind failsafe;
    int   line;
};

// Controller state, passed in explicitly rather than hidden in struct target.
struct target_state {
    long prev_temp_mc;
    long last_step_ms;
    int  have_prev;
};

/* Fan health. */
struct airflow {
    int  present;
    int  delta_pct;         // how far from baseline counts as a fault
    int  debounce;          // consecutive violations before saying anything
    int  line;
};

struct fanconfig {
    struct curve  c[MAX_CURVES];
    int           ncurve;
    struct target t[MAX_TARGETS];
    int           ntarget;
    struct airflow air;
    char err[CURVE_ERRLEN];
    int  err_line;
};

int fanconfig_parse_string(const char *text, struct fanconfig *fc);
int fanconfig_parse_file(const char *path, struct fanconfig *fc);
int fanconfig_validate(const struct fanconfig *fc, const struct surface *s,
                       char *err, size_t errlen);

const struct curve  *fanconfig_find_curve(const struct fanconfig *fc, const char *name);
const struct target *fanconfig_find_target(const struct fanconfig *fc, const char *name);

// PURE. Rising crosses a threshold at once; falling must clear it by the full hysteresis.
int curve_next(const struct curve *c, long temp_mc, int cur);

// STATEFUL. `now_ms` is a monotonic millisecond clock; `st` is updated in place.
int target_next(const struct target *t, struct target_state *st,
                long temp_mc, int cur, long now_ms);

// The safety ceiling, over whatever either strategy decided. Belongs to neither.
int fan_safety_clamp(int want, int cur, long temp_mc, int max_state);
int fan_safety_active(long temp_mc, int cur, int max_state);

#endif /* QCCTL_FANCURVE_H */
