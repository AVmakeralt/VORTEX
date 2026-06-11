#ifndef VORTEX_PROFILE_PHASE_H
#define VORTEX_PROFILE_PHASE_H

#include <stdint.h>
#include <stdbool.h>
#include "profile/data.h"

/**
 * VORTEX Phase Detection
 *
 * Detects program phases from the call graph in profile data.
 * A "phase" is a strongly connected component (SCC) in the call graph
 * that has more than VTX_PHASE_MIN_METHODS methods and more than
 * VTX_PHASE_MIN_FREQUENCY total invocations.
 *
 * Phase transitions are edges between SCCs — when control flow moves
 * from one phase to another, that is a phase transition.
 *
 * Algorithm: Tarjan's SCC algorithm on the call graph built from
 * profile data, followed by filtering and transition extraction.
 */

/* ========================================================================== */
/* Phase (SCC) representation                                                 */
/* ========================================================================== */

/**
 * A single phase: an SCC in the call graph that meets the size and
 * frequency thresholds.
 */
typedef struct {
    uint32_t   phase_id;        /* unique phase identifier (SCC index) */
    uint32_t  *method_ids;      /* array of method IDs in this phase */
    uint32_t   method_count;    /* number of methods */
    uint32_t   method_capacity; /* allocated capacity */
    uint64_t   total_frequency; /* sum of invocation counts of all methods */
    bool       is_significant;  /* true if method_count >= VTX_PHASE_MIN_METHODS
                                   && total_frequency >= VTX_PHASE_MIN_FREQUENCY */
} vtx_phase_t;

/* ========================================================================== */
/* Phase graph                                                                */
/* ========================================================================== */

/**
 * The phase graph: the result of phase detection.
 * Contains the list of phases and the transitions between them.
 */
typedef struct vtx_phase_graph vtx_phase_graph_t;

struct vtx_phase_graph {
    vtx_phase_t           *phases;        /* array of phases */
    uint32_t               phase_count;
    uint32_t               phase_capacity;

    /* Transitions between phases */
    vtx_phase_transition_t *transitions;
    uint32_t                transition_count;
    uint32_t                transition_capacity;

    /* Method ID → phase ID mapping (dense lookup table) */
    uint32_t              *method_to_phase; /* indexed by method_id, value = phase_id
                                              or VTX_PHASE_NONE if no phase */
    uint32_t               method_to_phase_capacity;

    /* Mapping from method_id to an index in the global method array,
     * used internally by the SCC algorithm. */
};

/* Sentinel: method not in any phase */
#define VTX_PHASE_NONE 0xFFFFFFFFu

/* ========================================================================== */
/* Phase detection                                                            */
/* ========================================================================== */

/**
 * Detect phases from the call graph in the global profile.
 * Returns a newly allocated phase graph (caller must free with
 * vtx_phase_graph_destroy), or NULL on failure.
 *
 * Algorithm:
 *   1. Build adjacency lists from the call edges in the profile.
 *   2. Run Tarjan's SCC algorithm.
 *   3. For each SCC, compute total invocation frequency.
 *   4. Mark SCCs as significant if they meet the thresholds.
 *   5. Phase transitions = inter-SCC edges between significant SCCs.
 */
vtx_phase_graph_t *vtx_phase_detect(const vtx_profile_global_t *global);

/**
 * Destroy a phase graph and free all memory.
 */
void vtx_phase_graph_destroy(vtx_phase_graph_t *graph);

/* ========================================================================== */
/* Phase transition queries                                                   */
/* ========================================================================== */

/**
 * Check if the program is transitioning from one phase to another.
 * A transition occurs when the current top-of-stack method changes
 * from one phase to a different phase.
 *
 * prev_top_method: the method_id of the previous top-of-stack method.
 * curr_top_method: the method_id of the current top-of-stack method.
 *
 * Returns true if there is a phase transition (different phases).
 * Returns false if both methods are in the same phase, or if either
 * method is not in any phase, or if either is VTX_PHASE_NONE.
 */
bool vtx_phase_is_entering(const vtx_phase_graph_t *graph,
                            uint32_t prev_top_method,
                            uint32_t curr_top_method);

/**
 * Look up the phase ID for a method. Returns VTX_PHASE_NONE if not in any phase.
 */
uint32_t vtx_phase_for_method(const vtx_phase_graph_t *graph,
                               uint32_t method_id);

/**
 * Get a phase by its ID. Returns NULL if not found.
 */
const vtx_phase_t *vtx_phase_get_by_id(const vtx_phase_graph_t *graph,
                                         uint32_t phase_id);

#endif /* VORTEX_PROFILE_PHASE_H */
