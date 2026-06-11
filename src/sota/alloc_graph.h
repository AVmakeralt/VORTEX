#ifndef VORTEX_SOTA_ALLOC_GRAPH_H
#define VORTEX_SOTA_ALLOC_GRAPH_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vortex_config.h"
#include "ir/graph.h"
#include "runtime/arena.h"

/**
 * VORTEX SOTA — Allocation Graph Elimination
 *
 * Builds an object graph from field stores and computes "effective escape"
 * for each allocation. If an allocation only escapes through field stores
 * and the field is never read at the escape point, the allocation's
 * effective escape is NoEscape — enabling cross-object scalar replacement.
 *
 * This implements the key SOTA optimization: when object A escapes but
 * object B is only reachable through A's fields, B can be scalar-replaced
 * even though A cannot.
 *
 * Example:
 *   Node a = new Node(x);    // a escapes (returned)
 *   Node b = new Node(y);    // b: only stored in a.next
 *   a.next = b;              // field store: a → b
 *   return a.value + b.value; // b.value read directly
 *
 * Analysis:
 *   - a: GlobalEscape (returned from method)
 *   - b: NoEscape effective (b only escapes through a.next, and
 *     a.next is never read at any escape point of a — b.value is
 *     read directly, which we can rewrite to use y)
 *
 * Algorithm:
 *   1. Build object graph: for each StoreField(NewObject, field, NewObject),
 *      create an edge (store_target → stored_value) labeled with field_offset
 *   2. Run standard escape analysis on all allocations
 *   3. For each allocation with escape > NoEscape:
 *      a. Trace all paths from this allocation to escape points
 *      b. For each field-stored object on these paths:
 *         - Check if the field is ever read at an escape point
 *         - If not, the stored object's effective escape = NoEscape
 *   4. Apply cross-object scalar replacement for NoEscape-effective objects
 */

/* ========================================================================== */
/* Escape state                                                                */
/* ========================================================================== */

typedef enum {
    VTX_ESCAPE_NONE       = 0,  /* does not escape — safe for SR */
    VTX_ESCAPE_ARG        = 1,  /* escapes through arguments only */
    VTX_ESCAPE_GLOBAL     = 2   /* escapes globally — cannot SR */
} vtx_escape_state_t;

/* ========================================================================== */
/* Object graph edge                                                           */
/* ========================================================================== */

/**
 * An edge in the allocation graph: a field store from one allocation
 * to another (or from an allocation to a non-allocation value).
 */
typedef struct {
    vtx_nodeid_t from_alloc;    /* allocation that owns the field */
    vtx_nodeid_t to_alloc;      /* allocation stored into the field (or VTX_NODEID_INVALID) */
    uint32_t     field_offset;  /* field offset of the store */
    vtx_nodeid_t store_node;    /* the StoreField node that created this edge */
} vtx_alloc_edge_t;

/* ========================================================================== */
/* Per-allocation record                                                       */
/* ========================================================================== */

typedef struct {
    vtx_nodeid_t      alloc_node;       /* the allocation node (NewObject/NewArray/Allocate) */
    vtx_escape_state_t escape_state;     /* standard escape analysis result */
    vtx_escape_state_t effective_escape; /* effective escape (considering field reads) */
    bool              is_virtual;        /* true if effectively NoEscape → SR candidate */

    /* Edges from this allocation to other allocations through field stores */
    vtx_alloc_edge_t *out_edges;        /* array of outgoing edges */
    uint32_t          out_edge_count;
    uint32_t          out_edge_capacity;

    /* Edges pointing to this allocation from other allocations */
    vtx_alloc_edge_t *in_edges;         /* array of incoming edges */
    uint32_t          in_edge_count;
    uint32_t          in_edge_capacity;

    /* Fields read from this allocation at escape points */
    uint32_t         *read_fields;      /* field offsets read at escape points */
    uint32_t          read_field_count;
    uint32_t          read_field_capacity;
} vtx_alloc_record_t;

/* ========================================================================== */
/* Allocation graph                                                            */
/* ========================================================================== */

#define VTX_ALLOC_GRAPH_INITIAL_CAPACITY 32

typedef struct {
    vtx_alloc_record_t *records;       /* array of allocation records */
    uint32_t            record_count;
    uint32_t            record_capacity;

    /* All edges in the graph (for iteration) */
    vtx_alloc_edge_t   *all_edges;
    uint32_t            edge_count;
    uint32_t            edge_capacity;

    /* Statistics */
    uint32_t            total_allocations;
    uint32_t            no_escape_count;
    uint32_t            arg_escape_count;
    uint32_t            global_escape_count;
    uint32_t            effective_no_escape_count; /* cross-object SR candidates */
} vtx_alloc_graph_t;

/* ========================================================================== */
/* Build                                                                       */
/* ========================================================================== */

/**
 * Build the allocation graph from a SoN graph.
 *
 * Scans the graph for:
 *   - Allocation nodes (NewObject, NewArray, Allocate)
 *   - StoreField nodes that store into allocation fields
 *   - Field reads at escape points
 *
 * Runs a standard escape analysis pass, then computes effective escape.
 *
 * @param graph  The SoN graph to analyze
 * @param arena  Arena for allocations
 * @return       Populated allocation graph, or NULL on failure
 */
vtx_alloc_graph_t *vtx_alloc_graph_build(const vtx_graph_t *graph,
                                           vtx_arena_t *arena);

/**
 * Destroy an allocation graph.
 */
void vtx_alloc_graph_destroy(vtx_alloc_graph_t *alloc_graph);

/* ========================================================================== */
/* Queries                                                                     */
/* ========================================================================== */

/**
 * Get the effective escape state for an allocation node.
 *
 * This is the key API: returns whether the allocation can be
 * scalar-replaced considering cross-object analysis.
 *
 * @param alloc_graph Allocation graph (must be built)
 * @param alloc_node  The allocation node to query
 * @return            Effective escape state
 */
vtx_escape_state_t vtx_alloc_graph_effective_escape(
    const vtx_alloc_graph_t *alloc_graph,
    vtx_nodeid_t alloc_node);

/**
 * Look up the allocation record for a node.
 * Returns NULL if the node is not an allocation.
 */
const vtx_alloc_record_t *vtx_alloc_graph_lookup(
    const vtx_alloc_graph_t *alloc_graph,
    vtx_nodeid_t alloc_node);

/**
 * Check if an allocation can be scalar-replaced.
 * An allocation can be SR'd if its effective escape is NoEscape.
 */
bool vtx_alloc_graph_can_scalar_replace(
    const vtx_alloc_graph_t *alloc_graph,
    vtx_nodeid_t alloc_node);

/**
 * Get all scalar replacement candidates.
 * Returns the count of allocations with effective escape = NoEscape.
 * If output array is provided, fills it with their node IDs.
 *
 * @param alloc_graph  Allocation graph
 * @param[out] candidates Array to fill with SR candidate node IDs (may be NULL)
 * @param capacity     Capacity of the candidates array
 * @return             Number of SR candidates
 */
uint32_t vtx_alloc_graph_sr_candidates(
    const vtx_alloc_graph_t *alloc_graph,
    vtx_nodeid_t *candidates,
    uint32_t capacity);

/* ========================================================================== */
/* Scalar Replacement Application                                              */
/* ========================================================================== */

/**
 * Apply cross-object scalar replacement to the SoN graph.
 *
 * For each allocation with effective escape = NoEscape:
 *   1. Replace the allocation node with scalar locals for each field
 *   2. Replace LoadField nodes with reads from the corresponding scalar local
 *   3. Replace StoreField nodes with writes to the corresponding scalar local
 *   4. For allocations that escape only through unreferenced fields,
 *      eliminate the field stores entirely (dead store elimination)
 *   5. At deopt points where the allocation must be materialized,
 *      insert NewObject + StoreField sequences to reify the scalar locals
 *
 * For each allocation with effective escape = NoEscape that is stored
 * into a container's unreferenced field (cross-object SR):
 *   - The allocation is eliminated (no heap allocation)
 *   - Direct field reads of the allocation are rewritten to scalar locals
 *   - The container's field store of this allocation is removed (dead store)
 *
 * @param graph       The SoN graph to transform (modified in place)
 * @param alloc_graph Pre-built allocation graph with effective escape computed
 * @param arena       Arena for temporary allocations
 * @return            Number of allocations eliminated by scalar replacement
 */
uint32_t vtx_alloc_graph_apply_sr(vtx_graph_t *graph,
                                    vtx_alloc_graph_t *alloc_graph,
                                    vtx_arena_t *arena);

#endif /* VORTEX_SOTA_ALLOC_GRAPH_H */
