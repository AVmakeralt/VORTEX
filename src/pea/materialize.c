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

    pt->materialized_obj_id = new_obj_id;

    /* Create StoreField nodes for each field */
    for (uint32_t f = 0; f < pt->field_count; f++) {
        vtx_nodeid_t store_id = vtx_node_create(table, VTX_OP_StoreField);
        if (store_id == VTX_NODEID_INVALID) return -1;

        vtx_node_t *store = vtx_node_get(table, store_id);
        store->field_offset = pt->field_offsets[f];
        store->type    = VTX_TYPE_Void;
        store->flags   = vtx_nf_union(VTX_NF_SIDE_EFFECT, VTX_NF_MEMORY);

        /* Add inputs: memory chain (null for now — scheduling will fix),
         * receiver (new object), value (scalar local) */
        vtx_node_add_input(table, store_id, new_obj_id);
        vtx_node_add_input(table, store_id, pt->field_local_ids[f]);
    }

    return 0;
}

/* ========================================================================== */
/* Main entry point                                                            */
/* ========================================================================== */

vtx_materialize_result_t *vtx_materialize_run(vtx_graph_t *graph,
                                                const vtx_pea_analysis_t *analysis,
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

            pt->field_count = collect_field_values(
                graph, input_id, pt->field_offsets, pt->field_local_ids,
                MAX_FIELDS_PER_OBJ);

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

            pt->field_count = collect_field_values(
                graph, input_id, pt->field_offsets, pt->field_local_ids,
                MAX_FIELDS_PER_OBJ);

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
