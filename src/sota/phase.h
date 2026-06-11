#ifndef VORTEX_SOTA_PHASE_H
#define VORTEX_SOTA_PHASE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vortex_config.h"
#include "profile/data.h"
#include "profile/phase.h"
#include "runtime/arena.h"

/**
 * VORTEX SOTA — Phase Detection + Preemptive Compilation
 *
 * Detects program phases from previous run profiles and preemptively
 * compiles hot methods before they are individually hot in the current run.
 *
 * A "phase" is a set of methods that execute together frequently (detected
 * as SCCs in the call graph). When the current method call pattern matches
 * a known phase signature, all methods in that phase are queued for
 * T2/T3 compilation immediately, even if their individual invocation
 * counts are still low.
 *
 * Wrong predictions have zero cost: compiled code that is never used
 * simply occupies code cache space until evicted. There is no runtime
 * overhead for unused compiled code (it's never entered).
 *
 * Algorithm:
 *   1. At startup, load the phase graph from the previous run's profile
 *   2. Monitor the current top-N method call signatures
 *   3. Compute Jaccard similarity between current signature and each
 *      known phase's method set
 *   4. When similarity exceeds VTX_PHASE_MATCH_THRESHOLD, predict that
 *      the program is entering that phase
 *   5. Queue all methods in the predicted phase for compilation
 *   6. Continue monitoring — if the prediction was wrong (methods don't
 *      get called), the compiled code is simply unused (zero cost)
 */

/* ========================================================================== */
/* Configuration                                                               */
/* ========================================================================== */

/* Minimum Jaccard similarity to trigger phase prediction.
 * A value of 0.5 means at least half the methods in the current
 * signature must match a known phase. This balances responsiveness
 * (detecting phases early) with precision (avoiding false predictions). */
#define VTX_PHASE_MATCH_THRESHOLD 0.5

/* Number of recent methods to track for phase matching.
 * A sliding window of the last N method entries. */
#define VTX_PHASE_SIGNATURE_SIZE 10

/* ========================================================================== */
/* Phase signature                                                             */
/* ========================================================================== */

/**
 * A snapshot of the currently executing methods, used to match
 * against known phases.
 */
typedef struct {
    uint32_t method_ids[VTX_PHASE_SIGNATURE_SIZE]; /* recent method IDs */
    uint32_t count;                                 /* number of entries */
    uint32_t insert_pos;                            /* circular buffer position */
} vtx_phase_signature_t;

/* ========================================================================== */
/* Phase prediction state                                                      */
/* ========================================================================== */

typedef struct {
    /* The loaded phase graph from the previous run */
    vtx_phase_graph_t *phase_graph;

    /* Current method signature (sliding window) */
    vtx_phase_signature_t current_sig;

    /* Currently predicted phase (VTX_PHASE_NONE if no prediction) */
    uint32_t predicted_phase;

    /* Methods already pre-compiled in the current phase prediction */
    uint32_t *precompiled_methods;
    uint32_t  precompiled_count;
    uint32_t  precompiled_capacity;

    /* Statistics */
    uint32_t predictions_made;
    uint32_t predictions_correct;  /* methods in predicted phase were actually called */
    uint32_t predictions_wrong;    /* methods in predicted phase were never called */
} vtx_sota_phase_t;

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

/**
 * Initialize the phase prediction system.
 * If phase_graph is non-NULL, it is used for predictions (takes ownership).
 * If NULL, predictions are disabled until a graph is loaded.
 *
 * @param phase       Phase prediction state to initialize
 * @param phase_graph Phase graph from previous run (may be NULL)
 * @param arena       Arena for allocations
 * @return            0 on success, -1 on failure
 */
int vtx_sota_phase_init(vtx_sota_phase_t *phase,
                         vtx_phase_graph_t *phase_graph,
                         vtx_arena_t *arena);

/**
 * Destroy the phase prediction system.
 * Does NOT destroy the phase_graph (caller owns it).
 */
void vtx_sota_phase_destroy(vtx_sota_phase_t *phase);

/* ========================================================================== */
/* Monitoring                                                                  */
/* ========================================================================== */

/**
 * Record a method entry for phase monitoring.
 * Updates the current signature with the new method ID.
 *
 * @param phase     Phase prediction state
 * @param method_id Method that was just entered
 */
void vtx_sota_phase_record_method(vtx_sota_phase_t *phase, uint32_t method_id);

/**
 * Update the phase graph with a newly entered method.
 *
 * When a method is entered at the top level, this function:
 *   1. Adds the method to the sliding window signature
 *   2. Checks if the current method's phase matches the predicted phase
 *   3. If a phase transition is detected, triggers prediction evaluation
 *   4. If the method was pre-compiled by a phase prediction, records a hit
 *
 * This is the primary entry point called from the interpreter/JIT at each
 * method entry to keep the phase monitor up-to-date.
 *
 * @param phase     Phase prediction state
 * @param entered_method Method that was just entered
 */
void vtx_sota_phase_update(vtx_sota_phase_t *phase, uint32_t entered_method);

/**
 * Predict which methods should be pre-compiled based on the
 * current method call pattern.
 *
 * Compares the current signature against all known phases
 * using Jaccard similarity. Returns the method IDs that
 * should be queued for compilation.
 *
 * @param phase         Phase prediction state
 * @param[out] methods  Array of method IDs to pre-compile (arena-allocated)
 * @param[out] count    Number of methods in the array
 * @return              0 on success, -1 on failure
 */
int vtx_sota_phase_predict(vtx_sota_phase_t *phase,
                             vtx_arena_t *arena,
                             uint32_t **methods,
                             uint32_t *count);

/**
 * Predict which methods should be pre-compiled based on an explicit
 * set of current top-level method call patterns.
 *
 * This variant accepts an explicit array of current method IDs
 * rather than using the internal sliding window. Useful when
 * the caller has already computed the hot method set.
 *
 * Computes Jaccard similarity between the provided method set
 * and each known phase's method set. Returns the method IDs
 * from the best-matching phase that should be queued for
 * T2/T3 compilation.
 *
 * @param phase            Phase prediction state
 * @param current_methods  Array of current top-level method IDs
 * @param method_count     Number of methods in the array
 * @param arena            Arena for output allocation
 * @param[out] out_methods Array of method IDs to pre-compile (arena-allocated)
 * @param[out] out_count   Number of methods in the output array
 * @return                 0 on success, -1 on failure
 */
int vtx_sota_phase_predict_with_methods(vtx_sota_phase_t *phase,
                                          const uint32_t *current_methods,
                                          uint32_t method_count,
                                          vtx_arena_t *arena,
                                          uint32_t **out_methods,
                                          uint32_t *out_count);

/* ========================================================================== */
/* Similarity computation                                                      */
/* ========================================================================== */

/**
 * Compute Jaccard similarity between two sets of method IDs.
 *
 * Jaccard(A, B) = |A ∩ B| / |A ∪ B|
 *
 * Returns a value in [0, 1]. 1.0 means identical sets.
 */
double vtx_sota_phase_jaccard(const uint32_t *set_a, uint32_t count_a,
                                const uint32_t *set_b, uint32_t count_b);

/**
 * Compute Jaccard similarity between the current signature
 * and a phase's method set.
 */
double vtx_sota_phase_match_score(const vtx_sota_phase_t *phase,
                                    const vtx_phase_t *phase_def);

/* ========================================================================== */
/* Feedback                                                                    */
/* ========================================================================== */

/**
 * Record that a pre-compiled method was actually called (prediction correct).
 *
 * @param phase     Phase prediction state
 * @param method_id Method that was called
 */
void vtx_sota_phase_record_hit(vtx_sota_phase_t *phase, uint32_t method_id);

/**
 * Record that the current phase prediction has ended
 * (e.g., the program has moved to a different phase).
 * Updates prediction accuracy statistics.
 *
 * @param phase Phase prediction state
 */
void vtx_sota_phase_end_prediction(vtx_sota_phase_t *phase);

#endif /* VORTEX_SOTA_PHASE_H */
