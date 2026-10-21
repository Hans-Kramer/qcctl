// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer
// SPDX-FileContributor: Claude Opus 5 (Anthropic), paired on the behavioural suite
//
// Discovery against a captured board. Everything this project decides rests on what
// surface_discover() found, and until now that function had never been run anywhere but on
// hardware - where a wrong answer looks like a working one.
//
// Expectations are read OUT of the fixture wherever a value could drift, so a re-capture does
// not invalidate the test: what is pinned is the mapping from files to a surface, not the
// weather. The counts and the shapes are the board's, and those are stable.

#define _GNU_SOURCE
#include "../src/surface.h"
#include "../src/fanown.h"
#include "../src/io.h"
#include "t.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIXTURE "tests/fixtures/monza-20260710-235-6.8.0-1084"

static long fixture_long(const char *rel)
{
    char path[512], buf[64];
    FILE *f;
    long v = -1;

    snprintf(path, sizeof path, "%s/%s", FIXTURE, rel);
    if ((f = fopen(path, "r")) == NULL)
        return -1;
    if (fgets(buf, sizeof buf, f))
        v = strtol(buf, NULL, 10);
    fclose(f);
    return v;
}

/* Count whitespace-separated tokens on the FIRST line of a fixture file, up to `cap`: the
 * governor and OPP lists are space-separated, and their LENGTH is what discovery has to get
 * right.
 *
 * Both limits matter and neither was here. The code reads one line - read_file_str() stops
 * at the first newline - so counting further would expect more than discovery can see. And
 * the code stores at most MAX_OPP / MAX_GOVERNORS, so an uncapped count would expect more
 * than it can hold: today's fixture is far inside both, but the comment below promises a
 * re-capture cannot invalidate this test, and an uncapped count is exactly how it could. */
static int fixture_tokens(const char *rel, int cap)
{
    char path[512], line[1024], *p;
    FILE *f;
    int n = 0;

    snprintf(path, sizeof path, "%s/%s", FIXTURE, rel);
    if ((f = fopen(path, "r")) == NULL)
        return -1;
    if (fgets(line, sizeof line, f) == NULL)
        line[0] = '\0';
    fclose(f);
    for (p = strtok(line, " \t\n"); p; p = strtok(NULL, " \t\n"))
        if (++n == cap)
            break;
    return n;
}

/* How many entries of a kind the capture holds, e.g. thermal_zone* or cooling_device*. */
static int fixture_entries(const char *subdir, const char *prefix)
{
    char cmd[640], buf[64];
    FILE *p;
    int n = -1;

    snprintf(cmd, sizeof cmd,
             "ls -d '%s/%s/%s'* 2>/dev/null | wc -l", FIXTURE, subdir, prefix);
    if ((p = popen(cmd, "r")) == NULL)
        return -1;
    if (fgets(buf, sizeof buf, p))
        n = atoi(buf);
    pclose(p);
    return n;
}

static struct surface s;

static void discover(void)
{
    io_set_sysroot(FIXTURE);
    surface_discover(&s);
    io_set_sysroot("");
}

static void t_the_board_this_project_was_written_for(void)
{
    /* Three policies, which is the fact the whole project turns on: keying on cpu0 would
     * miss two thirds of the machine, and the vendor's own defaults file names a policy7
     * that does not exist. */
    T_EQ_INT(s.npol, 3, "three cpufreq policies");
    T_EQ_INT(s.pol[0].id, 0, "policy0");
    T_EQ_INT(s.pol[1].id, 2, "policy2 - the one the vendor leaves on the wrong governor");
    T_EQ_INT(s.pol[2].id, 4, "policy4");
    T_TRUE(surface_policy(&s, 7) == NULL, "and no policy7, whatever the vendor file says");

    T_EQ_INT(s.pol[0].ncpus, 2, "policy0 drives two cores");
    T_EQ_INT(s.pol[2].ncpus, 4, "and policy4 drives four");

    /* Governors and operating points are counted out of the fixture rather than pinned: a
     * kernel bump can legitimately change both, and what must not change is that discovery
     * reads every one of them. An OPP list read short is how a valid frequency gets refused. */
    T_EQ_INT(s.pol[0].ngov,
             fixture_tokens("sys/devices/system/cpu/cpufreq/policy0/"
                            "scaling_available_governors", MAX_GOVERNORS),
             "every governor the policy offers is discovered");
    T_EQ_INT(s.pol[0].nopp,
             fixture_tokens("sys/devices/system/cpu/cpufreq/policy0/"
                            "scaling_available_frequencies", MAX_OPP),
             "and every operating point");
    T_TRUE(s.pol[0].nopp > 1, "of which there are several, not one");
    T_EQ_INT((int)s.pol[0].cpuinfo_max_khz,
             (int)fixture_long("sys/devices/system/cpu/cpufreq/policy0/cpuinfo_max_freq"),
             "with the hardware ceiling the board reports");
}

static void t_the_gpu_and_the_hwmon_nodes_are_found(void)
{
    /* Both were asserted only on the EMPTY sysroot, where absence is the right answer and
     * so proves nothing: skipping find_gpu() entirely, or making hwmon_find() never match,
     * passed every check. io.h calls the name-not-index resolution essential, because hwmon
     * numbering is assigned at probe time and a hardcoded index reads another chip. */
    T_EQ_INT(s.gpu.present, 1, "the GPU is discovered on a board that has one");
    T_TRUE(s.gpu.nopp > 1, "with its operating points");
    T_TRUE(s.gpu.ngov > 1, "and its governors");

    T_TRUE(s.hwmon_fan[0] != '\0', "the fan's hwmon node is resolved");
    T_HAS(s.hwmon_fan, "hwmon", "to an hwmon path");
    /* Resolved BY NAME: the captured board has the fan on hwmon0, and a reader that
     * assumed an index would be right here by luck - so the test also asserts the node it
     * picked actually carries the fan's own attributes. */
    {
        char path[512];
        FILE *f;

        /* hwmon_fan comes back ALREADY sysroot-prefixed - io.h's note that these paths
         * are returned that way - so it is used as it is, not prefixed again. */
        snprintf(path, sizeof path, "%s/pwm1", s.hwmon_fan);
        f = fopen(path, "r");
        T_TRUE(f != NULL, "and that node is the one with the fan's pwm attribute on it");
        if (f)
            fclose(f);
    }
}

static void t_zones_and_the_fan(void)
{
    const struct zone *z = surface_zone(&s, "cpuss-0-0");
    const struct cdev *fan = surface_cdev(&s, "emc2305_fan");

    /* Counted out of the capture like every other count in this file. Pinned at a literal
     * 48, this was the one assertion that contradicted the promise at the top: a re-capture
     * on a kernel that exposes one more zone would have failed a correct discovery. */
    T_EQ_INT(s.nzone, fixture_entries("sys/class/thermal", "thermal_zone"),
             "every thermal zone in the capture is discovered");
    T_TRUE(s.nzone > 40, "and this board has dozens of them, not a handful");
    T_EQ_INT(s.ncdev, fixture_entries("sys/class/thermal", "cooling_device"),
             "and every cooling device, which is what a map is resolved against");
    T_REQUIRE(z != NULL, "the fan's zone resolves by its short config spelling");
    T_TRUE(surface_zone(&s, "cpuss-0-0-thermal") == z,
           "and by the kernel's full type - the same zone either way");
    T_TRUE(surface_zone(&s, "no-such-zone") == NULL, "an unknown name resolves to nothing");

    T_REQUIRE(fan != NULL, "the fan's cooling device resolves by name, never by index");
    T_EQ_INT((int)fan->max_state, 10, "with eleven states, 0 to 10");

    /* The binding comes from the zone's cdevN symlink, which is why the capture keeps those
     * links rather than resolving them away. */
    T_EQ_INT(z->nbound, 1, "the zone drives exactly one cooling device");
    T_EQ_INT(z->bound[0], fan->idx, "and it is the fan");
    T_EQ_INT(surface_zone_binds(&s, z, fan), 1, "which the binding check confirms");

    /* Exactly one active trip exists on the whole board, and it is this zone's. */
    T_EQ_INT(z->ntrips, 3, "three trips on this zone");
    {
        int active = 0;
        long trip = 0, hyst = 0;

        for (int i = 0; i < z->ntrips; i++)
            if (strcmp(z->trips[i].type, "active") == 0) {
                active++;
                trip = z->trips[i].temp_mc;
                hyst = z->trips[i].hyst_mc;
            }
        T_EQ_INT(active, 1, "one of them active");
        T_EQ_INT(trip, fixture_long("sys/class/thermal/thermal_zone37/trip_point_2_temp"),
                 "at the temperature the fixture records");
        T_EQ_INT(hyst, fixture_long("sys/class/thermal/thermal_zone37/trip_point_2_hyst"),
                 "with the hysteresis it records");
    }
}

static void t_the_manifest_supplies_what_a_copy_cannot(void)
{
    const struct policy *p = surface_policy(&s, 0);
    const struct zone *z = surface_zone(&s, "cpuss-0-0");

    /* This is the manifest's whole purpose. A copied tree is owned by whoever ran the
     * capture, with whatever mode the copy produced; knob_access() answers from the OWNER
     * bits, so without the real modes every control point would classify differently here
     * than on the board. */
    T_REQUIRE(p != NULL, "policy0 was discovered");
    T_REQUIRE(z != NULL, "and the fan's zone");
    T_EQ_INT(p->k_governor.mode, 0644, "the governor's mode is the board's 0644");
    T_EQ_STR(p->k_governor.owner, "root", "and its owner is root, not whoever ran the capture");
    T_EQ_INT(knob_access(&p->k_governor), KNOB_RW, "so it classifies as writable");

    T_EQ_INT(z->k_mode.mode, 0644, "the zone's mode attribute is 0644");
    T_EQ_INT(knob_access(&z->k_mode), KNOB_RW, "and writable - the lock this project takes");

    /* A read-only attribute must still classify read-only, or a plan would be built around
     * a write that cannot happen. */
    {
        const struct cdev *fan = surface_cdev(&s, "emc2305_fan");
        char path[256];
        int mode;

        T_REQUIRE(fan != NULL, "the fan's cooling device is discovered");
        snprintf(path, sizeof path, "%s/sys/class/thermal/cooling_device%d",
                 FIXTURE, fan->idx);
        io_set_sysroot(FIXTURE);
        mode = path_mode(path, "max_state");
        io_set_sysroot("");
        T_EQ_INT(mode, 0444, "max_state is 0444 on the board, and says so here");
    }
}

static void t_the_device_tree_the_kernel_does_not_export(void)
{
    long clear = 0;
    int want = -1;

    /* sysfs exposes no thermal instance bounds, so the cooling map's lower limit comes from
     * the device tree - and that is the number the whole release rule turns on. This runs
     * the real chain: discovery, the trip scan, and the big-endian cell in the capture. */
    io_set_sysroot(FIXTURE);
    T_EQ_INT(fanown_clear_point(&s, "cpuss-0-0", &clear), 1, "the clear point is computed");
    T_EQ_INT(clear,
             fixture_long("sys/class/thermal/thermal_zone37/trip_point_2_temp")
             - fixture_long("sys/class/thermal/thermal_zone37/trip_point_2_hyst"),
             "as the trip minus its hysteresis, both read from the capture");

    /* The enum is LEAVE_AS_IS on both sides of the trip, so it cannot tell the two branches
     * apart - the state does. The labels used to claim otherwise, which reads as though the
     * branch were covered by the line above the one that covers it. */
    want = 7;
    T_EQ_INT(fanown_release_mode(&s, "cpuss-0-0", 1, 34000, &want), FANOWN_LEAVE_AS_IS,
             "below the trip, a release names a state to write");
    T_EQ_INT(want, 0, "and below the trip that state is 0 - the fan off");

    want = 7;
    T_EQ_INT(fanown_release_mode(&s, "cpuss-0-0", 1, 41000, &want), FANOWN_LEAVE_AS_IS,
             "at or above it, a release also names a state");
    T_EQ_INT(want, 4, "but there it is the map's floor, which this device tree gives as 4");
    io_set_sysroot("");
}

static void t_a_sysroot_with_nothing_in_it(void)
{
    struct surface empty;

    /* What running on the wrong hardware looks like. Discovery must come back empty rather
     * than inventing a surface - every guard in the project is capability-based, and this is
     * the state those guards are written against. */
    io_set_sysroot("tests/fixtures/does-not-exist");
    surface_discover(&empty);
    io_set_sysroot("");

    T_EQ_INT(empty.npol, 0, "no policies");
    T_EQ_INT(empty.nzone, 0, "no zones");
    T_EQ_INT(empty.ncdev, 0, "no cooling devices");
    T_EQ_INT(empty.gpu.present, 0, "no GPU");
    T_TRUE(surface_zone(&empty, "cpuss-0-0") == NULL, "and the fan's zone is absent");
}

int main(void)
{
    discover();
    t_the_board_this_project_was_written_for();
    t_the_gpu_and_the_hwmon_nodes_are_found();
    t_zones_and_the_fan();
    t_the_manifest_supplies_what_a_copy_cannot();
    t_the_device_tree_the_kernel_does_not_export();
    t_a_sysroot_with_nothing_in_it();
    return t_summary("surface");
}
