// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer

#define _GNU_SOURCE
#include "fanown.h"
#include "io.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define THERMAL_DIR "/sys/class/thermal"
/* One <phandle, min, max> entry of a cooling-map, in device-tree cells. */
#define CMAP_CELL 12

static int lowest_active_trip(const struct zone *z);
static int hold_write(const struct fanown *fo);
static void hold_clear(void);

int fanown_take(struct fanown *fo, const struct surface *s,
                const char *zone_name, const char *cdev_name,
                char *err, size_t errlen)
{
    const struct zone *z = surface_zone(s, zone_name);
    const struct cdev *cd = surface_cdev(s, cdev_name);
    char path[PATH_MAXLEN], val[24];

    memset(fo, 0, sizeof *fo);
    fo->last_state = -1;
    fo->trip_idx = -1;

    if (!z)  { snprintf(err, errlen, "no zone '%s'", zone_name); return -1; }
    if (!cd) { snprintf(err, errlen, "no cooling device '%s'", cdev_name); return -1; }
    fo->zone_idx = z->idx;
    fo->cdev_idx = cd->idx;
    snprintf(fo->zone, sizeof fo->zone, "%s", zone_name);

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

    /* Park the active trip while the kernel is STILL managing this zone: the trip write is
     * what re-programs the sensor's threshold window, and the same write to an already
     * disabled zone does not reach it. Only when every other trip on the zone sits above
     * the park value, so this can neither reorder the trips nor mask a passive limit.
     *
     * A refused park is not a reason to refuse the take: what it avoids is an interrupt
     * storm, which costs a core and threatens nothing. The zone is taken either way. */
    {
        int ti = lowest_active_trip(z);
        int safe = ti >= 0;

        for (int i = 0; safe && i < z->ntrips; i++)
            if (i != ti && z->trips[i].temp_mc <= FANOWN_PARK_MC)
                safe = 0;
        if (safe) {
            spath(path, sizeof path, THERMAL_DIR "/thermal_zone%d/trip_point_%d_temp",
                  z->idx, z->trips[ti].idx);
            snprintf(val, sizeof val, "%d", FANOWN_PARK_MC);
            if (write_path_str(path, val) == 0) {
                fo->trip_idx       = z->trips[ti].idx;
                fo->trip_before_mc = z->trips[ti].temp_mc;
                fo->trip_parked    = 1;
            }
        }
    }

    spath(path, sizeof path, THERMAL_DIR "/thermal_zone%d/mode", z->idx);
    if (write_path_str(path, "disabled") < 0) {
        /* The park above must not outlive a take that failed here. */
        if (fo->trip_parked) {
            spath(path, sizeof path, THERMAL_DIR "/thermal_zone%d/trip_point_%d_temp",
                  z->idx, fo->trip_idx);
            snprintf(val, sizeof val, "%ld", fo->trip_before_mc);
            (void)write_path_str(path, val);
            fo->trip_parked = 0;
        }
        snprintf(err, errlen, "cannot disable zone %d: %s", z->idx, strerror(errno));
        return -1;
    }
    fo->owned = 1;
    (void)hold_write(fo);      /* best effort: a missing record costs a crash, not a board */
    fo->max_state = cd->state_ok ? (int)cd->max_state : 10;
    fo->binding = surface_zone_binds(s, z, cd);
    return 0;
}

/* Index of the lowest `active` trip, or -1. Both answers below come from this one scan. */
static int lowest_active_trip(const struct zone *z)
{
    int best = -1;

    if (!z)
        return -1;
    for (int i = 0; i < z->ntrips; i++) {
        if (strcmp(z->trips[i].type, "active"))
            continue;
        if (best < 0 || z->trips[i].temp_mc < z->trips[best].temp_mc)
            best = i;
    }
    return best;
}

int fanown_active_trip(const struct surface *s, const char *zone_name, long *out_trip_mc)
{
    const struct zone *z = surface_zone(s, zone_name);
    int i = lowest_active_trip(z);

    if (i < 0)
        return 0;
    if (out_trip_mc)
        *out_trip_mc = z->trips[i].temp_mc;
    return 1;
}

int fanown_clear_point(const struct surface *s, const char *zone_name, long *out_mc)
{
    const struct zone *z = surface_zone(s, zone_name);
    int i = lowest_active_trip(z);

    if (i < 0)
        return 0;
    if (out_mc)
        *out_mc = z->trips[i].temp_mc - z->trips[i].hyst_mc;
    return 1;
}

/* The cooling map's lower limit: the state the kernel itself brings this zone's fan to
 * when the active trip fires. sysfs exposes no thermal instance bounds, so it comes from
 * the device tree, where the property is <phandle, min, max> in big-endian cells. Returns
 * -1 if it cannot be read, which the caller reads as "leave cooling at maximum".
 *
 * A map may bind SEVERAL cooling devices, and then no entry can be attributed to the fan:
 * each names its device by phandle, and nothing reachable from a cooling_device in sysfs
 * identifies its device-tree node - measured. Reading the first entry would hand the fan a
 * floor belonging to a CPU or the GPU, so such a zone is reported unknown instead. */
static int cooling_map_lower(const struct zone *z)
{
    char dir[PATH_MAXLEN], path[PATH_MAXLEN];
    struct dirent *e;
    DIR *d;
    int lower = -1;

    spath(dir, sizeof dir, "/proc/device-tree/thermal-zones/%.64s/cooling-maps", z->type);
    d = opendir(dir);
    if (!d)
        return -1;
    while ((e = readdir(d))) {
        unsigned char cell[CMAP_CELL + 1];      /* one byte more than an entry, to see it */
        size_t n;
        FILE *f;

        if (e->d_name[0] == '.')
            continue;
        if (snprintf(path, sizeof path, "%s/%.64s/cooling-device", dir, e->d_name)
            >= (int)sizeof path)
            continue;
        f = fopen(path, "rb");
        if (!f)
            continue;
        n = fread(cell, 1, sizeof cell, f);
        fclose(f);
        /* Exactly one entry, or this zone has no floor anyone can name - and one such map
         * decides the zone, because the highest of the LEGIBLE floors is not the zone's
         * floor. A length that completes no entry is refused for the same reason: its
         * first 12 bytes read as a perfectly good state. */
        if (n != CMAP_CELL) {
            closedir(d);
            return -1;
        }
        {
            long min = ((long)cell[4] << 24) | ((long)cell[5] << 16) |
                       ((long)cell[6] << 8)  | (long)cell[7];
            /* Highest floor wins: if several maps drive this zone, the most cooling any of
             * them asks for is the safe one to leave behind. */
            if (min >= 0 && min <= 64 && min > lower)
                lower = (int)min;
        }
    }
    closedir(d);
    return lower;
}

enum fanown_release fanown_release_mode(const struct surface *s, const char *zone_name,
                                        int have_temp, long temp_mc, int *out_state)
{
    const struct zone *z = surface_zone(s, zone_name);
    long trip_mc = 0;
    int lower;

    if (out_state)
        *out_state = -1;
    if (!have_temp || !z || !fanown_active_trip(s, zone_name, &trip_mc))
        return FANOWN_LEAVE_MAX;
    if (temp_mc < trip_mc) {
        /* Trip clear: the kernel asks for no active cooling, and the next upward crossing
         * raises the fan to the map's floor by itself - measured. */
        if (out_state)
            *out_state = 0;
        return FANOWN_LEAVE_AS_IS;
    }
    lower = cooling_map_lower(z);
    if (lower < 0)
        return FANOWN_LEAVE_MAX;    /* floor unknown: more cooling is the safe guess */
    if (out_state)
        *out_state = lower;
    return FANOWN_LEAVE_AS_IS;
}

int fanown_correct_stale(const struct surface *s, const char *zone_name, long temp_mc,
                         long *out_clear_mc, int *out_state)
{
    const struct zone *z = surface_zone(s, zone_name);
    char path[PATH_MAXLEN], mode[NAME_LEN];
    uint64_t state = 0;
    long clear_mc = 0;

    if (!z || z->nbound != 1)
        return 0;
    /* The zone's mode is the lock, and this corrects a zone the KERNEL holds. Disabled
     * means someone else has taken it - a --fan-baseline capture, or --cool-max - and the
     * fan is theirs to drive until they hand it back. Measured: without this, a capture
     * lost a state's rpm to a correction landing mid-measurement. */
    spath(path, sizeof path, THERMAL_DIR "/thermal_zone%d/mode", z->idx);
    if (read_file_str(path, mode, sizeof mode) < 0 || strcmp(mode, "enabled") != 0)
        return 0;
    if (!fanown_clear_point(s, zone_name, &clear_mc) || temp_mc >= clear_mc)
        return 0;
    spath(path, sizeof path, THERMAL_DIR "/cooling_device%d/cur_state", z->bound[0]);
    if (read_file_u64(path, &state) < 0)
        return -1;
    if (state == 0)
        return 0;
    if (out_clear_mc)
        *out_clear_mc = clear_mc;
    if (out_state)
        *out_state = (int)state;
    return write_path_str(path, "0") < 0 ? -1 : 1;
}

int fanown_state(const struct fanown *fo, int *out_state)
{
    char path[PATH_MAXLEN];
    uint64_t v = 0;

    spath(path, sizeof path, THERMAL_DIR "/cooling_device%d/cur_state", fo->cdev_idx);
    if (read_file_u64(path, &v) < 0)
        return -1;
    if (out_state)
        *out_state = (int)v;
    return 0;
}

int fanown_set(struct fanown *fo, int state)
{
    char path[PATH_MAXLEN], val[16];

    /* CLAMPED TO THE DEVICE. fo->max_state is read from the cooling device at take time, and
     * this is the one place every fan write in this project passes through, so bounding it
     * here bounds all of them. A state above max_state is refused by the kernel with EINVAL,
     * so the unclamped version turned a config mistake into a write that failed at every
     * tick - visible only in the journal, and only if someone looked.
     *
     * The config is still the place to catch it: fanconfig_validate refuses a strategy whose
     * ceiling exceeds the named device, which is where an operator finds out. This is the
     * backstop for everything that does not come from a validated config. */
    if (fo->max_state > 0 && state > fo->max_state)
        state = fo->max_state;
    if (state < 0)
        state = 0;

    fo->last_state = state;
    spath(path, sizeof path, THERMAL_DIR "/cooling_device%d/cur_state", fo->cdev_idx);
    snprintf(val, sizeof val, "%d", state);
    return write_path_str(path, val);
}

/*
 * The parked-trip record.
 *
 * A release puts the trip back, and so does a failed take - but neither runs if the process
 * is killed outright. The value then survives nowhere, and the next take would read the park
 * value as though the board had authored it and restore THAT on release, making one crash
 * permanent. A reboot clears the park on its own, since sysfs trips are not persistent;
 * this is for everything short of one.
 */
static char hold_path[PATH_MAXLEN];

void fanown_set_hold_path(const char *path)
{
    snprintf(hold_path, sizeof hold_path, "%s", path ? path : "");
}

static int hold_write(const struct fanown *fo)
{
    FILE *f;

    if (!fo->trip_parked || !hold_path[0])
        return 0;
    if ((f = fopen(hold_path, "w")) == NULL)
        return -1;
    fprintf(f, "# qcctl: a thermal trip parked while this tool owns the zone\n");
    fprintf(f, "zone %d\ntrip %d\norig %ld\npark %d\n",
            fo->zone_idx, fo->trip_idx, fo->trip_before_mc, FANOWN_PARK_MC);
    return (fclose(f) == 0) ? 1 : -1;
}

static void hold_clear(void)
{
    if (hold_path[0])
        (void)unlink(hold_path);
}

int fanown_recover_hold(const char *path, char *err, size_t errlen)
{
    char line[128], cur[32], tpath[PATH_MAXLEN], val[24];
    int zone = -1, trip = -1, park = -1;
    long orig = -1;
    FILE *f;

    if ((f = fopen(path, "r")) == NULL)
        return 0;                               /* nothing held: the usual case */
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "zone %d", &zone) == 1) continue;
        if (sscanf(line, "trip %d", &trip) == 1) continue;
        if (sscanf(line, "orig %ld", &orig) == 1) continue;
        if (sscanf(line, "park %d", &park) == 1) continue;
    }
    fclose(f);

    if (zone < 0 || trip < 0 || orig < 0 || park < 0) {
        snprintf(err, errlen, "%s is unreadable or incomplete; remove it by hand", path);
        return -1;
    }

    /* Only put back what is still parked. A reboot re-authors the trip from the device
     * tree, and after one the recorded value is already in place - or the zone has been
     * renumbered and this trip belongs to something else entirely, which is the case that
     * makes an unconditional write dangerous. Zone indices are not stable across boots. */
    spath(tpath, sizeof tpath, THERMAL_DIR "/thermal_zone%d/trip_point_%d_temp", zone, trip);
    if (read_file_str(tpath, cur, sizeof cur) < 0) {
        hold_clear();
        return 0;
    }
    if (atol(cur) != park) {
        hold_clear();
        return 0;
    }

    snprintf(val, sizeof val, "%ld", orig);
    if (write_path_str(tpath, val) < 0) {
        snprintf(err, errlen,
                 "thermal_zone%d trip %d is still parked at %d mC and cannot be put back "
                 "to %ld mC (%s) - the kernel will not act on this zone's fan below %d mC "
                 "until it is",
                 zone, trip, park, orig, strerror(errno), park);
        return -1;
    }
    (void)unlink(path);
    return 1;
}

int fanown_release(struct fanown *fo, enum fanown_release how)
{
    char path[PATH_MAXLEN], v[8];
    int fan_failed = 0, zone_failed = 0, fan_errno = 0, zone_errno = 0;
    int trip_failed = 0;

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

    /* Before the mode write, not after: writing mode is the one evaluation the kernel
     * makes here, and it arms the sensor from whatever the trips say at that moment. Put
     * them back first and the zone returns armed as the board authored it. */
    if (fo->trip_parked) {
        char tv[24];

        spath(path, sizeof path, THERMAL_DIR "/thermal_zone%d/trip_point_%d_temp",
              fo->zone_idx, fo->trip_idx);
        snprintf(tv, sizeof tv, "%ld", fo->trip_before_mc);
        if (write_path_str(path, tv) < 0)
            trip_failed = 1;
        else {
            fo->trip_parked = 0;
            hold_clear();
        }
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

    return (zone_failed || fan_failed || trip_failed) ? -1 : 0;
}
