/**
 * VORTEX Profile Introspection CLI (Sprint 1.5 + Sprint 2.4)
 *
 * Usage:
 *   vortex-profile dump <file.prof>
 *   vortex-profile dump <file.prof> --method <id>
 *   vortex-profile dump <file.prof> --summary
 *   vortex-profile dump <file.prof> --confidence
 *   vortex-profile phases <dir> <hash_hex>
 *
 * The `dump` subcommand reads a persisted VORTEX profile file (the same
 * format written by profile/persist.c at exit) and prints its contents in
 * a human-readable form. This is essential for debugging PGO behavior:
 *
 *   - "Why did the JIT speculate on type X?" → dump shows what the
 *     profile recorded at that call site.
 *   - "Why didn't this method get promoted to T2?" → dump shows the
 *     confidence score for each feature.
 *   - "Is the profile being poisoned by a bad run?" → dump shows
 *     invocation counts and branch probabilities.
 *
 * Sprint 2.4: The `phases` subcommand lists all phase profile files in
 * a directory for a given bytecode hash. Each file is a per-phase profile
 * (Sprint 2 partitioning). Use `dump` on any individual file to inspect
 * its contents.
 *
 * Without this tool, PGO is a black box: you can see that compilation
 * happened, but not why. This tool makes PGO introspectable.
 *
 * Exit codes:
 *   0 = success
 *   1 = bad arguments
 *   2 = file not found / unreadable
 *   3 = file format error (bad magic, version mismatch, CRC failure)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <dirent.h>
#include <sys/stat.h>

#include "vortex_config.h"
#include "profile/data.h"
#include "profile/persist.h"
#include "profile/confidence.h"
#include "profile/phase_partition.h"
#include "profile/phase_persist.h"

/* ========================================================================== */
/* Helpers                                                                     */
/* ========================================================================== */

static void print_usage(FILE *out)
{
    fprintf(out,
        "Usage:\n"
        "  vortex-profile dump <file.prof> [options]\n"
        "  vortex-profile phases <dir> <hash_hex>\n"
        "\n"
        "dump options:\n"
        "  --summary         Print only aggregate stats (method count, edge count)\n"
        "  --method <id>     Print only the given method's profile\n"
        "  --confidence      Print per-method confidence scores (Sprint 1.1)\n"
        "  --branches        Print branch probabilities\n"
        "  --callsites       Print call-site type distributions\n"
        "  --loops           Print loop back-edge counts\n"
        "  --fields          Print field-access shape distributions\n"
        "  --all             Print everything (default)\n"
        "  -h, --help        Show this help\n"
        "\n"
        "If no section flag is given, --all is assumed.\n"
        "\n"
        "phases:\n"
        "  Lists all per-phase profile files in <dir> for the given bytecode\n"
        "  hash. Each file is a Sprint 2 per-phase profile. Use `dump` on any\n"
        "  individual file to inspect its contents.\n");
}

typedef struct {
    bool summary;
    bool confidence;
    bool branches;
    bool callsites;
    bool loops;
    bool fields;
    bool all;
    uint32_t method_filter;
    bool has_method_filter;
} dump_options_t;

static dump_options_t parse_options(int argc, char **argv, const char **file_out)
{
    dump_options_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.all = true;  /* default */
    *file_out = NULL;

    /* argv[1] should be "dump", argv[2] the file */
    int i = 1;
    if (i < argc && strcmp(argv[i], "dump") == 0) i++;
    if (i < argc && argv[i][0] != '-') {
        *file_out = argv[i];
        i++;
    }

    bool any_section = false;
    for (; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(stdout);
            exit(0);
        } else if (strcmp(argv[i], "--summary") == 0) {
            opts.summary = true;
            opts.all = false;
        } else if (strcmp(argv[i], "--confidence") == 0) {
            opts.confidence = true;
            opts.all = false;
        } else if (strcmp(argv[i], "--branches") == 0) {
            opts.branches = true;
            opts.all = false;
        } else if (strcmp(argv[i], "--callsites") == 0) {
            opts.callsites = true;
            opts.all = false;
        } else if (strcmp(argv[i], "--loops") == 0) {
            opts.loops = true;
            opts.all = false;
        } else if (strcmp(argv[i], "--fields") == 0) {
            opts.fields = true;
            opts.all = false;
        } else if (strcmp(argv[i], "--all") == 0) {
            opts.all = true;
        } else if (strcmp(argv[i], "--method") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --method requires an argument\n");
                exit(1);
            }
            opts.method_filter = (uint32_t)strtoul(argv[++i], NULL, 10);
            opts.has_method_filter = true;
        } else {
            fprintf(stderr, "error: unknown option '%s'\n", argv[i]);
            print_usage(stderr);
            exit(1);
        }
        (void)any_section;
    }

    return opts;
}

static const char *confidence_level_str(vtx_confidence_level_t lvl)
{
    switch (lvl) {
        case VTX_CONFIDENCE_LOW:    return "LOW";
        case VTX_CONFIDENCE_MEDIUM: return "MEDIUM";
        case VTX_CONFIDENCE_HIGH:   return "HIGH";
        default:                    return "?";
    }
}

/* ========================================================================== */
/* Per-section printers                                                        */
/* ========================================================================== */

static void print_summary(const vtx_profile_global_t *g)
{
    printf("=== Profile Summary ===\n");
    printf("  Methods:     %u\n", g->method_count);
    printf("  Call edges:  %u\n", g->call_edge_count);
    printf("  Phase transitions: %u\n", g->phase_transition_count);

    uint64_t total_invocations = 0;
    uint64_t total_branch_obs  = 0;
    uint64_t total_callsite_obs = 0;
    uint64_t total_loop_obs    = 0;
    uint64_t total_field_obs   = 0;

    for (uint32_t i = 0; i < g->method_count; i++) {
        const vtx_profile_method_t *m = &g->methods[i];
        total_invocations += m->invocation_count;
        for (uint32_t b = 0; b < m->branch_count; b++) {
            total_branch_obs += m->branches[b].taken + m->branches[b].not_taken;
        }
        total_callsite_obs += m->call_site_count;
        total_loop_obs    += m->loop_count;
        total_field_obs   += m->field_access_count;
    }

    printf("  Total invocations:    %" PRIu64 "\n", total_invocations);
    printf("  Total branch samples: %" PRIu64 "\n", total_branch_obs);
    printf("  Total call sites:     %" PRIu64 "\n", total_callsite_obs);
    printf("  Total loops:          %" PRIu64 "\n", total_loop_obs);
    printf("  Total field sites:    %" PRIu64 "\n", total_field_obs);
    printf("\n");
}

static void print_method_header(const vtx_profile_method_t *m)
{
    printf("--- Method %u ---\n", m->method_id);
    printf("  Invocations:    %" PRIu64 "\n", m->invocation_count);
    printf("  Branch sites:   %u\n", m->branch_count);
    printf("  Call sites:     %u\n", m->call_site_count);
    printf("  Loop sites:     %u\n", m->loop_count);
    printf("  Field sites:    %u\n", m->field_access_count);
}

static void print_confidence(const vtx_profile_method_t *m)
{
    double conf = vtx_confidence_method(m);
    vtx_confidence_level_t lvl = vtx_confidence_classify(conf);

    printf("  Confidence: %.3f  [%s]\n", conf, confidence_level_str(lvl));
    printf("    Eligibility:\n");
    printf("      T1: %s\n",
        vtx_confidence_eligible_for_tier(m, 1, 1) ? "yes" : "no");
    printf("      T2: %s  (need conf >= %.2f)\n",
        vtx_confidence_eligible_for_tier(m, VORTEX_T1_THRESHOLD, 2) ? "yes" : "no",
        VTX_PROMOTION_CONFIDENCE_T2);
    printf("      T3: %s  (need conf >= %.2f)\n",
        vtx_confidence_eligible_for_tier(m, VORTEX_T2_THRESHOLD, 3) ? "yes" : "no",
        VTX_PROMOTION_CONFIDENCE_T3);

    /* Per-feature breakdown. */
    if (m->branch_count > 0) {
        double min_b = 1.0, sum_b = 0.0;
        for (uint32_t i = 0; i < m->branch_count; i++) {
            double c = vtx_confidence_branch(&m->branches[i]);
            if (c < min_b) min_b = c;
            sum_b += c;
        }
        printf("    Branches:     min=%.3f  avg=%.3f  (threshold=%u)\n",
            min_b, sum_b / m->branch_count, VTX_CONFIDENCE_THRESHOLD_BRANCH);
    }
    if (m->call_site_count > 0) {
        double min_cs = 1.0, sum_cs = 0.0;
        for (uint32_t i = 0; i < m->call_site_count; i++) {
            double c = vtx_confidence_type_dist(&m->call_sites[i]);
            if (c < min_cs) min_cs = c;
            sum_cs += c;
        }
        printf("    Type dist:    min=%.3f  avg=%.3f  (threshold=%u)\n",
            min_cs, sum_cs / m->call_site_count, VTX_CONFIDENCE_THRESHOLD_TYPE_DIST);
    }
    if (m->loop_count > 0) {
        double min_l = 1.0;
        for (uint32_t i = 0; i < m->loop_count; i++) {
            double c = vtx_confidence_loop_trip(&m->loops[i]);
            if (c < min_l) min_l = c;
        }
        printf("    Loops:        min=%.3f  (threshold=%u)\n",
            min_l, VTX_CONFIDENCE_THRESHOLD_LOOP_TRIP);
    }
    if (m->field_access_count > 0) {
        double min_f = 1.0;
        for (uint32_t i = 0; i < m->field_access_count; i++) {
            double c = vtx_confidence_field_shape(&m->field_accesses[i]);
            if (c < min_f) min_f = c;
        }
        printf("    Field shapes: min=%.3f  (threshold=%u)\n",
            min_f, VTX_CONFIDENCE_THRESHOLD_FIELD_SHAPE);
    }
}

static void print_branches(const vtx_profile_method_t *m)
{
    if (m->branch_count == 0) return;
    printf("  Branches:\n");
    for (uint32_t i = 0; i < m->branch_count; i++) {
        const vtx_branch_profile_t *b = &m->branches[i];
        uint64_t total = b->taken + b->not_taken;
        double pct = (total > 0) ? (100.0 * (double)b->taken / (double)total) : 0.0;
        printf("    pc=%-6u  taken=%-10" PRIu64 "  not_taken=%-10" PRIu64
               "  P(taken)=%5.1f%%\n",
            b->bytecode_pc, b->taken, b->not_taken, pct);
    }
}

static void print_callsites(const vtx_profile_method_t *m)
{
    if (m->call_site_count == 0) return;
    printf("  Call sites:\n");
    for (uint32_t i = 0; i < m->call_site_count; i++) {
        const vtx_callsite_profile_t *cs = &m->call_sites[i];
        printf("    cs#%u  count=%u  megamorphic=%s",
            i, cs->count, cs->megamorphic ? "yes" : "no");
        if (cs->megamorphic) {
            printf("\n");
            continue;
        }
        printf("  types=[");
        for (uint32_t t = 0; t < cs->count; t++) {
            printf("%s%u", (t == 0) ? "" : ", ", cs->types[t]);
        }
        printf("]\n");
    }
}

static void print_loops(const vtx_profile_method_t *m)
{
    if (m->loop_count == 0) return;
    printf("  Loops:\n");
    for (uint32_t i = 0; i < m->loop_count; i++) {
        const vtx_loop_profile_t *l = &m->loops[i];
        printf("    header_pc=%-6u  backedges=%-10" PRIu64
               "  trip_stable=%s  last_trip=%" PRIu64 "\n",
            l->loop_header_pc, l->backedge_count,
            l->is_trip_stable ? "yes" : "no",
            l->last_trip_count);
    }
}

static void print_fields(const vtx_profile_method_t *m)
{
    if (m->field_access_count == 0) return;
    printf("  Field accesses:\n");
    for (uint32_t i = 0; i < m->field_access_count; i++) {
        const vtx_field_profile_t *f = &m->field_accesses[i];
        printf("    offset=%-6u  count=%u  megamorphic=%s",
            f->field_offset, f->count, f->megamorphic ? "yes" : "no");
        if (f->megamorphic) {
            printf("\n");
            continue;
        }
        printf("  shapes=[");
        for (uint32_t s = 0; s < f->count; s++) {
            printf("%s%u", (s == 0) ? "" : ", ", f->shapes[s]);
        }
        printf("]\n");
    }
}

/* ========================================================================== */
/* Main dump routine                                                           */
/* ========================================================================== */

static int do_dump(const char *filename, const dump_options_t *opts)
{
    vtx_profile_global_t global;
    if (vtx_profile_global_init(&global) != 0) {
        fprintf(stderr, "error: failed to initialize profile struct\n");
        return 2;
    }

    /* Load with no bytecode-hash check (we just want to inspect the file). */
    if (!vtx_profile_load(&global, filename, NULL)) {
        fprintf(stderr, "error: failed to load profile from '%s'\n", filename);
        fprintf(stderr, "       (file may not exist, be unreadable, or have a bad format)\n");
        vtx_profile_global_destroy(&global);
        return 3;
    }

    if (opts->summary) {
        print_summary(&global);
        vtx_profile_global_destroy(&global);
        return 0;
    }

    print_summary(&global);

    for (uint32_t i = 0; i < global.method_count; i++) {
        const vtx_profile_method_t *m = &global.methods[i];

        if (opts->has_method_filter && m->method_id != opts->method_filter) {
            continue;
        }

        print_method_header(m);

        if (opts->all || opts->confidence) {
            print_confidence(m);
        }
        if (opts->all || opts->branches) {
            print_branches(m);
        }
        if (opts->all || opts->callsites) {
            print_callsites(m);
        }
        if (opts->all || opts->loops) {
            print_loops(m);
        }
        if (opts->all || opts->fields) {
            print_fields(m);
        }
        printf("\n");
    }

    /* Print call-graph edges. */
    if ((opts->all) && global.call_edge_count > 0) {
        printf("=== Call Graph Edges ===\n");
        for (uint32_t i = 0; i < global.call_edge_count; i++) {
            printf("  %u -> %u  freq=%" PRIu64 "\n",
                global.call_edges[i].caller_method_id,
                global.call_edges[i].callee_method_id,
                global.call_edges[i].frequency);
        }
        printf("\n");
    }

    /* Print phase transitions. */
    if ((opts->all) && global.phase_transition_count > 0) {
        printf("=== Phase Transitions ===\n");
        for (uint32_t i = 0; i < global.phase_transition_count; i++) {
            printf("  phase %u -> phase %u  freq=%" PRIu64 "\n",
                global.phase_transitions[i].from_phase_id,
                global.phase_transitions[i].to_phase_id,
                global.phase_transitions[i].frequency);
        }
        printf("\n");
    }

    vtx_profile_global_destroy(&global);
    return 0;
}

/* ========================================================================== */
/* Sprint 2.4: `phases` subcommand                                             */
/* ========================================================================== */

/**
 * List all per-phase profile files in a directory for a given bytecode hash.
 *
 * For each file matching <hash_hex>.*.prof, prints:
 *   - The phase label ("default" or numeric ID)
 *   - The file path
 *   - The method count and total invocations in that phase's profile
 *
 * This is the entry point for debugging phase-partitioned PGO. Users
 * run `vortex-profile phases <dir> <hash>` to see what phases exist,
 * then `vortex-profile dump <file>` to inspect any individual phase.
 */
static int do_phases(const char *dir, const char *hash_hex)
{
    /* Use the partition loader to discover and load all phase files. */
    vtx_phase_partition_t part;
    if (vtx_phase_partition_init(&part) != 0) {
        fprintf(stderr, "error: failed to initialize partition\n");
        return 2;
    }

    int loaded = vtx_phase_partition_load_all(&part, dir, hash_hex, NULL);
    if (loaded < 0) {
        fprintf(stderr, "error: failed to scan directory '%s'\n", dir);
        vtx_phase_partition_destroy(&part);
        return 2;
    }

    printf("=== Phase Profiles in %s for hash %s ===\n", dir, hash_hex);
    printf("  Phases found: %u\n\n", vtx_phase_partition_phase_count(&part));

    uint32_t active = vtx_phase_partition_active_phase(&part);
    for (uint32_t i = 0; i < part.entry_count; i++) {
        if (!part.entries[i].valid) continue;

        const char *label =
            (part.entries[i].phase_id == VTX_PHASE_NONE)
                ? "default" : "(numeric)";
        char numbuf[16];
        if (part.entries[i].phase_id != VTX_PHASE_NONE) {
            snprintf(numbuf, sizeof(numbuf), "%u", part.entries[i].phase_id);
            label = numbuf;
        }

        /* Compute aggregate stats for this phase. */
        const vtx_profile_global_t *g = &part.entries[i].profile;
        uint64_t total_invocations = 0;
        for (uint32_t m = 0; m < g->method_count; m++) {
            total_invocations += g->methods[m].invocation_count;
        }

        char filename[600];
        vtx_phase_partition_filename(dir, hash_hex,
                                       part.entries[i].phase_id,
                                       filename, sizeof(filename));

        printf("  Phase %s%s\n",
               label,
               (part.entries[i].phase_id == active) ? "  [ACTIVE]" : "");
        printf("    File:         %s\n", filename);
        printf("    Methods:      %u\n", g->method_count);
        printf("    Invocations:  %" PRIu64 "\n", total_invocations);
        printf("    Transitions:  %" PRIu64 " (times entered)\n",
               part.entries[i].transition_count);
        printf("\n");
    }

    uint64_t total_transitions, total_creations;
    uint32_t active_id;
    vtx_phase_partition_stats(&part, &total_transitions, &total_creations, &active_id);
    printf("  Total transitions: %" PRIu64 "\n", total_transitions);
    printf("  Total creations:   %" PRIu64 "\n", total_creations);
    printf("  Active phase:      %s\n",
           (active_id == VTX_PHASE_NONE) ? "default" : "(numeric)");

    vtx_phase_partition_destroy(&part);
    return 0;
}

/* ========================================================================== */
/* Entry point                                                                 */
/* ========================================================================== */

int main(int argc, char **argv)
{
    if (argc < 2) {
        print_usage(stderr);
        return 1;
    }

    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        print_usage(stdout);
        return 0;
    }

    /* Sprint 2.4: `phases` subcommand — list per-phase profile files. */
    if (strcmp(argv[1], "phases") == 0) {
        if (argc < 4) {
            fprintf(stderr, "error: 'phases' requires <dir> and <hash_hex>\n");
            fprintf(stderr, "  usage: vortex-profile phases <dir> <hash_hex>\n");
            return 1;
        }
        return do_phases(argv[2], argv[3]);
    }

    if (strcmp(argv[1], "dump") != 0) {
        fprintf(stderr, "error: unknown subcommand '%s'\n", argv[1]);
        fprintf(stderr, "       supported subcommands: dump, phases\n");
        print_usage(stderr);
        return 1;
    }

    const char *filename = NULL;
    dump_options_t opts = parse_options(argc, argv, &filename);

    if (filename == NULL) {
        fprintf(stderr, "error: no profile file specified\n");
        print_usage(stderr);
        return 1;
    }

    return do_dump(filename, &opts);
}
