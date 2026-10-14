// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer
//
// Taking the fan's thermal zone away from the kernel, and giving it back safely.

#ifndef FANOWN_H
#define FANOWN_H

#include "surface.h"
#include <stddef.h>

struct fanown {
    char zone_mode_before[NAME_LEN];    // always "enabled" when owned; take refuses otherwise
    char fan_state_before[NAME_LEN];
    int  zone_idx;
    int  cdev_idx;
    int  owned;                         // we hold the zone
    int  max_state;                     // the cooling device's own ceiling
    int  last_state;                    // last state WE commanded, -1 if none.
    int  binding;
    char release_err[384];
};

enum fanown_release {
    FANOWN_HANDBACK = 0,
    FANOWN_LEAVE_MAX,
    FANOWN_LEAVE_AS_IS,
};

int  fanown_take(struct fanown *fo, const struct surface *s,
                 const char *zone_name, const char *cdev_name,
                 char *err, size_t errlen);


// out_mc: the governor's release point (lowest active trip minus its hysteresis).
// out_trip_mc: that trip itself, which is the temperature above which the board is
// actually hot. Either may be NULL.
int  fanown_release_point(const struct surface *s, const char *zone_name, long *out_mc,
                          long *out_trip_mc);

// Command a cooling state.
int  fanown_set(struct fanown *fo, int state);

int fanown_release(struct fanown *fo, enum fanown_release how);

// Leave the fan where the kernel itself would hold it, so nothing has to be walked back.
// The governor only rewrites a cooling device when its own target changes, and on this
// hardware a downward trip transition decrements by ONE - so a state it would not have
// chosen can take many heat/cool cycles to clear, which on an idle board is never.
//   below the active trip        -> 0; the next upward crossing raises it to the floor
//   at or above it               -> the cooling map's lower limit, the kernel's own answer
//   no temperature, or no floor  -> FANOWN_LEAVE_MAX; more cooling is the safe guess
// *out_state is the state to write before releasing, or -1 to write nothing.
enum fanown_release fanown_release_mode(const struct surface *s, const char *zone_name,
                                        int have_temp, long temp_mc, int *out_state);

#endif
