// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer
// SPDX-FileContributor: Claude Opus 5 (Anthropic), paired on the behavioural suite
//
// The fan.config parser and validator - about 270 lines that nothing called.
//
// t_curve.c tests the strategy ENGINES by hand-building a struct curve, so every refusal
// between a file on disk and that struct was unreached: a config with descending points, a
// target above the 85 C safety ceiling, a deadband of zero, a poll interval of 10ms, a point
// above the cooling device's maximum. Each of those is a line in fancurve.c whose only
// purpose is to refuse, and deleting any of them changed no test result.
//
// These are the operator's mistakes, not the programmer's: the file is hand-written, and a
// refusal with a line number is the difference between a config that is corrected in a
// minute and a fan that behaves oddly for a week.

#define _GNU_SOURCE
#include "../src/fancurve.h"
#include "t.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The board as the validator needs to see it: the fan with ten states, and a second, much
 * smaller cooling device - this board really has several, with maxima from 3 to 100, and a
 * ceiling check that only ever sees the fan proves nothing about the others. */
static void board_surface(struct surface *s)
{
    memset(s, 0, sizeof *s);
    s->nzone = 1;
    s->zone[0].idx = 37;
    snprintf(s->zone[0].type, sizeof s->zone[0].type, "cpuss-0-0-thermal");
    s->ncdev = 2;
    s->cdev[0].idx = 7;
    snprintf(s->cdev[0].type, sizeof s->cdev[0].type, "emc2305_fan");
    s->cdev[0].max_state = 10;
    s->cdev[0].state_ok = 1;
    s->cdev[1].idx = 11;
    snprintf(s->cdev[1].type, sizeof s->cdev[1].type, "cdsp:cdsp_sw");
    s->cdev[1].max_state = 4;
    s->cdev[1].state_ok = 1;
}

/* parse <text>: 0 and the error text in fc->err on refusal. */
static struct fanconfig fc;

static int parse(const char *text)
{
    memset(&fc, 0, sizeof fc);
    return fanconfig_parse_string(text, &fc);
}

/* refused <text> <needle> <what>: the parser must reject it and say why. */
static void refused(const char *text, const char *needle, const char *what)
{
    char line[160];

    if (parse(text) == 0) {
        snprintf(line, sizeof line, "%s: ACCEPTED, and should not be", what);
        T_FAIL("%s", line);
        return;
    }
    T_HAS(fc.err, needle, what);
}

static const char *good_curve =
    "< CURVE NAME=c >\n"
    "  ZONE cpuss-0-0\n"
    "  COOLING emc2305_fan\n"
    "  POINT 30C STATE 0\n"
    "  POINT 40C STATE 4\n"
    "  POINT 50C STATE 8\n"
    "  HYSTERESIS 4C\n"
    "  POLL 2s\n"
    "  FAILSAFE kernel\n"
    ">\n";

static void t_a_good_file_parses_and_validates(void)
{
    struct surface s;
    char err[256];

    /* The positive control: without it every refusal below could be a parser that rejects
     * everything. */
    board_surface(&s);
    T_EQ_INT(parse(good_curve), 0, "a well-formed curve parses");
    T_EQ_INT(fc.ncurve, 1, "one curve");
    T_EQ_INT(fc.c[0].npt, 3, "with its three points");
    T_EQ_INT(fc.c[0].hyst_mc, 4000, "its hysteresis in milli-C");
    T_EQ_INT(fc.c[0].poll_ms, 2000, "and its poll interval in ms");
    T_EQ_INT(fanconfig_validate(&fc, &s, err, sizeof err), 0, "and validates on this board");
}

static void t_the_block_structure_is_checked(void)
{
    refused("< CURVE NAME=c >\n< CURVE NAME=d >\n", "nested", "a block inside a block");
    refused("POINT 30C STATE 0\n", "outside a block", "a setting with no block");
    refused(">\n", "outside a block", "a close with nothing open");
    refused("< WHAT NAME=x >\n>\n", "expected", "a block of an unknown kind");
    refused("< CURVE >\n>\n", "needs NAME=", "a block with no name");
}

static void t_a_curve_needs_a_zone_and_two_points(void)
{
    refused("< CURVE NAME=c >\n  POINT 30C STATE 0\n  POINT 40C STATE 4\n>\n",
            "needs a ZONE", "a curve with no zone");
    refused("< CURVE NAME=c >\n  ZONE cpuss-0-0\n  POINT 30C STATE 0\n>\n",
            "at least 2 POINTs", "a curve with one point");
    refused("< CURVE NAME=c >\n  ZONE cpuss-0-0\n>\n",
            "at least 2 POINTs", "and a curve with none");
}

static void t_points_must_ascend(void)
{
    /* The engine walks the points in order and takes the first that the temperature has
     * passed, so a descending list silently means something other than it looks like. */
    refused("< CURVE NAME=c >\n"
            "  ZONE cpuss-0-0\n"
            "  POINT 50C STATE 8\n"
            "  POINT 40C STATE 4\n>\n",
            "ascending", "points listed hot to cold");
    refused("< CURVE NAME=c >\n"
            "  ZONE cpuss-0-0\n"
            "  POINT 40C STATE 4\n"
            "  POINT 40C STATE 6\n>\n",
            "not above the previous", "two points at the same temperature");
}

static void t_a_target_needs_its_own_settings(void)
{
    refused("< TARGET NAME=t >\n  TARGET 40C\n>\n", "needs a ZONE", "a target with no zone");
    refused("< TARGET NAME=t >\n  ZONE cpuss-0-0\n>\n",
            "needs a TARGET temperature", "a target with no temperature");

    /* Each setting belongs to one kind of block, and putting it in the other is a mistake
     * worth naming rather than ignoring: a HYSTERESIS in a TARGET block would read as
     * configured and do nothing. */
    refused("< CURVE NAME=c >\n  ZONE cpuss-0-0\n  DEADBAND 2C\n"
            "  POINT 30C STATE 0\n  POINT 40C STATE 4\n>\n",
            "DEADBAND belongs in a TARGET", "a deadband in a curve");
    refused("< TARGET NAME=t >\n  ZONE cpuss-0-0\n  TARGET 40C\n  HYSTERESIS 4C\n>\n",
            "HYSTERESIS belongs in a CURVE", "a hysteresis in a target");
    refused("< CURVE NAME=c >\n  ZONE cpuss-0-0\n  MAX_STATE 4\n"
            "  POINT 30C STATE 0\n  POINT 40C STATE 4\n>\n",
            "MAX_STATE belongs in a TARGET", "a max state in a curve");
    refused("< TARGET NAME=t >\n  ZONE cpuss-0-0\n  TARGET 40C\n  POINT 30C STATE 0\n>\n",
            "POINT belongs in a CURVE", "a point in a target");
}

static void t_the_controller_limits_are_enforced(void)
{
    /* Each of these is a number that would make the controller behave badly rather than
     * wrongly, which is why they are refusals and not clamps. */
    refused("< TARGET NAME=t >\n  ZONE cpuss-0-0\n  TARGET 40C\n  DEADBAND 0C\n>\n",
            "must be above 0C", "a deadband of zero");
    refused("< TARGET NAME=t >\n  ZONE cpuss-0-0\n  TARGET 40C\n  STEP_INTERVAL 1s\n>\n",
            "below 3s", "a step interval that outruns the board");
    refused("< CURVE NAME=c >\n  ZONE cpuss-0-0\n  POLL 10ms\n"
            "  POINT 30C STATE 0\n  POINT 40C STATE 4\n>\n",
            "below 200ms", "a poll interval that costs power for nothing");
    refused("< CURVE NAME=c >\n  ZONE cpuss-0-0\n  FAILSAFE sideways\n"
            "  POINT 30C STATE 0\n  POINT 40C STATE 4\n>\n",
            "kernel or full", "a failsafe that is neither");
}

static void t_the_airflow_block_is_bounded(void)
{
    /* Two bounds, each with its own reason: below the fan's MEASURED noise floor a delta
     * is indistinguishable from noise, and above 90% nothing could ever trigger it. */
    refused("< AIRFLOW >\n  DELTA 0%\n>\n", "noise floor",
            "an airflow delta below the measured noise floor");
    refused("< AIRFLOW >\n  DELTA 95%\n>\n", "absurd", "and one nothing could reach");
    refused("< AIRFLOW >\n  DEBOUNCE 99\n>\n", "out of range", "a debounce beyond the range");
    refused("< AIRFLOW >\n>\n< AIRFLOW >\n>\n", "only one AIRFLOW",
            "a second airflow block");
}

static void t_validation_is_against_this_board(void)
{
    struct surface s;
    char err[256];

    board_surface(&s);

    /* A zone or device this board does not have. The parser cannot know; the validator can,
     * and the alternative is a daemon that starts and then cannot find its sensor. */
    T_EQ_INT(parse("< CURVE NAME=c >\n  ZONE no-such-zone\n"
                   "  POINT 30C STATE 0\n  POINT 40C STATE 4\n>\n"), 0, "it parses");
    T_EQ_INT(fanconfig_validate(&fc, &s, err, sizeof err), -1,
             "a zone this board does not have is refused");
    T_HAS(err, "no thermal zone", "and says so");

    T_EQ_INT(parse("< CURVE NAME=c >\n  ZONE cpuss-0-0\n  COOLING no-such-fan\n"
                   "  POINT 30C STATE 0\n  POINT 40C STATE 4\n>\n"), 0, "it parses");
    T_EQ_INT(fanconfig_validate(&fc, &s, err, sizeof err), -1,
             "a cooling device this board does not have is refused");
    T_HAS(err, "no cooling device", "and says so");

    /* THE CEILING CHECK, against the named device and not against the fan. cdsp:cdsp_sw
     * has four states on this board; asking for eight is asking for a write the kernel
     * will reject, and finding that out at --verify time is the whole point. */
    T_EQ_INT(parse("< CURVE NAME=c >\n  ZONE cpuss-0-0\n  COOLING cdsp:cdsp_sw\n"
                   "  POINT 30C STATE 0\n  POINT 40C STATE 8\n>\n"), 0, "it parses");
    T_EQ_INT(fanconfig_validate(&fc, &s, err, sizeof err), -1,
             "a point above the NAMED device's maximum is refused");
    T_HAS(err, "max_state", "naming the limit it exceeds");

    /* The same curve on the fan, which does have eight states, must pass - or the check
     * above would be a blanket refusal rather than a comparison. */
    T_EQ_INT(parse("< CURVE NAME=c >\n  ZONE cpuss-0-0\n  COOLING emc2305_fan\n"
                   "  POINT 30C STATE 0\n  POINT 40C STATE 8\n>\n"), 0, "it parses");
    T_EQ_INT(fanconfig_validate(&fc, &s, err, sizeof err), 0,
             "while the same states on a ten-state device are fine");

    /* A non-zero point below MIN_STATE would be raised silently, so the file would not
     * describe what happens. */
    T_EQ_INT(parse("< CURVE NAME=c >\n  ZONE cpuss-0-0\n  COOLING emc2305_fan\n"
                   "  MIN_STATE 4\n  POINT 30C STATE 0\n  POINT 40C STATE 2\n>\n"), 0,
             "it parses");
    T_EQ_INT(fanconfig_validate(&fc, &s, err, sizeof err), -1,
             "a non-zero point below MIN_STATE is refused");
    T_HAS(err, "below MIN_STATE", "and says which point");

    /* A TARGET with no MAX_STATE on a device with fewer states than the default ceiling.
     * This is the defect the suite found on 2026-10-20: the ceiling check was skipped
     * entirely when MAX_STATE was absent, target_next() then bounded at a literal 10, and
     * the daemon commanded states a four-state device cannot take - every write refused by
     * the kernel, visible only in the journal. */
    T_EQ_INT(parse("< TARGET NAME=t >\n  ZONE cpuss-0-0\n  COOLING cdsp:cdsp_sw\n"
                   "  TARGET 40C\n>\n"), 0, "it parses");
    T_EQ_INT(fanconfig_validate(&fc, &s, err, sizeof err), -1,
             "a target with no MAX_STATE on a small device is refused");
    T_HAS(err, "set MAX_STATE explicitly", "and asks the operator for the ceiling");

    /* With a ceiling the device can take, the same target is fine. */
    T_EQ_INT(parse("< TARGET NAME=t >\n  ZONE cpuss-0-0\n  COOLING cdsp:cdsp_sw\n"
                   "  TARGET 40C\n  MAX_STATE 4\n>\n"), 0, "it parses");
    T_EQ_INT(fanconfig_validate(&fc, &s, err, sizeof err), 0,
             "given an explicit ceiling it can honour, it validates");

    /* And on the fan, whose maximum IS the default, no ceiling is needed. */
    T_EQ_INT(parse("< TARGET NAME=t >\n  ZONE cpuss-0-0\n  COOLING emc2305_fan\n"
                   "  TARGET 40C\n>\n"), 0, "it parses");
    T_EQ_INT(fanconfig_validate(&fc, &s, err, sizeof err), 0,
             "on a device with the default number of states, MAX_STATE is optional");

    /* MIN_STATE itself cannot exceed the device. */
    T_EQ_INT(parse("< CURVE NAME=c >\n  ZONE cpuss-0-0\n  COOLING cdsp:cdsp_sw\n"
                   "  MIN_STATE 9\n  POINT 30C STATE 0\n  POINT 40C STATE 2\n>\n"), 0,
             "it parses");
    T_EQ_INT(fanconfig_validate(&fc, &s, err, sizeof err), -1,
             "a MIN_STATE above the device's maximum is refused");
}

static void t_a_missing_file_is_not_a_parse_error(void)
{
    /* fan.config is optional - a profile with no fan strategy needs none - so an absent
     * file and a malformed one are different answers. */
    memset(&fc, 0, sizeof fc);
    T_EQ_INT(fanconfig_parse_file("/nonexistent/fan.config", &fc), -1,
             "an unreadable file is reported");
    T_EQ_INT(fc.ncurve, 0, "with nothing parsed");
}

int main(void)
{
    t_a_good_file_parses_and_validates();
    t_the_block_structure_is_checked();
    t_a_curve_needs_a_zone_and_two_points();
    t_points_must_ascend();
    t_a_target_needs_its_own_settings();
    t_the_controller_limits_are_enforced();
    t_the_airflow_block_is_bounded();
    t_validation_is_against_this_board();
    t_a_missing_file_is_not_a_parse_error();
    return t_summary("fanconfig");
}
