/* ============================================================================ *
 * AI-GENERATED CODE
 *
 * This file was written from scratch by an AI assistant (GLM/Z.ai).
 * It is part of the VORTEX JIT compiler project.
 *
 * Human-written code lives in: src/interp/ (dispatch loop), src/baseline/
 * (codegen), src/runtime/ (GC, type system, arena), src/main_new.c.
 *
 * If reviewing, please verify correctness independently.
 * ============================================================================ */

/**
 * VORTEX Ensemble Profiles (Sprint 3) — Implementation
 *
 * See ensemble.h for design rationale.
 *
 * The robust aggregate uses median/mode/intersection operators instead
 * of sum/union. This is the key difference from vtx_profile_merge_into
 * (which sums and unions, letting outliers dominate).
 */

#include "profile/ensemble.h"
#include "profile/merge.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* ========================================================================== */
/* Internal helpers                                                            */
/* ========================================================================== */

static uint64_t ens_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

int vtx_ensemble_init(vtx_ensemble_t *ens)
{
    if (ens == NULL) return -1;
    memset(ens, 0, sizeof(*ens));
    /* runs[] is zero-initialized by memset — all slots have valid=false */
    ens->working_profile = NULL;
    ens->previous_aggregate = NULL;
    ens->owned_working = NULL;
    ens->owned_previous = NULL;
    ens->pending_validation = false;
    ens->validation_start_ns = 0;
    return 0;
}

void vtx_ensemble_destroy(vtx_ensemble_t *ens)
{
    if (ens == NULL) return;

    /* Destroy all run profiles. */
    for (uint32_t i = 0; i < VTX_ENSEMBLE_MAX_RUNS; i++) {
        if (ens->runs[i].valid) {
            vtx_profile_global_destroy(&ens->runs[i].profile);
            ens->runs[i].valid = false;
        }
    }

    /* Destroy owned aggregate profiles. */
    if (ens->owned_working != NULL) {
        vtx_profile_global_destroy(ens->owned_working);
        free(ens->owned_working);
        ens->owned_working = NULL;
    }
    if (ens->owned_previous != NULL) {
        vtx_profile_global_destroy(ens->owned_previous);
        free(ens->owned_previous);
        ens->owned_previous = NULL;
    }

    ens->working_profile = NULL;
    ens->previous_aggregate = NULL;
    ens->run_count = 0;
}

/* ========================================================================== */
/* Quality scoring                                                             */
/* ========================================================================== */

void vtx_ensemble_compute_quality(vtx_ensemble_run_meta_t *meta)
{
    if (meta == NULL) return;

    /* Sample count factor: 1.0 at MIN_SAMPLES, scales up but clamps. */
    double sample_factor = (double)meta->sample_count / (double)VTX_ENSEMBLE_MIN_SAMPLES;
    if (sample_factor > 1.0) sample_factor = 1.0;
    if (sample_factor < 0.0) sample_factor = 0.0;

    /* Deopt rate factor: 1.0 at 0% deopt, 0.0 at 100% deopt. */
    double deopt_factor = 1.0 - meta->deopt_rate;
    if (deopt_factor < 0.0) deopt_factor = 0.0;
    if (deopt_factor > 1.0) deopt_factor = 1.0;

    /* Duration factor: 1.0 at MIN_DURATION, scales up but clamps. */
    double duration_factor = meta->runtime_duration_s / VTX_ENSEMBLE_MIN_DURATION_S;
    if (duration_factor > 1.0) duration_factor = 1.0;
    if (duration_factor < 0.0) duration_factor = 0.0;

    meta->quality = sample_factor * deopt_factor * duration_factor;

    /* Demote if below threshold. */
    meta->demoted = (meta->quality < VTX_ENSEMBLE_QUALITY_THRESHOLD);
}

bool vtx_ensemble_run_is_demoted(const vtx_ensemble_run_meta_t *meta)
{
    if (meta == NULL) return true;
    return meta->demoted;
}

/* ========================================================================== */
/* Adding runs                                                                 */
/* ========================================================================== */

int vtx_ensemble_add_run(vtx_ensemble_t *ens,
                           const vtx_profile_global_t *profile,
                           vtx_ensemble_run_meta_t meta)
{
    if (ens == NULL || profile == NULL) return -1;

    /* Fill in timestamp if not provided. */
    if (meta.timestamp_ns == 0) {
        meta.timestamp_ns = ens_now_ns();
    }

    /* Compute deopt_rate if not provided. */
    if (meta.sample_count > 0 && meta.deopt_rate == 0.0 && meta.deopt_count > 0) {
        meta.deopt_rate = (double)meta.deopt_count / (double)meta.sample_count;
    }

    /* Compute quality score. */
    vtx_ensemble_compute_quality(&meta);

    /* Find the slot to write to (ring buffer). */
    uint32_t slot = ens->next_slot;

    /* If the slot is occupied, destroy the old profile (FIFO eviction). */
    if (ens->runs[slot].valid) {
        vtx_profile_global_destroy(&ens->runs[slot].profile);
        ens->runs[slot].valid = false;
        /* Note: run_count stays the same — we're replacing, not adding. */
    } else {
        ens->run_count++;
    }

    /* Initialize the slot's profile by copying from the input. */
    if (vtx_profile_global_init(&ens->runs[slot].profile) != 0) {
        return -1;
    }

    /* Use merge to copy the profile data (merge into empty = copy). */
    vtx_profile_merge_into(&ens->runs[slot].profile, profile);

    ens->runs[slot].meta = meta;
    ens->runs[slot].valid = true;

    /* Track demotions. */
    if (meta.demoted) {
        ens->total_demotions++;
    }

    /* Advance the ring buffer pointer. */
    ens->next_slot = (ens->next_slot + 1) % VTX_ENSEMBLE_MAX_RUNS;

    return 0;
}

/* ========================================================================== */
/* Internal: median computation                                                */
/* ========================================================================== */

/* Compute the median of an array of doubles.
 * Sorts the array in place. */
static double median_double(double *arr, uint32_t count)
{
    if (count == 0) return 0.0;
    if (count == 1) return arr[0];

    /* Simple insertion sort — arrays are tiny (K <= 5). */
    for (uint32_t i = 1; i < count; i++) {
        double key = arr[i];
        int32_t j = (int32_t)i - 1;
        while (j >= 0 && arr[j] > key) {
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = key;
    }

    if (count % 2 == 1) {
        return arr[count / 2];
    } else {
        return (arr[count / 2 - 1] + arr[count / 2]) / 2.0;
    }
}

/* Compute the median of an array of uint64_t values. */
static uint64_t median_u64(uint64_t *arr, uint32_t count)
{
    if (count == 0) return 0;
    if (count == 1) return arr[0];

    /* Insertion sort. */
    for (uint32_t i = 1; i < count; i++) {
        uint64_t key = arr[i];
        int32_t j = (int32_t)i - 1;
        while (j >= 0 && arr[j] > key) {
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = key;
    }

    if (count % 2 == 1) {
        return arr[count / 2];
    } else {
        /* For even counts, use the lower median to be conservative. */
        return arr[count / 2 - 1];
    }
}

/* ========================================================================== */
/* Internal: aggregate a single method across runs                             */
/* ========================================================================== */

/**
 * Aggregate one method's profile across all non-demoted runs that
 * contain it, using robust operators.
 *
 * Writes the result into `out_method` (which must already exist in the
 * target profile).
 */
static void aggregate_method(vtx_profile_method_t *out_method,
                               vtx_ensemble_entry_t *runs,
                               uint32_t run_count)
{
    /* --- Invocation count: median across runs --- */
    uint64_t invocations[VTX_ENSEMBLE_MAX_RUNS];
    uint32_t inv_count = 0;
    for (uint32_t r = 0; r < run_count; r++) {
        if (!runs[r].valid || runs[r].meta.demoted) continue;
        const vtx_profile_method_t *m =
            vtx_profile_get_method(&runs[r].profile, out_method->method_id);
        if (m == NULL) continue;
        invocations[inv_count++] = m->invocation_count;
    }
    if (inv_count > 0) {
        out_method->invocation_count = median_u64(invocations, inv_count);
    }

    /* --- Branches: median P(taken) across runs --- */
    /* Collect all unique branch PCs across runs, then for each PC,
     * compute the median P(taken). */
    for (uint32_t r = 0; r < run_count; r++) {
        if (!runs[r].valid || runs[r].meta.demoted) continue;
        const vtx_profile_method_t *m =
            vtx_profile_get_method(&runs[r].profile, out_method->method_id);
        if (m == NULL) continue;

        for (uint32_t b = 0; b < m->branch_count; b++) {
            uint32_t pc = m->branches[b].bytecode_pc;

            /* Skip if we've already processed this PC. */
            bool already_done = false;
            for (uint32_t ob = 0; ob < out_method->branch_count; ob++) {
                if (out_method->branches[ob].bytecode_pc == pc) {
                    already_done = true;
                    break;
                }
            }
            if (already_done) continue;

            /* Collect P(taken) from all runs that have this branch. */
            double probs[VTX_ENSEMBLE_MAX_RUNS];
            uint64_t totals[VTX_ENSEMBLE_MAX_RUNS];
            uint32_t n = 0;
            for (uint32_t r2 = 0; r2 < run_count; r2++) {
                if (!runs[r2].valid || runs[r2].meta.demoted) continue;
                const vtx_profile_method_t *m2 =
                    vtx_profile_get_method(&runs[r2].profile, out_method->method_id);
                if (m2 == NULL) continue;
                for (uint32_t b2 = 0; b2 < m2->branch_count; b2++) {
                    if (m2->branches[b2].bytecode_pc != pc) continue;
                    uint64_t tot = m2->branches[b2].taken + m2->branches[b2].not_taken;
                    if (tot == 0) continue;
                    probs[n] = (double)m2->branches[b2].taken / (double)tot;
                    totals[n] = tot;
                    n++;
                    break;
                }
            }

            if (n == 0) continue;

            double median_p = median_double(probs, n);
            uint64_t median_total = median_u64(totals, n);

            /* Reconstruct taken/not_taken from median P(taken) and median total. */
            uint64_t taken = (uint64_t)(median_p * (double)median_total);
            uint64_t not_taken = median_total - taken;

            /* Find or create the branch entry in out_method. */
            vtx_branch_profile_t *ob = NULL;
            for (uint32_t k = 0; k < out_method->branch_count; k++) {
                if (out_method->branches[k].bytecode_pc == pc) {
                    ob = &out_method->branches[k];
                    break;
                }
            }
            if (ob == NULL) {
                /* Grow if needed. */
                if (out_method->branch_count >= out_method->branch_capacity) {
                    uint32_t new_cap = out_method->branch_capacity == 0
                        ? 8 : out_method->branch_capacity * 2;
                    vtx_branch_profile_t *new_arr = realloc(
                        out_method->branches,
                        (size_t)new_cap * sizeof(vtx_branch_profile_t));
                    if (new_arr == NULL) continue;
                    memset(new_arr + out_method->branch_capacity, 0,
                           (size_t)(new_cap - out_method->branch_capacity) *
                           sizeof(vtx_branch_profile_t));
                    out_method->branches = new_arr;
                    out_method->branch_capacity = new_cap;
                }
                ob = &out_method->branches[out_method->branch_count++];
                memset(ob, 0, sizeof(*ob));
                ob->bytecode_pc = pc;
            }
            ob->taken = taken;
            ob->not_taken = not_taken;
        }
    }

    /* --- Call sites: mode — types seen in >50% of runs --- */
    for (uint32_t r = 0; r < run_count; r++) {
        if (!runs[r].valid || runs[r].meta.demoted) continue;
        const vtx_profile_method_t *m =
            vtx_profile_get_method(&runs[r].profile, out_method->method_id);
        if (m == NULL) continue;

        for (uint32_t cs = 0; cs < m->call_site_count; cs++) {
            uint32_t cs_idx = cs;  /* callsite index = position in array */

            /* Skip if already processed. */
            if (cs_idx < out_method->call_site_count &&
                out_method->call_sites[cs_idx].count > 0) {
                /* Already have data — but we need to aggregate. Skip
                 * for now; we'll process each callsite once. */
            }

            /* Collect all types seen at this callsite across runs,
             * and count how many runs saw each type. */
            vtx_typeid_t types_seen[VTX_POLY_LIMIT * VTX_ENSEMBLE_MAX_RUNS];
            uint32_t     type_run_counts[VTX_POLY_LIMIT * VTX_ENSEMBLE_MAX_RUNS];
            uint32_t     types_seen_count = 0;
            uint32_t     runs_with_cs = 0;
            bool         any_megamorphic = false;

            for (uint32_t r2 = 0; r2 < run_count; r2++) {
                if (!runs[r2].valid || runs[r2].meta.demoted) continue;
                const vtx_profile_method_t *m2 =
                    vtx_profile_get_method(&runs[r2].profile, out_method->method_id);
                if (m2 == NULL) continue;
                if (cs_idx >= m2->call_site_count) continue;

                const vtx_callsite_profile_t *cs2 = &m2->call_sites[cs_idx];
                if (cs2->megamorphic) {
                    any_megamorphic = true;
                    continue;
                }
                runs_with_cs++;

                for (uint32_t t = 0; t < cs2->count; t++) {
                    vtx_typeid_t ty = cs2->types[t];
                    bool found = false;
                    for (uint32_t k = 0; k < types_seen_count; k++) {
                        if (types_seen[k] == ty) {
                            type_run_counts[k]++;
                            found = true;
                            break;
                        }
                    }
                    if (!found && types_seen_count <
                        VTX_POLY_LIMIT * VTX_ENSEMBLE_MAX_RUNS) {
                        types_seen[types_seen_count] = ty;
                        type_run_counts[types_seen_count] = 1;
                        types_seen_count++;
                    }
                }
            }

            /* Ensure the output has this callsite. */
            /* BUGFIX P9: The old code used a while loop that broke on realloc
             * failure, then fell through to write past the allocated array.
             * If realloc fails, we must skip this callsite entirely, not
             * write to unallocated memory. */
            while (cs_idx >= out_method->call_site_capacity) {
                uint32_t new_cap = out_method->call_site_capacity == 0
                    ? 8 : out_method->call_site_capacity * 2;
                vtx_callsite_profile_t *new_arr = realloc(
                    out_method->call_sites,
                    (size_t)new_cap * sizeof(vtx_callsite_profile_t));
                if (new_arr == NULL) {
                    /* Realloc failed — skip this callsite, don't write OOB. */
                    goto skip_callsite;
                }
                memset(new_arr + out_method->call_site_capacity, 0,
                       (size_t)(new_cap - out_method->call_site_capacity) *
                       sizeof(vtx_callsite_profile_t));
                out_method->call_sites = new_arr;
                out_method->call_site_capacity = new_cap;
            }
            if (cs_idx >= out_method->call_site_count) {
                out_method->call_site_count = cs_idx + 1;
            }
            vtx_callsite_profile_t *ocs = &out_method->call_sites[cs_idx];
            memset(ocs, 0, sizeof(*ocs));

            if (any_megamorphic) {
                ocs->megamorphic = true;
                continue;
            }

            /* Mode: include types seen in >50% of runs that had this callsite. */
            if (runs_with_cs == 0) continue;
            uint32_t threshold = (runs_with_cs + 1) / 2;  /* >50% */

            for (uint32_t k = 0; k < types_seen_count && ocs->count < VTX_POLY_LIMIT; k++) {
                if (type_run_counts[k] >= threshold) {
                    ocs->types[ocs->count++] = types_seen[k];
                }
            }
            /* If no types met the threshold, fall back to the most common. */
            if (ocs->count == 0 && types_seen_count > 0) {
                uint32_t best = 0;
                for (uint32_t k = 1; k < types_seen_count; k++) {
                    if (type_run_counts[k] > type_run_counts[best]) best = k;
                }
                ocs->types[ocs->count++] = types_seen[best];
            }
        skip_callsite: ;
        }
    }

    /* --- Field accesses: intersection — shapes seen in ALL runs --- */
    for (uint32_t r = 0; r < run_count; r++) {
        if (!runs[r].valid || runs[r].meta.demoted) continue;
        const vtx_profile_method_t *m =
            vtx_profile_get_method(&runs[r].profile, out_method->method_id);
        if (m == NULL) continue;

        for (uint32_t fa = 0; fa < m->field_access_count; fa++) {
            uint32_t offset = m->field_accesses[fa].field_offset;

            /* Skip if already processed. */
            bool already_done = false;
            for (uint32_t k = 0; k < out_method->field_access_count; k++) {
                if (out_method->field_accesses[k].field_offset == offset) {
                    already_done = true;
                    break;
                }
            }
            if (already_done) continue;

            /* Collect shapes per run. */
            vtx_shapeid_t shapes_per_run[VTX_ENSEMBLE_MAX_RUNS][VTX_POLY_LIMIT];
            uint32_t shape_counts[VTX_ENSEMBLE_MAX_RUNS];
            bool any_megamorphic = false;
            uint32_t runs_with_field = 0;

            for (uint32_t r2 = 0; r2 < run_count; r2++) {
                if (!runs[r2].valid || runs[r2].meta.demoted) continue;
                const vtx_profile_method_t *m2 =
                    vtx_profile_get_method(&runs[r2].profile, out_method->method_id);
                if (m2 == NULL) continue;

                shape_counts[runs_with_field] = 0;
                for (uint32_t fa2 = 0; fa2 < m2->field_access_count; fa2++) {
                    if (m2->field_accesses[fa2].field_offset != offset) continue;
                    if (m2->field_accesses[fa2].megamorphic) {
                        any_megamorphic = true;
                    }
                    for (uint32_t s = 0; s < m2->field_accesses[fa2].count; s++) {
                        if (shape_counts[runs_with_field] < VTX_POLY_LIMIT) {
                            shapes_per_run[runs_with_field][shape_counts[runs_with_field]++] =
                                m2->field_accesses[fa2].shapes[s];
                        }
                    }
                    break;
                }
                if (shape_counts[runs_with_field] > 0 || any_megamorphic) {
                    runs_with_field++;
                }
            }

            /* Find or create the field access entry. */
            vtx_field_profile_t *ofa = NULL;
            for (uint32_t k = 0; k < out_method->field_access_count; k++) {
                if (out_method->field_accesses[k].field_offset == offset) {
                    ofa = &out_method->field_accesses[k];
                    break;
                }
            }
            if (ofa == NULL) {
                if (out_method->field_access_count >= out_method->field_access_capacity) {
                    uint32_t new_cap = out_method->field_access_capacity == 0
                        ? 8 : out_method->field_access_capacity * 2;
                    vtx_field_profile_t *new_arr = realloc(
                        out_method->field_accesses,
                        (size_t)new_cap * sizeof(vtx_field_profile_t));
                    if (new_arr == NULL) continue;
                    memset(new_arr + out_method->field_access_capacity, 0,
                           (size_t)(new_cap - out_method->field_access_capacity) *
                           sizeof(vtx_field_profile_t));
                    out_method->field_accesses = new_arr;
                    out_method->field_access_capacity = new_cap;
                }
                ofa = &out_method->field_accesses[out_method->field_access_count++];
                memset(ofa, 0, sizeof(*ofa));
                ofa->field_offset = offset;
            }

            if (any_megamorphic) {
                ofa->megamorphic = true;
                continue;
            }

            /* Intersection: shapes seen in ALL runs. */
            if (runs_with_field == 0) continue;

            /* Start with the first run's shapes, then keep only those
             * present in all other runs. */
            ofa->count = 0;
            for (uint32_t s = 0; s < shape_counts[0]; s++) {
                vtx_shapeid_t candidate = shapes_per_run[0][s];
                bool in_all = true;
                for (uint32_t r2 = 1; r2 < runs_with_field; r2++) {
                    bool found = false;
                    for (uint32_t s2 = 0; s2 < shape_counts[r2]; s2++) {
                        if (shapes_per_run[r2][s2] == candidate) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) { in_all = false; break; }
                }
                if (in_all && ofa->count < VTX_POLY_LIMIT) {
                    /* Avoid duplicates. */
                    bool dup = false;
                    for (uint32_t k = 0; k < ofa->count; k++) {
                        if (ofa->shapes[k] == candidate) { dup = true; break; }
                    }
                    if (!dup) {
                        ofa->shapes[ofa->count++] = candidate;
                    }
                }
            }
        }
    }

    /* --- Loops: median backedge count --- */
    for (uint32_t r = 0; r < run_count; r++) {
        if (!runs[r].valid || runs[r].meta.demoted) continue;
        const vtx_profile_method_t *m =
            vtx_profile_get_method(&runs[r].profile, out_method->method_id);
        if (m == NULL) continue;

        for (uint32_t l = 0; l < m->loop_count; l++) {
            uint32_t pc = m->loops[l].loop_header_pc;

            bool already_done = false;
            for (uint32_t k = 0; k < out_method->loop_count; k++) {
                if (out_method->loops[k].loop_header_pc == pc) {
                    already_done = true;
                    break;
                }
            }
            if (already_done) continue;

            uint64_t backedges[VTX_ENSEMBLE_MAX_RUNS];
            uint32_t n = 0;
            for (uint32_t r2 = 0; r2 < run_count; r2++) {
                if (!runs[r2].valid || runs[r2].meta.demoted) continue;
                const vtx_profile_method_t *m2 =
                    vtx_profile_get_method(&runs[r2].profile, out_method->method_id);
                if (m2 == NULL) continue;
                for (uint32_t l2 = 0; l2 < m2->loop_count; l2++) {
                    if (m2->loops[l2].loop_header_pc != pc) continue;
                    backedges[n++] = m2->loops[l2].backedge_count;
                    break;
                }
            }

            if (n == 0) continue;
            uint64_t median_be = median_u64(backedges, n);

            vtx_loop_profile_t *ol = NULL;
            for (uint32_t k = 0; k < out_method->loop_count; k++) {
                if (out_method->loops[k].loop_header_pc == pc) {
                    ol = &out_method->loops[k];
                    break;
                }
            }
            if (ol == NULL) {
                if (out_method->loop_count >= out_method->loop_capacity) {
                    uint32_t new_cap = out_method->loop_capacity == 0
                        ? 8 : out_method->loop_capacity * 2;
                    vtx_loop_profile_t *new_arr = realloc(
                        out_method->loops,
                        (size_t)new_cap * sizeof(vtx_loop_profile_t));
                    if (new_arr == NULL) continue;
                    memset(new_arr + out_method->loop_capacity, 0,
                           (size_t)(new_cap - out_method->loop_capacity) *
                           sizeof(vtx_loop_profile_t));
                    out_method->loops = new_arr;
                    out_method->loop_capacity = new_cap;
                }
                ol = &out_method->loops[out_method->loop_count++];
                memset(ol, 0, sizeof(*ol));
                ol->loop_header_pc = pc;
            }
            ol->backedge_count = median_be;
        }
    }
}

/* ========================================================================== */
/* Aggregate computation                                                       */
/* ========================================================================== */

vtx_profile_global_t *vtx_ensemble_compute_aggregate(vtx_ensemble_t *ens)
{
    if (ens == NULL) return NULL;

    /* Count non-demoted runs. */
    uint32_t good_runs = 0;
    for (uint32_t i = 0; i < VTX_ENSEMBLE_MAX_RUNS; i++) {
        if (ens->runs[i].valid && !ens->runs[i].meta.demoted) {
            good_runs++;
        }
    }

    if (good_runs == 0) {
        /* No good runs — keep the existing working profile if any. */
        return ens->working_profile;
    }

    /* Move the current working profile to previous (for rollback). */
    if (ens->owned_working != NULL) {
        if (ens->owned_previous != NULL) {
            vtx_profile_global_destroy(ens->owned_previous);
            free(ens->owned_previous);
        }
        ens->owned_previous = ens->owned_working;
        ens->previous_aggregate = ens->owned_previous;
        ens->owned_working = NULL;
    }

    /* Allocate a new working profile. */
    ens->owned_working = (vtx_profile_global_t *)calloc(1, sizeof(vtx_profile_global_t));
    if (ens->owned_working == NULL) {
        /* Allocation failed — fall back to previous. */
        ens->working_profile = ens->previous_aggregate;
        return ens->working_profile;
    }
    if (vtx_profile_global_init(ens->owned_working) != 0) {
        free(ens->owned_working);
        ens->owned_working = NULL;
        ens->working_profile = ens->previous_aggregate;
        return ens->working_profile;
    }
    ens->working_profile = ens->owned_working;

    /* If fewer than MIN_RUNS, just copy the most recent good run. */
    if (good_runs < VTX_ENSEMBLE_MIN_RUNS) {
        /* Find the most recent good run. */
        for (int32_t i = VTX_ENSEMBLE_MAX_RUNS - 1; i >= 0; i--) {
            if (ens->runs[i].valid && !ens->runs[i].meta.demoted) {
                vtx_profile_merge_into(ens->owned_working, &ens->runs[i].profile);
                break;
            }
        }
    } else {
        /* Collect all unique method IDs across good runs. */
        for (uint32_t r = 0; r < VTX_ENSEMBLE_MAX_RUNS; r++) {
            if (!ens->runs[r].valid || ens->runs[r].meta.demoted) continue;
            for (uint32_t m = 0; m < ens->runs[r].profile.method_count; m++) {
                uint32_t mid = ens->runs[r].profile.methods[m].method_id;
                if (vtx_profile_get_method(ens->owned_working, mid) == NULL) {
                    vtx_profile_add_method(ens->owned_working, mid);
                }
            }
        }

        /* Aggregate each method. */
        for (uint32_t m = 0; m < ens->owned_working->method_count; m++) {
            aggregate_method(&ens->owned_working->methods[m], ens->runs, VTX_ENSEMBLE_MAX_RUNS);
        }

        /* Merge call edges (sum is fine for edges — they're frequency-weighted
         * and don't have the outlier problem that branch probabilities do). */
        for (uint32_t r = 0; r < VTX_ENSEMBLE_MAX_RUNS; r++) {
            if (!ens->runs[r].valid || ens->runs[r].meta.demoted) continue;
            for (uint32_t e = 0; e < ens->runs[r].profile.call_edge_count; e++) {
                const vtx_call_edge_t *src = &ens->runs[r].profile.call_edges[e];
                /* Find or create the edge. */
                bool found = false;
                for (uint32_t k = 0; k < ens->owned_working->call_edge_count; k++) {
                    vtx_call_edge_t *tgt = &ens->owned_working->call_edges[k];
                    if (tgt->caller_method_id == src->caller_method_id &&
                        tgt->callee_method_id == src->callee_method_id) {
                        uint64_t sum = tgt->frequency + src->frequency;
                        tgt->frequency = (sum < tgt->frequency) ? UINT64_MAX : sum;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    /* Reuse merge's growth logic by calling merge_into
                     * with just the edges. For simplicity, add directly. */
                    if (ens->owned_working->call_edge_count >=
                        ens->owned_working->call_edge_capacity) {
                        uint32_t new_cap = ens->owned_working->call_edge_capacity == 0
                            ? 8 : ens->owned_working->call_edge_capacity * 2;
                        vtx_call_edge_t *new_arr = realloc(
                            ens->owned_working->call_edges,
                            (size_t)new_cap * sizeof(vtx_call_edge_t));
                        if (new_arr == NULL) continue;
                        ens->owned_working->call_edges = new_arr;
                        ens->owned_working->call_edge_capacity = new_cap;
                    }
                    vtx_call_edge_t *tgt =
                        &ens->owned_working->call_edges[ens->owned_working->call_edge_count++];
                    tgt->caller_method_id = src->caller_method_id;
                    tgt->callee_method_id = src->callee_method_id;
                    tgt->frequency = src->frequency;
                }
            }
        }
    }

    /* Mark as pending validation. */
    ens->pending_validation = true;
    ens->validation_start_ns = ens_now_ns();
    ens->total_aggregates_computed++;

    return ens->owned_working;
}

vtx_profile_global_t *vtx_ensemble_get_working(vtx_ensemble_t *ens)
{
    if (ens == NULL) return NULL;
    return ens->working_profile;
}

/* ========================================================================== */
/* Validation and rollback                                                     */
/* ========================================================================== */

bool vtx_ensemble_validate(vtx_ensemble_t *ens, double observed_deopt_rate)
{
    if (ens == NULL) return false;
    if (!ens->pending_validation) return true;  /* already validated */

    if (observed_deopt_rate > VTX_ENSEMBLE_ROLLBACK_DEOPT_RATE) {
        /* Bad aggregate — roll back. */
        vtx_ensemble_rollback(ens);
        return false;
    }

    /* Good aggregate — mark as validated. */
    ens->pending_validation = false;
    return true;
}

bool vtx_ensemble_rollback(vtx_ensemble_t *ens)
{
    if (ens == NULL) return false;
    if (ens->previous_aggregate == NULL) return false;

    /* Swap: the current working becomes nothing (we discard it),
     * the previous becomes the working. */
    if (ens->owned_working != NULL) {
        vtx_profile_global_destroy(ens->owned_working);
        free(ens->owned_working);
        ens->owned_working = NULL;
    }

    ens->owned_working = ens->owned_previous;
    ens->working_profile = ens->owned_working;
    ens->owned_previous = NULL;
    ens->previous_aggregate = NULL;

    /* The rolled-back aggregate is already validated (it was the
     * previous working profile before this one). */
    ens->pending_validation = false;
    ens->total_rollbacks++;

    return true;
}

bool vtx_ensemble_is_pending_validation(const vtx_ensemble_t *ens)
{
    if (ens == NULL) return false;
    return ens->pending_validation;
}

/* ========================================================================== */
/* Statistics                                                                  */
/* ========================================================================== */

void vtx_ensemble_stats(const vtx_ensemble_t *ens,
                          uint32_t *run_count,
                          uint32_t *demoted_count,
                          uint64_t *total_aggregates,
                          uint64_t *total_rollbacks,
                          uint64_t *total_demotions)
{
    if (ens == NULL) {
        if (run_count) *run_count = 0;
        if (demoted_count) *demoted_count = 0;
        if (total_aggregates) *total_aggregates = 0;
        if (total_rollbacks) *total_rollbacks = 0;
        if (total_demotions) *total_demotions = 0;
        return;
    }

    if (run_count) *run_count = ens->run_count;

    if (demoted_count) {
        uint32_t d = 0;
        for (uint32_t i = 0; i < VTX_ENSEMBLE_MAX_RUNS; i++) {
            if (ens->runs[i].valid && ens->runs[i].meta.demoted) d++;
        }
        *demoted_count = d;
    }

    if (total_aggregates) *total_aggregates = ens->total_aggregates_computed;
    if (total_rollbacks)  *total_rollbacks  = ens->total_rollbacks;
    if (total_demotions)  *total_demotions  = ens->total_demotions;
}
