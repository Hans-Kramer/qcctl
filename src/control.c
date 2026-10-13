// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer

#define _GNU_SOURCE
#include "control.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

int control_open(struct control *c, const char *path, char *err, size_t errlen)
{
    struct sockaddr_un a;
    int fd;

    memset(c, 0, sizeof *c);
    c->listen_fd = -1;
    for (int i = 0; i < CONTROL_MAXCLI; i++)
        c->cli[i] = -1;
    snprintf(c->path, sizeof c->path, "%s", path);

    if (strlen(path) >= sizeof a.sun_path) {
        snprintf(err, errlen, "control socket path too long: %s", path);
        return -1;
    }
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        snprintf(err, errlen, "socket: %s", strerror(errno));
        return -1;
    }
    /* A leftover socket from a previous run would make bind() fail with EADDRINUSE. The
     * pidfile is what prevents two daemons, so by here we are entitled to this path. */
    unlink(path);
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        snprintf(err, errlen, "bind %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    /* 0666: see the header. The only thing a caller can ask for is more cooling, so there
     * is nothing here that permissions would protect. Set explicitly rather than left to
     * umask, so the behaviour does not depend on how the daemon was started. */
    if (chmod(path, 0666) < 0) {
        snprintf(err, errlen, "chmod %s: %s", path, strerror(errno));
        close(fd);
        unlink(path);
        return -1;
    }
    if (listen(fd, CONTROL_MAXCLI) < 0) {
        snprintf(err, errlen, "listen %s: %s", path, strerror(errno));
        close(fd);
        unlink(path);
        return -1;
    }
    c->listen_fd = fd;
    return 0;
}

void control_close(struct control *c)
{
    for (int i = 0; i < CONTROL_MAXCLI; i++) {
        if (c->cli[i] >= 0)
            close(c->cli[i]);
        c->cli[i] = -1;
        c->holding[i] = 0;
    }
    if (c->listen_fd >= 0) {
        close(c->listen_fd);
        c->listen_fd = -1;
    }
    if (c->path[0])
        unlink(c->path);
}

int control_pollfds(const struct control *c, struct pollfd *out, int max)
{
    int n = 0;

    if (c->listen_fd >= 0 && n < max) {
        out[n].fd = c->listen_fd;
        out[n].events = POLLIN;
        out[n].revents = 0;
        n++;
    }
    for (int i = 0; i < CONTROL_MAXCLI && n < max; i++) {
        if (c->cli[i] < 0)
            continue;
        out[n].fd = c->cli[i];
        /* POLLIN catches both a request and EOF; POLLHUP catches a client that died
         * without sending anything. Either way the hold must go. */
        out[n].events = POLLIN;
        out[n].revents = 0;
        n++;
    }
    return n;
}

int control_holds(const struct control *c)
{
    int n = 0;
    for (int i = 0; i < CONTROL_MAXCLI; i++)
        if (c->cli[i] >= 0 && c->holding[i])
            n++;
    return n;
}

static int revents_for(const struct pollfd *fds, int nfds, int fd)
{
    for (int i = 0; i < nfds; i++)
        if (fds[i].fd == fd)
            return fds[i].revents;
    return 0;
}

static void drop(struct control *c, int slot)
{
    if (c->cli[slot] >= 0)
        close(c->cli[slot]);
    c->cli[slot] = -1;
    c->holding[slot] = 0;
}

int control_drop_holds(struct control *c)
{
    int n = 0;

    for (int i = 0; i < CONTROL_MAXCLI; i++)
        if (c->cli[i] >= 0 && c->holding[i]) {
            drop(c, i);
            n++;
        }
    return n;
}

int control_service(struct control *c, const struct pollfd *fds, int nfds)
{
    /* Accept first, so a new request is seen in the same pass. */
    if (c->listen_fd >= 0 && (revents_for(fds, nfds, c->listen_fd) & POLLIN)) {
        for (;;) {
            int fd = accept4(c->listen_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
            int slot = -1;
            if (fd < 0)
                break;
            for (int i = 0; i < CONTROL_MAXCLI; i++)
                if (c->cli[i] < 0) { slot = i; break; }
            if (slot < 0) {
                close(fd);              /* at capacity; refuse quietly */
                continue;
            }
            c->cli[slot] = fd;
            c->holding[slot] = 0;
        }
    }

    for (int i = 0; i < CONTROL_MAXCLI; i++) {
        int re, n;
        char buf[64];

        if (c->cli[i] < 0)
            continue;
        re = revents_for(fds, nfds, c->cli[i]);
        if (!re)
            continue;
        if (re & (POLLHUP | POLLERR | POLLNVAL)) {
            drop(c, i);                 /* client gone: its hold goes with it */
            continue;
        }
        if (!(re & POLLIN))
            continue;
        n = (int)read(c->cli[i], buf, sizeof buf - 1);
        if (n <= 0) {
            drop(c, i);                 /* EOF - the client closed or died */
            continue;
        }
        buf[n] = '\0';
        if (!strncmp(buf, CONTROL_REQ_COOLMAX, strlen(CONTROL_REQ_COOLMAX)) ||
            !strcmp(buf, "COOLMAX")) {
            c->holding[i] = 1;
            (void)!write(c->cli[i], "OK\n", 3);
        } else if (!strncmp(buf, CONTROL_REQ_REBASELINE,
                            strlen(CONTROL_REQ_REBASELINE)) ||
                   !strcmp(buf, "REBASELINE")) {
            /* One-shot, not a hold: the caller does not stay connected for it. */
            c->rebaseline = 1;
            (void)!write(c->cli[i], "OK\n", 3);
        } else {
            /* Anything else is refused. There is deliberately no vocabulary for lowering
             * cooling or touching the zone: unprivileged callers may ask for more, never
             * less. */
            (void)!write(c->cli[i], "ERR unknown request\n", 20);
            drop(c, i);
        }
    }
    return control_holds(c);
}
