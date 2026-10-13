// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer
//
// Derived from qcstats src/io.c

#define _GNU_SOURCE
#include "io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>

const char *sysroot = "";

struct ment { char path[256]; int mode; char owner[32]; };
static struct ment *manifest;
static size_t nmanifest;

static void manifest_load(void)
{
    char path[PATH_MAXLEN], line[512];
    size_t cap = 0;
    FILE *f;

    free(manifest);
    manifest = NULL;
    nmanifest = 0;
    if (!*sysroot)
        return;                 /* live system: real modes are on the real files */

    snprintf(path, sizeof path, "%s/.manifest", sysroot);
    f = fopen(path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        char mode[16], owner[32], p[256];
        if (sscanf(line, "%15s %31s %255s", mode, owner, p) != 3)
            continue;
        if (nmanifest == cap) {
            size_t want = cap ? cap * 2 : 256;
            struct ment *grow = realloc(manifest, want * sizeof *grow);
            if (!grow)
                break;
            manifest = grow;
            cap = want;
        }
        manifest[nmanifest].mode = (int)strtol(mode, NULL, 8);
        snprintf(manifest[nmanifest].owner, sizeof manifest[0].owner, "%s", owner);
        snprintf(manifest[nmanifest].path, sizeof manifest[0].path, "%s", p);
        nmanifest++;
    }
    fclose(f);
}

void io_set_sysroot(const char *dir)
{
    sysroot = dir ? dir : "";
    manifest_load();
}

/* Strip the sysroot prefix so a joined path can be matched against the manifest,
 * which records board-absolute paths. */
static const char *unprefix(const char *p)
{
    size_t n = strlen(sysroot);
    if (n && strncmp(p, sysroot, n) == 0)
        return p + n;
    return p;
}

static const struct ment *manifest_find(const char *joined)
{
    const char *want = unprefix(joined);

    for (size_t i = 0; i < nmanifest; i++)
        if (strcmp(manifest[i].path, want) == 0)
            return &manifest[i];
    return NULL;
}

void spath(char *out, size_t len, const char *fmt, ...)
{
    va_list ap;
    size_t n = strlen(sysroot);

    if (n >= len)
        n = 0;                  /* pathological sysroot; fall back to bare path */
    else
        memcpy(out, sysroot, n);
    va_start(ap, fmt);
    vsnprintf(out + n, len - n, fmt, ap);
    va_end(ap);
}

int read_file_str(const char *path, char *buf, size_t len)
{
    FILE *f = fopen(path, "r");

    if (!f)
        return -1;
    if (!fgets(buf, (int)len, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    buf[strcspn(buf, "\n")] = '\0';
    return 0;
}

int read_file_u64(const char *path, uint64_t *out)
{
    char buf[64], *end;
    uint64_t v;

    if (read_file_str(path, buf, sizeof buf) < 0)
        return -1;
    errno = 0;
    v = strtoull(buf, &end, 10);
    /* Reject junk and a leading '-': strtoull silently wraps a negative, which
     * turned an invalid --count into a huge positive in the sibling project. */
    if (errno || end == buf || buf[0] == '-')
        return -1;
    *out = v;
    return 0;
}

/* dir is already sysroot-prefixed. Refuse a truncated join rather than open a
 * path that silently names something else. */
static int join(char *out, size_t len, const char *dir, const char *attr)
{
    int n = snprintf(out, len, "%s/%s", dir, attr);
    return (n > 0 && (size_t)n < len) ? 0 : -1;
}

int read_attr_str(const char *dir, const char *attr, char *buf, size_t len)
{
    char path[PATH_MAXLEN];
    return join(path, sizeof path, dir, attr) < 0
         ? -1 : read_file_str(path, buf, len);
}

int read_attr_u64(const char *dir, const char *attr, uint64_t *out)
{
    char path[PATH_MAXLEN];
    return join(path, sizeof path, dir, attr) < 0
         ? -1 : read_file_u64(path, out);
}

int path_exists(const char *dir, const char *attr)
{
    char path[PATH_MAXLEN];
    return join(path, sizeof path, dir, attr) == 0 && access(path, F_OK) == 0;
}

int path_present(const char *path)
{
    return access(path, F_OK) == 0;
}

int path_mode(const char *dir, const char *attr)
{
    char path[PATH_MAXLEN];
    const struct ment *m;
    struct stat st;

    if (join(path, sizeof path, dir, attr) < 0)
        return -1;
    m = manifest_find(path);
    if (m)
        return m->mode;                  /* fixture: the board's mode, not ours */
    if (stat(path, &st) == 0)
        return (int)(st.st_mode & 07777);
    return -1;
}

int path_owner(const char *dir, const char *attr, char *out, size_t len)
{
    char path[PATH_MAXLEN];
    const struct ment *m;

    if (len)
        out[0] = '\0';
    if (join(path, sizeof path, dir, attr) < 0)
        return -1;
    m = manifest_find(path);
    if (!m)
        return -1;
    snprintf(out, len, "%s", m->owner);
    return 0;
}

int hwmon_find(const char *want, char *out, size_t len)
{
    char path[PATH_MAXLEN], name[64];
    struct dirent *e;
    DIR *d;
    int rc = -1;

    spath(path, sizeof path, HWMON_DIR);
    d = opendir(path);
    if (!d)
        return -1;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "hwmon", 5) != 0)
            continue;
        spath(path, sizeof path, HWMON_DIR "/%.32s/name", e->d_name);
        if (read_file_str(path, name, sizeof name) < 0)
            continue;
        if (strcmp(name, want) == 0) {
            spath(out, len, HWMON_DIR "/%.32s", e->d_name);
            rc = 0;
            break;
        }
    }
    closedir(d);
    return rc;
}

// The only write in the codebase.
int write_new_file(const char *path, const char *value)
{
    char buf[96];
    size_t len;
    ssize_t n;
    int fd;

    n = snprintf(buf, sizeof buf, "%s\n", value);
    if (n <= 0 || (size_t)n >= sizeof buf)
        return -1;
    len = (size_t)n;

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;
    n = write(fd, buf, len);
    if (close(fd) < 0 && n >= 0)
        return -1;
    return (n == (ssize_t)len) ? 0 : -1;
}

int write_path_str(const char *path, const char *value)
{
    /* Longest value we ever write is a frequency in Hz plus a newline. io.c is
     * the bottom layer and must not reach up into surface.h for NAME_LEN. */
    char buf[96];
    size_t len;
    ssize_t n;
    int fd;

    /* Trailing newline, exactly as `echo value > attr` produces. sysfs strips it,
     * and it makes a restored value byte-identical to what was read - which is
     * what lets the round-trip test compare whole trees rather than fields. */
    n = snprintf(buf, sizeof buf, "%s\n", value);
    if (n <= 0 || (size_t)n >= sizeof buf)
        return -1;
    len = (size_t)n;

    fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd < 0)
        return -1;
    /* One write() of the whole value: some sysfs attributes reject a split. */
    n = write(fd, buf, len);
    if (close(fd) < 0 && n >= 0)
        return -1;
    return (n == (ssize_t)len) ? 0 : -1;
}

static int cmp_int(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

/* readdir order is arbitrary; policy10 must not sort before policy2, and a
 * caller that prints the surface needs a stable order to diff against. */
void scan_indexed(const char *base, const char *prefix, scan_fn fn, void *ctx)
{
    char path[PATH_MAXLEN];
    struct dirent *e;
    DIR *d;
    int idx[256];
    size_t n = 0;
    size_t plen = strlen(prefix);

    spath(path, sizeof path, "%s", base);
    d = opendir(path);
    if (!d)
        return;
    while ((e = readdir(d)) && n < sizeof idx / sizeof idx[0]) {
        const char *s;
        char *end;
        long v;

        if (strncmp(e->d_name, prefix, plen) != 0)
            continue;
        s = e->d_name + plen;
        if (*s < '0' || *s > '9')
            continue;
        errno = 0;
        v = strtol(s, &end, 10);
        if (errno || *end != '\0' || v < 0 || v > 4095)
            continue;
        idx[n++] = (int)v;
    }
    closedir(d);

    qsort(idx, n, sizeof idx[0], cmp_int);
    for (size_t i = 0; i < n; i++) {
        spath(path, sizeof path, "%s/%s%d", base, prefix, idx[i]);
        fn(path, idx[i], ctx);
    }
}
