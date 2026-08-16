/* ============================================================================ *
 * AI-MODIFIED CODE
 *
 * This file was originally written by a human developer. It has been
 * substantially modified by an AI assistant (GLM/Z.ai) for bug fixes,
 * performance improvements, and feature additions.
 *
 * Original human-written structure is preserved; AI changes are marked
 * with bug fix IDs (B1-B28) or perf notes (Perf 1-10) in comments.
 *
 * If reviewing, please verify AI changes against the original logic.
 * ============================================================================ */

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

/* Maximum fields per object (used by insert_materialization_code and
 * vtx_materialize_run). PEA-2-7 tracks this as a follow-up to make
 * dynamic — currently 32 is enough for most objects. */
#define MAX_FIELDS_PER_OBJ 32

/* ========================================================================== */
/* Internal: find all field values for a scalar-replaced allocation            */
/* ========================================================================== */

/**
 * Collect the current values of all fields of a scalar-replaced allocation.
 * For each StoreField that targets this allocation, record the stored value.
 * Returns the field count and fills the offsets/values arrays.
 *
 * B22 fix: The previous implementation iterated over all StoreField nodes
 * in TABLE ORDER (i.e., node-creation order) and applied "last write wins"
 * based on that order. But node-creation order does NOT match execution
 * order after inlining/GVN/LICM passes — a StoreField created later (higher
 * node id) might actually execute BEFORE an earlier-created StoreField if
 * it was moved or cloned by a transformation.
 *
 * The fix: walk the MEMORY CHAIN (threaded through StoreField.inputs[0])
 * to determine execution order. Each StoreField's first input is the
 * previous memory state — either the alloc itself, an earlier StoreField,
 * or some other memory-producing node. Following this chain from head to
 * tail gives us the writes in execution order; overwriting as we go means
 * the LAST write in execution order wins for each field offset.
 */
static uint32_t collect_field_values(vtx_graph_t *graph, vtx_nodeid_t alloc_id,
                                      uint32_t *offsets, vtx_nodeid_t *values,
                                      uint32_t max_fields)
{
    vtx_node_table_t *table = &graph->node_table;
    uint32_t count = 0;

    /* Step 1: collect all StoreField node IDs targeting this alloc.
     *
     * PEA-001 fix: the old code used a fixed [256] buffer and stopped
     * at sf_count < MAX_SF. An allocation with >256 StoreField nodes
     * (e.g., after aggressive loop unrolling, or a large initializer)
     * would silently drop stores beyond 256, producing incorrect
     * materialization (stale field values). Fix: first count the
     * stores, then heap-allocate an array sized to the actual count. */
    uint32_t sf_total = 0;
    for (uint32_t i = 0; i < table->count; i++) {
        vtx_node_t *n = &table->nodes[i];
        if (n->dead || n->opcode != VTX_OP_StoreField) continue;
        if (n->input_count < 2) continue;
        vtx_nodeid_t recv = n->inputs[n->input_count - 2];
        if (recv != alloc_id) continue;
        sf_total++;
    }

    if (sf_total == 0) return 0;

    vtx_nodeid_t *sf_nodes = (vtx_nodeid_t *)malloc(sf_total * sizeof(vtx_nodeid_t));
    if (sf_nodes == NULL) return 0; /* OOM — bail out of PEA for this alloc */
    uint32_t sf_count = 0;

    for (uint32_t i = 0; i < table->count && sf_count < sf_total; i++) {
        vtx_node_t *node = &table->nodes[i];
        if (node->dead) continue;
        if (node->opcode != VTX_OP_StoreField) continue;
        if (node->input_count < 2) continue;

        vtx_nodeid_t receiver_id = node->inputs[node->input_count - 2];
        if (receiver_id != alloc_id) continue;

        sf_nodes[sf_count++] = node->id;
    }

    if (sf_count == 0) return 0;

    /* Step 2: find the chain HEAD — the StoreField whose memory input
     * is NOT another StoreField in sf_nodes. (The head's mem input is
     * typically the alloc itself or some entry memory state.) */
    vtx_nodeid_t head = VTX_NODEID_INVALID;
    for (uint32_t i = 0; i < sf_count; i++) {
        vtx_node_t *node = vtx_node_get(table, sf_nodes[i]);
        if (!node || node->input_count < 1) continue;
        vtx_nodeid_t mem_in = node->inputs[0];

        bool is_chain_member = false;
        for (uint32_t j = 0; j < sf_count; j++) {
            if (sf_nodes[j] == mem_in) { is_chain_member = true; break; }
        }
        if (!is_chain_member) {
            head = sf_nodes[i];
            break;
        }
    }
    /* Fallback: if no head found (e.g., a cycle, which shouldn't happen
     * in valid SSA), use the first node in table order. */
    if (head == VTX_NODEID_INVALID) head = sf_nodes[0];

    /* Step 3: walk forward from head, overwriting field values as we
     * encounter later writes. The last write in the chain wins (which
     * is the last in EXECUTION order, not table order). */
    vtx_nodeid_t cur = head;
    uint32_t safety = 0;
    while (cur != VTX_NODEID_INVALID && safety++ < sf_count + 1) {
        vtx_node_t *node = vtx_node_get(table, cur);
        if (!node || node->input_count < 2) break;

        uint32_t field_offset = node->field_offset;
        vtx_nodeid_t value_id = node->inputs[node->input_count - 1];

        bool found = false;
        for (uint32_t k = 0; k < count; k++) {
            if (offsets[k] == field_offset) {
                values[k] = value_id; /* overwrite: last write in chain wins */
                found = true;
                break;
            }
        }
        if (!found && count < max_fields) {
            offsets[count] = field_offset;
            values[count]  = value_id;
            count++;
        }

        /* Find the next StoreField in the chain — the one whose memory
         * input is `cur`. */
        vtx_nodeid_t next = VTX_NODEID_INVALID;
        for (uint32_t j = 0; j < sf_count; j++) {
            vtx_node_t *sf = vtx_node_get(table, sf_nodes[j]);
            if (!sf || sf->input_count < 1) continue;
            if (sf->inputs[0] == cur) {
                next = sf_nodes[j];
                break;
            }
        }
        cur = next;
    }

    /* PEA-001 fix: free the heap-allocated sf_nodes array. */
    free(sf_nodes);
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
/* PEA-2-6: insert_materialization_code now returns the final memory
 * state (the last StoreField's NodeID) via out_final_mem_state. The
 * caller uses this to connect the materialization chain to the escape
 * point's memory input — without this connection, the scheduler may
 * place the StoreFields AFTER the escape point, causing it to read
 * uninitialized heap memory.
 *
 * PEA-2-4/PEA-2-5: recursive materialization with cycle handling.
 * When a field value is itself a virtual allocation, we must
 * materialize it first (recursively) and store the materialized
 * object's ID. Cycles (A.field→B, B.field→A) are broken by
 * pre-allocating the NewObject and registering it in the
 * `mat_in_progress` map before recursing into fields. */
static int insert_materialization_code(vtx_graph_t *graph,
                                        vtx_materialize_point_t *pt,
                                        vtx_arena_t *arena,
                                        vtx_nodeid_t *out_final_mem_state,
                                        const vtx_pea_analysis_t *analysis,
                                        const vtx_virtual_result_t *virtual_result,
                                        vtx_materialize_result_t *result,
                                        vtx_nodeid_t *mat_in_progress_ids,
                                        vtx_nodeid_t *mat_in_progress_objs,
                                        uint32_t *mat_in_progress_count,
                                        uint32_t mat_in_progress_cap)
{
    vtx_node_table_t *table = &graph->node_table;

    /* PEA-2-5: cycle detection — check if this allocation is already
     * being materialized (pre-allocated). If so, return the existing
     * NewObject ID without recursing into its fields again. */
    for (uint32_t i = 0; i < *mat_in_progress_count; i++) {
        if (mat_in_progress_ids[i] == pt->alloc_id) {
            if (out_final_mem_state) {
                *out_final_mem_state = mat_in_progress_objs[i];
            }
            return 0;
        }
    }

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

    /* PEA-2-5: register this NewObject as "in progress" BEFORE
     * recursing into fields. This breaks cycles: if A.field→B and
     * B.field→A, when we recurse into B, we find A in the map and
     * use the pre-allocated NewObject ID instead of recursing again. */
    if (*mat_in_progress_count < mat_in_progress_cap) {
        mat_in_progress_ids[*mat_in_progress_count] = pt->alloc_id;
        mat_in_progress_objs[*mat_in_progress_count] = new_obj_id;
        (*mat_in_progress_count)++;
    }

    pt->materialized_obj_id = new_obj_id;

    /* Track the current memory state for chaining StoreField nodes.
     * Initially, the NewObject produces the new memory state. */
    vtx_nodeid_t mem_state = new_obj_id;

    /* Create StoreField nodes for each field */
    for (uint32_t f = 0; f < pt->field_count; f++) {
        vtx_nodeid_t field_value = pt->field_local_ids[f];

        /* PEA-2-4: if the field value is a virtual allocation,
         * recursively materialize it first. */
        if (virtual_result != NULL && analysis != NULL &&
            field_value < virtual_result->state_count &&
            vtx_virtual_is_virtual(virtual_result, field_value) &&
            vtx_pea_is_scalar_replaceable(analysis, field_value)) {

            /* Check if already materialized */
            bool already_done = false;
            for (uint32_t m = 0; m < *mat_in_progress_count; m++) {
                if (mat_in_progress_ids[m] == field_value) {
                    field_value = mat_in_progress_objs[m];
                    already_done = true;
                    break;
                }
            }

            if (!already_done) {
                /* Recursively materialize the nested virtual object. */
                vtx_materialize_point_t nested_pt;
                memset(&nested_pt, 0, sizeof(nested_pt));
                nested_pt.alloc_id = field_value;
                vtx_node_t *nested_alloc = vtx_node_get(table, field_value);
                nested_pt.type_id = nested_alloc ? nested_alloc->type_id : 0;
                nested_pt.field_offsets = vtx_arena_alloc(arena,
                    MAX_FIELDS_PER_OBJ * sizeof(uint32_t));
                nested_pt.field_local_ids = vtx_arena_alloc(arena,
                    MAX_FIELDS_PER_OBJ * sizeof(vtx_nodeid_t));

                if (nested_pt.field_offsets && nested_pt.field_local_ids) {
                    nested_pt.field_count = collect_field_values_from_virtual(
                        virtual_result, field_value,
                        nested_pt.field_offsets, nested_pt.field_local_ids,
                        MAX_FIELDS_PER_OBJ);

                    vtx_nodeid_t nested_mem = VTX_NODEID_INVALID;
                    if (insert_materialization_code(graph, &nested_pt, arena,
                                                     &nested_mem, analysis,
                                                     virtual_result, result,
                                                     mat_in_progress_ids,
                                                     mat_in_progress_objs,
                                                     mat_in_progress_count,
                                                     mat_in_progress_cap) == 0) {
                        field_value = nested_pt.materialized_obj_id;
                        result->objects_materialized++;
                        result->fields_stored += nested_pt.field_count;
                    }
                }
            }
        }

        vtx_nodeid_t store_id = vtx_node_create(table, VTX_OP_StoreField);
        if (store_id == VTX_NODEID_INVALID) return -1;

        vtx_node_t *store = vtx_node_get(table, store_id);
        store->field_offset = pt->field_offsets[f];
        store->type    = VTX_TYPE_Void;
        store->flags   = vtx_nf_union(VTX_NF_SIDE_EFFECT, VTX_NF_MEMORY);

        /* Add inputs: memory chain, receiver (new object), value.
         * PEA-2-4: field_value may have been updated to the nested
         * object's materialized ID. */
        vtx_node_add_input(table, store_id, mem_state);  /* memory chain */
        vtx_node_add_input(table, store_id, new_obj_id); /* receiver */
        vtx_node_add_input(table, store_id, field_value); /* value */

        /* This StoreField becomes the new memory state */
        mem_state = store_id;
    }

    /* PEA-2-6: return the final memory state so the caller can
     * connect it to the escape point's memory input. */
    if (out_final_mem_state) {
        *out_final_mem_state = mem_state;
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

    /* PEA-2-5: materialization-in-progress map for cycle detection.
     * Sized to the number of allocations (from the analysis). */
    uint32_t mat_cap = analysis->escape_map.alloc_count;
    if (mat_cap == 0) mat_cap = 1;
    vtx_nodeid_t *mat_in_progress_ids = vtx_arena_alloc(arena,
        mat_cap * sizeof(vtx_nodeid_t));
    vtx_nodeid_t *mat_in_progress_objs = vtx_arena_alloc(arena,
        mat_cap * sizeof(vtx_nodeid_t));
    uint32_t mat_in_progress_count = 0;
    if (!mat_in_progress_ids || !mat_in_progress_objs) return NULL;

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

            /* Insert materialization code.
             * PEA-2-6: get the final memory state so we can connect the
             * materialization chain to the escape point's memory input. */
            vtx_nodeid_t final_mem = VTX_NODEID_INVALID;
            if (insert_materialization_code(graph, pt, arena, &final_mem, analysis, virtual_result, result, mat_in_progress_ids, mat_in_progress_objs, &mat_in_progress_count, mat_cap) != 0) {
                return NULL;
            }

            /* PEA-2-1: insert_materialization_code() calls vtx_node_create()
             * (NewObject + StoreField per field), which may realloc
             * table->nodes when count == capacity. The `node` pointer
             * captured at the top of this scan loop is now dangling.
             * Re-fetch via the loop index (= node->id by the table invariant)
             * before reading node->id below and before the inner for-loop
             * continues (next iteration reads node->input_count and
             * node->inputs[inp] from the stale pointer). */
            node = &table->nodes[i];

            /* Replace the reference to the scalar-replaced allocation
             * with the materialized object */
            vtx_node_replace_input(table, node->id, inp,
                                    pt->materialized_obj_id);

            /* PEA-2-6: connect the materialization chain to the escape
             * point's memory input. Add the last StoreField as an
             * additional memory input to the escape point so the
             * scheduler orders the stores BEFORE the escape. Without
             * this, the escape point (Call/Return/Deopt) may execute
             * before the field stores complete, reading uninitialized
             * heap memory. */
            if (final_mem != VTX_NODEID_INVALID) {
                vtx_node_add_input(table, node->id, final_mem);
            }

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

            /* PEA-2-6: pass out_final_mem_state to connect the chain. */
            vtx_nodeid_t final_mem = VTX_NODEID_INVALID;
            if (insert_materialization_code(graph, pt, arena, &final_mem, analysis, virtual_result, result, mat_in_progress_ids, mat_in_progress_objs, &mat_in_progress_count, mat_cap) != 0) {
                return NULL;
            }

            /* PEA-2-1: re-fetch `node` — insert_materialization_code()
             * calls vtx_node_create() (NewObject + StoreField per field),
             * which may realloc table->nodes. The FrameState scan loop's
             * `node` pointer is now dangling; re-fetch via the loop index. */
            node = &table->nodes[i];

            vtx_node_replace_input(table, node->id, inp,
                                    pt->materialized_obj_id);

            /* PEA-2-6: connect materialization chain to escape point. */
            if (final_mem != VTX_NODEID_INVALID) {
                vtx_node_add_input(table, node->id, final_mem);
            }

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
             * ensuring correct placement.
             * PEA-2-6: pass out_final_mem_state to connect the chain. */
            vtx_nodeid_t final_mem = VTX_NODEID_INVALID;
            if (insert_materialization_code(graph, pt, arena, &final_mem, analysis, virtual_result, result, mat_in_progress_ids, mat_in_progress_objs, &mat_in_progress_count, mat_cap) != 0) {
                return NULL;
            }

            /* PEA-2-1: re-fetch `node` — insert_materialization_code()
             * calls vtx_node_create() (NewObject + StoreField per field),
             * which may realloc table->nodes. The Phi scan loop's
             * `node` pointer is now dangling; re-fetch via the loop index. */
            node = &table->nodes[i];

            /* Replace the Phi's input with the materialized object */
            vtx_node_replace_input(table, node->id, inp,
                                    pt->materialized_obj_id);

            /* PEA-2-6: connect materialization chain to the Phi's
             * memory input so the scheduler orders stores before
             * any memory-dependent use of the Phi's result. */
            if (final_mem != VTX_NODEID_INVALID) {
                vtx_node_add_input(table, node->id, final_mem);
            }

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
