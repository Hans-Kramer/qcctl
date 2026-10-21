// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer
// SPDX-FileContributor: Claude Opus 5 (Anthropic), paired on the behavioural suite
//
// The contract with qcstats, which is the one thing neither repository can test alone.
//
// qcctl reads that stream with strstr, so field spelling and order are load-bearing, and the
// schema version is two hand-synced constants with no build-time check. A field renamed on
// the producer's side makes this consumer read "not measured" forever - a valid answer in
// this schema, and therefore completely silent.
//
// By default this asserts against tests/fixtures/stream/sample.ndjson, a captured stream, so
// the test runs with the sibling repo absent. That default run cannot detect producer drift
// at all: the fixture is frozen, so it always agrees with itself. CI's CONTRACT job runs this
// a second time with a FRESHLY generated line - argv[1] - and that run is the one that catches
// the producer moving. The stage job only assembles a package; it never invokes this test.
//
//   ./t_contract [path-to-ndjson]

#define _GNU_SOURCE
#include "../src/stats.h"
#include "t.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_STREAM "tests/fixtures/stream/sample.ndjson"

static char meta[STATS_LINE_MAX];
static char sample[STATS_LINE_MAX];

/* Split a captured stream into its meta line and its last sample line, which is what the
 * daemon sees: one meta line at startup, then samples. */
static int load(const char *path)
{
    char line[STATS_LINE_MAX];
    FILE *f = fopen(path, "r");

    if (!f) {
        fprintf(stderr, "t_contract: cannot open %s\n", path);
        return -1;
    }
    meta[0] = sample[0] = '\0';
    while (fgets(line, sizeof line, f)) {
        if (strstr(line, "\"meta\":"))
            snprintf(meta, sizeof meta, "%s", line);
        else if (line[0] == '{')
            snprintf(sample, sizeof sample, "%s", line);
    }
    fclose(f);
    if (!sample[0]) {
        fprintf(stderr, "t_contract: no sample line in %s\n", path);
        return -1;
    }
    return 0;
}

/* The producer's own constant, read out of the sibling repository's header at run time.
 * -1 when the sibling is not checked out beside this one.
 *
 * Reading it rather than #including it keeps this test buildable alone, which it has to be:
 * the default run exists so the suite works with qcstats absent. */
static int producer_schema_constant(void)
{
    static const char *const cands[] = {
        "../qcstats/src/qcstats.h", "qcstats/src/qcstats.h",
    };
    char line[256];

    for (size_t i = 0; i < sizeof cands / sizeof cands[0]; i++) {
        FILE *f = fopen(cands[i], "r");

        if (!f)
            continue;
        while (fgets(line, sizeof line, f)) {
            char *at = strstr(line, "#define QCSTATS_JSON_SCHEMA");

            if (at) {
                int v = (int)strtol(at + strlen("#define QCSTATS_JSON_SCHEMA"), NULL, 10);
                fclose(f);
                return v;
            }
        }
        fclose(f);
    }
    return -1;
}

static void t_the_two_constants_agree(void)
{
    int producer = producer_schema_constant();

    /* THE constant pair this whole file exists for, and it was compared nowhere in C: the
     * schema check below reads the version out of a FROZEN fixture, so it compares
     * STATS_SCHEMA_V against a captured 1 and agrees with itself forever. The real
     * comparison lived only in a `sed` step in the workflow, which means a developer
     * bumping one side locally got a green `make test`.
     *
     * When the sibling repo is absent the check cannot run, and says so out loud rather
     * than passing quietly - a skipped check that prints nothing is indistinguishable from
     * a check that held. */
    if (producer < 0) {
        printf("    %-18s NOTE: qcstats not checked out beside this repo, so the\n",
               "contract");
        printf("    %-18s       two schema constants were NOT compared\n", "");
        return;
    }
    T_EQ_INT(producer, STATS_SCHEMA_V,
             "the producer's QCSTATS_JSON_SCHEMA and this build's STATS_SCHEMA_V agree");
}

static void t_the_schema_version_agrees(void)
{
    /* The producer writes its QCSTATS_JSON_SCHEMA into every line; this build reads
     * STATS_SCHEMA_V. They are separate constants in separate repositories, kept equal by
     * hand. A line of any other version counts as NO READING rather than a possibly
     * misread one, so a silent disagreement would stop the daemon reading temperatures
     * while it carried on reporting itself healthy. */
    T_EQ_INT(stats_line_version(sample), STATS_SCHEMA_V,
             "the stream's schema version is the one this build reads");
    T_EQ_INT(stats_line_version(meta), STATS_SCHEMA_V, "and the meta line carries it too");
    T_EQ_INT(stats_line_version("{\"t\":1.0}"), -1, "a line without a version says -1");
    T_EQ_INT(stats_line_version("not json at all"), -1, "and so does one that is not ours");
}

/* Read one policy object straight out of the raw line, independently of stats.c.
 *
 * Each policy object this producer emits begins with {"label":" and the next one begins the
 * same way, so the region between two of them is exactly one policy's bytes. Reading the
 * frequency from inside THAT region is what makes "pol[0].mhz is 1017" an assertion about
 * policy 0 rather than about the line containing 1017 somewhere.
 *
 * Returns 0 if the objects cannot be located at all - which is itself the answer when the
 * producer has reordered the keys, because then nothing in the array starts with the label.
 */
static int policy_object(const char *line, int idx, char *label, size_t llen, long *mhz)
{
    static const char *const START = "{\"label\":\"";
    const char *p = strstr(line, "\"cpu\":[");
    const char *end;
    const char *at;

    if (!p)
        return 0;
    for (int i = 0; i <= idx; i++) {
        p = strstr(p + 1, START);
        if (!p)
            return 0;
    }
    end = strstr(p + 1, START);
    if (!end)
        end = p + strlen(p);

    at = p + strlen(START);
    {
        size_t n = strcspn(at, "\"");
        if (n >= llen)
            return 0;
        memcpy(label, at, n);
        label[n] = '\0';
    }
    at = strstr(p, "\"mhz\":");
    if (!at || at >= end)
        return 0;
    *mhz = strtol(at + 6, NULL, 10);
    return 1;
}

static void t_each_policy_carries_its_own_frequency(void)
{
    struct cpuobs cpu;
    int found = 0;

    /* The parser walks FORWARD from a label to find "mhz", so emitting mhz before label -
     * which qcstats.h:26 declares a non-breaking change - makes every policy read its
     * neighbour's clock: policy 0 gets 940, policy 1 gets 844, policy 2 reads nothing. Every
     * assertion in this file still held, because the labels were right, the count was right,
     * and a plausibility band of 100-4000 MHz accepts another policy's number.
     *
     * So each frequency is compared against the bytes inside its own object. */
    memset(&cpu, 0, sizeof cpu);
    T_TRUE(stats_extract_cpu(sample, &cpu), "the cpu array is found");

    for (int i = 0; i < cpu.n; i++) {
        char label[64] = "", what[128];
        long mhz = -1;

        if (!policy_object(sample, i, label, sizeof label, &mhz)) {
            T_FAIL("policy %d could not be located in the raw line - have the keys moved?",
                   i);
            continue;
        }
        found++;
        snprintf(what, sizeof what, "policy %d's label is read from policy %d's object", i, i);
        T_EQ_STR(cpu.pol[i].label, label, what);
        snprintf(what, sizeof what, "and %s's frequency is the one in %s's object",
                 label, label);
        T_EQ_INT(cpu.pol[i].mhz, mhz, what);
        snprintf(what, sizeof what, "%s's frequency parsed at all", label);
        T_EQ_INT(cpu.pol[i].mhz_ok, 1, what);
    }
    T_EQ_INT(found, cpu.n, "every policy the consumer reported was found in the line");
}

static void t_the_seven_fields_parse(void)
{
    struct fanobs fan;
    struct cpuobs cpu;
    long mc = 0;

    memset(&fan, 0, sizeof fan);
    T_TRUE(stats_extract_fan(sample, &fan), "the fan object is found");
    T_TRUE(fan.present, "and reports itself present");
    T_TRUE(fan.duty_ok, "duty_pct parsed");
    /* Bounded both ways: a sign flip or a negative duty in the stream passed the
     * upper-bound-only check, and a negative percentage would drive the airflow
     * judgement through a comparison it was never meant to see. */
    T_TRUE(fan.duty_pct <= 100, "as a percentage, not above 100");
    T_TRUE((long)fan.duty_pct >= 0, "and not below 0");
    T_TRUE(fan.rpm_ok, "rpm parsed");
    T_TRUE(fan.rpm > 0 || fan.duty_pct == 0, "with a speed, or the fan is off");

    /* rpm_valid is the flag the airflow judgement gates on. This used to assert that it was
     * 0 or 1 - which a `bool` cannot help being, so the check was T_TRUE(1) and the drift it
     * names in its own comment went straight past it. The flag has to be shown to FOLLOW the
     * stream, which takes a line of each kind. */
    {
        struct fanobs t, f;
        char line[STATS_LINE_MAX];
        const char *at = strstr(sample, "\"rpm_valid\":");

        T_TRUE(at != NULL, "the stream carries rpm_valid at all");
        if (at) {
            const char *rest = strchr(at, ',');

            snprintf(line, sizeof line, "{\"v\":%d,\"fan\":{\"duty_pct\":40,\"rpm\":2588,"
                     "\"rpm_valid\":true}}", STATS_SCHEMA_V);
            memset(&t, 0, sizeof t);
            T_TRUE(stats_extract_fan(line, &t), "a line saying true parses");
            T_EQ_INT(t.rpm_valid, 1, "and rpm_valid reads true");

            snprintf(line, sizeof line, "{\"v\":%d,\"fan\":{\"duty_pct\":40,\"rpm\":2588,"
                     "\"rpm_valid\":false}}", STATS_SCHEMA_V);
            memset(&f, 0, sizeof f);
            T_TRUE(stats_extract_fan(line, &f), "a line saying false parses");
            T_EQ_INT(f.rpm_valid, 0, "and rpm_valid reads false - the coasting-fan case");
            (void)rest;
        }
    }

    memset(&cpu, 0, sizeof cpu);
    T_TRUE(stats_extract_cpu(sample, &cpu), "the cpu array is found");
    T_EQ_INT(cpu.n, 3, "with one entry per POLICY, not per core - three on this board");
    T_EQ_INT(cpu.overflow, 0, "and it fits");
    {
        /* The overflow flag is never asserted TRUE anywhere, and with STATS_MAX_CPU at 4
         * and every real line carrying three policies it cannot be: the branch that sets
         * it is unreachable from the fixture. A line with more policies than the consumer
         * can hold is the case where "a partial list reported as whole" would matter. */
        struct cpuobs many;
        char line[STATS_LINE_MAX];
        int n = snprintf(line, sizeof line, "{\"v\":%d,\"cpu\":[", STATS_SCHEMA_V);

        for (int i = 0; i < STATS_MAX_CPU + 2; i++)
            n += snprintf(line + n, sizeof line - (size_t)n,
                          "%s{\"label\":\"P%d\",\"mhz\":%d}", i ? "," : "", i,
                          1000 + i);
        snprintf(line + n, sizeof line - (size_t)n, "]}");

        memset(&many, 0, sizeof many);
        T_TRUE(stats_extract_cpu(line, &many), "a line with more policies than fit parses");
        T_EQ_INT(many.n, STATS_MAX_CPU, "only as many as there is room for are kept");
        T_EQ_INT(many.overflow, 1, "and the overflow is REPORTED, not silently truncated");
    }
    T_EQ_STR(cpu.pol[0].label, "A78C.0", "the first policy's label");
    T_EQ_STR(cpu.pol[2].label, "A55", "and the last one's");
    T_TRUE(cpu.pol[0].mhz_ok, "its frequency parsed");
    T_TRUE(cpu.pol[0].mhz > 100 && cpu.pol[0].mhz < 4000, "as a plausible MHz value");

    /* The temperature the fan curve is driven from. Both zone spellings must resolve: the
     * config uses the short one and the kernel's own type is the long one. */
    T_TRUE(stats_extract_temp(sample, "cpuss-0-0", &mc), "the zone resolves by short name");
    T_TRUE(mc > 20000 && mc < 90000, "with a plausible milli-C temperature");
    {
        /* The two spellings must resolve to the SAME zone, so the readings are compared
         * rather than each being bounded: a full-type lookup landing on a neighbouring zone
         * returns a plausible temperature too, and that would drive the curve off the wrong
         * sensor. 36.5 and 36.8 are both "plausible". */
        long full = 0;

        T_TRUE(stats_extract_temp(sample, "cpuss-0-0-thermal", &full),
               "and by the kernel's full type");
        T_EQ_INT(full, mc, "the same zone, so the very same reading");
    }

    /* A zone the stream does not carry is absent, not zero. */
    mc = 4242;
    T_TRUE(!stats_extract_temp(sample, "no-such-zone", &mc), "an unknown zone is absent");
    T_EQ_INT(mc, 4242, "and the caller's value is left alone");
}

static void t_absence_survives_the_parser(void)
{
    struct fanobs fan;
    struct cpuobs cpu;
    long mc = 0;

    /* The producer's rule is that an unmeasured field is null. The consumer's half of that
     * rule is to carry the absence through rather than turn it into a number - these are
     * the lines a board without a fan or power sensor actually emits. */
    const char *nulls =
        "{\"v\":1,\"t\":1.0,\"ram\":null,\"cpu\":[{\"label\":\"A78C.0\",\"mhz\":null,"
        "\"cores\":[]},{\"label\":\"A78C.1\",\"mhz\":null,\"cores\":[]},"
        "{\"label\":\"A55\",\"mhz\":null,\"cores\":[]}],\"gpu_mhz\":null,"
        "\"gpu_busy_pct\":null,\"npu\":null,\"thermal_c\":{\"cpuss-0-0\":null},"
        "\"power\":null,\"fan\":null,\"throttle\":{\"A78C.0\":null}}\n";

    memset(&fan, 0, sizeof fan);
    T_TRUE(!stats_extract_fan(nulls, &fan) || !fan.present,
           "a null fan object is not a fan at 0 rpm");
    T_EQ_INT(fan.rpm_ok, 0, "no rpm was read");
    T_EQ_INT(fan.duty_ok, 0, "and no duty");

    memset(&cpu, 0, sizeof cpu);
    stats_extract_cpu(nulls, &cpu);
    T_EQ_INT(cpu.n, 3, "the policies are still there");
    T_EQ_INT(cpu.pol[0].mhz_ok, 0, "but a null frequency is not 0 MHz");

    mc = 4242;
    T_TRUE(!stats_extract_temp(nulls, "cpuss-0-0", &mc),
           "a null temperature is absent, which is what holds a fan curve rather than"
           " driving it from an invented number");
    T_EQ_INT(mc, 4242, "and nothing was written");
}

static void t_the_consumer_path_over_a_pipe(void)
{
    struct stats st;
    long mc = 0;
    bool ok = true;
    int fd[2];

    /* The real read path: a pipe, partial lines, and the version dispatch. The daemon never
     * calls the extractors directly - it calls this. */
    if (pipe(fd) != 0) {
        T_FAIL("%s", "cannot create a pipe");
        return;
    }
    memset(&st, 0, sizeof st);
    st.fd = fd[0];
    st.pid = -1;
    st.v_bad_last = -1;

    /* Reassembly, actually exercised. Both halves used to be written before the first
     * sample, so one read() returned the whole line and the reassembly buffer was never
     * involved: deleting it - discarding the buffered prefix on every read - passed. The
     * sampler has to be asked BETWEEN the writes, see nothing complete, and say so. */
    {
        size_t half = strlen(sample) / 2;
        ssize_t w1 = write(fd[1], sample, half);

        T_TRUE(w1 > 0, "the first half of the line is written");
        mc = 4242;
        ok = true;
        T_EQ_INT(stats_sample(&st, 50, "cpuss-0-0", &mc, &ok), STATS_TIMEOUT,
                 "half a line is not a line: the sampler reports a timeout");
        T_EQ_INT(mc, 4242, "and writes nothing from a partial line");
        T_EQ_INT(ok, 0, "nor claims a reading");

        T_TRUE(write(fd[1], sample + half, strlen(sample) - half) > 0,
               "then the second half arrives");
    }
    T_EQ_INT(stats_sample(&st, 200, "cpuss-0-0", &mc, &ok), STATS_OK,
             "and the two halves are reassembled into one line");
    T_TRUE(ok, "its temperature is present");
    T_TRUE(mc > 20000 && mc < 90000, "and plausible");
    /* Compared against the line that was actually fed in, not probed for a substring any
     * plausible line would contain: the extra extractors are handed THIS line, and a
     * last_line that returned something else - or a constant - would read as fine. The
     * newline is the reader's delimiter and is consumed, so it is not part of the line. */
    {
        char want[STATS_LINE_MAX];

        snprintf(want, sizeof want, "%s", sample);
        want[strcspn(want, "\n")] = '\0';
        T_EQ_STR(stats_last_line(&st), want, "the line kept is the line that arrived");
    }

    /* A version this build does not read counts as no reading - not as a misread one. */
    {
        char bumped[STATS_LINE_MAX];
        const char *p = strstr(sample, "\"v\":");
        const char *rest = p ? p + 4 : NULL;

        /* Skip the digits rather than assuming there is one of them. `p + 5` worked only
         * while the schema version was a single character: at version 10 it spliced from
         * the second digit, so the forged line read {"v":990,...} and the test failed on
         * correct, properly-synchronised code - the worst kind of failure, because the
         * person who bumped the schema did nothing wrong. */
        while (rest && *rest >= '0' && *rest <= '9')
            rest++;
        snprintf(bumped, sizeof bumped, "{\"v\":99%s", rest ? rest : "}\n");
        if (write(fd[1], bumped, strlen(bumped)) < 0)
            T_FAIL("%s", "cannot write the bumped line");
        ok = true;
        mc = 4242;
        T_EQ_INT(stats_sample(&st, 200, "cpuss-0-0", &mc, &ok), STATS_OK,
                 "a line of another schema version still reads as a line");
        T_EQ_INT(ok, 0, "but carries no temperature");
        T_EQ_INT(mc, 4242, "and writes nothing");
        T_EQ_INT((int)st.v_bad, 1, "it is counted");
        T_EQ_INT((int)st.v_bad_last, 99, "and the version is remembered, for the log");
    }

    /* Nothing to read is a timeout, which the daemon treats as a stalled stream rather
     * than as a reading of zero. */
    ok = true;
    T_EQ_INT(stats_sample(&st, 50, "cpuss-0-0", &mc, &ok), STATS_TIMEOUT,
             "an idle stream times out");

    close(fd[1]);
    T_EQ_INT(stats_sample(&st, 50, "cpuss-0-0", &mc, &ok), STATS_EOF,
             "and a closed one reports EOF, which is how a dead sampler is noticed");
    close(fd[0]);
}

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : DEFAULT_STREAM;

    if (load(path) < 0)
        return 2;
    printf("    (stream: %s)\n", path);

    t_the_two_constants_agree();
    t_the_schema_version_agrees();
    t_the_seven_fields_parse();
    t_each_policy_carries_its_own_frequency();
    t_absence_survives_the_parser();
    t_the_consumer_path_over_a_pipe();
    return t_summary("contract");
}
