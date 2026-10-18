// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer
//
// Parse a profile config, validate it against the discovered surface, and print
// every write it would perform. This is the dry run: nothing is written, and it
// runs against a fixture with no board attached.
//
//   QCCTL_SYSROOT=<a captured fixture tree> ./qcctl-check etc/profiles.conf

#define _GNU_SOURCE
#include "config.h"
#include "surface.h"
#include "apply.h"
#include "io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// From the Makefile; "unknown" for a hand-compiled binary.
#ifndef QCCTL_VERSION
#define QCCTL_VERSION "unknown"
#endif

static void usage(FILE *out)
{
    fprintf(out,
"usage: qcctl-check [<profiles.conf>] [<profile>]\n"
"\n"
"Validates a profile config against what this board actually exposes, and plans\n"
"every profile against it. Writes nothing. Naming a profile, by name or id, limits\n"
"the report to that one. The default path is etc/profiles.conf, for running it from\n"
"a source tree.\n"
"\n"
"  -h, --help            this text\n"
"  -V, --version\n"
"\n"
"  QCCTL_SYSROOT=<dir>   read a captured tree instead of this machine\n"
"\n"
"Exit: 0 it parses, validates, and every profile is applicable here\n"
"      2 the file does not parse\n"
"      3 it parses but is not a usable config - a bad setting, or no DEFAULT profile\n"
"      4 the file is fine; this kernel does not expose something it names\n");
}

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "etc/profiles.conf";
    const char *only = (argc > 2) ? argv[2] : NULL;
    struct pconfig c;
    struct surface s;
    char err[ERRLEN], line[160];
    char planerr[MAX_PROFILES][ERRLEN];
    int nbad = 0;

    if (argc > 1) {
        if (!strcmp(argv[1], "-V") || !strcmp(argv[1], "--version")) {
            printf("qcctl-check %s\n", QCCTL_VERSION);
            return 0;
        }
        if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
            usage(stdout);
            return 0;
        }
        /* An option this does not know was taken as a config PATH, so --help opened a file
         * called "--help", failed to parse it and exited 2. The installer excluded this
         * binary from its "does it run here" test for exactly that reason. */
        if (argv[1][0] == '-') {
            fprintf(stderr, "qcctl-check: unknown option '%s'\n", argv[1]);
            usage(stderr);
            return 2;
        }
    }

    io_set_sysroot(getenv(ENV_SYSROOT));
    surface_discover(&s);

    if (config_parse_file(path, &c) < 0) {
        fprintf(stderr, "%s:%d: %s\n", path, c.err_line, c.err);
        return 2;
    }
    if (config_validate(&c, &s, err, sizeof err) < 0) {
        fprintf(stderr, "%s: %s\n", path, err);
        return 3;
    }

    printf("config : %s  (%d profiles)\n", path, c.nprof);
    printf("board  : %s\n\n", s.model[0] ? s.model : "<unknown>");

    for (int i = 0; i < c.nprof; i++) {
        const struct profile *p = &c.prof[i];

        if (only && strcmp(p->name, only) && config_find(&c, only) != p)
            continue;
        /* Settings and writes are not the same number: FAN MODE and FAN CURVE are
         * daemon state with no sysfs attribute behind them, and a policy setting
         * both MIN and MAX gains a staged write. Report both rather than imply
         * every setting is a write. */
        struct applyctx ctx;
        char perr[ERRLEN];
        int nw = -1;

        memset(&ctx, 0, sizeof ctx);
        ctx.dry = 1;
        if (apply_plan(p, &s, &ctx, perr, sizeof perr) == 0) {
            nw = ctx.nw;
        } else {
            /* Not cosmetic: a profile that cannot be planned cannot be applied. Saying
             * "validated OK" here would be the same false reassurance in the checker
             * that --verify used to give. Keep listing the rest, then exit non-zero. */
            snprintf(planerr[nbad], sizeof planerr[0], "%s", perr);
            if (nbad < MAX_PROFILES - 1) nbad++;
        }

        printf("profile %d '%s'%s  -- %d setting%s", p->id, p->name,
               p->is_default ? " [default]" : "",
               p->nact, p->nact == 1 ? "" : "s");
        if (nw >= 0)
            printf(", %d sysfs write%s", nw, nw == 1 ? "" : "s");
        printf("\n");
        for (int j = 0; j < p->nact; j++) {
            enum act_kind k = p->act[j].kind;
            int is_write = (k != ACT_FAN_MODE && k != ACT_FAN_CURVE);
            printf("    %-46s %s\n", act_describe(&p->act[j], line, sizeof line),
                   is_write ? "" : "(daemon state, not a write)");
        }
        printf("\n");
    }

    {
        /* Named per profile, and attributed: it is the profile carrying one that needs the
         * flag, not the file. And it is `qcctl -m` that asks - qcctld applies the config it
         * is given, because a daemon that refuses to start for want of a flag nobody can
         * pass it would be worse than the setting. The old wording said the opposite. */
        int ndisr = 0;

        for (int i = 0; i < c.nprof; i++) {
            const struct action *pd[8];
            size_t n = config_disruptive(&c.prof[i], pd, 8);

            for (size_t j = 0; j < n; j++) {
                if (!ndisr++)
                    printf("DISRUPTIVE -- legal and known to work, but drastic:\n");
                printf("    profile '%s' line %d: %s\n", c.prof[i].name, pd[j]->line,
                       act_describe(pd[j], line, sizeof line));
            }
        }
        if (ndisr)
            printf("  `qcctl -m <that profile>` refuses these without "
                   "--allow-disruptive.\n\n");
    }
    if (nbad) {
        printf("NOT APPLICABLE on this board -- %d profile%s cannot be planned:\n",
               nbad, nbad == 1 ? "" : "s");
        for (int i = 0; i < nbad; i++)
            printf("    %s\n", planerr[i]);
        printf("  The config is well-formed; this kernel does not expose what it names.\n");
        return 4;   /* distinct from 3: nothing is wrong with the FILE */
    }
    /* The installer runs this BEFORE enabling the unit, which is the whole point of
     * catching it here: a config with no DEFAULT leaves the daemon with nothing to apply,
     * and finding that out at the first restart means finding it out on a board that is
     * no longer serving its profile. */
    if (!config_default(&c)) {
        printf("NO DEFAULT PROFILE -- the file is well-formed, but the daemon starts with\n");
        printf("  no -m and so would have nothing to apply. Add DEFAULT to one profile's\n");
        printf("  < PROFILE ... > line.\n");
        return 3;
    }
    printf("validated OK -- every profile is applicable here, no writes performed\n");
    return 0;
}
