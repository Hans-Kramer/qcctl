// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer
//
// WHICH IMAGE AND KERNEL A MEASUREMENT CAME FROM.

#ifndef ENVID_H
#define ENVID_H

#include <stddef.h>

struct envid {
    char build_id[64];      // BUILD_ID from /etc/buildinfo, e.g. "YYYYMMDD-xxx"
    char kernel[80];        // /proc/sys/kernel/osrelease
    int  have_build_id;
    int  have_kernel;
};

// Never fails: an unreadable source leaves that field absent.
void envid_read(struct envid *e);

// Human-readable one-liner for a file header or a status line. Always writes something.
void envid_describe(const struct envid *e, char *buf, size_t len);

// Does the recorded environment differ from the live one? Only fields present on BOTH sides
// are compared - an older artifact that recorded nothing is not evidence of a change.
// Returns 1 when a present-on-both field differs, 0 otherwise, and on 1 fills `what` with
// what moved. `what` may be NULL.
int envid_differs(const struct envid *recorded, const struct envid *live,
                  char *what, size_t len);

#endif
