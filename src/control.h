// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer
//
// The daemon's control socket: an unprivileged process asks root for more cooling.
// Requests can only ever raise cooling, which is why the socket is mode 0666.

#ifndef CONTROL_H
#define CONTROL_H

#include <poll.h>
#include <stddef.h>

#define CONTROL_SOCK   "/run/qcctl/control"
#define CONTROL_MAXCLI 8

// Hold the fan at maximum for as long as the connection stays open.
#define CONTROL_REQ_COOLMAX "COOLMAX\n"
// Re-apply the profile named in the root-owned active file. Carries no argument.
#define CONTROL_REQ_REBASELINE "REBASELINE\n"

struct control {
    int  listen_fd;                     // -1 when not listening
    int  cli[CONTROL_MAXCLI];           // -1 = free slot
    int  holding[CONTROL_MAXCLI];       // this client has asked for cooling
    int  rebaseline;                    // one-shot: a client asked us to re-assert
    char path[256];
};

// Create and listen. 0, or -1 with a reason in err.
int  control_open(struct control *c, const char *path, char *err, size_t errlen);
void control_close(struct control *c);

// Add our fds to a pollfd array. Returns how many were added.
int  control_pollfds(const struct control *c, struct pollfd *out, int max);

// Service whatever poll reported. Returns the number of clients now holding.
int  control_service(struct control *c, const struct pollfd *fds, int nfds);

// Current hold count, servicing nothing.
int  control_holds(const struct control *c);

// Close every holding connection, dropping those holds. Returns how many.
int  control_drop_holds(struct control *c);

#endif
