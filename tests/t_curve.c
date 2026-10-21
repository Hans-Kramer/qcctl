// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer
// SPDX-FileContributor: Claude Opus 5 (Anthropic), paired on the behavioural suite
//
// The two fan strategies and the ceiling over both. These are pure functions - no board,
// no fixture - and they decide what the fan does, so their semantics are pinned here
// rather than rediscovered on hardware. The curve below is the shipped `desktop` one,
// whose points were trimmed to the measured thermal envelope on 2026-10-16.

#include "../src/fancurve.h"
#include "t.h"

#include <string.h>

/* The shipped desktop curve: off to 38C, then 4, 6, 8, 10 across the band this board
 * actually reaches. HYSTERESIS 4C, MIN_STATE 2. */
static struct curve desktop(void)
{
    struct curve c;

    memset(&c, 0, sizeof c);
    snprintf(c.name, sizeof c.name, "desktop");
    c.npt = 5;
    c.pt[0].temp_mc = 30000; c.pt[0].state = 0;
    c.pt[1].temp_mc = 38000; c.pt[1].state = 4;
    c.pt[2].temp_mc = 43000; c.pt[2].state = 6;
    c.pt[3].temp_mc = 45000; c.pt[3].state = 8;
    c.pt[4].temp_mc = 47000; c.pt[4].state = 10;
    c.hyst_mc = 4000;
    c.min_state = 2;
    return c;
}

static void t_curve_rises_at_once(void)
{
    struct curve c = desktop();

    /* Rising crosses a threshold immediately: no hysteresis on the way up, because the
     * board is already hotter than the point asked for. */
    T_EQ_INT(curve_next(&c, 29000, 0), 0, "below every point the fan stays off");
    T_EQ_INT(curve_next(&c, 37900, 0), 0, "just under the first active point, still off");
    T_EQ_INT(curve_next(&c, 38000, 0), 4, "at 38C it goes straight to state 4");
    T_EQ_INT(curve_next(&c, 43000, 4), 6, "and to 6 at 43C");
    T_EQ_INT(curve_next(&c, 47000, 6), 10, "a jump of two points is taken in one step");
}

static void t_curve_falls_only_past_the_hysteresis(void)
{
    struct curve c = desktop();

    /* Falling is evaluated against temp + hysteresis, so the state held at 43C is kept
     * until the real temperature is a full 4C below the point that set it. 41C still maps
     * to 8 once the hysteresis is added, which is not lower than 6, so it holds. */
    T_EQ_INT(curve_next(&c, 41000, 6), 6, "6 holds at 41C: inside the hysteresis band");
    T_EQ_INT(curve_next(&c, 38000, 6), 4, "and drops to 4 once 38C+4C maps below it");
    T_EQ_INT(curve_next(&c, 33000, 4), 0, "and to off once 33C+4C is under the first point");

    /* The measured consequence on this board: it idles near 36.5C with the fan off, which
     * is inside the band, so after a load spike the fan holds its state for a while. That
     * is the curve working, not a fault. */
    T_EQ_INT(curve_next(&c, 36000, 4), 4, "at an idle 36C a running fan keeps its state");
}

static void t_curve_min_state_leaves_zero_legal(void)
{
    struct curve c = desktop();

    /* MIN_STATE is the lowest NON-ZERO state: state 1's tachometer misreads, so a curve
     * must never ask for it, but off stays available. */
    c.pt[1].state = 1;
    T_EQ_INT(curve_next(&c, 38000, 0), 2, "a point asking for state 1 is raised to MIN_STATE");
    T_EQ_INT(curve_next(&c, 29000, 2), 0, "and 0 is still reachable below the first point");
}

static void t_curve_with_no_points_holds(void)
{
    struct curve c;

    memset(&c, 0, sizeof c);
    c.min_state = 2;
    T_EQ_INT(curve_next(&c, 50000, 7), 7, "a curve with no points changes nothing");
}

static void t_safety_ceiling(void)
{
    /* The 85C ceiling belongs to neither strategy and overrides both. It is a
     * user-facing limit, not a hardware one: this zone's own passive trips are at 130C
     * and 135C. */
    T_EQ_INT(fan_safety_active(84999, 0, 10), 0, "just under the ceiling, nothing is forced");
    T_EQ_INT(fan_safety_active(85000, 0, 10), 1, "at the ceiling it engages");
    T_EQ_INT(fan_safety_clamp(2, 0, 85000, 10), 10, "and a curve asking for 2 gets maximum");
    T_EQ_INT(fan_safety_clamp(2, 0, 84000, 10), 2, "below it the curve decides");

    /* Released with hysteresis so it cannot flap at exactly the ceiling: while already at
     * maximum it stays engaged down to 82C. */
    T_EQ_INT(fan_safety_active(83000, 10, 10), 1, "at 83C it holds while already at maximum");
    T_EQ_INT(fan_safety_active(83000, 9, 10), 0, "but does not engage from below maximum");
    T_EQ_INT(fan_safety_active(81999, 10, 10), 0, "and releases under 82C");
}

/* held40 as shipped: target 40C, deadband 2C after the 2026-10-16 measurement, min_state 2,
 * one step per 15s. */
static struct target held40(void)
{
    struct target t;

    memset(&t, 0, sizeof t);
    snprintf(t.name, sizeof t.name, "held40");
    t.target_mc = 40000;
    t.deadband_mc = 2000;
    t.min_state = 2;
    t.step_ms = 15000;
    return t;
}

static void t_target_deadband(void)
{
    struct target t = held40();
    struct target_state st;

    /* The band is why DEADBAND went from 1C to 2C: no fan state holds a loaded board below
     * 40.9C, so a 1C band had the controller stepping to maximum for the last 0.2C. */
    memset(&st, 0, sizeof st);
    T_EQ_INT(target_next(&t, &st, 41200, 4, 1000), 4, "41.2C is inside 40C +/- 2C: hold");
    memset(&st, 0, sizeof st);
    T_EQ_INT(target_next(&t, &st, 38500, 4, 1000), 4, "and so is 38.5C");
}

static void t_target_steps_one_at_a_time(void)
{
    struct target t = held40();
    struct target_state st;

    /* Too hot and not already falling: one state, and a step up from off goes to MIN_STATE
     * rather than to the state whose tachometer lies. */
    memset(&st, 0, sizeof st);
    T_EQ_INT(target_next(&t, &st, 45000, 0, 1000), 2, "from off it steps to MIN_STATE");

    memset(&st, 0, sizeof st);
    st.have_prev = 1; st.prev_temp_mc = 44000;      /* rising */
    T_EQ_INT(target_next(&t, &st, 45000, 4, 1000), 5, "rising and too hot: one state up");

    /* Anti-windup: if it is already cooling, the cooling in place is working. */
    memset(&st, 0, sizeof st);
    st.have_prev = 1; st.prev_temp_mc = 46000;      /* falling */
    T_EQ_INT(target_next(&t, &st, 45000, 4, 1000), 4, "too hot but already falling: hold");

    /* And the mirror: do not remove cooling while the temperature is still climbing. */
    memset(&st, 0, sizeof st);
    st.have_prev = 1; st.prev_temp_mc = 36000;      /* rising */
    T_EQ_INT(target_next(&t, &st, 37000, 4, 1000), 4, "too cold but still rising: hold");

    memset(&st, 0, sizeof st);
    st.have_prev = 1; st.prev_temp_mc = 38000;      /* falling */
    T_EQ_INT(target_next(&t, &st, 37000, 4, 1000), 3, "too cold and falling: one state down");
}

static void t_target_skips_the_untrustworthy_band(void)
{
    struct target t = held40();
    struct target_state st;

    memset(&st, 0, sizeof st);
    st.have_prev = 1; st.prev_temp_mc = 38000;
    T_EQ_INT(target_next(&t, &st, 37000, 2, 1000), 0,
             "stepping down from MIN_STATE goes to off, not to state 1");
}

static void t_target_rate_limit(void)
{
    struct target t = held40();
    struct target_state st;

    memset(&st, 0, sizeof st);
    st.have_prev = 1; st.prev_temp_mc = 44000;
    T_EQ_INT(target_next(&t, &st, 45000, 4, 100000), 5, "the first step is allowed");
    T_EQ_INT(st.last_step_ms, 100000, "and the clock is recorded");

    /* STEP_INTERVAL is the board's thermal time constant, not the poll rate: stepping every
     * poll crossed the whole range in 20s and saturated before any feedback arrived. */
    st.prev_temp_mc = 45000;
    T_EQ_INT(target_next(&t, &st, 46000, 5, 110000), 5, "a step inside the interval is refused");
    T_EQ_INT(target_next(&t, &st, 46000, 5, 115001), 6, "and allowed once it has elapsed");
}

static void t_target_saturates_at_the_top(void)
{
    struct target t = held40();
    struct target_state st;

    /* With MAX_STATE unset, target_next() bounds at TARGET_TOP_DEFAULT - it is handed no
     * surface and no cooling device, so it cannot know the real ceiling. That default is
     * this board's FAN maximum and is wrong for every smaller cooling device on the board,
     * which is why fanconfig_validate() now refuses a target with no MAX_STATE on a device
     * with fewer states, and why fanown_set() clamps to the device as a backstop. Both were
     * missing until 2026-10-21; see tests/t_fanconfig.c and tests/t_fanown.c.
     *
     * At the top there is nothing left to add, and the daemon reports the target as
     * unreachable instead. */
    memset(&st, 0, sizeof st);
    st.have_prev = 1; st.prev_temp_mc = 44000;
    T_EQ_INT(target_next(&t, &st, 45000, 10, 1000), 10, "at maximum it stays at maximum");
}

static void t_every_edge_is_probed_at_the_edge(void)
{
    struct curve c = desktop();
    struct target t = held40();
    struct target_state st;

    /* The suite pinned every one of these behaviours and not one of its boundaries, so six
     * independent off-by-one mutations survived it. A hysteresis of the wrong WIDTH, a
     * deadband that became an open interval, a step interval off by a millisecond: each is
     * a number in a config file meaning something other than it says.
     *
     * The curve's falling edge for state 6 is its own point (43000) minus the hysteresis
     * (4000) = 39000, and the rule is "falling needs the FULL band", so 39000 holds. */
    T_EQ_INT(curve_next(&c, 39000, 6), 6, "exactly at the falling edge, state 6 holds");
    T_EQ_INT(curve_next(&c, 38999, 6), 4, "one milli-degree below it, it drops");

    /* The same at the next step down: 38000 - 4000 = 34000. */
    T_EQ_INT(curve_next(&c, 34000, 4), 4, "exactly at the next edge, state 4 holds");
    T_EQ_INT(curve_next(&c, 33999, 4), 0, "and one below, it drops to off");

    /* MIN_STATE on the FALLING path, which was pinned only going up: with a curve whose
     * second point asks for 1, falling must still be raised to the minimum. A fan state
     * below MIN_STATE is one the operator declared untrustworthy on this hardware. */
    {
        struct curve m = desktop();

        m.pt[1].state = 1;
        T_EQ_INT(curve_next(&m, 35000, 4), 2,
                 "falling into a sub-minimum point is raised to MIN_STATE");
        T_EQ_INT(curve_next(&m, 40000, 0), 2, "as it is when rising into one");
    }

    /* Coming down from INSIDE the untrustworthy band, 1..MIN_STATE-1. Below the first
     * point the curve wants 0, and 0 is a legal answer however MIN_STATE is set - so from
     * state 3 the fan goes off, and MIN_STATE does not hold it at 2 on the way. The rule
     * raises a non-zero target, not every target: that is the distinction, and asserting
     * the other reading of it is how this check first failed. */
    T_EQ_INT(curve_next(&c, 20000, 3), 0, "below the first point, state 3 goes to off");
    T_EQ_INT(curve_next(&c, 20000, 2), 0, "as does the minimum itself");
    {
        /* And where the curve DOES ask for a state inside the band, every starting state
         * is raised to the minimum rather than allowed through. */
        struct curve m = desktop();

        m.pt[1].state = 1;
        T_EQ_INT(curve_next(&m, 38000, 3), 2, "a point inside the band is raised from 3");
        T_EQ_INT(curve_next(&m, 38000, 1), 2, "and from 1");
    }

    /* The deadband is INCLUSIVE: exactly on it is still inside. An open interval would
     * make DEADBAND 2C mean 1.999C, and the fan would hunt at the edge. */
    memset(&st, 0, sizeof st);
    st.have_prev = 1; st.prev_temp_mc = 42000;
    T_EQ_INT(target_next(&t, &st, 42000, 4, 100000), 4,
             "exactly at the deadband's edge, nothing moves");
    memset(&st, 0, sizeof st);
    st.have_prev = 1; st.prev_temp_mc = 38000;
    T_EQ_INT(target_next(&t, &st, 38000, 4, 100000), 4, "and at the other edge");
    memset(&st, 0, sizeof st);
    st.have_prev = 1; st.prev_temp_mc = 42000;
    T_EQ_INT(target_next(&t, &st, 42001, 4, 100000), 5,
             "one milli-degree outside it, the controller steps");

    /* STEP_INTERVAL is also inclusive: exactly the interval has elapsed, so the step is
     * allowed. Off by one here makes a 15s interval mean 15.001s, forever. */
    memset(&st, 0, sizeof st);
    st.have_prev = 1; st.prev_temp_mc = 44000; st.last_step_ms = 1000;
    T_EQ_INT(target_next(&t, &st, 45000, 4, 1000 + 15000), 5,
             "exactly at the step interval, a step is allowed");
    memset(&st, 0, sizeof st);
    st.have_prev = 1; st.prev_temp_mc = 44000; st.last_step_ms = 1000;
    T_EQ_INT(target_next(&t, &st, 45000, 4, 1000 + 14999), 4,
             "one millisecond before it, nothing moves");

    /* The safety ceiling's release point: 85000 - 3000 = 82000, and at the maximum state
     * the clamp is still engaged there. A release one degree early is a fan that drops out
     * of full cooling while the board is still at 82C. */
    T_EQ_INT(fan_safety_active(82000, 10, 10), 1,
             "exactly at the release point, the safety clamp is still engaged");
    T_EQ_INT(fan_safety_active(81999, 10, 10), 0, "and one below it, released");
}

int main(void)
{
    t_curve_rises_at_once();
    t_curve_falls_only_past_the_hysteresis();
    t_curve_min_state_leaves_zero_legal();
    t_curve_with_no_points_holds();
    t_safety_ceiling();
    t_target_deadband();
    t_target_steps_one_at_a_time();
    t_target_skips_the_untrustworthy_band();
    t_target_rate_limit();
    t_target_saturates_at_the_top();
    t_every_edge_is_probed_at_the_edge();
    return t_summary("curve");
}
