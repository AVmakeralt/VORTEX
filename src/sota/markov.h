#ifndef VORTEX_SOTA_MARKOV_H
#define VORTEX_SOTA_MARKOV_H

#include <stdint.h>
#include <stdbool.h>
#include "vortex_config.h"
#include "runtime/arena.h"

/**
 * VORTEX Phase Transition Markov Chain
 *
 * Predicts the NEXT execution phase based on the current phase,
 * enabling proactive compilation before the phase transition occurs.
 *
 * A "phase" is a distinct period of execution characterized by
 * dominant method call patterns (e.g., "initialization phase",
 * "steady-state computation phase", "I/O phase").
 *
 * The Markov chain models:
 *   P(next_phase | current_phase) = transition_matrix[current][next]
 *
 * When a phase transition is predicted (e.g., from initialization
 * to computation), we can proactively compile the methods that
 * will be needed in the next phase, reducing warmup time.
 */

#define VTX_MARKOV_MAX_PHASES  16
#define VTX_MARKOV_MAX_METHODS 256

/* A phase descriptor */
typedef struct {
    uint32_t phase_id;                    /* unique phase identifier */
    uint32_t dominant_method_ids[8];      /* top 8 methods in this phase */
    uint32_t dominant_method_counts[8];   /* call counts for dominant methods */
    uint32_t dominant_method_count;       /* number of dominant methods */
    uint64_t total_calls;                 /* total calls in this phase */
} vtx_phase_desc_t;

/* Markov chain for phase transitions */
typedef struct {
    /* Transition matrix: transition_matrix[from][to] = count of transitions */
    uint32_t transition_matrix[VTX_MARKOV_MAX_PHASES][VTX_MARKOV_MAX_PHASES];

    /* Current phase */
    uint32_t current_phase;

    /* Phase descriptors */
    vtx_phase_desc_t phases[VTX_MARKOV_MAX_PHASES];
    uint32_t         phase_count;

    /* Method → phase membership (which phase is each method most associated with?) */
    uint32_t method_phase_map[VTX_MARKOV_MAX_METHODS];

    /* Total transitions observed */
    uint64_t total_transitions;

    /* Is the model trained? */
    bool is_trained;

    /* Running method call counts for current phase (for transition detection) */
    uint32_t current_phase_method_calls[VTX_MARKOV_MAX_METHODS];
    uint64_t current_phase_total_calls;

    /* Number of observations needed before we consider the model trained */
    uint32_t min_observations;
} vtx_markov_t;

/* Initialize the Markov chain */
int vtx_markov_init(vtx_markov_t *mk);

/* Record a phase transition observation */
void vtx_markov_record_transition(vtx_markov_t *mk, uint32_t from_phase, uint32_t to_phase);

/* Record a method call in the current phase */
void vtx_markov_record_method_call(vtx_markov_t *mk, uint32_t method_id);

/* Predict the next phase given the current phase */
uint32_t vtx_markov_predict_next(vtx_markov_t *mk, uint32_t current_phase);

/* Get the probability of transitioning from one phase to another */
double vtx_markov_transition_prob(vtx_markov_t *mk, uint32_t from, uint32_t to);

/* Get the methods predicted to be hot in the next phase */
uint32_t vtx_markov_predict_hot_methods(vtx_markov_t *mk, uint32_t next_phase,
                                          uint32_t *method_ids, uint32_t max_methods);

/* Detect if a phase transition has occurred */
bool vtx_markov_detect_transition(vtx_markov_t *mk, uint32_t *new_phase);

#endif /* VORTEX_SOTA_MARKOV_H */
