/**
 * VORTEX Virtual Object Tracking
 *
 * Marks non-escaping allocations as virtual, tracks them through Phis,
 * and rewrites field accesses to local variable access.
 */

#include "pea/virtual.h"
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
/* Internal: grow virtual obj array                                            */
/* ========================================================================== */

static vtx_virtual_obj_t *grow_virtual_objs(vtx_virtual_result_t *result,
                                              vtx_arena_t *arena)
{
    if (result->virtual_obj_count >= result->virtual_obj_capacity) {
        uint32_t new_cap = result->virtual_obj_capacity == 0 ?
                           16 : result->virtual_obj_capacity * 2;
        vtx_virtual_obj_t *new_objs = vtx_arena_alloc(arena,
            new_cap * sizeof(vtx_virtual_obj_t));
        if (!new_objs) return NULL;
        if (result->virtual_objs && result->virtual_obj_count > 0) {
            memcpy(new_objs, result->virtual_objs,
                   result->virtual_obj_count * sizeof(vtx_virtual_obj_t));
        }
        result->virtual_objs = new_objs;
        result->virtual_obj_capacity = new_cap;
    }
    return &result->virtual_objs[result->virtual_obj_count];
}

/* ========================================================================== */
/* Internal: find or create virtual obj entry                                  */
/* ========================================================================== */

static vtx_virtual_obj_t *find_or_create_virtual_obj(
    vtx_virtual_result_t *result, vtx_nodeid_t alloc_id,
    uint32_t type_id, vtx_arena_t *arena)
{
    /* Check if already exists */
    for (uint32_t i = 0; i < result->virtual_obj_count; i++) {
        if (result->virtual_objs[i].alloc_id == alloc_id) {
            return &result->virtual_objs[i];
        }
    }

    /* Create new entry */
    vtx_virtual_obj_t *obj = grow_virtual_objs(result, arena);
    if (!obj) return NULL;

    memset(obj, 0, sizeof(*obj));
    obj->alloc_id = alloc_id;
    obj->type_id  = type_id;
    obj->state    = VTX_VIRTUAL_YES;

    /* Allocate field arrays with initial capacity */
    obj->field_capacity = 8;
    obj->field_offsets = vtx_arena_alloc(arena,
        obj->field_capacity * sizeof(uint32_t));
    obj->field_values  = vtx_arena_alloc(arena,
        obj->field_capacity * sizeof(vtx_nodeid_t));
    if (!obj->field_offsets || !obj->field_values) return NULL;

    result->virtual_obj_count++;
    return obj;
}

/* ========================================================================== */
/* Internal: set field value in virtual object                                 */
/* ========================================================================== */

static int virtual_obj_set_field(vtx_virtual_obj_t *obj,
                                  uint32_t field_offset,
                                  vtx_nodeid_t value_id,
                                  vtx_arena_t *arena)
{
    /* Check if field already exists */
    for (uint32_t i = 0; i < obj->field_count; i++) {
        if (obj->field_offsets[i] == field_offset) {
            obj->field_values[i] = value_id;
            return 0;
        }
    }

    /* Add new field */
    if (obj->field_count >= obj->field_capacity) {
        uint32_t new_cap = obj->field_capacity * 2;
        uint32_t *new_offsets = vtx_arena_alloc(arena,
            new_cap * sizeof(uint32_t));
        vtx_nodeid_t *new_values = vtx_arena_alloc(arena,
            new_cap * sizeof(vtx_nodeid_t));
        if (!new_offsets || !new_values) return -1;
        if (obj->field_offsets && obj->field_count > 0) {
            memcpy(new_offsets, obj->field_offsets,
                   obj->field_count * sizeof(uint32_t));
            memcpy(new_values, obj->field_values,
                   obj->field_count * sizeof(vtx_nodeid_t));
        }
        obj->field_offsets  = new_offsets;
        obj->field_values   = new_values;
        obj->field_capacity = new_cap;
    }

    obj->field_offsets[obj->field_count] = field_offset;
    obj->field_values[obj->field_count]  = value_id;
    obj->field_count++;
    return 0;
}

/* ========================================================================== */
/* Internal: classify allocations as virtual or not                            */
/* ========================================================================== */

static int classify_allocations(vtx_graph_t *graph,
                                 const vtx_pea_analysis_t *analysis,
                                 vtx_virtual_result_t *result,
                                 vtx_arena_t *arena)
{
    vtx_node_table_t *table = &graph->node_table;
    uint32_t state_count = table->count;

    result->state_count = state_count;
    result->virtual_states = vtx_arena_alloc(arena,
        state_count * sizeof(vtx_virtual_state_t));
    if (!result->virtual_states) return -1;

    /* Initialize all to UNKNOWN */
    memset(result->virtual_states, 0, state_count * sizeof(vtx_virtual_state_t));

    /* Classify each allocation */
    for (uint32_t a = 0; a < analysis->escape_map.alloc_count; a++) {
        vtx_nodeid_t alloc_id = analysis->escape_map.alloc_ids[a];
        vtx_escape_state_t escape = vtx_pea_get_escape(analysis, alloc_id);

        result->total_allocs++;

        if (escape == VTX_ESCAPE_NONE) {
            /* Non-escaping allocation → virtual */
            result->virtual_states[alloc_id] = VTX_VIRTUAL_YES;
            result->virtual_count++;

            vtx_node_t *alloc_node = vtx_node_get(table, alloc_id);
            uint32_t type_id = alloc_node ? alloc_node->type_id : 0;

            vtx_virtual_obj_t *obj = find_or_create_virtual_obj(
                result, alloc_id, type_id, arena);
            if (!obj) return -1;
        } else {
            /* Escaping allocation → not virtual */
            result->virtual_states[alloc_id] = VTX_VIRTUAL_NO;
            result->non_virtual_count++;
        }
    }

    return 0;
}

/* ========================================================================== */
/* Internal: resolve Phi nodes for virtual objects                             */
/* ========================================================================== */

/**
 * Process Phi nodes that reference virtual objects. A Phi of two virtual
 * objects is virtual if both inputs are virtual with the same type.
 * Otherwise, the inputs must be materialized (non-virtual).
 *
 * Returns the number of Phis resolved as virtual.
 */
static uint32_t resolve_virtual_phis(vtx_graph_t *graph,
                                      vtx_virtual_result_t *result,
                                      vtx_arena_t *arena)
{
    vtx_node_table_t *table = &graph->node_table;
    uint32_t resolved = 0;

    /* Iterate until no more changes (Phi resolution may cascade) */
    bool changed = true;
    while (changed) {
        changed = false;

        for (uint32_t i = 0; i < table->count; i++) {
            vtx_node_t *node = &table->nodes[i];
            if (node->dead || node->opcode != VTX_OP_Phi) continue;

            /* Check if all inputs are virtual with the same type */
            bool all_virtual = true;
            uint32_t common_type = 0;
            bool first_input = true;

            for (uint32_t inp = 0; inp < node->input_count; inp++) {
                vtx_nodeid_t input_id = node->inputs[inp];
                if (input_id >= result->state_count) {
                    all_virtual = false;
                    break;
                }

                vtx_node_t *input_node = vtx_node_get(table, input_id);
                if (!input_node || input_node->dead) {
                    all_virtual = false;
                    break;
                }

                /* Check if this input is a virtual allocation */
                if (!is_allocation(input_node->opcode)) {
                    /* Non-allocation input (e.g., null constant).
                     * If it's a null, the Phi can still be virtual. */
                    if (input_node->opcode == VTX_OP_Constant &&
                        input_node->constval.kind == VTX_TYPE_Ptr &&
                        input_node->constval.as.ptr_val == NULL) {
                        /* null input — ok for virtual Phi */
                        continue;
                    }
                    all_virtual = false;
                    break;
                }

                if (result->virtual_states[input_id] != VTX_VIRTUAL_YES) {
                    all_virtual = false;
                    break;
                }

                /* Check type compatibility */
                if (first_input) {
                    common_type = input_node->type_id;
                    first_input = false;
                } else if (input_node->type_id != common_type) {
                    /* Different types — cannot be virtual */
                    all_virtual = false;
                    break;
                }
            }

            if (all_virtual && node->input_count > 0) {
                /* The Phi is virtual. The Phi itself represents a virtual
                 * object that merges the field values of its inputs.
                 * For each field, create a new Phi that merges the
                 * corresponding field values. */
                vtx_virtual_obj_t *obj = find_or_create_virtual_obj(
                    result, node->id, common_type, arena);
                if (!obj) break;

                if (obj->state == VTX_VIRTUAL_YES) {
                    /* Already processed */
                    continue;
                }

                /* Collect fields from the first virtual input */
                vtx_virtual_obj_t *first_vobj = NULL;
                for (uint32_t inp = 0; inp < node->input_count; inp++) {
                    vtx_nodeid_t input_id = node->inputs[inp];
                    if (input_id < result->state_count &&
                        result->virtual_states[input_id] == VTX_VIRTUAL_YES) {
                        for (uint32_t v = 0; v < result->virtual_obj_count; v++) {
                            if (result->virtual_objs[v].alloc_id == input_id) {
                                first_vobj = &result->virtual_objs[v];
                                break;
                            }
                        }
                        if (first_vobj) break;
                    }
                }

                if (!first_vobj) continue;

                /* For each field, create a Phi that merges the values
                 * from all virtual inputs */
                for (uint32_t f = 0; f < first_vobj->field_count; f++) {
                    uint32_t field_offset = first_vobj->field_offsets[f];

                    /* Create a new Phi node for this field */
                    vtx_nodeid_t field_phi_id = vtx_node_create(table, VTX_OP_Phi);
                    if (field_phi_id == VTX_NODEID_INVALID) break;

                    vtx_node_t *field_phi = vtx_node_get(table, field_phi_id);
                    field_phi->flags = VTX_NF_PINNED;

                    /* Add inputs from each virtual object's field value */
                    for (uint32_t inp = 0; inp < node->input_count; inp++) {
                        vtx_nodeid_t input_id = node->inputs[inp];
                        vtx_nodeid_t field_value = VTX_NODEID_INVALID;

                        if (input_id < result->state_count &&
                            result->virtual_states[input_id] == VTX_VIRTUAL_YES) {
                            /* Find the field value in this virtual object */
                            for (uint32_t v = 0; v < result->virtual_obj_count; v++) {
                                if (result->virtual_objs[v].alloc_id == input_id) {
                                    for (uint32_t ff = 0; ff < result->virtual_objs[v].field_count; ff++) {
                                        if (result->virtual_objs[v].field_offsets[ff] == field_offset) {
                                            field_value = result->virtual_objs[v].field_values[ff];
                                            break;
                                        }
                                    }
                                    break;
                                }
                            }
                        }

                        /* If we couldn't find the field value, use a null constant */
                        if (field_value == VTX_NODEID_INVALID) {
                            vtx_nodeid_t null_id = vtx_node_create(table, VTX_OP_Constant);
                            if (null_id != VTX_NODEID_INVALID) {
                                vtx_node_t *null_node = vtx_node_get(table, null_id);
                                null_node->type = VTX_TYPE_Ptr;
                                null_node->constval = vtx_constval_ptr(NULL);
                                field_value = null_id;
                            }
                        }

                        vtx_node_add_input(table, field_phi_id, field_value);
                    }

                    /* Record the field in the virtual object */
                    virtual_obj_set_field(obj, field_offset, field_phi_id, arena);
                }

                obj->state = VTX_VIRTUAL_YES;
                result->virtual_states[node->id] = VTX_VIRTUAL_YES;
                result->virtual_count++;
                resolved++;
                changed = true;
            } else {
                /* Phi is not virtual — mark it as non-virtual */
                if (node->id < result->state_count &&
                    result->virtual_states[node->id] == VTX_VIRTUAL_UNKNOWN) {
                    result->virtual_states[node->id] = VTX_VIRTUAL_NO;
                }
            }
        }
    }

    return resolved;
}

/* ========================================================================== */
/* Internal: rewrite field accesses for virtual objects                        */
/* ========================================================================== */

static uint32_t rewrite_virtual_field_accesses(vtx_graph_t *graph,
                                                 vtx_virtual_result_t *result)
{
    vtx_node_table_t *table = &graph->node_table;
    uint32_t rewritten = 0;

    for (uint32_t i = 0; i < table->count; i++) {
        vtx_node_t *node = &table->nodes[i];
        if (node->dead) continue;

        if (node->opcode != VTX_OP_LoadField &&
            node->opcode != VTX_OP_StoreField) continue;

        /* Find the receiver input */
        vtx_nodeid_t receiver_id = VTX_NODEID_INVALID;
        if (node->opcode == VTX_OP_LoadField) {
            if (node->input_count >= 1) {
                receiver_id = node->inputs[node->input_count - 1];
            }
        } else {
            /* StoreField: receiver is second-to-last input */
            if (node->input_count >= 2) {
                receiver_id = node->inputs[node->input_count - 2];
            }
        }

        if (receiver_id == VTX_NODEID_INVALID) continue;
        if (receiver_id >= result->state_count) continue;
        if (result->virtual_states[receiver_id] != VTX_VIRTUAL_YES) continue;

        /* The receiver is a virtual object — rewrite the field access */
        vtx_virtual_obj_t *vobj = NULL;
        for (uint32_t v = 0; v < result->virtual_obj_count; v++) {
            if (result->virtual_objs[v].alloc_id == receiver_id) {
                vobj = &result->virtual_objs[v];
                break;
            }
        }

        if (!vobj) continue;

        if (node->opcode == VTX_OP_LoadField) {
            /* Replace LoadField with the scalar local value */
            vtx_nodeid_t field_value = VTX_NODEID_INVALID;
            for (uint32_t f = 0; f < vobj->field_count; f++) {
                if (vobj->field_offsets[f] == node->field_offset) {
                    field_value = vobj->field_values[f];
                    break;
                }
            }

            if (field_value != VTX_NODEID_INVALID) {
                /* Replace all uses of this LoadField with the field value */
                for (uint32_t u = 0; u < table->count; u++) {
                    vtx_node_t *user = &table->nodes[u];
                    if (user->dead) continue;
                    for (uint32_t inp = 0; inp < user->input_count; inp++) {
                        if (user->inputs[inp] == node->id) {
                            vtx_node_replace_input(table, user->id, inp,
                                                    field_value);
                        }
                    }
                }
                node->dead = true;
                rewritten++;
            }
        } else {
            /* StoreField: update the virtual object's field value */
            vtx_nodeid_t value_id = node->inputs[node->input_count - 1];

            /* Update the field value in the virtual object */
            bool found = false;
            for (uint32_t f = 0; f < vobj->field_count; f++) {
                if (vobj->field_offsets[f] == node->field_offset) {
                    vobj->field_values[f] = value_id;
                    found = true;
                    break;
                }
            }

            if (!found) {
                /* New field — this shouldn't normally happen if we
                 * initialized all fields during classification, but
                 * handle it gracefully */
                vobj->field_offsets[vobj->field_count] = node->field_offset;
                vobj->field_values[vobj->field_count]  = value_id;
                vobj->field_count++;
            }

            node->dead = true;
            rewritten++;
        }
    }

    return rewritten;
}

/* ========================================================================== */
/* Internal: eliminate virtual allocation nodes                                */
/* ========================================================================== */

static uint32_t eliminate_virtual_allocations(vtx_graph_t *graph,
                                                vtx_virtual_result_t *result)
{
    vtx_node_table_t *table = &graph->node_table;
    uint32_t eliminated = 0;

    for (uint32_t i = 0; i < table->count; i++) {
        vtx_node_t *node = &table->nodes[i];
        if (node->dead) continue;
        if (!is_allocation(node->opcode)) continue;

        if (node->id < result->state_count &&
            result->virtual_states[node->id] == VTX_VIRTUAL_YES) {
            /* Check if the allocation has any remaining non-dead uses.
             * All field accesses have been rewritten, but there might be
             * remaining uses (e.g., identity comparisons, monitor ops). */
            bool has_uses = false;
            for (uint32_t u = 0; u < table->count; u++) {
                vtx_node_t *user = &table->nodes[u];
                if (user->dead) continue;
                if (user->id == node->id) continue;
                for (uint32_t inp = 0; inp < user->input_count; inp++) {
                    if (user->inputs[inp] == node->id) {
                        has_uses = true;
                        break;
                    }
                }
                if (has_uses) break;
            }

            if (!has_uses) {
                node->dead = true;
                eliminated++;
            }
        }
    }

    return eliminated;
}

/* ========================================================================== */
/* Main entry point                                                            */
/* ========================================================================== */

vtx_virtual_result_t *vtx_virtual_run(vtx_graph_t *graph,
                                       const vtx_pea_analysis_t *analysis,
                                       vtx_arena_t *arena)
{
    VTX_ASSERT(graph != NULL, "graph must not be NULL");
    VTX_ASSERT(analysis != NULL, "analysis must not be NULL");
    VTX_ASSERT(arena != NULL, "arena must not be NULL");

    /* Allocate result */
    vtx_virtual_result_t *result = vtx_arena_alloc(arena,
        sizeof(vtx_virtual_result_t));
    if (!result) return NULL;
    memset(result, 0, sizeof(*result));

    /* Step 1: Classify allocations as virtual or not */
    if (classify_allocations(graph, analysis, result, arena) != 0) {
        return NULL;
    }

    /* Step 2: Collect initial field values from StoreField nodes */
    vtx_node_table_t *table = &graph->node_table;

    for (uint32_t i = 0; i < table->count; i++) {
        vtx_node_t *node = &table->nodes[i];
        if (node->dead || node->opcode != VTX_OP_StoreField) continue;
        if (node->input_count < 2) continue;

        vtx_nodeid_t receiver_id = node->inputs[node->input_count - 2];
        vtx_nodeid_t value_id    = node->inputs[node->input_count - 1];

        if (receiver_id >= result->state_count) continue;
        if (result->virtual_states[receiver_id] != VTX_VIRTUAL_YES) continue;

        /* Find the virtual object */
        vtx_virtual_obj_t *vobj = NULL;
        for (uint32_t v = 0; v < result->virtual_obj_count; v++) {
            if (result->virtual_objs[v].alloc_id == receiver_id) {
                vobj = &result->virtual_objs[v];
                break;
            }
        }

        if (!vobj) continue;

        /* Update the field value */
        virtual_obj_set_field(vobj, node->field_offset, value_id, arena);
    }

    /* Step 3: Resolve virtual Phis */
    result->phis_resolved = resolve_virtual_phis(graph, result, arena);

    /* Step 4: Rewrite field accesses */
    result->field_accesses_rewritten = rewrite_virtual_field_accesses(
        graph, result);

    /* Step 5: Eliminate virtual allocation nodes */
    eliminate_virtual_allocations(graph, result);

    return result;
}

/* ========================================================================== */
/* Query helpers                                                               */
/* ========================================================================== */

bool vtx_virtual_is_virtual(const vtx_virtual_result_t *result,
                             vtx_nodeid_t alloc_id)
{
    VTX_ASSERT(result != NULL, "result must not be NULL");
    if (alloc_id < result->state_count) {
        return result->virtual_states[alloc_id] == VTX_VIRTUAL_YES;
    }
    return false;
}

vtx_nodeid_t vtx_virtual_get_field(const vtx_virtual_result_t *result,
                                    vtx_nodeid_t alloc_id,
                                    uint32_t field_offset)
{
    VTX_ASSERT(result != NULL, "result must not be NULL");
    for (uint32_t i = 0; i < result->virtual_obj_count; i++) {
        if (result->virtual_objs[i].alloc_id == alloc_id) {
            const vtx_virtual_obj_t *obj = &result->virtual_objs[i];
            for (uint32_t f = 0; f < obj->field_count; f++) {
                if (obj->field_offsets[f] == field_offset) {
                    return obj->field_values[f];
                }
            }
            break;
        }
    }
    return VTX_NODEID_INVALID;
}

const vtx_virtual_obj_t *vtx_virtual_get_obj(const vtx_virtual_result_t *result,
                                               vtx_nodeid_t alloc_id)
{
    VTX_ASSERT(result != NULL, "result must not be NULL");
    for (uint32_t i = 0; i < result->virtual_obj_count; i++) {
        if (result->virtual_objs[i].alloc_id == alloc_id) {
            return &result->virtual_objs[i];
        }
    }
    return NULL;
}
