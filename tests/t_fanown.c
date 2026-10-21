// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer
// SPDX-FileContributor: Claude Opus 5 (Anthropic), paired on the behavioural suite
//
// Handing the fan back - the DECISION, not the act. Both fan defects of 2026-10-17 were
// decided here: a release that wrote max(driven, found) and never lowered, and a watchdog
// that corrected a zone it did not hold. The two thresholds involved are the same number
// used for opposite questions, which is exactly the confusion these checks exist to prevent.
//
// What this file does NOT reach: fanown_take(), _release(), _set(), _state() and
// _correct_stale() all touch sysfs, so they are exercised against a fixture tree by
// tests/sh/t_daemon.sh (sections A-D) instead. Only fanown_release_mode(),
// fanown_clear_point() and the cooling-map reader are tested here.
//
// The zone is built in this file rather than captured: the shapes that matter - no active
// trip, no readable cooling map, a map that reads - are states this board cannot all be in.

#define _GNU_SOURCE
#include "../src/fanown.h"
#include "../src/io.h"
#include "t.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char dir[64];

/* The board's own zone: two passive trips well out of the way and one active trip at 38C
 * with 3C of hysteresis, which is the only active trip on this hardware. */
static void zone_as_shipped(struct surface *s)
{
    memset(s, 0, sizeof *s);
    s->nzone = 1;
    snprintf(s->zone[0].type, sizeof s->zone[0].type, "cpuss-0-0-thermal");
    s->zone[0].idx = 37;
    s->zone[0].ntrips = 3;
    snprintf(s->zone[0].trips[0].type, sizeof s->zone[0].trips[0].type, "passive");
    s->zone[0].trips[0].temp_mc = 135000; s->zone[0].trips[0].hyst_mc = 1000;
    snprintf(s->zone[0].trips[1].type, sizeof s->zone[0].trips[1].type, "passive");
    s->zone[0].trips[1].temp_mc = 130000; s->zone[0].trips[1].hyst_mc = 5000;
    snprintf(s->zone[0].trips[2].type, sizeof s->zone[0].trips[2].type, "active");
    s->zone[0].trips[2].temp_mc = 38000;  s->zone[0].trips[2].hyst_mc = 3000;
    s->zone[0].nbound = 1;
    s->zone[0].bound[0] = 7;
}

/* <phandle, min, max> in big-endian cells, as the device tree stores it. THERMAL_NO_LIMIT
 * is 0xFFFFFFFF, and the floor on this board is 4. */
static void write_cooling_map(const char *zone_type, const char *map, unsigned min)
{
    char path[512];
    unsigned char cell[12] = { 0, 0, 0, 0x2a,  0, 0, 0, 0,  0xff, 0xff, 0xff, 0xff };
    FILE *f;

    cell[4] = (unsigned char)(min >> 24); cell[5] = (unsigned char)(min >> 16);
    cell[6] = (unsigned char)(min >> 8);  cell[7] = (unsigned char)min;

    snprintf(path, sizeof path,
             "mkdir -p '%s/proc/device-tree/thermal-zones/%s/cooling-maps/%s'",
             dir, zone_type, map);
    if (system(path) != 0) {
        T_FAIL("%s", "cannot create the cooling-map fixture");
        return;
    }
    snprintf(path, sizeof path,
             "%s/proc/device-tree/thermal-zones/%s/cooling-maps/%s/cooling-device",
             dir, zone_type, map);
    if ((f = fopen(path, "wb")) == NULL) {
        T_FAIL("%s", "cannot write the cooling-device cell");
        return;
    }
    fwrite(cell, 1, sizeof cell, f);
    fclose(f);
}

/* Each test states the device tree it needs. Without this, a map left behind by an earlier
 * case would make "at or above the trip is the floor" pass against a floor it did not write
 * - proving nothing about its own tree - and would make "no readable floor" fail outright,
 * because a readable map is the opposite of what that case needs. */
static void remove_cooling_maps(void)
{
    char cmd[256];

    snprintf(cmd, sizeof cmd, "rm -rf '%s/proc'", dir);
    if (system(cmd) != 0)
        T_FAIL("%s", "cannot clear the device-tree fixture");
}

static void t_clear_point_is_the_trip_minus_hysteresis(void)
{
    struct surface s;
    long mc = 0;

    zone_as_shipped(&s);
    T_EQ_INT(fanown_active_trip(&s, "cpuss-0-0", &mc), 1, "the active trip is found");
    T_EQ_INT(mc, 38000, "by the short zone spelling, and it is the LOWEST active one");
    mc = 0;
    T_EQ_INT(fanown_clear_point(&s, "cpuss-0-0-thermal", &mc), 1,
             "and so is the clear point, by the full spelling");
    T_EQ_INT(mc, 35000, "which is the trip minus its hysteresis");

    /* A zone with no active trip answers neither question, which is what stops the
     * watchdog acting on a zone whose fan the kernel never drives. */
    s.zone[0].ntrips = 2;
    T_EQ_INT(fanown_active_trip(&s, "cpuss-0-0", &mc), 0, "no active trip: no trip");
    T_EQ_INT(fanown_clear_point(&s, "cpuss-0-0", &mc), 0, "no active trip: no clear point");
}

static void t_release_below_the_trip_is_zero(void)
{
    struct surface s;
    int want = -1;

    zone_as_shipped(&s);
    remove_cooling_maps();
    write_cooling_map("cpuss-0-0-thermal", "map-active", 4);
    io_set_sysroot(dir);

    /* Below the active trip the kernel asks for no active cooling, and the next upward
     * crossing raises the fan to the map's floor by itself. Writing 0 is what the
     * governor itself holds there - measured on the board. */
    T_EQ_INT(fanown_release_mode(&s, "cpuss-0-0", 1, 34000, &want), FANOWN_LEAVE_AS_IS,
             "below the trip the caller sets a state");
    T_EQ_INT(want, 0, "and the state is 0");

    /* Inside the hysteresis band is still BELOW the trip, and that is the distinction the
     * pinned-fan defect got wrong: comparing against trip-minus-hysteresis here left the
     * fan at maximum across the whole 35-38C band, which is where this board idles. */
    want = -1;
    T_EQ_INT(fanown_release_mode(&s, "cpuss-0-0", 1, 37500, &want), FANOWN_LEAVE_AS_IS,
             "37.5C is inside the band but below the trip");
    T_EQ_INT(want, 0, "so the answer is still 0, not maximum");
    io_set_sysroot("");
}

static void t_release_at_or_above_the_trip_is_the_floor(void)
{
    struct surface s;
    int want = -1;

    zone_as_shipped(&s);
    remove_cooling_maps();
    write_cooling_map("cpuss-0-0-thermal", "map-active", 4);
    io_set_sysroot(dir);

    T_EQ_INT(fanown_release_mode(&s, "cpuss-0-0", 1, 38000, &want), FANOWN_LEAVE_AS_IS,
             "at the trip the caller sets a state");
    T_EQ_INT(want, 4, "and it is the cooling map's lower limit, read from the device tree");

    want = -1;
    T_EQ_INT(fanown_release_mode(&s, "cpuss-0-0", 1, 41600, &want), FANOWN_LEAVE_AS_IS,
             "and well above it too");
    T_EQ_INT(want, 4, "still the floor, not maximum");
    io_set_sysroot("");
}

static void t_a_write_is_clamped_to_the_device(void)
{
    struct fanown fo;
    char path[512], buf[32];
    FILE *f;

    /* fanown_set is the one place every fan write in this project passes through, and it
     * used to write whatever it was handed. A state above the device's maximum is refused by
     * the kernel with EINVAL, so an over-maximum command became a write that failed at every
     * tick with nothing above it noticing. The config is where an operator should find out -
     * fanconfig_validate refuses it now - and this is the backstop. */
    memset(&fo, 0, sizeof fo);
    fo.cdev_idx = 7;
    fo.max_state = 4;
    fo.owned = 1;

    snprintf(path, sizeof path, "%s/sys/class/thermal/cooling_device7", dir);
    {
        char cmd[640];
        snprintf(cmd, sizeof cmd, "mkdir -p '%s'", path);
        if (system(cmd) != 0) {
            T_FAIL("%s", "cannot create the cooling device fixture");
            return;
        }
    }
    snprintf(path, sizeof path, "%s/sys/class/thermal/cooling_device7/cur_state", dir);
    if ((f = fopen(path, "w")) == NULL) {
        T_FAIL("%s", "cannot create cur_state");
        return;
    }
    fputs("0\n", f);
    fclose(f);

    io_set_sysroot(dir);
    T_EQ_INT(fanown_set(&fo, 9), 0, "a state above the device's maximum is accepted");
    io_set_sysroot("");
    if ((f = fopen(path, "r")) != NULL) {
        if (!fgets(buf, sizeof buf, f))
            buf[0] = '\0';
        fclose(f);
        buf[strcspn(buf, "\n")] = '\0';
        T_EQ_STR(buf, "4", "but what lands in the device is its own maximum");
    } else {
        T_FAIL("%s", "cannot read cur_state back");
    }
    T_EQ_INT(fo.last_state, 4, "and the recorded state is the clamped one");

    /* A state the device CAN take is written unchanged, so the clamp is a bound and not a
     * ceiling applied to everything. */
    io_set_sysroot(dir);
    T_EQ_INT(fanown_set(&fo, 3), 0, "a state it can take is written");
    io_set_sysroot("");
    if ((f = fopen(path, "r")) != NULL) {
        if (!fgets(buf, sizeof buf, f))
            buf[0] = '\0';
        fclose(f);
        buf[strcspn(buf, "\n")] = '\0';
        T_EQ_STR(buf, "3", "unchanged");
    }

    /* A negative state is not a request for anything: 0 is off. */
    io_set_sysroot(dir);
    T_EQ_INT(fanown_set(&fo, -2), 0, "a negative state is accepted");
    io_set_sysroot("");
    if ((f = fopen(path, "r")) != NULL) {
        if (!fgets(buf, sizeof buf, f))
            buf[0] = '\0';
        fclose(f);
        buf[strcspn(buf, "\n")] = '\0';
        T_EQ_STR(buf, "0", "and becomes off, not a negative write");
    }
}

static void t_the_lowest_active_trip_is_the_one(void)
{
    struct surface s;
    long mc = 0;
    int want = -1;

    /* This board has exactly one active trip, so "the LOWEST active one" was a claim about
     * a selection among one - and flipping the comparison to pick the HIGHEST passed every
     * check. A zone with two is a shape this board cannot be in, which is the whole reason
     * this file builds its zones rather than capturing them.
     *
     * It matters because the release threshold and the watchdog's clear point are both
     * derived from this trip: choosing the wrong one moves both, silently, by 7 degrees. */
    zone_as_shipped(&s);
    s.zone[0].ntrips = 4;
    snprintf(s.zone[0].trips[3].type, sizeof s.zone[0].trips[3].type, "active");
    s.zone[0].trips[3].temp_mc = 45000;
    s.zone[0].trips[3].hyst_mc = 2000;

    T_EQ_INT(fanown_active_trip(&s, "cpuss-0-0", &mc), 1, "with two active trips, one wins");
    T_EQ_INT(mc, 38000, "and it is the LOWER of them, not the higher");

    /* The order they appear in must not decide it either: the kernel numbers trips by
     * index, not by temperature, so a zone could list them the other way round. */
    s.zone[0].trips[2].temp_mc = 45000;
    s.zone[0].trips[2].hyst_mc = 2000;
    s.zone[0].trips[3].temp_mc = 38000;
    s.zone[0].trips[3].hyst_mc = 3000;
    mc = 0;
    T_EQ_INT(fanown_active_trip(&s, "cpuss-0-0", &mc), 1, "listed the other way round too");
    T_EQ_INT(mc, 38000, "the lower one still wins");

    /* And the release decision follows it: at 40C the board is above the lower trip and
     * below the higher, so a release must write the map's floor rather than 0. Keying on
     * the higher trip would hand back a stopped fan at 40C. */
    write_cooling_map("cpuss-0-0-thermal", "map-active", 4);
    io_set_sysroot(dir);
    T_EQ_INT(fanown_release_mode(&s, "cpuss-0-0", 1, 40000, &want), FANOWN_LEAVE_AS_IS,
             "a release between the two trips names a state");
    T_EQ_INT(want, 4, "the floor, because 40C is above the LOWER active trip");
    io_set_sysroot("");
}

static void t_release_without_information_leaves_maximum(void)
{
    struct surface s;
    int want = -1;

    zone_as_shipped(&s);
    remove_cooling_maps();
    io_set_sysroot(dir);

    /* No temperature: more cooling is the safe guess, and the CLI takes it because it has
     * just driven the fan to maximum on purpose. The daemon converts this to its own
     * never-lower handback instead, which is why the rule lives in the caller. */
    /* Seeded with a value the function must overwrite. It used to be seeded with -1, the
     * same value expected back, so a function that never touched *out_state passed - and
     * clearing it is part of the contract in fanown.h, relied on by any caller that does
     * not pre-initialise. */
    want = 7;
    T_EQ_INT(fanown_release_mode(&s, "cpuss-0-0", 0, 0, &want), FANOWN_LEAVE_MAX,
             "no temperature: leave maximum");
    T_EQ_INT(want, -1, "and the caller's state is cleared, not left as it found it");

    /* A temperature at the trip but no readable cooling map - no device tree here - is the
     * same answer for the same reason. */
    want = 7;
    T_EQ_INT(fanown_release_mode(&s, "cpuss-0-0", 1, 40000, &want), FANOWN_LEAVE_MAX,
             "no readable floor: leave maximum");
    T_EQ_INT(want, -1, "and names no state here either");

    /* And a zone this surface does not have at all. */
    want = 7;
    T_EQ_INT(fanown_release_mode(&s, "no-such-zone", 1, 34000, &want), FANOWN_LEAVE_MAX,
             "an unknown zone: leave maximum");
    T_EQ_INT(want, -1, "with nothing named to write");
    io_set_sysroot("");
}

static void t_the_floor_reader_takes_the_highest(void)
{
    struct surface s;
    int want = -1;

    /* One map with one cooling device is all this board has, so the rule that the highest
     * floor wins has never been exercised on hardware. A zone binding several devices is
     * an open item; this at least pins the reader's half of it. */
    zone_as_shipped(&s);
    remove_cooling_maps();
    write_cooling_map("cpuss-0-0-thermal", "map-active", 4);
    write_cooling_map("cpuss-0-0-thermal", "map-active-2", 6);
    io_set_sysroot(dir);
    T_EQ_INT(fanown_release_mode(&s, "cpuss-0-0", 1, 40000, &want), FANOWN_LEAVE_AS_IS,
             "two maps are both read");
    T_EQ_INT(want, 6, "and the most cooling any of them asks for is what is left behind");
    io_set_sysroot("");
}

int main(void)
{
    char cmd[128];

    snprintf(dir, sizeof dir, "/tmp/qcctl-t_fanown.%d", (int)getpid());
    snprintf(cmd, sizeof cmd, "mkdir -p '%s'", dir);
    if (system(cmd) != 0) {
        fprintf(stderr, "cannot create %s\n", dir);
        return 2;
    }

    t_clear_point_is_the_trip_minus_hysteresis();
    t_release_below_the_trip_is_zero();
    t_release_at_or_above_the_trip_is_the_floor();
    t_a_write_is_clamped_to_the_device();
    t_the_lowest_active_trip_is_the_one();
    t_release_without_information_leaves_maximum();
    t_the_floor_reader_takes_the_highest();

    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0)
        fprintf(stderr, "warning: %s was left behind\n", dir);
    return t_summary("fanown");
}
