// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer
// SPDX-FileContributor: Claude Opus 5 (Anthropic), paired on the behavioural suite
//
// The fan's measured full scale, and judging against the baseline that holds it.
//
// `fan_rpm_max` is published for consumers to scale rpm against, so where it comes from
// has to be pinned: the highest state that actually HAS a reading, which is not the same
// as `max_state`. The reference capture of 2026-10-25 is the fixture.

#include "../src/fanbase.h"
#include "t.h"

#include <string.h>

/* The reference board's capture: states 2..10, max_state 10, state 10 at 5237 rpm. */
static struct fanbase reference(void)
{
    static const long mean[] = { 0, 0, 1386, 2077, 2667, 3167,
                                 3617, 4112, 4465, 4856, 5237 };
    struct fanbase fb;
    int st;

    memset(&fb, 0, sizeof fb);
    fb.max_state = 10;
    for (st = FANBASE_MIN_STATE; st <= FANBASE_MAX_STATE; st++) {
        fb.have[st] = 1;
        fb.mean[st] = mean[st];
        fb.n[st] = 4;
    }
    return fb;
}

static void t_top_is_the_highest_baselined_state(void)
{
    struct fanbase fb = reference();
    int state = -1;

    T_EQ_INT(fanbase_top(&fb, &state), 5237, "the reference fan's full scale");
    T_EQ_INT(state, 10, "and the state it was measured at");
}

/* The whole reason this reads `have[]` and not `max_state`. A capture interrupted
 * part-way leaves max_state at what the cooling device reported while the upper states
 * hold nothing, and 0 rpm read off an empty slot is not a full scale. */
static void t_a_partial_capture_reports_what_it_reached(void)
{
    struct fanbase fb = reference();
    int state = -1;

    fb.have[10] = fb.have[9] = fb.have[8] = 0;
    fb.mean[10] = fb.mean[9] = fb.mean[8] = 0;
    T_EQ_INT(fb.max_state, 10, "max_state still says what the device reported");
    T_EQ_INT(fanbase_top(&fb, &state), 4112, "but the full scale is where it stopped");
    T_EQ_INT(state, 7, "at the highest state it actually measured");
}

/* A slot flagged present with a nonsense mean is not a measurement. */
static void t_a_zero_mean_is_not_a_reading(void)
{
    struct fanbase fb = reference();
    int state = -1;

    fb.mean[10] = 0;
    T_EQ_INT(fanbase_top(&fb, &state), 4856, "a zero mean is skipped");
    T_EQ_INT(state, 9, "for the state below it");
}

/* No capture means no full scale, and saying 0 rpm would be a lie about the fan. */
static void t_an_empty_baseline_has_no_full_scale(void)
{
    struct fanbase fb;
    int state = -1;

    memset(&fb, 0, sizeof fb);
    fb.max_state = 10;
    T_EQ_INT(fanbase_top(&fb, &state), 0, "no readings, no full scale");
    T_EQ_INT(state, -1, "and no state to attribute one to");
    T_EQ_INT(fanbase_top(NULL, &state), 0, "nor from no baseline at all");
    T_EQ_INT(state, -1, "still no state");
}

/* States 0 and 1 are excluded by capture and must stay excluded here: state 0 is a
 * stopped fan and state 1's tachometer misreads, so neither is a full scale however it
 * came to be in the struct. */
static void t_the_unusable_low_states_are_never_the_full_scale(void)
{
    struct fanbase fb;
    int state = -1;

    memset(&fb, 0, sizeof fb);
    fb.max_state = 10;
    fb.have[0] = fb.have[1] = 1;
    fb.mean[0] = 0;
    fb.mean[1] = 3172;    /* the measured misread */
    T_EQ_INT(fanbase_top(&fb, &state), 0, "state 1 is not a full scale");
    T_EQ_INT(state, -1, "and is not attributed one");
}

/* The judgement the full scale sits beside, pinned at its edges.
 *
 * The deviation is integer percent, truncated toward zero, and the delta is inclusive -
 * so against state 4's 2667 rpm at a delta of 8 the verdict turns over at 2908 rpm up
 * and 2426 rpm down, not at the arithmetic 8%. Pinned at the exact crossing because an
 * off-by-one here is the difference between a fault reported and a fault missed. */
static void t_judge_brackets_the_baseline(void)
{
    struct fanbase fb = reference();
    long dev = 999;

    T_EQ_INT(fanbase_judge(&fb, 4, 2667, 8, &dev), FANBASE_OK, "dead on the baseline");
    T_EQ_INT(dev, 0, "no deviation");

    T_EQ_INT(fanbase_judge(&fb, 4, 2907, 8, &dev), FANBASE_OK,
             "the last rpm that still truncates to 8% is within");
    T_EQ_INT(dev, 8, "truncation, not rounding");
    T_EQ_INT(fanbase_judge(&fb, 4, 2908, 8, &dev), FANBASE_RESTRICTED,
             "one rpm further is an obstructed inlet");
    T_EQ_INT(dev, 9, "and the deviation is positive");

    T_EQ_INT(fanbase_judge(&fb, 4, 2427, 8, &dev), FANBASE_OK,
             "the same edge below the baseline");
    T_EQ_INT(dev, -8, "truncated toward zero, not away from it");
    T_EQ_INT(fanbase_judge(&fb, 4, 2426, 8, &dev), FANBASE_DEGRADED,
             "one rpm further is a fan not delivering");
    T_EQ_INT(dev, -9, "and the deviation is negative");
}

/* Silence must not read as health - the reason the verdict has an UNKNOWN at all. */
static void t_judge_refuses_what_it_cannot_judge(void)
{
    struct fanbase fb = reference();
    long dev = 999;

    T_EQ_INT(fanbase_judge(&fb, 1, 3172, 8, &dev), FANBASE_UNKNOWN,
             "below the lowest usable state");
    T_EQ_INT(fanbase_judge(&fb, 11, 5237, 8, &dev), FANBASE_UNKNOWN,
             "above the highest");
    T_EQ_INT(fanbase_judge(&fb, 4, 0, 8, &dev), FANBASE_UNKNOWN,
             "no reading is not a reading of zero");
    fb.have[4] = 0;
    T_EQ_INT(fanbase_judge(&fb, 4, 2667, 8, &dev), FANBASE_UNKNOWN,
             "and a state with no baseline entry");
}

static void t_every_verdict_has_a_name(void)
{
    T_EQ_STR(fanbase_verdict_str(FANBASE_UNKNOWN), "unknown", "unknown");
    T_EQ_STR(fanbase_verdict_str(FANBASE_OK), "ok", "ok");
    T_EQ_STR(fanbase_verdict_str(FANBASE_RESTRICTED), "restricted", "restricted");
    T_EQ_STR(fanbase_verdict_str(FANBASE_DEGRADED), "degraded", "degraded");
}

int main(void)
{
    t_top_is_the_highest_baselined_state();
    t_a_partial_capture_reports_what_it_reached();
    t_a_zero_mean_is_not_a_reading();
    t_an_empty_baseline_has_no_full_scale();
    t_the_unusable_low_states_are_never_the_full_scale();
    t_judge_brackets_the_baseline();
    t_judge_refuses_what_it_cannot_judge();
    t_every_verdict_has_a_name();
    return t_summary("fanbase");
}
