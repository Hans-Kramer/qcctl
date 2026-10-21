// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer
// SPDX-FileContributor: Claude Opus 5 (Anthropic), paired on the behavioural suite
//
// Reading a write back. A successful write proves only that the store handler took the
// bytes; it may have clamped the value, rounded it, or been superseded. These two passes
// are what say whether what the kernel accepted is what was asked for - and both carry the
// same guard, that a clean report over nothing read has verified nothing.

#define _GNU_SOURCE
#include "../src/apply.h"
#include "t.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char dir[64];

static void put_file(const char *name, const char *content)
{
    char path[256];
    FILE *f;

    snprintf(path, sizeof path, "%s/%s", dir, name);
    if ((f = fopen(path, "w")) == NULL) {
        perror(path);
        exit(2);
    }
    fputs(content, f);
    fclose(f);
}

/* A plan entry: the path as the planner resolved it, and the exact string written. */
static void plan(struct applyctx *ctx, const char *name, const char *value)
{
    int i = ctx->nw++;

    snprintf(ctx->w[i].path, sizeof ctx->w[i].path, "%s/%s", dir, name);
    snprintf(ctx->w[i].value, sizeof ctx->w[i].value, "%s", value);
}

static void t_verify_reports_what_holds(void)
{
    struct applyctx ctx;
    struct verifyreport r;

    memset(&ctx, 0, sizeof ctx);
    put_file("scaling_governor", "schedutil\n");
    put_file("scaling_max_freq", "2112000\n");
    plan(&ctx, "scaling_governor", "schedutil");
    plan(&ctx, "scaling_max_freq", "2112000");

    T_EQ_INT(apply_verify(&ctx, &r), 0, "a plan that holds verifies");
    T_EQ_INT(r.checked, 2, "both settings were read back");
    T_EQ_INT(r.diverged, 0, "neither diverged");
    T_EQ_INT(r.unreadable, 0, "and both were readable");

    /* Two separate things, and this used to conflate them.
     *
     * The newline is handled by the READER: read_file_str() cuts at the first one, so a
     * second newline never reaches the comparison and proves nothing about trimming.
     * Trailing BLANKS are handled by apply_verify's own loop, and reaching it takes a space
     * or a tab or a carriage return before the newline - which is what a kernel that pads
     * an attribute returns, and what this test fed none of. */
    put_file("scaling_governor", "schedutil\n\n");
    T_EQ_INT(apply_verify(&ctx, &r), 0, "a second newline is not a divergence");
    T_EQ_INT(r.diverged, 0, "because the reader stops at the first");

    put_file("scaling_governor", "schedutil \n");
    T_EQ_INT(apply_verify(&ctx, &r), 0, "a trailing space is trimmed, not compared");
    T_EQ_INT(r.diverged, 0, "so this is not a divergence");

    put_file("scaling_governor", "schedutil\t\r\n");
    T_EQ_INT(apply_verify(&ctx, &r), 0, "nor are a tab and a carriage return");
    T_EQ_INT(r.diverged, 0, "which is what the trim loop is for");

    /* The other direction: trimming is for the END of the value only. A difference inside
     * it is a real divergence and must still be reported. */
    /* Divergence is reported, not failed - apply.h:65 is explicit that rolling back over a
     * frequency the kernel rounded would be worse than the silence it replaced. So the
     * return stays 0 and the count is where the answer lives. */
    put_file("scaling_governor", "sched util\n");
    T_EQ_INT(apply_verify(&ctx, &r), 0, "a blank INSIDE the value is not a read failure");
    T_EQ_INT(r.diverged, 1, "but it IS a divergence, and is counted");
    put_file("scaling_governor", "schedutil\n");
}

static void t_verify_finds_a_clamp(void)
{
    struct applyctx ctx;
    struct verifyreport r;

    memset(&ctx, 0, sizeof ctx);
    /* What a clamping driver leaves behind: the write succeeded, the value did not take.
     * Measured on the board with scaling_min_freq above the live max. */
    put_file("scaling_min_freq", "1497600\n");
    plan(&ctx, "scaling_min_freq", "2112000");

    T_EQ_INT(apply_verify(&ctx, &r), 0, "a divergence is a report, not a failure");
    T_EQ_INT(r.checked, 1, "the setting was read");
    T_EQ_INT(r.diverged, 1, "and it holds something else");
    T_HAS(r.path, "scaling_min_freq", "the first divergence names its path");
    T_EQ_STR(r.want, "2112000", "what was asked");
    T_EQ_STR(r.got, "1497600", "and what is held");
}

static void t_verify_counts_unreadable_separately(void)
{
    struct applyctx ctx;
    struct verifyreport r;

    memset(&ctx, 0, sizeof ctx);
    put_file("present", "1\n");
    plan(&ctx, "present", "1");
    plan(&ctx, "absent", "1");       /* never created */

    T_EQ_INT(apply_verify(&ctx, &r), 0, "one readable setting is enough to report on");
    T_EQ_INT(r.checked, 1, "one was checked");
    T_EQ_INT(r.unreadable, 1, "and one could not be read");
    T_EQ_INT(r.diverged, 0, "an unreadable setting is not a divergence");
}

static void t_verify_refuses_to_look_clean_over_nothing(void)
{
    struct applyctx ctx;
    struct verifyreport r;

    /* The guard that matters: a pass that read nothing must not be indistinguishable from
     * a pass that found everything in order. */
    memset(&ctx, 0, sizeof ctx);
    plan(&ctx, "absent-one", "1");
    plan(&ctx, "absent-two", "2");
    T_EQ_INT(apply_verify(&ctx, &r), -1, "candidates but nothing readable fails");
    T_EQ_INT(r.checked, 0, "nothing was checked");
    T_EQ_INT(r.unreadable, 2, "and both are accounted for");

    /* An empty plan is not that case: there was nothing to check, which is not a failure. */
    memset(&ctx, 0, sizeof ctx);
    T_EQ_INT(apply_verify(&ctx, &r), 0, "an empty plan is not a failure");
    T_EQ_INT(r.checked, 0, "with nothing checked");
}

static void t_cooling_states_are_skipped(void)
{
    struct applyctx ctx;
    struct verifyreport r;
    char sub[128], cmd[256];

    /* A cooling device's cur_state is the governor's OUTPUT, not a setting that holds:
     * state_restore() skips these for the same reason, and verifying them would report a
     * divergence every time the kernel did its job. */
    snprintf(sub, sizeof sub, "%s/cooling_device7", dir);
    snprintf(cmd, sizeof cmd, "mkdir -p '%s'", sub);
    if (system(cmd) != 0) {
        T_FAIL("%s", "cannot create the cooling device fixture");
        return;
    }
    put_file("cooling_device7/cur_state", "4\n");

    memset(&ctx, 0, sizeof ctx);
    plan(&ctx, "cooling_device7/cur_state", "10");
    T_EQ_INT(apply_verify(&ctx, &r), 0, "a plan of only cooling states is not a failure");
    T_EQ_INT(r.checked, 0, "it is skipped rather than checked");
    T_EQ_INT(r.diverged, 0, "so the kernel's own choice is never called a divergence");
}

static void t_state_verify_reads_the_snapshot_back(void)
{
    char snap[256];
    struct verifyreport r;
    FILE *f;

    /* --reset replays this file and used to report only what it had WRITTEN. The format is
     * path TAB value, with # comments, and the same cooling-device exclusion. */
    put_file("governor", "schedutil\n");
    put_file("max_freq", "1497600\n");
    /* This case names a cooling device in its snapshot, so it creates one: it used to
     * inherit the directory from whichever case ran before it, which made "the cooling
     * state is not one of the two" true for the wrong reason - the path was merely
     * unreadable. Swapping the order of two calls in main() changed the result. */
    {
        char sub[128], cmd[256];

        snprintf(sub, sizeof sub, "%s/cooling_device7", dir);
        snprintf(cmd, sizeof cmd, "mkdir -p '%s'", sub);
        if (system(cmd) != 0) {
            T_FAIL("%s", "cannot create the cooling device fixture");
            return;
        }
        put_file("cooling_device7/cur_state", "4\n");
    }
    snprintf(snap, sizeof snap, "%s/boot-state", dir);
    if ((f = fopen(snap, "w")) == NULL) {
        T_FAIL("%s", "cannot write the snapshot fixture");
        return;
    }
    /* A comment WITH a tab in it: without one it is discarded by the no-tab rule and the
     * '#' rule is never the reason, so disabling that rule changed nothing. */
    fprintf(f, "# a comment\twith a tab is still a comment\n");
    fprintf(f, "%s/governor\tschedutil\n", dir);
    fprintf(f, "%s/max_freq\t2112000\n", dir);          /* diverges: holds 1497600 */
    fprintf(f, "%s/cooling_device7/cur_state\t0\n", dir);  /* skipped */
    fprintf(f, "a line with no tab is skipped\n");
    fclose(f);

    T_EQ_INT(state_verify(snap, &r), 0, "the snapshot is read back");
    T_EQ_INT(r.checked, 2, "two values were compared - the cooling state is not one");
    T_EQ_INT(r.diverged, 1, "one holds something else");
    /* Nothing UNREADABLE either, which is what makes the comment and no-tab rules
     * observable: a comment treated as data becomes a path that cannot be read, and
     * without this line that was indistinguishable from a comment correctly skipped. */
    T_EQ_INT(r.unreadable, 0, "and nothing was unreadable - every skipped line was skipped");
    T_HAS(r.path, "max_freq", "and it is named");
    T_EQ_STR(r.want, "2112000", "with the snapshot's value");
    T_EQ_STR(r.got, "1497600", "and the live one");

    T_EQ_INT(state_verify("/no/such/snapshot", &r), -1, "a missing snapshot fails");

    /* The same guard as apply_verify's, and it has to be asserted separately: there are two
     * copies of this rule, one per pass, and removing either used to go unnoticed here. */
    if ((f = fopen(snap, "w")) == NULL) {
        T_FAIL("%s", "cannot rewrite the snapshot fixture");
        return;
    }
    fprintf(f, "%s/gone-one\tschedutil\n", dir);
    fprintf(f, "%s/gone-two\t1497600\n", dir);
    fclose(f);
    T_EQ_INT(state_verify(snap, &r), -1,
             "a snapshot whose paths cannot be read proves nothing, and says so");
    T_EQ_INT(r.checked, 0, "nothing checked");
    T_EQ_INT(r.unreadable, 2, "both unreadable");

    /* A snapshot of nothing but cooling states has no candidates, so it is not the
     * "read nothing" failure either. */
    if ((f = fopen(snap, "w")) == NULL) {
        /* Reported, not returned silently: a bare return here dropped two checks and still
         * printed "ok", because the harness can only see a file that checked NOTHING. */
        T_FAIL("%s", "cannot rewrite the snapshot fixture for the last case");
        return;
    }
    fprintf(f, "%s/cooling_device7/cur_state\t4\n", dir);
    fclose(f);
    T_EQ_INT(state_verify(snap, &r), 0, "a snapshot of only cooling states is not a failure");
    T_EQ_INT(r.checked, 0, "and checks nothing");

    /* THE TRIM, in this pass too. The rule is implemented twice - once per pass - and
     * fixing apply_verify's copy left this one uncovered: a kernel that pads an attribute
     * would make every --reset report a divergence it should have trimmed. */
    put_file("governor", "schedutil \t\n");
    if ((f = fopen(snap, "w")) == NULL) {
        T_FAIL("%s", "cannot rewrite the snapshot fixture for the trim case");
        return;
    }
    fprintf(f, "%s/governor\tschedutil\n", dir);
    fclose(f);
    T_EQ_INT(state_verify(snap, &r), 0, "a restored value padded with blanks is read back");
    T_EQ_INT(r.checked, 1, "it is compared");
    T_EQ_INT(r.diverged, 0, "and the padding is trimmed, not called a divergence");

    /* And the cooling-device exclusion is NARROW: it is cur_state on a cooling device that
     * is the governor's output, not every attribute of one. An exclusion widened to either
     * substring would quietly stop verifying a device's max_state as well. */
    put_file("cooling_device7/max_state", "10\n");
    if ((f = fopen(snap, "w")) == NULL) {
        T_FAIL("%s", "cannot rewrite the snapshot fixture for the exclusion case");
        return;
    }
    fprintf(f, "%s/cooling_device7/max_state\t10\n", dir);
    fclose(f);
    T_EQ_INT(state_verify(snap, &r), 0, "another attribute of a cooling device is read back");
    T_EQ_INT(r.checked, 1, "and IS compared - the exclusion is cur_state alone");
}

int main(void)
{
    char cmd[128];

    snprintf(dir, sizeof dir, "/tmp/qcctl-t_apply.%d", (int)getpid());
    snprintf(cmd, sizeof cmd, "mkdir -p '%s'", dir);
    if (system(cmd) != 0) {
        fprintf(stderr, "cannot create %s\n", dir);
        return 2;
    }

    t_verify_reports_what_holds();
    t_verify_finds_a_clamp();
    t_verify_counts_unreadable_separately();
    t_verify_refuses_to_look_clean_over_nothing();
    t_cooling_states_are_skipped();
    t_state_verify_reads_the_snapshot_back();

    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0)
        fprintf(stderr, "warning: %s was left behind\n", dir);
    return t_summary("apply");
}
