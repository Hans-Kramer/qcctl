// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer
// SPDX-FileContributor: Claude Opus 5 (Anthropic), paired on the behavioural suite
//
// The WRITE half of apply.c, which nothing exercised.
//
// apply.h leads with two guarantees: every path is captured before it is written, and a
// write that fails mid-apply rolls back the writes already made - "a profile is applied
// whole or not at all". The suite reached neither. apply_commit's success path runs through
// the CLI test, but no test anywhere made a write FAIL, so apply_rollback could be deleted
// outright and every check in both repositories still passed. Same for apply_orphans, the
// revert-on-switch mechanism, and for the boot-id scoping of a saved selection.
//
// A write is made to fail the way the kernel fails one: the target is there, was readable
// when the plan was built, and refuses the open. A read-only sysfs attribute does that to
// any caller - kernfs checks the mode itself, so even root is refused - and chmod 0444 on a
// fixture file reproduces it for everyone EXCEPT root, who overrides DAC and sails through.
//
// That is not a detail: qcctld runs as root, and run as root this file reported seven
// failures while the code was correct. So the two cases that exist to be refused drop
// CAP_DAC_OVERRIDE and CAP_DAC_READ_SEARCH first, and the kernel then refuses them for the
// same reason the board would. See bind_to_mode_bits().

#define _GNU_SOURCE
#include "../src/apply.h"
#include "t.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <linux/capability.h>

static char dir[64];

/* 0 when this process can still ignore the mode bits, which makes the two refusal cases
 * untestable rather than merely failing. */
static int bound_by_mode = 1;

/* Mode bits are the only lever a fixture has for "the kernel refused this write", and for
 * root they are no lever at all. Dropping these two capabilities makes the ordinary DAC
 * check apply to us: a 0444 target then refuses our open and a 0200 one refuses our read,
 * which is what the attribute itself would do on the board. Nothing after this needs them -
 * every fixture file is ours and 0644 - and non-root never had them to drop. */
static void bind_to_mode_bits(void)
{
    struct __user_cap_header_struct hdr;
    struct __user_cap_data_struct data[2];
    const uint32_t drop = (1u << CAP_DAC_OVERRIDE) | (1u << CAP_DAC_READ_SEARCH);

    if (geteuid() != 0)
        return;                         /* already bound */

    memset(&hdr, 0, sizeof hdr);
    memset(data, 0, sizeof data);
    hdr.version = _LINUX_CAPABILITY_VERSION_3;
    if (syscall(SYS_capget, &hdr, data) != 0) {
        bound_by_mode = 0;
        return;
    }
    /* Only the first word: both capabilities are below 32, and the second word's bits 1
     * and 2 are other capabilities entirely. */
    data[0].effective   &= ~drop;
    data[0].permitted   &= ~drop;
    data[0].inheritable &= ~drop;
    bound_by_mode = (syscall(SYS_capset, &hdr, data) == 0);
}

#define POLICY0 "/sys/devices/system/cpu/cpufreq/policy0"

/* Absolute path of a fixture attribute, for the test's own reads and chmods. */
static void fx(char *out, size_t len, const char *rel)
{
    snprintf(out, len, "%s%s", dir, rel);
}

static void put(const char *rel, const char *content)
{
    char path[320];
    FILE *f;

    fx(path, sizeof path, rel);
    if ((f = fopen(path, "w")) == NULL) {
        perror(path);
        exit(2);
    }
    fprintf(f, "%s\n", content);
    fclose(f);
}

/* Contents of a fixture attribute, newline stripped, "" when unreadable. */
static const char *get(const char *rel)
{
    static char buf[128];
    char path[320];
    FILE *f;

    buf[0] = '\0';
    fx(path, sizeof path, rel);
    if ((f = fopen(path, "r")) == NULL)
        return buf;
    if (fgets(buf, sizeof buf, f) == NULL)
        buf[0] = '\0';
    fclose(f);
    buf[strcspn(buf, "\n")] = '\0';
    return buf;
}

static void chmod_fx(const char *rel, mode_t m)
{
    char path[320];

    fx(path, sizeof path, rel);
    if (chmod(path, m) != 0) {
        perror(path);
        exit(2);
    }
}

/* The three attributes every case here starts from, at their "boot" values. The modes are
 * reset first, because a case that made one read-only to force a failed write would
 * otherwise leave the next case unable to write its own starting state. */
static void boot_state(void)
{
    static const char *const attrs[] = {
        POLICY0 "/scaling_governor", POLICY0 "/scaling_min_freq",
        POLICY0 "/scaling_max_freq",
    };
    static const char *const vals[] = { "schedutil", "902400", "2112000" };
    char path[320];

    for (size_t i = 0; i < sizeof attrs / sizeof attrs[0]; i++) {
        fx(path, sizeof path, attrs[i]);
        if (access(path, F_OK) == 0)
            chmod_fx(attrs[i], 0644);
        put(attrs[i], vals[i]);
    }
}

/* A plan entry built by hand, so the ORDER of the writes is the test's to decide.
 * apply_plan sorts by phase; these cases are about what happens at write number two. */
static void plan(struct applyctx *ctx, const char *rel, const char *value)
{
    int i = ctx->nw++;

    fx(ctx->w[i].path, sizeof ctx->w[i].path, rel);
    snprintf(ctx->w[i].value, sizeof ctx->w[i].value, "%s", value);
}

/* policy0 as apply_plan needs to see it, narrow on purpose - see t_config.c. */
static void board_surface(struct surface *s)
{
    memset(s, 0, sizeof *s);
    s->npol = 1;
    s->pol[0].id = 0;
    s->pol[0].ncpus = 2;
    s->pol[0].cpus[0] = 0;
    s->pol[0].cpus[1] = 1;
    s->pol[0].cpuinfo_min_khz = 902400;
    s->pol[0].cpuinfo_max_khz = 2112000;
    s->pol[0].nopp = 3;
    s->pol[0].opp_khz[0] = 902400;
    s->pol[0].opp_khz[1] = 1497600;
    s->pol[0].opp_khz[2] = 2112000;
    s->pol[0].ngov = 2;
    snprintf(s->pol[0].governors[0], NAME_LEN, "schedutil");
    snprintf(s->pol[0].governors[1], NAME_LEN, "performance");
    s->ncpu = 2;
    s->cpu[0].cpu = 0; s->cpu[0].present = 1; s->cpu[0].online = 1;
    s->cpu[1].cpu = 1; s->cpu[1].present = 1; s->cpu[1].online = 1;
}

static void t_commit_captures_before_it_writes(void)
{
    struct applyctx ctx;
    char err[ERRLEN];

    boot_state();
    memset(&ctx, 0, sizeof ctx);
    plan(&ctx, POLICY0 "/scaling_governor", "performance");
    plan(&ctx, POLICY0 "/scaling_min_freq", "1497600");

    T_EQ_INT(apply_commit(&ctx, err, sizeof err), 0, "a plan that can be written commits");
    T_EQ_STR(get(POLICY0 "/scaling_governor"), "performance", "the governor is written");
    T_EQ_STR(get(POLICY0 "/scaling_min_freq"), "1497600", "and the frequency");

    /* The capture is what makes --reset restore what was there rather than what we
     * believe stock is, so it has to hold the PRIOR values, not the new ones. */
    T_EQ_INT(ctx.nsv, 2, "both paths were captured");
    T_EQ_STR(ctx.sv[0].value, "schedutil", "the first capture is the prior value");
    T_EQ_STR(ctx.sv[1].value, "902400", "and so is the second");
    T_EQ_INT(ctx.sv[0].had_value, 1, "both were readable");
    T_EQ_INT(ctx.sv[1].had_value, 1, "so both can be rolled back");
}

static void t_a_failed_write_rolls_the_earlier_ones_back(void)
{
    struct applyctx ctx;
    char err[ERRLEN];

    /* THE GUARANTEE apply.h LEADS WITH. Three writes, the second refused: the first must
     * be undone and the third never attempted, so the board is left at boot values and
     * not in a state no config describes. Deleting apply_rollback's call used to pass
     * every check in both repositories. */
    if (!bound_by_mode) {
        T_FAIL("%s", "cannot drop CAP_DAC_OVERRIDE, so a refused write cannot be simulated");
        return;
    }
    boot_state();
    chmod_fx(POLICY0 "/scaling_min_freq", 0444);

    memset(&ctx, 0, sizeof ctx);
    plan(&ctx, POLICY0 "/scaling_governor", "performance");
    plan(&ctx, POLICY0 "/scaling_min_freq", "1497600");
    plan(&ctx, POLICY0 "/scaling_max_freq", "1497600");

    T_EQ_INT(apply_commit(&ctx, err, sizeof err), -1, "a refused write fails the apply");
    /* The message is built from the count the planner had, not from what the rollback did,
     * so these two say the report is right - never that the undo happened. The three state
     * checks below are the only evidence of that, and they are what caught a deleted
     * apply_rollback() call. */
    T_HAS(err, "rolled back", "and says it rolled back");
    T_HAS(err, "scaling_min_freq", "naming the write that failed");

    T_EQ_STR(get(POLICY0 "/scaling_governor"), "schedutil",
             "the write BEFORE the failure is undone");
    T_EQ_STR(get(POLICY0 "/scaling_min_freq"), "902400",
             "the refused write never landed");
    T_EQ_STR(get(POLICY0 "/scaling_max_freq"), "2112000",
             "and the write after it was never attempted");

    chmod_fx(POLICY0 "/scaling_min_freq", 0644);
}

static void t_rollback_restores_every_captured_value(void)
{
    struct applyctx ctx;
    char err[ERRLEN];

    /* Called directly, which is also how a caller that changed its mind would use it.
     * Ordering is newest-first in the source, for the min/max dance; plain files cannot
     * observe the order, so what is asserted here is that NOTHING is left behind. */
    boot_state();
    memset(&ctx, 0, sizeof ctx);
    plan(&ctx, POLICY0 "/scaling_governor", "performance");
    plan(&ctx, POLICY0 "/scaling_min_freq", "1497600");
    plan(&ctx, POLICY0 "/scaling_max_freq", "1497600");
    T_EQ_INT(apply_commit(&ctx, err, sizeof err), 0, "three writes commit");

    T_EQ_INT(apply_rollback(&ctx, err, sizeof err), 0, "and roll back cleanly");
    T_EQ_STR(get(POLICY0 "/scaling_governor"), "schedutil", "the governor is back");
    T_EQ_STR(get(POLICY0 "/scaling_min_freq"), "902400", "the minimum is back");
    T_EQ_STR(get(POLICY0 "/scaling_max_freq"), "2112000", "and the maximum");
}

static void t_an_unreadable_prior_value_is_not_a_rollback_failure(void)
{
    struct applyctx ctx;
    char err[ERRLEN];

    /* A write-only node - sysfs has them - cannot be captured, so there is nothing to
     * restore and that is not an error. The distinction is `had_value`: a rollback that
     * treated an unreadable capture as a failure would report a failed undo on every
     * apply that touched one. */
    if (!bound_by_mode) {
        T_FAIL("%s", "cannot drop CAP_DAC_READ_SEARCH, so an unreadable node cannot be simulated");
        return;
    }
    boot_state();
    chmod_fx(POLICY0 "/scaling_max_freq", 0200);        /* write-only */

    memset(&ctx, 0, sizeof ctx);
    plan(&ctx, POLICY0 "/scaling_max_freq", "1497600");
    plan(&ctx, POLICY0 "/scaling_governor", "performance");

    T_EQ_INT(apply_commit(&ctx, err, sizeof err), 0, "the apply still commits");
    T_EQ_INT(ctx.sv[0].had_value, 0, "the write-only node captured nothing");
    T_EQ_INT(ctx.sv[1].had_value, 1, "the readable one did");

    T_EQ_INT(apply_rollback(&ctx, err, sizeof err), 0,
             "and the rollback reports no failure");
    T_EQ_STR(get(POLICY0 "/scaling_governor"), "schedutil",
             "restoring what it could");

    chmod_fx(POLICY0 "/scaling_max_freq", 0644);
}

static void t_a_dry_commit_captures_but_writes_nothing(void)
{
    struct applyctx ctx;
    char err[ERRLEN];

    boot_state();
    memset(&ctx, 0, sizeof ctx);
    ctx.dry = 1;
    plan(&ctx, POLICY0 "/scaling_governor", "performance");

    T_EQ_INT(apply_commit(&ctx, err, sizeof err), 0, "a dry commit succeeds");
    T_EQ_STR(get(POLICY0 "/scaling_governor"), "schedutil", "and writes nothing");
    /* It still captures: --dry-run reports what it WOULD change, which needs the
     * prior value. */
    T_EQ_INT(ctx.nsv, 1, "but the prior value is still captured");
    T_EQ_STR(ctx.sv[0].value, "schedutil", "and it is the value on disk");
}

/* ---- apply_orphans: making a profile switch history-independent ------------------- */

static const char *two_profiles =
    "< PROFILE ID=0 NAME=old DEFAULT >\n"
    "  CPU_POLICY 0  GOVERNOR performance\n"
    "  CPU_POLICY 0  MIN 1497600kHz\n"
    ">\n"
    "< PROFILE ID=1 NAME=new >\n"
    "  CPU_POLICY 0  GOVERNOR schedutil\n"
    ">\n";

/* A boot-state snapshot in state_save's own format: path, tab, value. */
static void write_snapshot(const char *name)
{
    char path[320], gov[320], min[320];
    FILE *f;

    fx(path, sizeof path, name);
    fx(gov, sizeof gov, POLICY0 "/scaling_governor");
    fx(min, sizeof min, POLICY0 "/scaling_min_freq");
    if ((f = fopen(path, "w")) == NULL) {
        perror(path);
        exit(2);
    }
    fprintf(f, "# a boot-state snapshot\n");
    fprintf(f, "%s\tschedutil\n", gov);
    fprintf(f, "%s\t902400\n", min);
    fclose(f);
}

static void t_orphans_revert_what_the_new_profile_does_not_mention(void)
{
    struct pconfig c;
    struct surface s;
    struct applyctx newctx;
    char err[ERRLEN], snap[320];
    int nrev = -1;

    /* The defect this exists for: 'old' sets the minimum, 'new' says nothing about it, so
     * without this the board keeps old's minimum and its state depends on which profiles
     * it has been through rather than which one is applied. */
    boot_state();
    write_snapshot("/snapshot");
    fx(snap, sizeof snap, "/snapshot");
    board_surface(&s);
    T_EQ_INT(config_parse_string(two_profiles, &c), 0, "the two profiles parse");

    /* Apply 'old' for real, so the board is in the state a switch would find. */
    memset(&newctx, 0, sizeof newctx);
    T_EQ_INT(apply_plan(config_find(&c, "old"), &s, &newctx, err, sizeof err), 0,
             "old plans");
    T_EQ_INT(apply_commit(&newctx, err, sizeof err), 0, "and applies");
    T_EQ_STR(get(POLICY0 "/scaling_min_freq"), "1497600", "old's minimum is in force");

    /* Now switch to 'new', which mentions only the governor. */
    memset(&newctx, 0, sizeof newctx);
    T_EQ_INT(apply_plan(config_find(&c, "new"), &s, &newctx, err, sizeof err), 0,
             "new plans");
    T_EQ_INT(apply_commit(&newctx, err, sizeof err), 0, "and applies");

    T_EQ_INT(apply_orphans(&c, "old", &newctx, &s, snap, 0, err, sizeof err, &nrev), 0,
             "the orphan sweep succeeds");
    T_EQ_INT(nrev, 1, "and reverts exactly the one setting new does not mention");
    T_EQ_STR(get(POLICY0 "/scaling_min_freq"), "902400",
             "the minimum is back at its boot value");
    T_EQ_STR(get(POLICY0 "/scaling_governor"), "schedutil",
             "while the setting new DOES own keeps new's value");
}

static void t_a_dry_orphan_sweep_counts_without_writing(void)
{
    struct pconfig c;
    struct surface s;
    struct applyctx newctx;
    char err[ERRLEN], snap[320];
    int nrev = -1;

    boot_state();
    write_snapshot("/snapshot");
    fx(snap, sizeof snap, "/snapshot");
    board_surface(&s);
    config_parse_string(two_profiles, &c);
    put(POLICY0 "/scaling_min_freq", "1497600");        /* as if old were in force */

    memset(&newctx, 0, sizeof newctx);
    apply_plan(config_find(&c, "new"), &s, &newctx, err, sizeof err);

    /* A dry sweep REPORTS, so its stdout is part of the contract and is captured rather
     * than left to scroll past the suite's own output. */
    {
        char cappath[320], capline[512] = "";
        FILE *cap;
        int saved_fd = dup(1), rc;

        fx(cappath, sizeof cappath, "/dry.out");
        fflush(stdout);
        if ((cap = fopen(cappath, "w")) != NULL && saved_fd >= 0) {
            dup2(fileno(cap), 1);
            rc = apply_orphans(&c, "old", &newctx, &s, snap, 1, err, sizeof err, &nrev);
            fflush(stdout);
            dup2(saved_fd, 1);
            close(saved_fd);
            fclose(cap);

            T_EQ_INT(rc, 0, "a dry sweep succeeds");
            T_EQ_INT(nrev, 1, "and counts what it would revert");
            T_EQ_STR(get(POLICY0 "/scaling_min_freq"), "1497600",
                     "without writing anything");
            if ((cap = fopen(cappath, "r")) != NULL) {
                if (fgets(capline, sizeof capline, cap) == NULL)
                    capline[0] = '\0';
                fclose(cap);
            }
            T_HAS(capline, "revert", "while saying it would revert");
            T_HAS(capline, "scaling_min_freq", "naming the setting");
            T_HAS(capline, "902400", "and the value it would go back to");
        } else {
            T_FAIL("%s", "cannot capture the dry sweep's output");
        }
    }
}

static void t_orphans_with_nothing_in_force_is_a_no_op(void)
{
    struct pconfig c;
    struct surface s;
    struct applyctx newctx;
    char err[ERRLEN], snap[320];
    int nrev = -1;

    /* First boot, or a profile that has since been deleted from the config: neither is an
     * error, and neither may revert anything. */
    boot_state();
    write_snapshot("/snapshot");
    fx(snap, sizeof snap, "/snapshot");
    board_surface(&s);
    config_parse_string(two_profiles, &c);
    memset(&newctx, 0, sizeof newctx);
    apply_plan(config_find(&c, "new"), &s, &newctx, err, sizeof err);
    put(POLICY0 "/scaling_min_freq", "1497600");

    T_EQ_INT(apply_orphans(&c, "", &newctx, &s, snap, 0, err, sizeof err, &nrev), 0,
             "no previous profile is not an error");
    T_EQ_INT(nrev, 0, "and reverts nothing");

    nrev = -1;
    T_EQ_INT(apply_orphans(&c, "gone", &newctx, &s, snap, 0, err, sizeof err, &nrev), 0,
             "a previous profile no longer in the config is not an error either");
    T_EQ_INT(nrev, 0, "and reverts nothing");
    T_EQ_STR(get(POLICY0 "/scaling_min_freq"), "1497600",
             "so the board is left exactly as it was");
}

/* ---- the selection, and its boot scope -------------------------------------------- */

static void t_a_selection_is_scoped_to_this_boot(void)
{
    char sel[320], out[NAME_LEN], line[256];
    FILE *f;

    /* The selection must survive a daemon restart and NOT a power cycle: it is what the
     * operator chose, and a choice from before a reboot is not evidence of a choice now. */
    fx(sel, sizeof sel, "/selected");
    T_EQ_INT(selection_save(sel, "workstation"), 0, "a selection is recorded");
    T_EQ_INT(selection_load(sel, out, sizeof out), 1, "and reads back within the boot");
    T_EQ_STR(out, "workstation", "as the profile that was chosen");

    /* Forge the header with an id from another boot. Nothing else changes. */
    if ((f = fopen(sel, "r")) != NULL) {
        char saved[NAME_LEN] = "";

        /* line 1 is the header we are replacing; line 2 is the profile name to keep. */
        if (fgets(line, sizeof line, f) == NULL || fgets(saved, sizeof saved, f) == NULL)
            saved[0] = '\0';
        fclose(f);
        if ((f = fopen(sel, "w")) != NULL) {
            fprintf(f, "# boot_id 00000000-0000-0000-0000-000000000000\n%s", saved);
            fclose(f);
        }
    }
    out[0] = 'x';
    T_EQ_INT(selection_load(sel, out, sizeof out), 0,
             "a selection from another boot is not a selection");
    T_EQ_STR(out, "", "and nothing is handed back");

    T_EQ_INT(selection_load("/nonexistent/selected", out, sizeof out), 0,
             "nor is a missing file");
}

static void t_a_tagged_snapshot_carries_the_boot_id(void)
{
    struct surface s;
    char err[ERRLEN], path[320], line[256];
    FILE *f;
    int tagged = 0;

    /* The tag is what state_save_once() keys on to avoid re-snapshotting its own applied
     * values after a daemon restart. Without it in the header there is no way to tell a
     * snapshot from this boot from one from the last. */
    board_surface(&s);
    fx(path, sizeof path, "/tagged");
    T_EQ_INT(state_save_tagged(&s, path, "feedface-0000-0000-0000-000000000001",
                               err, sizeof err), 0, "a tagged snapshot is written");
    if ((f = fopen(path, "r")) != NULL) {
        while (fgets(line, sizeof line, f))
            if (strstr(line, "feedface-0000-0000-0000-000000000001"))
                tagged = 1;
        fclose(f);
    }
    T_EQ_INT(tagged, 1, "and the boot id is in it");
}

int main(void)
{
    char cmd[512];

    snprintf(dir, sizeof dir, "/tmp/qcctl-t_commit.%d", (int)getpid());
    snprintf(cmd, sizeof cmd, "mkdir -p '%s%s'", dir, POLICY0);
    if (system(cmd) != 0) {
        fprintf(stderr, "cannot create %s\n", dir);
        return 2;
    }
    io_set_sysroot(dir);
    bind_to_mode_bits();

    t_commit_captures_before_it_writes();
    t_a_failed_write_rolls_the_earlier_ones_back();
    t_rollback_restores_every_captured_value();
    t_an_unreadable_prior_value_is_not_a_rollback_failure();
    t_a_dry_commit_captures_but_writes_nothing();
    t_orphans_revert_what_the_new_profile_does_not_mention();
    t_a_dry_orphan_sweep_counts_without_writing();
    t_orphans_with_nothing_in_force_is_a_no_op();
    t_a_selection_is_scoped_to_this_boot();
    t_a_tagged_snapshot_carries_the_boot_id();

    io_set_sysroot("");
    snprintf(cmd, sizeof cmd, "chmod -R u+rwX '%s' 2>/dev/null; rm -rf '%s'", dir, dir);
    if (system(cmd) != 0)
        fprintf(stderr, "warning: %s was left behind\n", dir);
    return t_summary("commit");
}
