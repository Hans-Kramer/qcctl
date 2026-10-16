// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer
//
// Prints the discovered control surface: what qcctl can see, what it may write
// and what each knob held at boot. A shipped diagnostic, and read-only.
//
//   QCCTL_SYSROOT=<a captured fixture tree> qcctl-surface-dump

#define _GNU_SOURCE
#include "surface.h"
#include "io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *wr(const struct knob *k)
{
    return knob_access_str(k);
}

static void show_knob(const char *label, const struct knob *k)
{
    char perm[48] = "-";

    if (k->present && k->mode >= 0)
        snprintf(perm, sizeof perm, "%03o %.20s", k->mode,
                 k->owner[0] ? k->owner : "?");
    printf("      %-14s %-3s %-14s %s\n", label, knob_access_str(k), perm,
           k->present ? (k->boot[0] ? k->boot : "<no readable value>") : "-");
}

// From the Makefile; "unknown" for a hand-compiled binary.
#ifndef QCCTL_VERSION
#define QCCTL_VERSION "unknown"
#endif

static void usage(FILE *out)
{
    fprintf(out,
"usage: qcctl-surface-dump [-h|--help] [-V|--version]\n"
"\n"
"Prints the control surface qcctl discovered here: cpufreq policies and their\n"
"operating points, the GPU, thermal zones with their trips and cooling devices,\n"
"and for each control point whether it is readable, writable, its mode and owner,\n"
"and the value it held at boot. It writes nothing.\n"
"\n"
"  QCCTL_SYSROOT=<dir>   read a captured tree instead of this machine, which is\n"
"                        how a dry run and the tests work with no board attached\n");
}

int main(int argc, char **argv)
{
    const char *env = getenv(ENV_SYSROOT);
    struct surface s;

    if (argc > 1) {
        if (!strcmp(argv[1], "-V") || !strcmp(argv[1], "--version")) {
            printf("qcctl-surface-dump %s\n", QCCTL_VERSION);
            return 0;
        }
        if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
            usage(stdout);
            return 0;
        }
        fprintf(stderr, "qcctl-surface-dump: unknown argument '%s'\n", argv[1]);
        usage(stderr);
        return 2;
    }

    io_set_sysroot(env);
    surface_discover(&s);

    printf("sysroot: %s\n", (env && *env) ? env : "<live system>");
    printf("model  : %s\n\n", s.model[0] ? s.model : "<unknown>");

    printf("cpufreq policies: %d\n", s.npol);
    for (int i = 0; i < s.npol; i++) {
        const struct policy *p = &s.pol[i];
        printf("  policy%-3d cpus=", p->id);
        for (int c = 0; c < p->ncpus; c++)
            printf("%s%d", c ? "," : "", p->cpus[c]);
        printf(" capacity=%llu opps=%d govs=%d\n",
               (unsigned long long)p->capacity, p->nopp, p->ngov);
        if (p->nopp)
            printf("      range        %llu - %llu kHz\n",
                   (unsigned long long)p->opp_khz[0],
                   (unsigned long long)p->opp_khz[p->nopp - 1]);
        printf("      governors    ");
        for (int g = 0; g < p->ngov; g++)
            printf("%s%s", g ? " " : "", p->governors[g]);
        printf("\n");
        show_knob("governor", &p->k_governor);
        show_knob("min_freq", &p->k_min);
        show_knob("max_freq", &p->k_max);
        show_knob("setspeed", &p->k_setspeed);
    }

    printf("\ncpu hotplug: %d cpus\n", s.ncpu);
    printf("  online nodes present: ");
    for (int i = 0; i < s.ncpu; i++)
        printf("%scpu%d=%s", i ? " " : "", s.cpu[i].cpu,
               s.cpu[i].present ? (s.cpu[i].online ? "1" : "0") : "-");
    printf("\n");

    printf("\ngpu: %s\n", s.gpu.present ? s.gpu.node : "absent");
    if (s.gpu.present) {
        printf("      opps         ");
        for (int i = 0; i < s.gpu.nopp; i++)
            printf("%s%llu", i ? " " : "",
                   (unsigned long long)(s.gpu.opp_hz[i] / 1000000));
        printf(" MHz\n      governors    ");
        for (int g = 0; g < s.gpu.ngov; g++)
            printf("%s%s", g ? " " : "", s.gpu.governors[g]);
        printf("\n");
        show_knob("governor", &s.gpu.k_governor);
        show_knob("min_freq", &s.gpu.k_min);
        show_knob("max_freq", &s.gpu.k_max);
    }

    printf("\ncooling devices: %d\n", s.ncdev);
    for (int i = 0; i < s.ncdev; i++) {
        const struct cdev *c = &s.cdev[i];
        printf("  %-24s idx=%-3d state=%llu/%llu  %s\n", c->type, c->idx,
               (unsigned long long)c->cur_state,
               (unsigned long long)c->max_state, wr(&c->k_cur_state));
    }

    printf("\nthermal zones: %d\n", s.nzone);
    int writable_trips = 0;
    for (int i = 0; i < s.nzone; i++)
        for (int t = 0; t < s.zone[i].ntrips; t++)
            writable_trips += (s.zone[i].trips[t].mode & 0200) != 0;
    printf("  trip points with an owner write bit, all zones: %d\n", writable_trips);
    /* Zones a profile is likely to name; 48 is noise. Temperatures are deliberately
     * not shown - that is qcstats' job, not ours. */
    static const char *interesting[] = {
        "cpuss-0-0", "gpuss-0", "nsp-0-0-0", "ddrss-0", NULL
    };
    for (int i = 0; interesting[i]; i++) {
        const struct zone *z = surface_zone(&s, interesting[i]);
        if (!z) {
            printf("  %-12s <not present>\n", interesting[i]);
            continue;
        }
        printf("  %-12s idx=%-3d trips=%d mode=%s\n", interesting[i], z->idx,
               z->ntrips, wr(&z->k_mode));
        for (int t = 0; t < z->ntrips; t++)
            printf("      trip%d %-9s %ldC hyst=%ldC mode=%03o\n",
                   z->trips[t].idx, z->trips[t].type,
                   z->trips[t].temp_mc / 1000, z->trips[t].hyst_mc / 1000,
                   z->trips[t].mode < 0 ? 0 : z->trips[t].mode);
    }

    printf("\nhwmon (resolved by name, never index)\n");
    printf("  fan   : %s\n", s.hwmon_fan[0]   ? s.hwmon_fan   : "absent");
    printf("  power : %s\n", s.hwmon_power[0] ? s.hwmon_power : "absent");

    return 0;
}
