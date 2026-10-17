// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer
//
// Reading telemetry from qcstats, which is the only way qcctl learns a temperature.
// Many qcstats fields are nullable: a null reading means NOT MEASURED, never 0.

#ifndef QCCTL_STATS_H
#define QCCTL_STATS_H

#include <stdbool.h>
#include <sys/types.h>

#define STATS_LINE_MAX 8192

// The qcstats NDJSON schema version this reader understands. Any other version counts
// as no reading rather than a possibly misread one.
#define STATS_SCHEMA_V 1

struct stats {
    pid_t pid;
    int   fd;                   // read end of the child's stdout
    char  buf[STATS_LINE_MAX];
    size_t len;                 // bytes buffered, for line reassembly
    char  last[STATS_LINE_MAX]; // most recent complete line, for extra extractors
    long  v_bad;                // lines whose schema version is not understood
    long  v_bad_last;           // the version on the most recent one, or -1
    int   exit_status;          // raw waitpid() status, set by stats_close()
    int   have_status;          // 0 = never reaped, so exit_status means nothing
    int   killed;               // it did not go on SIGTERM and we escalated
    long  samples;              // complete samples read on this stream, ever
};

// Why the sampler stream ended, for the log. NULL when there is nothing to say.
// `path` is the binary the command is expected to be, and is named in the message.
const char *stats_exit_hint(const struct stats *st, const char *path);

#define STATS_MAX_CPU 4         // 3 policies on this board, plus one so a 4th shows up

// One entry per cpufreq POLICY, not per core - frequency is a policy property.
struct cpuobs {
    int  n;                     // policy entries read; 0 = no "cpu" in the line
    bool overflow;              // more than STATS_MAX_CPU: n is partial
    struct {
        char label[16];         // qcstats' label, e.g. "A78C.0", "A55"
        bool mhz_ok;  long mhz; // mhz_ok false = null, not 0 MHz
    } pol[STATS_MAX_CPU];
};

// What the sampler saw of the fan. The object and each field are separately nullable.
struct fanobs {
    bool present;       // "fan" was an object, not null
    bool duty_ok;  long duty_pct;
    bool rpm_ok;   long rpm;
    bool rpm_valid;     // qcstats' own flag: the tach reading is trustworthy
};

// Spawn the sampler. `cmd` runs via /bin/sh -c. Returns 0, or -1 with a reason in err.
int stats_open(struct stats *st, const char *cmd, char *err, size_t errlen);

// The last complete line stats_sample() parsed, or "" before the first one.
const char *stats_last_line(const struct stats *st);

// Fan observation from one NDJSON line. False if there was no "fan" key at all.
bool stats_extract_fan(const char *line, struct fanobs *out);

// Per-policy CPU frequencies from one line. True when at least one policy was read.
bool stats_extract_cpu(const char *line, struct cpuobs *out);

void stats_close(struct stats *st);

enum stats_read {
    STATS_OK = 0,       // a line was read and parsed
    STATS_TIMEOUT,      // nothing within the deadline - the stream stalled
    STATS_EOF,          // the sampler exited
    STATS_ERROR,
};

// Read one sample and extract the temperature of `zone` (short or full name).
// *temp_ok false means the field was null or absent - do not substitute a number.
enum stats_read stats_sample(struct stats *st, int timeout_ms, const char *zone,
                             long *temp_mc, bool *temp_ok);

// One zone's temperature from a single line. Exposed so parsing can be asserted.
bool stats_extract_temp(const char *line, const char *zone, long *temp_mc);

// The schema version of one line: its leading `v`, or -1 if absent or unparseable.
long stats_line_version(const char *line);

#endif /* QCCTL_STATS_H */
