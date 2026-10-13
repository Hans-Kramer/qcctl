// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer
//
// Taking the fan's thermal zone away from the kernel, and giving it back safely.

#ifndef FANOWN_H
#define FANOWN_H

#include "surface.h"
#include <stddef.h>

struct fanown {
    char zone_mode_before[NAME_LEN];    // "" = never read, so never restored
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


int  fanown_release_point(const struct surface *s, const char *zone_name, long *out_mc);

// Command a cooling state.
int  fanown_set(struct fanown *fo, int state);

int fanown_release(struct fanown *fo, enum fanown_release how);

#endif
