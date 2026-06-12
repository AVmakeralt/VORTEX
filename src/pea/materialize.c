/**
 * VORTEX Object Materialization
 *
 * Inserts NewObject + StoreField nodes at escape/deopt points to reify
 * scalar-replaced objects back into real heap objects.
 */

#include "pea/materialize.h"
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* Internal: check if opcode is an allocation                                  */
/* ========================================================================== */

static inline bool is_allocation(vtx_node_opcode_t opcode)
{
    return opcode == VTX_OP_NewObject ||
           opcode == VTX_OP_NewArray  ||
           opcode == VTX_OP_Allocate;
}

/* ========================================================================== */
/* Internal: find all field values for a scalar-replaced allocation            */
/* ========================================================================== */

/**
 * Collect the current values of all fields of a scalar-replaced allocation.
 * For each StoreField that targets this allocation, record the stored value.
 * Returns the field count and fills the offsets/values arrays.
 */
static uint32_t collect_field_values(vtx_graph_t *graph, vtx_nodeid_t alloc_id,
                                      uint32_t *offsets, vtx_nodeid_t *values,
                                      uint32_t max_fields)
{
    vtx_node_table_t *table = &graph->node_table;
    uint32_t count = 0;

    for (uint32_t i = 0; i < table->count; i++) {
        vtx_node_t *node = &table->nodes[i];
        if (node->dead) continue;
        if (node->opcode != VTX_OP_StoreField) continue;
        if (node->input_count < 2) continue;

        vtx_nodeid_t receiver_id = node->inputs[node->input_count - 2];
        vtx_nodeid_t value_id    = node->inputs[node->input_count - 1];

        if (receiver_id != alloc_id) continue;

        /* Check if this field offset is already recorded (last write wins) */
        bool found = false;
        for (uint32_t k = 0; k < count; k++) {
            if (offsets[k] == node->field_offset) {
                values[k] = value_id; /* update: last write wins */
                found = true;
                break;
            }
        }

        if (!found && count < max_fields) {
            offsets[count] = node->field_offset;
            values[count]  = value_id;
            count++;
        }
    }

    return count;
}

/* ========================================================================== */
/* Internal: collect field values from virtual object tracking                 */
/* ========================================================================== */

/**
 * Collect field values from the virtual object's field map.
 * This is used when virtual.c has already rewritten StoreField nodes
 * (marking them dead) and moved field values into the virtual object's
 * field map. Reading from dead StoreField nodes would yield stale values,
 * so we read from the virtual result instead.
 */
static uint32_t collect_field_values_from_virtual(
    const vtx_virtual_result_t *virtual_result,
    vtx_nodeid_t alloc_id,
    uint32_t *offsets,
    vtx_nodeid_t *values,
    uint32_t max_fields)
{
    const vtx_virtual_obj_t *vobj = vtx_virtual_get_obj(virtual_result, alloc_id);
    if (vobj == NULL) return 0;

    uint32_t count = 0;
    for (uint32_t f = 0; f < vobj->field_count && count < max_fields; f++) {
        offsets[count] = vobj->field_offsets[f];
        values[count]  = vobj->field_values[f];
        count++;
    }
    return count;
}

/* ========================================================================== */
/* Internal: check if a node references a scalar-replaced allocation           */
/* ========================================================================== */

/**
 * Check if any input of the given node is a scalar-replaced allocation.
 * Returns the allocation NodeID if found, VTX_NODEID_INVALID otherwise.
 */
static vtx_nodeid_t find_scalar_replaced_input(vtx_node_t *node,
                                                 const vtx_pea_analysis_t *analysis,
                                                 vtx_node_table_t *table)
{
    for (uint32_t i = 0; i < node->input_count; i++) {
        vtx_nodeid_t input_id = node->inputs[i];
        vtx_node_t *input = vtx_node_get(table, input_id);
        if (!input || input->dead) continue;

        if (is_allocation(input->opcode) &&
            vtx_pea_is_scalar_replaceable(analysis, input_id)) {
            return input_id;
        }
    }
    return VTX_NODEID_INVALID;
}

/* ========================================================================== */
/* Internal: add a materialization point                                       */
/* ========================================================================== */

static vtx_materialize_point_t *add_materialize_point(
    vtx_materialize_result_t *result, vtx_arena_t *arena)
{
    if (result->point_count >= result->point_capacity) {
        uint32_t new_cap = result->point_capacity == 0 ? 16 :
                           result->point_capacity * 2;
        vtx_materialize_point_t *new_pts = vtx_arena_alloc(arena,
            new_cap * sizeof(vtx_materialize_point_t));
        if (!new_pts) return NULL;
        if (result->points && result->point_count > 0) {
            memcpy(new_pts, result->points,
                   result->point_count * sizeof(vtx_materialize_point_t));
        }
        result->points = new_pts;
        result->point_capacity = new_cap;
    }

    vtx_materialize_point_t *pt = &result->points[result->point_count];
    memset(pt, 0, sizeof(*pt));
    result->point_count++;
    return pt;
}

/* ========================================================================== */
/* Internal: insert materialization code into the graph                        */
/* ========================================================================== */

/**
 * Insert NewObject + StoreField nodes for a single materialization point.
 * The materialized object node replaces references to the scalar-replaced
 * allocation at the escape point.
 *
 * If pt->predecessor_control is not VTX_NODEID_INVALID, the NewObject
 * node is anchored to that control node as its first input. This ensures
 * the scheduler places the materialization in the predecessor block
 * (correct for Phi merge-point materialization per SSA semantics).
 * The last StoreField node becomes the new memory state for subsequent
 * nodes in the predecessor block.
 */
static int insert_materialization_code(vtx_graph_t *graph,
                                        vtx_materialize_point_t *pt,
                                        vtx_arena_t *arena)
{
    vtx_node_table_t *table = &graph->node_table;

    /* Create NewObject node */
    vtx_nodeid_t new_obj_id = vtx_node_create(table, VTX_OP_NewObject);
    if (new_obj_id == VTX_NODEID_INVALID) return -1;

    vtx_node_t *new_obj = vtx_node_get(table, new_obj_id);
    new_obj->type_id = pt->type_id;
    new_obj->type    = VTX_TYPE_Ptr;
    new_obj->flags   = vtx_nf_union(VTX_NF_SIDE_EFFECT, VTX_NF_MEMORY);

    /* If a predecessor control node is specified, add it as the first
     * input to the NewObject. This anchors the allocation to the
     * predecessor block's control flow, ensuring the scheduler places
     * it in the correct block. */
    if (pt->predecessor_control != VTX_NODEID_INVALID) {
        vtx_node_add_input(table, new_obj_id, pt->predecessor_control);
    }

    pt->materialized_obj_id = new_obj_id;

    /* Track the current memory state for chaining StoreField nodes.
     * Initially, the NewObject produces the new memory state. */
    vtx_nodeid_t mem_state = new_obj_id;

    /* Create StoreField nodes for each field */
    for (uint32_t f = 0; f < pt->field_count; f++) {
        vtx_nodeid_t store_id = vtx_node_create(table, VTX_OP_StoreField);
        if (store_id == VTX_NODEID_INVALID) return -1;

        vtx_node_t *store = vtx_node_get(table, store_id);
        store->field_offset = pt->field_offsets[f];
        store->type    = VTX_TYPE_Void;
        store->flags   = vtx_nf_union(VTX_NF_SIDE_EFFECT, VTX_NF_MEMORY);

        /* Add inputs: memory chain, receiver (new object), value (scalar local).
         * The memory chain is threaded: each StoreField takes the previous
         * memory state as input, ensuring correct ordering. */
        vtx_node_add_input(table, store_id, mem_state);  /* memory chain */
        vtx_node_add_input(table, store_id, new_obj_id); /* receiver */
        vtx_node_add_input(table, store_id, pt->field_local_ids[f]); /* value */

        /* This StoreField becomes the new memory state */
        mem_state = store_id;
    }

    return 0;
}

/* ========================================================================== */
/* Main entry point                                                            */
/* ========================================================================== */

vtx_materialize_result_t *vtx_materialize_run(vtx_graph_t *graph,
                                                const vtx_pea_analysis_t *analysis,
                                                const vtx_virtual_result_t *virtual_result,
                                                vtx_arena_t *arena)
{
    VTX_ASSERT(graph != NULL, "graph must not be NULL");
    VTX_ASSERT(analysis != NULL, "analysis must not be NULL");
    VTX_ASSERT(arena != NULL, "arena must not be NULL");

    vtx_node_table_t *table = &graph->node_table;

    /* Allocate result */
    vtx_materialize_result_t *result = vtx_arena_alloc(arena,
        sizeof(vtx_materialize_result_t));
    if (!result) return NULL;
    memset(result, 0, sizeof(*result));

    /* Maximum fields per object (conservative bound) */
    const uint32_t MAX_FIELDS_PER_OBJ = 32;

    /* Scan for escape/deopt points that reference scalar-replaced allocations */
    for (uint32_t i = 0; i < table->count; i++) {
        vtx_node_t *node = &table->nodes[i];
        if (node->dead) continue;

        /* Check if this node is a deopt point or escape point */
        bool is_escape_point = false;
        bool is_deopt_point = false;

        switch (node->opcode) {
        case VTX_OP_Deopt:
        case VTX_OP_DeoptGuard:
            is_deopt_point = true;
            is_escape_point = true;
            break;
        case VTX_OP_CallStatic:
        case VTX_OP_CallVirtual:
        case VTX_OP_CallInterface:
        case VTX_OP_CallRuntime:
            is_escape_point = true;
            break;
        case VTX_OP_Return:
            is_escape_point = true;
            break;
        default:
            break;
        }

        if (!is_escape_point) continue;

        /* Find scalar-replaced allocations referenced by this node */
        for (uint32_t inp = 0; inp < node->input_count; inp++) {
            vtx_nodeid_t input_id = node->inputs[inp];
            vtx_node_t *input_node = vtx_node_get(table, input_id);
            if (!input_node || input_node->dead) continue;

            /* Check if this input is a scalar-replaced allocation */
            if (!is_allocation(input_node->opcode)) continue;
            if (!vtx_pea_is_scalar_replaceable(analysis, input_id)) continue;

            /* This allocation is scalar-replaced and referenced at an
             * escape/deopt point — it must be materialized. */

            /* Check if we already have a materialization point for this
             * (alloc, escape_node) pair */
            bool already_materialized = false;
            for (uint32_t p = 0; p < result->point_count; p++) {
                if (result->points[p].alloc_id == input_id &&
                    result->points[p].escape_node_id == node->id) {
                    already_materialized = true;
                    break;
                }
            }
            if (already_materialized) continue;

            /* Create a new materialization point */
            vtx_materialize_point_t *pt = add_materialize_point(result, arena);
            if (!pt) return NULL;

            pt->escape_node_id = node->id;
            pt->alloc_id       = input_id;
            pt->type_id        = input_node->type_id;

            /* Collect field values */
            pt->field_offsets = vtx_arena_alloc(arena,
                MAX_FIELDS_PER_OBJ * sizeof(uint32_t));
            pt->field_local_ids = vtx_arena_alloc(arena,
                MAX_FIELDS_PER_OBJ * sizeof(vtx_nodeid_t));
            if (!pt->field_offsets || !pt->field_local_ids) return NULL;

            /* F4 fix: when virtual_result is available and the allocation
             * is classified as virtual, read field values from the virtual
             * object's field map instead of scanning dead StoreField nodes.
             * virtual.c marks StoreField nodes as dead after rewriting them
             * to local variables, so collect_field_values() would miss them. */
            if (virtual_result != NULL &&
                vtx_virtual_is_virtual(virtual_result, input_id)) {
                pt->field_count = collect_field_values_from_virtual(
                    virtual_result, input_id, pt->field_offsets,
                    pt->field_local_ids, MAX_FIELDS_PER_OBJ);
            } else {
                pt->field_count = collect_field_values(
                    graph, input_id, pt->field_offsets, pt->field_local_ids,
                    MAX_FIELDS_PER_OBJ);
            }

            /* Insert materialization code */
            if (insert_materialization_code(graph, pt, arena) != 0) {
                return NULL;
            }

            /* Replace the reference to the scalar-replaced allocation
             * with the materialized object */
            vtx_node_replace_input(table, node->id, inp,
                                    pt->materialized_obj_id);

            result->objects_materialized++;
            result->fields_stored += pt->field_count;
            if (is_deopt_point) {
                result->deopt_points_handled++;
            }
        }
    }

    /* Also scan FrameState nodes: they may reference scalar-replaced
     * allocations in the local/stack arrays */
    for (uint32_t i = 0; i < table->count; i++) {
        vtx_node_t *node = &table->nodes[i];
        if (node->dead || node->opcode != VTX_OP_FrameState) continue;

        /* FrameState nodes have inputs: [control, locals..., stack..., monitors...] */
        for (uint32_t inp = 0; inp < node->input_count; inp++) {
            vtx_nodeid_t input_id = node->inputs[inp];
            vtx_node_t *input_node = vtx_node_get(table, input_id);
            if (!input_node || input_node->dead) continue;

            if (!is_allocation(input_node->opcode)) continue;
            if (!vtx_pea_is_scalar_replaceable(analysis, input_id)) continue;

            /* This FrameState references a scalar-replaced allocation.
             * It must be materialized. */
            bool already_materialized = false;
            for (uint32_t p = 0; p < result->point_count; p++) {
                if (result->points[p].alloc_id == input_id &&
                    result->points[p].escape_node_id == node->id) {
                    already_materialized = true;
                    /* Already have a materialization point — just replace
                     * the FrameState input with the materialized object */
                    vtx_node_replace_input(table, node->id, inp,
                                            result->points[p].materialized_obj_id);
                    break;
                }
            }

            if (already_materialized) continue;

            /* Create materialization point for the FrameState */
            vtx_materialize_point_t *pt = add_materialize_point(result, arena);
            if (!pt) return NULL;

            pt->escape_node_id = node->id;
            pt->alloc_id       = input_id;

            vtx_node_t *alloc_node = vtx_node_get(table, input_id);
            pt->type_id = alloc_node ? alloc_node->type_id : 0;

            pt->field_offsets = vtx_arena_alloc(arena,
                MAX_FIELDS_PER_OBJ * sizeof(uint32_t));
            pt->field_local_ids = vtx_arena_alloc(arena,
                MAX_FIELDS_PER_OBJ * sizeof(vtx_nodeid_t));
            if (!pt->field_offsets || !pt->field_local_ids) return NULL;

            /* F4 fix: use virtual field map when available */
            if (virtual_result != NULL &&
                vtx_virtual_is_virtual(virtual_result, input_id)) {
                pt->field_count = collect_field_values_from_virtual(
                    virtual_result, input_id, pt->field_offsets,
                    pt->field_local_ids, MAX_FIELDS_PER_OBJ);
            } else {
                pt->field_count = collect_field_values(
                    graph, input_id, pt->field_offsets, pt->field_local_ids,
                    MAX_FIELDS_PER_OBJ);
            }

            if (insert_materialization_code(graph, pt, arena) != 0) {
                return NULL;
            }

            vtx_node_replace_input(table, node->id, inp,
                                    pt->materialized_obj_id);

            result->objects_materialized++;
            result->fields_stored += pt->field_count;
            result->deopt_points_handled++;
        }
    }

    /* ---- Phase 3: Handle Phi merge points ----
     *
     * When a Phi node merges a scalar-replaced virtual object with another
     * value (either another virtual object or a real heap object), the
     * virtual object must be materialized in the predecessor block that
     * corresponds to the virtual input. Otherwise, the Phi would try to
     * merge a "virtual" (scalar-replaced) object with a real pointer,
     * which is type-incorrect.
     *
     * For each Phi node that has one or more virtual object inputs:
     *   1. Identify which inputs are virtual (scalar-replaced allocations)
     *   2. For each virtual input, determine the predecessor block that
     *      corresponds to this input (Phi input[i] comes from the i-th
     *      predecessor of the Phi's Region node)
     *   3. Emit the allocation + field stores (materialization) in that
     *      predecessor block, anchored to the predecessor's terminal
     *      control node
     *   4. Replace the virtual input with the materialized object
     *
     * This ensures that the Phi only merges concrete heap pointers, and
     * the materialized object exists before control flow reaches the Phi.
     *
     * In the SoN IR, a Phi node's layout is:
     *   Phi->inputs[0] = Region node
     *   Phi->inputs[1..N] = values from predecessors 0..N-1
     *   Region->inputs[0..N-1] = control from predecessors 0..N-1
     * So Phi->inputs[inp] (inp >= 1) corresponds to
     *   Region->inputs[inp - 1] (the predecessor's control output).
     */
    for (uint32_t i = 0; i < table->count; i++) {
        vtx_node_t *node = &table->nodes[i];
        if (node->dead || node->opcode != VTX_OP_Phi) continue;

        /* The Phi's first input is the Region node */
        vtx_nodeid_t region_id = (node->input_count > 0)
                                 ? node->inputs[0]
                                 : VTX_NODEID_INVALID;
        vtx_node_t *region = vtx_node_get(table, region_id);

        /* Check each input of the Phi for scalar-replaced allocations */
        for (uint32_t inp = 0; inp < node->input_count; inp++) {
            vtx_nodeid_t input_id = node->inputs[inp];
            vtx_node_t *input_node = vtx_node_get(table, input_id);
            if (!input_node || input_node->dead) continue;

            if (!is_allocation(input_node->opcode)) continue;
            if (!vtx_pea_is_scalar_replaceable(analysis, input_id)) continue;

            /* This Phi input is a scalar-replaced allocation that
             * must be materialized in the predecessor block. */

            /* Determine the predecessor control node for this input.
             * Phi input[inp] corresponds to Region input[inp - 1].
             * (Input 0 of the Phi is the Region itself, value inputs
             * start at index 1 and map 1:1 to Region predecessor index.) */
            vtx_nodeid_t pred_control = VTX_NODEID_INVALID;
            if (inp >= 1 && region && inp - 1 < region->input_count) {
                pred_control = region->inputs[inp - 1];
            }

            bool already_materialized = false;
            for (uint32_t p = 0; p < result->point_count; p++) {
                if (result->points[p].alloc_id == input_id &&
                    result->points[p].escape_node_id == node->id) {
                    already_materialized = true;
                    /* Replace the Phi input with the already-materialized object */
                    vtx_node_replace_input(table, node->id, inp,
                                            result->points[p].materialized_obj_id);
                    break;
                }
            }

            if (already_materialized) continue;

            /* Create a materialization point for the Phi merge */
            vtx_materialize_point_t *pt = add_materialize_point(result, arena);
            if (!pt) return NULL;

            pt->escape_node_id = node->id;  /* Phi is the escape point */
            pt->alloc_id       = input_id;
            pt->predecessor_control = pred_control; /* anchor to predecessor */

            vtx_node_t *alloc_node = vtx_node_get(table, input_id);
            pt->type_id = alloc_node ? alloc_node->type_id : 0;

            pt->field_offsets = vtx_arena_alloc(arena,
                MAX_FIELDS_PER_OBJ * sizeof(uint32_t));
            pt->field_local_ids = vtx_arena_alloc(arena,
                MAX_FIELDS_PER_OBJ * sizeof(vtx_nodeid_t));
            if (!pt->field_offsets || !pt->field_local_ids) return NULL;

            /* F4 fix: use virtual field map when available */
            if (virtual_result != NULL &&
                vtx_virtual_is_virtual(virtual_result, input_id)) {
                pt->field_count = collect_field_values_from_virtual(
                    virtual_result, input_id, pt->field_offsets,
                    pt->field_local_ids, MAX_FIELDS_PER_OBJ);
            } else {
                pt->field_count = collect_field_values(
                    graph, input_id, pt->field_offsets, pt->field_local_ids,
                    MAX_FIELDS_PER_OBJ);
            }

            /* Insert materialization code (NewObject + StoreField).
             * The predecessor_control field is set, so the NewObject
             * will be anchored to the predecessor block's control node,
             * ensuring correct placement. */
            if (insert_materialization_code(graph, pt, arena) != 0) {
                return NULL;
            }

            /* Replace the Phi's input with the materialized object */
            vtx_node_replace_input(table, node->id, inp,
                                    pt->materialized_obj_id);

            result->objects_materialized++;
            result->fields_stored += pt->field_count;
            /* Phi merge points are not deopt points per se, but the
             * materialization is needed for correctness. */
        }
    }

    return result;
}

/* ========================================================================== */
/* Query helpers                                                               */
/* ========================================================================== */

vtx_nodeid_t vtx_materialize_get_obj(const vtx_materialize_result_t *result,
                                      vtx_nodeid_t alloc_id,
                                      vtx_nodeid_t escape_node_id)
{
    VTX_ASSERT(result != NULL, "result must not be NULL");
    for (uint32_t i = 0; i < result->point_count; i++) {
        if (result->points[i].alloc_id == alloc_id &&
            result->points[i].escape_node_id == escape_node_id) {
            return result->points[i].materialized_obj_id;
        }
    }
    return VTX_NODEID_INVALID;
}

bool vtx_materialize_is_materialized(const vtx_materialize_result_t *result,
                                      vtx_nodeid_t alloc_id)
{
    VTX_ASSERT(result != NULL, "result must not be NULL");
    for (uint32_t i = 0; i < result->point_count; i++) {
        if (result->points[i].alloc_id == alloc_id) {
            return true;
        }
    }
    return false;
}
