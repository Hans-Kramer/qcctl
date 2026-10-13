// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer
//
// Runtime discovery of the control surface.

#define _GNU_SOURCE
#include "surface.h"

#include <unistd.h>
#include "io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>

/* Split a whitespace-separated sysfs list into a fixed table. */
static int split_words(const char *s, char out[][NAME_LEN], int max)
{
    int n = 0;

    while (*s && n < max) {
        size_t len = 0;
        while (*s == ' ' || *s == '\t')
            s++;
        while (s[len] && s[len] != ' ' && s[len] != '\t')
            len++;
        if (!len)
            break;
        if (len >= NAME_LEN)
            len = NAME_LEN - 1;
        memcpy(out[n], s, len);
        out[n][len] = '\0';
        n++;
        s += len;
    }
    return n;
}

static int split_u64(const char *s, uint64_t *out, int max)
{
    int n = 0;
    char *end;

    while (*s && n < max) {
        uint64_t v;
        errno = 0;
        v = strtoull(s, &end, 10);
        if (end == s)
            break;
        if (!errno)
            out[n++] = v;
        s = end;
    }
    return n;
}

/* Capture a knob: does it exist, could we write it, and what was it at boot.
 * All three are recorded because a --reset that guesses the default is a
 * --reset that silently changes the machine. */
static void knob_take(struct knob *k, const char *dir, const char *attr,
                      const char *report_path)
{
    snprintf(k->path, sizeof k->path, "%s", report_path);
    k->present = path_exists(dir, attr);
    k->mode    = k->present ? path_mode(dir, attr) : -1;
    k->owner[0] = '\0';
    if (k->present)
        (void)path_owner(dir, attr, k->owner, sizeof k->owner);
    k->boot[0] = '\0';
    /* A write-only node has no boot value to capture; that is recorded, not
     * papered over with an empty string that reads like a measurement. */
    if (k->present)
        (void)read_attr_str(dir, attr, k->boot, sizeof k->boot);
}

enum knob_access knob_access(const struct knob *k)
{
    if (!k->present)
        return KNOB_ABSENT;
    if (k->mode < 0)
        return KNOB_UNKNOWN;
    /* Owner bits decide: every control point here is root-owned, so what matters
     * is what root may do with it. */
    {
        int r = (k->mode & 0400) != 0, w = (k->mode & 0200) != 0;
        if (w && !r) return KNOB_WO;
        if (w)       return KNOB_RW;
        if (r)       return KNOB_RO;
    }
    return KNOB_UNKNOWN;
}

const char *knob_access_str(const struct knob *k)
{
    switch (knob_access(k)) {
    case KNOB_ABSENT: return "absent";
    case KNOB_RO:     return "ro";
    case KNOB_RW:     return "rw";
    case KNOB_WO:     return "wo";
    default:          return "?";
    }
}

/* ---------------- cpufreq policies ---------------- */

static void on_policy(const char *dir, int idx, void *ctx)
{
    struct surface *s = ctx;
    struct policy *p;
    char buf[1024], rep[256];

    if (s->npol >= MAX_POLICIES)
        return;
    p = &s->pol[s->npol];
    memset(p, 0, sizeof *p);
    p->id = idx;

    if (read_attr_str(dir, "related_cpus", buf, sizeof buf) == 0) {
        const char *q = buf;
        char *end;
        while (*q && p->ncpus < MAX_CPUS) {
            long v;
            errno = 0;
            v = strtol(q, &end, 10);
            if (end == q)
                break;
            if (!errno && v >= 0)
                p->cpus[p->ncpus++] = (int)v;
            q = end;
        }
    }
    read_attr_u64(dir, "cpuinfo_min_freq", &p->cpuinfo_min_khz);
    read_attr_u64(dir, "cpuinfo_max_freq", &p->cpuinfo_max_khz);

    if (read_attr_str(dir, "scaling_available_frequencies", buf, sizeof buf) == 0)
        p->nopp = split_u64(buf, p->opp_khz, MAX_OPP);
    if (read_attr_str(dir, "scaling_available_governors", buf, sizeof buf) == 0)
        p->ngov = split_words(buf, p->governors, MAX_GOVERNORS);

    if (p->ncpus > 0) {
        char cdir[PATH_MAXLEN];
        spath(cdir, sizeof cdir, SYS_CPU "/cpu%d", p->cpus[0]);
        read_attr_u64(cdir, "cpu_capacity", &p->capacity);
    }

    snprintf(rep, sizeof rep, CPUFREQ_DIR "/policy%d/scaling_governor", idx);
    knob_take(&p->k_governor, dir, "scaling_governor", rep);
    snprintf(rep, sizeof rep, CPUFREQ_DIR "/policy%d/scaling_min_freq", idx);
    knob_take(&p->k_min, dir, "scaling_min_freq", rep);
    snprintf(rep, sizeof rep, CPUFREQ_DIR "/policy%d/scaling_max_freq", idx);
    knob_take(&p->k_max, dir, "scaling_max_freq", rep);
    snprintf(rep, sizeof rep, CPUFREQ_DIR "/policy%d/scaling_setspeed", idx);
    knob_take(&p->k_setspeed, dir, "scaling_setspeed", rep);

    s->npol++;
}

/* ---------------- thermal zones ---------------- */

static void on_zone(const char *dir, int idx, void *ctx)
{
    struct surface *s = ctx;
    struct zone *z;
    char buf[NAME_LEN], rep[256];

    if (s->nzone >= MAX_ZONES)
        return;
    if (read_attr_str(dir, "type", buf, sizeof buf) < 0)
        return;                 /* no type: not a zone we can address by name */

    z = &s->zone[s->nzone];
    memset(z, 0, sizeof *z);
    z->idx = idx;
    snprintf(z->type, sizeof z->type, "%s", buf);

    for (int i = 0; i < MAX_TRIPS; i++) {
        char attr[64];
        uint64_t v;

        snprintf(attr, sizeof attr, "trip_point_%d_type", i);
        if (read_attr_str(dir, attr, buf, sizeof buf) < 0)
            continue;
        z->trips[z->ntrips].idx = i;
        snprintf(z->trips[z->ntrips].type, sizeof z->trips[0].type, "%.15s", buf);

        snprintf(attr, sizeof attr, "trip_point_%d_temp", i);
        z->trips[z->ntrips].temp_mc = (read_attr_u64(dir, attr, &v) == 0) ? (long)v : -1;
        z->trips[z->ntrips].mode = path_mode(dir, attr);

        snprintf(attr, sizeof attr, "trip_point_%d_hyst", i);
        z->trips[z->ntrips].hyst_mc = (read_attr_u64(dir, attr, &v) == 0) ? (long)v : -1;
        z->ntrips++;
    }

    /* Which cooling devices this zone drives. These are SYMLINKS - cdev0 -> ../cooling_
     * device7 - so the information is in the link target, not in any file. The capture
     * walked `find -type f` for days and silently had none of it.
     *
     * Enumerated by trying cdev0, cdev1, ... and stopping at the first gap: the kernel
     * numbers them contiguously from 0. Deliberately NOT opendir(): the directory also
     * holds cdevN_trip_point and cdevN_weight regular files and a `subsystem` link, and
     * filtering those by name is more fragile than asking for exactly what we want. */
    for (int i = 0; ; i++) {
        char lp[PATH_MAXLEN], tgt[256];
        ssize_t n;
        int num;

        if (i >= MAX_ZONE_CDEVS) {
            /* More bindings than we record. Say so rather than truncate quietly: with
             * this set, "cd is not in bound[]" stops being a conclusive answer. */
            z->bound_overflow = 1;
            break;
        }
        spath(lp, sizeof lp, THERMAL_DIR "/thermal_zone%d/cdev%d", idx, i);
        n = readlink(lp, tgt, sizeof tgt - 1);
        if (n < 0)
            break;              /* no cdev at this index: the list ends here */
        tgt[n] = '\0';
        /* Target is "../cooling_deviceN". Parse the number off the basename rather than
         * assuming the prefix length - a different kernel could write an absolute path. */
        {
            const char *base = strrchr(tgt, '/');
            base = base ? base + 1 : tgt;
            if (sscanf(base, "cooling_device%d", &num) != 1)
                continue;       /* a link we do not understand is not a binding we can use */
        }
        z->bound[z->nbound++] = num;
        s->bindings_visible = 1;
    }

    snprintf(rep, sizeof rep, THERMAL_DIR "/thermal_zone%d/mode", idx);
    knob_take(&z->k_mode, dir, "mode", rep);
    snprintf(rep, sizeof rep, THERMAL_DIR "/thermal_zone%d/policy", idx);
    knob_take(&z->k_policy, dir, "policy", rep);
    snprintf(rep, sizeof rep, THERMAL_DIR "/thermal_zone%d/sustainable_power", idx);
    knob_take(&z->k_sustainable, dir, "sustainable_power", rep);

    s->nzone++;
}

/* ---------------- cooling devices ---------------- */

static void on_cdev(const char *dir, int idx, void *ctx)
{
    struct surface *s = ctx;
    struct cdev *c;
    char buf[NAME_LEN], rep[256];

    if (s->ncdev >= MAX_CDEVS)
        return;
    if (read_attr_str(dir, "type", buf, sizeof buf) < 0)
        return;

    c = &s->cdev[s->ncdev];
    memset(c, 0, sizeof *c);
    c->idx = idx;
    snprintf(c->type, sizeof c->type, "%s", buf);
    c->state_ok = (read_attr_u64(dir, "cur_state", &c->cur_state) == 0)
               && (read_attr_u64(dir, "max_state", &c->max_state) == 0);

    snprintf(rep, sizeof rep, THERMAL_DIR "/cooling_device%d/cur_state", idx);
    knob_take(&c->k_cur_state, dir, "cur_state", rep);
    s->ncdev++;
}

/* ---------------- GPU ---------------- */

static void find_gpu(struct surface *s)
{
    char base[PATH_MAXLEN], buf[1024], rep[256];
    struct dirent *e;
    DIR *d;

    spath(base, sizeof base, DEVFREQ_DIR);
    d = opendir(base);
    if (!d)
        return;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        /* Only one devfreq node exists on this board, but do not assume it:
         * match the ".gpu" suffix rather than taking the first entry. */
        if (!strstr(e->d_name, ".gpu"))
            continue;
        snprintf(s->gpu.node, sizeof s->gpu.node, "%.63s", e->d_name);
        spath(s->gpu.dir, sizeof s->gpu.dir, DEVFREQ_DIR "/%.48s", e->d_name);
        s->gpu.present = 1;
        break;
    }
    closedir(d);
    if (!s->gpu.present)
        return;

    read_attr_u64(s->gpu.dir, "cur_freq", &s->gpu.cur_hz);
    read_attr_u64(s->gpu.dir, "min_freq", &s->gpu.min_hz);
    read_attr_u64(s->gpu.dir, "max_freq", &s->gpu.max_hz);
    if (read_attr_str(s->gpu.dir, "available_frequencies", buf, sizeof buf) == 0)
        s->gpu.nopp = split_u64(buf, s->gpu.opp_hz, MAX_OPP);
    if (read_attr_str(s->gpu.dir, "available_governors", buf, sizeof buf) == 0)
        s->gpu.ngov = split_words(buf, s->gpu.governors, MAX_GOVERNORS);

    snprintf(rep, sizeof rep, DEVFREQ_DIR "/%s/governor", s->gpu.node);
    knob_take(&s->gpu.k_governor, s->gpu.dir, "governor", rep);
    snprintf(rep, sizeof rep, DEVFREQ_DIR "/%s/min_freq", s->gpu.node);
    knob_take(&s->gpu.k_min, s->gpu.dir, "min_freq", rep);
    snprintf(rep, sizeof rep, DEVFREQ_DIR "/%s/max_freq", s->gpu.node);
    knob_take(&s->gpu.k_max, s->gpu.dir, "max_freq", rep);
}

/* ---------------- CPU hotplug ---------------- */

/* Every cpuN gets an entry, including cpu0. The synthetic fixture asserted that
 * cpu0 has no 'online' node; the board says otherwise (mode 644, value 1), so
 * presence is recorded per cpu rather than assumed. */
static void on_cpu(const char *dir, int idx, void *ctx)
{
    struct surface *s = ctx;
    struct cpu_online *c;
    char rep[256], buf[NAME_LEN];

    if (s->ncpu >= MAX_CPUS)
        return;
    /* scan_indexed matches the "cpu" prefix, which also hits cpuidle/cpufreq
     * style siblings; require an actual per-cpu directory. */
    if (!path_exists(dir, "uevent") && !path_exists(dir, "online")
        && !path_exists(dir, "topology"))
        return;

    c = &s->cpu[s->ncpu];
    memset(c, 0, sizeof *c);
    c->cpu = idx;
    c->present = path_exists(dir, "online");
    c->online = 1;              /* a cpu we can see is online unless it says 0 */
    if (c->present && read_attr_str(dir, "online", buf, sizeof buf) == 0)
        c->online = (buf[0] == '1');

    /* State NAMES differ between clusters (cpu0: cpu-sleep-1-*, cpu4:
     * cpu-sleep-0-*), so count by index and never match on name. */
    for (int i = 0; i < 16; i++) {
        char attr[64];
        snprintf(attr, sizeof attr, "cpuidle/state%d/disable", i);
        if (!path_exists(dir, attr))
            break;
        c->nidle = i + 1;
    }

    snprintf(rep, sizeof rep, SYS_CPU "/cpu%d/online", idx);
    knob_take(&c->k_online, dir, "online", rep);
    s->ncpu++;
}

/* ---------------- entry point ---------------- */

void surface_discover(struct surface *s)
{
    char path[PATH_MAXLEN];

    memset(s, 0, sizeof *s);

    spath(path, sizeof path, "/proc/device-tree/model");
    if (read_file_str(path, s->model, sizeof s->model) < 0) {
        spath(path, sizeof path, "/sys/firmware/devicetree/base/model");
        (void)read_file_str(path, s->model, sizeof s->model);
    }

    scan_indexed(CPUFREQ_DIR, "policy", on_policy, s);
    scan_indexed(SYS_CPU, "cpu", on_cpu, s);
    scan_indexed(THERMAL_DIR, "thermal_zone", on_zone, s);
    scan_indexed(THERMAL_DIR, "cooling_device", on_cdev, s);
    find_gpu(s);

    /* The uclamp sysctls are a flat pair, not an indexed device, so they are taken
     * here rather than by a scan. knob_take() wants a directory and an attribute. */
    {
        char dir[PATH_MAXLEN];
        spath(dir, sizeof dir, "/proc/sys/kernel");
        knob_take(&s->k_uclamp_min, dir, "sched_util_clamp_min",
                  "/proc/sys/kernel/sched_util_clamp_min");
        knob_take(&s->k_uclamp_max, dir, "sched_util_clamp_max",
                  "/proc/sys/kernel/sched_util_clamp_max");
    }

    if (hwmon_find(HWMON_FAN, s->hwmon_fan, sizeof s->hwmon_fan) < 0)
        s->hwmon_fan[0] = '\0';
    if (hwmon_find(HWMON_POWER, s->hwmon_power, sizeof s->hwmon_power) < 0)
        s->hwmon_power[0] = '\0';
}

/* ---------------- lookups ---------------- */

/* Accepts the short name ("cpuss-0-0") as well as the full type, because the
 * short form is what gets printed and a user copying a label out of the output
 * must not be told a plainly existing zone does not exist. */
int surface_zone_binds(const struct surface *s, const struct zone *z,
                       const struct cdev *cd)
{
    if (!s || !z || !cd)
        return -1;
    /* Nothing anywhere exposed a binding, so this board's layout is simply not visible to
     * us. Returning 0 here would turn "we cannot see" into "it is not bound", and a caller
     * would warn about a correct configuration. */
    if (!s->bindings_visible)
        return -1;
    for (int i = 0; i < z->nbound; i++)
        if (z->bound[i] == cd->idx)
            return 1;
    /* Absent from a list we know to be partial proves nothing. */
    if (z->bound_overflow)
        return -1;
    return 0;
}

const struct zone *surface_zone(const struct surface *s, const char *name)
{
    size_t wl = strlen(name);

    for (int i = 0; i < s->nzone; i++) {
        if (strcmp(s->zone[i].type, name) == 0)
            return &s->zone[i];
        if (strncmp(s->zone[i].type, name, wl) == 0
            && strcmp(s->zone[i].type + wl, "-thermal") == 0)
            return &s->zone[i];
    }
    return NULL;
}

int surface_uclamp(const struct knob *k, unsigned long long *out)
{
    char *end = NULL;
    unsigned long long v;

    if (!k || !k->present || !k->boot[0])
        return -1;
    errno = 0;
    v = strtoull(k->boot, &end, 10);
    if (errno || !end || end == k->boot)
        return -1;
    if (out)
        *out = v;
    return 0;
}

const struct cdev *surface_cdev(const struct surface *s, const char *type)
{
    for (int i = 0; i < s->ncdev; i++)
        if (strcmp(s->cdev[i].type, type) == 0)
            return &s->cdev[i];
    return NULL;
}

const struct policy *surface_policy(const struct surface *s, int id)
{
    for (int i = 0; i < s->npol; i++)
        if (s->pol[i].id == id)
            return &s->pol[i];
    return NULL;
}
