// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer

#include "envid.h"
#include "io.h"

#include <stdio.h>
#include <string.h>

/* /etc/buildinfo is a KEY=value file - BUILD_ID=<id>, matching the distributed archive
 * name - parsed by key rather than by position: a line added ahead of BUILD_ID later must
 * not shift the answer.
 *
 * NOT read_file_str(): that is deliberately a SINGLE-line reader (one fgets), so a bare
 * buildinfo whose first line is something else would report BUILD_ID absent. That failure
 * is easy to ship with a comment claiming key-parsing, which is why this loop is explicit
 * about it. */
static int read_build_id(char *out, size_t len)
{
    char jp[PATH_MAXLEN], line[512];
    FILE *f;

    spath(jp, sizeof jp, "/etc/buildinfo");
    f = fopen(jp, "r");
    if (!f)
        return -1;

    while (fgets(line, sizeof line, f)) {
        char *p = line;
        line[strcspn(line, "\r\n")] = '\0';
        while (*p == ' ' || *p == '\t')
            p++;
        if (strncmp(p, "BUILD_ID=", 9))
            continue;
        snprintf(out, len, "%s", p + 9);
        fclose(f);
        return out[0] ? 0 : -1;
    }
    fclose(f);
    return -1;
}

void envid_read(struct envid *e)
{
    memset(e, 0, sizeof *e);
    e->have_build_id = (read_build_id(e->build_id, sizeof e->build_id) == 0);
    {
        char jp[PATH_MAXLEN];
        spath(jp, sizeof jp, "/proc/sys/kernel/osrelease");
        e->have_kernel = (read_file_str(jp, e->kernel, sizeof e->kernel) == 0 && e->kernel[0]);
    }
    if (!e->have_build_id) e->build_id[0] = '\0';
    if (!e->have_kernel)   e->kernel[0]   = '\0';
}

void envid_describe(const struct envid *e, char *buf, size_t len)
{
    snprintf(buf, len, "kernel %s, image %s",
             e->have_kernel   ? e->kernel   : "(not recorded)",
             e->have_build_id ? e->build_id : "(not recorded)");
}

int envid_differs(const struct envid *recorded, const struct envid *live,
                  char *what, size_t len)
{
    /* Only compare what both sides actually have. A baseline captured before this field
     * existed records nothing, and "nothing" is not evidence that the kernel changed. */
    if (recorded->have_kernel && live->have_kernel &&
        strcmp(recorded->kernel, live->kernel)) {
        if (what) snprintf(what, len, "kernel %s -> %s", recorded->kernel, live->kernel);
        return 1;
    }
    if (recorded->have_build_id && live->have_build_id &&
        strcmp(recorded->build_id, live->build_id)) {
        if (what) snprintf(what, len, "image %s -> %s", recorded->build_id, live->build_id);
        return 1;
    }
    return 0;
}
