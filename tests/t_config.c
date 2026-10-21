// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer
// SPDX-FileContributor: Claude Opus 5 (Anthropic), paired on the behavioural suite
//
// The profile config: its two standing rules, and what it refuses. config_parse_string()
// means none of this needs a file, and a hand-built surface means none of it needs a board -
// which is the point, because "this config is applicable on this hardware" is the half that
// the installer trusts before it enables anything.

#include "../src/config.h"
#include "t.h"

#include <stdio.h>
#include <string.h>

/* A DELIBERATELY NARROW stand-in for policy0: two governors and three OPPs, not the board's
 * six and fifteen. The point is a surface small enough that a refusal can be attributed - a
 * governor or a frequency absent HERE is absent because this surface says so, not because
 * the real board happens to lack it. The OPPs are real values though, and not a continuum:
 * that is why frequencies are kHz and why an arbitrary one is refused rather than rounded. */
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

    /* A zone with two trips, so the ceiling rule has an envelope to be measured against,
     * and a GPU with two operating points. Both needed by the cross-field checks below,
     * which had no surface to run against and so had no test. */
    s->nzone = 1;
    s->zone[0].idx = 37;
    snprintf(s->zone[0].type, sizeof s->zone[0].type, "cpuss-0-0-thermal");
    s->zone[0].ntrips = 2;
    s->zone[0].trips[0].idx = 0;
    snprintf(s->zone[0].trips[0].type, sizeof s->zone[0].trips[0].type, "passive");
    s->zone[0].trips[0].temp_mc = 130000;
    s->zone[0].trips[1].idx = 2;
    snprintf(s->zone[0].trips[1].type, sizeof s->zone[0].trips[1].type, "active");
    s->zone[0].trips[1].temp_mc = 38000;

    s->gpu.present = 1;
    s->gpu.nopp = 2;
    s->gpu.opp_hz[0] = 479000000;
    s->gpu.opp_hz[1] = 877000000;
    s->gpu.ngov = 2;
    snprintf(s->gpu.governors[0], NAME_LEN, "simple_ondemand");
    snprintf(s->gpu.governors[1], NAME_LEN, "performance");
}

static void t_units_are_mandatory(void)
{
    struct pconfig c;

    /* Both kHz and Hz appear on this board, so a bare number is genuinely ambiguous.
     * Mixed units between fields have cost two real bugs here. */
    T_EQ_INT(config_parse_string(
        "< PROFILE ID=0 NAME=p DEFAULT >\n"
        "  CPU_POLICY 0 GOVERNOR schedutil MAX 2112000\n"
        ">\n", &c), -1, "a frequency without a unit is a parse error");
    T_EQ_INT(c.err_line, 2, "and the error names the line");

    T_EQ_INT(config_parse_string(
        "< PROFILE ID=0 NAME=p DEFAULT >\n"
        "  CPU_POLICY 0 GOVERNOR schedutil MAX 2112000kHz\n"
        ">\n", &c), 0, "with the unit it parses");
    T_EQ_INT(c.nprof, 1, "one profile");
    T_EQ_INT(c.prof[0].nact, 2, "and exactly the two settings named - unset is not a default");
}

static void t_default_and_lookup(void)
{
    struct pconfig c;
    const char *two =
        "< PROFILE ID=0 NAME=fast >\n"
        "  CPU_POLICY 0 GOVERNOR performance\n"
        ">\n"
        "< PROFILE ID=1 NAME=steady DEFAULT >\n"
        "  CPU_POLICY 0 GOVERNOR schedutil\n"
        ">\n";

    T_EQ_INT(config_parse_string(two, &c), 0, "two profiles parse");
    T_REQUIRE(config_default(&c) != NULL, "the DEFAULT profile is found");
    T_EQ_STR(config_default(&c)->name, "steady", "and it is the one marked");
    T_REQUIRE(config_find(&c, "fast") != NULL, "a profile is found by name");
    T_EQ_INT(config_find(&c, "1")->id, 1, "and by id");
    T_TRUE(config_find(&c, "nosuch") == NULL, "an unknown name is not found");

    /* No DEFAULT is the one way a well-formed config still leaves the daemon unable to
     * start: the unit carries no -m, so there would be nothing to apply. */
    T_EQ_INT(config_parse_string(
        "< PROFILE ID=0 NAME=only >\n  CPU_POLICY 0 GOVERNOR schedutil\n>\n", &c), 0,
        "a config with no DEFAULT still parses");
    T_TRUE(config_default(&c) == NULL, "but names no default");
}

static void t_validate_against_the_board(void)
{
    struct pconfig c;
    struct surface s;
    char err[ERRLEN];

    board_surface(&s);

    T_EQ_INT(config_parse_string(
        "< PROFILE ID=0 NAME=p DEFAULT >\n"
        "  CPU_POLICY 0 GOVERNOR schedutil MIN 902400kHz MAX 2112000kHz\n"
        ">\n", &c), 0, "a profile naming real OPPs parses");
    T_EQ_INT(config_validate(&c, &s, err, sizeof err), 0, "and validates on this surface");

    /* The measured case: `qcctl -m` with MAX 1800000kHz is refused before any write,
     * because the hardware would round it and the board would end up somewhere the
     * config does not describe. This is why apply's read-back is a backstop and not the
     * first line of defence. */
    T_EQ_INT(config_parse_string(
        "< PROFILE ID=0 NAME=p DEFAULT >\n"
        "  CPU_POLICY 0 GOVERNOR schedutil MAX 1800000kHz\n"
        ">\n", &c), 0, "an off-OPP frequency parses");
    T_EQ_INT(config_validate(&c, &s, err, sizeof err), -1, "and is refused by validation");
    T_HAS(err, "1800000", "the message names the frequency");

    T_EQ_INT(config_parse_string(
        "< PROFILE ID=0 NAME=p DEFAULT >\n  CPU_POLICY 0 GOVERNOR ondemand\n>\n", &c), 0,
        "a governor name parses");
    T_EQ_INT(config_validate(&c, &s, err, sizeof err), -1,
             "but a governor this policy does not have is refused");
    T_HAS(err, "ondemand", "and is named");

    T_EQ_INT(config_parse_string(
        "< PROFILE ID=0 NAME=p DEFAULT >\n  CPU_POLICY 7 GOVERNOR schedutil\n>\n", &c), 0,
        "a policy number parses");
    T_EQ_INT(config_validate(&c, &s, err, sizeof err), -1,
             "but a policy this board does not expose is refused");

    /* The vendor's own defaults file names policy7, which does not exist here. That is the
     * shape of failure this check exists for. */
    T_HAS(err, "policy7", "naming the policy, as the vendor file's bug would");
}

static void t_fan_mode_and_curve_must_agree(void)
{
    struct pconfig c;
    struct surface s;
    char err[ERRLEN];

    memset(&s, 0, sizeof s);        /* FAN actions touch no hardware to validate against */

    T_EQ_INT(config_parse_string(
        "< PROFILE ID=0 NAME=p DEFAULT >\n  FAN MODE curve\n>\n", &c), 0,
        "FAN MODE curve alone parses");
    T_EQ_INT(config_validate(&c, &s, err, sizeof err), -1,
             "but is refused without a FAN CURVE: there would be no strategy to run");

    T_EQ_INT(config_parse_string(
        "< PROFILE ID=0 NAME=p DEFAULT >\n  FAN CURVE desktop\n>\n", &c), 0,
        "FAN CURVE alone parses");
    T_EQ_INT(config_validate(&c, &s, err, sizeof err), -1,
             "and is refused without FAN MODE curve: it would have no effect");

    T_EQ_INT(config_parse_string(
        "< PROFILE ID=0 NAME=p DEFAULT >\n  FAN MODE curve\n  FAN CURVE desktop\n>\n", &c),
        0, "both together parse");
    T_EQ_INT(config_validate(&c, &s, err, sizeof err), 0, "and validate");
}

static void t_disruptive_is_per_profile(void)
{
    struct pconfig c;
    const struct action *d[8];
    /* 'drastic' carries a harmless setting BEFORE the gated one on purpose: with the
     * disruptive action first, act[0] is also the matching action, and an identity check
     * cannot tell a function that returns the match from one that returns the first entry. */
    const char *mixed =
        "< PROFILE ID=0 NAME=ordinary DEFAULT >\n"
        "  CPU_ONLINE 1 0\n"
        ">\n"
        "< PROFILE ID=1 NAME=drastic >\n"
        "  CPU_POLICY 0  GOVERNOR schedutil\n"
        "  CPU_ONLINE 0 0\n"
        ">\n";

    /* Offlining cpu0 WORKS and re-onlines cleanly; it is gated because it is drastic.
     * The gate used to be collected across the whole FILE, so a setting in a profile
     * nobody applies demanded --allow-disruptive from everybody. */
    T_EQ_INT(config_parse_string(mixed, &c), 0, "both profiles parse");
    T_EQ_INT((int)config_disruptive(config_find(&c, "ordinary"), d, 8), 0,
             "offlining cpu1 is not gated");
    T_EQ_INT((int)config_disruptive(config_find(&c, "drastic"), d, 8), 1,
             "offlining cpu0, the boot CPU, is");
    /* Identity, not target_id: act_is_disruptive() only ever matches cpu0, so asserting
     * target_id == 0 was true by construction and would hold even if the function handed
     * back a different action than the one it counted. The line number is unique to the
     * action in the 'drastic' profile. */
    T_EQ_INT(d[0]->line, 6, "and the action handed back is the gated one, not the first");
    T_EQ_INT(d[0]->kind, ACT_CPU_ONLINE, "of the kind that is gated");
    T_EQ_INT((int)d[0]->value, 0, "asking for cpu0 to go offline");
    T_EQ_INT((int)config_disruptive(NULL, d, 8), 0, "no profile, nothing gated");

    /* The count may be asked for without the list. */
    T_EQ_INT((int)config_disruptive(config_find(&c, "drastic"), NULL, 0), 1,
             "the count alone is available");
}

static void t_cross_field_rules_are_checked_per_profile(void)
{
    struct pconfig c;
    struct surface s;
    char err[ERRLEN];

    /* These four refusals exist because the KERNEL rejects the combination, not because the
     * value is out of range - so each is invisible to a per-action check and every one was
     * unreached. The UCLAMP one carries its own history in config.c: "this check existed
     * for the cpufreq pair above and not for this one, which is how 'lowpower' shipped
     * unappliable while --verify called it applicable". */
    board_surface(&s);

    T_EQ_INT(config_parse_string(
        "< PROFILE ID=0 NAME=p DEFAULT >\n"
        "  UCLAMP MIN 1024  MAX 512\n>\n", &c), 0, "an inverted uclamp pair parses");
    T_EQ_INT(config_validate(&c, &s, err, sizeof err), -1, "but does not validate");
    T_HAS(err, "UCLAMP MIN", "and the refusal names the pair");
    T_HAS(err, "EINVAL", "and why the kernel refuses it");

    /* The same pair the right way round must pass, or the check above is a blanket
     * refusal of any profile that sets both. */
    T_EQ_INT(config_parse_string(
        "< PROFILE ID=0 NAME=p DEFAULT >\n"
        "  UCLAMP MIN 512  MAX 1024\n>\n", &c), 0, "the pair the right way round parses");
    T_EQ_INT(config_validate(&c, &s, err, sizeof err), 0, "and validates");

    T_EQ_INT(config_parse_string(
        "< PROFILE ID=0 NAME=p DEFAULT >\n"
        "  CPU_POLICY 0  MIN 2112000kHz  MAX 902400kHz\n>\n", &c), 0,
        "an inverted cpufreq pair parses");
    T_EQ_INT(config_validate(&c, &s, err, sizeof err), -1, "but does not validate");
    T_HAS(err, "exceeds MAX", "and says which way round it is");

    T_EQ_INT(config_parse_string(
        "< PROFILE ID=0 NAME=p DEFAULT >\n"
        "  GPU  MIN 877000000Hz  MAX 479000000Hz\n>\n", &c), 0,
        "an inverted GPU pair parses");
    T_EQ_INT(config_validate(&c, &s, err, sizeof err), -1, "but does not validate");

    /* A trip may be lowered, or raised within the zone's envelope, never above it: it is a
     * safety feature, and a profile that raises one is a profile that disables protection
     * the board shipped with. */
    T_EQ_INT(config_parse_string(
        "< PROFILE ID=0 NAME=p DEFAULT >\n"
        "  THERMAL_ZONE cpuss-0-0  TRIP 2 TEMP 45C\n>\n", &c), 0,
        "lowering a trip inside the envelope parses");
    T_EQ_INT(config_validate(&c, &s, err, sizeof err), 0, "and validates");

    T_EQ_INT(config_parse_string(
        "< PROFILE ID=0 NAME=p DEFAULT >\n"
        "  THERMAL_ZONE cpuss-0-0  TRIP 2 TEMP 200C\n>\n", &c), 0,
        "raising one above the envelope parses");
    T_EQ_INT(config_validate(&c, &s, err, sizeof err), -1, "but is refused");
    T_HAS(err, "must not be raised", "because thermal protections are not configuration");

    /* And a trip index the zone does not have. */
    T_EQ_INT(config_parse_string(
        "< PROFILE ID=0 NAME=p DEFAULT >\n"
        "  THERMAL_ZONE cpuss-0-0  TRIP 9 TEMP 40C\n>\n", &c), 0, "it parses");
    T_EQ_INT(config_validate(&c, &s, err, sizeof err), -1,
             "a trip point this zone does not have is refused");
    T_HAS(err, "no trip point", "and named");
}

int main(void)
{
    t_units_are_mandatory();
    t_default_and_lookup();
    t_validate_against_the_board();
    t_fan_mode_and_curve_must_agree();
    t_disruptive_is_per_profile();
    t_cross_field_rules_are_checked_per_profile();
    return t_summary("config");
}
