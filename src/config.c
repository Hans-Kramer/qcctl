// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer
//
// Parser and validator. No writes, no filesystem access beyond reading the
// config file itself.

#define _GNU_SOURCE
#include "config.h"
#include "io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>

struct pstate {
    struct pconfig *c;
    int   line;
    int   in_profile;
};

static int fail(struct pconfig *c, int line, const char *fmt, ...)
{
    va_list ap;

    if (c->err[0])
        return -1;                      /* keep the first error */
    c->err_line = line;
    va_start(ap, fmt);
    vsnprintf(c->err, sizeof c->err, fmt, ap);
    va_end(ap);
    return -1;
}

/* Tokenise on whitespace, in place. */
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

/* Frequency with a MANDATORY unit, normalised to `want` ("kHz" or "Hz").
 * A bare number is rejected: both kHz and Hz appear on this board, so a unitless
 * number is ambiguous. Mixed units have cost two real bugs here. */
static int parse_freq(const char *s, const char *want, uint64_t *out, const char **why)
{
    char *end;
    uint64_t v;

    errno = 0;
    v = strtoull(s, &end, 10);
    if (end == s || errno || s[0] == '-') {
        *why = "not a number";
        return -1;
    }
    uint64_t hz;
    if (!strcasecmp(end, "khz"))      hz = v * 1000ULL;
    else if (!strcasecmp(end, "mhz")) hz = v * 1000000ULL;
    else if (!strcasecmp(end, "ghz")) hz = v * 1000000000ULL;
    else if (!strcasecmp(end, "hz"))  hz = v;
    else {
        *why = *end ? "unknown unit (use kHz, MHz, GHz or Hz)"
                    : "missing unit (use kHz, MHz, GHz or Hz)";
        return -1;
    }

    if (!strcmp(want, "kHz")) {
        /* CPU OPPs are kHz and several are not whole MHz (2361600 kHz =
         * 2361.6 MHz), so a value that is not an exact kHz cannot be meant. */
        if (hz % 1000ULL) {
            *why = "not an exact kHz value; use kHz for CPU frequencies";
            return -1;
        }
        *out = hz / 1000ULL;
    } else {
        *out = hz;
    }
    return 0;
}

/* Temperature with a mandatory C suffix, normalised to milli-C. */
static int parse_temp(const char *s, uint64_t *out, const char **why)
{
    char *end;
    long v;

    errno = 0;
    v = strtol(s, &end, 10);
    if (end == s || errno) {
        *why = "not a number";
        return -1;
    }
    if (strcasecmp(end, "c") != 0 && strcasecmp(end, "°c") != 0) {
        *why = *end ? "unknown unit (use C)" : "missing unit (use C, e.g. 45C)";
        return -1;
    }
    if (v < 0) {
        *why = "negative temperature";
        return -1;
    }
    *out = (uint64_t)v * 1000ULL;
    return 0;
}

static int parse_uint(const char *s, uint64_t *out, const char **why)
{
    char *end;
    uint64_t v;

    errno = 0;
    v = strtoull(s, &end, 10);
    /* strtoull silently wraps a leading '-'; an invalid count became a huge
     * positive in the sibling project and hung it. */
    if (end == s || *end || errno || s[0] == '-') {
        *why = "not a plain non-negative integer";
        return -1;
    }
    *out = v;
    return 0;
}

static struct action *new_act(struct pconfig *c, struct profile *p, int line)
{
    struct action *a;

    if (p->nact >= MAX_ACTIONS) {
        fail(c, line, "too many settings in profile '%s' (max %d)",
             p->name, MAX_ACTIONS);
        return NULL;
    }
    a = &p->act[p->nact++];
    memset(a, 0, sizeof *a);
    a->line = line;
    return a;
}

#define WANT(cond, ...) do { if (!(cond)) return fail(c, line, __VA_ARGS__); } while (0)

static int do_cpu_policy(struct pconfig *c, struct profile *p, char **t, int n, int line)
{
    uint64_t id;
    const char *why;

    WANT(n >= 2, "CPU_POLICY needs a policy id");
    WANT(parse_uint(t[1], &id, &why) == 0, "CPU_POLICY id: %s", why);

    /* Settings come in key/value pairs, hence i += 2. */
    for (int i = 2; i < n; i += 2) {
        struct action *a;
        WANT(i + 1 < n, "%s needs a value", t[i]);
        a = new_act(c, p, line);
        if (!a)
            return -1;
        a->target_id = (int)id;
        if (!strcasecmp(t[i], "GOVERNOR")) {
            a->kind = ACT_CPU_GOVERNOR;
            snprintf(a->sval, sizeof a->sval, "%.*s", NAME_LEN - 1, t[i + 1]);
        } else if (!strcasecmp(t[i], "MIN") || !strcasecmp(t[i], "MAX")
                || !strcasecmp(t[i], "SETSPEED")) {
            a->kind = !strcasecmp(t[i], "MIN") ? ACT_CPU_MIN
                    : !strcasecmp(t[i], "MAX") ? ACT_CPU_MAX : ACT_CPU_SETSPEED;
            WANT(parse_freq(t[i + 1], "kHz", &a->value, &why) == 0,
                 "CPU_POLICY %llu %s '%s': %s",
                 (unsigned long long)id, t[i], t[i + 1], why);
        } else {
            return fail(c, line, "unknown CPU_POLICY setting '%s'", t[i]);
        }
    }
    return 0;
}

static int do_gpu(struct pconfig *c, struct profile *p, char **t, int n, int line)
{
    const char *why;

    for (int i = 1; i < n; i += 2) {
        struct action *a;
        WANT(i + 1 < n, "GPU %s needs a value", t[i]);
        a = new_act(c, p, line);
        if (!a)
            return -1;
        if (!strcasecmp(t[i], "GOVERNOR")) {
            a->kind = ACT_GPU_GOVERNOR;
            snprintf(a->sval, sizeof a->sval, "%.*s", NAME_LEN - 1, t[i + 1]);
        } else if (!strcasecmp(t[i], "MIN") || !strcasecmp(t[i], "MAX")) {
            a->kind = !strcasecmp(t[i], "MIN") ? ACT_GPU_MIN : ACT_GPU_MAX;
            /* devfreq reports and accepts Hz. Reading it as kHz gives a
             * plausible-looking wrong number. */
            WANT(parse_freq(t[i + 1], "Hz", &a->value, &why) == 0,
                 "GPU %s '%s': %s", t[i], t[i + 1], why);
        } else {
            return fail(c, line, "unknown GPU setting '%s'", t[i]);
        }
    }
    return 0;
}

static int do_cooling(struct pconfig *c, struct profile *p, const char *type,
                      char **t, int n, int line)
{
    struct action *a;
    const char *why;

    WANT(n >= 2 && !strcasecmp(t[n - 2], "STATE"),
         "%s needs 'STATE <n>'", t[0]);
    a = new_act(c, p, line);
    if (!a)
        return -1;
    a->kind = ACT_COOLING_STATE;
    snprintf(a->target, sizeof a->target, "%.*s", NAME_LEN - 1, type);
    WANT(parse_uint(t[n - 1], &a->value, &why) == 0, "%s STATE: %s", t[0], why);
    return 0;
}

static int do_zone(struct pconfig *c, struct profile *p, char **t, int n, int line)
{
    const char *why;
    const char *zone;

    WANT(n >= 2, "THERMAL_ZONE needs a zone name");
    zone = t[1];

    /* Token count per setting varies here (TRIP consumes 3 or 5), so walk
     * explicitly rather than stepping a fixed stride. */
    for (int i = 2; i < n; ) {
        struct action *a;
        if (!strcasecmp(t[i], "TRIP")) {
            uint64_t idx;
            WANT(i + 3 < n, "TRIP needs '<index> TEMP <t>'");
            WANT(parse_uint(t[i + 1], &idx, &why) == 0, "TRIP index: %s", why);
            i += 2;
            /* TEMP and an optional HYST both attach to this trip. */
            while (i + 1 < n && (!strcasecmp(t[i], "TEMP") || !strcasecmp(t[i], "HYST"))) {
                a = new_act(c, p, line);
                if (!a)
                    return -1;
                a->kind = !strcasecmp(t[i], "TEMP") ? ACT_ZONE_TRIP_TEMP
                                                    : ACT_ZONE_TRIP_HYST;
                a->target_id = (int)idx;
                snprintf(a->target, sizeof a->target, "%.*s", NAME_LEN - 1, zone);
                WANT(parse_temp(t[i + 1], &a->value, &why) == 0,
                     "%s %s '%s': %s", zone, t[i], t[i + 1], why);
                i += 2;
            }
            continue;
        }
        WANT(i + 1 < n, "%s needs a value", t[i]);
        a = new_act(c, p, line);
        if (!a)
            return -1;
        snprintf(a->target, sizeof a->target, "%.*s", NAME_LEN - 1, zone);
        if (!strcasecmp(t[i], "MODE")) {
            a->kind = ACT_ZONE_MODE;
            snprintf(a->sval, sizeof a->sval, "%.*s", NAME_LEN - 1, t[i + 1]);
            WANT(!strcasecmp(a->sval, "enabled") || !strcasecmp(a->sval, "disabled"),
                 "MODE must be enabled or disabled, got '%s'", a->sval);
        } else if (!strcasecmp(t[i], "POLICY")) {
            a->kind = ACT_ZONE_POLICY;
            snprintf(a->sval, sizeof a->sval, "%.*s", NAME_LEN - 1, t[i + 1]);
        } else if (!strcasecmp(t[i], "SUSTAINABLE")) {
            a->kind = ACT_ZONE_SUSTAINABLE;
            WANT(parse_uint(t[i + 1], &a->value, &why) == 0, "SUSTAINABLE: %s", why);
        } else {
            return fail(c, line, "unknown THERMAL_ZONE setting '%s'", t[i]);
        }
        i += 2;
    }
    return 0;
}

static int do_line(struct pstate *st, char *line_buf, int line)
{
    struct pconfig *c = st->c;
    struct profile *p = st->in_profile ? &c->prof[c->nprof - 1] : NULL;
    char *t[32];
    const char *why;
    int n = tokenise(line_buf, t, 32);

    if (!n)
        return 0;

    if (!strcmp(t[0], "<")) {
        WANT(!st->in_profile, "nested PROFILE block");
        WANT(n >= 2 && !strcasecmp(t[1], "PROFILE"), "expected '< PROFILE ... >'");
        WANT(c->nprof < MAX_PROFILES, "too many profiles (max %d)", MAX_PROFILES);
        p = &c->prof[c->nprof++];
        memset(p, 0, sizeof *p);
        p->id = -1;
        p->line = line;
        for (int i = 2; i < n; i++) {
            if (!strcmp(t[i], ">"))
                break;
            if (!strncasecmp(t[i], "ID=", 3)) {
                uint64_t v;
                WANT(parse_uint(t[i] + 3, &v, &why) == 0, "PROFILE ID: %s", why);
                p->id = (int)v;
            } else if (!strncasecmp(t[i], "NAME=", 5)) {
                snprintf(p->name, sizeof p->name, "%.*s", NAME_LEN - 1, t[i] + 5);
            } else if (!strcasecmp(t[i], "DEFAULT")) {
                p->is_default = 1;
            } else if (!strcasecmp(t[i], "FALLBACK")) {
                /* Deliberately NOT called FAILSAFE: that word already means something
                 * else inside a CURVE block (what happens to the fan on release), and one
                 * name meaning two things in two scopes is how a name goes wrong. */
                p->is_fallback = 1;
            } else {
                return fail(c, line, "unknown PROFILE attribute '%s'", t[i]);
            }
        }
        WANT(p->name[0], "PROFILE needs NAME=");
        WANT(p->id >= 0, "PROFILE needs ID=");
        for (int i = 0; i < c->nprof - 1; i++) {
            WANT(strcmp(c->prof[i].name, p->name) != 0,
                 "duplicate profile name '%s' (also line %d)", p->name, c->prof[i].line);
            WANT(c->prof[i].id != p->id,
                 "duplicate profile ID=%d (also line %d)", p->id, c->prof[i].line);
        }
        st->in_profile = 1;
        return 0;
    }

    if (!strcmp(t[0], ">")) {
        WANT(st->in_profile, "'>' outside a PROFILE block");
        st->in_profile = 0;
        return 0;
    }

    WANT(st->in_profile, "'%s' outside a PROFILE block", t[0]);

    if (!strcasecmp(t[0], "CPU_POLICY"))
        return do_cpu_policy(c, p, t, n, line);
    if (!strcasecmp(t[0], "GPU"))
        return do_gpu(c, p, t, n, line);
    if (!strcasecmp(t[0], "FAN")) {
        struct action *a;
        /* FAN STATE n    - a one-off cooling state, kernel still owns the fan
         * FAN MODE x     - kernel | curve
         * FAN CURVE name - which curve from fan.config, when MODE is curve */
        if (n >= 2 && !strcasecmp(t[1], "MODE")) {
            WANT(n == 3, "FAN MODE needs kernel or curve");
            a = new_act(c, p, line);
            if (!a)
                return -1;
            a->kind = ACT_FAN_MODE;
            if (!strcasecmp(t[2], "kernel"))     a->value = FAN_MODE_KERNEL;
            else if (!strcasecmp(t[2], "curve")) a->value = FAN_MODE_CURVE;
            else return fail(c, line, "FAN MODE must be kernel or curve, got '%s'",
                             t[2]);
            return 0;
        }
        if (n >= 2 && !strcasecmp(t[1], "CURVE")) {
            WANT(n == 3, "FAN CURVE needs a curve name");
            a = new_act(c, p, line);
            if (!a)
                return -1;
            a->kind = ACT_FAN_CURVE;
            snprintf(a->target, sizeof a->target, "%.*s", NAME_LEN - 1, t[2]);
            return 0;
        }
        return do_cooling(c, p, "emc2305_fan", t, n, line);
    }
    if (!strcasecmp(t[0], "NPU"))
        return do_cooling(c, p, "cdsp:cdsp_sw", t, n, line);
    if (!strcasecmp(t[0], "COOLING")) {
        WANT(n >= 4, "COOLING needs '<type> STATE <n>'");
        return do_cooling(c, p, t[1], t, n, line);
    }
    if (!strcasecmp(t[0], "THERMAL_ZONE"))
        return do_zone(c, p, t, n, line);
    if (!strcasecmp(t[0], "CPU_ONLINE")) {
        struct action *a;
        uint64_t cpu, on;
        WANT(n == 3, "CPU_ONLINE needs '<cpu> <0|1>'");
        WANT(parse_uint(t[1], &cpu, &why) == 0, "CPU_ONLINE cpu: %s", why);
        WANT(parse_uint(t[2], &on, &why) == 0, "CPU_ONLINE value: %s", why);
        WANT(on <= 1, "CPU_ONLINE value must be 0 or 1");
        a = new_act(c, p, line);
        if (!a)
            return -1;
        a->kind = ACT_CPU_ONLINE;
        a->target_id = (int)cpu;
        a->value = on;
        return 0;
    }
    if (!strcasecmp(t[0], "CPUIDLE")) {
        struct action *a;
        uint64_t cpu, idx;
        WANT(n == 5 && !strcasecmp(t[2], "STATE"),
             "CPUIDLE needs '<cpu> STATE <i> <enabled|disabled>'");
        WANT(parse_uint(t[1], &cpu, &why) == 0, "CPUIDLE cpu: %s", why);
        WANT(parse_uint(t[3], &idx, &why) == 0, "CPUIDLE state: %s", why);
        a = new_act(c, p, line);
        if (!a)
            return -1;
        a->kind = ACT_CPUIDLE;
        a->target_id = (int)cpu;
        a->aux = (int)idx;
        if (!strcasecmp(t[4], "disabled"))      a->value = 1;
        else if (!strcasecmp(t[4], "enabled"))  a->value = 0;
        else return fail(c, line, "CPUIDLE must be enabled or disabled, got '%s'", t[4]);
        return 0;
    }
    if (!strcasecmp(t[0], "UCLAMP")) {
        for (int i = 1; i < n; i += 2) {
            struct action *a;
            WANT(i + 1 < n, "UCLAMP %s needs a value", t[i]);
            a = new_act(c, p, line);
            if (!a)
                return -1;
            if (!strcasecmp(t[i], "MIN"))      a->kind = ACT_UCLAMP_MIN;
            else if (!strcasecmp(t[i], "MAX")) a->kind = ACT_UCLAMP_MAX;
            else return fail(c, line, "unknown UCLAMP setting '%s'", t[i]);
            WANT(parse_uint(t[i + 1], &a->value, &why) == 0, "UCLAMP: %s", why);
            WANT(a->value <= 1024, "UCLAMP value must be 0..1024");
        }
        return 0;
    }
    return fail(c, line, "unknown directive '%s'", t[0]);
}

int config_parse_string(const char *text, struct pconfig *c)
{
    struct pstate st = { .c = c, .line = 0, .in_profile = 0 };
    const char *p = text;
    char buf[1024];
    int line = 0, ndefault = 0;

    memset(c, 0, sizeof *c);

    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char *hash;

        line++;
        if (len >= sizeof buf)
            len = sizeof buf - 1;
        memcpy(buf, p, len);
        buf[len] = '\0';
        hash = strchr(buf, '#');
        if (hash)
            *hash = '\0';
        if (do_line(&st, buf, line) < 0)
            return -1;
        if (!nl)
            break;
        p = nl + 1;
    }
    if (st.in_profile)
        return fail(c, line, "unterminated PROFILE block (missing '>')");
    for (int i = 0; i < c->nprof; i++)
        ndefault += c->prof[i].is_default;
    if (ndefault > 1)
        return fail(c, 0, "more than one profile marked DEFAULT");
    {
        int nfallback = 0;
        for (int i = 0; i < c->nprof; i++)
            nfallback += c->prof[i].is_fallback;
        if (nfallback > 1)
            return fail(c, 0, "more than one profile marked FALLBACK");
    }
    return 0;
}

int config_parse_file(const char *path, struct pconfig *c)
{
    char *text;
    long sz;
    FILE *f = fopen(path, "r");

    memset(c, 0, sizeof *c);
    if (!f)
        return fail(c, 0, "cannot open %s", path);
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > (1 << 20)) {
        fclose(f);
        return fail(c, 0, "config too large");
    }
    text = malloc((size_t)sz + 1);
    if (!text) {
        fclose(f);
        return fail(c, 0, "out of memory");
    }
    sz = (long)fread(text, 1, (size_t)sz, f);
    text[sz] = '\0';
    fclose(f);
    {
        int rc = config_parse_string(text, c);
        free(text);
        return rc;
    }
}

/* The profile marked DEFAULT, or NULL. Deliberately NOT folded into config_validate():
 * a config with no DEFAULT is still a VALID config, because `qcctld -m <name>` and
 * `qcctl -m <name>` never consult it - which is what the daemon's own "use -m" message
 * promises. It is only the AS-INSTALLED setup that requires one, the unit carrying no -m,
 * so the tools answering "will this work as installed?" check for it and the parser does
 * not. */
const struct profile *config_default(const struct pconfig *c)
{
    for (int i = 0; i < c->nprof; i++)
        if (c->prof[i].is_default)
            return &c->prof[i];
    return NULL;
}

const struct profile *config_find(const struct pconfig *c, const char *key)
{
    char *end;
    long id;

    for (int i = 0; i < c->nprof; i++)
        if (!strcmp(c->prof[i].name, key))
            return &c->prof[i];
    errno = 0;
    id = strtol(key, &end, 10);
    if (!errno && end != key && !*end)
        for (int i = 0; i < c->nprof; i++)
            if (c->prof[i].id == id)
                return &c->prof[i];
    return NULL;
}

/* ------------------------------------------------------------------ *
 * Validation. Every action is checked against what the board actually
 * exposes, before a single write is attempted.
 * ------------------------------------------------------------------ */

static int opp_has(const uint64_t *tab, int n, uint64_t v)
{
    for (int i = 0; i < n; i++)
        if (tab[i] == v)
            return 1;
    return 0;
}

static int gov_has(const char gov[][NAME_LEN], int n, const char *want)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(gov[i], want))
            return 1;
    return 0;
}

static const struct cpu_online *find_cpu(const struct surface *s, int cpu)
{
    for (int i = 0; i < s->ncpu; i++)
        if (s->cpu[i].cpu == cpu)
            return &s->cpu[i];
    return NULL;
}

#define VFAIL(...) do { snprintf(err, errlen, __VA_ARGS__); return -1; } while (0)

/* The one definition of what makes an action disruptive. Currently exactly one thing:
 * offlining cpu0, the boot CPU. It WORKS - confirmed on hardware, and it re-onlines
 * cleanly - so this is not a validity question, which is why the validator no longer
 * answers it. */
static int act_is_disruptive(const struct action *a)
{
    return a->kind == ACT_CPU_ONLINE && a->target_id == 0 && a->value == 0;
}

size_t config_disruptive(const struct profile *p, const struct action **disr, size_t maxdisr)
{
    size_t n = 0;

    if (!p)
        return 0;
    for (int i = 0; i < p->nact; i++) {
        if (!act_is_disruptive(&p->act[i]))
            continue;
        if (disr) {
            if (n == maxdisr)
                break;              /* the count is what was stored, never more */
            disr[n] = &p->act[i];
        }
        n++;
    }
    return n;
}

static int validate_action(const struct action *a, const struct surface *s,
                           char *err, size_t errlen)
{
    const struct policy *p;
    const struct cdev *cd;
    const struct zone *z;
    const struct cpu_online *cpu;

    switch (a->kind) {
    case ACT_CPU_GOVERNOR:
    case ACT_CPU_MIN:
    case ACT_CPU_MAX:
    case ACT_CPU_SETSPEED:
        p = surface_policy(s, a->target_id);
        if (!p)
            VFAIL("line %d: no cpufreq policy%d on this board", a->line, a->target_id);
        if (a->kind == ACT_CPU_GOVERNOR) {
            if (!gov_has(p->governors, p->ngov, a->sval))
                VFAIL("line %d: policy%d has no governor '%s'",
                      a->line, a->target_id, a->sval);
        } else {
            /* Frequencies must be a real OPP. The hardware silently rounds an
             * arbitrary value, so the machine would end up somewhere the config
             * does not describe. */
            if (p->nopp && !opp_has(p->opp_khz, p->nopp, a->value))
                VFAIL("line %d: %llu kHz is not an available frequency for policy%d",
                      a->line, (unsigned long long)a->value, a->target_id);
            if (a->kind == ACT_CPU_SETSPEED && knob_access(&p->k_setspeed) == KNOB_ABSENT)
                VFAIL("line %d: policy%d has no scaling_setspeed", a->line, a->target_id);
        }
        return 0;

    case ACT_CPU_ONLINE:
        cpu = find_cpu(s, a->target_id);
        if (!cpu)
            VFAIL("line %d: cpu%d does not exist", a->line, a->target_id);
        if (!cpu->present)
            VFAIL("line %d: cpu%d has no 'online' node", a->line, a->target_id);
        /* Offlining cpu0 is gated, not invalid - and the gate belongs to the profile that
         * asks for it, so it is config_disruptive()'s answer and not this function's. */
        return 0;

    case ACT_GPU_GOVERNOR:
    case ACT_GPU_MIN:
    case ACT_GPU_MAX:
        if (!s->gpu.present)
            VFAIL("line %d: no GPU devfreq node on this board", a->line);
        if (a->kind == ACT_GPU_GOVERNOR) {
            if (!gov_has(s->gpu.governors, s->gpu.ngov, a->sval))
                VFAIL("line %d: GPU has no governor '%s'", a->line, a->sval);
        } else if (s->gpu.nopp && !opp_has(s->gpu.opp_hz, s->gpu.nopp, a->value)) {
            VFAIL("line %d: %llu Hz is not an available GPU frequency",
                  a->line, (unsigned long long)a->value);
        }
        return 0;

    case ACT_COOLING_STATE:
        cd = surface_cdev(s, a->target);
        if (!cd)
            VFAIL("line %d: no cooling device '%s' on this board", a->line, a->target);
        if (cd->state_ok && a->value > cd->max_state)
            VFAIL("line %d: %s state %llu exceeds max_state %llu",
                  a->line, a->target, (unsigned long long)a->value,
                  (unsigned long long)cd->max_state);
        return 0;

    case ACT_ZONE_MODE:
    case ACT_ZONE_POLICY:
    case ACT_ZONE_SUSTAINABLE:
    case ACT_ZONE_TRIP_TEMP:
    case ACT_ZONE_TRIP_HYST:
        z = surface_zone(s, a->target);
        if (!z)
            VFAIL("line %d: no thermal zone '%s' on this board", a->line, a->target);
        if (a->kind == ACT_ZONE_TRIP_TEMP || a->kind == ACT_ZONE_TRIP_HYST) {
            int found = 0;
            long ceiling = 0;
            for (int i = 0; i < z->ntrips; i++) {
                if (z->trips[i].idx == a->target_id)
                    found = 1;
                if (z->trips[i].temp_mc > ceiling)
                    ceiling = z->trips[i].temp_mc;
            }
            if (!found)
                VFAIL("line %d: zone '%s' has no trip point %d",
                      a->line, a->target, a->target_id);
            /* Trips are safety features. A profile may lower one or raise it
             * within the zone's existing envelope, never above it. */
            if (a->kind == ACT_ZONE_TRIP_TEMP && (long)a->value > ceiling)
                VFAIL("line %d: zone '%s' trip %d: %ldC exceeds the zone's highest "
                      "existing trip (%ldC); thermal protections must not be raised",
                      a->line, a->target, a->target_id,
                      (long)a->value / 1000, ceiling / 1000);
        }
        return 0;

    case ACT_CPUIDLE:
        cpu = find_cpu(s, a->target_id);
        if (!cpu)
            VFAIL("line %d: cpu%d does not exist", a->line, a->target_id);
        if (a->aux >= cpu->nidle)
            VFAIL("line %d: cpu%d has %d cpuidle states, no state%d",
                  a->line, a->target_id, cpu->nidle, a->aux);
        return 0;

    case ACT_UCLAMP_MIN:
    case ACT_UCLAMP_MAX:
        return 0;

    /* The curve name cannot be checked here - that needs fan.config, which the
     * caller validates separately via fanconfig_*. Cross-field consistency is
     * checked in config_validate() below. */
    case ACT_FAN_MODE:
    case ACT_FAN_CURVE:
        return 0;
    }
    VFAIL("line %d: unhandled action", a->line);
}

int config_validate(const struct pconfig *c, const struct surface *s,
                    char *err, size_t errlen)
{
    if (errlen)
        err[0] = '\0';

    for (int i = 0; i < c->nprof; i++) {
        const struct profile *p = &c->prof[i];

        for (int j = 0; j < p->nact; j++)
            if (validate_action(&p->act[j], s, err, errlen) < 0)
                return -1;

        /* Cross-checks that only make sense once a whole profile is known. */
        for (int a = 0; a < p->nact; a++) {
            if (p->act[a].kind != ACT_CPU_MIN)
                continue;
            for (int b = 0; b < p->nact; b++)
                if (p->act[b].kind == ACT_CPU_MAX
                    && p->act[b].target_id == p->act[a].target_id
                    && p->act[b].value < p->act[a].value)
                    VFAIL("profile '%s': policy%d MIN %llu kHz exceeds MAX %llu kHz",
                          p->name, p->act[a].target_id,
                          (unsigned long long)p->act[a].value,
                          (unsigned long long)p->act[b].value);
        }
        /* THE UCLAMP PAIR. The kernel enforces min <= max on every write and returns
         * EINVAL rather than clamping, so an inconsistent pair is not a profile that
         * applies imperfectly - it is a profile that cannot be applied at all. This
         * check existed for the cpufreq pair above and not for this one, which is how
         * 'lowpower' shipped unappliable while --verify called it "applicable here". */
        {
            const struct action *umin = NULL, *umax = NULL;
            for (int a = 0; a < p->nact; a++) {
                if (p->act[a].kind == ACT_UCLAMP_MIN) umin = &p->act[a];
                if (p->act[a].kind == ACT_UCLAMP_MAX) umax = &p->act[a];
            }
            if (umin && umax && umin->value > umax->value)
                VFAIL("profile '%s': UCLAMP MIN %llu exceeds MAX %llu; the kernel "
                      "rejects the pair with EINVAL", p->name,
                      (unsigned long long)umin->value,
                      (unsigned long long)umax->value);
            /* ONE OF THE PAIR, AGAINST A LIVE VALUE THAT FORBIDS IT. The apply path
             * orders the pair min-first, which is all it can honestly do: moving the
             * OTHER clamp to make this one legal would write a value the profile never
             * asked for, and that write would belong to no profile and never be reverted
             * on a switch. So a profile that needs the other half moved has to say so,
             * and this is where it is told - at --verify time, instead of as an EINVAL
             * partway through an apply. */
            if (umax && !umin && s) {
                unsigned long long live_min;
                if (surface_uclamp(&s->k_uclamp_min, &live_min) == 0
                    && umax->value < live_min)
                    VFAIL("profile '%s' line %d: UCLAMP MAX %llu is below the live "
                          "sched_util_clamp_min of %llu, so the kernel rejects the "
                          "write with EINVAL; add 'UCLAMP MIN %llu' to this profile "
                          "so the pair moves together and is restored on a switch",
                          p->name, umax->line, (unsigned long long)umax->value,
                          live_min, (unsigned long long)umax->value);
            }
            if (umin && !umax && s) {
                unsigned long long live_max;
                if (surface_uclamp(&s->k_uclamp_max, &live_max) == 0
                    && umin->value > live_max)
                    VFAIL("profile '%s' line %d: UCLAMP MIN %llu exceeds the live "
                          "sched_util_clamp_max of %llu, so the kernel rejects the "
                          "write with EINVAL; add 'UCLAMP MAX %llu' to this profile "
                          "so the pair moves together and is restored on a switch",
                          p->name, umin->line, (unsigned long long)umin->value,
                          live_max, (unsigned long long)umin->value);
            }
        }

        /* FAN MODE and FAN CURVE must agree. Naming a curve without asking for
         * curve mode would silently do nothing; asking for curve mode without a
         * curve leaves the daemon owning the fan with no rule to follow - which is
         * worse than not taking it over at all. */
        {
            int mode = -1, has_curve = 0, mode_line = 0, curve_line = 0;
            for (int a = 0; a < p->nact; a++) {
                if (p->act[a].kind == ACT_FAN_MODE) {
                    mode = (int)p->act[a].value;
                    mode_line = p->act[a].line;
                } else if (p->act[a].kind == ACT_FAN_CURVE) {
                    has_curve = 1;
                    curve_line = p->act[a].line;
                }
            }
            if (has_curve && mode != FAN_MODE_CURVE)
                VFAIL("profile '%s' line %d: FAN CURVE has no effect without "
                      "FAN MODE curve", p->name, curve_line);
            if (mode == FAN_MODE_CURVE && !has_curve)
                VFAIL("profile '%s' line %d: FAN MODE curve needs a FAN CURVE; "
                      "owning the fan with no curve to follow is worse than "
                      "leaving it to the kernel", p->name, mode_line);
        }

        for (int a = 0; a < p->nact; a++) {
            if (p->act[a].kind != ACT_GPU_MIN)
                continue;
            for (int b = 0; b < p->nact; b++)
                if (p->act[b].kind == ACT_GPU_MAX
                    && p->act[b].value < p->act[a].value)
                    VFAIL("profile '%s': GPU MIN %llu Hz exceeds MAX %llu Hz",
                          p->name, (unsigned long long)p->act[a].value,
                          (unsigned long long)p->act[b].value);
        }
    }
    return 0;
}

const char *act_describe(const struct action *a, char *buf, size_t len)
{
    switch (a->kind) {
    case ACT_CPU_GOVERNOR:
        snprintf(buf, len, "policy%d governor      = %s", a->target_id, a->sval); break;
    case ACT_CPU_MIN:
        snprintf(buf, len, "policy%d scaling_min   = %llu kHz", a->target_id,
                 (unsigned long long)a->value); break;
    case ACT_CPU_MAX:
        snprintf(buf, len, "policy%d scaling_max   = %llu kHz", a->target_id,
                 (unsigned long long)a->value); break;
    case ACT_CPU_SETSPEED:
        snprintf(buf, len, "policy%d setspeed      = %llu kHz", a->target_id,
                 (unsigned long long)a->value); break;
    case ACT_CPU_ONLINE:
        snprintf(buf, len, "cpu%d online          = %llu", a->target_id,
                 (unsigned long long)a->value); break;
    case ACT_GPU_GOVERNOR:
        snprintf(buf, len, "gpu governor          = %s", a->sval); break;
    case ACT_GPU_MIN:
        snprintf(buf, len, "gpu min_freq          = %llu Hz", (unsigned long long)a->value); break;
    case ACT_GPU_MAX:
        snprintf(buf, len, "gpu max_freq          = %llu Hz", (unsigned long long)a->value); break;
    case ACT_COOLING_STATE:
        snprintf(buf, len, "cooling %-14s cur_state = %llu", a->target,
                 (unsigned long long)a->value); break;
    case ACT_ZONE_MODE:
        snprintf(buf, len, "zone %-12s mode     = %s", a->target, a->sval); break;
    case ACT_ZONE_POLICY:
        snprintf(buf, len, "zone %-12s policy   = %s", a->target, a->sval); break;
    case ACT_ZONE_SUSTAINABLE:
        snprintf(buf, len, "zone %-12s sustainable_power = %llu", a->target,
                 (unsigned long long)a->value); break;
    case ACT_ZONE_TRIP_TEMP:
        snprintf(buf, len, "zone %-12s trip%d temp = %lldC", a->target, a->target_id,
                 (long long)a->value / 1000); break;
    case ACT_ZONE_TRIP_HYST:
        snprintf(buf, len, "zone %-12s trip%d hyst = %lldC", a->target, a->target_id,
                 (long long)a->value / 1000); break;
    case ACT_CPUIDLE:
        snprintf(buf, len, "cpu%d cpuidle state%d disable = %llu", a->target_id,
                 a->aux, (unsigned long long)a->value); break;
    case ACT_UCLAMP_MIN:
        snprintf(buf, len, "sched_util_clamp_min  = %llu", (unsigned long long)a->value); break;
    case ACT_UCLAMP_MAX:
        snprintf(buf, len, "sched_util_clamp_max  = %llu", (unsigned long long)a->value); break;
    case ACT_FAN_MODE:
        snprintf(buf, len, "fan ownership         = %s",
                 a->value == FAN_MODE_CURVE ? "curve (daemon)" : "kernel"); break;
    case ACT_FAN_CURVE:
        snprintf(buf, len, "fan curve             = %s", a->target); break;
    default:
        snprintf(buf, len, "<unknown action>"); break;
    }
    return buf;
}
