// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer
//
// The profile config: parse, then validate against the discovered surface.
// Nothing here writes. Two rules drive the design:
//
//   Unset is never a default. A field absent from a profile means "do not touch",
//   never "write the value we think is stock". This is the control-side twin of
//   the sampler's absent-is-never-zero.
//
//   Units are mandatory, so a bare number is a parse error. Both kHz and Hz appear
//   on this board - CPU OPPs are kHz because several are not whole MHz, GPU is Hz as
//   devfreq reports it - which makes a unitless number genuinely ambiguous rather
//   than merely untidy. Mixed units between fields have cost two real bugs here.

#ifndef QCCTL_CONFIG_H
#define QCCTL_CONFIG_H

#include "surface.h"
#include <stdint.h>
#include <stddef.h>

#define MAX_PROFILES    16
#define MAX_ACTIONS     128
#define ERRLEN          256

enum act_kind {
    ACT_CPU_GOVERNOR,   // target_id = policy id, sval = governor
    ACT_CPU_MIN,        // target_id = policy id, value = kHz
    ACT_CPU_MAX,
    ACT_CPU_SETSPEED,
    ACT_CPU_ONLINE,     // target_id = cpu,       value = 0|1
    ACT_GPU_GOVERNOR,   // sval = governor
    ACT_GPU_MIN,        // value = Hz
    ACT_GPU_MAX,
    ACT_COOLING_STATE,  // target = cdev type,    value = state
    ACT_ZONE_MODE,      // target = zone name,    sval = enabled|disabled
    ACT_ZONE_POLICY,    // target = zone name,    sval = governor
    ACT_ZONE_TRIP_TEMP, // target = zone, target_id = trip, value = milli-C
    ACT_ZONE_TRIP_HYST,
    ACT_ZONE_SUSTAINABLE,
    ACT_CPUIDLE,        // target_id = cpu, aux = state idx, value = disable 0|1
    ACT_UCLAMP_MIN,     // value = 0..1024
    ACT_UCLAMP_MAX,
    ACT_FAN_MODE,       // value: 0 = kernel keeps the fan, 1 = daemon owns it
    ACT_FAN_CURVE,      // target = curve name from fan.config
};

struct action {
    enum act_kind kind;
    int      line;
    int      target_id;             // policy id, cpu, or trip index
    int      aux;                   // cpuidle state index
    char     target[NAME_LEN];      // zone type or cooling-device type
    char     sval[NAME_LEN];        // governor / mode
    uint64_t value;                 // already in the SYSFS unit for its domain
};

struct profile {
    int  id;
    char name[NAME_LEN];
    int  is_default;
    // Applied when a recorded selection exists but cannot be honoured. NOT applied on a
    // clean boot - nothing has gone wrong there, and DEFAULT keeps its meaning.
    int  is_fallback;
    int  line;
    struct action act[MAX_ACTIONS];
    int  nact;
};

struct pconfig {
    struct profile prof[MAX_PROFILES];
    int  nprof;
    char err[ERRLEN];               // first error only; parsing stops there
    int  err_line;
};

// FAN MODE values. Kernel is the default: it changes nothing about how the board
// already behaves, and only the curve mode takes the fan off the thermal governor.
#define FAN_MODE_KERNEL 0
#define FAN_MODE_CURVE  1

int config_parse_string(const char *text, struct pconfig *c);
int config_parse_file(const char *path, struct pconfig *c);

// Validates every action against what the board actually exposes. A profile is
// accepted or rejected WHOLE: a half-applied profile leaves the machine in a
// state no config describes.
//
int config_validate(const struct pconfig *c, const struct surface *s,
                    char *err, size_t errlen);

// The disruptive actions of ONE profile: legal and KNOWN TO WORK, but drastic enough that
// applying one should be a deliberate act. Currently exactly one thing, offlining cpu0.
// Returns how many were found, and stores the first `maxdisr` of them when `disr` is given.
//
// Per profile, because the gate is about what is being applied. config_validate() used to
// collect them across the whole file, which made a setting in a profile nobody applies
// demand a flag from everybody.
//
// NOTE: this mechanism means ONE thing. A genuinely unverified setting, if one ever
// appears, needs its own gate rather than borrowing this one. One flag meaning two things
// is how the name went wrong the first time.
size_t config_disruptive(const struct profile *p, const struct action **disr,
                         size_t maxdisr);

const struct profile *config_find(const struct pconfig *c, const char *name_or_id);

// The profile marked DEFAULT, or NULL. A config without one is still valid - `-m <name>`
// never consults it - but the installed daemon carries no -m, so --verify and qcctl-check
// treat its absence as a failure. See the note on the definition.
const struct profile *config_default(const struct pconfig *c);
const char *act_describe(const struct action *a, char *buf, size_t len);

#endif /* QCCTL_CONFIG_H */
