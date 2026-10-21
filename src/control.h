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

/* ONE REQUEST PER WRITE, and one per connection for COOLMAX.
 *
 * Each read is matched as a whole: a buffer carrying two requests has only its first
 * honoured, and a request split across two writes matches neither and is refused. Both are
 * measured behaviours, pinned in tests/t_control.c, not accidents of the parser - and they
 * are left as they are deliberately. Splitting the buffer into lines would mean either
 * discarding a trailing fragment or buffering it across reads, and both change what a
 * 0666 socket accepts; the clients here are this project's own, each of which writes one
 * request and then either waits or exits. A batching client is the case to document, not
 * the case to widen the vocabulary for. */

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
