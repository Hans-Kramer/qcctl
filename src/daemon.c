// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer
//
// qcctld - applies a profile at startup and runs the fan curve.
//
// It owns ONE qcstats child and reads its NDJSON. It contains no monitoring code -
// see the observes-acts-boundary skill.
//
// Failsafe: on any exit path the zone goes back to the kernel and the fan to the
// state it had. A SIGKILL cannot be caught, so the systemd unit carries
// ExecStopPost as the backstop for that case.

#define _GNU_SOURCE
#include "apply.h"
#include "config.h"
#include "fancurve.h"
#include "stats.h"
#include "fanown.h"
#include "control.h"
#include "fanbase.h"
#include "envid.h"
#include "surface.h"
#include "io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <syslog.h>

#define DEF_CONF    "/etc/qcctl/profiles.conf"
#define DEF_FAN     "/etc/qcctl/fan.config"
#define DEF_STATE   "/var/lib/qcctl/boot-state"
/* Inside the unit's RuntimeDirectory with the socket and the status file, so systemd
 * removes it on stop and the two-writer guard only ever compares one path. */
#define DEF_PIDFILE "/run/qcctl/pid"
/* The name of the profile currently in force. Written so that something which is not
 * the daemon - the resume hook - can re-apply it without IPC. */
#define DEF_ACTIVE  "/run/qcctl/active"
/* The operator's choice. In /var/lib because it must OUTLIVE this process - the active
 * file above is unlinked on exit and so cannot carry a selection across a restart. */
#define DEF_SELECTED "/var/lib/qcctl/selected"
/* Telemetry for a consumer. A file, not a bus: it satisfies both invariants (one sampler,
 * unprivileged GUI) without sd-bus. */
#define DEF_STATUS  "/run/qcctl/status"
#define DEF_FANBASE "/var/lib/qcctl/fan-baseline"
/* The fan watchdog corrects a state the kernel would never hold, on a zone we are not
 * driving. Bounded on purpose: a state that keeps coming back is someone's decision rather
 * than a leftover, and this daemon reports drift instead of fighting it. */
#define FANWD_INTERVAL_MS 60000
#define FANWD_TRIES 3
/* And the cap is forgotten after this long without one, so "three strikes" means three in
 * quick succession - someone fighting us - rather than three in a fortnight of uptime. */
#define FANWD_FORGET_MS 3600000
#define ENV_FANWD_FORGET "QCCTL_FANWD_FORGET_MS"   /* for the tests; see --stats-cmd */
/* rpm needs time to settle after a state change */
#define AIRFLOW_DWELL_MS 8000

/* Stream tolerance. A null or missing temperature is a real answer meaning "not
 * measured", so the curve holds rather than acting on an invented number. After
 * this many consecutive misses the stream is treated as dead and we fail safe -
 * the defined failure mode that replaces reading sysfs ourselves. */
#define MISS_LIMIT  3
/* How often the loop wakes to service the control socket. --cool-max may be a split-second
 * decision to save hardware, so it must not wait for the 2 s sampler tick. */
#define CTL_TICK_MS 200
/* How often the applied profile is re-checked against the hardware. Cheap - about a dozen
 * sysfs reads - and it does not need to be fast: drift is a human-timescale event. */
#define DRIFT_CHECK_MS 10000
/* A sampler that dies must not kill a daemon whose job is to stay resident. */
#define SAMPLER_RETRIES   3
#define SAMPLER_RETRY_MS  2000
/* How long the sampler must behave before its retry budget is forgiven. Without this the
 * budget is cumulative over the daemon's whole lifetime, so three isolated hiccups days
 * apart would force a restart - while a sampler flapping once a second would still be
 * caught, which is the case the budget is actually for. */
#define SAMPLER_HEALTHY_MS 60000
#define ENV_RETRY_MS "QCCTL_SAMPLER_RETRY_MS"
#define COOLMAX_CDEV "emc2305_fan"

static volatile sig_atomic_t stop_flag;
static void on_signal(int s) { (void)s; stop_flag = 1; }

/* Whichever strategy a profile selected. Exactly one of curve/target is set. The loop
 * treats them uniformly apart from the one call that decides a state. */
struct strategy {
    const struct curve  *curve;
    const struct target *target;
    struct target_state  tstate;
};

/* How long to stay quiet about a cool-max refusal that has not changed. Clients retry
 * every second; the condition is worth saying once, not sixty times a minute. */
#define COOLMAX_REFUSE_QUIET_MS 60000
#define QCSTATS_PATH    "/usr/bin/qcstats"
#define DEF_WATCH_ZONE  "cpuss-0-0"
#define DEF_WATCH_POLL  2000

static int strat_active(const struct strategy *g)
{ return g->curve != NULL || g->target != NULL; }
static const char *strat_zone(const struct strategy *g)
{ return g->curve ? g->curve->zone : g->target ? g->target->zone : DEF_WATCH_ZONE; }
static const char *strat_cdev(const struct strategy *g)
{ return g->curve ? g->curve->cdev : g->target ? g->target->cdev : ""; }
static int strat_poll_ms(const struct strategy *g)
{ return g->curve ? g->curve->poll_ms : g->target ? g->target->poll_ms : DEF_WATCH_POLL; }
static enum failsafe_kind strat_failsafe(const struct strategy *g)
{ return g->curve ? g->curve->failsafe : g->target ? g->target->failsafe : FAILSAFE_KERNEL; }
static const char *strat_name(const struct strategy *g)
{ return g->curve ? g->curve->name : g->target ? g->target->name : "none"; }
static const char *strat_kind(const struct strategy *g)
{ return g->curve ? "curve" : g->target ? "target" : "none"; }

static int strat_derive(const struct profile *p, const struct fanconfig *fc,
                        struct strategy *out, char *err, size_t errlen)
{
    int want_curve = 0;
    const char *cname = NULL;

    memset(out, 0, sizeof *out);
    for (int i = 0; i < p->nact; i++) {
        if (p->act[i].kind == ACT_FAN_MODE)
            want_curve = (p->act[i].value == FAN_MODE_CURVE);
        else if (p->act[i].kind == ACT_FAN_CURVE)
            cname = p->act[i].target;
    }
    if (!want_curve)
        return 0;
    if (cname) {
        out->curve = fanconfig_find_curve(fc, cname);
        if (!out->curve)
            out->target = fanconfig_find_target(fc, cname);
    }
    if (!out->curve && !out->target) {
        snprintf(err, errlen, "profile '%s' wants '%s', which is in neither the CURVE "
                 "nor the TARGET blocks", p->name, cname ? cname : "(unnamed)");
        return -1;
    }
    return 0;
}

static long mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Drift: does the hardware still hold what we applied? */
struct driftref {
    char path[PATH_MAXLEN];
    char want[NAME_LEN];
};
static struct driftref drift_ref[MAX_SAVED];
static int  drift_n = -1;           /* -1 = never established; publish null, not false */
static int  drift_count;            /* mismatches at the last check */
static char drift_first[PATH_MAXLEN];

/* cooling_device cur_state is the thermal governor's OUTPUT, not configuration. */
static int drift_watchable(const char *path)
{
    return !(strstr(path, "/cooling_device") && strstr(path, "/cur_state"));
}

static void drift_baseline(const struct applyctx *ctx)
{
    char got[NAME_LEN];

    drift_n = 0;
    drift_count = 0;
    drift_first[0] = '\0';
    for (int i = 0; i < ctx->nw && drift_n < MAX_SAVED; i++) {
        if (!drift_watchable(ctx->w[i].path))
            continue;
        if (read_file_str(ctx->w[i].path, got, sizeof got) < 0)
            continue;               /* unreadable now: nothing to compare later */
        snprintf(drift_ref[drift_n].path, sizeof drift_ref[drift_n].path, "%.*s",
                 (int)sizeof ctx->w[i].path - 1, ctx->w[i].path);
        snprintf(drift_ref[drift_n].want, sizeof drift_ref[drift_n].want, "%s", got);
        drift_n++;
    }
}

/* Returns the number of settings that no longer match. */
static int drift_check(void)
{
    char got[NAME_LEN];
    int bad = 0;

    if (drift_n <= 0)
        return 0;
    drift_first[0] = '\0';
    for (int i = 0; i < drift_n; i++) {
        if (read_file_str(drift_ref[i].path, got, sizeof got) < 0)
            continue;
        if (strcmp(got, drift_ref[i].want)) {
            if (!bad)
                snprintf(drift_first, sizeof drift_first, "%.*s",
                         (int)sizeof drift_ref[i].path - 1, drift_ref[i].path);
            bad++;
        }
    }
    drift_count = bad;
    return bad;
}

/* Publish a status snapshot for a consumer. */
struct statusinfo {
    const char *profile;
    /* How `profile` was chosen: selected | default | fallback | fallback-unusable |
     * explicit. A GUI needs this to tell "what you asked for" from "what we fell back
     * to" - the two look identical in `profile` alone. */
    const char *profile_source;
    const struct strategy *g;
    int  have_temp;  long temp_mc;
    int  have_fan;   int  fan_state;
    int  max_state;
    int  fan_owned;
    int  safety;
    const struct fanobs *fo;
    const struct cpuobs *co;
    int  holds;                 /* cool-max clients currently holding */
    int  drift_known;           /* 0 -> publish null, not false */
    int  drift_count;
    const char *drift_first;
    int  have_target;  long target_mc;   long target_err_mc;
    int  target_unreachable;    /* -1 unknown, 0 no, 1 yes */
    int  sampler_down;          /* alive but blind */
    enum fanbase_verdict airflow;
    int  airflow_known;         /* 0 -> publish null, not "unknown" as a value */
    long airflow_dev_pct;
};

/* Set once, when the daemon detaches: a background process has no terminal to print to. */
static int log_to_syslog = 0;

static void logmsg(const char *fmt, ...)
{
    va_list ap;
    struct timespec ts;

    va_start(ap, fmt);
    if (log_to_syslog) {
        vsyslog(LOG_INFO, fmt, ap);
        va_end(ap);
        return;
    }
    clock_gettime(CLOCK_REALTIME, &ts);
    printf("[%ld.%03ld] ", (long)ts.tv_sec, ts.tv_nsec / 1000000);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

/* Said on the transition into failing and out of it again, not per write: at this poll rate
 * a per-pass complaint would bury everything else in the journal, and the fact does not
 * change between passes. The recovery is said too, so a journal that carries the failure
 * also shows whether it is still true. */
static int status_failing;

static void status_publish_failed(const char *what, const char *why)
{
    if (status_failing)
        return;
    status_failing = 1;
    logmsg("warning: cannot publish the status file %s: %s - a consumer sees a stale file "
           "or none at all. Said once, until it works again.", what, why);
}

static void publish_status(const char *path, const struct statusinfo *si)
{
    const char *profile = si->profile;
    const struct strategy *g = si->g;
    int have_temp = si->have_temp, have_fan = si->have_fan;
    long temp_mc = si->temp_mc;
    int fan_state = si->fan_state, max_state = si->max_state;
    int fan_owned = si->fan_owned, safety = si->safety;
    const struct fanobs *fo = si->fo;

    char tmp[PATH_MAXLEN];
    FILE *f;

    if (!path || !*path)
        return;
    if (snprintf(tmp, sizeof tmp, "%s.new", path) >= (int)sizeof tmp) {
        status_publish_failed(path, "the path leaves no room for a .new sibling");
        return;
    }
    f = fopen(tmp, "w");
    if (!f) {
        status_publish_failed(tmp, strerror(errno));
        return;
    }
    fprintf(f, "{\"v\":1");
    fprintf(f, ",\"profile\":\"%s\"", profile ? profile : "");
    fprintf(f, ",\"profile_source\":\"%s\"",
            si->profile_source ? si->profile_source : "default");
    fprintf(f, ",\"strategy\":\"%s\"", strat_kind(g));
    fprintf(f, ",\"strategy_name\":\"%s\"", strat_name(g));
    fprintf(f, ",\"zone\":\"%s\"", strat_zone(g));
    if (have_temp) fprintf(f, ",\"temp_mc\":%ld", temp_mc);
    else           fprintf(f, ",\"temp_mc\":null");
    if (have_fan)  fprintf(f, ",\"fan_state\":%d", fan_state);
    else           fprintf(f, ",\"fan_state\":null");
    if (max_state > 0) fprintf(f, ",\"fan_max_state\":%d", max_state);
    else               fprintf(f, ",\"fan_max_state\":null");
    fprintf(f, ",\"fan_owned\":%s", fan_owned ? "true" : "false");
    /* Observed fan, straight from the sampler. qcctl never reads the tach itself:
     * qcstats observes, qcctl acts. When we do not own the fan this is the only
     * honest source for its speed, and it carries qcstats' own validity flags - the tach
     * misreads below state 2 on this board, so "a number" is not "a trustworthy number". */
    if (fo && fo->duty_ok) fprintf(f, ",\"fan_duty_pct\":%ld", fo->duty_pct);
    else                   fprintf(f, ",\"fan_duty_pct\":null");
    if (fo && fo->rpm_ok)  fprintf(f, ",\"fan_rpm\":%ld", fo->rpm);
    else                   fprintf(f, ",\"fan_rpm\":null");
    fprintf(f, ",\"fan_rpm_valid\":%s", (fo && fo->rpm_ok && fo->rpm_valid) ? "true" : "false");
    /* PER POLICY, not per core - three entries on this board. The only nested value in
     * this file; everything else is a flat scalar. Absent "cpu" in the sample publishes
     * an empty array rather than omitting the key, so a consumer can tell "no policies
     * reported" from "this build does not publish them". */
    fprintf(f, ",\"cpu\":[");
    if (si->co) {
        for (int i = 0; i < si->co->n; i++) {
            fprintf(f, "%s{\"label\":\"%s\",\"mhz\":", i ? "," : "", si->co->pol[i].label);
            if (si->co->pol[i].mhz_ok) fprintf(f, "%ld}", si->co->pol[i].mhz);
            else                       fprintf(f, "null}");
        }
    }
    fprintf(f, "]");
    fprintf(f, ",\"safety_ceiling\":%s", safety ? "true" : "false");
    fprintf(f, ",\"cool_max_holds\":%d", si->holds);

    /* Drift: the hardware no longer matches what we applied. null when we never applied
     * anything (--no-apply) - "we did not check" is not "nothing drifted". */
    if (!si->drift_known) {
        fprintf(f, ",\"drifted\":null,\"drifted_count\":null,\"drifted_first\":null");
    } else {
        fprintf(f, ",\"drifted\":%s", si->drift_count > 0 ? "true" : "false");
        fprintf(f, ",\"drifted_count\":%d", si->drift_count);
        if (si->drift_count > 0 && si->drift_first && *si->drift_first)
            fprintf(f, ",\"drifted_first\":\"%s\"", si->drift_first);
        else
            fprintf(f, ",\"drifted_first\":null");
    }

    /* TARGET: what it is aiming for, how far off it is, and whether it has given up.
     * All null unless a TARGET strategy is actually running. */
    if (si->have_target) {
        fprintf(f, ",\"target_mc\":%ld", si->target_mc);
        fprintf(f, ",\"target_error_mc\":%ld", si->target_err_mc);
    } else {
        fprintf(f, ",\"target_mc\":null,\"target_error_mc\":null");
    }
    if (si->target_unreachable < 0)
        fprintf(f, ",\"target_unreachable\":null");
    else
        fprintf(f, ",\"target_unreachable\":%s",
                si->target_unreachable ? "true" : "false");

    /* Fan health. */
    if (!si->airflow_known) {
        fprintf(f, ",\"airflow\":null,\"airflow_dev_pct\":null");
    } else {
        fprintf(f, ",\"airflow\":\"%s\"", fanbase_verdict_str(si->airflow));
        if (si->airflow == FANBASE_UNKNOWN)
            fprintf(f, ",\"airflow_dev_pct\":null");
        else
            fprintf(f, ",\"airflow_dev_pct\":%ld", si->airflow_dev_pct);
    }
    fprintf(f, ",\"sampler\":\"%s\"", si->sampler_down ? "down" : "ok");
    fprintf(f, ",\"ts\":%ld", (long)time(NULL));
    fprintf(f, "}\n");
    /* fclose reports the write errors fprintf buffered away - a full filesystem arrives
     * here, not above. */
    if (fclose(f) != 0) {
        status_publish_failed(tmp, strerror(errno));
        unlink(tmp);
        return;
    }
    if (rename(tmp, path) != 0) {
        status_publish_failed(path, strerror(errno));
        unlink(tmp);
        return;
    }
    if (status_failing) {
        status_failing = 0;
        logmsg("status file %s is being written again", path);
    }
}

/* Publish an honest snapshot while the sampler is down. */
static void publish_degraded(const char *path, const char *profile, const char *source,
                             const struct strategy *g)
{
    struct statusinfo si;

    memset(&si, 0, sizeof si);
    si.profile = profile;
    si.profile_source = source;
    si.g = g;
    si.drift_known = (drift_n >= 0);
    si.drift_count = drift_count;
    si.drift_first = drift_first;
    si.target_unreachable = -1;
    si.sampler_down = 1;
    publish_status(path, &si);
}


static int fan_take(struct fanown *fo, const struct surface *s, const struct strategy *g,
                    char *err, size_t errlen)
{
    if (fanown_take(fo, s, strat_zone(g), strat_cdev(g), err, errlen) < 0)
        return -1;
    logmsg("took zone%d (%s) for %s '%s'; fan was state %s, zone mode was %s",
           fo->zone_idx, strat_zone(g), strat_kind(g), strat_name(g),
           fo->fan_state_before[0] ? fo->fan_state_before : "?",
           fo->zone_mode_before[0] ? fo->zone_mode_before : "?");
    /* A mismatch is not fatal - cur_state writes still land - but disabling the zone may not
     * stop the kernel driving this device, so the cooling can be quietly overridden. Said
     * once, at take time, because it cannot change while we hold the zone. */
    if (fo->binding == 0)
        logmsg("WARNING zone%d does not drive cooling device %d according to the kernel's "
               "own cdev links - disabling this zone may not stop the fan being overridden. "
               "Fan commands still apply; the curve may not hold.",
               fo->zone_idx, fo->cdev_idx);
    else if (fo->binding < 0)
        logmsg("note: this kernel exposes no zone-to-cooling-device links, so the "
               "zone%d/device%d pairing could not be checked",
               fo->zone_idx, fo->cdev_idx);
    return 0;
}

/* `have_temp`/`temp_mc` are the last reading this daemon saw. With one, the fan is left
 * where the kernel itself would hold it - the same decision --cool-max makes. Without one
 * the old never-lower handback stands: no temperature is not a licence to raise the fan,
 * and it is not a licence to lower it either. */
static void fan_release(struct fanown *fo, const struct strategy *g,
                        const struct surface *s, int have_temp, long temp_mc)
{
    enum fanown_release how;
    long trip_mc = 0;
    int before, want = -1, set_failed = 0;

    if (!fo->owned)
        return;
    before = fo->fan_state_before[0] ? atoi(fo->fan_state_before) : -1;

    if (g && (g->curve || g->target) && strat_failsafe(g) == FAILSAFE_FULL) {
        if (fanown_release(fo, FANOWN_LEAVE_MAX) < 0)
            logmsg("RELEASE FAILED: %s", fo->release_err);
        else
            logmsg("failsafe=full: fan left at state %d",
                   fo->max_state ? fo->max_state : 10);
        return;
    }

    how = fanown_release_mode(s, fo->zone, have_temp, temp_mc, &want);
    if (how == FANOWN_LEAVE_MAX && !have_temp)
        how = FANOWN_HANDBACK;
    fanown_active_trip(s, fo->zone, &trip_mc);
    if (want >= 0 && fanown_set(fo, want) < 0) {
        logmsg("could not set the fan to %d (%s) - releasing anyway, it stays where it is",
               want, strerror(errno));
        set_failed = 1;
    }
    if (fanown_release(fo, how) < 0) {
        logmsg("RELEASE FAILED: %s", fo->release_err);
        return;
    }
    if (how == FANOWN_LEAVE_MAX)
        logmsg("released zone%d back to the kernel (mode=%s); fan LEFT at maximum - the "
               "cooling map's floor could not be read, so more cooling is the safe guess",
               fo->zone_idx, fo->zone_mode_before[0] ? fo->zone_mode_before : "?");
    else if (how == FANOWN_HANDBACK) {
        /* What fanown_release() writes on this path, and -1 when the take could not read
         * cur_state - in which case it writes nothing and there is no state to name. */
        int held = fo->last_state > before ? fo->last_state : before;

        if (held >= 0)
            logmsg("released zone%d back to the kernel (mode=%s); no temperature, so the "
                   "fan was not lowered from state %d", fo->zone_idx,
                   fo->zone_mode_before[0] ? fo->zone_mode_before : "?", held);
        else
            logmsg("released zone%d back to the kernel (mode=%s); no temperature and no "
                   "recorded fan state, so the fan was left untouched", fo->zone_idx,
                   fo->zone_mode_before[0] ? fo->zone_mode_before : "?");
    }
    else if (set_failed)
        logmsg("released zone%d back to the kernel (mode=%s), but the fan could NOT be set "
               "to state %d - it stays where it was", fo->zone_idx,
               fo->zone_mode_before[0] ? fo->zone_mode_before : "?", want);
    else
        logmsg("released zone%d back to the kernel (mode=%s); fan set to state %d, what "
               "the kernel holds at %ld.%ld C (trip %ld.%ld C) - it decides from here",
               fo->zone_idx, fo->zone_mode_before[0] ? fo->zone_mode_before : "?",
               want, temp_mc / 1000, (temp_mc % 1000) / 100,
               trip_mc / 1000, (trip_mc % 1000) / 100);
}

static int fan_set(struct fanown *fo, int state)
{
    return fanown_set(fo, state);
}

/* The sampler died. Fail the fan safe, then try to get the sampler back.
 *
 * Order matters and is not negotiable: the fan is released FIRST. We cannot drive a
 * strategy without temperatures, so holding the zone while blind is the one thing we must
 * never do - a retry policy does not get to defer that.
 *
 * Returns 0 if the sampler is back (and the fan re-taken, if a strategy wants it), or -1
 * when the retries are exhausted. The caller then exits NON-ZERO so systemd restarts the
 * unit; in-process retries handle a hiccup, systemd handles a persistent fault. */
static int sampler_recover(struct stats *st, const char *cmd, struct fanown *rt,
                           const struct surface *s, const struct strategy *g,
                           int *attempt, int *cur)
{
    char err[ERRLEN];
    int delay_ms;

    /* No temperature, deliberately: the sampler has just died and this path's whole
     * purpose is conservatism. fan_release() then keeps the never-lower handback. */
    fan_release(rt, g, s, 0, 0);        /* blind: hand the fan back before anything else */
    stats_close(st);
    /* stats_close() reaped the child, so NOW the real cause is available. Logged here
     * because "sampler stream ended (EOF)" on its own told a user whose qcstats had been
     * removed absolutely nothing about why their fan control had stopped. */
    {
        const char *why = stats_exit_hint(st, QCSTATS_PATH);
        if (why)
            logmsg("sampler: %s", why);
    }

    {
        const char *e = getenv(ENV_RETRY_MS);
        delay_ms = (e && *e) ? atoi(e) : SAMPLER_RETRY_MS;
        if (delay_ms < 1)
            delay_ms = 1;
    }
    while (*attempt < SAMPLER_RETRIES) {
        (*attempt)++;
        logmsg("sampler lost - retry %d of %d in %d ms (fan handed back meanwhile)",
               *attempt, SAMPLER_RETRIES, delay_ms);
        usleep((useconds_t)delay_ms * 1000);
        if (stop_flag)
            return -1;
        if (stats_open(st, cmd, err, sizeof err) < 0) {
            logmsg("sampler retry %d failed: %s", *attempt, err);
            continue;
        }
        /* "back" was a claim we cannot make here: stats_open() only proves a pipe and a
         * fork, not that qcstats ran. With a missing binary the log read "sampler back
         * after 3 retries" and then immediately EOF'd again, which is worse than no
         * message. We restarted it; whether it works is the next sample's news. */
        logmsg("sampler restarted (attempt %d of %d); waiting for its first sample",
               *attempt, SAMPLER_RETRIES);
        *cur = -1;
        if (strat_active(g)) {
            if (fan_take(rt, s, g, err, sizeof err) < 0) {
                logmsg("sampler back but the fan could not be re-taken: %s", err);
                return -1;
            }
            *cur = rt->fan_state_before[0] ? atoi(rt->fan_state_before) : 0;
        }
        return 0;
    }
    logmsg("sampler did not come back after %d retries - exiting non-zero so systemd "
           "restarts the unit", SAMPLER_RETRIES);
    return -1;
}

/* ======================================================================= */
/*                               PID FILE                                  */
/* ======================================================================= */
/* Is the pid in an existing pidfile a live qcctld, or a corpse?
 * After a SIGKILL the file survives, and a plain O_EXCL would then refuse to start
 * for ever - which with Restart=on-failure means the daemon can never come back. */
static int pidfile_is_stale(const char *path)
{
    char buf[64], comm[64], procpath[128];
    long pid;

    if (read_file_str(path, buf, sizeof buf) < 0)
        return 1;                       /* unreadable: treat as stale */
    pid = strtol(buf, NULL, 10);
    if (pid <= 1)
        return 1;
    snprintf(procpath, sizeof procpath, "/proc/%ld/comm", pid);
    if (read_file_str(procpath, comm, sizeof comm) < 0)
        return 1;                       /* no such process */
    /* A live process with that pid is not enough - the pid may have been reused by
     * something unrelated. It has to actually be us. */
    return strcmp(comm, "qcctld") != 0;
}

/* RuntimeDirectory= covers the unit; a hand-started daemon has to make this itself. 0755
 * is load-bearing: the 0666 control socket inside needs a traversable directory. */
static void ensure_rundir(const char *path)
{
    char dir[PATH_MAXLEN];
    char *slash;

    if (snprintf(dir, sizeof dir, "%s", path) >= (int)sizeof dir)
        return;
    slash = strrchr(dir, '/');
    if (!slash || slash == dir)
        return;
    *slash = '\0';
    if (mkdir(dir, 0755) == 0) {
        /* mkdir's mode is masked by umask, and 0755 here is load-bearing: the 0666 control
         * socket inside is only reachable while this is world-traversable. A hand-started
         * daemon inherits the operator's umask, so state the mode rather than request it -
         * which is what install.sh and RuntimeDirectoryMode= both do. */
        if (chmod(dir, 0755) != 0)
            logmsg("warning: created %s but could not set mode 0755 (%s) - an "
                   "unprivileged --cool-max may not reach the control socket",
                   dir, strerror(errno));
        logmsg("created %s - no RuntimeDirectory, so this was started outside systemd", dir);
    }
}

static int pidfile_create(const char *path)
{
    char buf[32];
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    int firsterr = errno;

    /* pidfile_is_stale() opens and reads two files, and a library call may set errno even
     * when it succeeds. The caller distinguishes EEXIST - another daemon - from every other
     * cause, so on the one path where nothing after the open sets errno meaningfully,
     * restore what the open failed with. On the others, unlink or the retry has set it and
     * that is the more useful value. */
    if (fd < 0 && firsterr == EEXIST) {
        if (pidfile_is_stale(path)) {
            logmsg("removing stale pidfile %s", path);
            if (unlink(path) == 0)
                fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
        } else {
            errno = firsterr;
        }
    }
    if (fd < 0)
        return -1;
    snprintf(buf, sizeof buf, "%d\n", (int)getpid());
    if (write(fd, buf, strlen(buf)) < 0) {
        close(fd);
        unlink(path);
        return -1;
    }
    close(fd);
    return 0;
}


/* The default. Returns the readiness pipe's write end, which the child reports a startup
 * verdict on - so the caller's exit status is the daemon's own and not the fork's. */
static int daemon_detach(void)
{
    int pfd[2];
    pid_t pid;

    if (pipe2(pfd, O_CLOEXEC) < 0) {
        fprintf(stderr, "pipe: %s\n", strerror(errno));
        exit(1);
    }
    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "fork: %s\n", strerror(errno));
        exit(1);
    }
    if (pid > 0) {
        unsigned char code = 1;
        ssize_t n;

        close(pfd[1]);
        /* EOF means the child exited before reporting, which is a failure to start. */
        do { n = read(pfd[0], &code, 1); } while (n < 0 && errno == EINTR);
        _exit(n == 1 ? (int)code : 1);
    }
    close(pfd[0]);
    setsid();
    return pfd[1];
}

/* The byte is the exit code the parent is to return. No-op in the foreground. */
static void startup_verdict(int *fd, int code)
{
    unsigned char b = (unsigned char)code;
    ssize_t n;

    if (*fd < 0)
        return;
    n = write(*fd, &b, 1);
    (void)n;
    close(*fd);
    *fd = -1;
}

/* From here there is no terminal: whoever started us has their prompt back. */
static void log_detach(void)
{
    int fd;

    openlog("qcctld", LOG_PID, LOG_DAEMON);
    log_to_syslog = 1;
    fd = open("/dev/null", O_RDWR);
    if (fd < 0)
        return;
    dup2(fd, STDIN_FILENO);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    if (fd > STDERR_FILENO)
        close(fd);
}


/* ======================================================================= */
/*                               MAIN                                      */
/* ======================================================================= */

static void usage(void)
{
    printf(
"usage: qcctld [options]\n"
"\n"
"  -c, --config <path>         profiles (default %s)\n"
"      --fan-config <path>     fan curves (default %s)\n"
"      --state <path>          boot-state snapshot (default %s)\n"
"      --pidfile <path>        default %s\n"
"      --status-file <p>       where the status snapshot is published, for the GUI\n"
"      --control-socket <p>    where unprivileged clients ask for cooling\n"
"      --fan-baseline-file <p> the captured rpm-per-state reference\n"
"      --drift-interval <ms>   how often to re-check that the applied profile still\n"
"                              holds (default %d)\n"
"      --active-file <p>       where the active profile name is recorded, for the\n"
"                              resume hook to read. Default /run/qcctl/active\n"
"      --selection-file <p>    the operator's recorded choice, honoured on a restart.\n"
"                              Boot-id scoped: a power cycle returns to DEFAULT.\n"
"                              Default %s\n"
"  -m, --mode <name|id>        profile to apply (default: the DEFAULT profile)\n"
"      --stats-cmd <cmd>       override the sampler command; used by the tests to\n"
"                              feed a canned NDJSON stream instead of qcstats\n"
"      --no-apply              run the fan curve only; do not apply the profile\n"
"      --oneshot               apply, then exit without running a curve\n"
"      --foreground            stay in the foreground and log to stdout; what the\n"
"                              systemd unit passes. The default is to detach once the\n"
"                              pidfile is held and log to syslog. --oneshot implies it.\n"
"  -V, --version               print the version and exit\n"
"  -h, --help\n", DEF_CONF, DEF_FAN, DEF_STATE, DEF_PIDFILE, DRIFT_CHECK_MS,
   DEF_SELECTED);
}

// QCCTL_VERSION comes from the Makefile; see the note in main.c. "unknown" rather than a
// plausible-looking number, for a hand-compiled binary.
#ifndef QCCTL_VERSION
#define QCCTL_VERSION "unknown"
#endif

// The daemon reports its OWN name, so `systemctl` logs and a -V on the installed binary
// agree about which of this project's four binaries is being asked.
static void version(void)
{
    printf("qcctld %s\n", QCCTL_VERSION);
}

int main(int argc, char **argv)
{
    const char *conf = DEF_CONF, *fanconf = DEF_FAN, *statef = DEF_STATE;
    const char *pidf = DEF_PIDFILE, *mode = NULL, *stats_cmd = NULL;
    const char *activef = DEF_ACTIVE;
    const char *selectedf = DEF_SELECTED;
    /* Which of the three paths chose `prof`, published so a GUI can say so. */
    const char *prof_source = "default";
    const char *statusf = DEF_STATUS;
    const char *ctlsock = CONTROL_SOCK;
    const char *fanbasef = DEF_FANBASE;
    int drift_ms = DRIFT_CHECK_MS;
    int no_apply = 0, oneshot = 0, foreground = 0;
    int ready_fd = -1;
    struct surface s;
    struct pconfig pc;
    struct fanconfig fc;
    struct fanown rt;
    struct control ctl;
    struct fanbase fbase;
    int have_fbase = 0;
    char prev_profile[NAME_LEN] = "";
    /* The last reading seen, out here because the release at `out:` needs it. */
    long last_temp_mc = 0;
    int have_temp_seen = 0;
    const struct profile *prof = NULL;
    struct strategy strat;
    char err[ERRLEN];
    int rc = 1, made_pid = 0;

    memset(&rt, 0, sizeof rt);
    memset(&ctl, 0, sizeof ctl);
    ctl.listen_fd = -1;
    memset(&strat, 0, sizeof strat);

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help"))       { usage(); return 0; }
        else if (!strcmp(a, "-V") || !strcmp(a, "--version")) { version(); return 0; }
        else if (!strcmp(a, "--no-apply"))                    no_apply = 1;
        else if (!strcmp(a, "--oneshot"))                     oneshot = 1;
        else if (!strcmp(a, "--foreground"))                  foreground = 1;
        else if (!strcmp(a, "-c") || !strcmp(a, "--config")) { if (++i>=argc) goto badarg; conf = argv[i]; }
        else if (!strcmp(a, "--fan-config"))                 { if (++i>=argc) goto badarg; fanconf = argv[i]; }
        else if (!strcmp(a, "--state"))                      { if (++i>=argc) goto badarg; statef = argv[i]; }
        else if (!strcmp(a, "--pidfile"))                    { if (++i>=argc) goto badarg; pidf = argv[i]; }
        else if (!strcmp(a, "--active-file"))                { if (++i>=argc) goto badarg; activef = argv[i]; }
        else if (!strcmp(a, "--selection-file"))             { if (++i>=argc) goto badarg; selectedf = argv[i]; }
        else if (!strcmp(a, "--status-file"))                { if (++i>=argc) goto badarg; statusf = argv[i]; }
        else if (!strcmp(a, "--control-socket"))             { if (++i>=argc) goto badarg; ctlsock = argv[i]; }
        else if (!strcmp(a, "--fan-baseline-file"))          { if (++i>=argc) goto badarg; fanbasef = argv[i]; }
        else if (!strcmp(a, "--drift-interval"))             { if (++i>=argc) goto badarg; drift_ms = atoi(argv[i]); if (drift_ms < 100) drift_ms = 100; }
        else if (!strcmp(a, "-m") || !strcmp(a, "--mode"))   { if (++i>=argc) goto badarg; mode = argv[i]; }
        else if (!strcmp(a, "--stats-cmd"))                  { if (++i>=argc) goto badarg; stats_cmd = argv[i]; }
        else { fprintf(stderr, "unknown option '%s'\n", a); return 2; }
        continue;
badarg:
        fprintf(stderr, "%s needs a value\n", a);
        return 2;
    }

    /* --oneshot's exit status IS its answer, so detaching would hand back a prompt and
     * no verdict. */
    if (oneshot)
        foreground = 1;

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP,  on_signal);
    signal(SIGPIPE, SIG_IGN);   /* the sampler dying must not kill us */

    io_set_sysroot(getenv(ENV_SYSROOT));
    surface_discover(&s);

    if (config_parse_file(conf, &pc) < 0) {
        fprintf(stderr, "%s:%d: %s\n", conf, pc.err_line, pc.err);
        return 3;
    }
    if (config_validate(&pc, &s, err, sizeof err) < 0) {
        fprintf(stderr, "%s: %s\n", conf, err);
        return 3;
    }
    /* fan.config is optional: a board that never uses FAN MODE curve needs none. */
    memset(&fc, 0, sizeof fc);
    if (access(fanconf, R_OK) == 0) {
        if (fanconfig_parse_file(fanconf, &fc) < 0) {
            fprintf(stderr, "%s:%d: %s\n", fanconf, fc.err_line, fc.err);
            return 3;
        }
        if (fanconfig_validate(&fc, &s, err, sizeof err) < 0) {
            fprintf(stderr, "%s: %s\n", fanconf, err);
            return 3;
        }
    }

    /* ---- which profile ----
     *
     * Three outcomes, kept distinct because they mean different things to an operator who
     * walks up to the board later:
     *
     *   selected  a choice recorded THIS boot, still valid -> honour it. This is the case
     *             that used to be lost: the daemon unlinks the active file on exit, so a
     *             restart reverted to DEFAULT and the board ran a profile nobody chose.
     *   default   nothing recorded this boot. A clean start is not a failure, so DEFAULT
     *             applies and DEFAULT keeps its meaning.
     *   fallback  a choice WAS recorded and cannot be honoured - renamed, deleted, or its
     *             fan curve no longer resolves. Something is wrong, so prefer the profile
     *             marked FALLBACK: slower and cooler beats fast and unattended.
     */
    {
        const struct profile *fb = NULL;
        char sel[NAME_LEN];

        for (int i = 0; i < pc.nprof; i++) {
            if (pc.prof[i].is_default)  prof = &pc.prof[i];
            if (pc.prof[i].is_fallback) fb   = &pc.prof[i];
        }

        if (mode) {
            prof = config_find(&pc, mode);
            if (!prof) {
                fprintf(stderr, "no profile '%s' in %s\n", mode, conf);
                return 3;
            }
            prof_source = "explicit";
        } else if (selection_load(selectedf, sel, sizeof sel)) {
            const struct profile *sp = config_find(&pc, sel);
            struct strategy probe;
            char e2[ERRLEN];

            /* Validate the WHOLE selection before committing to it, fan strategy included:
             * a profile whose curve has been removed from fan.config would otherwise be
             * accepted here and kill the daemon a few lines later, which is the one place
             * a fallback must not fail to engage. */
            if (sp && strat_derive(sp, &fc, &probe, e2, sizeof e2) == 0) {
                prof = sp;
                prof_source = "selected";
            } else if (fb && strat_derive(fb, &fc, &probe, e2, sizeof e2) == 0) {
                prof = fb;
                prof_source = "fallback";
                fprintf(stderr, "the recorded selection '%s' cannot be applied (%s) - "
                                "falling back to '%s'\n", sel,
                        sp ? "its fan curve no longer resolves" : "no such profile",
                        fb->name);
            } else {
                fprintf(stderr, "the recorded selection '%s' cannot be applied, and %s - "
                                "using DEFAULT\n", sel,
                        fb ? "neither can the FALLBACK profile" : "no profile is marked "
                             "FALLBACK");
                prof_source = "fallback-unusable";
            }
        } else {
            prof_source = "default";
        }

        if (!prof) {
            fprintf(stderr, "%s names no DEFAULT profile; use -m\n", conf);
            return 3;
        }

        /* The safety net is checked even when it is not needed, because a guard nothing
         * exercises is a guard that rots - and this one is only ever reached when
         * something has already gone wrong. */
        if (fb && fb != prof) {
            struct strategy probe;
            char e2[ERRLEN];
            if (strat_derive(fb, &fc, &probe, e2, sizeof e2) < 0)
                fprintf(stderr, "WARNING the FALLBACK profile '%s' is not usable: %s - "
                                "there is no safety net if a selection goes bad\n",
                        fb->name, e2);
        }
    }

    /* does it ask us to own the fan? */
    if (strat_derive(prof, &fc, &strat, err, sizeof err) < 0) {
        fprintf(stderr, "%s (see %s)\n", err, fanconf);
        return 3;
    }

    /* After everything that reports to stderr and before the pidfile, so startup errors
     * reach the caller directly and the pid recorded is the resident one. */
    if (!foreground)
        ready_fd = daemon_detach();

    ensure_rundir(pidf);
    if (pidfile_create(pidf) < 0) {
        int e = errno;
        fprintf(stderr, "cannot create %s: %s\n", pidf, strerror(e));
        /* Only EEXIST means another writer. Reporting one for a missing directory or a
         * permission failure sends the operator hunting a process that is not there. */
        if (e == EEXIST)
            fprintf(stderr, "  another qcctld is running; "
                            "two writers must not share the fan\n");
        startup_verdict(&ready_fd, 4);
        return 4;
    }
    made_pid = 1;
    logmsg("qcctld starting: profile '%s'%s", prof->name,
           (strat.curve || strat.target) ? "" : " (no fan strategy)");
    /* Say WHY this profile, not just which. An operator who returns to a board running
     * something they did not choose needs that distinction in the journal, and it is the
     * half that makes a silent reversion visible. */
    if (!strcmp(prof_source, "selected"))
        logmsg("profile source: restored the selection recorded earlier this boot");
    else if (!strcmp(prof_source, "default"))
        logmsg("profile source: DEFAULT - no selection was recorded this boot");
    else if (!strcmp(prof_source, "fallback"))
        logmsg("profile source: FALLBACK - a selection was recorded but could not be "
               "applied. The board is deliberately slower and cooler than asked for.");
    else if (!strcmp(prof_source, "fallback-unusable"))
        logmsg("profile source: DEFAULT after a failed selection - no usable FALLBACK "
               "profile exists, so there was no safety net to fall back to");

    /* Record the active profile. The resume hook re-applies whatever is named here,
     * which is how a profile survives a resume without the daemon needing IPC. */
    /* Capture what WAS in force before overwriting the record - the orphan revert below
     * needs it, and writing the new name first destroys it. */
    if (read_file_str(activef, prev_profile, sizeof prev_profile) < 0)
        prev_profile[0] = '\0';

    if (write_new_file(activef, prof->name) < 0)
        logmsg("warning: cannot record the active profile in %s: %s",
               activef, strerror(errno));

    /* Snapshot before changing anything, so --reset has a real restore point - but only
     * once per boot. We snapshot and then apply, so re-snapshotting on a restart would
     * record our own applied values as "stock". */
    if (!no_apply) {
        int snap_skipped = 0;
        if (state_save_once(&s, statef, &snap_skipped, err, sizeof err) < 0)
            logmsg("warning: could not snapshot boot state: %s", err);
        else if (snap_skipped)
            logmsg("boot state snapshot kept from earlier this boot: %s", statef);
        else
            logmsg("boot state snapshot -> %s", statef);

        struct applyctx ctx;
        memset(&ctx, 0, sizeof ctx);
        if (apply_plan(prof, &s, &ctx, err, sizeof err) < 0) {
            logmsg("plan failed: %s", err);
            goto out;
        }
        if (apply_commit(&ctx, err, sizeof err) < 0) {
            logmsg("apply failed: %s", err);
            goto out;
        }
        logmsg("applied profile '%s': %d writes", prof->name, ctx.nw);

        /* Same idempotence rule the CLI follows: if a DIFFERENT profile was in force,
         * revert what it owned and this one does not mention. Normally there is nothing to
         * do - the active file lives in /run, cleared at boot and unlinked when we exit -
         * but it is populated when the CLI applied a profile and the daemon is then
         * restarted, which is exactly the case that would otherwise strand settings. */
        if (prev_profile[0] && strcmp(prev_profile, prof->name)) {
            int nrev = 0;
            if (apply_orphans(&pc, prev_profile, &ctx, &s, statef, 0,
                              err, sizeof err, &nrev) < 0)
                logmsg("warning: could not revert '%s' settings: %s", prev_profile, err);
            else if (nrev)
                logmsg("reverted %d setting(s) owned by '%s' and not by '%s'",
                       nrev, prev_profile, prof->name);
        }

        /* Record what the kernel ACCEPTED, so later drift checks compare like with like -
         * and say so when what it accepted is not what was asked for, which the baseline
         * alone cannot show. */
        {
            struct verifyreport vr;

            if (apply_verify(&ctx, &vr) < 0) {
                logmsg("warning: none of the %d applied settings could be read back, so "
                       "nothing was verified", ctx.nw);
            } else {
                if (vr.diverged)
                    logmsg("%d of %d applied settings hold a different value; first %s: "
                           "asked %s, holding %s", vr.diverged, vr.checked,
                           vr.path, vr.want, vr.got);
                else if (vr.checked)
                    logmsg("verified %d of %d applied settings read back as asked",
                           vr.checked, ctx.nw);
                if (vr.unreadable)
                    logmsg("warning: %d applied setting(s) could not be read back",
                           vr.unreadable);
            }
        }
        drift_baseline(&ctx);
        logmsg("watching %d of %d applied settings for drift", drift_n, ctx.nw);
    }

    /* --oneshot keeps the apply-and-exit behaviour, which is what --reapply and scripted
     * use need. Otherwise we stay resident even with no fan strategy: telemetry and status
     * are the daemon's job regardless, and the GUI has nothing to read if we exit. */
    if (oneshot) {
        rc = 0;
        goto out;
    }
    /* Ready: the profile is applied and the fan strategy resolved. Anything that failed
     * before here has already exited, and the parent returns that status. */
    startup_verdict(&ready_fd, 0);
    if (!foreground)
        log_detach();

    /* ========================================================================= */
    /*                              RESIDENT LOOP                                */
    /* ========================================================================= */
    /* Always samples, drives the fan only if asked ---- */
    {
        struct stats st;
        char cmd[512];
        int misses = 0, cur;
        int v_bad_logged = 0;       /* the schema complaint is made ONCE, not per line */
        long last_drift_ms = mono_ms(), unreach_since_ms = 0;
        int target_unreachable = 0;
        int sampler_attempt = 0;
        enum fanbase_verdict air_now = FANBASE_UNKNOWN, air_reported = FANBASE_UNKNOWN;
        long air_dev = 0;
        int air_streak = 0;
        long air_state_since_ms = mono_ms();
        int air_last_state = -1;
        long last_recover_ms = 0;

        if (stats_cmd) {
            snprintf(cmd, sizeof cmd, "%s", stats_cmd);
        } else {
            /* One sampler, owned here. The GUI must subscribe to us rather than
             * start a second one: each costs real power. */
            snprintf(cmd, sizeof cmd,
                     "qcstats --json --zones %s --interval %d",
                     strat_zone(&strat), strat_poll_ms(&strat));
        }
        if (stats_open(&st, cmd, err, sizeof err) < 0) {
            logmsg("cannot start sampler: %s", err);
            goto out;
        }
        logmsg("sampler: %s", cmd);

        /* Unprivileged clients ask for cooling here. Failure is NOT fatal: no socket
         * just means no unprivileged requests, and the daemon's own job is unaffected. */
        if (control_open(&ctl, ctlsock, err, sizeof err) < 0)
            logmsg("warning: no control socket (%s) - unprivileged --cool-max will fall "
                   "back to needing root", err);
        else
            logmsg("control socket: %s (mode 0666; requests can only RAISE cooling)",
                   ctlsock);

        /* Airflow health needs BOTH a captured reference and a user-declared delta. Absent
         * either, we judge nothing and publish null - never "ok", because a missing
         * judgement is not a clean one. */
        if (!fc.air.present) {
            logmsg("airflow: no AIRFLOW block in the fan config - not judging");
        } else if (fanbase_read(fanbasef, &fbase, err, sizeof err) < 0) {
            logmsg("airflow: no usable baseline (%s) - not judging. Capture one with "
                   "'qcctl --fan-baseline capture'", err);
        } else {
            have_fbase = 1;
            logmsg("airflow: judging against %s captured %s, delta %d%%, debounce %d",
                   fanbasef, fbase.captured[0] ? fbase.captured : "?",
                   fc.air.delta_pct, fc.air.debounce);
            {
                /* The daemon is where the baseline is ACTED on, so the staleness warning
                 * belongs here too and not only in --fan-baseline show. A stale baseline
                 * is wrong in a consistent direction: it would report "restricted" or
                 * "ok" steadily and plausibly. Warn and keep judging - refusing to judge
                 * on a newer kernel would turn "possibly off" into "definitely nothing". */
                struct envid live;
                char what[160];
                envid_read(&live);
                if (envid_differs(&fbase.env, &live, what, sizeof what))
                    logmsg("airflow: WARNING the baseline was captured in a different "
                           "environment (%s) - its rpm figures may no longer describe what "
                           "is sampled. Still judging; re-capture to be sure.", what);
            }
        }

        if (strat_active(&strat)) {
            if (fan_take(&rt, &s, &strat, err, sizeof err) < 0) {
                logmsg("cannot take the fan: %s", err);
                stats_close(&st);
                goto out;
            }
            cur = rt.fan_state_before[0] ? atoi(rt.fan_state_before) : 0;
        } else {
            /* No strategy: the kernel keeps the fan and we only observe. */
            logmsg("no fan strategy - the kernel keeps the fan; publishing status only");
            cur = -1;
        }

        /* Sampler staleness is tracked as a DEADLINE, not by counting poll timeouts: the
         * loop now also wakes for control traffic, so a timeout no longer means "no
         * sample arrived". Same lesson as the harness cadence fix - once something else
         * can wake you, counting wake-ups stops measuring time. */
        long last_sample_ms = mono_ms();
        long stale_after_ms = (long)strat_poll_ms(&strat) * 3 * MISS_LIMIT;
        long fanwd_last_ms = 0;
        long fanwd_forget_ms = FANWD_FORGET_MS;
        int fanwd_n = 0, fanwd_warned = 0;
        {
            const char *e = getenv(ENV_FANWD_FORGET);
            if (e && *e && atol(e) > 0)
                fanwd_forget_ms = atol(e);
        }
        int prev_holds = 0, coolmax_owned = 0;
        long coolmax_refused_ms = 0;
        struct strategy strat_next;
        int strat_pending = 0;

        memset(&strat_next, 0, sizeof strat_next);

        while (!stop_flag) {
            long temp;
            bool ok;
            enum stats_read r;
            struct pollfd pf[2 + CONTROL_MAXCLI];
            int nfds = 0, pr, holds;

            pf[nfds].fd = st.fd; pf[nfds].events = POLLIN; pf[nfds].revents = 0; nfds++;
            nfds += control_pollfds(&ctl, pf + nfds,
                                    (int)(sizeof pf / sizeof pf[0]) - nfds);

            pr = poll(pf, (nfds_t)nfds, CTL_TICK_MS);
            if (pr < 0) {
                if (errno == EINTR)
                    continue;
                logmsg("poll failed: %s - failing safe", strerror(errno));
                rc = 1;         /* non-zero: let systemd restart us */
                break;
            }

            /* Control first: it is the emergency path. */
            holds = control_service(&ctl, pf, nfds);

            /* A client asked us to re-assert the profile ROOT recorded as active. This is
             * how an unprivileged GUI switches profiles: pkexec does the privileged apply
             * and records the name, then nudges us so our published status does not go
             * stale and the drift baseline matches the new profile.
             *
             * The name comes from the root-owned active file, never from the request, so a
             * caller cannot choose a profile - only ask us to look again. */
            if (ctl.rebaseline) {
                char newname[NAME_LEN];
                ctl.rebaseline = 0;
                if (read_file_str(activef, newname, sizeof newname) < 0 || !newname[0]) {
                    logmsg("rebaseline: no profile recorded in %s - ignoring", activef);
                } else {
                    const struct profile *np = config_find(&pc, newname);
                    if (!np) {
                        logmsg("rebaseline: no profile '%s' in %s - ignoring",
                               newname, conf);
                    } else if (no_apply) {
                        logmsg("rebaseline: --no-apply, so only the reported name changes "
                               "('%s')", np->name);
                        prof = np;
                    } else {
                        struct applyctx rc2;
                        memset(&rc2, 0, sizeof rc2);
                        if (apply_plan(np, &s, &rc2, err, sizeof err) < 0)
                            logmsg("rebaseline: plan failed: %s", err);
                        else if (apply_commit(&rc2, err, sizeof err) < 0)
                            logmsg("rebaseline: apply failed: %s", err);
                        else {
                            prof = np;
                            {
                                struct verifyreport vr2;

                                if (apply_verify(&rc2, &vr2) < 0)
                                    logmsg("warning: none of the %d applied settings could "
                                           "be read back", rc2.nw);
                                else if (vr2.diverged)
                                    logmsg("%d of %d applied settings hold a different "
                                           "value; first %s: asked %s, holding %s",
                                           vr2.diverged, vr2.checked, vr2.path,
                                           vr2.want, vr2.got);
                            }
                            drift_baseline(&rc2);
                            last_drift_ms = mono_ms();
                            /* A rebaseline follows the CLI, which records a selection as
                             * it applies - so what is in force is now a deliberate choice,
                             * whatever got us here. */
                            prof_source = "selected";
                            logmsg("rebaseline: re-asserted '%s' (%d writes); watching %d "
                                   "settings for drift", np->name, rc2.nw, drift_n);
                            /* And does the new profile want a DIFFERENT fan strategy?
                             * Only noticed here; the swap itself waits for a safe point
                             * below, so an emergency cool-max hold is never interrupted. */
                            {
                                struct strategy ns;
                                char e3[ERRLEN];
                                if (strat_derive(np, &fc, &ns, e3, sizeof e3) < 0) {
                                    logmsg("rebaseline: %s - keeping the fan strategy "
                                           "already in force", e3);
                                } else if (ns.curve != strat.curve ||
                                           ns.target != strat.target) {
                                    strat_next = ns;
                                    strat_pending = 1;
                                    logmsg("rebaseline: fan strategy %s '%s' -> %s '%s'",
                                           strat_kind(&strat), strat_name(&strat),
                                           strat_kind(&ns), strat_name(&ns));
                                }
                            }
                        }
                    }
                }
            }
            if (holds != prev_holds) {
                if (holds > 0 && !rt.owned) {
                    char e2[ERRLEN];
                    if (fanown_take(&rt, &s, strat_zone(&strat), COOLMAX_CDEV,
                                    e2, sizeof e2) < 0) {
                        /* WE SAID OK AND THEN COULD NOT DO IT. The reply goes out when
                         * the request is accepted, so the client is already showing "the
                         * fan is held at maximum" - and nothing holds it. Drop the holds
                         * rather than leave that standing: the hold IS the connection, so
                         * closing it is how the client finds out, and it will retry. */
                        int dropped = control_drop_holds(&ctl);
                        /* RATE-LIMITED. A client retries once a second, so logging every
                         * refusal filled the journal with the same two lines for as long
                         * as the condition lasted - which buries everything else and is
                         * its own kind of unreadable. Say it immediately, then at most
                         * once a minute while nothing changes. */
                        long now_ms = mono_ms();
                        if (!coolmax_refused_ms ||
                            now_ms - coolmax_refused_ms > COOLMAX_REFUSE_QUIET_MS) {
                            coolmax_refused_ms = now_ms;
                            logmsg("cool-max request: cannot take the fan: %s", e2);
                            logmsg("cool-max: dropped %d hold(s) rather than report "
                                   "cooling that is not happening. The fan is NOT being "
                                   "held. If no other qcctld or qcctl is running, the "
                                   "zone was orphaned - run: qcctl-fan-release %s",
                                   dropped, strat_zone(&strat));
                        }
                        holds = 0;
                    } else {
                        coolmax_owned = 1;
                        logmsg("cool-max requested by %d client(s): took zone%d",
                               holds, rt.zone_idx);
                        if (rt.binding == 0)
                            logmsg("WARNING zone%d does not drive cooling device %d per the "
                                   "kernel's cdev links - emergency cooling may be "
                                   "overridden by the zone that does",
                                   rt.zone_idx, rt.cdev_idx);
                    }
                }
                if (holds > 0 && rt.owned) {
                    int mx = rt.max_state ? rt.max_state : 10;
                    if (fan_set(&rt, mx) == 0)
                        logmsg("cool-max: fan forced to state %d (%d client(s) holding)",
                               mx, holds);
                    cur = mx;
                } else if (holds == 0 && coolmax_owned) {
                    /* We held the fan only for the request. Hand it back exactly as the
                     * CLI does - one shared decision, because this logic was triplicated
                     * and all three copies compared against the wrong threshold. */
                    long trip_mc = 0;
                    int want_state = -1, set_failed;
                    enum fanown_release how =
                        fanown_release_mode(&s, strat_zone(&strat), have_temp_seen,
                                            last_temp_mc, &want_state);

                    fanown_active_trip(&s, strat_zone(&strat), &trip_mc);
                    set_failed = 0;
                    if (want_state >= 0 && fanown_set(&rt, want_state) < 0) {
                        logmsg("cool-max: could not set the fan to %d (%s) - releasing "
                               "anyway, it stays where it is", want_state, strerror(errno));
                        set_failed = 1;
                    }
                    if (fanown_release(&rt, how) < 0)
                        logmsg("cool-max RELEASE FAILED: %s", rt.release_err);
                    else if (how == FANOWN_LEAVE_MAX)
                        logmsg("cool-max: released; fan LEFT at maximum (%s)",
                               have_temp_seen ? "the kernel's own fan state could not be "
                                                "read" : "no temperature");
                    else if (set_failed)
                        logmsg("cool-max: released, but the fan could NOT be set to "
                               "state %d - left as it was", want_state);
                    else
                        logmsg("cool-max: released; fan set to state %d, what the kernel "
                               "holds at %ld.%ld C (trip %ld.%ld C)",
                               want_state,
                               last_temp_mc / 1000, (last_temp_mc % 1000) / 100,
                               trip_mc / 1000, (trip_mc % 1000) / 100);
                    coolmax_owned = 0;
                    cur = -1;
                }
                prev_holds = holds;
            }

            if (strat_pending && holds == 0 && !coolmax_owned) {
                char e3[ERRLEN];
                int resampled = 0;

                if (rt.owned)
                    fan_release(&rt, &strat, &s, have_temp_seen, last_temp_mc);
                strat = strat_next;
                strat_pending = 0;
                cur = -1;
                stale_after_ms = (long)strat_poll_ms(&strat) * 3 * MISS_LIMIT;

                /* --stats-cmd pins the command: that is the tests' canned stream, and
                 * restarting it would discard the fixture rather than re-read it. */
                if (!stats_cmd) {
                    char newcmd[sizeof cmd];
                    snprintf(newcmd, sizeof newcmd,
                             "qcstats --json --zones %s --interval %d",
                             strat_zone(&strat), strat_poll_ms(&strat));
                    if (strcmp(newcmd, cmd) != 0) {
                        stats_close(&st);
                        snprintf(cmd, sizeof cmd, "%s", newcmd);
                        if (stats_open(&st, cmd, e3, sizeof e3) < 0) {
                            logmsg("cannot restart the sampler for the new strategy: "
                                   "%s", e3);
                            rc = 1;
                            goto out;
                        }
                        logmsg("sampler: %s", cmd);
                        last_sample_ms = mono_ms();
                        resampled = 1;
                    }
                }

                if (strat_active(&strat)) {
                    if (fan_take(&rt, &s, &strat, e3, sizeof e3) < 0)
                        logmsg("strategy change: cannot take the fan: %s - leaving it "
                               "with the kernel", e3);
                    else
                        cur = rt.fan_state_before[0] ? atoi(rt.fan_state_before) : 0;
                } else {
                    logmsg("no fan strategy - the kernel keeps the fan; publishing "
                           "status only");
                }
                /* A new sampler means a new fd, and pf[] was filled from the old one. */
                if (resampled)
                    continue;
            }

            if (!(pf[0].revents & (POLLIN | POLLHUP | POLLERR))) {
                if (stop_flag)
                    break;
                if (mono_ms() - last_sample_ms > stale_after_ms) {
                    logmsg("no sample for %ld ms - the stream stalled",
                           mono_ms() - last_sample_ms);
                    publish_degraded(statusf, prof->name, prof_source, &strat);
                    if (sampler_recover(&st, cmd, &rt, &s, &strat,
                                        &sampler_attempt, &cur) < 0) {
                        rc = 1;
                        goto out;
                    }
                    last_sample_ms = last_recover_ms = mono_ms();
                }
                continue;
            }

            r = stats_sample(&st, 0, strat_zone(&strat), &temp, &ok);
            if (r == STATS_EOF || r == STATS_ERROR) {
                logmsg("sampler stream ended (%s) - failing safe", 
                       r == STATS_EOF ? "EOF" : "error");
                publish_degraded(statusf, prof->name, prof_source, &strat);
                if (sampler_recover(&st, cmd, &rt, &s, &strat,
                                    &sampler_attempt, &cur) < 0) {
                    rc = 1;             /* non-zero: let systemd restart us */
                    goto out;
                }
                last_sample_ms = last_recover_ms = mono_ms();
                continue;
            }
            if (r == STATS_TIMEOUT) {
                if (stop_flag)
                    break;
                if (mono_ms() - last_sample_ms > stale_after_ms) {
                    logmsg("no sample for %ld ms - failing safe",
                           mono_ms() - last_sample_ms);
                    rc = 1;     /* non-zero: let systemd restart us */
                    break;
                }
                continue;
            }
            last_sample_ms = mono_ms();

            /* Sustained health forgives past failures: the budget exists to catch a
             * flapping sampler, not to ration recovery over a long uptime.
             *
             * Checked HERE, where a complete line has just arrived, because a line
             * arriving is what "the sampler is healthy" means - the budget counts the
             * sampler PROCESS dying, not whether a temperature in it was usable. It
             * previously sat inside the stall branch two lines after
             * last_recover_ms = mono_ms(), so the elapsed time was always ~0 and the
             * reset could never fire: the budget was cumulative over the daemon's whole
             * lifetime, which is the exact thing it exists to prevent. Three isolated
             * hiccups days apart would force a restart. */
            if (sampler_attempt && last_recover_ms &&
                mono_ms() - last_recover_ms > SAMPLER_HEALTHY_MS) {
                logmsg("sampler healthy for %d s - retry budget reset",
                       SAMPLER_HEALTHY_MS / 1000);
                sampler_attempt = 0;
                last_recover_ms = 0;
            }

            /* Said once. At the poll rates this runs at, a per-line complaint would be the
             * noisiest possible way to report a single unchanging fact, and it would bury
             * the fail-safe message that follows. */
            if (st.v_bad && !v_bad_logged) {
                v_bad_logged = 1;
                if (st.v_bad_last < 0)
                    logmsg("sampler: a line carried no schema version - this is not "
                           "qcstats output. Every such line counts as NO READING.");
                else
                    logmsg("sampler: stream is schema v%ld, this build reads v%d. Every "
                           "such line counts as NO READING rather than risk misreading a "
                           "temperature this daemon drives a fan from. Upgrade qcctl.",
                           st.v_bad_last, STATS_SCHEMA_V);
            }

            if (!ok) {
                /* null or absent: a real answer meaning not measured. Hold the
                 * current state; never invent a temperature. */
                if (++misses >= MISS_LIMIT) {
                    /* Name the actual cause. "temperature null" would send someone looking
                     * at a thermal zone when the real problem is a schema this build does
                     * not read. */
                    if (st.v_bad >= misses && st.v_bad_last >= 0)
                        logmsg("no usable reading for %d samples (schema v%ld, we read v%d)"
                               " - failing safe", misses, st.v_bad_last, STATS_SCHEMA_V);
                    else if (st.v_bad >= misses)
                        /* No version at all. "schema v-1" is nonsense - -1 is this reader's
                         * marker for "absent", not a version anyone published. */
                        logmsg("no usable reading for %d samples (the stream carries no "
                               "schema version) - failing safe", misses);
                    else
                        logmsg("temperature null for %d samples - failing safe", misses);
                    rc = 1;     /* non-zero: let systemd restart us */
                    break;
                }
                continue;
            }
            misses = 0;
            last_temp_mc = temp;
            have_temp_seen = 1;
            /* A control hold overrides the strategy: an unprivileged caller asked for
             * maximum cooling and must not be argued with by a curve. */
            if (strat_active(&strat) && control_holds(&ctl) == 0) {
                /* The one place the two strategies differ. The safety ceiling is applied
                 * over whichever decided, because it belongs to neither. */
                int want = strat.curve
                         ? curve_next(strat.curve, temp, cur)
                         : target_next(strat.target, &strat.tstate, temp, cur, mono_ms());
                int next = fan_safety_clamp(want, cur, temp,
                                            rt.max_state ? rt.max_state : 10);

                if (next != want)
                    logmsg("SAFETY CEILING: %ld.%ldC is at or above %dC - forcing state %d",
                           temp / 1000, (temp % 1000) / 100,
                           SAFETY_CEILING_MC / 1000, next);
                if (next != cur) {
                    if (fan_set(&rt, next) < 0)
                        logmsg("warning: cannot write fan state %d: %s",
                               next, strerror(errno));
                    else
                        logmsg("%ld.%ldC: fan %d -> %d",
                               temp / 1000, (temp % 1000) / 100, cur, next);
                    cur = next;
                }
            }
            /* Not driving the fan: the kernel owns the zone and will not lower a state it
             * did not choose, because it rewrites a cooling device only on a trip
             * transition - which an idle board never makes. */
            if (fanwd_n && mono_ms() - fanwd_last_ms > fanwd_forget_ms) {
                logmsg("fan watchdog: nothing to correct for %ld minutes - the %d earlier "
                       "correction(s) are forgotten", fanwd_forget_ms / 60000, fanwd_n);
                fanwd_n = 0;
            }
            if (!rt.owned && fanwd_n < FANWD_TRIES
                && (!fanwd_last_ms || mono_ms() - fanwd_last_ms >= FANWD_INTERVAL_MS)) {
                long clear_mc = 0;
                int was = 0;
                int r = fanown_correct_stale(&s, strat_zone(&strat), temp,
                                             &clear_mc, &was);

                if (r > 0) {
                    fanwd_last_ms = mono_ms();
                    fanwd_n++;
                    logmsg("fan was state %d at %ld.%ldC, below the %ld.%ldC its trip "
                           "clears at - set to 0, which is the kernel's own answer there%s",
                           was, temp / 1000, (temp % 1000) / 100,
                           clear_mc / 1000, (clear_mc % 1000) / 100,
                           fanwd_n >= FANWD_TRIES
                           ? ". That is the third time, so something else is setting it;"
                             " not correcting it again." : "");
                } else if (r < 0 && !fanwd_warned) {
                    fanwd_warned = 1;
                    logmsg("warning: cannot read or write the fan state of zone '%s' "
                           "(%s) - a state left behind there will not be corrected",
                           strat_zone(&strat), strerror(errno));
                }
            }

            /* Drift check on its own slow timer: a dozen sysfs reads, and drift is a
             * human-timescale event. */
            if (drift_n >= 0 && mono_ms() - last_drift_ms >= drift_ms) {
                int was = drift_count;
                last_drift_ms = mono_ms();
                if (drift_check() != was) {
                    if (drift_count)
                        logmsg("DRIFT: %d applied setting(s) no longer match, first %s "
                               "- profile '%s' is no longer fully in force",
                               drift_count, drift_first, prof->name);
                    else
                        logmsg("drift cleared: all applied settings match again");
                }
            }

            /* Can TARGET actually reach its target? §11h had it sit at maximum, +5.3 C
             * above target, for 436 s in silence. Report it: this is a STATUS, not a
             * fault - the fan is doing everything it can and the board is in no danger.
             * Only assert it after it has persisted, so a transient climb does not
             * trigger it. */
            if (strat.target) {
                int mx = rt.max_state ? rt.max_state : 10;
                long err = temp - strat.target->target_mc;
                int saturated = (cur >= mx) && (err > strat.target->deadband_mc);

                if (!saturated) {
                    if (unreach_since_ms && target_unreachable)
                        logmsg("TARGET %ld.%ldC: reachable again",
                               strat.target->target_mc / 1000,
                               (strat.target->target_mc % 1000) / 100);
                    unreach_since_ms = 0;
                    target_unreachable = 0;
                } else {
                    if (!unreach_since_ms)
                        unreach_since_ms = mono_ms();
                    else if (!target_unreachable &&
                             mono_ms() - unreach_since_ms > 3L * strat.target->step_ms) {
                        target_unreachable = 1;
                        logmsg("TARGET %ld.%ldC CANNOT BE HELD: fan at maximum (%d) and "
                               "still %ld.%ldC above target - more airflow is needed, not "
                               "more fan",
                               strat.target->target_mc / 1000,
                               (strat.target->target_mc % 1000) / 100,
                               mx, err / 1000, (err % 1000) / 100);
                    }
                }
            }

            {
                struct fanobs fo;
                struct cpuobs co;
                struct statusinfo si;

                stats_extract_fan(stats_last_line(&st), &fo);
                /* RELAYED, not observed: the daemon is already holding this line, so
                 * copying the per-policy frequencies out of it adds no sampler and no
                 * sysfs read. Nothing here acts on them - they exist so a GUI never has
                 * a reason to start a second qcstats. */
                stats_extract_cpu(stats_last_line(&st), &co);

                /* ---- airflow health ----
                 * Judge rpm against the captured baseline AT THE SAME FAN STATE. Across
                 * states rpm merely tracks pwm; at a fixed state pwm is fixed, so the
                 * variation is purely aerodynamic. That is the whole basis of this.
                 *
                 * The state comes from what WE commanded when we own the fan, and from
                 * qcstats' duty_pct otherwise (duty% = state * 10 on this controller).
                 * rpm always comes from qcstats - we never read the tach. */
                if (have_fbase) {
                    int fstate = -1;

                    if (rt.owned && cur >= 0)
                        fstate = cur;
                    else if (fo.duty_ok)
                        fstate = (int)((fo.duty_pct + 5) / 10);

                    if (fstate != air_last_state) {
                        air_last_state = fstate;
                        air_state_since_ms = mono_ms();
                        air_streak = 0;              /* the transient is not evidence */
                    }
                    if (fstate < 0 || !fo.rpm_ok || !fo.rpm_valid ||
                        mono_ms() - air_state_since_ms < AIRFLOW_DWELL_MS) {
                        air_now = FANBASE_UNKNOWN;
                        air_dev = 0;
                    } else {
                        enum fanbase_verdict v =
                            fanbase_judge(&fbase, fstate, fo.rpm, fc.air.delta_pct, &air_dev);
                        /* Debounce: a single sample is not a fault. Only a run of the SAME
                         * verdict counts, so an rpm blip cannot flip it. */
                        if (v == air_now) {
                            if (air_streak < 1000)
                                air_streak++;
                        } else {
                            air_now = v;
                            air_streak = 1;
                        }
                        if (air_streak >= fc.air.debounce && air_reported != air_now) {
                            enum fanbase_verdict air_reported_prev = air_reported;
                            air_reported = air_now;
                            if (air_now == FANBASE_RESTRICTED)
                                logmsg("AIRFLOW: fan at state %d is running %+ld%% vs its "
                                       "baseline (%ld rpm) - the intake looks restricted",
                                       fstate, air_dev, fbase.mean[fstate]);
                            else if (air_now == FANBASE_DEGRADED)
                                logmsg("AIRFLOW: fan at state %d is running %+ld%% vs its "
                                       "baseline (%ld rpm) - it is not delivering; check "
                                       "for dust or a worn bearing",
                                       fstate, air_dev, fbase.mean[fstate]);
                            else if (air_now == FANBASE_OK)
                                logmsg("airflow: %swithin %d%% of baseline at state %d",
                                       /* "back" only if we had previously said otherwise */
                                       (air_reported_prev == FANBASE_RESTRICTED ||
                                        air_reported_prev == FANBASE_DEGRADED)
                                           ? "back " : "",
                                       fc.air.delta_pct, fstate);
                        }
                    }
                }

                memset(&si, 0, sizeof si);
                si.profile    = prof->name;
                si.profile_source = prof_source;
                si.g          = &strat;
                si.have_temp  = 1;
                si.temp_mc    = temp;
                si.have_fan   = (cur >= 0);
                si.fan_state  = cur;
                si.max_state  = rt.max_state;
                si.fan_owned  = rt.owned;
                si.safety     = fan_safety_active(temp, cur < 0 ? 0 : cur,
                                                  rt.max_state ? rt.max_state : 10);
                si.fo         = &fo;
                si.co         = &co;
                si.holds      = control_holds(&ctl);
                si.drift_known = (drift_n >= 0);
                si.drift_count = drift_count;
                si.drift_first = drift_first;
                /* Only publish a verdict once the debounce has been met; before that the
                 * honest answer is "no judgement yet", not a provisional one. */
                si.airflow_known = have_fbase && air_streak >= fc.air.debounce;
                si.airflow = air_reported;
                si.airflow_dev_pct = air_dev;
                if (strat.target) {
                    si.have_target  = 1;
                    si.target_mc    = strat.target->target_mc;
                    si.target_err_mc = temp - strat.target->target_mc;
                    si.target_unreachable = target_unreachable;
                } else {
                    si.target_unreachable = -1;   /* no TARGET: unknown, not false */
                }
                publish_status(statusf, &si);
            }
        }
        control_close(&ctl);
        stats_close(&st);
        if (st.killed)
            logmsg("the sampler did not exit on SIGTERM and was killed");
        /* ONLY a clean stop is a success. Every other way out of the loop above is a
         * fail-safe, and systemd has to be told so. This was an unconditional rc = 0,
         * which overwrote the rc = 1 those paths set and reported a daemon that had gone
         * blind as "Deactivated successfully" - so Restart=on-failure had nothing to act
         * on and the board silently lost its curve, its status file and its drift watch
         * until a human noticed. Same defect class as the sampler-EOF exit fixed on
         * 2026-10-03; these were the paths that fix did not cover.
         *
         * rc is 1 from its declaration, so a failure break is caught here even without an
         * assignment. The explicit ones above are kept so each failure says so at the site. */
        if (stop_flag)
            rc = 0;
    }

out:
    fan_release(&rt, &strat, &s, have_temp_seen, last_temp_mc);
    if (made_pid) {
        unlink(pidf);
        /* No daemon means no profile in force, so the hook must not re-apply one. */
        unlink(activef);
        /* A stale snapshot is worse than none: the GUI would show numbers that stopped
         * being true at an unknown moment. */
        unlink(statusf);
    }
    logmsg("qcctld stopped");
    return rc;
}
