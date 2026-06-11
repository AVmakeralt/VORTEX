#include "sota/phase.h"
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

int vtx_sota_phase_init(vtx_sota_phase_t *phase,
                         vtx_phase_graph_t *phase_graph,
                         vtx_arena_t *arena)
{
    if (phase == NULL) return -1;

    memset(phase, 0, sizeof(*phase));
    phase->phase_graph = phase_graph;
    phase->predicted_phase = VTX_PHASE_NONE;

    /* Initialize current signature */
    for (uint32_t i = 0; i < VTX_PHASE_SIGNATURE_SIZE; i++) {
        phase->current_sig.method_ids[i] = 0;
    }
    phase->current_sig.count = 0;
    phase->current_sig.insert_pos = 0;

    /* Allocate precompiled methods tracking */
    phase->precompiled_capacity = 64;
    phase->precompiled_methods = (uint32_t *)vtx_arena_alloc(
        arena, phase->precompiled_capacity * sizeof(uint32_t));
    if (phase->precompiled_methods == NULL) {
        phase->precompiled_capacity = 0;
        return -1;
    }
    phase->precompiled_count = 0;

    phase->predictions_made = 0;
    phase->predictions_correct = 0;
    phase->predictions_wrong = 0;

    return 0;
}

void vtx_sota_phase_destroy(vtx_sota_phase_t *phase)
{
    if (phase == NULL) return;
    /* Phase graph is not owned — don't destroy it.
     * Arena-allocated memory is freed with the arena. */
    phase->precompiled_methods = NULL;
    phase->precompiled_count = 0;
}

/* ========================================================================== */
/* Monitoring                                                                  */
/* ========================================================================== */

void vtx_sota_phase_record_method(vtx_sota_phase_t *phase, uint32_t method_id)
{
    if (phase == NULL) return;

    vtx_phase_signature_t *sig = &phase->current_sig;

    /* Add method to circular buffer */
    sig->method_ids[sig->insert_pos] = method_id;
    sig->insert_pos = (sig->insert_pos + 1) % VTX_PHASE_SIGNATURE_SIZE;

    if (sig->count < VTX_PHASE_SIGNATURE_SIZE) {
        sig->count++;
    }
}

void vtx_sota_phase_update(vtx_sota_phase_t *phase, uint32_t entered_method)
{
    if (phase == NULL) return;

    /* Step 1: Add method to the sliding window signature */
    vtx_sota_phase_record_method(phase, entered_method);

    /* Step 2: If we have a phase graph, check if the entered method
     * belongs to the currently predicted phase. If so, record a hit. */
    if (phase->phase_graph != NULL && phase->predicted_phase != VTX_PHASE_NONE) {
        uint32_t method_phase = vtx_phase_for_method(phase->phase_graph, entered_method);

        if (method_phase == phase->predicted_phase) {
            /* This method is in the predicted phase — prediction was correct */
            vtx_sota_phase_record_hit(phase, entered_method);
        } else if (method_phase != VTX_PHASE_NONE && method_phase != phase->predicted_phase) {
            /* Method is in a different phase — we may be transitioning.
             * Check if we should end the current prediction and start a new one.
             * We only transition if we see multiple methods from the new phase
             * to avoid thrashing on methods that appear in multiple phases. */
            const vtx_phase_t *new_phase = vtx_phase_get_by_id(
                phase->phase_graph, method_phase);
            if (new_phase != NULL && new_phase->is_significant) {
                /* Compute a quick match score for the new phase using
                 * just the current signature */
                double score = vtx_sota_phase_match_score(phase, new_phase);
                if (score >= VTX_PHASE_MATCH_THRESHOLD) {
                    /* End current prediction and start new one */
                    vtx_sota_phase_end_prediction(phase);
                }
            }
        }
    }

    /* Step 3: Check if the entered method was pre-compiled by us.
     * This tracks prediction accuracy — if a pre-compiled method is
     * actually called, the prediction was useful. */
    for (uint32_t i = 0; i < phase->precompiled_count; i++) {
        if (phase->precompiled_methods[i] == entered_method) {
            /* Found it — this pre-compiled method was actually used */
            phase->predictions_correct++;
            break;
        }
    }
}

/* ========================================================================== */
/* Jaccard similarity                                                          */
/* ========================================================================== */

/**
 * Helper: count elements in set_a that are also in set_b.
 */
static uint32_t count_intersection(const uint32_t *set_a, uint32_t count_a,
                                     const uint32_t *set_b, uint32_t count_b)
{
    uint32_t count = 0;
    for (uint32_t i = 0; i < count_a; i++) {
        for (uint32_t j = 0; j < count_b; j++) {
            if (set_a[i] == set_b[j]) {
                count++;
                break;
            }
        }
    }
    return count;
}

/**
 * Helper: count unique elements across both sets.
 */
static uint32_t count_union(const uint32_t *set_a, uint32_t count_a,
                              const uint32_t *set_b, uint32_t count_b)
{
    /* Start with all elements of set_a */
    uint32_t count = count_a;

    /* Add elements of set_b that are not in set_a */
    for (uint32_t j = 0; j < count_b; j++) {
        bool found = false;
        for (uint32_t i = 0; i < count_a; i++) {
            if (set_a[i] == set_b[j]) {
                found = true;
                break;
            }
        }
        if (!found) count++;
    }

    return count;
}

double vtx_sota_phase_jaccard(const uint32_t *set_a, uint32_t count_a,
                                const uint32_t *set_b, uint32_t count_b)
{
    if (count_a == 0 && count_b == 0) return 1.0; /* both empty = identical */
    if (count_a == 0 || count_b == 0) return 0.0; /* one empty = no overlap */

    uint32_t intersection = count_intersection(set_a, count_a, set_b, count_b);
    uint32_t union_count = count_union(set_a, count_a, set_b, count_b);

    if (union_count == 0) return 0.0;
    return (double)intersection / (double)union_count;
}

double vtx_sota_phase_match_score(const vtx_sota_phase_t *phase,
                                    const vtx_phase_t *phase_def)
{
    if (phase == NULL || phase_def == NULL) return 0.0;

    return vtx_sota_phase_jaccard(
        phase->current_sig.method_ids,
        phase->current_sig.count,
        phase_def->method_ids,
        phase_def->method_count);
}

/* ========================================================================== */
/* Phase prediction                                                            */
/* ========================================================================== */

/**
 * Check if a method has already been pre-compiled in the current prediction.
 */
static bool is_precompiled(const vtx_sota_phase_t *phase, uint32_t method_id)
{
    for (uint32_t i = 0; i < phase->precompiled_count; i++) {
        if (phase->precompiled_methods[i] == method_id) {
            return true;
        }
    }
    return false;
}

/**
 * Add a method to the precompiled list.
 */
static void mark_precompiled(vtx_sota_phase_t *phase, uint32_t method_id)
{
    if (phase->precompiled_count >= phase->precompiled_capacity) {
        /* Can't grow arena-allocated array — just skip */
        return;
    }
    phase->precompiled_methods[phase->precompiled_count++] = method_id;
}

int vtx_sota_phase_predict(vtx_sota_phase_t *phase,
                             vtx_arena_t *arena,
                             uint32_t **methods,
                             uint32_t *count)
{
    if (phase == NULL || arena == NULL || methods == NULL || count == NULL) {
        return -1;
    }

    *methods = NULL;
    *count = 0;

    /* No phase graph → no predictions */
    if (phase->phase_graph == NULL) return 0;

    /* Not enough signature data yet */
    if (phase->current_sig.count < 3) return 0;

    /* Find the best matching phase */
    uint32_t best_phase_id = VTX_PHASE_NONE;
    double best_score = 0.0;

    for (uint32_t p = 0; p < phase->phase_graph->phase_count; p++) {
        const vtx_phase_t *phase_def = &phase->phase_graph->phases[p];
        if (!phase_def->is_significant) continue;

        double score = vtx_sota_phase_match_score(phase, phase_def);
        if (score > best_score) {
            best_score = score;
            best_phase_id = phase_def->phase_id;
        }
    }

    /* Check if the best match exceeds the threshold */
    if (best_score < VTX_PHASE_MATCH_THRESHOLD) {
        /* No good match — if we had a previous prediction, end it */
        if (phase->predicted_phase != VTX_PHASE_NONE) {
            vtx_sota_phase_end_prediction(phase);
        }
        return 0;
    }

    /* Check if this is the same phase we already predicted */
    if (best_phase_id == phase->predicted_phase) {
        /* Same phase — no new methods to pre-compile */
        return 0;
    }

    /* New phase detected — end previous prediction if any */
    if (phase->predicted_phase != VTX_PHASE_NONE) {
        vtx_sota_phase_end_prediction(phase);
    }

    /* Get the phase definition */
    const vtx_phase_t *phase_def = vtx_phase_get_by_id(
        phase->phase_graph, best_phase_id);
    if (phase_def == NULL) return 0;

    /* Collect methods that haven't been pre-compiled yet */
    uint32_t new_count = 0;
    for (uint32_t i = 0; i < phase_def->method_count; i++) {
        if (!is_precompiled(phase, phase_def->method_ids[i])) {
            new_count++;
        }
    }

    if (new_count == 0) return 0;

    /* Allocate output array */
    uint32_t *result = (uint32_t *)vtx_arena_alloc(
        arena, new_count * sizeof(uint32_t));
    if (result == NULL) return -1;

    /* Fill the array */
    uint32_t idx = 0;
    for (uint32_t i = 0; i < phase_def->method_count; i++) {
        uint32_t mid = phase_def->method_ids[i];
        if (!is_precompiled(phase, mid)) {
            result[idx++] = mid;
            mark_precompiled(phase, mid);
        }
    }

    /* Update prediction state */
    phase->predicted_phase = best_phase_id;
    phase->predictions_made++;

    *methods = result;
    *count = idx;
    return 0;
}

int vtx_sota_phase_predict_with_methods(vtx_sota_phase_t *phase,
                                          const uint32_t *current_methods,
                                          uint32_t method_count,
                                          vtx_arena_t *arena,
                                          uint32_t **out_methods,
                                          uint32_t *out_count)
{
    if (phase == NULL || arena == NULL || out_methods == NULL || out_count == NULL) {
        return -1;
    }

    *out_methods = NULL;
    *out_count = 0;

    if (current_methods == NULL || method_count == 0) return 0;

    /* No phase graph → no predictions */
    if (phase->phase_graph == NULL) return 0;

    /* Find the best matching phase using Jaccard similarity */
    uint32_t best_phase_id = VTX_PHASE_NONE;
    double best_score = 0.0;

    for (uint32_t p = 0; p < phase->phase_graph->phase_count; p++) {
        const vtx_phase_t *phase_def = &phase->phase_graph->phases[p];
        if (!phase_def->is_significant) continue;

        double score = vtx_sota_phase_jaccard(
            current_methods, method_count,
            phase_def->method_ids, phase_def->method_count);

        if (score > best_score) {
            best_score = score;
            best_phase_id = phase_def->phase_id;
        }
    }

    /* Check if the best match exceeds the threshold */
    if (best_score < VTX_PHASE_MATCH_THRESHOLD) return 0;

    /* Get the phase definition */
    const vtx_phase_t *phase_def = vtx_phase_get_by_id(
        phase->phase_graph, best_phase_id);
    if (phase_def == NULL) return 0;

    /* Collect methods that haven't been pre-compiled yet */
    uint32_t new_count = 0;
    for (uint32_t i = 0; i < phase_def->method_count; i++) {
        if (!is_precompiled(phase, phase_def->method_ids[i])) {
            new_count++;
        }
    }

    if (new_count == 0) return 0;

    /* Allocate output array */
    uint32_t *result = (uint32_t *)vtx_arena_alloc(
        arena, new_count * sizeof(uint32_t));
    if (result == NULL) return -1;

    /* Fill the array with methods to pre-compile */
    uint32_t idx = 0;
    for (uint32_t i = 0; i < phase_def->method_count; i++) {
        uint32_t mid = phase_def->method_ids[i];
        if (!is_precompiled(phase, mid)) {
            result[idx++] = mid;
            mark_precompiled(phase, mid);
        }
    }

    /* Update prediction state */
    if (best_phase_id != phase->predicted_phase) {
        if (phase->predicted_phase != VTX_PHASE_NONE) {
            vtx_sota_phase_end_prediction(phase);
        }
        phase->predicted_phase = best_phase_id;
        phase->predictions_made++;
    }

    *out_methods = result;
    *out_count = idx;
    return 0;
}

/* ========================================================================== */
/* Feedback                                                                    */
/* ========================================================================== */

void vtx_sota_phase_record_hit(vtx_sota_phase_t *phase, uint32_t method_id)
{
    if (phase == NULL) return;
    /* A pre-compiled method was actually called → prediction was correct */
    phase->predictions_correct++;
}

void vtx_sota_phase_end_prediction(vtx_sota_phase_t *phase)
{
    if (phase == NULL) return;

    if (phase->predicted_phase != VTX_PHASE_NONE) {
        /* Count methods that were pre-compiled but never called as wrong predictions.
         * The difference between precompiled_count and the number of hits
         * gives the number of wrong predictions. */
        uint32_t hits_in_phase = phase->predictions_correct;
        if (phase->precompiled_count > hits_in_phase) {
            phase->predictions_wrong +=
                (phase->precompiled_count - hits_in_phase);
        }
    }

    phase->predicted_phase = VTX_PHASE_NONE;
    phase->precompiled_count = 0;
}
