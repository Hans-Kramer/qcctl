// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer

#define _GNU_SOURCE
#include "apply.h"
#include "io.h"
#include "envid.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>

/* Ordering buckets. Within a bucket, config order is preserved.
 *
 * Thermal comes before cooling on purpose: the thermal governor overwrites a
 * cooling device's cur_state whenever it re-evaluates, so a profile that pins the
 * fan must be able to disable or retune the zone first. That is exactly how the
 * fan behaves on this board - its zone is IRQ-driven and re-asserts state 0 or 4.
 *
 * CPU hotplug is last because it is the most disruptive and the least reversible
 * within a single apply. */
enum {
    PH_CPUFREQ = 0,
    PH_GPU     = 1,
    PH_SCHED   = 2,
    PH_CPUIDLE = 3,
    PH_THERMAL = 4,
    PH_COOLING = 5,
    PH_HOTPLUG = 6,
};

static int phase_of(enum act_kind k)
{
    switch (k) {
    case ACT_CPU_GOVERNOR: case ACT_CPU_MIN:
    case ACT_CPU_MAX:      case ACT_CPU_SETSPEED:  return PH_CPUFREQ;
    case ACT_GPU_GOVERNOR: case ACT_GPU_MIN:
    case ACT_GPU_MAX:                              return PH_GPU;
    case ACT_UCLAMP_MIN:   case ACT_UCLAMP_MAX:    return PH_SCHED;
    case ACT_CPUIDLE:                              return PH_CPUIDLE;
    case ACT_ZONE_MODE:    case ACT_ZONE_POLICY:
    case ACT_ZONE_TRIP_TEMP: case ACT_ZONE_TRIP_HYST:
    case ACT_ZONE_SUSTAINABLE:                     return PH_THERMAL;
    case ACT_COOLING_STATE:                        return PH_COOLING;
    case ACT_CPU_ONLINE:                           return PH_HOTPLUG;
    case ACT_FAN_MODE: case ACT_FAN_CURVE:         return PH_COOLING;
    }
    return PH_CPUFREQ;
}

#define PFAIL(...) do { snprintf(err, errlen, __VA_ARGS__); return -1; } while (0)

static struct wr *addw(struct applyctx *c, int phase, const struct action *a,
                       char *err, size_t errlen)
{
    struct wr *w;

    if (c->nw >= MAX_SAVED) {
        snprintf(err, errlen, "too many writes in one profile (max %d)", MAX_SAVED);
        return NULL;
    }
    w = &c->w[c->nw++];
    memset(w, 0, sizeof *w);
    w->phase = phase;
    w->act = a;
    (void)err; (void)errlen;
    return w;
}

static void setw(struct wr *w, const char *path, const char *fmt, ...)
{
    va_list ap;

    snprintf(w->path, sizeof w->path, "%s", path);
    va_start(ap, fmt);
    vsnprintf(w->value, sizeof w->value, fmt, ap);
    va_end(ap);
}

/* WHICH ORDER THE UCLAMP PAIR MUST BE WRITTEN IN, and it depends on which way the pair
 * is MOVING. The kernel checks every write against the clamp currently in force, not
 * against the end state the profile describes:
 *
 *   lowering the pair (target min <= live max) -> min first, then max
 *   raising  the pair (target min >  live max) -> max first, then min
 *
 * Writing min first unconditionally is not enough, and hardware said so: applying
 * 'lowpower' (1024/1024 -> 512/512) needs min first, but switching AWAY from it
 * (512/512 -> 1024/1024) needs max first, and the revert wrote min=1024 against a max
 * still at 512. The kernel rejected it, apply_orphans() does not look at write failures,
 * and sched_util_clamp_min silently stayed at 512 for the rest of the boot.
 *
 * Unknown live max -> keep the declared order; a guess is not a measurement, and the
 * write then fails loudly instead of being reordered on a belief. */
static int uclamp_min_first(unsigned long long target_min, const struct surface *s)
{
    unsigned long long live_max;

    if (surface_uclamp(&s->k_uclamp_max, &live_max) != 0)
        return 1;
    return target_min <= live_max;
}

int apply_plan(const struct profile *p, const struct surface *s,
               struct applyctx *ctx, char *err, size_t errlen)
{
    int dry = ctx->dry;

    memset(ctx, 0, sizeof *ctx);
    ctx->dry = dry;
    if (errlen)
        err[0] = '\0';

    for (int i = 0; i < p->nact; i++) {
        const struct action *a = &p->act[i];
        char base[PATH_MAXLEN];
        struct wr *w;

        switch (a->kind) {
        case ACT_CPU_GOVERNOR:
        case ACT_CPU_MIN:
        case ACT_CPU_MAX:
        case ACT_CPU_SETSPEED: {
            const char *attr =
                a->kind == ACT_CPU_GOVERNOR ? "scaling_governor" :
                a->kind == ACT_CPU_MIN      ? "scaling_min_freq" :
                a->kind == ACT_CPU_MAX      ? "scaling_max_freq" : "scaling_setspeed";
            spath(base, sizeof base, CPUFREQ_DIR "/policy%d/%s", a->target_id, attr);
            w = addw(ctx, phase_of(a->kind), a, err, errlen);
            if (!w)
                return -1;
            if (a->kind == ACT_CPU_GOVERNOR)
                setw(w, base, "%s", a->sval);
            else
                setw(w, base, "%llu", (unsigned long long)a->value);
            break;
        }
        case ACT_CPU_ONLINE:
            spath(base, sizeof base, SYS_CPU "/cpu%d/online", a->target_id);
            w = addw(ctx, phase_of(a->kind), a, err, errlen);
            if (!w)
                return -1;
            setw(w, base, "%llu", (unsigned long long)a->value);
            break;

        case ACT_GPU_GOVERNOR:
        case ACT_GPU_MIN:
        case ACT_GPU_MAX: {
            const char *attr = a->kind == ACT_GPU_GOVERNOR ? "governor" :
                               a->kind == ACT_GPU_MIN      ? "min_freq" : "max_freq";
            if (!s->gpu.present)
                PFAIL("line %d: no GPU devfreq node", a->line);
            snprintf(base, sizeof base, "%s/%s", s->gpu.dir, attr);
            w = addw(ctx, phase_of(a->kind), a, err, errlen);
            if (!w)
                return -1;
            if (a->kind == ACT_GPU_GOVERNOR)
                setw(w, base, "%s", a->sval);
            else
                setw(w, base, "%llu", (unsigned long long)a->value);
            break;
        }
        case ACT_COOLING_STATE: {
            const struct cdev *cd = surface_cdev(s, a->target);
            if (!cd)
                PFAIL("line %d: no cooling device '%s'", a->line, a->target);
            spath(base, sizeof base, THERMAL_DIR "/cooling_device%d/cur_state", cd->idx);
            w = addw(ctx, phase_of(a->kind), a, err, errlen);
            if (!w)
                return -1;
            setw(w, base, "%llu", (unsigned long long)a->value);
            break;
        }
        case ACT_ZONE_MODE:
        case ACT_ZONE_POLICY:
        case ACT_ZONE_SUSTAINABLE:
        case ACT_ZONE_TRIP_TEMP:
        case ACT_ZONE_TRIP_HYST: {
            const struct zone *z = surface_zone(s, a->target);
            char attr[64];
            if (!z)
                PFAIL("line %d: no thermal zone '%s'", a->line, a->target);
            switch (a->kind) {
            case ACT_ZONE_MODE:        snprintf(attr, sizeof attr, "mode"); break;
            case ACT_ZONE_POLICY:      snprintf(attr, sizeof attr, "policy"); break;
            case ACT_ZONE_SUSTAINABLE: snprintf(attr, sizeof attr, "sustainable_power"); break;
            case ACT_ZONE_TRIP_TEMP:
                snprintf(attr, sizeof attr, "trip_point_%d_temp", a->target_id); break;
            default:
                snprintf(attr, sizeof attr, "trip_point_%d_hyst", a->target_id); break;
            }
            spath(base, sizeof base, THERMAL_DIR "/thermal_zone%d/%s", z->idx, attr);
            w = addw(ctx, phase_of(a->kind), a, err, errlen);
            if (!w)
                return -1;
            if (a->kind == ACT_ZONE_MODE || a->kind == ACT_ZONE_POLICY)
                setw(w, base, "%s", a->sval);
            else
                setw(w, base, "%llu", (unsigned long long)a->value);
            break;
        }
        case ACT_CPUIDLE:
            spath(base, sizeof base, SYS_CPU "/cpu%d/cpuidle/state%d/disable",
                  a->target_id, a->aux);
            w = addw(ctx, phase_of(a->kind), a, err, errlen);
            if (!w)
                return -1;
            setw(w, base, "%llu", (unsigned long long)a->value);
            break;

        /* Fan ownership is daemon state, not a sysfs attribute: there is nothing
         * to write and nothing to roll back. The daemon reads these from the
         * profile and runs the curve itself. */
        case ACT_FAN_MODE:
        case ACT_FAN_CURVE:
            continue;

        case ACT_UCLAMP_MIN:
        case ACT_UCLAMP_MAX:
            spath(base, sizeof base, "/proc/sys/kernel/sched_util_clamp_%s",
                  a->kind == ACT_UCLAMP_MIN ? "min" : "max");
            w = addw(ctx, phase_of(a->kind), a, err, errlen);
            if (!w)
                return -1;
            setw(w, base, "%llu", (unsigned long long)a->value);
            break;
        }
    }

    /* Stable sort: qsort is not stable, so bucket into phases explicitly. */
    {
        struct wr tmp[MAX_SAVED];
        int n = 0;
        for (int ph = PH_CPUFREQ; ph <= PH_HOTPLUG; ph++)
            for (int i = 0; i < ctx->nw; i++)
                if (ctx->w[i].phase == ph)
                    tmp[n++] = ctx->w[i];
        memcpy(ctx->w, tmp, (size_t)n * sizeof tmp[0]);
    }

    /* A policy setting both MIN and MAX can transiently violate min <= max,
     * which sysfs clamps silently - leaving a value the config never asked for.
     * Dropping min to the policy floor first makes every intermediate state
     * valid. Cheap: one extra write per policy that sets both. */
    for (int pol = 0; pol < s->npol; pol++) {
        const struct policy *pp = &s->pol[pol];
        int has_min = 0, has_max = 0, first = -1;

        for (int i = 0; i < ctx->nw; i++) {
            if (!ctx->w[i].act || ctx->w[i].act->target_id != pp->id)
                continue;
            if (ctx->w[i].act->kind == ACT_CPU_MIN) { has_min = 1; if (first < 0) first = i; }
            if (ctx->w[i].act->kind == ACT_CPU_MAX) { has_max = 1; if (first < 0) first = i; }
        }
        if (!(has_min && has_max) || first < 0 || ctx->nw >= MAX_SAVED)
            continue;
        memmove(&ctx->w[first + 1], &ctx->w[first],
                (size_t)(ctx->nw - first) * sizeof ctx->w[0]);
        ctx->nw++;
        memset(&ctx->w[first], 0, sizeof ctx->w[0]);
        ctx->w[first].phase = PH_CPUFREQ;
        ctx->w[first].act = NULL;       /* synthesised, not from the config */
        spath(ctx->w[first].path, sizeof ctx->w[first].path,
              CPUFREQ_DIR "/policy%d/scaling_min_freq", pp->id);
        snprintf(ctx->w[first].value, sizeof ctx->w[first].value, "%llu",
                 (unsigned long long)pp->cpuinfo_min_khz);
    }

    /* THE SAME min <= max TRAP ON THE UCLAMP PAIR, one level nastier: the kernel does
     * not clamp a bad uclamp write silently, it REJECTS it with EINVAL. So the pair must
     * be written min FIRST - with min <= max already guaranteed by config_validate(),
     * that order makes every intermediate state legal whichever order the config lists
     * them in.
     *
     * A SYNTHESISED min write WAS TRIED HERE AND IS WRONG. Dropping min to the planned
     * max (the cpufreq trick above) let a MAX-only profile apply, but the synthetic write
     * belongs to no profile, so switching away never reverted it: 'lowpower' left
     * sched_util_clamp_min at 512 for the rest of the boot. The round-trip suite caught
     * it as a profile-switch idempotency failure - state has to depend on WHICH profile
     * is active, not on the path taken to it. A profile that needs min moved must say so,
     * and config_validate() now refuses one that needs it without saying so. */
    {
        int imin = -1, imax = -1;

        for (int i = 0; i < ctx->nw; i++) {
            if (!ctx->w[i].act)
                continue;
            if (ctx->w[i].act->kind == ACT_UCLAMP_MIN && imin < 0) imin = i;
            if (ctx->w[i].act->kind == ACT_UCLAMP_MAX && imax < 0) imax = i;
        }
        if (imin >= 0 && imax >= 0) {
            int want_min_first = uclamp_min_first(ctx->w[imin].act->value, s);
            if ((want_min_first && imin > imax) || (!want_min_first && imax > imin)) {
                struct wr t = ctx->w[imin];
                ctx->w[imin] = ctx->w[imax];
                ctx->w[imax] = t;
            }
        }
    }

    /* Every planned path must EXIST before we write a single byte.
     *
     * Without this, a profile naming an attribute the kernel no longer exposes planned
     * cleanly, --dry-run reported "no writes performed", and only the real apply failed -
     * partway through, relying on rollback. That is the scenario a new board image creates:
     * a renamed or dropped knob. A dry-run that cannot predict the failure has no reason to
     * exist, and failing here means nothing is written at all rather than written and undone.
     *
     * access(F_OK), never a read: a write-only attribute exists but is unreadable. */
    {
        int missing = 0;
        size_t used = 0;
        char list[384];

        list[0] = '\0';
        for (int i = 0; i < ctx->nw; i++) {
            if (path_present(ctx->w[i].path))
                continue;
            missing++;
            if (used < sizeof list - 1) {
                int n = snprintf(list + used, sizeof list - used, "%s%s",
                                 used ? ", " : "", ctx->w[i].path);
                if (n > 0)
                    used += (size_t)n;
            }
        }
        if (missing)
            PFAIL("profile '%s': %d setting%s target%s a path this kernel does not expose: %s",
                  p->name, missing, missing == 1 ? "" : "s",
                  missing == 1 ? "s" : "", list);
    }

    return 0;
}

static int save_one(struct applyctx *c, const char *path)
{
    struct saved *s;

    if (c->nsv >= MAX_SAVED)
        return -1;
    s = &c->sv[c->nsv++];
    snprintf(s->path, sizeof s->path, "%s", path);
    s->value[0] = '\0';
    s->had_value = (read_file_str(path, s->value, sizeof s->value) == 0);
    return 0;
}

int apply_commit(struct applyctx *ctx, char *err, size_t errlen)
{
    if (errlen)
        err[0] = '\0';

    for (int i = 0; i < ctx->nw; i++) {
        if (save_one(ctx, ctx->w[i].path) < 0)
            PFAIL("cannot capture prior state (too many writes)");

        if (ctx->dry)
            continue;

        if (write_path_str(ctx->w[i].path, ctx->w[i].value) < 0) {
            int e = errno;
            char rerr[ERRLEN];
            /* Undo what we already did. A half-applied profile leaves the machine
             * in a state no config describes. */
            apply_rollback(ctx, rerr, sizeof rerr);
            PFAIL("write '%s' to %s failed: %s (rolled back %d earlier write%s)",
                  ctx->w[i].value, ctx->w[i].path, strerror(e),
                  i, i == 1 ? "" : "s");
        }
    }
    return 0;
}

int apply_rollback(struct applyctx *ctx, char *err, size_t errlen)
{
    int failed = 0;

    if (errlen)
        err[0] = '\0';
    /* Newest first: undoing in reverse is what makes the min/max dance unwind
     * through the same valid intermediate states it went out through. */
    for (int i = ctx->nsv - 1; i >= 0; i--) {
        if (!ctx->sv[i].had_value)
            continue;           /* nothing readable to restore; not an error */
        if (ctx->dry)
            continue;
        if (write_path_str(ctx->sv[i].path, ctx->sv[i].value) < 0)
            failed++;
    }
    if (failed)
        snprintf(err, errlen, "%d value%s could not be restored",
                 failed, failed == 1 ? "" : "s");
    ctx->nsv = 0;
    return failed ? -1 : 0;
}

/* ======================================================== */
/*                  whole-surface snapshot                  */
/* ======================================================== */
/* Written as "path<TAB>value" lines. Deliberately plain text: a --reset that
 * cannot be inspected by eye is a --reset nobody will trust. */
static void snap_line(FILE *f, const char *path)
{
    char v[NAME_LEN];

    if (read_file_str(path, v, sizeof v) == 0)
        fprintf(f, "%s\t%s\n", path, v);
}

/* Does a literal path exist? Not through spath(): used for the snapshot file, whose
 * location is given explicitly on the command line. */
static int file_exists_plain(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

/* The boot this kernel is running. Used to keep exactly ONE boot-state snapshot per
 * boot - see state_save_once(). Returns 0 and an empty id if it cannot be read. */
static void current_boot_id(char *out, size_t len)
{
    out[0] = '\0';
    /* Deliberately NOT through spath(): the boot id is a property of the running kernel,
     * not of the control surface under test, so it must not be redirected into a fixture.
     * This costs no testability - a test simulates "a different boot" by writing a
     * different id into a snapshot HEADER, which it controls completely.
     *
     * envid_read() goes the other way and DOES use spath(), which is not an inconsistency:
     * there the question is "was this baseline measured in a different environment", so a
     * test has to be able to present one. Here the question is "is this the same boot",
     * and an answer a fixture could forge would be worthless. */
    if (read_file_str("/proc/sys/kernel/random/boot_id", out, len) < 0)
        out[0] = '\0';
}

/* Read the boot id a snapshot was taken under, from its header. */
static void snapshot_boot_id(const char *path, char *out, size_t len)
{
    char line[256];
    FILE *f;

    out[0] = '\0';
    f = fopen(path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "# boot_id ", 10) == 0) {
            size_t n = strcspn(line + 10, "\r\n");
            if (n < len) { memcpy(out, line + 10, n); out[n] = '\0'; }
            break;
        }
        if (line[0] != '#')
            break;                      /* header is over */
    }
    fclose(f);
}

/* ---- the operator's PROFILE SELECTION, as distinct from what is in force ----
 *
 * /run/qcctl/active says "a daemon is running and this is in force", and is unlinked when
 * the daemon exits - which is what keeps the resume hook from re-applying a profile when
 * nothing is running, and what keeps an unprivileged REBASELINE unable to choose one.
 *
 * This file answers a different question: "what did the operator ASK for". It survives the
 * daemon exiting, so a restart can honour the choice instead of silently reverting to
 * DEFAULT - which is what happened before, and left a board running a profile nobody
 * selected after any sampler flap.
 *
 * Tagged with the boot id and kept in /var/lib, which gives the scope deliberately: a
 * daemon restart honours it, a POWER CYCLE does not. That bound is the safety argument -
 * a selection that destabilises the board cannot be re-applied forever by a restart loop,
 * because one power cycle returns the machine to DEFAULT. */
int selection_save(const char *path, const char *profile)
{
    char now[64], buf[160];

    current_boot_id(now, sizeof now);
    if (!now[0])
        return -1;                      /* no id means no scope; record nothing */
    if (snprintf(buf, sizeof buf, "# boot_id %s\n%s", now, profile) >= (int)sizeof buf)
        return -1;
    return write_new_file(path, buf);
}

/* 1 = a selection from THIS boot is in `out`. 0 = nothing recorded, or from another boot.
 * The caller decides what either means; this only reports what is on disk. */
int selection_load(const char *path, char *out, size_t len)
{
    char now[64], was[64], line[256];
    FILE *f;

    out[0] = '\0';
    current_boot_id(now, sizeof now);
    snapshot_boot_id(path, was, sizeof was);
    /* An unreadable current id is NOT treated as a match: we cannot prove the same boot,
     * and inventing a match is how a stale selection would outlive its scope. */
    if (!now[0] || !was[0] || strcmp(now, was) != 0)
        return 0;

    f = fopen(path, "r");
    if (!f)
        return 0;
    while (fgets(line, sizeof line, f)) {
        size_t n;
        if (line[0] == '#')
            continue;                   /* still in the header */
        n = strcspn(line, "\r\n");
        if (n == 0 || n >= len)
            break;                      /* blank or absurd: nothing usable */
        memcpy(out, line, n);
        out[n] = '\0';
        fclose(f);
        return 1;
    }
    fclose(f);
    return 0;
}

/* Snapshot the surface, but only ONCE per boot.
 *
 * state_save() overwrites unconditionally, which is right for an explicit
 * `--save-state`. It is wrong for the daemon: the daemon runs, snapshots, and then
 * APPLIES a profile, so a second start in the same boot would record our own applied
 * values as "boot state" and --reset would restore them instead of stock - silently
 * turning both --reset and the uninstaller's stock-restore into no-ops. A restart or a
 * single Restart=on-failure is enough to trigger it.
 *
 * If the boot id cannot be determined we keep any existing snapshot rather than
 * replacing it: preserving a possibly-stale snapshot is recoverable, destroying the only
 * record of stock is not. Reports via *skipped whether it wrote.
 */
int state_save_once(const struct surface *s, const char *path, int *skipped,
                    char *err, size_t errlen)
{
    char now[64], was[64];

    if (skipped)
        *skipped = 0;
    current_boot_id(now, sizeof now);
    snapshot_boot_id(path, was, sizeof was);

    /* Overwrite ONLY when we can prove the snapshot came from a different boot.
     * Everything else keeps what is there: same boot, an unreadable current boot id, or
     * an untagged snapshot written by an older build. An untagged file is the dangerous
     * case - it is most likely the true stock record - and losing it is unrecoverable,
     * while keeping a stale one is not. */
    if (file_exists_plain(path)) {
        int different = (was[0] != '\0' && now[0] != '\0' && strcmp(now, was) != 0);
        if (!different) {
            if (skipped)
                *skipped = 1;
            if (errlen)
                err[0] = '\0';
            return 0;
        }
    }
    return state_save_tagged(s, path, now, err, errlen);
}

/* Look up one path in a boot-state snapshot. The file is "path\tvalue" lines with a few
 * leading comments; a linear scan is fine - a profile orphans a handful of paths and the
 * snapshot is a few hundred lines. */
static int snapshot_value(const char *statefile, const char *path,
                          char *out, size_t outlen)
{
    char line[PATH_MAXLEN + NAME_LEN + 8];
    FILE *f = fopen(statefile, "r");
    size_t plen = strlen(path);
    int found = 0;

    if (!f)
        return 0;
    while (fgets(line, sizeof line, f)) {
        char *tab, *nl;
        if (line[0] == '#')
            continue;
        tab = strchr(line, '\t');
        if (!tab)
            continue;
        if ((size_t)(tab - line) != plen || strncmp(line, path, plen))
            continue;
        nl = strchr(tab + 1, '\n');
        if (nl)
            *nl = '\0';
        snprintf(out, outlen, "%s", tab + 1);
        found = 1;
        break;
    }
    fclose(f);
    return found;
}

static int ctx_writes_path(const struct applyctx *ctx, const char *path)
{
    for (int i = 0; i < ctx->nw; i++)
        if (!strcmp(ctx->w[i].path, path))
            return 1;
    return 0;
}

int apply_orphans(const struct pconfig *c, const char *prev_name,
                  const struct applyctx *newctx, const struct surface *s,
                  const char *statefile, int dry,
                  char *err, size_t errlen, int *nreverted)
{
    /* Static: struct applyctx is large and both callers are single-threaded, so BSS keeps
     * the stack frame small. */
    static struct applyctx prevctx;
    const struct profile *prev;
    char want[NAME_LEN];
    char firstfail[PATH_MAXLEN + 96];
    int n = 0, nfail = 0;

    firstfail[0] = '\0';

    if (errlen)
        err[0] = '\0';
    if (nreverted)
        *nreverted = 0;
    if (!prev_name || !*prev_name)
        return 0;                       /* nothing was in force */

    prev = config_find(c, prev_name);
    if (!prev)
        return 0;                       /* the old profile is gone from the config */
    memset(&prevctx, 0, sizeof prevctx);
    if (apply_plan(prev, s, &prevctx, err, errlen) < 0)
        return -1;

    /* A REVERT MOVES THE UCLAMP PAIR THE OTHER WAY, so it needs the other order. The
     * plan was built for APPLYING prev; restoring it walks the same writes with the
     * SNAPSHOT's values, and those are the boot values - usually higher. Reorder against
     * the restore target, not the plan's. Hardware caught this: switching away from
     * 'lowpower' left sched_util_clamp_min at 512 because min=1024 was written first and
     * rejected against a max still at 512. */
    {
        int imin = -1, imax = -1;
        char wantmin[NAME_LEN];

        for (int i = 0; i < prevctx.nw; i++) {
            if (!strstr(prevctx.w[i].path, "sched_util_clamp_"))
                continue;
            if (strstr(prevctx.w[i].path, "_min") && imin < 0) imin = i;
            if (strstr(prevctx.w[i].path, "_max") && imax < 0) imax = i;
        }
        if (imin >= 0 && imax >= 0
            && snapshot_value(statefile, prevctx.w[imin].path, wantmin, sizeof wantmin)) {
            int want_min_first = uclamp_min_first(strtoull(wantmin, NULL, 10), s);
            if ((want_min_first && imin > imax) || (!want_min_first && imax > imin)) {
                struct wr t = prevctx.w[imin];
                prevctx.w[imin] = prevctx.w[imax];
                prevctx.w[imax] = t;
            }
        }
    }

    for (int i = 0; i < prevctx.nw; i++) {
        const char *path = prevctx.w[i].path;

        if (ctx_writes_path(newctx, path))
            continue;                   /* the new profile owns it too */
        /* cooling_device cur_state is the governor's OUTPUT, never configuration - the same
         * exclusion state_restore() makes. Replaying it can leave a board uncooled (11j). */
        if (strstr(path, "/cooling_device") && strstr(path, "/cur_state"))
            continue;
        if (!snapshot_value(statefile, path, want, sizeof want))
            continue;                   /* not in the snapshot: nothing to restore it to */
        if (dry) {
            printf("  revert %s = %s\n", path, want);
            n++;
            continue;
        }
        if (write_path_str(path, want) == 0) {
            n++;
            continue;
        }
        /* A REVERT THAT FAILS HAS TO SAY SO. This counted successes and returned 0
         * whatever happened, so a REJECTED restore was indistinguishable from a clean
         * one - which is how the uclamp ordering bug left sched_util_clamp_min at 512
         * with nothing printed anywhere, on any stream, at any verbosity.
         *
         * The kernel refuses these writes for real reasons: a cross-field rule the pair
         * violates (EINVAL), a busy attribute, a node that has gone away under a new
         * kernel. Someone holding a board whose state no longer matches any profile in
         * their config needs to be told which write failed and why - most people running
         * this are not going to go reading sysfs to find out. */
        if (!nfail)
            snprintf(firstfail, sizeof firstfail, "%s = %s (%s)",
                     path, want, strerror(errno));
        nfail++;
    }
    if (nreverted)
        *nreverted = n;
    if (nfail) {
        snprintf(err, errlen,
                 "%d of %d restore write%s failed and the board no longer matches '%s'; "
                 "first failure: %s. 'qcctl --reset' restores the boot state.",
                 nfail, nfail + n, (nfail + n) == 1 ? "" : "s", prev_name, firstfail);
        return -1;
    }
    return 0;
}

int state_save(const struct surface *s, const char *path, char *err, size_t errlen)
{
    char id[64];
    current_boot_id(id, sizeof id);
    return state_save_tagged(s, path, id, err, errlen);
}

int state_save_tagged(const struct surface *s, const char *path, const char *bootid,
                      char *err, size_t errlen)
{
    char p[PATH_MAXLEN];
    FILE *f = fopen(path, "w");

    if (errlen)
        err[0] = '\0';
    if (!f)
        PFAIL("cannot write %s: %s", path, strerror(errno));

    fprintf(f, "# qcctl boot-state snapshot; restore with --reset\n");
    if (bootid && bootid[0])
        fprintf(f, "# boot_id %s\n", bootid);
    {
        /* Comment, not data: --reset must keep working on a snapshot that predates this
         * line, and must not care what it says. It is here so a snapshot restored after a
         * kernel change can be SEEN to predate it - boot_id alone cannot show that. */
        struct envid e;
        char desc[160];
        envid_read(&e);
        envid_describe(&e, desc, sizeof desc);
        fprintf(f, "# environment %s\n", desc);
    }
    for (int i = 0; i < s->npol; i++) {
        const char *attrs[] = { "scaling_governor", "scaling_min_freq",
                                "scaling_max_freq", NULL };
        for (int a = 0; attrs[a]; a++) {
            spath(p, sizeof p, CPUFREQ_DIR "/policy%d/%s", s->pol[i].id, attrs[a]);
            snap_line(f, p);
        }
    }
    for (int i = 0; i < s->ncpu; i++) {
        if (!s->cpu[i].present)
            continue;
        spath(p, sizeof p, SYS_CPU "/cpu%d/online", s->cpu[i].cpu);
        snap_line(f, p);
    }
    if (s->gpu.present) {
        const char *attrs[] = { "governor", "min_freq", "max_freq", NULL };
        for (int a = 0; attrs[a]; a++) {
            snprintf(p, sizeof p, "%s/%s", s->gpu.dir, attrs[a]);
            snap_line(f, p);
        }
    }
    for (int i = 0; i < s->ncdev; i++) {
        spath(p, sizeof p, THERMAL_DIR "/cooling_device%d/cur_state", s->cdev[i].idx);
        snap_line(f, p);
    }
    for (int i = 0; i < s->nzone; i++) {
        spath(p, sizeof p, THERMAL_DIR "/thermal_zone%d/mode", s->zone[i].idx);
        snap_line(f, p);
        for (int t = 0; t < s->zone[i].ntrips; t++) {
            spath(p, sizeof p, THERMAL_DIR "/thermal_zone%d/trip_point_%d_temp",
                  s->zone[i].idx, s->zone[i].trips[t].idx);
            snap_line(f, p);
            spath(p, sizeof p, THERMAL_DIR "/thermal_zone%d/trip_point_%d_hyst",
                  s->zone[i].idx, s->zone[i].trips[t].idx);
            snap_line(f, p);
        }
    }
    for (int i = 0; i < 2; i++) {
        spath(p, sizeof p, "/proc/sys/kernel/sched_util_clamp_%s", i ? "max" : "min");
        snap_line(f, p);
    }
    fclose(f);
    return 0;
}

/* A cooling device's cur_state is the thermal governor's OUTPUT, not configuration -
 * restoring it is like restoring a CPU's instantaneous frequency instead of its governor.
 * Worse, on this board it is actively harmful.
 * So the snapshot keeps these lines (they are useful diagnostics - they say what the fan
 * was doing) but a restore must never replay them. */
static int is_governor_output(const char *path)
{
    return strstr(path, "/cooling_device") != NULL && strstr(path, "/cur_state") != NULL;
}

int apply_verify(const struct applyctx *ctx, struct verifyreport *r)
{
    char got[NAME_LEN];
    int candidates = 0;

    memset(r, 0, sizeof *r);
    for (int i = 0; i < ctx->nw; i++) {
        size_t n;

        if (is_governor_output(ctx->w[i].path))
            continue;
        candidates++;
        if (read_file_str(ctx->w[i].path, got, sizeof got) < 0) {
            r->unreadable++;
            continue;
        }
        /* read_file_str strips the newline; trailing blanks are not ours to interpret. */
        n = strlen(got);
        while (n > 0 && (got[n - 1] == ' ' || got[n - 1] == '\t' || got[n - 1] == '\r'))
            got[--n] = '\0';
        r->checked++;
        if (!strcmp(got, ctx->w[i].value))
            continue;
        if (r->diverged++ == 0) {
            snprintf(r->path, sizeof r->path, "%s", ctx->w[i].path);
            snprintf(r->want, sizeof r->want, "%s", ctx->w[i].value);
            snprintf(r->got,  sizeof r->got,  "%s", got);
        }
    }
    /* Nothing read means nothing proved, which must not read as a clean result. */
    if (candidates > 0 && r->checked == 0)
        return -1;
    return 0;
}

int state_restore(const char *path, char *err, size_t errlen, int *nrestored)
{
    char line[PATH_MAXLEN + NAME_LEN + 8];
    int n = 0, failed = 0;
    FILE *f = fopen(path, "r");

    if (errlen)
        err[0] = '\0';
    if (nrestored)
        *nrestored = 0;
    if (!f)
        PFAIL("cannot read %s: %s", path, strerror(errno));

    while (fgets(line, sizeof line, f)) {
        char *tab, *nl;
        if (line[0] == '#')
            continue;
        nl = strchr(line, '\n');
        if (nl)
            *nl = '\0';
        tab = strchr(line, '\t');
        if (!tab)
            continue;
        *tab = '\0';
        if (is_governor_output(line))
            continue;               /* see is_governor_output(): replaying this can leave
                                       the board uncooled */
        if (write_path_str(line, tab + 1) < 0)
            failed++;
        else
            n++;
    }
    fclose(f);
    if (nrestored)
        *nrestored = n;
    if (failed)
        PFAIL("%d of %d values could not be restored", failed, failed + n);
    return 0;
}
