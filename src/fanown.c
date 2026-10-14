// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer

#define _GNU_SOURCE
#include "fanown.h"
#include "io.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define THERMAL_DIR "/sys/class/thermal"

int fanown_take(struct fanown *fo, const struct surface *s,
                const char *zone_name, const char *cdev_name,
                char *err, size_t errlen)
{
    const struct zone *z = surface_zone(s, zone_name);
    const struct cdev *cd = surface_cdev(s, cdev_name);
    char path[PATH_MAXLEN];

    memset(fo, 0, sizeof *fo);
    fo->last_state = -1;

    if (!z)  { snprintf(err, errlen, "no zone '%s'", zone_name); return -1; }
    if (!cd) { snprintf(err, errlen, "no cooling device '%s'", cdev_name); return -1; }
    fo->zone_idx = z->idx;
    fo->cdev_idx = cd->idx;

    /* The mode we must write back. Without it the release below would skip the zone and
     * leave nothing managing the fan, so an unreadable mode refuses the take outright. */
    spath(path, sizeof path, THERMAL_DIR "/thermal_zone%d/mode", z->idx);
    if (read_file_str(path, fo->zone_mode_before, sizeof fo->zone_mode_before) < 0) {
        /* read_file_str() conflates "cannot open" with "read nothing", and an empty
         * sysfs attribute is the likelier of the two here - so no strerror, which
         * would report a stale errno from some earlier call. */
        snprintf(err, errlen,
                 "zone %d (%s) mode is unreadable or empty - refusing to disable a zone "
                 "that cannot be handed back", z->idx, zone_name);
        return -1;
    }
    spath(path, sizeof path, THERMAL_DIR "/cooling_device%d/cur_state", cd->idx);
    if (read_file_str(path, fo->fan_state_before, sizeof fo->fan_state_before) < 0)
        fo->fan_state_before[0] = '\0';

    /* Only "enabled" is takeable: "disabled" means someone already owns it, and anything
     * else is not the ABI this restores against. */
    if (strcmp(fo->zone_mode_before, "enabled")) {
        if (!strcmp(fo->zone_mode_before, "disabled"))
            snprintf(err, errlen,
                     "zone %d (%s) is already disabled - something else owns the fan",
                     z->idx, zone_name);
        else
            snprintf(err, errlen,
                     "zone %d (%s) mode reads '%s', not 'enabled' - refusing to take a "
                     "zone this cannot restore", z->idx, zone_name, fo->zone_mode_before);
        return -1;
    }

    spath(path, sizeof path, THERMAL_DIR "/thermal_zone%d/mode", z->idx);
    if (write_path_str(path, "disabled") < 0) {
        snprintf(err, errlen, "cannot disable zone %d: %s", z->idx, strerror(errno));
        return -1;
    }
    fo->owned = 1;
    fo->max_state = cd->state_ok ? (int)cd->max_state : 10;
    fo->binding = surface_zone_binds(s, z, cd);
    return 0;
}

int fanown_release_point(const struct surface *s, const char *zone_name, long *out_mc)
{
    const struct zone *z = surface_zone(s, zone_name);
    long best = -1, hyst = 0;

    if (!z)
        return 0;
    for (int i = 0; i < z->ntrips; i++) {
        if (strcmp(z->trips[i].type, "active"))
            continue;
        if (best < 0 || z->trips[i].temp_mc < best) {
            best = z->trips[i].temp_mc;
            hyst = z->trips[i].hyst_mc;
        }
    }
    if (best < 0)
        return 0;
    *out_mc = best - hyst;
    return 1;
}

int fanown_set(struct fanown *fo, int state)
{
    char path[PATH_MAXLEN], val[16];

    fo->last_state = state;
    spath(path, sizeof path, THERMAL_DIR "/cooling_device%d/cur_state", fo->cdev_idx);
    snprintf(val, sizeof val, "%d", state);
    return write_path_str(path, val);
}

int fanown_release(struct fanown *fo, enum fanown_release how)
{
    char path[PATH_MAXLEN], v[8];
    int fan_failed = 0, zone_failed = 0, fan_errno = 0, zone_errno = 0;

    fo->release_err[0] = '\0';
    if (!fo->owned)
        return 0;

    spath(path, sizeof path, THERMAL_DIR "/cooling_device%d/cur_state", fo->cdev_idx);
    if (how == FANOWN_LEAVE_MAX) {
        snprintf(v, sizeof v, "%d", fo->max_state ? fo->max_state : 10);
        if (write_path_str(path, v) < 0) { fan_failed = 1; fan_errno = errno; }
    } else if (how == FANOWN_LEAVE_AS_IS) {
        /* Nothing to write: the caller has already set what it wants. */
    } else if (fo->fan_state_before[0]) {
        /* Never lower. Restoring the captured state verbatim is the obvious thing and it
         * is unsafe: the fan's zone has no polling_delay, so the kernel re-evaluates only
         * on a trip crossing. Handing a hot board back with the old low state leaves it
         * uncooled indefinitely, because the crossing it waits for already happened. */
        int before = atoi(fo->fan_state_before);
        int keep = (fo->last_state > before) ? fo->last_state : before;
        snprintf(v, sizeof v, "%d", keep);
        if (write_path_str(path, v) < 0) { fan_failed = 1; fan_errno = errno; }
    }

    if (fo->zone_mode_before[0]) {
        spath(path, sizeof path, THERMAL_DIR "/thermal_zone%d/mode", fo->zone_idx);
        if (write_path_str(path, fo->zone_mode_before) < 0) {
            zone_failed = 1;
            zone_errno = errno;
        }
    }
    /* Cleared either way: we have stopped driving the fan whatever the kernel accepted,
     * and a struct that claims we still own the zone would have the next release try to
     * hand back something we are not holding. */
    fo->owned = 0;

    /* The ZONE failure is named first and in the strongest terms available. A fan-state
     * write that is refused leaves the fan wherever it was - usually still cooling. A
     * zone that will not re-enable leaves NOTHING managing the fan: not the kernel,
     * because its zone is disabled, and not us, because we have just stopped. */
    if (zone_failed)
        snprintf(fo->release_err, sizeof fo->release_err,
                 "thermal_zone%d was NOT handed back to the kernel (writing mode=%s was "
                 "refused: %s)"
                 "%s - nothing is managing the fan now. Re-enable it with: "
                 "echo %s > /sys/class/thermal/thermal_zone%d/mode",
                 fo->zone_idx, fo->zone_mode_before, strerror(zone_errno),
                 fan_failed ? ", and the fan state write was refused too" : "",
                 fo->zone_mode_before, fo->zone_idx);
    else if (fan_failed)
        snprintf(fo->release_err, sizeof fo->release_err,
                 "the zone was handed back, but writing cooling_device%d/cur_state was "
                 "refused (%s); the fan is left as it was, and the kernel takes it over "
                 "at its next trip crossing", fo->cdev_idx, strerror(fan_errno));

    return (zone_failed || fan_failed) ? -1 : 0;
}
