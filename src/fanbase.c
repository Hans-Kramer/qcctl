// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer

#define _GNU_SOURCE
#include "fanbase.h"
#include "io.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PFAIL(...) do { if (errlen) snprintf(err, errlen, __VA_ARGS__); return -1; } while (0)

int fanbase_write(const char *path, const struct fanbase *fb, char *err, size_t errlen)
{
    FILE *f;
    int wrote = 0;

    if (errlen)
        err[0] = '\0';
    /* Not write_path_str(): that one has no O_CREAT on purpose, for sysfs. This is a file
     * we own. */
    f = fopen(path, "w");
    if (!f)
        PFAIL("cannot write %s: %s", path, strerror(errno));

    fprintf(f, "# qcctl fan baseline - MEASURED DATA, not configuration.\n");
    fprintf(f, "# Do not hand-edit: the delta that judges against it lives in fan.config,\n");
    fprintf(f, "# and keeping intent and measurement in separate files is deliberate.\n");
    fprintf(f, "# Re-capture with: qcctl --fan-baseline capture\n");
    fprintf(f, "captured %s\n", fb->captured[0] ? fb->captured : "unknown");
    fprintf(f, "board %s\n", fb->board[0] ? fb->board : "unknown");
    fprintf(f, "cdev %s\n", fb->cdev[0] ? fb->cdev : "unknown");
    /* Written only when actually known. An absent source must leave the KEY absent, not
     * record the word "unknown": a later comparison would then read a change that was
     * never observed. */
    if (fb->env.have_kernel)
        fprintf(f, "kernel %s\n", fb->env.kernel);
    if (fb->env.have_build_id)
        fprintf(f, "build_id %s\n", fb->env.build_id);
    fprintf(f, "max_state %d\n", fb->max_state);
    for (int s = 0; s <= FANBASE_MAX_STATE; s++) {
        if (!fb->have[s])
            continue;
        fprintf(f, "state %d mean %ld lo %ld hi %ld n %d\n",
                s, fb->mean[s], fb->lo[s], fb->hi[s], fb->n[s]);
        wrote++;
    }
    if (fclose(f) != 0)
        PFAIL("cannot close %s: %s", path, strerror(errno));
    if (!wrote)
        PFAIL("refusing to write an empty baseline to %s", path);
    return 0;
}

int fanbase_read(const char *path, struct fanbase *fb, char *err, size_t errlen)
{
    char line[256];
    FILE *f;
    int states = 0;

    if (errlen)
        err[0] = '\0';
    memset(fb, 0, sizeof *fb);
    f = fopen(path, "r");
    if (!f)
        PFAIL("cannot read %s: %s", path, strerror(errno));

    while (fgets(line, sizeof line, f)) {
        int s, n;
        long mean, lo, hi;
        char *nl = strchr(line, '\n');

        if (nl)
            *nl = '\0';
        if (line[0] == '#' || !line[0])
            continue;
        if (!strncmp(line, "captured ", 9)) {
            snprintf(fb->captured, sizeof fb->captured, "%.*s",
                     (int)sizeof fb->captured - 1, line + 9);
        } else if (!strncmp(line, "board ", 6)) {
            snprintf(fb->board, sizeof fb->board, "%.*s",
                     (int)sizeof fb->board - 1, line + 6);
        } else if (!strncmp(line, "cdev ", 5)) {
            snprintf(fb->cdev, sizeof fb->cdev, "%.*s",
                     (int)sizeof fb->cdev - 1, line + 5);
        } else if (!strncmp(line, "kernel ", 7)) {
            snprintf(fb->env.kernel, sizeof fb->env.kernel, "%.*s",
                     (int)sizeof fb->env.kernel - 1, line + 7);
            fb->env.have_kernel = (fb->env.kernel[0] != '\0');
        } else if (!strncmp(line, "build_id ", 9)) {
            snprintf(fb->env.build_id, sizeof fb->env.build_id, "%.*s",
                     (int)sizeof fb->env.build_id - 1, line + 9);
            fb->env.have_build_id = (fb->env.build_id[0] != '\0');
        } else if (!strncmp(line, "max_state ", 10)) {
            fb->max_state = atoi(line + 10);
        } else if (sscanf(line, "state %d mean %ld lo %ld hi %ld n %d",
                          &s, &mean, &lo, &hi, &n) == 5) {
            if (s < 0 || s > FANBASE_MAX_STATE) {
                fclose(f);
                PFAIL("%s: state %d out of range", path, s);
            }
            if (mean <= 0) {
                fclose(f);
                PFAIL("%s: state %d has a non-positive mean (%ld)", path, s, mean);
            }
            fb->have[s] = 1;
            fb->mean[s] = mean;
            fb->lo[s] = lo;
            fb->hi[s] = hi;
            fb->n[s] = n;
            states++;
        } else {
            fclose(f);
            PFAIL("%s: cannot parse '%s'", path, line);
        }
    }
    fclose(f);
    if (!states)
        PFAIL("%s: no per-state readings", path);
    return 0;
}

enum fanbase_verdict fanbase_judge(const struct fanbase *fb, int state, long rpm,
                                   int delta_pct, long *dev_pct)
{
    long mean, dev;

    if (dev_pct)
        *dev_pct = 0;
    /* Below FANBASE_MIN_STATE there is no signal: state 0 is 0 rpm and state 1's tach
     * misreads. Returning UNKNOWN rather than OK matters - a caller must not read silence
     * as health. */
    if (state < FANBASE_MIN_STATE || state > FANBASE_MAX_STATE)
        return FANBASE_UNKNOWN;
    if (!fb->have[state] || fb->mean[state] <= 0)
        return FANBASE_UNKNOWN;
    if (rpm <= 0)
        return FANBASE_UNKNOWN;         /* no reading is not a reading of zero */
    if (delta_pct < FANBASE_MIN_DELTA_PCT)
        delta_pct = FANBASE_MIN_DELTA_PCT;

    mean = fb->mean[state];
    dev = ((rpm - mean) * 100) / mean;
    if (dev_pct)
        *dev_pct = dev;

    if (dev > delta_pct)
        return FANBASE_RESTRICTED;      /* faster than baseline: inlet obstructed */
    if (-dev > delta_pct)
        return FANBASE_DEGRADED;        /* slower than baseline: fan not delivering */
    return FANBASE_OK;
}

const char *fanbase_verdict_str(enum fanbase_verdict v)
{
    switch (v) {
    case FANBASE_OK:         return "ok";
    case FANBASE_RESTRICTED: return "restricted";
    case FANBASE_DEGRADED:   return "degraded";
    default:                 return "unknown";
    }
}
