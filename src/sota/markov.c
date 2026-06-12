/**
 * VORTEX Phase Transition Markov Chain
 *
 * Implements a first-order Markov chain that models phase transitions
 * in program execution. Phases are characterized by dominant method
 * call patterns, and the chain predicts which phase will come next
 * based on the transition probability matrix.
 *
 * Key algorithms:
 *   - vtx_markov_predict_next(): find the phase with highest transition
 *     count from the current phase.
 *   - vtx_markov_detect_transition(): use KL divergence between the
 *     current method call distribution and the current phase's expected
 *     distribution to detect phase changes.
 *   - vtx_markov_predict_hot_methods(): return the methods associated
 *     with the predicted next phase, enabling proactive compilation.
 */

#include "sota/markov.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ========================================================================== */
/* KL divergence helper                                                        */
/* ========================================================================== */

/**
 * Compute the Kullback-Leibler divergence between two discrete distributions.
 * D_KL(P || Q) = sum_i P(i) * log(P(i) / Q(i))
 *
 * Uses a small epsilon to avoid division by zero and log(0).
 * Both distributions must have `n` elements and be non-negative.
 */
static double kl_divergence(const double *p, const double *q, uint32_t n)
{
    const double eps = 1e-10;
    double kl = 0.0;

    for (uint32_t i = 0; i < n; i++) {
        double pi = p[i] + eps;
        double qi = q[i] + eps;
        if (pi > eps) {
            kl += pi * log(pi / qi);
        }
    }

    return kl;
}

/* ========================================================================== */
/* Initialization                                                              */
/* ========================================================================== */

int vtx_markov_init(vtx_markov_t *mk)
{
    if (mk == NULL) return -1;

    memset(mk, 0, sizeof(*mk));

    /* Initialize current phase to 0 (unknown/initialization) */
    mk->current_phase = 0;
    mk->phase_count = 0;
    mk->total_transitions = 0;
    mk->is_trained = false;
    mk->min_observations = 10; /* need at least 10 transitions before
                                  we consider the model trained */

    /* Initialize all phase IDs to invalid */
    for (uint32_t i = 0; i < VTX_MARKOV_MAX_PHASES; i++) {
        mk->phases[i].phase_id = UINT32_MAX;
    }

    /* Initialize method_phase_map to invalid */
    for (uint32_t i = 0; i < VTX_MARKOV_MAX_METHODS; i++) {
        mk->method_phase_map[i] = UINT32_MAX;
    }

    return 0;
}

/* ========================================================================== */
/* Recording observations                                                      */
/* ========================================================================== */

void vtx_markov_record_transition(vtx_markov_t *mk, uint32_t from_phase, uint32_t to_phase)
{
    if (mk == NULL) return;
    if (from_phase >= VTX_MARKOV_MAX_PHASES || to_phase >= VTX_MARKOV_MAX_PHASES) return;

    mk->transition_matrix[from_phase][to_phase]++;
    mk->total_transitions++;
    mk->current_phase = to_phase;

    /* Mark as trained once we have enough observations */
    if (mk->total_transitions >= mk->min_observations) {
        mk->is_trained = true;
    }
}

void vtx_markov_record_method_call(vtx_markov_t *mk, uint32_t method_id)
{
    if (mk == NULL) return;
    if (method_id >= VTX_MARKOV_MAX_METHODS) return;

    mk->current_phase_method_calls[method_id]++;
    mk->current_phase_total_calls++;

    /* Update the method → phase mapping.
     * A method belongs to the phase where it is called most frequently.
     * We track this by assigning the method to the current phase
     * if the current count exceeds the count in its previously
     * assigned phase. */
    uint32_t current_assignment = mk->method_phase_map[method_id];
    if (current_assignment == UINT32_MAX) {
        /* Not yet assigned — assign to current phase */
        mk->method_phase_map[method_id] = mk->current_phase;
    }
    /* Note: In a more sophisticated implementation, we would compare
     * the call frequency in the current phase vs. the previously
     * assigned phase and reassign if necessary. For the first-order
     * model, simple assignment suffices. */
}

/* ========================================================================== */
/* Prediction                                                                  */
/* ========================================================================== */

uint32_t vtx_markov_predict_next(vtx_markov_t *mk, uint32_t current_phase)
{
    if (mk == NULL) return 0;
    if (current_phase >= VTX_MARKOV_MAX_PHASES) return 0;

    /* Find the phase with the highest transition count from current_phase */
    uint32_t best_phase = current_phase; /* default: stay in same phase */
    uint32_t best_count = 0;

    for (uint32_t to = 0; to < VTX_MARKOV_MAX_PHASES; to++) {
        uint32_t count = mk->transition_matrix[current_phase][to];
        if (count > best_count) {
            best_count = count;
            best_phase = to;
        }
    }

    return best_phase;
}

double vtx_markov_transition_prob(vtx_markov_t *mk, uint32_t from, uint32_t to)
{
    if (mk == NULL) return 0.0;
    if (from >= VTX_MARKOV_MAX_PHASES || to >= VTX_MARKOV_MAX_PHASES) return 0.0;

    /* Compute the total transitions from this phase */
    uint64_t total_from = 0;
    for (uint32_t t = 0; t < VTX_MARKOV_MAX_PHASES; t++) {
        total_from += mk->transition_matrix[from][t];
    }

    if (total_from == 0) return 0.0;

    return (double)mk->transition_matrix[from][to] / (double)total_from;
}

uint32_t vtx_markov_predict_hot_methods(vtx_markov_t *mk, uint32_t next_phase,
                                           uint32_t *method_ids, uint32_t max_methods)
{
    if (mk == NULL || method_ids == NULL || max_methods == 0) return 0;
    if (next_phase >= VTX_MARKOV_MAX_PHASES) return 0;

    /* Find the phase descriptor for next_phase */
    const vtx_phase_desc_t *phase = NULL;
    for (uint32_t p = 0; p < mk->phase_count; p++) {
        if (mk->phases[p].phase_id == next_phase) {
            phase = &mk->phases[p];
            break;
        }
    }

    uint32_t count = 0;

    if (phase != NULL) {
        /* Return the dominant methods from the phase descriptor */
        for (uint32_t i = 0; i < phase->dominant_method_count && count < max_methods; i++) {
            method_ids[count++] = phase->dominant_method_ids[i];
        }
    }

    /* Also include any methods that are mapped to this phase
     * via method_phase_map but aren't already in the list. */
    for (uint32_t m = 0; m < VTX_MARKOV_MAX_METHODS && count < max_methods; m++) {
        if (mk->method_phase_map[m] == next_phase) {
            /* Check if already in the list */
            bool already = false;
            for (uint32_t j = 0; j < count; j++) {
                if (method_ids[j] == m) {
                    already = true;
                    break;
                }
            }
            if (!already) {
                method_ids[count++] = m;
            }
        }
    }

    return count;
}

/* ========================================================================== */
/* Phase transition detection                                                  */
/* ========================================================================== */

bool vtx_markov_detect_transition(vtx_markov_t *mk, uint32_t *new_phase)
{
    if (mk == NULL || new_phase == NULL) return false;

    /* Need enough calls in the current observation window to make
     * a meaningful comparison. */
    if (mk->current_phase_total_calls < 50) {
        return false;
    }

    /* Build a distribution of method calls in the current observation window */
    double current_dist[VTX_MARKOV_MAX_METHODS];
    double phase_dist[VTX_MARKOV_MAX_METHODS];
    memset(current_dist, 0, sizeof(current_dist));
    memset(phase_dist, 0, sizeof(phase_dist));

    /* Normalize current method calls into a distribution */
    double current_total = (double)mk->current_phase_total_calls;
    for (uint32_t m = 0; m < VTX_MARKOV_MAX_METHODS; m++) {
        current_dist[m] = (double)mk->current_phase_method_calls[m] / current_total;
    }

    /* For each known phase, compute the expected distribution and
     * measure KL divergence from the current distribution.
     * The phase with the smallest KL divergence is the best match. */
    double best_kl = 1e30;
    uint32_t best_phase = mk->current_phase;
    bool found_better = false;

    for (uint32_t p = 0; p < mk->phase_count; p++) {
        uint32_t pid = mk->phases[p].phase_id;

        /* Build the expected distribution for this phase */
        double phase_total = (double)mk->phases[p].total_calls;
        if (phase_total < 1.0) continue;

        memset(phase_dist, 0, sizeof(phase_dist));
        for (uint32_t j = 0; j < mk->phases[p].dominant_method_count; j++) {
            uint32_t mid = mk->phases[p].dominant_method_ids[j];
            if (mid < VTX_MARKOV_MAX_METHODS) {
                phase_dist[mid] = (double)mk->phases[p].dominant_method_counts[j] / phase_total;
            }
        }

        /* Compute KL divergence: how different is the current distribution
         * from this phase's expected distribution? */
        double kl = kl_divergence(current_dist, phase_dist, VTX_MARKOV_MAX_METHODS);

        if (kl < best_kl) {
            best_kl = kl;
            best_phase = pid;
        }
    }

    /* If the best-matching phase differs from the current phase
     * and the KL divergence is small enough (good match),
     * then a phase transition has likely occurred. */
    double kl_threshold = 2.0; /* empirical threshold */
    if (best_phase != mk->current_phase && best_kl < kl_threshold) {
        *new_phase = best_phase;

        /* Record the transition */
        vtx_markov_record_transition(mk, mk->current_phase, best_phase);

        /* Reset current observation window */
        memset(mk->current_phase_method_calls, 0,
               sizeof(mk->current_phase_method_calls));
        mk->current_phase_total_calls = 0;

        return true;
    }

    return false;
}
