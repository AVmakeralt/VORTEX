#ifndef VORTEX_PEA_ANALYSIS_H
#define VORTEX_PEA_ANALYSIS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vortex_config.h"
#include "ir/graph.h"
#include "runtime/arena.h"

/**
 * VORTEX Partial Escape Analysis — Flow-Sensitive Escape Analysis
 *
 * Determines for each allocation whether it escapes the current compilation
 * unit. An allocation "escapes" if it can be observed outside the compiled
 * method — i.e., the allocated object's identity (not just its value) is
 * needed at runtime.
 *
 * Escape states form a lattice:
 *
 *   NoEscape     — the object is never observable outside this method
 *   ArgEscape    — the object escapes through method arguments but not
 *                  globally; callees may observe it but cannot store it
 *                  into a globally-visible location
 *   GlobalEscape — the object is globally visible (stored into a static
 *                  field, returned, or escapes through an unknown callee)
 *
 *   NoEscape ≤ ArgEscape ≤ GlobalEscape
 *
 * The analysis is flow-sensitive: it computes per-basic-block entry/exit
 * states using a standard dataflow framework. At merge points (Region/Phi
 * nodes), states are joined (max over the lattice). The worklist algorithm
 * iterates until a fixed point is reached.
 *
 * Escape triggers:
 *   - Stored into a field of a GlobalEscape object → GlobalEscape
 *   - Stored into a field of an ArgEscape object   → ArgEscape
 *   - Passed as argument to an unknown function     → ArgEscape (conservative:
 *                                                     callee may store globally)
 *   - Returned from the current method               → GlobalEscape
 *   - Stored into a global/static field               → GlobalEscape
 *   - Used in a monitor enter/exit                    → GlobalEscape
 *   - Stored into an array element of escaping array  → propagates array's state
 *   - Used as receiver of a virtual call              → ArgEscape (may dispatch
 *                                                       to unknown method)
 */

/* ========================================================================== */
/* Escape state                                                                */
/* ========================================================================== */

typedef enum {
    VTX_ESCAPE_NONE   = 0,  /* object does not escape */
    VTX_ESCAPE_ARG    = 1,  /* object escapes through args only */
    VTX_ESCAPE_GLOBAL = 2   /* object escapes globally */
} vtx_escape_state_t;

/* Lattice join: take the more conservative (larger) of two states */
static inline vtx_escape_state_t vtx_escape_join(vtx_escape_state_t a,
                                                   vtx_escape_state_t b)
{
    return (a > b) ? a : b;
}

/* Check if an escape state means the object is eligible for scalar replacement */
static inline bool vtx_escape_is_no_escape(vtx_escape_state_t s)
{
    return s == VTX_ESCAPE_NONE;
}

/* Check if an object might be observed outside this compilation unit */
static inline bool vtx_escape_escapes(vtx_escape_state_t s)
{
    return s != VTX_ESCAPE_NONE;
}

/* Human-readable name for escape state */
const char *vtx_escape_state_name(vtx_escape_state_t s);

/* ========================================================================== */
/* Per-allocation state map                                                    */
/* ========================================================================== */

/**
 * Maps allocation NodeIDs to their escape states.
 * Stored as a flat array indexed by NodeID for O(1) lookup.
 *
 * Only allocation nodes (VTX_OP_NewObject, VTX_OP_NewArray, VTX_OP_Allocate)
 * have entries; non-allocation nodes have state VTX_ESCAPE_NONE (unused).
 */
typedef struct {
    vtx_escape_state_t *states;      /* array indexed by node ID */
    uint32_t            state_count; /* size of the states array (= node_table.count) */
    vtx_nodeid_t       *alloc_ids;   /* array of allocation node IDs */
    uint32_t            alloc_count; /* number of allocations found */
    uint32_t            alloc_capacity;
} vtx_escape_map_t;

/* ========================================================================== */
/* Per-basic-block dataflow state                                              */
/* ========================================================================== */

/**
 * The dataflow state for one basic block: a snapshot of the escape map
 * at block entry and exit. The transfer function walks the block's nodes
 * and updates the exit state based on how each node uses allocations.
 */
typedef struct {
    vtx_escape_state_t *entry_state; /* escape state at block entry (array by NodeID) */
    vtx_escape_state_t *exit_state;  /* escape state at block exit (array by NodeID) */
    uint32_t            state_count; /* size of entry/exit arrays */
    bool                entry_changed; /* true if entry state changed on last iteration */
} vtx_pea_block_state_t;

/* ========================================================================== */
/* Analysis result                                                             */
/* ========================================================================== */

/**
 * The result of running PEA on a graph. Contains the final escape state
 * for every allocation, plus the per-block dataflow states for use by
 * downstream transforms (cross-object SR, materialization, virtual objects).
 *
 * All memory is arena-allocated; destroy by freeing the arena.
 */
typedef struct {
    vtx_escape_map_t         escape_map;    /* final per-allocation escape states */
    vtx_pea_block_state_t   *block_states;  /* per-block entry/exit states */
    uint32_t                 block_state_count;

    /* Statistics */
    uint32_t                 total_allocs;      /* number of allocations analyzed */
    uint32_t                 no_escape_count;   /* allocations with NoEscape */
    uint32_t                 arg_escape_count;  /* allocations with ArgEscape */
    uint32_t                 global_escape_count; /* allocations with GlobalEscape */
    uint32_t                 iterations;        /* dataflow iterations to fixed point */
} vtx_pea_analysis_t;

/* ========================================================================== */
/* Analysis entry point                                                        */
/* ========================================================================== */

/**
 * Run flow-sensitive partial escape analysis on the graph.
 *
 * Algorithm:
 *   1. Identify all allocation nodes in the graph.
 *   2. Initialize all allocations to NoEscape.
 *   3. Build per-block entry/exit state arrays.
 *   4. Iterate using a reverse-postorder worklist:
 *      a. For each block, compute entry state as join of predecessor exit states.
 *      b. Apply the transfer function: walk each node, update escape states.
 *      c. If exit state changed, add successors to the worklist.
 *   5. Stop at fixed point (no state changes).
 *
 * The analysis result is allocated from the given arena.
 * Returns the analysis result, or NULL on failure.
 *
 * @param graph  The SoN graph to analyze
 * @param arena  Arena for allocating the result
 * @return       Analysis result, or NULL on failure
 */
vtx_pea_analysis_t *vtx_pea_run(vtx_graph_t *graph, vtx_arena_t *arena);

/* ========================================================================== */
/* Query helpers                                                               */
/* ========================================================================== */

/**
 * Get the escape state for a node from the analysis result.
 * Returns VTX_ESCAPE_GLOBAL for non-allocation nodes (conservative default).
 */
vtx_escape_state_t vtx_pea_get_escape(const vtx_pea_analysis_t *analysis,
                                       vtx_nodeid_t node_id);

/**
 * Check if a node is a scalar-replaceable allocation (NoEscape).
 */
bool vtx_pea_is_scalar_replaceable(const vtx_pea_analysis_t *analysis,
                                    vtx_nodeid_t node_id);

/**
 * Get the entry escape state for a specific allocation in a specific block.
 */
vtx_escape_state_t vtx_pea_block_entry_state(const vtx_pea_analysis_t *analysis,
                                              uint32_t block_idx,
                                              vtx_nodeid_t alloc_id);

/**
 * Get the exit escape state for a specific allocation in a specific block.
 */
vtx_escape_state_t vtx_pea_block_exit_state(const vtx_pea_analysis_t *analysis,
                                             uint32_t block_idx,
                                             vtx_nodeid_t alloc_id);

#endif /* VORTEX_PEA_ANALYSIS_H */
