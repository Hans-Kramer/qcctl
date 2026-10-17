// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer
//
// Taking the fan's thermal zone away from the kernel, and giving it back safely.

#ifndef FANOWN_H
#define FANOWN_H

#include "surface.h"
#include <stddef.h>

struct fanown {
    char zone[NAME_LEN];                // the zone we took, so a release needs no caller to say
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


// The lowest `active` trip on this zone, which is the temperature at or above which the
// board needs cooling. Returns 0 if the zone has no active trip.
//
// This used to also hand back that trip minus its hysteresis - the governor's release
// point - and comparing against THAT rather than the trip is what left the fan pinned
// through the hysteresis band. No caller wants it now, so it is gone rather than left
// here for someone to reach for.
int  fanown_active_trip(const struct surface *s, const char *zone_name, long *out_trip_mc);

// The temperature below which the kernel treats the zone's lowest active trip as CLEAR:
// the trip minus its hysteresis. Below it the governor's own answer is state 0, so a
// higher state down there is one it never chose and will never correct, since it rewrites
// a cooling device only on a trip transition.
//
// This is the number fanown_release_point() used to return, and it was deleted for being
// the wrong one THERE: a release asks "what would the kernel hold here", and inside the
// band that is the map's floor, not maximum - comparing against this is what left the fan
// pinned. The question here is the opposite one, "is this a state the kernel would never
// hold", and that one does have to exclude the band, or a correction would fight a
// governor entitled to its answer.
int  fanown_clear_point(const struct surface *s, const char *zone_name, long *out_mc);

// Lower a cooling state the kernel would never hold, on a zone we do NOT own: below the
// clear point above, its own answer is 0. Acts only when the zone drives exactly one
// cooling device, because with several, which one is the fan is a guess.
// 1 = it wrote 0, and *out_* describe what it found; 0 = nothing to do; -1 = read or
// write failure. The caller owns the policy of how often to do this, and when to stop.
int  fanown_correct_stale(const struct surface *s, const char *zone_name, long temp_mc,
                          long *out_clear_mc, int *out_state);

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
