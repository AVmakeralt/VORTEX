#ifndef VORTEX_PEA_MATERIALIZE_H
#define VORTEX_PEA_MATERIALIZE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vortex_config.h"
#include "ir/graph.h"
#include "pea/analysis.h"
#include "pea/virtual.h"
#include "runtime/arena.h"

/**
 * VORTEX Object Materialization
 *
 * At deoptimization points and escape points, scalar-replaced objects must
 * be reified (materialized) back into real heap objects. This pass inserts
 * NewObject + StoreField nodes into the SoN graph at those points.
 *
 * Materialization is lazy: we only materialize at points where the object
 * actually escapes or where deoptimization requires a concrete heap object.
 * If an object is never used in a way that requires heap identity, no
 * materialization code is generated.
 *
 * When is materialization needed?
 *   1. At Deopt/DeoptGuard nodes: the interpreter expects real objects
 *      on the stack and in locals. If a scalar-replaced object appears
 *      in a FrameState, it must be materialized.
 *   2. At escape points: if an object escapes (e.g., passed as argument
 *      to an unknown function), it must be materialized before the
 *      escape point.
 *   3. At Phi merge points: if a Phi merges a scalar-replaced object
 *      with a non-scalar object, the scalar-replaced one must be
 *      materialized.
 *
 * Materialization code generated for each object:
 *   NewObject(type_id)           → allocate on heap
 *   StoreField(obj, f0, v0)      → store scalar local value into field 0
 *   StoreField(obj, f1, v1)      → store scalar local value into field 1
 *   ...                           → one StoreField per field
 *
 * The resulting heap object is indistinguishable from one that was
 * never scalar-replaced: same type, same field values, same identity
 * (new allocation).
 */

/* ========================================================================== */
/* Materialization point                                                       */
/* ========================================================================== */

/**
 * Describes a single point where a scalar-replaced object must be
 * materialized. Contains the node where materialization is inserted
 * and the list of fields to store.
 */
typedef struct {
    vtx_nodeid_t  escape_node_id;    /* the node at which materialization is needed */
    vtx_nodeid_t  alloc_id;          /* the scalar-replaced allocation to materialize */
    uint32_t      type_id;           /* type ID for the NewObject node */

    /* Field values to store: pairs of (field_offset, local_node_id) */
    uint32_t     *field_offsets;     /* array of field offsets */
    vtx_nodeid_t *field_local_ids;   /* array of scalar local node IDs */
    uint32_t      field_count;       /* number of fields */

    /* The NewObject node created by materialization */
    vtx_nodeid_t  materialized_obj_id; /* NewObject node ID (set during materialization) */

    /* For Phi materialization: the predecessor block's terminal control node.
     * The NewObject + StoreField sequence is anchored to this control node,
     * ensuring the scheduler places the materialization in the predecessor
     * block rather than at the Phi itself. VTX_NODEID_INVALID for non-Phi
     * materializations. */
    vtx_nodeid_t  predecessor_control; /* control input from predecessor block */
} vtx_materialize_point_t;

/* ========================================================================== */
/* Materialization result                                                      */
/* ========================================================================== */

typedef struct {
    vtx_materialize_point_t *points;     /* array of materialization points */
    uint32_t                 point_count;
    uint32_t                 point_capacity;

    /* Statistics */
    uint32_t                 objects_materialized;  /* total objects materialized */
    uint32_t                 fields_stored;         /* total StoreField nodes inserted */
    uint32_t                 deopt_points_handled;  /* deopt points requiring materialization */
} vtx_materialize_result_t;

/* ========================================================================== */
/* Entry point                                                                 */
/* ========================================================================== */

/**
 * Run the materialization pass on the graph.
 *
 * Scans for deopt points and escape points that reference scalar-replaced
 * objects, and inserts NewObject + StoreField nodes to reify those objects.
 *
 * This pass must run AFTER cross-object scalar replacement, so it can
 * identify which objects were scalar-replaced and what their field values
 * are.
 *
 * @param graph          The SoN graph (modified in place — new nodes inserted)
 * @param analysis       The escape analysis result
 * @param virtual_result The virtual object tracking result (may be NULL if
 *                       virtual pass was not run; when provided, field values
 *                       are read from virtual field maps instead of dead
 *                       StoreField nodes)
 * @param arena          Arena for allocating temporary data
 * @return               Materialization result, or NULL on failure
 */
vtx_materialize_result_t *vtx_materialize_run(vtx_graph_t *graph,
                                                const vtx_pea_analysis_t *analysis,
                                                const vtx_virtual_result_t *virtual_result,
                                                vtx_arena_t *arena);

/**
 * Get the materialized object NodeID for a given allocation at a given
 * escape point. Returns VTX_NODEID_INVALID if the object was not
 * materialized at that point.
 */
vtx_nodeid_t vtx_materialize_get_obj(const vtx_materialize_result_t *result,
                                      vtx_nodeid_t alloc_id,
                                      vtx_nodeid_t escape_node_id);

/**
 * Check if an allocation was materialized at any point.
 */
bool vtx_materialize_is_materialized(const vtx_materialize_result_t *result,
                                      vtx_nodeid_t alloc_id);

#endif /* VORTEX_PEA_MATERIALIZE_H */
