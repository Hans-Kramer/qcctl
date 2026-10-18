// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer
//
// Turning a validated profile into writes, and undoing them exactly.
//
// Two guarantees:
//   Every path touched has its prior value captured first, so --reset restores
//   what was actually there rather than a value we believe is stock.
//   A write that fails mid-apply rolls back the writes already made. A profile
//   is applied whole or not at all; leaving the machine in a state no config
//   describes is the one outcome worse than failing.

#ifndef QCCTL_APPLY_H
#define QCCTL_APPLY_H

#include "config.h"
#include "surface.h"
#include "io.h"
#include <stddef.h>

#define MAX_SAVED   (MAX_ACTIONS * 2)

struct saved {
    char path[PATH_MAXLEN];
    char value[NAME_LEN];
    int  had_value;             // false = could not read it back (write-only node)
};

// A resolved write: a concrete path and the exact string to put in it.
struct wr {
    char path[PATH_MAXLEN];
    char value[NAME_LEN];
    int  phase;                 // ordering bucket; see apply.c
    const struct action *act;
};

struct applyctx {
    struct wr    w[MAX_SAVED];
    int          nw;
    struct saved sv[MAX_SAVED];
    int          nsv;
    int          dry;           // resolve and report, write nothing
};

// What reading the plan back found. `checked` matters as much as `diverged`: a report of
// zero divergences over zero readable settings has verified nothing.
struct verifyreport {
    int  checked;               // planned settings actually read back
    int  diverged;              // of those, how many hold something else
    int  unreadable;            // planned paths that could not be read now
    char path[PATH_MAXLEN];     // the first divergence, for a one-line report
    char want[NAME_LEN];
    char got[NAME_LEN];
};

/* Read every planned write back and compare it with what was asked for.
 *
 * A successful write proves only that the store handler did not reject the bytes. It may
 * have clamped a frequency to an available operating point, rounded a value to the
 * hardware's granularity, or been superseded moments later by another writer - on this
 * board sysfsutils and the thermal governor both write attributes a profile touches.
 * drift_baseline() already records what the kernel ACCEPTED, which is right for watching
 * drift; this is the half that says whether what it accepted is what was requested.
 *
 * Divergence is deliberately NOT a failure. Rolling a profile back because the kernel
 * rounded a frequency to a valid operating point would be worse than the silence it
 * replaces; a refused write already fails and rolls back in apply_commit().
 *
 * Returns -1 only when there were settings to check and none could be read.
 * Cooling-device states are skipped for the same reason state_restore() skips them. */
int apply_verify(const struct applyctx *ctx, struct verifyreport *r);

// Resolve a profile to concrete writes without touching anything.
int apply_plan(const struct profile *p, const struct surface *s,
               struct applyctx *ctx, char *err, size_t errlen);

// Capture prior values, then write. Rolls back on failure.
int apply_commit(struct applyctx *ctx, char *err, size_t errlen);

// Restore every captured value, newest first.
int apply_rollback(struct applyctx *ctx, char *err, size_t errlen);

/* Make profile switching idempotent.
 *
 * "A setting you omit is NOT touched" is deliberate and lets users write partial profiles.
 * Its consequence, `quiet` sets the fan trip to 45 C, and
 * switching back to `balanced` LEFT it at 45 C, because `balanced` says nothing about
 * thermal. The board's state then depends on which profiles you have been through rather
 * than which one is applied - and a fan that engages 7 C late is a thermal consequence, not
 * a cosmetic one.
 *
 * So on a switch, every path the PREVIOUS profile wrote that the new one does not mention is
 * restored from the boot-state snapshot. Targeted, so settings no profile touches are left
 * alone, and history-independent: applying B always yields boot+B whatever came before.
 *
 * The previous profile is re-PLANNED from its name rather than remembered as a path list -
 * the name is already persisted, so nothing new needs storing. If the config changed between
 * the two applies the re-plan can differ; that is a documented limit, not a guarded one.
 *
 * Call AFTER apply_commit() has SUCCEEDED, not before. apply_commit rolls back its own
 * writes on failure but knows nothing about these reverts, so reverting first would leave a
 * failed apply with the old profile's orphans at boot values and the new profile rolled
 * back - a state no config describes, which is exactly what the whole-or-nothing guarantee
 * above exists to prevent. *nreverted gets the count. */
int apply_orphans(const struct pconfig *c, const char *prev_name,
                  const struct applyctx *newctx, const struct surface *s,
                  const char *statefile, int dry,
                  char *err, size_t errlen, int *nreverted);

// Snapshot the whole control surface, for a --reset that outlives one apply.
int state_save(const struct surface *s, const char *path, char *err, size_t errlen);

/* As state_save(), but tags the snapshot with a boot id in its header. */
int state_save_tagged(const struct surface *s, const char *path, const char *bootid,
                      char *err, size_t errlen);

/* Snapshot only if no snapshot exists for the CURRENT boot. The daemon must use this:
 * it snapshots and then applies, so re-snapshotting on a restart would record its own
 * applied values as stock. *skipped is set when an existing snapshot was kept. */
int state_save_once(const struct surface *s, const char *path, int *skipped,
                    char *err, size_t errlen);
int state_restore(const char *path, char *err, size_t errlen, int *nrestored);

/* Read the snapshot back against the live surface, after state_restore() has replayed it.
 * Same report and the same contract as apply_verify(): divergence is a finding for the
 * caller to judge, and -1 means nothing could be read at all. Skips the same lines the
 * restore skips, because a verification of a different set proves nothing about it. */
int state_verify(const char *path, struct verifyreport *r);

/* The operator's profile SELECTION, boot-id tagged so it survives a daemon restart but
 * not a power cycle. Distinct from the active file, which says what is in force NOW and
 * is unlinked on exit. 0 on success, -1 if nothing could be recorded. */
int selection_save(const char *path, const char *profile);

/* 1 = a selection from this boot is in `out`; 0 = none recorded, or from another boot. */
int selection_load(const char *path, char *out, size_t len);

#endif /* QCCTL_APPLY_H */
