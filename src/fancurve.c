// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer

#define _GNU_SOURCE
#include "fancurve.h"
#include "fanbase.h"
#include "io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>

#define DEFAULT_POLL_MS   2000
#define DEFAULT_HYST_MC   4000
#define FAN_CDEV_DEFAULT  "emc2305_fan"

static int cfail(struct fanconfig *fc, int line, const char *fmt, ...)
{
    va_list ap;

    if (fc->err[0])
        return -1;                      /* keep the first error */
    fc->err_line = line;
    va_start(ap, fmt);
    vsnprintf(fc->err, sizeof fc->err, fmt, ap);
    va_end(ap);
    return -1;
}

static int tokenise(char *s, char **tok, int max)
{
    int n = 0;

    while (*s && n < max) {
        while (*s && isspace((unsigned char)*s))
            *s++ = '\0';
        if (!*s)
            break;
        tok[n++] = s;
        while (*s && !isspace((unsigned char)*s))
            s++;
    }
    return n;
}

static int p_temp(const char *s, long *out, const char **why)
{
    char *end;
    long v;

    errno = 0;
    v = strtol(s, &end, 10);
    if (end == s || errno) {
        *why = "not a number";
        return -1;
    }
    if (strcasecmp(end, "c") != 0) {
        *why = *end ? "unknown unit (use C)" : "missing unit (use C, e.g. 45C)";
        return -1;
    }
    if (v < 0) {
        *why = "negative temperature";
        return -1;
    }
    *out = v * 1000;
    return 0;
}

static int p_uint(const char *s, long *out, const char **why)
{
    char *end;
    long v;

    errno = 0;
    v = strtol(s, &end, 10);
    if (end == s || *end || errno || s[0] == '-') {
        *why = "not a plain non-negative integer";
        return -1;
    }
    *out = v;
    return 0;
}

static int p_pct(const char *s, long *out, const char **why)
{
    char *end;
    long v;
    size_t len = strlen(s);

    if (len < 2 || s[len - 1] != '%') {
        *why = "needs a '%' suffix, e.g. 8%";
        return -1;
    }
    errno = 0;
    v = strtol(s, &end, 10);
    if (end == s || end != s + len - 1 || errno || s[0] == '-') {
        *why = "not a non-negative percentage";
        return -1;
    }
    *out = v;
    return 0;
}

static int p_dur(const char *s, int *out_ms, const char **why)
{
    char *end;
    long v;

    errno = 0;
    v = strtol(s, &end, 10);
    if (end == s || errno || v < 0) {
        *why = "not a positive number";
        return -1;
    }
    if (!strcasecmp(end, "ms"))     *out_ms = (int)v;
    else if (!strcasecmp(end, "s")) *out_ms = (int)(v * 1000);
    else {
        *why = *end ? "unknown unit (use s or ms)" : "missing unit (use s or ms)";
        return -1;
    }
    return 0;
}

#define CWANT(cond, ...) do { if (!(cond)) return cfail(fc, line, __VA_ARGS__); } while (0)

#define DEFAULT_POLL_MS      2000
#define DEFAULT_HYST_MC      4000
#define DEFAULT_DEADBAND_MC  2000
#define DEFAULT_STEP_MS     15000
#define FAN_CDEV_DEFAULT    "emc2305_fan"

enum block { BLK_NONE = 0, BLK_CURVE, BLK_TARGET, BLK_AIRFLOW };

int fanconfig_parse_string(const char *text, struct fanconfig *fc)
{
    const char *p = text;
    char buf[512];
    int line = 0;
    enum block in = BLK_NONE;

    memset(fc, 0, sizeof *fc);

    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        struct curve  *c = (in == BLK_CURVE)  ? &fc->c[fc->ncurve - 1]  : NULL;
        struct target *tg = (in == BLK_TARGET) ? &fc->t[fc->ntarget - 1] : NULL;
        char *t[16], *hash;
        const char *why;
        int n;

        line++;
        if (len >= sizeof buf)
            len = sizeof buf - 1;
        memcpy(buf, p, len);
        buf[len] = '\0';
        hash = strchr(buf, '#');
        if (hash)
            *hash = '\0';
        n = tokenise(buf, t, 16);
        if (!n)
            goto next;

        /* ---- block open ---- */
        if (!strcmp(t[0], "<")) {
            CWANT(in == BLK_NONE, "nested block");
            CWANT(n >= 2, "expected '< CURVE NAME=... >' or '< TARGET NAME=... >'");
            if (!strcasecmp(t[1], "CURVE")) {
                CWANT(fc->ncurve < MAX_CURVES, "too many curves (max %d)", MAX_CURVES);
                c = &fc->c[fc->ncurve++];
                memset(c, 0, sizeof *c);
                c->line = line;
                c->hyst_mc = DEFAULT_HYST_MC;
                c->poll_ms = DEFAULT_POLL_MS;
                c->failsafe = FAILSAFE_KERNEL;
                snprintf(c->cdev, sizeof c->cdev, "%s", FAN_CDEV_DEFAULT);
                in = BLK_CURVE;
            } else if (!strcasecmp(t[1], "TARGET")) {
                CWANT(fc->ntarget < MAX_TARGETS, "too many targets (max %d)", MAX_TARGETS);
                tg = &fc->t[fc->ntarget++];
                memset(tg, 0, sizeof *tg);
                tg->line = line;
                tg->deadband_mc = DEFAULT_DEADBAND_MC;
                tg->poll_ms = DEFAULT_POLL_MS;
                tg->step_ms = DEFAULT_STEP_MS;
                tg->failsafe = FAILSAFE_KERNEL;
                snprintf(tg->cdev, sizeof tg->cdev, "%s", FAN_CDEV_DEFAULT);
                in = BLK_TARGET;
            } else if (!strcasecmp(t[1], "AIRFLOW")) {
                CWANT(!fc->air.present, "only one AIRFLOW block per file");
                memset(&fc->air, 0, sizeof fc->air);
                fc->air.present = 1;
                fc->air.line = line;
                fc->air.delta_pct = FANBASE_DEF_DELTA_PCT;
                fc->air.debounce = 3;
                in = BLK_AIRFLOW;
            } else {
                return cfail(fc, line,
                             "unknown block type '%s'; expected CURVE, TARGET or AIRFLOW",
                             t[1]);
            }
            if (in == BLK_AIRFLOW) {
                /* No NAME=: there is only ever one, and nothing refers to it by name. */
                for (int i = 2; i < n; i++)
                    CWANT(!strcmp(t[i], ">"), "AIRFLOW takes no attributes, got '%s'", t[i]);
                goto next;
            }
            for (int i = 2; i < n; i++) {
                char *dst = (in == BLK_CURVE) ? c->name : tg->name;
                if (!strcmp(t[i], ">"))
                    break;
                CWANT(!strncasecmp(t[i], "NAME=", 5),
                      "unknown block attribute '%s'", t[i]);
                snprintf(dst, NAME_LEN, "%.*s", NAME_LEN - 1, t[i] + 5);
            }
            {
                const char *nm = (in == BLK_CURVE) ? c->name : tg->name;
                CWANT(nm[0], "block needs NAME=");
                /* One namespace across both kinds: a profile says FAN CURVE <name> and
                 * must not be ambiguous about which it meant. */
                for (int i = 0; i < fc->ncurve; i++)
                    if (&fc->c[i] != c)
                        CWANT(strcmp(fc->c[i].name, nm) != 0,
                              "duplicate name '%s' (also line %d)", nm, fc->c[i].line);
                for (int i = 0; i < fc->ntarget; i++)
                    if (&fc->t[i] != tg)
                        CWANT(strcmp(fc->t[i].name, nm) != 0,
                              "duplicate name '%s' (also line %d)", nm, fc->t[i].line);
            }
            goto next;
        }

        /* ---- block close ---- */
        if (!strcmp(t[0], ">")) {
            CWANT(in != BLK_NONE, "'>' outside a block");
            if (in == BLK_AIRFLOW) {
                in = BLK_NONE;
                goto next;
            }
            if (in == BLK_CURVE) {
                CWANT(c->zone[0], "curve '%s' needs a ZONE", c->name);
                CWANT(c->npt >= 2, "curve '%s' needs at least 2 POINTs, has %d",
                      c->name, c->npt);
            } else {
                CWANT(tg->zone[0], "target '%s' needs a ZONE", tg->name);
                CWANT(tg->target_mc > 0, "target '%s' needs a TARGET temperature",
                      tg->name);
            }
            in = BLK_NONE;
            goto next;
        }
        CWANT(in != BLK_NONE, "'%s' outside a block", t[0]);

        /* ---- AIRFLOW settings ---- */
        if (in == BLK_AIRFLOW) {
            if (!strcasecmp(t[0], "DELTA")) {
                long v;
                const char *why = "";
                CWANT(n == 2, "DELTA needs a percentage, e.g. 8%%");
                CWANT(p_pct(t[1], &v, &why) == 0, "DELTA '%s': %s", t[1], why);
                /* The floor is measured, not taste: within-state rpm spread reached ~2.3%%
                 * in free air (11h), so under ~5%% calls ordinary noise a fault. */
                CWANT(v >= FANBASE_MIN_DELTA_PCT,
                      "DELTA %ld%% is below the %d%% noise floor measured on this fan",
                      v, FANBASE_MIN_DELTA_PCT);
                CWANT(v <= 90, "DELTA %ld%% is absurd", v);
                fc->air.delta_pct = (int)v;
                goto next;
            }
            if (!strcasecmp(t[0], "DEBOUNCE")) {
                long v;
                const char *why = "";
                CWANT(n == 2, "DEBOUNCE needs a count");
                CWANT(p_uint(t[1], &v, &why) == 0, "DEBOUNCE '%s': %s", t[1], why);
                CWANT(v >= 1 && v <= 30, "DEBOUNCE %ld out of range 1..30", v);
                fc->air.debounce = (int)v;
                goto next;
            }
            return cfail(fc, line, "'%s' is not an AIRFLOW setting", t[0]);
        }

        /* ---- settings common to both ---- */
        if (!strcasecmp(t[0], "ZONE")) {
            CWANT(n == 2, "ZONE needs a zone name");
            snprintf(in == BLK_CURVE ? c->zone : tg->zone, NAME_LEN, "%.*s",
                     NAME_LEN - 1, t[1]);
            goto next;
        }
        if (!strcasecmp(t[0], "COOLING")) {
            CWANT(n == 2, "COOLING needs a cooling-device type");
            snprintf(in == BLK_CURVE ? c->cdev : tg->cdev, NAME_LEN, "%.*s",
                     NAME_LEN - 1, t[1]);
            goto next;
        }
        if (!strcasecmp(t[0], "MIN_STATE")) {
            long v;
            CWANT(n == 2, "MIN_STATE needs a value");
            CWANT(p_uint(t[1], &v, &why) == 0, "MIN_STATE: %s", why);
            if (in == BLK_CURVE) c->min_state = (int)v; else tg->min_state = (int)v;
            goto next;
        }
        if (!strcasecmp(t[0], "POLL")) {
            int ms;
            CWANT(n == 2, "POLL needs a duration");
            CWANT(p_dur(t[1], &ms, &why) == 0, "POLL: %s", why);
            CWANT(ms >= 200, "POLL below 200ms is pointless: the sampler costs power and "
                             "the fan cannot react that fast");
            if (in == BLK_CURVE) c->poll_ms = ms; else tg->poll_ms = ms;
            goto next;
        }
        if (!strcasecmp(t[0], "FAILSAFE")) {
            enum failsafe_kind f;
            CWANT(n == 2, "FAILSAFE needs kernel or full");
            if (!strcasecmp(t[1], "kernel"))    f = FAILSAFE_KERNEL;
            else if (!strcasecmp(t[1], "full")) f = FAILSAFE_FULL;
            else return cfail(fc, line, "FAILSAFE must be kernel or full, got '%s'", t[1]);
            if (in == BLK_CURVE) c->failsafe = f; else tg->failsafe = f;
            goto next;
        }

        /* ---- curve-only ---- */
        if (!strcasecmp(t[0], "POINT")) {
            long temp, st;
            CWANT(in == BLK_CURVE, "POINT belongs in a CURVE block, not a TARGET");
            CWANT(n == 4 && !strcasecmp(t[2], "STATE"), "POINT needs '<temp>C STATE <n>'");
            CWANT(c->npt < MAX_POINTS, "too many POINTs (max %d)", MAX_POINTS);
            CWANT(p_temp(t[1], &temp, &why) == 0, "POINT temperature: %s", why);
            CWANT(p_uint(t[3], &st, &why) == 0, "POINT state: %s", why);
            if (c->npt) {
                CWANT(temp > c->pt[c->npt - 1].temp_mc,
                      "POINT %ldC is not above the previous point (%ldC); list points in "
                      "ascending temperature", temp / 1000,
                      c->pt[c->npt - 1].temp_mc / 1000);
                CWANT(st >= c->pt[c->npt - 1].state,
                      "POINT %ldC drops to state %ld below the previous point's state %d; "
                      "a curve must not go down as it gets hotter",
                      temp / 1000, st, c->pt[c->npt - 1].state);
            }
            c->pt[c->npt].temp_mc = temp;
            c->pt[c->npt].state   = (int)st;
            c->pt[c->npt].line    = line;
            c->npt++;
            goto next;
        }
        if (!strcasecmp(t[0], "HYSTERESIS")) {
            CWANT(in == BLK_CURVE, "HYSTERESIS belongs in a CURVE block; a TARGET uses "
                                   "DEADBAND");
            CWANT(n == 2, "HYSTERESIS needs a temperature");
            CWANT(p_temp(t[1], &c->hyst_mc, &why) == 0, "HYSTERESIS: %s", why);
            goto next;
        }

        /* ---- target-only ---- */
        if (!strcasecmp(t[0], "TARGET")) {
            CWANT(in == BLK_TARGET, "TARGET belongs in a TARGET block");
            CWANT(n == 2, "TARGET needs a temperature");
            CWANT(p_temp(t[1], &tg->target_mc, &why) == 0, "TARGET: %s", why);
            CWANT(tg->target_mc < SAFETY_CEILING_MC,
                  "TARGET %ldC is at or above the %dC safety ceiling, which overrides it "
                  "unconditionally; pick a lower target",
                  tg->target_mc / 1000, SAFETY_CEILING_MC / 1000);
            /* Measured: this zone sits 34-41 C, and full CPU load adds only +4.4 C. A
             * target outside the reachable band can never be met, and the fan would simply
             * sit at one rail for ever. Say so now rather than let it be discovered. */
            CWANT(tg->target_mc >= REACHABLE_LOW_MC && tg->target_mc <= REACHABLE_HIGH_MC,
                  "TARGET %ldC is outside the %d-%dC band this board's fan zone actually "
                  "reaches, so it can never be met",
                  tg->target_mc / 1000, REACHABLE_LOW_MC / 1000, REACHABLE_HIGH_MC / 1000);
            goto next;
        }
        if (!strcasecmp(t[0], "DEADBAND")) {
            CWANT(in == BLK_TARGET, "DEADBAND belongs in a TARGET block; a CURVE uses "
                                    "HYSTERESIS");
            CWANT(n == 2, "DEADBAND needs a temperature");
            CWANT(p_temp(t[1], &tg->deadband_mc, &why) == 0, "DEADBAND: %s", why);
            CWANT(tg->deadband_mc > 0, "DEADBAND must be above 0C: with no deadband the "
                                       "fan would step on every sample");
            goto next;
        }
        if (!strcasecmp(t[0], "STEP_INTERVAL")) {
            CWANT(in == BLK_TARGET, "STEP_INTERVAL belongs in a TARGET block");
            CWANT(n == 2, "STEP_INTERVAL needs a duration");
            CWANT(p_dur(t[1], &tg->step_ms, &why) == 0, "STEP_INTERVAL: %s", why);
            /* Below a few seconds the controller outruns the board again. */
            CWANT(tg->step_ms >= 3000,
                  "STEP_INTERVAL below 3s lets the controller outrun the board: stepping "
                  "every 2s crossed the whole fan range in 20s and saturated");
            goto next;
        }
        if (!strcasecmp(t[0], "MAX_STATE")) {
            long v;
            CWANT(in == BLK_TARGET, "MAX_STATE belongs in a TARGET block");
            CWANT(n == 2, "MAX_STATE needs a value");
            CWANT(p_uint(t[1], &v, &why) == 0, "MAX_STATE: %s", why);
            tg->max_state = (int)v;
            goto next;
        }
        return cfail(fc, line, "unknown setting '%s'", t[0]);
next:
        if (!nl)
            break;
        p = nl + 1;
    }
    if (in != BLK_NONE)
        return cfail(fc, line, "unterminated block (missing '>')");
    return 0;
}

int fanconfig_parse_file(const char *path, struct fanconfig *fc)
{
    char *text;
    long sz;
    int rc;
    FILE *f = fopen(path, "r");

    memset(fc, 0, sizeof *fc);
    if (!f)
        return cfail(fc, 0, "cannot open %s", path);
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > (1 << 20)) {
        fclose(f);
        return cfail(fc, 0, "fan config too large");
    }
    text = malloc((size_t)sz + 1);
    if (!text) {
        fclose(f);
        return cfail(fc, 0, "out of memory");
    }
    sz = (long)fread(text, 1, (size_t)sz, f);
    text[sz] = '\0';
    fclose(f);
    rc = fanconfig_parse_string(text, fc);
    free(text);
    return rc;
}

/* VALIDATION */

#define VF(...) do { snprintf(err, errlen, __VA_ARGS__); return -1; } while (0)

static int check_common(const struct surface *s, const char *kind, const char *name,
                        const char *zone, const char *cdevname, int min_state,
                        const struct cdev **out, char *err, size_t errlen)
{
    const struct cdev *cd = surface_cdev(s, cdevname);

    if (!surface_zone(s, zone))
        VF("%s '%s': no thermal zone '%s' on this board", kind, name, zone);
    if (!cd)
        VF("%s '%s': no cooling device '%s' on this board", kind, name, cdevname);
    if (cd->state_ok && (uint64_t)min_state > cd->max_state)
        VF("%s '%s': MIN_STATE %d exceeds %s max_state %llu", kind, name, min_state,
           cdevname, (unsigned long long)cd->max_state);
    *out = cd;
    return 0;
}

int fanconfig_validate(const struct fanconfig *fc, const struct surface *s,
                       char *err, size_t errlen)
{
    if (errlen)
        err[0] = '\0';

    for (int i = 0; i < fc->ncurve; i++) {
        const struct curve *c = &fc->c[i];
        const struct cdev *cd;

        if (check_common(s, "curve", c->name, c->zone, c->cdev, c->min_state,
                         &cd, err, errlen) < 0)
            return -1;
        for (int p = 0; p < c->npt; p++) {
            if (cd->state_ok && (uint64_t)c->pt[p].state > cd->max_state)
                VF("curve '%s' line %d: state %d exceeds %s max_state %llu",
                   c->name, c->pt[p].line, c->pt[p].state, c->cdev,
                   (unsigned long long)cd->max_state);
            /* A non-zero state below MIN_STATE would be raised silently, so the config
             * would not describe what happens. */
            if (c->pt[p].state > 0 && c->pt[p].state < c->min_state)
                VF("curve '%s' line %d: state %d is below MIN_STATE %d; raise the point "
                   "or lower MIN_STATE rather than have them disagree",
                   c->name, c->pt[p].line, c->pt[p].state, c->min_state);
        }
    }
    for (int i = 0; i < fc->ntarget; i++) {
        const struct target *t = &fc->t[i];
        const struct cdev *cd;

        if (check_common(s, "target", t->name, t->zone, t->cdev, t->min_state,
                         &cd, err, errlen) < 0)
            return -1;
        if (cd->state_ok && t->max_state && (uint64_t)t->max_state > cd->max_state)
            VF("target '%s': MAX_STATE %d exceeds %s max_state %llu",
               t->name, t->max_state, t->cdev, (unsigned long long)cd->max_state);
        /* MAX_STATE UNSET IS NOT UNBOUNDED. target_next() then bounds at TARGET_TOP_DEFAULT,
         * which is right for this board's fan and wrong for every smaller cooling device on
         * it - and the check above skipped entirely when MAX_STATE was absent, so a target
         * on a four-state device was validated and then commanded states it cannot take.
         * Asking the operator for an explicit ceiling is better than picking one for them:
         * the number decides how hard the device is driven. */
        if (cd->state_ok && !t->max_state && cd->max_state < TARGET_TOP_DEFAULT)
            VF("target '%s': %s has only %llu states, fewer than the default ceiling of %d; "
               "set MAX_STATE explicitly", t->name, t->cdev,
               (unsigned long long)cd->max_state, TARGET_TOP_DEFAULT);
    }
    return 0;
}

const struct curve *fanconfig_find_curve(const struct fanconfig *fc, const char *name)
{
    for (int i = 0; i < fc->ncurve; i++)
        if (!strcmp(fc->c[i].name, name))
            return &fc->c[i];
    return NULL;
}

const struct target *fanconfig_find_target(const struct fanconfig *fc, const char *name)
{
    for (int i = 0; i < fc->ntarget; i++)
        if (!strcmp(fc->t[i].name, name))
            return &fc->t[i];
    return NULL;
}

/* Safety ceiling */

int fan_safety_active(long temp_mc, int cur, int max_state)
{
    if (temp_mc >= SAFETY_CEILING_MC)
        return 1;
    /* Released with hysteresis, so it cannot flap at exactly the ceiling. */
    return (temp_mc >= SAFETY_CEILING_MC - SAFETY_RELEASE_MC && cur >= max_state);
}

int fan_safety_clamp(int want, int cur, long temp_mc, int max_state)
{
    if (fan_safety_active(temp_mc, cur, max_state))
        return max_state;
    return want;
}

/* CURVE */

/* State the curve asks for at this temperature, ignoring hysteresis. */
static int target_at(const struct curve *c, long temp_mc)
{
    int st = 0;

    for (int i = 0; i < c->npt; i++)
        if (temp_mc >= c->pt[i].temp_mc)
            st = c->pt[i].state;
    return st;
}

/* 0 stays legal - the fan being off is a real state - but 1..min_state-1 is not, because
 * below state 2 the tachometer misreads (state 1 reports ~3172 rpm). Validation rejects a
 * point in that band, so this is a guard for a struct built in memory. */
static int clamp_min(int min_state, int st)
{
    if (st > 0 && st < min_state)
        return min_state;
    return st;
}

int curve_next(const struct curve *c, long temp_mc, int cur)
{
    int rising, falling;

    if (c->npt == 0)
        return cur;

    rising = clamp_min(c->min_state, target_at(c, temp_mc));
    if (rising > cur)
        return rising;                  /* hotter: step up at once, no hysteresis */

    /* Cooling: evaluate against a temperature raised by the hysteresis, so we only drop
     * once the real temperature has fallen a full hyst_mc below the threshold that put us
     * here. Without this the stock 38 C / 3 C arrangement oscillates once a minute. */
    falling = clamp_min(c->min_state, target_at(c, temp_mc + c->hyst_mc));
    if (falling < cur)
        return falling;
    return cur;
}

/* TARGET */

int target_next(const struct target *t, struct target_state *st,
                long temp_mc, int cur, long now_ms)
{
    int top = t->max_state ? t->max_state : TARGET_TOP_DEFAULT;
    long err = temp_mc - t->target_mc;
    long dtemp;
    int rising, falling, next = cur;

    dtemp = st->have_prev ? temp_mc - st->prev_temp_mc : 0;
    rising  = st->have_prev && dtemp > 0;
    falling = st->have_prev && dtemp < 0;
    st->prev_temp_mc = temp_mc;
    st->have_prev = 1;

    /* Inside the deadband nothing changes at all. */
    if (err <= t->deadband_mc && err >= -t->deadband_mc)
        return cur;

    /* Rate limit. The board takes far longer than one poll to respond; stepping every
     * poll crossed the whole range in 20 s and saturated before feedback arrived. */
    if (st->last_step_ms && now_ms - st->last_step_ms < t->step_ms)
        return cur;

    if (err > t->deadband_mc) {
        /* Too hot - but if it is ALREADY falling, the cooling in place is working and
         * adding more would overshoot. This is the anti-windup that the hardware run
         * showed was missing. */
        if (falling)
            return cur;
        if (cur < top)
            next = clamp_min(t->min_state, cur + 1);
    } else {
        /* Too cold - but if it is still rising, do not remove cooling yet. */
        if (rising)
            return cur;
        if (cur > 0) {
            next = cur - 1;
            /* Stepping down into the untrustworthy band goes straight to off rather than
             * to a state whose tachometer lies. */
            if (next > 0 && next < t->min_state)
                next = 0;
        }
    }
    if (next != cur)
        st->last_step_ms = now_ms;
    return next;
}
