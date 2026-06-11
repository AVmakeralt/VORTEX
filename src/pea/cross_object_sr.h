#ifndef VORTEX_PEA_CROSS_OBJECT_SR_H
#define VORTEX_PEA_CROSS_OBJECT_SR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vortex_config.h"
#include "ir/graph.h"
#include "pea/analysis.h"
#include "runtime/arena.h"

/**
 * VORTEX Cross-Object Scalar Replacement
 *
 * Extends basic scalar replacement to handle objects that are reachable
 * only through fields of other (escaping) objects. This is the key SOTA
 * optimization: even if object A escapes, object B stored in A's field
 * may not escape independently — it can be scalar-replaced.
 *
 * Algorithm:
 *   1. Build an allocation graph: nodes = allocations, edges = field stores
 *      between them. An edge A → B (via field f) means "A.f = B" exists.
 *   2. For each allocation, compute "effective escape": trace all paths
 *      from the allocation to actual escape points. If the ONLY escaping
 *      path goes through a field store into an escaping container AND the
 *      field is never read at the escape point, the effective escape is
 *      NoEscape.
 *   3. For allocations with effective NoEscape:
 *      a. Replace the allocation node with scalar locals (one per field).
 *      b. Rewrite all LoadField/StoreField accesses through the container
 *         to use the scalar locals directly.
 *      c. Remove the allocation if all its uses are rewritten.
 *
 * Example:
 *   Node a = new Node(x);   // a escapes (returned)
 *   Node b = new Node(y);   // b stored into a.next
 *   a.next = b;
 *   return a.value + b.value;
 *
 *   a escapes → not scalar-replaceable
 *   b's only escape path: a.next = b (field store into escaping a)
 *   b.value is read directly (not through a.next.value at escape point)
 *   → b's effective escape = NoEscape → scalar-replace b
 *   → rewrite b.value to y, remove b's allocation
 */

/* ========================================================================== */
/* Allocation graph                                                            */
/* ========================================================================== */

/**
 * An edge in the allocation graph: a field store from one allocation
 * to another (container.field = value).
 */
typedef struct vtx_alloc_edge vtx_alloc_edge_t;

struct vtx_alloc_edge {
    vtx_nodeid_t  container_id;  /* the allocation that receives the field store */
    vtx_nodeid_t  value_id;      /* the allocation stored into the field */
    uint32_t      field_offset;  /* the field offset of the store */
    vtx_nodeid_t  store_node_id; /* the StoreField node that creates this edge */
    vtx_alloc_edge_t *next;      /* linked list of edges from the same container */
};

/**
 * The allocation graph: maps each allocation to the list of outgoing
 * edges (field stores from this allocation to other allocations).
 */
typedef struct {
    /* Adjacency list: alloc_edges[alloc_id] = head of edge list from this alloc */
    vtx_alloc_edge_t **alloc_edges;  /* array indexed by node ID */
    uint32_t           edge_array_size; /* size of alloc_edges array */

    /* All edges (for iteration) */
    vtx_alloc_edge_t  *all_edges;     /* contiguous array of edges */
    uint32_t           edge_count;
    uint32_t           edge_capacity;

    /* Reverse adjacency: for each allocation, which edges point TO it */
    vtx_alloc_edge_t **reverse_edges;  /* array indexed by node ID */
    uint32_t           reverse_array_size;
} vtx_alloc_graph_t;

/* ========================================================================== */
/* Effective escape result                                                     */
/* ========================================================================== */

/**
 * For each allocation, the effective escape state after cross-object
 * analysis. This may be lower than the raw escape state from the
 * base analysis: an ArgEscape or GlobalEscape object may have effective
 * NoEscape if its only escape path goes through a container field that
 * is never read at the escape point.
 */
typedef struct {
    vtx_escape_state_t *effective_states; /* array indexed by node ID */
    uint32_t            state_count;
    uint32_t           *scalar_fields;  /* for each alloc: count of scalar-replaced fields */
    uint32_t            scalar_field_count; /* size of scalar_fields array */
} vtx_effective_escape_t;

/* ========================================================================== */
/* Scalar replacement mapping                                                  */
/* ========================================================================== */

/**
 * Maps a (allocation, field_offset) pair to the NodeID of the scalar
 * local variable that replaces the field access. Used to rewrite
 * LoadField/StoreField nodes.
 */
typedef struct {
    vtx_nodeid_t  alloc_id;     /* the scalar-replaced allocation */
    uint32_t      field_offset; /* the field offset */
    vtx_nodeid_t  local_id;     /* the scalar local variable node ID */
} vtx_sr_mapping_t;

/**
 * The complete scalar replacement result: all mappings from
 * (alloc, field) → local variable.
 */
typedef struct {
    vtx_sr_mapping_t *mappings;     /* array of mappings */
    uint32_t          mapping_count;
    uint32_t          mapping_capacity;

    /* Statistics */
    uint32_t          allocs_replaced;    /* number of allocations eliminated */
    uint32_t          field_accesses_rewritten; /* number of field accesses rewritten */
    uint32_t          edges_analyzed;     /* allocation graph edges analyzed */
} vtx_cross_sr_result_t;

/* ========================================================================== */
/* Entry point                                                                 */
/* ========================================================================== */

/**
 * Run cross-object scalar replacement on the graph.
 *
 * This transforms the graph in place:
 *   - Creates scalar local variable nodes for each field of
 *     scalar-replaceable allocations
 *   - Rewrites LoadField/StoreField accesses to use the locals
 *   - Removes allocation nodes for fully scalar-replaced objects
 *
 * @param graph    The SoN graph (modified in place)
 * @param analysis The escape analysis result from vtx_pea_run()
 * @param arena    Arena for allocating temporary data
 * @return         Result structure, or NULL on failure
 */
vtx_cross_sr_result_t *vtx_cross_object_sr_run(vtx_graph_t *graph,
                                                 const vtx_pea_analysis_t *analysis,
                                                 vtx_arena_t *arena);

/**
 * Get the scalar local NodeID for a given (allocation, field) pair.
 * Returns VTX_NODEID_INVALID if no mapping exists.
 */
vtx_nodeid_t vtx_cross_sr_get_local(const vtx_cross_sr_result_t *result,
                                      vtx_nodeid_t alloc_id,
                                      uint32_t field_offset);

/**
 * Check if an allocation was fully scalar-replaced.
 */
bool vtx_cross_sr_is_replaced(const vtx_cross_sr_result_t *result,
                               vtx_nodeid_t alloc_id);

#endif /* VORTEX_PEA_CROSS_OBJECT_SR_H */
