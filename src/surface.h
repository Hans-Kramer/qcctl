// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer
//
// Discovery of the live control surface. Policies, zones, cooling devices and hwmon
// are all found at runtime and addressed by name or id, never by sysfs index.

#ifndef QCCTL_SURFACE_H
#define QCCTL_SURFACE_H

#include <stdint.h>
#include <stddef.h>

#define MAX_POLICIES    8
#define MAX_CPUS        32
#define MAX_OPP         64
#define MAX_GOVERNORS   16
#define MAX_ZONES       128
#define MAX_TRIPS       16
#define MAX_CDEVS       32
#define NAME_LEN        64
#define MAX_ZONE_CDEVS  8       // cooling devices recorded per zone

// A capability we may write. mode and owner are what the board reports, not a
// judgement about this process. boot is captured so --reset restores what was there.
struct knob {
    char     path[256];         // board-absolute, for reporting
    int      present;
    int      mode;              // octal, -1 unknown
    char     owner[32];         // "root:root", or "" if unknown
    char     boot[NAME_LEN];    // value at discovery; "" if unreadable
};

// Classification derived from mode. Write-only is its own case: some knobs are 200/220.
enum knob_access { KNOB_ABSENT, KNOB_RO, KNOB_RW, KNOB_WO, KNOB_UNKNOWN };
enum knob_access knob_access(const struct knob *k);
const char *knob_access_str(const struct knob *k);

struct policy {
    int      id;                            // policyN
    int      cpus[MAX_CPUS];
    int      ncpus;
    uint64_t capacity;                      // cpu_capacity of its first cpu
    uint64_t cpuinfo_min_khz, cpuinfo_max_khz;
    uint64_t opp_khz[MAX_OPP];              // scaling_available_frequencies
    int      nopp;
    char     governors[MAX_GOVERNORS][NAME_LEN];
    int      ngov;
    struct knob k_governor, k_min, k_max, k_setspeed;
};

struct trip {
    int   idx;
    char  type[16];             // active | passive | critical | hot
    long  temp_mc, hyst_mc;
    int   mode;                 // octal mode of trip_point_N_temp, -1 unknown
};

// No live temperature here: that is qcstats'. trip[].temp_mc is a threshold, not a reading.
struct zone {
    char  type[NAME_LEN];       // e.g. "cpuss-0-0-thermal"
    int   idx;
    struct trip trips[MAX_TRIPS];
    int   ntrips;
    struct knob k_mode, k_policy, k_sustainable;

    // Cooling devices this zone drives, from its cdevN symlinks.
    int   bound[MAX_ZONE_CDEVS];    // cooling_deviceN numbers, matching struct cdev.idx
    int   nbound;                   // how many bindings were read
    int   bound_overflow;           // more than MAX_ZONE_CDEVS: the list is partial
};

struct cdev {
    char     type[NAME_LEN];    // e.g. "emc2305_fan", "cdsp:cdsp_sw"
    int      idx;
    uint64_t cur_state, max_state;
    int      state_ok;
    struct knob k_cur_state;
};

struct gpu {
    int      present;
    char     node[NAME_LEN];    // e.g. "3d00000.gpu"
    char     dir[256];
    uint64_t cur_hz, min_hz, max_hz;
    uint64_t opp_hz[MAX_OPP];
    int      nopp;
    char     governors[MAX_GOVERNORS][NAME_LEN];
    int      ngov;
    struct knob k_governor, k_min, k_max;
};

struct cpu_online {
    int  cpu;
    int  present;               // the 'online' node exists
    int  online;
    int  nidle;                 // cpuidle states, for validating CPUIDLE
    struct knob k_online;
};

struct surface {
    struct policy     pol[MAX_POLICIES];
    int               npol;
    // 0 = no zone exposed any cdevN link, so bindings are unknown, not absent.
    int               bindings_visible;
    struct zone       zone[MAX_ZONES];
    int               nzone;
    struct cdev       cdev[MAX_CDEVS];
    int               ncdev;
    struct gpu        gpu;
    struct cpu_online cpu[MAX_CPUS];
    int               ncpu;
    char              hwmon_fan[256];       // "" if absent
    char              hwmon_power[256];
    char              model[128];
    // The system-wide uclamp pair. The kernel enforces min <= max on every write.
    struct knob       k_uclamp_min, k_uclamp_max;
};

void surface_discover(struct surface *s);

// Lookups by name. Index is never a valid way to address these.
const struct zone *surface_zone(const struct surface *s, const char *name);

// Is `cd` bound to `z`? 1 = yes, 0 = conclusively no, -1 = cannot tell.
// -1 must never be read as "no".
int surface_zone_binds(const struct surface *s, const struct zone *z,
                       const struct cdev *cd);
const struct cdev *surface_cdev(const struct surface *s, const char *type);
// Live value of a uclamp knob. 0 = read, *out set; -1 = not readable, do not guess.
int surface_uclamp(const struct knob *k, unsigned long long *out);
const struct policy *surface_policy(const struct surface *s, int id);

#endif /* QCCTL_SURFACE_H */
