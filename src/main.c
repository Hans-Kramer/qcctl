// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer
//
// qcctl CLI. Applies named profiles from a config to the board.
//
// Writes need root; everything else runs unprivileged. There is no implicit
// action: with no arguments it reports and changes nothing.

#define _GNU_SOURCE
#include "apply.h"
#include "config.h"
#include "surface.h"
#include "io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fanown.h"
#include "fanbase.h"
#include "envid.h"
#include <time.h>
#include "control.h"
#include <sys/un.h>
#include <sys/socket.h>
#include <poll.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>

#define DEFAULT_CONF  "/etc/qcctl/profiles.conf"
#define DEFAULT_STATE "/var/lib/qcctl/boot-state"
#define DEFAULT_ACTIVE "/run/qcctl/active"
/* In /var/lib, not /run: this must survive the daemon exiting. See selection_save(). */
#define DEFAULT_SELECTED "/var/lib/qcctl/selected"
#define COOLMAX_STATUS "/run/qcctl/status"
#define FANBASE_PATH   "/var/lib/qcctl/fan-baseline"

static void usage(void)
{
    printf(
"usage: qcctl [options]\n"
"\n"
"  -q, --query            report the current state and exit\n"
"  -l, --list             list the profiles in the config\n"
"  -m, --mode <name|id>   apply a profile\n"
"      --dry-run          with -m: print the writes, perform none\n"
"      --reset            restore the saved boot state\n"
"      --save-state       capture the current state as the restore point\n"
"      --verify           parse and validate the config, then exit\n"
"      --reapply          re-apply the profile named in the active-file. Used by the\n"
"                         resume hook; a no-op when no profile is in force\n"
"      --active-file <p>  default %s\n"
"      --selection-file <p>\n"
"                         where -m records the operator's choice, so a daemon restart\n"
"                         honours it instead of reverting to DEFAULT. Boot-id scoped:\n"
"                         a power cycle returns to DEFAULT. Default /var/lib/qcctl/selected\n"
"  -c, --config <path>    config file (default %s)\n"
"      --state <path>     boot-state file (default %s)\n"
"      --allow-disruptive permit settings whose effect is drastic. Currently only\n"
"                         offlining cpu0, the boot CPU\n"
"      --json             machine-readable output for -q and -l. Consumers must use\n"
"                         this, never the human layout\n"
"      --fan-baseline capture|show\n"
"                         capture this board's healthy rpm-per-state reference, or print\n"
"                         the stored one. Capture drives the fan and takes ~2 minutes;\n"
"                         --force accepts values that look obstructed\n"
"      --cool-max         run the fan(s) at full speed until stopped (Ctrl-C or SIGTERM).\n"
"                         Needs no privilege when qcctld is running: it asks the\n"
"                         daemon, which can only ever RAISE cooling\n"
"      --status-file <p>  where to read the daemon's published temperature, for\n"
"                         --cool-max's handback decision (default %s)\n"
"  -V, --version          print the version and exit\n"
"  -h, --help             this text\n",
        // %s order: active-file, config, state, status-file
        DEFAULT_ACTIVE, DEFAULT_CONF, DEFAULT_STATE, COOLMAX_STATUS);
}

// QCCTL_VERSION comes from the Makefile so a release needs no source edit. "unknown" is
// the honest answer for a hand-compiled binary, and is deliberately not a plausible-looking
// number: a wrong version is worse than an admitted gap, the same rule the sampler follows
// for a reading it could not take.
#ifndef QCCTL_VERSION
#define QCCTL_VERSION "unknown"
#endif

// One line, binary name then version, so `qcctl -V | cut -d' ' -f2` works and the name
// identifies WHICH of this project's binaries answered.
static void version(void)
{
    printf("qcctl %s\n", QCCTL_VERSION);
}

// --cool-max. Hold the fan at maximum until the operator stops us.
#define COOLMAX_ZONE "cpuss-0-0"
#define COOLMAX_CDEV "emc2305_fan"

/* Find a JSON key and return a pointer just past its colon, whitespace skipped. Exists so
 * no caller hand-counts a key length - see the bug noted in status_fan_rpm(). */
static const char *json_after_key(const char *buf, const char *key)
{
    const char *k = strstr(buf, key);

    if (!k)
        return NULL;
    k += strlen(key);
    while (*k == ' ' || *k == '\t')
        k++;
    return k;
}

/* temp_mc from the daemon's published status snapshot. A narrow single-field scan, and it
 * must treat null as "no reading" rather than zero. */
static int status_temp_mc(const char *path, long *out)
{
    char buf[1024];
    const char *k;
    FILE *f = fopen(path, "r");
    size_t n;

    if (!f)
        return 0;
    n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = '\0';
    fclose(f);
    k = json_after_key(buf, "\"temp_mc\":");
    if (!k)
        return 0;
    if (!strncmp(k, "null", 4))
        return 0;
    *out = strtol(k, NULL, 10);
    return 1;
}

// Every action that writes needs root against the real system.
static int needs_root(const char *action)
{
    const char *sr = getenv(ENV_SYSROOT);

    if ((sr && *sr) || geteuid() == 0)
        return 0;
    fprintf(stderr, "%s needs root: it writes root-owned attributes under /sys.\n", action);
    fprintf(stderr, "  try:  sudo qcctl %s\n", action);
    return 1;
}

/* How long --cool-max keeps trying to get its hold back when the daemon disappears, in
 * one-second steps. The unit restarts 5s after a failure, so this allows for several
 * restarts (or a slow one under load) before admitting the fan is unheld. */
#define COOLMAX_REHOLD_TRIES 30
/* How long a re-established hold must SURVIVE before it is announced. The daemon drops an
 * unhonourable hold on its very next loop pass, so this only has to outlast that. */
#define COOLMAX_CONFIRM_MS   1500

static volatile sig_atomic_t coolmax_stop;
static void coolmax_sig(int sig) { (void)sig; coolmax_stop = 1; }

/* rpm from the daemon's published status, honouring its validity flag.
 *
 * NOT a direct read of the tach: qcstats observes, qcctl acts. The daemon owns the one
 * sampler and republishes what it sees, so this is the in-architecture way to learn an rpm -
 * the same route --cool-max uses for temperature. */
static int status_fan_rpm(const char *path, long *rpm)
{
    char buf[1024];
    const char *k;
    FILE *f = fopen(path, "r");
    size_t n;

    if (!f)
        return 0;
    n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = '\0';
    fclose(f);

    /* Offsets are derived from the key, never hand-counted. The first version of this
     * advanced 17 bytes past a 16-byte key, so it compared "rue" to "true" and every
     * reading looked untrustworthy - which the capture then refused, on hardware. */
    k = json_after_key(buf, "\"fan_rpm_valid\":");
    if (!k || strncmp(k, "true", 4))
        return 0;                       /* the sampler says do not trust it */
    k = json_after_key(buf, "\"fan_rpm\":");
    if (!k)
        return 0;
    while (*k == ' ')
        k++;
    if (!strncmp(k, "null", 4))
        return 0;
    *rpm = strtol(k, NULL, 10);
    /* Zero is a REAL reading - the fan is off. Returning false for it made the pre-flight
     * below demand a spinning fan before the capture that spins it, which is exactly how
     * the first hardware run failed. Callers needing motion check the value themselves. */
    return (*rpm >= 0);
}

/* The free-air figures from section 10c, used ONLY as a sniff test on a fresh capture.
 * They are our fan in free air and vary +/-2 % between sessions, so they are useless as a
 * judgement reference - but a captured state wildly off them suggests the user is baking an
 * existing obstruction into their own baseline, which is the one failure mode that turns
 * this whole feature into a silent no-op. */
static long freeair_ref(int state)
{
    switch (state) {
    case 4:  return 2616;
    case 6:  return 3648;
    case 8:  return 4352;
    case 10: return 5260;
    default: return 0;      /* no reference for this state; nothing to sniff */
    }
}

/* Ask the daemon to cool the board. Returns 0 if it accepted and we held until stopped,
 * -1 if there is no daemon to ask (caller falls back to doing it directly).
 *
 * The HOLD IS THE CONNECTION. There is no release message: when this process exits for
 * any reason - Ctrl-C, SIGTERM, SIGKILL, a crash - the socket closes and the daemon drops
 * the hold. That is why a killed client cannot leave the fan pinned. */
/* Send one request to the daemon and read its reply. Returns 0 on "OK", -1 if there is no
 * daemon to talk to. Used for the fire-and-forget REBASELINE; COOLMAX needs to keep the
 * connection open instead, because the connection IS the hold. */
static int control_tell(const char *sockpath, const char *req)
{
    struct sockaddr_un a;
    char reply[32];
    int fd;
    ssize_t n;

    if (strlen(sockpath) >= sizeof a.sun_path)
        return -1;
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", sockpath);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return -1; }
    if (write(fd, req, strlen(req)) < 0) { close(fd); return -1; }
    n = read(fd, reply, sizeof reply - 1);
    close(fd);
    if (n <= 0)
        return -1;
    reply[n] = '\0';
    return strncmp(reply, "OK", 2) ? -1 : 0;
}

static int cool_max_via_daemon(const char *sockpath)
{
    struct sockaddr_un a;
    char reply[32];
    int fd;
    ssize_t n;

    if (strlen(sockpath) >= sizeof a.sun_path)
        return -1;
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", sockpath);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        close(fd);
        return -1;                      /* no daemon listening */
    }
    if (write(fd, CONTROL_REQ_COOLMAX, strlen(CONTROL_REQ_COOLMAX)) < 0) {
        close(fd);
        return -1;
    }
    n = read(fd, reply, sizeof reply - 1);
    if (n <= 0) {
        close(fd);
        return -1;
    }
    reply[n] = '\0';
    if (strncmp(reply, "OK", 2)) {
        fprintf(stderr, "cool-max: daemon refused: %s", reply);
        close(fd);
        return 1;
    }
    printf("cool-max: qcctld is cooling at maximum. No privilege needed - the\n");
    printf("cool-max: daemon does the work, and it can only ever RAISE cooling.\n");
    printf("cool-max: stop with Ctrl-C; the hold ends when this process exits, however\n");
    printf("cool-max: it exits - the hold IS this connection. On release the daemon puts\n");
    printf("cool-max: the fan back to the state the kernel itself would hold: off below\n");
    printf("cool-max: the fan trip, the cooling map's floor at or above it.\n");

    /* THE HOLD IS THE CONNECTION, so the daemon going away ENDS IT - and this used to sit
     * in pause() for ever afterwards, still showing the message above. Someone who ran
     * this because their board was hot had no way to learn that cooling had stopped: the
     * fan stays high only because a release never lowers it, and the kernel steps it down
     * at its next downward trip crossing with nothing left to re-raise it.
     *
     * So watch the socket. A HUP or EOF means the hold is gone and must be re-established,
     * not waited out. The unit restarts in 5s (Restart=on-failure, RestartSec=5), so a
     * crash or an upgrade is recoverable and worth waiting for - but it is announced
     * immediately, because the board may be hot RIGHT NOW. */
    while (!coolmax_stop) {
        struct pollfd pfd;
        int pr;

        pfd.fd = fd;
        pfd.events = POLLIN;            /* the daemon never writes again; this is for HUP */
        pfd.revents = 0;
        pr = poll(&pfd, 1, 1000);
        if (pr < 0) {
            if (errno == EINTR)
                continue;               /* a signal; coolmax_stop decides */
            break;
        }
        if (pr == 0)
            continue;
        if (!(pfd.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)))
            continue;
        /* Either the peer closed (hold gone) or it sent something. A read of 0 is the
         * close; anything else we ignore, since the protocol is one request one reply. */
        {
            char junk[32];
            ssize_t r = read(fd, junk, sizeof junk);
            if (r > 0)
                continue;
        }

        fprintf(stderr, "\ncool-max: THE DAEMON HAS GONE AWAY AND THE HOLD IS GONE WITH\n");
        fprintf(stderr, "cool-max: IT - the fan is no longer being held at maximum. It is\n");
        fprintf(stderr, "cool-max: still spinning (a release never lowers it), but nothing\n");
        fprintf(stderr, "cool-max: will raise it again. Trying to re-establish the hold...\n");
        close(fd);
        fd = -1;

        /* The daemon restarts in 5s; give it a real budget before giving up. */
        for (int tries = 0; tries < COOLMAX_REHOLD_TRIES && !coolmax_stop; tries++) {
            sleep(1);
            fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            if (fd < 0)
                continue;
            if (connect(fd, (struct sockaddr *)&a, sizeof a) == 0
                && write(fd, CONTROL_REQ_COOLMAX, strlen(CONTROL_REQ_COOLMAX)) >= 0) {
                n = read(fd, reply, sizeof reply - 1);
                if (n > 0) {
                    reply[n] = '\0';
                    if (!strncmp(reply, "OK", 2)) {
                        /* CONFIRM BEFORE CLAIMING. "OK" means the request was accepted,
                         * not that the fan was taken - the daemon tries that afterwards
                         * and drops the hold if it fails. Announcing success on the reply
                         * alone printed "the fan is held at maximum again" one second
                         * before discovering it was not. So wait out a short window: if
                         * the hold is going to be dropped, it is dropped immediately. */
                        struct pollfd cfd;
                        cfd.fd = fd;
                        cfd.events = POLLIN;
                        cfd.revents = 0;
                        if (poll(&cfd, 1, COOLMAX_CONFIRM_MS) == 0) {
                            fprintf(stderr, "cool-max: hold RE-ESTABLISHED after %d s - "
                                    "the fan is held at maximum again.\n", tries + 1);
                            break;
                        }
                        /* Dropped inside the window: not honoured. Keep trying, and do
                         * not print a success we would have to retract. */
                    }
                }
            }
            close(fd);
            fd = -1;
        }
        if (fd < 0) {
            /* ASKED TO STOP IS NOT THE SAME AS GAVE UP. Ctrl-C during the reconnect
             * window broke out of the loop above, and this then announced "COULD NOT
             * RE-ESTABLISH THE HOLD after 30 s - NOTHING IS HOLDING THE FAN" at someone
             * who had simply pressed Ctrl-C. A false alarm on the emergency path teaches
             * people to ignore the real one. */
            if (coolmax_stop) {
                printf("\ncool-max: stopped before the hold could be re-established.\n");
                return 0;
            }
            /* Out of budget for real. Returning -1 hands this to the caller's existing
             * no-daemon fallback, which takes the fan directly when root and otherwise
             * prints how to proceed. Cooling is NOT held meanwhile, and saying so is the
             * only honest thing to do. */
            fprintf(stderr, "cool-max: COULD NOT RE-ESTABLISH THE HOLD after %d s.\n",
                    COOLMAX_REHOLD_TRIES);
            fprintf(stderr, "cool-max: NOTHING IS HOLDING THE FAN AT MAXIMUM. If no other\n");
            fprintf(stderr, "cool-max: qcctld or qcctl is running, the fan's zone was\n");
            fprintf(stderr, "cool-max: orphaned - recover it with:\n");
            fprintf(stderr, "cool-max:   sudo qcctl-fan-release\n");
            return -1;
        }
    }

    if (fd >= 0)
        close(fd);                      /* this IS the release */
    printf("\ncool-max: hold released.\n");
    return 0;
}

/* Capture a fan baseline: drive each usable state, let it settle, average the rpm.
 *
 * Takes the thermal zone, because a bare cur_state write is walked away by step_wise. On
 * exit it hands back exactly as --cool-max does - below the governor's release point write
 * 0, above it leave the fan high, never lower a hot board.
 *
 * States 2..max only: state 0 is 0 rpm and state 1's tach misreads, so neither carries a
 * signal worth recording.
 */
#define FB_DWELL_MS   8000      /* let the impeller and the tach settle after a state change */
#define FB_SAMPLES    4
#define FB_SAMPLE_MS  2500      /* qcstats samples at 2 s, so faster than this just repeats */

static int fan_baseline_capture(const struct surface *s, const char *statusf,
                                const char *outpath, int force)
{
    struct fanown fo;
    struct fanbase fb;
    char err[ERRLEN];
    const struct cdev *cd;
    int top, suspect = 0, rc = 1;

    memset(&fb, 0, sizeof fb);

    {
        long probe = -1;
        /* Only that the daemon is PUBLISHING a trustworthy rpm field - not that the fan is
         * already turning. It is off before we drive it. */
        if (!status_fan_rpm(statusf, &probe)) {
            fprintf(stderr, "fan-baseline: no trustworthy rpm available from %s\n", statusf);
            fprintf(stderr, "fan-baseline: qcctld must be running - it owns the "
                            "sampler.\n");
            fprintf(stderr, "fan-baseline:   sudo systemctl start qcctld\n");
            return 1;
        }
    }
    if (fanown_take(&fo, s, COOLMAX_ZONE, COOLMAX_CDEV, err, sizeof err) < 0) {
        fprintf(stderr, "fan-baseline: %s\n", err);
        return 1;
    }
    /* A capture drives the fan through every state and records the rpm. If another zone can
     * override those states, the recorded figures describe something other than what we
     * commanded - and a baseline that is wrong in a consistent direction is exactly the
     * failure this feature cannot detect in itself. Worth saying before the two minutes. */
    if (fo.binding == 0)
        fprintf(stderr, "fan-baseline: WARNING the kernel's cdev links say zone %s does not "
                "drive %s. States commanded during the capture may be overridden, which "
                "would record rpm for states the fan was not actually at.\n",
                COOLMAX_ZONE, COOLMAX_CDEV);
    cd = surface_cdev(s, COOLMAX_CDEV);
    top = fo.max_state ? fo.max_state : FANBASE_MAX_STATE;
    if (top > FANBASE_MAX_STATE)
        top = FANBASE_MAX_STATE;

    snprintf(fb.board, sizeof fb.board, "%.*s", (int)sizeof fb.board - 1,
             s->model[0] ? s->model : "unknown");
    snprintf(fb.cdev, sizeof fb.cdev, "%s", COOLMAX_CDEV);
    fb.max_state = top;
    envid_read(&fb.env);
    {
        time_t t = time(NULL);
        struct tm g;
        if (gmtime_r(&t, &g))
            strftime(fb.captured, sizeof fb.captured, "%Y-%m-%dT%H:%M:%SZ", &g);
    }
    (void)cd;

    printf("fan-baseline: capturing states %d..%d; about %d seconds.\n",
           FANBASE_MIN_STATE, top,
           (top - FANBASE_MIN_STATE + 1) * (FB_DWELL_MS + FB_SAMPLES * FB_SAMPLE_MS) / 1000);
    printf("fan-baseline: leave the board alone and the airflow AS IT NORMALLY IS - this\n");
    printf("fan-baseline: records your enclosure, not a laboratory.\n");
    /* stdout is block-buffered when redirected, so without this a two-and-a-half minute
     * command prints nothing until it exits and looks hung. Observed on the first run. */
    fflush(stdout);

    for (int st = FANBASE_MIN_STATE; st <= top; st++) {
        long sum = 0, lo = 0, hi = 0, ref;
        int got = 0;

        if (fanown_set(&fo, st) < 0) {
            fprintf(stderr, "fan-baseline: cannot set state %d: %s\n",
                    st, strerror(errno));
            goto done;
        }
        usleep((useconds_t)FB_DWELL_MS * 1000);
        for (int i = 0; i < FB_SAMPLES; i++) {
            long rpm = 0;
            usleep((useconds_t)FB_SAMPLE_MS * 1000);
            if (!status_fan_rpm(statusf, &rpm) || rpm <= 0)
                continue;       /* a DRIVEN fan reading zero is a fault, not a datum */
            if (!got || rpm < lo) lo = rpm;
            if (!got || rpm > hi) hi = rpm;
            sum += rpm;
            got++;
        }
        if (!got) {
            fprintf(stderr, "fan-baseline: no usable rpm at state %d - aborting\n", st);
            goto done;
        }
        fb.have[st] = 1;
        fb.mean[st] = sum / got;
        fb.lo[st] = lo;
        fb.hi[st] = hi;
        fb.n[st] = got;

        ref = freeair_ref(st);
        if (ref && labs(fb.mean[st] - ref) * 100 / ref > 25) {
            printf("  state %-2d %5ld rpm (%ld-%ld, n=%d)  ** %+ld%% vs our free-air %ld **\n",
                   st, fb.mean[st], lo, hi, got,
                   (fb.mean[st] - ref) * 100 / ref, ref);
            suspect++;
        } else {
            printf("  state %-2d %5ld rpm (%ld-%ld, n=%d)\n", st, fb.mean[st], lo, hi, got);
        }
        fflush(stdout);         /* progress, not a surprise at the end */
    }

    if (suspect && !force) {
        fprintf(stderr, "\nfan-baseline: %d state(s) are more than 25%% away from the "
                        "free-air figures\n", suspect);
        fprintf(stderr, "fan-baseline: this board measured (section 10c). That can be a "
                        "different fan or a\n");
        fprintf(stderr, "fan-baseline: different enclosure - both fine - but it can also "
                        "mean you are\n");
        fprintf(stderr, "fan-baseline: recording an EXISTING obstruction as your healthy "
                        "reference, which\n");
        fprintf(stderr, "fan-baseline: would make the detector permanently blind.\n");
        fprintf(stderr, "fan-baseline: check the intake, then re-run with --force to accept "
                        "these values.\n");
        goto done;
    }
    if (fanbase_write(outpath, &fb, err, sizeof err) < 0) {
        fprintf(stderr, "fan-baseline: %s\n", err);
        goto done;
    }
    printf("\nfan-baseline: written to %s\n", outpath);
    printf("fan-baseline: set AIRFLOW DELTA in fan.config to start judging against it.\n");
    rc = 0;

done:
    {
        long temp_mc = 0;
        int have_temp = status_temp_mc(statusf, &temp_mc);
        int want_state = -1;
        enum fanown_release how =
            fanown_release_mode(s, COOLMAX_ZONE, have_temp, temp_mc, &want_state);

        if (want_state >= 0 && fanown_set(&fo, want_state) < 0)
            fprintf(stderr, "fan-baseline: could not set the fan to %d (%s) - "
                    "releasing anyway, it stays where it is\n", want_state, strerror(errno));
        if (fanown_release(&fo, how) < 0)
            fprintf(stderr, "fan-baseline: RELEASE FAILED: %s\n", fo.release_err);
    }
    return rc;
}

static int cool_max(const struct surface *s, const char *statusf)
{
    struct sigaction sa;
    struct fanown fo;
    char err[ERRLEN];
    int want;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = coolmax_sig;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    if (fanown_take(&fo, s, COOLMAX_ZONE, COOLMAX_CDEV, err, sizeof err) < 0) {
        fprintf(stderr, "cool-max: %s\n", err);
        return 1;
    }
    /* Reported, not fatal: the fan is still commanded to maximum either way, and refusing
     * here would leave a hot board with nothing. Goes to stderr so it cannot be mistaken
     * for part of the normal confirmation on stdout. */
    if (fo.binding == 0)
        fprintf(stderr, "cool-max: WARNING the kernel's cdev links say zone %s does not "
                "drive %s. The fan is set to maximum, but the zone that does drive it may "
                "override that.\n", COOLMAX_ZONE, COOLMAX_CDEV);
    want = fo.max_state ? fo.max_state : 10;
    if (fanown_set(&fo, want) < 0) {
        fprintf(stderr, "cool-max: cannot write fan state %d: %s\n",
                want, strerror(errno));
        if (fanown_release(&fo, FANOWN_LEAVE_MAX) < 0)
            fprintf(stderr, "cool-max: RELEASE FAILED: %s\n", fo.release_err);
        return 1;
    }
    printf("cool-max: fan at state %d of %d, zone%d taken from the kernel.\n",
           want, fo.max_state, fo.zone_idx);
    printf("cool-max: stop with Ctrl-C or SIGTERM. On exit the fan is set to the state\n");
    printf("cool-max: the kernel itself would hold: 0 below the fan trip, the cooling\n");
    printf("cool-max: map's floor at or above it. Nothing is left for it to walk back.\n");

    while (!coolmax_stop)
        pause();

    /* How to hand back depends on how hot the board still is, and the honest source for
     * that is the daemon's published status - NOT a direct read of the zone's own temp.
     * qcstats observes, qcctl acts; the daemon owns the one sampler and publishes
     * what it sees, so consuming that is the in-architecture way to learn a temperature.
     *
     * The fan is left in the state the governor itself would hold, so nothing has to be
     * walked back: 0 below the active trip, and the cooling map's lower limit at or above
     * it. Lowering a warm board to that limit is deliberate - it is the governor's own
     * answer there, measured - and is NOT the same as lowering it below what the governor
     * wants, which remains the one move that can do harm.
     *
     * Leaving maximum instead is wrong rather than dangerous: with no polling_delay the
     * governor rewrites a cooling device only when its own target changes, and a downward
     * trip transition moves one state, so a value it would not have chosen persists.
     * Observed: still state 10 at 28.6 C a quarter of a minute after release.
     *
     * No temperature, or no readable limit: leave maximum, because without them there is
     * no way to know what the governor wants. */
    {
        long temp_mc = 0, trip_mc = 0;
        int have_temp = status_temp_mc(statusf, &temp_mc);
        int want_state = -1, set_failed = 0;
        enum fanown_release how =
            fanown_release_mode(s, COOLMAX_ZONE, have_temp, temp_mc, &want_state);

        fanown_active_trip(s, COOLMAX_ZONE, &trip_mc);
        if (want_state >= 0 && fanown_set(&fo, want_state) < 0) {
            fprintf(stderr, "cool-max: could not set the fan to %d (%s) - releasing "
                    "anyway, it stays where it is\n", want_state, strerror(errno));
            set_failed = 1;
        }
        if (fanown_release(&fo, how) < 0)
            fprintf(stderr, "\ncool-max: RELEASE FAILED: %s\n", fo.release_err);
        else if (how == FANOWN_LEAVE_MAX) {
            printf("\ncool-max: zone%d returned to the kernel, fan LEFT at state %d.\n",
                   fo.zone_idx, want);
            if (!have_temp)
                printf("cool-max: no temperature available (is qcctld running?), so the "
                       "fan was not lowered - that is the safe direction.\n");
            else
                printf("cool-max: the kernel's own fan state for this zone could not be "
                       "read, so the fan was not lowered - that is the safe direction.\n");
            printf("cool-max: the kernel lowers it by one state per trip transition, "
                   "which can take many heat/cool cycles.\n");
        } else if (set_failed) {
            /* The write above was refused, so the fan is wherever it already was. Saying
             * "set to N" here would assert the outcome the stderr line just denied. */
            printf("\ncool-max: zone%d returned to the kernel, but the fan could NOT be "
                   "set to state %d - it is left as it was, and the kernel will not "
                   "change it until the zone next changes trip state.\n",
                   fo.zone_idx, want_state);
        } else {
            printf("\ncool-max: zone%d returned to the kernel, fan set to state %d - "
                   "what the kernel itself holds at %ld.%ld C (trip %ld.%ld C), so "
                   "nothing is left for it to walk back.\n",
                   fo.zone_idx, want_state, temp_mc / 1000, (temp_mc % 1000) / 100,
                   trip_mc / 1000, (trip_mc % 1000) / 100);
        }
    }
    return 0;
}

/* JSON for consumers. The GUI must not scrape the human output: "  0   maxperf   12
 * settings" is a layout, not an interface, and changing a column width would break it.
 *
 * Unknown values are JSON null, never 0 or "" - the same rule the daemon's status snapshot
 * follows. A boot value that could not be read is genuinely unknown and a consumer has to be
 * able to tell that from a real reading. */
static void json_str(const char *s)
{
    putchar('"');
    for (; s && *s; s++) {
        if (*s == '"' || *s == '\\')
            printf("\\%c", *s);
        else if ((unsigned char)*s < 0x20)
            printf("\\u%04x", (unsigned char)*s);
        else
            putchar(*s);
    }
    putchar('"');
}

/* A knob's boot value as a number, or null if it was unreadable. */
static void json_num_or_null(const char *v)
{
    char *end;
    unsigned long long n;

    if (!v || !*v) { printf("null"); return; }
    n = strtoull(v, &end, 10);
    if (end == v) { printf("null"); return; }
    printf("%llu", n);
}

static void report_json(const struct surface *s)
{
    int off = 0, noff = 0;
    const struct cdev *fan = surface_cdev(s, "emc2305_fan");

    printf("{\"v\":1,\"board\":");
    if (s->model[0]) json_str(s->model); else printf("null");

    printf(",\"policies\":[");
    for (int i = 0; i < s->npol; i++) {
        const struct policy *p = &s->pol[i];
        printf("%s{\"id\":%d,\"cpus\":[", i ? "," : "", p->id);
        for (int c = 0; c < p->ncpus; c++)
            printf("%s%d", c ? "," : "", p->cpus[c]);
        printf("],\"governor\":");
        if (p->k_governor.boot[0]) json_str(p->k_governor.boot); else printf("null");
        printf(",\"min_khz\":"); json_num_or_null(p->k_min.boot);
        printf(",\"max_khz\":"); json_num_or_null(p->k_max.boot);
        printf(",\"governors\":[");
        for (int g = 0; g < p->ngov; g++) {
            if (g) putchar(',');
            json_str(p->governors[g]);
        }
        printf("]}");
    }
    printf("]");

    printf(",\"gpu\":");
    if (!s->gpu.present) {
        printf("null");
    } else {
        printf("{\"governor\":");
        if (s->gpu.k_governor.boot[0]) json_str(s->gpu.k_governor.boot); else printf("null");
        printf(",\"min_hz\":%llu,\"max_hz\":%llu}",
               (unsigned long long)s->gpu.min_hz, (unsigned long long)s->gpu.max_hz);
    }

    printf(",\"fan\":");
    if (fan && fan->state_ok)
        printf("{\"state\":%llu,\"max_state\":%llu}",
               (unsigned long long)fan->cur_state, (unsigned long long)fan->max_state);
    else
        printf("null");

    for (int i = 0; i < s->ncpu; i++)
        if (s->cpu[i].present) { noff++; if (!s->cpu[i].online) off++; }
    printf(",\"cpus_present\":%d,\"cpus_offline\":%d", noff, off);
    printf(",\"zones\":%d,\"cooling_devices\":%d}\n", s->nzone, s->ncdev);
    /* NO "env" MEMBER HERE, deliberately. Reporting which kernel and image the board runs
     * is OBSERVATION, and observation is qcstats'. -q is a surface report: the moment it
     * grows an environment field, this tool is publishing telemetry that a consumer will
     * then read from here instead of from the observer that owns it. envid is used in this
     * repo only to stamp artifacts WE author and read back - see envid.h. */
}

static void list_json(const struct pconfig *c)
{
    printf("{\"v\":1,\"profiles\":[");
    for (int i = 0; i < c->nprof; i++) {
        printf("%s{\"id\":%d,\"name\":", i ? "," : "", c->prof[i].id);
        json_str(c->prof[i].name);
        printf(",\"default\":%s", c->prof[i].is_default ? "true" : "false");
        printf(",\"settings\":%d}", c->prof[i].nact);
    }
    printf("]}\n");
}

static void report(const struct surface *s)
{
    printf("board: %s\n", s->model[0] ? s->model : "<unknown>");
    for (int i = 0; i < s->npol; i++) {
        const struct policy *p = &s->pol[i];
        printf("  policy%-2d cpus=", p->id);
        for (int c = 0; c < p->ncpus; c++)
            printf("%s%d", c ? "," : "", p->cpus[c]);
        printf("  %-12s %llu - %llu kHz\n",
               p->k_governor.boot[0] ? p->k_governor.boot : "?",
               (unsigned long long)strtoull(p->k_min.boot, NULL, 10),
               (unsigned long long)strtoull(p->k_max.boot, NULL, 10));
    }
    if (s->gpu.present)
        printf("  gpu      %-12s %llu - %llu Hz\n",
               s->gpu.k_governor.boot[0] ? s->gpu.k_governor.boot : "?",
               (unsigned long long)s->gpu.min_hz,
               (unsigned long long)s->gpu.max_hz);
    {
        const struct cdev *fan = surface_cdev(s, "emc2305_fan");
        if (fan && fan->state_ok)
            printf("  fan      state %llu/%llu\n",
                   (unsigned long long)fan->cur_state,
                   (unsigned long long)fan->max_state);
    }
    {
        int off = 0;
        for (int i = 0; i < s->ncpu; i++)
            if (s->cpu[i].present && !s->cpu[i].online)
                off++;
        if (off)
            printf("  cpus offline: %d\n", off);
    }
}

int main(int argc, char **argv)
{
    const char *conf = DEFAULT_CONF, *statef = DEFAULT_STATE, *mode = NULL;
    int do_query = 0, do_list = 0, do_reset = 0, do_save = 0, do_verify = 0, do_reapply = 0;
    const char *activef = DEFAULT_ACTIVE;
    const char *selectedf = DEFAULT_SELECTED;
    int dry = 0, allow_disrupt = 0, do_coolmax = 0, as_json = 0, force = 0;
    const char *fanbase_act = NULL, *fanbasef = FANBASE_PATH;
    const char *statusf = COOLMAX_STATUS;
    const char *ctlsock = CONTROL_SOCK;
    struct surface s;
    struct pconfig c;
    char err[ERRLEN];
    // --reapply points `mode` at this, so it must outlive the block that fills it.
    char active_name[NAME_LEN];

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help"))            { usage(); return 0; }
        else if (!strcmp(a, "-V") || !strcmp(a, "--version"))    { version(); return 0; }
        else if (!strcmp(a, "-q") || !strcmp(a, "--query"))        do_query = 1;
        else if (!strcmp(a, "-l") || !strcmp(a, "--list"))         do_list = 1;
        else if (!strcmp(a, "--dry-run"))                          dry = 1;
        else if (!strcmp(a, "--reset"))                            do_reset = 1;
        else if (!strcmp(a, "--save-state"))                       do_save = 1;
        else if (!strcmp(a, "--verify"))                           do_verify = 1;
        else if (!strcmp(a, "--reapply"))                          do_reapply = 1;
        else if (!strcmp(a, "--active-file")) {
            if (++i >= argc) { fprintf(stderr, "%s needs a path\n", a); return 2; }
            activef = argv[i];
        }
        else if (!strcmp(a, "--selection-file")) {
            if (++i >= argc) { fprintf(stderr, "%s needs a path\n", a); return 2; }
            selectedf = argv[i];
        }
        else if (!strcmp(a, "--allow-disruptive"))                 allow_disrupt = 1;
        else if (!strcmp(a, "--json"))                             as_json = 1;
        else if (!strcmp(a, "--force"))                            force = 1;
        else if (!strcmp(a, "--fan-baseline")) {
            if (++i >= argc) { fprintf(stderr, "%s needs capture|show\n", a); return 2; }
            fanbase_act = argv[i];
        }
        else if (!strcmp(a, "--fan-baseline-file")) {
            if (++i >= argc) { fprintf(stderr, "%s needs a path\n", a); return 2; }
            fanbasef = argv[i];
        }
        else if (!strcmp(a, "--cool-max"))                         do_coolmax = 1;
        else if (!strcmp(a, "--control-socket")) {
            if (++i >= argc) { fprintf(stderr, "%s needs a path\n", a); return 2; }
            ctlsock = argv[i];
        }
        else if (!strcmp(a, "--status-file")) {
            if (++i >= argc) { fprintf(stderr, "%s needs a path\n", a); return 2; }
            statusf = argv[i];
        }
        else if (!strcmp(a, "-m") || !strcmp(a, "--mode")) {
            if (++i >= argc) { fprintf(stderr, "%s needs a profile\n", a); return 2; }
            mode = argv[i];
        } else if (!strcmp(a, "-c") || !strcmp(a, "--config")) {
            if (++i >= argc) { fprintf(stderr, "%s needs a path\n", a); return 2; }
            conf = argv[i];
        } else if (!strcmp(a, "--state")) {
            if (++i >= argc) { fprintf(stderr, "%s needs a path\n", a); return 2; }
            statef = argv[i];
        } else {
            fprintf(stderr, "unknown option '%s' (try --help)\n", a);
            return 2;
        }
    }

    if (do_coolmax && (mode || do_reset || do_save || do_reapply)) {
        fprintf(stderr, "--cool-max cannot be combined with -m, --reset, --save-state "
                        "or --reapply\n");
        return 2;
    }

    io_set_sysroot(getenv(ENV_SYSROOT));
    surface_discover(&s);

    if (fanbase_act) {
        if (!strcmp(fanbase_act, "show")) {
            struct fanbase fb;
            if (fanbase_read(fanbasef, &fb, err, sizeof err) < 0) {
                fprintf(stderr, "fan-baseline: %s\n", err);
                return 1;
            }
            printf("captured %s\n", fb.captured);
            printf("board    %s\n", fb.board);
            printf("cdev     %s (max_state %d)\n", fb.cdev, fb.max_state);
            {
                char desc[160], what[160];
                struct envid live;
                envid_describe(&fb.env, desc, sizeof desc);
                printf("captured on %s\n", desc);
                envid_read(&live);
                if (envid_differs(&fb.env, &live, what, sizeof what)) {
                    /* A warning, never a refusal. Zone and hwmon indices are assigned at
                     * probe time, so the rpm figures may no longer describe what gets
                     * sampled - but a detector that disables itself on a newer kernel is
                     * strictly worse than one that reports a caveat. */
                    printf("\n");
                    printf("WARNING: the environment has changed since capture: %s\n", what);
                    printf("  These numbers were measured elsewhere. Judgements against them\n");
                    printf("  may be wrong in a consistent direction, which is the hardest\n");
                    printf("  kind to notice. Re-capture with: qcctl --fan-baseline capture\n");
                }
            }
            for (int st = 0; st <= FANBASE_MAX_STATE; st++)
                if (fb.have[st])
                    printf("  state %-2d %5ld rpm  (%ld-%ld, n=%d)\n",
                           st, fb.mean[st], fb.lo[st], fb.hi[st], fb.n[st]);
            return 0;
        }
        if (strcmp(fanbase_act, "capture")) {
            fprintf(stderr, "--fan-baseline takes 'capture' or 'show', not '%s'\n",
                    fanbase_act);
            return 2;
        }
        if (needs_root("--fan-baseline capture"))
            return 1;
        return fan_baseline_capture(&s, statusf, fanbasef, force);
    }

    if (do_coolmax) {
        int rc;
        /* Try the daemon FIRST. --cool-max may be a split-second decision to save
         * hardware, so it must not demand a password; the daemon is already root and is
         * the only thing allowed to touch the zone. Doing it ourselves is the fallback
         * for when no daemon is running, and only that path needs privilege. */
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = coolmax_sig;
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
        sigaction(SIGHUP, &sa, NULL);

        rc = cool_max_via_daemon(ctlsock);
        if (rc >= 0)
            return rc;
        if (geteuid() != 0) {
            const char *sr = getenv(ENV_SYSROOT);
            if (!sr || !*sr) {
                fprintf(stderr, "cool-max: no qcctld to ask at %s, and doing it\n",
                        ctlsock);
                fprintf(stderr, "cool-max: directly needs root. Either start the daemon\n");
                fprintf(stderr, "cool-max:   sudo systemctl start qcctld\n");
                fprintf(stderr, "cool-max: or run this as root\n");
                fprintf(stderr, "cool-max:   sudo qcctl --cool-max\n");
                return 1;
            }
        }
        return cool_max(&s, statusf);
    }

    /* --reapply: re-assert the profile the daemon recorded. Absent file means no
     * profile is in force, which is a normal state and not an error - the hook runs on
     * every resume whether or not the daemon is up. */
    if (do_reapply) {
        if (read_file_str(activef, active_name, sizeof active_name) < 0 || !active_name[0]) {
            printf("no active profile recorded in %s; nothing to re-apply\n", activef);
            return 0;
        }
        mode = active_name;
        printf("re-applying the recorded profile '%s'\n", active_name);
    }

    if (!do_query && !do_list && !do_reset && !do_save && !do_verify && !mode) {
        report(&s);
        printf("\nnothing to do; --help for options\n");
        return 0;
    }
    if (do_query) {
        if (as_json) report_json(&s); else report(&s);
        return 0;
    }
    if (do_reset) {
        if (needs_root("--reset")) return 1;
        int n = 0;
        if (state_restore(statef, err, sizeof err, &n) < 0) {
            fprintf(stderr, "reset: %s\n", err);
            return 1;
        }
        printf("restored %d values from %s\n", n, statef);
        return 0;
    }
    if (do_save) {
        if (needs_root("--save-state")) return 1;
        if (state_save(&s, statef, err, sizeof err) < 0) {
            fprintf(stderr, "save-state: %s\n", err);
            return 1;
        }
        printf("state saved to %s\n", statef);
        return 0;
    }

    if (config_parse_file(conf, &c) < 0) {
        fprintf(stderr, "%s:%d: %s\n", conf, c.err_line, c.err);
        return 3;
    }
    {
        const struct action *disr[32];
        size_t ndisr = 0;
        if (config_validate(&c, &s, err, sizeof err, disr, 32, &ndisr) < 0) {
            fprintf(stderr, "%s: %s\n", conf, err);
            return 3;
        }
        if (do_verify) {
            /* Validation checks the CONFIG. It cannot see that an attribute a profile
             * writes has gone from this kernel - only planning resolves paths. --verify
             * used to stop here and say "validated OK" where a real apply would fail, which
             * is exactly the reassurance a new board image would give falsely. */
            {
                int bad = 0;
                for (int i = 0; i < c.nprof; i++) {
                    struct applyctx vc;
                    char verr[ERRLEN];
                    memset(&vc, 0, sizeof vc);
                    vc.dry = 1;
                    if (apply_plan(&c.prof[i], &s, &vc, verr, sizeof verr) < 0) {
                        fprintf(stderr, "%s: profile '%s' cannot be applied: %s\n",
                                conf, c.prof[i].name, verr);
                        bad++;
                    }
                }
                if (bad) {
                    fprintf(stderr, "%s: %d of %d profiles would fail on this board\n",
                            conf, bad, c.nprof);
                    return 4;   /* distinct from 3: nothing is wrong with the FILE */
                }
            }
            /* No DEFAULT is the one way a well-formed config can still leave the daemon
             * unable to start: the unit carries no -m, so there is nothing to apply. It
             * used to pass here and fail at the next restart, which is the worst place to
             * find out - a user who edits the file, verifies it, and walks away has been
             * told the opposite of the truth. */
            if (!config_default(&c)) {
                fprintf(stderr, "%s: no profile is marked DEFAULT\n", conf);
                fprintf(stderr, "  The file is well-formed, but the daemon starts with no "
                                "-m and so has nothing to apply:\n");
                fprintf(stderr, "  it would refuse to start. Add DEFAULT to one profile's "
                                "< PROFILE ... > line.\n");
                return 3;
            }
            printf("%s: %d profiles, validated and all applicable on this board\n",
                   conf, c.nprof);
            for (size_t i = 0; i < ndisr; i++) {
                char d[160];
                printf("  disruptive: line %d: %s\n", disr[i]->line,
                       act_describe(disr[i], d, sizeof d));
            }
            return 0;
        }
        if (do_list) {
            if (as_json) {
                list_json(&c);
            } else {
                for (int i = 0; i < c.nprof; i++)
                    printf("%-3d %-16s %s%d setting%s\n", c.prof[i].id, c.prof[i].name,
                           c.prof[i].is_default ? "[default] " : "",
                           c.prof[i].nact, c.prof[i].nact == 1 ? "" : "s");
            }
            return 0;
        }

        /* --- apply --- */
        const struct profile *p = config_find(&c, mode);
        struct applyctx ctx;
        char d[160];

        if (!p) {
            fprintf(stderr, "no profile '%s' in %s\n", mode, conf);
            return 3;
        }
        /* A disruptive setting is refused by default rather than attempted and
         * hoped for. The flag makes accepting that risk a deliberate act. */
        if (ndisr && !allow_disrupt) {
            fprintf(stderr, "profile '%s' needs --allow-disruptive:\n", p->name);
            for (size_t i = 0; i < ndisr; i++)
                fprintf(stderr, "  line %d: %s\n", disr[i]->line,
                        act_describe(disr[i], d, sizeof d));
            return 4;
        }

        /* --dry-run performs no writes, so it stays usable unprivileged. A real apply
         * refused early beats one that fails partway: apply_commit rolls back, but the
         * rollback writes too, so a non-root failure mid-apply has no way to undo
         * itself. */
        if (!dry && needs_root(do_reapply ? "--reapply" : "-m"))
            return 1;

        memset(&ctx, 0, sizeof ctx);
        ctx.dry = dry;
        if (apply_plan(p, &s, &ctx, err, sizeof err) < 0) {
            fprintf(stderr, "plan: %s\n", err);
            return 1;
        }
        printf("%s profile %d '%s' -- %d write%s\n",
               dry ? "would apply" : "applying", p->id, p->name,
               ctx.nw, ctx.nw == 1 ? "" : "s");
        for (int i = 0; i < ctx.nw; i++)
            printf("    %s = %s%s\n", ctx.w[i].path, ctx.w[i].value,
                   ctx.w[i].act ? "" : "   (staged: keeps min <= max valid)");

        if (apply_commit(&ctx, err, sizeof err) < 0) {
            fprintf(stderr, "apply: %s\n", err);
            return 1;
        }

        /* Revert what the PREVIOUS profile owned and this one does not mention, so a switch
         * yields boot+new rather than depending on which profiles came before.
         *
         * AFTER apply_commit, not before. Before would break the whole-or-nothing
         * guarantee: apply_commit rolls back its own writes on failure, but it knows
         * nothing about these reverts, so a failed apply would leave the old profile's
         * orphans at boot values and the new profile rolled back - a state no config
         * describes. Doing it after means a failed apply leaves the old profile intact.
         *
         * The previous name comes from the active file, which is still the OLD name at this
         * point; it is rewritten below. */
        {
            char prevname[NAME_LEN] = "";
            int nrev = 0;

            if (read_file_str(activef, prevname, sizeof prevname) < 0)
                prevname[0] = '\0';
            if (prevname[0] && strcmp(prevname, p->name)) {
                if (apply_orphans(&c, prevname, &ctx, &s, statef, dry,
                                  err, sizeof err, &nrev) < 0)
                    fprintf(stderr, "warning: could not revert '%s' settings: %s\n",
                            prevname, err);
                else if (nrev)
                    printf("reverted %d setting%s owned by '%s' and not by '%s'\n",
                           nrev, nrev == 1 ? "" : "s", prevname, p->name);
            }
        }
        if (!dry) {
            /* Read the plan back. A successful write only means the handler took the bytes;
             * the operator is the one who needs told when the kernel kept something else. */
            struct verifyreport vr;

            if (apply_verify(&ctx, &vr) < 0) {
                fprintf(stderr, "warning: none of the %d applied setting%s could be read "
                        "back, so nothing was verified\n",
                        ctx.nw, ctx.nw == 1 ? "" : "s");
            } else {
                if (vr.diverged) {
                    printf("NOTE: %d of %d setting%s read back differently - the kernel "
                           "accepted the write and kept another value.\n",
                           vr.diverged, vr.checked, vr.checked == 1 ? "" : "s");
                    printf("      first: %s\n            asked %s, holding %s\n",
                           vr.path, vr.want, vr.got);
                } else if (vr.checked) {
                    /* Said positively on purpose: a check whose success looks exactly
                     * like not having run is not evidence that anything was checked. */
                    printf("verified %d setting%s read back as asked\n",
                           vr.checked, vr.checked == 1 ? "" : "s");
                }
                /* Only in the else: rc < 0 already said nothing could be read, and two
                 * messages for one condition is how a reader stops believing either. */
                if (vr.unreadable)
                    fprintf(stderr, "warning: %d applied setting%s could not be read "
                            "back\n", vr.unreadable, vr.unreadable == 1 ? "" : "s");
            }

            /* Record what is now in force. This is the ONLY source the daemon will take a
             * profile name from, which is what keeps the unprivileged REBASELINE request
             * unable to choose a profile. We are root here; nothing else writes this. */
            if (write_new_file(activef, p->name) < 0)
                fprintf(stderr, "warning: cannot record the active profile in %s: %s\n",
                        activef, strerror(errno));

            /* And record it as the operator's SELECTION, which outlives the daemon. Without
             * this a restart reverts to DEFAULT and the board runs a profile nobody chose. */
            if (selection_save(selectedf, p->name) < 0)
                fprintf(stderr, "warning: cannot record the selection in %s - a daemon "
                                "restart will fall back to DEFAULT\n", selectedf);

            /* Nudge the daemon so its published status does not go stale and its drift
             * baseline matches the profile that is now applied. No daemon is fine - this
             * is a courtesy, not a dependency. */
            if (control_tell(ctlsock, CONTROL_REQ_REBASELINE) == 0)
                printf("applied; qcctld notified\n");
            else
                printf("applied\n");
        } else {
            printf("no writes performed\n");
        }
    }
    return 0;
}
