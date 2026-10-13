// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer

#define _GNU_SOURCE
#include "stats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>

int stats_open(struct stats *st, const char *cmd, char *err, size_t errlen)
{
    int pipefd[2];

    memset(st, 0, sizeof *st);
    st->fd = -1;
    st->pid = -1;
    st->v_bad_last = -1;        /* never a misleading "version 0" */

    if (pipe(pipefd) < 0) {
        snprintf(err, errlen, "pipe: %s", strerror(errno));
        return -1;
    }
    st->pid = fork();
    if (st->pid < 0) {
        snprintf(err, errlen, "fork: %s", strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (st->pid == 0) {
        /* Put the child in its OWN process group. /bin/sh forks rather than execs
         * whenever the command is more than one simple command, so the sampler can
         * be a GRANDchild - and killing only our direct child then orphans it to
         * init. */
        setpgid(0, 0);
        /* stdout -> pipe; stderr is left alone so sampler errors stay visible */
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0)
            _exit(127);
        close(pipefd[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);
    /* Set it from the parent as well, so no scheduling order can leave the child
     * outside its own group. EACCES means the child already exec'd, which means it
     * ran setpgid first - equally fine. */
    (void)setpgid(st->pid, st->pid);
    st->fd = pipefd[0];
    return 0;
}

void stats_close(struct stats *st)
{
    if (st->fd >= 0)
        close(st->fd);
    st->fd = -1;
    if (st->pid > 0) {
        /* A NEGATIVE pid signals the whole process group, so a sampler that
         * /bin/sh forked dies with its shell instead of being orphaned. Fall back
         * to the single process if the group is somehow gone. */
        if (kill(-st->pid, SIGTERM) < 0)
            (void)kill(st->pid, SIGTERM);
        /* KEEP the status. It used to be thrown away, and with it the only evidence of
         * WHY the stream ended - 127 from /bin/sh is "command not found", which is the
         * difference between "something went wrong" and "qcstats is not installed". */
        {
            int status = 0;
            if (waitpid(st->pid, &status, 0) == st->pid) {
                st->exit_status = status;
                st->have_status = 1;
            }
        }
        /* Anything still in the group after a grace period gets the hammer. */
        usleep(200000);
        (void)kill(-st->pid, SIGKILL);
    }
    st->pid = -1;
}

const char *stats_exit_hint(const struct stats *st, const char *path)
{
    static char msg[256];

    if (!st || !st->have_status)
        return NULL;
    if (WIFSIGNALED(st->exit_status)) {
        /* Worth saying: an OOM kill looks exactly like this, and on a small board it is
         * a realistic way to lose the sampler. */
        snprintf(msg, sizeof msg, "the sampler was killed by signal %d%s",
                 WTERMSIG(st->exit_status),
                 WTERMSIG(st->exit_status) == 9 ? " (SIGKILL - out of memory?)" : "");
        return msg;
    }
    if (!WIFEXITED(st->exit_status))
        return NULL;
    switch (WEXITSTATUS(st->exit_status)) {
    case 0:
        return NULL;                    /* a clean exit says nothing extra */
    case 127:
        snprintf(msg, sizeof msg,
                 "the sampler command could not be run (exit 127 = not found). Is "
                 "qcstats installed and on root's PATH? It should be at %s", path);
        return msg;
    case 126:
        snprintf(msg, sizeof msg,
                 "the sampler command was found but could not be executed (exit 126 - "
                 "wrong permissions, or built for another architecture). Check %s", path);
        return msg;
    default:
        break;
    }
    /* 128+N IS THE USUAL SHAPE, not the WIFSIGNALED case above. The sampler normally runs
     * as a GRANDchild - /bin/sh forks rather than execs for anything but a bare simple
     * command - so when qcstats is killed it is the SHELL we reap, and a shell reports a
     * child killed by signal N as exit 128+N. An OOM kill on a small board therefore
     * arrives here as 137, and "the sampler exited 137" told a user nothing. */
    {
        int code = WEXITSTATUS(st->exit_status);
        if (code > 128 && code < 128 + 32) {
            int sig = code - 128;
            snprintf(msg, sizeof msg,
                     "the sampler was killed by signal %d%s", sig,
                     sig == 9 ? " (SIGKILL - out of memory is the usual cause on this "
                                "board; check 'dmesg | grep -i oom')" : "");
            return msg;
        }
        snprintf(msg, sizeof msg, "the sampler exited %d", code);
        return msg;
    }
}

/* Skip whitespace, then match a JSON number or the literal null.
 * Returns 1 for a number (value in out), 0 for null, -1 for anything else. */
static int json_number_or_null(const char *p, long *out_milli)
{
    char *end;
    double v;

    while (*p == ' ' || *p == '\t')
        p++;
    if (!strncmp(p, "null", 4))
        return 0;
    errno = 0;
    v = strtod(p, &end);
    if (end == p || errno)
        return -1;
    /* qcstats reports thermal_c in degrees with one decimal; we work in milli-C. */
    *out_milli = (long)(v * 1000.0 + (v >= 0 ? 0.5 : -0.5));
    return 1;
}

/* Deliberately a targeted scan rather than a JSON parser: one field, one object,
 * no allocation, no dependency. It is NOT a general JSON reader and must not be
 * reused as one - it would mis-handle nesting or escapes it never sees here. */
/* Like json_number_or_null but plain, not milli-scaled: duty_pct and rpm are integers
 * in their own units. */
static int json_int_or_null(const char *p, long *out)
{
    char *end;
    long v;

    while (*p == ' ' || *p == '\t')
        p++;
    if (!strncmp(p, "null", 4))
        return 0;
    errno = 0;
    v = strtol(p, &end, 10);
    if (end == p || errno)
        return -1;
    *out = v;
    return 1;
}

const char *stats_last_line(const struct stats *st)
{
    return st->last;
}

bool stats_extract_fan(const char *line, struct fanobs *out)
{
    const char *f, *obj_end, *k;

    memset(out, 0, sizeof *out);
    if (!line)
        return false;
    f = strstr(line, "\"fan\":");
    if (!f)
        return false;                   /* no fan key at all */
    f += 6;
    while (*f == ' ' || *f == '\t')
        f++;
    if (!strncmp(f, "null", 4))
        return true;                    /* present as a key, but null: not measured */
    if (*f != '{')
        return false;
    obj_end = strchr(f, '}');
    if (!obj_end)
        return false;
    out->present = true;

    k = strstr(f, "\"duty_pct\":");
    if (k && k < obj_end)
        out->duty_ok = (json_int_or_null(k + 11, &out->duty_pct) == 1);
    k = strstr(f, "\"rpm\":");
    if (k && k < obj_end)
        out->rpm_ok = (json_int_or_null(k + 6, &out->rpm) == 1);
    k = strstr(f, "\"rpm_valid\":");
    if (k && k < obj_end)
        out->rpm_valid = !strncmp(k + 12, "true", 4);
    return true;
}

long stats_line_version(const char *line)
{
    const char *k;
    long v;

    if (!line || !*line)
        return -1;
    k = strstr(line, "\"v\":");
    if (!k)
        return -1;          /* no v at all: not qcstats output */
    /* json_int_or_null returns 0 for an explicit null. "v":null is as unusable as a
     * missing v, so both land on -1 rather than on a version number. */
    if (json_int_or_null(k + 4, &v) != 1)
        return -1;
    return v;
}

bool stats_extract_cpu(const char *line, struct cpuobs *out)
{
    const char *a, *p, *arr_end;
    int depth;

    if (!out)
        return false;
    memset(out, 0, sizeof *out);
    if (!line)
        return false;

    a = strstr(line, "\"cpu\":");
    if (!a)
        return false;
    a = strchr(a, '[');
    if (!a)
        return false;

    /* Find the ] that closes the cpu array, NOT the one closing a policy's nested
     * cores[]. Counting depth is the only way: every policy object carries one. */
    depth = 0;
    arr_end = NULL;
    for (p = a; *p; p++) {
        if (*p == '[') {
            depth++;
        } else if (*p == ']') {
            if (--depth == 0) {
                arr_end = p;
                break;
            }
        }
    }
    if (!arr_end)
        return false;

    /* One entry per "label". Safe against straying into cores[]: those objects carry
     * "cpu" and "pct" and never "label" or "mhz" - see qcstats' schema v1. */
    p = a;
    for (;;) {
        const char *lb = strstr(p, "\"label\":");
        const char *mz, *q;
        long v;

        if (!lb || lb > arr_end)
            break;
        if (out->n >= STATS_MAX_CPU) {
            out->overflow = 1;      /* say so rather than report a partial list as whole */
            break;
        }
        lb += 8;
        while (*lb == ' ' || *lb == '\t')
            lb++;
        if (*lb != '"')
            break;
        lb++;
        q = strchr(lb, '"');
        if (!q || q > arr_end)
            break;
        {
            size_t len = (size_t)(q - lb);
            if (len >= sizeof out->pol[0].label)
                len = sizeof out->pol[0].label - 1;
            memcpy(out->pol[out->n].label, lb, len);
            out->pol[out->n].label[len] = '\0';
        }
        /* mhz is nullable: a policy that reports none must not read as 0 MHz. */
        mz = strstr(q, "\"mhz\":");
        if (mz && mz < arr_end && json_int_or_null(mz + 6, &v) == 1) {
            out->pol[out->n].mhz_ok = true;
            out->pol[out->n].mhz = v;
        }
        out->n++;
        p = q + 1;
    }
    return out->n > 0;
}

bool stats_extract_temp(const char *line, const char *zone, long *temp_mc)
{
    const char *tc, *obj_end, *k;
    char key[96];
    size_t zl = strlen(zone);
    int n;

    if (!line || !zone || !*zone)
        return false;
    tc = strstr(line, "\"thermal_c\"");
    if (!tc)
        return false;
    tc = strchr(tc, '{');
    if (!tc)
        return false;
    obj_end = strchr(tc, '}');
    if (!obj_end)
        return false;

    /* qcstats keys thermal_c by the SHORT zone name ("cpuss-0-0"), but accept the
     * full type too, since that is what a config may legitimately carry. */
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 0) {
            n = snprintf(key, sizeof key, "\"%s\":", zone);
        } else {
            if (zl > 8 && !strcmp(zone + zl - 8, "-thermal"))
                n = snprintf(key, sizeof key, "\"%.*s\":", (int)(zl - 8), zone);
            else
                n = snprintf(key, sizeof key, "\"%s-thermal\":", zone);
        }
        if (n <= 0 || (size_t)n >= sizeof key)
            continue;
        k = strstr(tc, key);
        if (!k || k > obj_end)
            continue;
        return json_number_or_null(k + n, temp_mc) == 1;
    }
    return false;
}

enum stats_read stats_sample(struct stats *st, int timeout_ms, const char *zone,
                            long *temp_mc, bool *temp_ok)
{
    struct pollfd pfd = { .fd = st->fd, .events = POLLIN };

    *temp_ok = false;
    if (st->fd < 0)
        return STATS_ERROR;

    for (;;) {
        char *nl = memchr(st->buf, '\n', st->len);
        int r;

        if (nl) {
            size_t linelen = (size_t)(nl - st->buf);
            char line[STATS_LINE_MAX];

            memcpy(line, st->buf, linelen);
            line[linelen] = '\0';
            /* consume, keeping any partial next line */
            memmove(st->buf, nl + 1, st->len - linelen - 1);
            st->len -= linelen + 1;

            snprintf(st->last, sizeof st->last, "%s", line);

            /* Version gate. A line this reader does not understand yields NO reading - the
             * same answer a null temperature gives, which the caller already handles by
             * holding rather than acting. Deliberately NOT an error return: the stream is
             * alive and the sampler is healthy, so claiming otherwise would trip recovery
             * and eventually exit, which is a worse outcome than holding. */
            {
                long v = stats_line_version(line);
                if (v != STATS_SCHEMA_V) {
                    st->v_bad++;
                    st->v_bad_last = v;
                    *temp_ok = false;
                    return STATS_OK;
                }
            }

            *temp_ok = stats_extract_temp(line, zone, temp_mc);
            return STATS_OK;
        }
        if (st->len == sizeof st->buf) {
            /* a line longer than the buffer: drop it rather than wedge */
            st->len = 0;
            continue;
        }

        r = poll(&pfd, 1, timeout_ms);
        if (r == 0)
            return STATS_TIMEOUT;
        if (r < 0)
            return (errno == EINTR) ? STATS_TIMEOUT : STATS_ERROR;

        {
            ssize_t got = read(st->fd, st->buf + st->len, sizeof st->buf - st->len);
            if (got == 0)
                return STATS_EOF;
            if (got < 0)
                return (errno == EINTR) ? STATS_TIMEOUT : STATS_ERROR;
            st->len += (size_t)got;
        }
    }
}
