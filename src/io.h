// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer
//
// Derived from qcstats src/io.h, with writability probing added.
// The only file that touches the filesystem, so QCCTL_SYSROOT relocates the whole tool.

#ifndef QCCTL_IO_H
#define QCCTL_IO_H

#include <stdint.h>
#include <stddef.h>

#define PATH_MAXLEN     768

#define SYS_CPU         "/sys/devices/system/cpu"
#define CPUFREQ_DIR     SYS_CPU "/cpufreq"
#define DEVFREQ_DIR     "/sys/class/devfreq"
#define THERMAL_DIR     "/sys/class/thermal"
#define HWMON_DIR       "/sys/class/hwmon"

// hwmon indices vary per boot, so these are resolved by name, never by index.
#define HWMON_POWER     "ina232"
#define HWMON_FAN       "emc2305"

#define ENV_SYSROOT     "QCCTL_SYSROOT"

extern const char *sysroot;
void io_set_sysroot(const char *dir);

// Build a sysroot-prefixed path. For LITERAL paths only - dirs from hwmon_find()
// and the scanners are already prefixed.
void spath(char *out, size_t len, const char *fmt, ...);

int read_file_str(const char *path, char *buf, size_t len);
int read_file_u64(const char *path, uint64_t *out);
int read_attr_str(const char *dir, const char *attr, char *buf, size_t len);
int read_attr_u64(const char *dir, const char *attr, uint64_t *out);

// Does dir/attr exist? Existence only - a node can exist and be unreadable.
int path_exists(const char *dir, const char *attr);

// Does this exact path exist? access(F_OK), so a write-only node still counts.
int path_present(const char *path);

// Octal mode, or -1 if unknown. Against a fixture this comes from the captured
// .manifest, not from the copied file.
int path_mode(const char *dir, const char *attr);

// Owner string ("root:root") or "" if unknown. Manifest-backed, as above.
int path_owner(const char *dir, const char *attr, char *out, size_t len);

int hwmon_find(const char *want, char *out, size_t len);

// The only write path in the codebase. 0 on success, -1 with errno set.
int write_path_str(const char *path, const char *value);

// Create-or-truncate, for files this project owns (/run, /var/lib). Not for sysfs.
int write_new_file(const char *path, const char *value);

// Scan a class directory for `prefix`N entries, sorted by numeric suffix.
typedef void (*scan_fn)(const char *dir, int idx, void *ctx);
void scan_indexed(const char *base, const char *prefix, scan_fn fn, void *ctx);

#endif /* QCCTL_IO_H */
