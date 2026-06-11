#ifndef VORTEX_PEA_VIRTUAL_H
#define VORTEX_PEA_VIRTUAL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vortex_config.h"
#include "ir/graph.h"
#include "pea/analysis.h"
#include "runtime/arena.h"

/**
 * VORTEX Virtual Object Tracking
 *
 * Non-escaping objects are marked as "virtual" — no allocation code is
 * generated for them. All field accesses to virtual objects are rewritten
 * to local variable accesses. Virtual objects exist only in the compiler's
 * analysis; at runtime, they are just a collection of local variables.
 *
 * Key properties:
 *   - Virtual objects never cause heap allocation
 *   - Field accesses to virtual objects are zero-cost (register/stack access)
 *   - Virtual objects are tracked through Phi nodes: a Phi of virtual
 *     objects is virtual if both inputs are virtual with the same type
 *   - If a Phi merges objects of different types or one virtual and one
 *     non-virtual, all inputs must be materialized
 *
 * This pass runs after the base escape analysis and cross-object SR.
 * It finalizes the virtual/non-virtual classification and rewrites
 * all field accesses.
 */

/* ========================================================================== */
/* Virtual object state                                                        */
/* ========================================================================== */

typedef enum {
    VTX_VIRTUAL_UNKNOWN  = 0,  /* not yet classified */
    VTX_VIRTUAL_YES      = 1,  /* confirmed virtual (no allocation needed) */
    VTX_VIRTUAL_NO       = 2   /* not virtual (allocation required) */
} vtx_virtual_state_t;

/* ========================================================================== */
/* Virtual object info                                                         */
/* ========================================================================== */

/**
 * Information about a single virtual object: its type and the mapping
 * from field offsets to the NodeIDs that produce the current field values.
 */
typedef struct {
    vtx_nodeid_t      alloc_id;      /* the allocation node that is virtual */
    uint32_t          type_id;       /* type ID of the virtual object */
    vtx_virtual_state_t state;       /* virtual classification */

    /* Field value mapping: field_offset → NodeID producing the value */
    uint32_t         *field_offsets;   /* array of field offsets */
    vtx_nodeid_t     *field_values;    /* array of value NodeIDs */
    uint32_t          field_count;     /* number of fields */
    uint32_t          field_capacity;  /* allocated capacity */
} vtx_virtual_obj_t;

/* ========================================================================== */
/* Virtual object tracking result                                              */
/* ========================================================================== */

typedef struct {
    /* Per-allocation virtual state: array indexed by NodeID */
    vtx_virtual_state_t *virtual_states;  /* array of virtual classifications */
    uint32_t             state_count;     /* size of virtual_states array */

    /* Virtual object details */
    vtx_virtual_obj_t   *virtual_objs;    /* array of virtual object info */
    uint32_t             virtual_obj_count;
    uint32_t             virtual_obj_capacity;

    /* Statistics */
    uint32_t             total_allocs;          /* total allocations examined */
    uint32_t             virtual_count;         /* objects classified virtual */
    uint32_t             non_virtual_count;     /* objects classified non-virtual */
    uint32_t             phis_resolved;         /* Phi nodes resolved as virtual */
    uint32_t             field_accesses_rewritten; /* field accesses rewritten to locals */
} vtx_virtual_result_t;

/* ========================================================================== */
/* Entry point                                                                 */
/* ========================================================================== */

/**
 * Run virtual object tracking on the graph.
 *
 * This pass:
 *   1. Classifies each NoEscape allocation as virtual
 *   2. Tracks virtual objects through Phi nodes
 *   3. Rewrites all field accesses to virtual objects as local variable access
 *   4. Eliminates allocation nodes for virtual objects
 *
 * Must run after escape analysis and before materialization (materialization
 * handles objects that need to become non-virtual at escape/deopt points).
 *
 * @param graph    The SoN graph (modified in place)
 * @param analysis The escape analysis result
 * @param arena    Arena for allocating temporary data
 * @return         Virtual tracking result, or NULL on failure
 */
vtx_virtual_result_t *vtx_virtual_run(vtx_graph_t *graph,
                                       const vtx_pea_analysis_t *analysis,
                                       vtx_arena_t *arena);

/**
 * Check if an allocation is virtual.
 */
bool vtx_virtual_is_virtual(const vtx_virtual_result_t *result,
                             vtx_nodeid_t alloc_id);

/**
 * Get the current value NodeID for a field of a virtual object.
 * Returns VTX_NODEID_INVALID if the object is not virtual or the
 * field is not tracked.
 */
vtx_nodeid_t vtx_virtual_get_field(const vtx_virtual_result_t *result,
                                    vtx_nodeid_t alloc_id,
                                    uint32_t field_offset);

/**
 * Get the virtual object info for an allocation.
 * Returns NULL if the allocation is not virtual.
 */
const vtx_virtual_obj_t *vtx_virtual_get_obj(const vtx_virtual_result_t *result,
                                               vtx_nodeid_t alloc_id);

#endif /* VORTEX_PEA_VIRTUAL_H */
