#include "ir/tbaa.h"
#include "ir/graph.h"

#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* Type-Based Alias Analysis (TBAA) for VORTEX SoN IR                        */
/*                                                                            */
/* Implementation:                                                             */
/*   1. Walk all memory nodes in the graph                                    */
/*   2. Classify each node's access kind based on type_id, field_offset,     */
/*      and opcode                                                            */
/*   3. Provide alias queries that use the classification to determine       */
/*      whether two memory operations can alias                              */
/*   4. Provide hoisting queries for LICM integration                        */
/* ========================================================================== */

/* ========================================================================== */
/* Node classification                                                        */
/*                                                                            */
/* Each memory node carries type_id and/or field_offset metadata from the     */
/* graph construction phase. We use this to determine the TBAA kind.         */
/*                                                                            */
/* Classification rules:                                                      */
/*   LoadField/StoreField:                                                    */
/*     - If the field's type is Int → VTX_TBAA_INT_FIELD                     */
/*     - If the field's type is Float → VTX_TBAA_FLOAT_FIELD                */
/*     - If the field's type is Ptr → VTX_TBAA_REF_FIELD                    */
/*                                                                            */
/*   LoadIndexed/StoreIndexed:                                                */
/*     - type_id refers to the array's element type                           */
/*     - If element type is Int → VTX_TBAA_INT_ARRAY                         */
/*     - If element type is Float → VTX_TBAA_FLOAT_ARRAY                    */
/*     - If element type is Ptr → VTX_TBAA_REF_ARRAY                        */
/*                                                                            */
/*   Load/Store (raw):                                                        */
/*     - VTX_TBAA_RAW — no type information available                        */
/* ========================================================================== */

/* Helper: classify a field access based on the node's type_id.
 * The node's type_id for field accesses refers to the declaring class,
 * and the node's field_offset tells us which field.
 * We use the field_offset + a heuristic based on the node's data type
 * to classify. */

static vtx_tbaa_kind_t classify_field_access(const vtx_node_t *node,
                                              const vtx_node_table_t *table)
{
    /* The node's own type (vtx_nodetype_t) tells us what the result is:
     *   VTX_TYPE_Int  → INT_FIELD
     *   VTX_TYPE_Float → FLOAT_FIELD
     *   VTX_TYPE_Ptr  → REF_FIELD
     * This is more reliable than looking at the type_id because the
     * node's type is already resolved by type propagation. */
    switch (node->type) {
    case VTX_TYPE_Int:
        return VTX_TBAA_INT_FIELD;
    case VTX_TYPE_Float:
        return VTX_TBAA_FLOAT_FIELD;
    case VTX_TYPE_Ptr:
        return VTX_TBAA_REF_FIELD;
    default:
        break;
    }

    /* Fallback: look at the first non-control, non-memory input to
     * determine the field type. For LoadField, the value loaded is the
     * result, and its type is set on the node. For StoreField, the
     * value being stored is one of the inputs. */
    if (node->opcode == VTX_OP_StoreField && node->input_count >= 3) {
        /* StoreField inputs: [memory, object, value] */
        vtx_nodeid_t value_id = node->inputs[2];
        if (value_id != VTX_NODEID_INVALID && value_id < table->count) {
            const vtx_node_t *value_node = &table->nodes[value_id];
            switch (value_node->type) {
            case VTX_TYPE_Int:
                return VTX_TBAA_INT_FIELD;
            case VTX_TYPE_Float:
                return VTX_TBAA_FLOAT_FIELD;
            case VTX_TYPE_Ptr:
                return VTX_TBAA_REF_FIELD;
            default:
                break;
            }
        }
    }

    /* Final fallback: use type_id heuristics.
     * type_id for NewArray encodes element kind. */
    return VTX_TBAA_ANY;
}

/* Helper: classify an indexed (array) access based on type_id.
 * For array accesses, type_id typically refers to the array class.
 * We need to determine the element type. */

static vtx_tbaa_kind_t classify_indexed_access(const vtx_node_t *node,
                                                 const vtx_node_table_t *table)
{
    /* First check the node's own type (set by type propagation) */
    switch (node->type) {
    case VTX_TYPE_Int:
        return VTX_TBAA_INT_ARRAY;
    case VTX_TYPE_Float:
        return VTX_TBAA_FLOAT_ARRAY;
    case VTX_TYPE_Ptr:
        return VTX_TBAA_REF_ARRAY;
    default:
        break;
    }

    /* For StoreIndexed, check the value being stored */
    if (node->opcode == VTX_OP_StoreIndexed && node->input_count >= 4) {
        /* StoreIndexed inputs: [memory, array, index, value] */
        vtx_nodeid_t value_id = node->inputs[3];
        if (value_id != VTX_NODEID_INVALID && value_id < table->count) {
            const vtx_node_t *value_node = &table->nodes[value_id];
            switch (value_node->type) {
            case VTX_TYPE_Int:
                return VTX_TBAA_INT_ARRAY;
            case VTX_TYPE_Float:
                return VTX_TBAA_FLOAT_ARRAY;
            case VTX_TYPE_Ptr:
                return VTX_TBAA_REF_ARRAY;
            default:
                break;
            }
        }
    }

    /* For LoadIndexed, check the array input's type_id.
     * The array input (inputs[1]) is a NewArray or Parameter whose
     * type_id encodes the element type. */
    if (node->opcode == VTX_OP_LoadIndexed && node->input_count >= 3) {
        vtx_nodeid_t array_id = node->inputs[1];
        if (array_id != VTX_NODEID_INVALID && array_id < table->count) {
            const vtx_node_t *array_node = &table->nodes[array_id];
            /* If the array was created by NewArray, its type_id
             * may encode the element type. */
            if (array_node->opcode == VTX_OP_NewArray) {
                /* type_id for NewArray: we use a convention where
                 * certain type IDs map to element kinds.
                 * Since the type system is dynamic, we use the
                 * node type as a proxy. */
                /* Fall through to VTX_TBAA_ANY for now — the
                 * type propagation result above is preferred. */
            }
        }
    }

    return VTX_TBAA_ANY;
}

/* ========================================================================== */
/* Public classification API                                                  */
/* ========================================================================== */

vtx_tbaa_kind_t vtx_tbaa_classify_node(const vtx_node_t *node,
                                        const vtx_node_table_t *table)
{
    if (node == NULL) return VTX_TBAA_ANY;

    switch (node->opcode) {
    case VTX_OP_LoadField:
        return classify_field_access(node, table);

    case VTX_OP_StoreField:
        return classify_field_access(node, table);

    case VTX_OP_LoadIndexed:
        return classify_indexed_access(node, table);

    case VTX_OP_StoreIndexed:
        return classify_indexed_access(node, table);

    case VTX_OP_Load:
    case VTX_OP_Store:
        /* Raw memory operations have no type info */
        return VTX_TBAA_RAW;

    case VTX_OP_Initialize:
        return VTX_TBAA_RAW;

    /* Vector operations: classify based on the node type */
    case VTX_OP_VectorLoad:
    case VTX_OP_VectorStore:
        switch (node->type) {
        case VTX_TYPE_Int:
            return VTX_TBAA_INT_ARRAY;
        case VTX_TYPE_Float:
            return VTX_TBAA_FLOAT_ARRAY;
        default:
            return VTX_TBAA_ANY;
        }

    default:
        return VTX_TBAA_ANY;
    }
}

/* ========================================================================== */
/* TBAA analysis: assign metadata to all memory nodes                        */
/* ========================================================================== */

vtx_tbaa_result_t *vtx_tbaa_analyze(vtx_graph_t *graph, vtx_arena_t *arena)
{
    VTX_ASSERT(graph != NULL, "graph must not be NULL");
    VTX_ASSERT(arena != NULL, "arena must not be NULL");

    if (graph == NULL || arena == NULL) return NULL;

    vtx_node_table_t *nt = &graph->node_table;
    uint32_t node_count = nt->count;

    if (node_count == 0) return NULL;

    /* Allocate the result */
    vtx_tbaa_result_t *result = (vtx_tbaa_result_t *)vtx_arena_alloc(
        arena, sizeof(vtx_tbaa_result_t));
    if (result == NULL) return NULL;

    result->info_count = node_count;
    result->info = (vtx_tbaa_info_t *)vtx_arena_alloc(
        arena, node_count * sizeof(vtx_tbaa_info_t));
    if (result->info == NULL) return NULL;

    /* Initialize all entries to VTX_TBAA_ANY */
    for (uint32_t i = 0; i < node_count; i++) {
        result->info[i].kind = VTX_TBAA_ANY;
        result->info[i].type_id = 0;
        result->info[i].field_offset = 0;
        result->info[i].is_load = false;
    }

    /* Walk all nodes and classify memory operations */
    for (uint32_t nid = 0; nid < node_count; nid++) {
        const vtx_node_t *node = &nt->nodes[nid];
        if (node->dead) continue;

        /* Only classify memory nodes */
        if (!vtx_nf_has(node->flags, VTX_NF_MEMORY) &&
            node->opcode != VTX_OP_LoadIndexed &&
            node->opcode != VTX_OP_StoreIndexed &&
            node->opcode != VTX_OP_LoadField &&
            node->opcode != VTX_OP_StoreField &&
            node->opcode != VTX_OP_VectorLoad &&
            node->opcode != VTX_OP_VectorStore) {
            continue;
        }

        vtx_tbaa_info_t *info = &result->info[nid];
        info->kind = vtx_tbaa_classify_node(node, nt);
        info->type_id = node->type_id;
        info->field_offset = node->field_offset;

        /* Determine if this is a load or store */
        switch (node->opcode) {
        case VTX_OP_Load:
        case VTX_OP_LoadField:
        case VTX_OP_LoadIndexed:
        case VTX_OP_VectorLoad:
            info->is_load = true;
            break;
        case VTX_OP_Store:
        case VTX_OP_StoreField:
        case VTX_OP_StoreIndexed:
        case VTX_OP_VectorStore:
        case VTX_OP_Initialize:
            info->is_load = false;
            break;
        default:
            info->is_load = false;
            break;
        }
    }

    return result;
}

/* ========================================================================== */
/* TBAA info query                                                            */
/* ========================================================================== */

const vtx_tbaa_info_t *vtx_tbaa_get_info(const vtx_tbaa_result_t *result,
                                           vtx_nodeid_t node_id)
{
    if (result == NULL) return NULL;
    if (node_id >= result->info_count) return NULL;
    return &result->info[node_id];
}

/* ========================================================================== */
/* Alias query                                                                */
/*                                                                            */
/* Determines whether two memory operations can alias based on their          */
/* TBAA metadata.                                                             */
/*                                                                            */
/* Rules (from most to least precise):                                        */
/*   1. Same kind + same type_id + same field_offset → MAY_ALIAS             */
/*      (they might access the same field of the same object)                */
/*   2. Different type categories → NO_ALIAS                                 */
/*      (int vs float vs ref can never alias)                                */
/*   3. Same type category, different field_offset → NO_ALIAS                */
/*      (different fields of the same type)                                  */
/*   4. Array vs field → NO_ALIAS                                            */
/*      (array elements are never object fields)                             */
/*   5. ANY or RAW → MAY_ALIAS (conservative)                               */
/* ========================================================================== */

vtx_alias_result_t vtx_tbaa_alias(const vtx_tbaa_info_t *a, const vtx_tbaa_info_t *b)
{
    if (a == NULL || b == NULL) return VTX_ALIAS_MAY_ALIAS;

    /* ANY or RAW: conservative — may alias anything */
    if (a->kind == VTX_TBAA_ANY || b->kind == VTX_TBAA_ANY) {
        return VTX_ALIAS_MAY_ALIAS;
    }
    if (a->kind == VTX_TBAA_RAW || b->kind == VTX_TBAA_RAW) {
        return VTX_ALIAS_MAY_ALIAS;
    }

    /* Different type categories → NO_ALIAS.
     * The three fundamental categories are: int, float, ref.
     * An int access can NEVER alias a float or ref access. */
    bool a_is_int   = vtx_tbaa_is_int(a->kind);
    bool a_is_float = vtx_tbaa_is_float(a->kind);
    bool a_is_ref   = vtx_tbaa_is_ref(a->kind);

    bool b_is_int   = vtx_tbaa_is_int(b->kind);
    bool b_is_float = vtx_tbaa_is_float(b->kind);
    bool b_is_ref   = vtx_tbaa_is_ref(b->kind);

    /* If the categories are different, they can't alias */
    if (a_is_int && b_is_float) return VTX_ALIAS_NO_ALIAS;
    if (a_is_int && b_is_ref)   return VTX_ALIAS_NO_ALIAS;
    if (a_is_float && b_is_int) return VTX_ALIAS_NO_ALIAS;
    if (a_is_float && b_is_ref) return VTX_ALIAS_NO_ALIAS;
    if (a_is_ref && b_is_int)   return VTX_ALIAS_NO_ALIAS;
    if (a_is_ref && b_is_float) return VTX_ALIAS_NO_ALIAS;

    /* Same type category. Now check if they are both array or both field. */
    bool a_is_array = vtx_tbaa_is_array(a->kind);
    bool b_is_array = vtx_tbaa_is_array(b->kind);
    bool a_is_field = vtx_tbaa_is_field(a->kind);
    bool b_is_field = vtx_tbaa_is_field(b->kind);

    /* Array access vs field access: NO_ALIAS.
     * Array elements are stored in the array's contiguous body,
     * while fields are stored at fixed offsets in the object header.
     * They can never overlap. */
    if (a_is_array && b_is_field) return VTX_ALIAS_NO_ALIAS;
    if (a_is_field && b_is_array) return VTX_ALIAS_NO_ALIAS;

    /* Both are field accesses of the same type category.
     * If the field_offsets are different, they can't alias. */
    if (a_is_field && b_is_field) {
        if (a->field_offset != b->field_offset) {
            return VTX_ALIAS_NO_ALIAS;
        }
        /* Same type category, same field offset → MAY_ALIAS.
         * They might be accessing the same field of the same object
         * or different objects with the same field offset. */
        return VTX_ALIAS_MAY_ALIAS;
    }

    /* Both are array accesses of the same type category.
     * We can't distinguish between different arrays of the same element
     * type without more analysis (e.g., different allocation sites).
     * Conservative: MAY_ALIAS. */
    if (a_is_array && b_is_array) {
        /* However, if one is a load and the other is a store of the
         * SAME array (same type_id), they might alias. We need more
         * precise analysis (e.g., V8's element kinds) to distinguish.
         * For now: MAY_ALIAS. */
        return VTX_ALIAS_MAY_ALIAS;
    }

    /* Default: conservative */
    return VTX_ALIAS_MAY_ALIAS;
}

/* ========================================================================== */
/* Hoisting query                                                             */
/*                                                                            */
/* Can a load of load_kind be hoisted past a store of store_kind?            */
/*                                                                            */
/* This returns true if the load cannot be affected by the store, i.e.,      */
/* the load and store access different memory locations.                     */
/* ========================================================================== */

bool vtx_tbaa_no_alias(vtx_tbaa_kind_t kind_a, vtx_tbaa_kind_t kind_b)
{
    vtx_tbaa_info_t a = { .kind = kind_a, .type_id = 0, .field_offset = 0, .is_load = false };
    vtx_tbaa_info_t b = { .kind = kind_b, .type_id = 0, .field_offset = 0, .is_load = false };
    return vtx_tbaa_alias(&a, &b) == VTX_ALIAS_NO_ALIAS;
}

bool vtx_tbaa_can_hoist_load(vtx_tbaa_kind_t load_kind, vtx_tbaa_kind_t store_kind)
{
    /* ANY or RAW: can't prove no-alias, so can't hoist */
    if (load_kind == VTX_TBAA_ANY || load_kind == VTX_TBAA_RAW) return false;
    if (store_kind == VTX_TBAA_ANY || store_kind == VTX_TBAA_RAW) return false;

    /* Same type category: might alias, can't hoist */
    if (vtx_tbaa_is_int(load_kind) && vtx_tbaa_is_int(store_kind)) return false;
    if (vtx_tbaa_is_float(load_kind) && vtx_tbaa_is_float(store_kind)) return false;
    if (vtx_tbaa_is_ref(load_kind) && vtx_tbaa_is_ref(store_kind)) return false;

    /* Different type categories: NO_ALIAS, can hoist */
    /* Array vs field of different types: can hoist */
    return true;
}
