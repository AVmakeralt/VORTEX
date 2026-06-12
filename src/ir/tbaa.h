#ifndef VORTEX_IR_TBAA_H
#define VORTEX_IR_TBAA_H

#include <stdint.h>
#include <stdbool.h>
#include "vortex_config.h"
#include "ir/node.h"
#include "ir/graph.h"
#include "runtime/arena.h"

/* ========================================================================== */
/* Type-Based Alias Analysis (TBAA)                                           */
/*                                                                            */
/* TBAA determines whether two memory operations can alias by examining       */
/* the types of the memory locations they access.                             */
/*                                                                            */
/* Core insight:                                                              */
/*   - An int[] store can NEVER alias a float[] load                         */
/*   - A ref field store can NEVER alias an int field load                   */
/*   - Different fields of the same type at different offsets can't alias    */
/*                                                                            */
/* This enables 50%+ of loop-invariant load hoisting because most loops      */
/* mix loads of different types (e.g., reading int array while writing       */
/* ref array). Without TBAA, ANY store in the loop prevents hoisting.       */
/*                                                                            */
/* V8 uses "element kinds" for the same purpose; Graal uses its type system. */
/* VORTEX's TBAA is a simpler but effective approach.                        */
/* ========================================================================== */

/* TBAA access kind — what type of memory is being accessed */
typedef enum {
    VTX_TBAA_ANY = 0,         /* unknown / any type */
    VTX_TBAA_INT_ARRAY,       /* int[] array elements */
    VTX_TBAA_FLOAT_ARRAY,     /* float[] array elements */
    VTX_TBAA_REF_ARRAY,       /* Object[] array elements */
    VTX_TBAA_INT_FIELD,       /* int field of an object */
    VTX_TBAA_FLOAT_FIELD,     /* float/double field */
    VTX_TBAA_REF_FIELD,       /* reference field (object pointer) */
    VTX_TBAA_RAW,             /* raw memory (no alias info) */
    VTX_TBAA_COUNT
} vtx_tbaa_kind_t;

/* TBAA alias query result */
typedef enum {
    VTX_ALIAS_MUST_ALIAS,    /* definitely the same memory location */
    VTX_ALIAS_MAY_ALIAS,     /* might be the same (conservative) */
    VTX_ALIAS_NO_ALIAS       /* definitely different locations */
} vtx_alias_result_t;

/* TBAA metadata for a memory node */
typedef struct {
    vtx_tbaa_kind_t  kind;          /* access kind */
    uint32_t         type_id;       /* type ID of the accessing object/array */
    uint32_t         field_offset;  /* field offset (for field accesses) */
    bool             is_load;       /* true for loads, false for stores */
} vtx_tbaa_info_t;

/* TBAA analysis result: per-node TBAA metadata */
typedef struct {
    vtx_tbaa_info_t *info;         /* array indexed by node ID */
    uint32_t         info_count;   /* size of info array (= node count) */
} vtx_tbaa_result_t;

/**
 * Assign TBAA metadata to all memory nodes in the graph.
 *
 * Walks all Load/Store/LoadField/StoreField/LoadIndexed/StoreIndexed
 * nodes and determines their access kind based on the node's type_id
 * and field_offset metadata.
 *
 * @param graph  The SoN graph
 * @param arena  Arena allocator for the result
 * @return       TBAA result (arena-allocated), or NULL on failure
 */
vtx_tbaa_result_t *vtx_tbaa_analyze(vtx_graph_t *graph, vtx_arena_t *arena);

/**
 * Get TBAA info for a specific node.
 *
 * @param result  The TBAA analysis result
 * @param node_id The node to query
 * @return        Pointer to TBAA info, or NULL if not available
 */
const vtx_tbaa_info_t *vtx_tbaa_get_info(const vtx_tbaa_result_t *result,
                                           vtx_nodeid_t node_id);

/**
 * Assign TBAA kind to a single node based on its opcode and metadata.
 *
 * @param node  The node to classify
 * @param table The node table (for looking up input types)
 * @return      The TBAA kind for this node
 */
vtx_tbaa_kind_t vtx_tbaa_classify_node(const vtx_node_t *node,
                                        const vtx_node_table_t *table);

/**
 * Query: can these two memory operations alias?
 *
 * Returns NO_ALIAS if the two operations definitely access different
 * memory locations, MAY_ALIAS if they might alias, or MUST_ALIAS if
 * they definitely access the same location.
 *
 * @param a  TBAA info for the first operation
 * @param b  TBAA info for the second operation
 * @return   Alias query result
 */
vtx_alias_result_t vtx_tbaa_alias(const vtx_tbaa_info_t *a, const vtx_tbaa_info_t *b);

/**
 * Convenience query: returns true if two TBAA kinds definitely don't alias.
 * Wrapper around vtx_tbaa_alias() that returns true only for VTX_ALIAS_NO_ALIAS.
 */
bool vtx_tbaa_no_alias(vtx_tbaa_kind_t kind_a, vtx_tbaa_kind_t kind_b);

/**
 * Query: can a load of the given kind be hoisted past a store of the
 * given kind?
 *
 * This is the key query for LICM: if a load is VTX_TBAA_INT_ARRAY and
 * the only stores in the loop are VTX_TBAA_REF_ARRAY or VTX_TBAA_FLOAT_ARRAY,
 * the load is loop-invariant and can be hoisted.
 *
 * @param load_kind   TBAA kind of the load
 * @param store_kind  TBAA kind of the store
 * @return            true if the load cannot be affected by the store
 */
bool vtx_tbaa_can_hoist_load(vtx_tbaa_kind_t load_kind, vtx_tbaa_kind_t store_kind);

/**
 * Query: is a given TBAA kind an integer access?
 */
static inline bool vtx_tbaa_is_int(vtx_tbaa_kind_t kind)
{
    return kind == VTX_TBAA_INT_ARRAY || kind == VTX_TBAA_INT_FIELD;
}

/**
 * Query: is a given TBAA kind a float access?
 */
static inline bool vtx_tbaa_is_float(vtx_tbaa_kind_t kind)
{
    return kind == VTX_TBAA_FLOAT_ARRAY || kind == VTX_TBAA_FLOAT_FIELD;
}

/**
 * Query: is a given TBAA kind a reference access?
 */
static inline bool vtx_tbaa_is_ref(vtx_tbaa_kind_t kind)
{
    return kind == VTX_TBAA_REF_ARRAY || kind == VTX_TBAA_REF_FIELD;
}

/**
 * Query: is a given TBAA kind an array element access?
 */
static inline bool vtx_tbaa_is_array(vtx_tbaa_kind_t kind)
{
    return kind == VTX_TBAA_INT_ARRAY || kind == VTX_TBAA_FLOAT_ARRAY ||
           kind == VTX_TBAA_REF_ARRAY;
}

/**
 * Query: is a given TBAA kind a field access?
 */
static inline bool vtx_tbaa_is_field(vtx_tbaa_kind_t kind)
{
    return kind == VTX_TBAA_INT_FIELD || kind == VTX_TBAA_FLOAT_FIELD ||
           kind == VTX_TBAA_REF_FIELD;
}

#endif /* VORTEX_IR_TBAA_H */
