#ifndef VORTEX_NODE_H
#define VORTEX_NODE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vortex_config.h"
#include "runtime/object.h"

/**
 * VORTEX Sea-of-Nodes IR — Node Taxonomy
 *
 * Every value, control point, memory operation, allocation, and deoptimization
 * marker in the IR is represented as a Node. Nodes reference each other by
 * NodeID (uint32_t). The graph is stored in a flat node table with growing
 * capacity. NodeIDs are never reused within a compilation.
 *
 * Node inputs form a directed graph. Control, memory, and data edges are
 * distinguished by per-node flags, not by input position — the consumer
 * inspects the producer's flags to determine edge kind.
 */

/* ========================================================================== */
/* Node ID                                                                     */
/* ========================================================================== */

typedef uint32_t vtx_nodeid_t;

#define VTX_NODEID_INVALID ((vtx_nodeid_t)0xFFFFFFFF)

/* ========================================================================== */
/* Node opcode enumeration                                                     */
/* ========================================================================== */

typedef enum {
    /* ---- Control ---- */
    VTX_OP_Start = 0,
    VTX_OP_End,
    VTX_OP_If,
    VTX_OP_Goto,
    VTX_OP_Switch,
    VTX_OP_LoopBegin,
    VTX_OP_LoopEnd,
    VTX_OP_Return,
    VTX_OP_Unwind,
    VTX_OP_Catch,
    VTX_OP_Province,       /* control-projection domain marker */

    /* ---- Data: constants and parameters ---- */
    VTX_OP_Constant,
    VTX_OP_Parameter,

    /* ---- Data: arithmetic ---- */
    VTX_OP_Add,
    VTX_OP_Sub,
    VTX_OP_Mul,
    VTX_OP_Div,
    VTX_OP_Mod,
    VTX_OP_Shl,
    VTX_OP_Shr,
    VTX_OP_Sar,     /* arithmetic shift right (fills with sign bit) */
    VTX_OP_And,
    VTX_OP_Or,
    VTX_OP_Xor,
    VTX_OP_Neg,
    VTX_OP_Not,

    /* ---- Data: comparisons ---- */
    VTX_OP_Cmp,            /* integer compare → condition code */
    VTX_OP_CmpP,           /* pointer compare */
    VTX_OP_CmpF,           /* float compare */
    VTX_OP_CmpD,           /* double compare */

    /* ---- Data: min/max ---- */
    VTX_OP_Min,            /* takes two inputs, returns minimum */
    VTX_OP_Max,            /* takes two inputs, returns maximum */

    /* ---- Memory ---- */
    VTX_OP_Load,
    VTX_OP_Store,
    VTX_OP_LoadField,
    VTX_OP_StoreField,
    VTX_OP_LoadIndexed,
    VTX_OP_StoreIndexed,
    VTX_OP_MemBar,
    VTX_OP_Initialize,     /* initialize raw memory after allocation */

    /* ---- Calls ---- */
    VTX_OP_CallStatic,
    VTX_OP_CallVirtual,
    VTX_OP_CallInterface,
    VTX_OP_CallRuntime,

    /* ---- Type operations ---- */
    VTX_OP_CheckCast,
    VTX_OP_InstanceOf,
    VTX_OP_Guard,          /* speculation guard (null-check, type-check, etc.) */
    VTX_OP_Phi,
    VTX_OP_Region,         /* merge point for control flow */
    VTX_OP_Proj,           /* projection of a multi-output node */
    VTX_OP_ExceptProj,     /* exception projection of a potentially-throwing node */

    /* ---- Allocation ---- */
    VTX_OP_NewObject,
    VTX_OP_NewArray,
    VTX_OP_Allocate,       /* low-level memory allocation */
    VTX_OP_InitializeKlass,/* class initialization barrier */

    /* ---- Deoptimization ---- */
    VTX_OP_Deopt,
    VTX_OP_DeoptGuard,
    VTX_OP_FrameState,

    /* ---- SIMD Vector operations ---- */
    VTX_OP_VectorLoad,    /* load vector from memory */
    VTX_OP_VectorStore,   /* store vector to memory */
    VTX_OP_VectorAdd,     /* vector add */
    VTX_OP_VectorMul,     /* vector multiply */

    /* ---- Total ---- */
    VTX_NODE_OP_COUNT
} vtx_node_opcode_t;

/* ========================================================================== */
/* Node type lattice                                                           */
/* ========================================================================== */

/**
 * Each node carries a type from a simple lattice:
 *
 *   Top (unreachable / not yet computed)
 *    |
 *   Int / Float / Ptr / Void  (concrete types)
 *    |
 *   Bottom (overdefined / multiple types meet)
 */
typedef enum {
    VTX_TYPE_Bottom = 0,   /* overdefined */
    VTX_TYPE_Int    = 1,
    VTX_TYPE_Float  = 2,
    VTX_TYPE_Ptr    = 3,
    VTX_TYPE_Void   = 4,
    VTX_TYPE_Top    = 5    /* unreachable / not yet computed */
} vtx_nodetype_t;

/* ========================================================================== */
/* Node flags                                                                  */
/* ========================================================================== */

typedef enum {
    VTX_NF_NONE       = 0,
    VTX_NF_CONTROL    = (1u << 0),   /* node is on the control path */
    VTX_NF_MEMORY     = (1u << 1),   /* node is on the memory chain */
    VTX_NF_DATA       = (1u << 2),   /* node is a pure data computation */
    VTX_NF_SIDE_EFFECT= (1u << 3),   /* node has side effects (keeps alive in DCE) */
    VTX_NF_PINNED     = (1u << 4),   /* node must not float freely (e.g. Phi) */
    VTX_NF_WRITE_BARRIER = (1u << 5), /* node requires a GC write barrier (StoreField/StoreIndexed of ref) */
    VTX_NF_RAW_INT    = (1u << 6),   /* SMI tag elision: node operates on raw int64,
                                      * not NaN-boxed SMI. isel skips untag/retag. */
} vtx_node_flags_t;

/* Bitwise operators for flags */
static inline vtx_node_flags_t vtx_nf_union(vtx_node_flags_t a, vtx_node_flags_t b)
{
    return (vtx_node_flags_t)((unsigned)a | (unsigned)b);
}

static inline bool vtx_nf_has(vtx_node_flags_t flags, vtx_node_flags_t bit)
{
    return ((unsigned)flags & (unsigned)bit) != 0;
}

/* ========================================================================== */
/* Constant value union                                                        */
/* ========================================================================== */

/**
 * For VTX_OP_Constant nodes, the value field holds the actual constant.
 * We use a tagged union discriminated by the node's vtx_nodetype_t.
 */
typedef struct {
    vtx_nodetype_t kind;   /* discriminator */
    union {
        int64_t   int_val;
        double    float_val;
        void     *ptr_val;   /* for pointer constants (null, etc.) */
    } as;
} vtx_constval_t;

/* Construct constant values */
static inline vtx_constval_t vtx_constval_int(int64_t v)
{
    vtx_constval_t c;
    c.kind = VTX_TYPE_Int;
    c.as.int_val = v;
    return c;
}

static inline vtx_constval_t vtx_constval_float(double v)
{
    vtx_constval_t c;
    c.kind = VTX_TYPE_Float;
    c.as.float_val = v;
    return c;
}

static inline vtx_constval_t vtx_constval_ptr(void *v)
{
    vtx_constval_t c;
    c.kind = VTX_TYPE_Ptr;
    c.as.ptr_val = v;
    return c;
}

static inline vtx_constval_t vtx_constval_void(void)
{
    vtx_constval_t c;
    c.kind = VTX_TYPE_Void;
    c.as.int_val = 0;
    return c;
}

/* ========================================================================== */
/* Comparison condition codes                                                  */
/* ========================================================================== */

typedef enum {
    VTX_COND_EQ = 0,
    VTX_COND_NE,
    VTX_COND_LT,
    VTX_COND_LE,
    VTX_COND_GT,
    VTX_COND_GE,
    VTX_COND_ULT,    /* unsigned less-than */
    VTX_COND_ULE,
    VTX_COND_UGT,
    VTX_COND_UGE,
    VTX_COND_ALWAYS,
    VTX_COND_NEVER
} vtx_cond_t;

/* Negate a comparison condition */
static inline vtx_cond_t vtx_cond_negate(vtx_cond_t c)
{
    switch (c) {
    case VTX_COND_EQ:     return VTX_COND_NE;
    case VTX_COND_NE:     return VTX_COND_EQ;
    case VTX_COND_LT:     return VTX_COND_GE;
    case VTX_COND_LE:     return VTX_COND_GT;
    case VTX_COND_GT:     return VTX_COND_LT;
    case VTX_COND_GE:     return VTX_COND_LE;
    case VTX_COND_ULT:    return VTX_COND_UGE;
    case VTX_COND_ULE:    return VTX_COND_UGT;
    case VTX_COND_UGT:    return VTX_COND_ULE;
    case VTX_COND_UGE:    return VTX_COND_ULT;
    case VTX_COND_ALWAYS: return VTX_COND_NEVER;
    case VTX_COND_NEVER:  return VTX_COND_ALWAYS;
    }
    return VTX_COND_NEVER; /* unreachable */
}

/* ========================================================================== */
/* Use-def list entries                                                        */
/* ========================================================================== */

/**
 * Use-def list: nodes that reference this node as an input.
 * This is the inverse of the inputs array. For each node N that
 * has N->inputs[i] == this->id, there is a use entry (N, i).
 *
 * This enables O(K) traversal of a node's users (where K is the
 * number of users) instead of O(N²) scanning of all nodes.
 */
typedef struct vtx_use_entry {
    vtx_nodeid_t  user_id;     /* the node that uses this node */
    uint32_t      input_index; /* which input of the user references this node */
} vtx_use_entry_t;

/* ========================================================================== */
/* Node structure                                                              */
/* ========================================================================== */

#define VTX_NODE_INITIAL_INPUT_CAPACITY 4
#define VTX_NODE_INITIAL_USE_CAPACITY   4

typedef struct {
    vtx_nodeid_t      id;              /* unique, never reused */
    vtx_node_opcode_t opcode;          /* opcode discriminates node kind */
    vtx_nodetype_t    type;            /* lattice type */
    vtx_node_flags_t  flags;           /* control / memory / data / side_effect */

    /* Input edges: this node depends on these producers */
    vtx_nodeid_t     *inputs;          /* dynamically allocated array of input NodeIDs */
    uint32_t          input_count;     /* number of used input slots */
    uint32_t          input_capacity;  /* allocated capacity of inputs array */

    /* Constant value (valid only when opcode == VTX_OP_Constant) */
    vtx_constval_t    constval;

    /* Supplementary data per opcode family */
    vtx_cond_t        cond;            /* condition for If, Cmp, Guard, DeoptGuard */
    uint32_t          local_index;     /* for Parameter: which parameter index */
    uint32_t          field_offset;    /* for LoadField/StoreField */
    uint32_t          method_index;    /* for CallStatic/CallVirtual/CallInterface */
    uint32_t          type_id;         /* for NewObject/CheckCast/InstanceOf/NewArray */
    uint32_t          bytecode_pc;     /* source bytecode PC for deopt/FrameState */
    vtx_nodeid_t      frame_state;     /* FrameState node for deopt points */

    /* Output count (number of other nodes that reference this node as input).
     * Maintained incrementally by add_input/replace_input/remove_input. */
    uint32_t          output_count;

    /* Use-def list: the inverse of the inputs array.
     * For each node U that has U->inputs[i] == this->id,
     * there is a use entry {user_id=U, input_index=i}.
     * This enables O(K) user traversal for optimizations. */
    vtx_use_entry_t  *uses;            /* dynamically allocated array of use entries */
    uint32_t          use_count;       /* number of used slots */
    uint32_t          use_capacity;    /* allocated capacity of uses array */

    /* GVN value number (0 = not yet computed) */
    uint32_t          value_number;

    /* Whether this node is "dead" (removed from graph, awaiting cleanup) */
    bool              dead;

    /* Mark bit for iterative algorithms (GVN, DCE, SCCP, scheduling) */
    bool              mark;
} vtx_node_t;

/* ========================================================================== */
/* Opcode metadata table                                                       */
/* ========================================================================== */

typedef struct {
    const char       *name;
    vtx_node_flags_t  default_flags;   /* flags a fresh node of this opcode gets */
    uint32_t          fixed_inputs;    /* minimum expected inputs (0 = variable) */
} vtx_node_opcode_info_t;

extern const vtx_node_opcode_info_t vtx_node_opcode_table[VTX_NODE_OP_COUNT];

/* ========================================================================== */
/* Node table                                                                  */
/* ========================================================================== */

#define VTX_NODE_TABLE_INITIAL_CAPACITY 256

typedef struct {
    vtx_node_t  *nodes;       /* array indexed by NodeID */
    uint32_t     count;       /* number of nodes in the table */
    uint32_t     capacity;    /* allocated capacity */
    vtx_nodeid_t next_id;     /* monotonically increasing ID counter */
} vtx_node_table_t;

/**
 * Initialize a node table with initial capacity.
 * Returns 0 on success, -1 on failure (allocation error).
 */
int vtx_node_table_init(vtx_node_table_t *table, uint32_t initial_capacity);

/**
 * Destroy a node table and free all memory (inputs arrays + nodes array).
 */
void vtx_node_table_destroy(vtx_node_table_t *table);

/**
 * Create a new node in the table with the given opcode.
 * Sets id, opcode, type (Top), flags (from opcode metadata), empty inputs.
 * Returns the new node's ID, or VTX_NODEID_INVALID on failure.
 */
vtx_nodeid_t vtx_node_create(vtx_node_table_t *table, vtx_node_opcode_t opcode);

/**
 * Look up a node by ID. Returns NULL if the ID is invalid or out of range.
 */
vtx_node_t *vtx_node_get(vtx_node_table_t *table, vtx_nodeid_t id);

/**
 * Look up a const node by ID. Returns NULL if the ID is invalid.
 */
const vtx_node_t *vtx_node_get_const(const vtx_node_table_t *table, vtx_nodeid_t id);

/**
 * Add an input edge from this node to the producer with the given ID.
 * Increments the producer's output_count.
 * Returns 0 on success, -1 on failure.
 */
int vtx_node_add_input(vtx_node_table_t *table, vtx_nodeid_t consumer, vtx_nodeid_t producer);

/**
 * Remove the input at the given index from the consumer node.
 * Decrements the producer's output_count.
 * Shifts subsequent inputs down by one.
 * Returns 0 on success, -1 on failure (index out of bounds).
 */
int vtx_node_remove_input(vtx_node_table_t *table, vtx_nodeid_t consumer, uint32_t index);

/**
 * Replace the input at the given index with a new producer.
 * Decrements old producer's output_count and increments new producer's output_count.
 * Returns 0 on success, -1 on failure.
 */
int vtx_node_replace_input(vtx_node_table_t *table, vtx_nodeid_t consumer,
                           uint32_t index, vtx_nodeid_t new_producer);

/**
 * Find the first input index that matches the given producer ID.
 * Returns the index, or -1 if not found.
 */
int vtx_node_find_input(const vtx_node_t *node, vtx_nodeid_t producer);

/**
 * Compute the default type for a given opcode (before any propagation).
 */
vtx_nodetype_t vtx_node_default_type(vtx_node_opcode_t opcode);

/**
 * Get the human-readable name for an opcode.
 */
const char *vtx_node_opcode_name(vtx_node_opcode_t opcode);

/**
 * Get the human-readable name for a node type.
 */
const char *vtx_nodetype_name(vtx_nodetype_t t);

/**
 * Check whether a node has side effects and must be kept by DCE.
 */
bool vtx_node_is_side_effecting(vtx_node_opcode_t opcode);

/**
 * Check whether a node is a control node.
 */
bool vtx_node_is_control(vtx_node_opcode_t opcode);

/**
 * Check whether a node is a memory node.
 */
bool vtx_node_is_memory(vtx_node_opcode_t opcode);

/**
 * Check whether a node is a pure data node.
 */
bool vtx_node_is_data(vtx_node_opcode_t opcode);

/**
 * Check whether two constant values are equal.
 */
bool vtx_constval_equal(vtx_constval_t a, vtx_constval_t b);

/**
 * Hash a constant value.
 */
uint32_t vtx_constval_hash(vtx_constval_t v);

/**
 * Clear all mark bits in the node table.
 */
void vtx_node_table_clear_marks(vtx_node_table_t *table);

/**
 * Clear all dead flags in the node table.
 */
void vtx_node_table_clear_dead(vtx_node_table_t *table);

/* ========================================================================== */
/* Use-def list API                                                            */
/* ========================================================================== */

/**
 * Get a pointer to the first use entry of a node.
 * Returns NULL if the node has no uses.
 */
vtx_use_entry_t *vtx_node_uses_begin(vtx_node_t *node);

/**
 * Get a pointer one past the last use entry of a node.
 */
vtx_use_entry_t *vtx_node_uses_end(vtx_node_t *node);

/**
 * Get a const pointer to the first use entry of a node.
 */
const vtx_use_entry_t *vtx_node_uses_begin_const(const vtx_node_t *node);

/**
 * Get a const pointer one past the last use entry of a node.
 */
const vtx_use_entry_t *vtx_node_uses_end_const(const vtx_node_t *node);

/**
 * Replace ALL uses of old_id with new_id throughout the graph.
 * This is the O(K) version of what was previously O(N²) —
 * it walks old_id's use list and updates each user's input.
 *
 * Returns 0 on success, -1 on failure.
 */
int vtx_node_replace_all_uses(vtx_node_table_t *table,
                               vtx_nodeid_t old_id, vtx_nodeid_t new_id);

/**
 * Callback type for vtx_node_for_each_user.
 * Called with (table, user_id, input_index, context).
 * Return 0 to continue iteration, non-zero to stop.
 */
typedef int (*vtx_user_callback_t)(vtx_node_table_t *table,
                                    vtx_nodeid_t user_id,
                                    uint32_t input_index,
                                    void *context);

/**
 * Iterate over all users of a node, calling fn for each use entry.
 * Stops early if fn returns non-zero.
 * Returns the last return value from fn (0 if all calls returned 0).
 */
int vtx_node_for_each_user(vtx_node_table_t *table, vtx_nodeid_t node_id,
                            vtx_user_callback_t fn, void *context);

/**
 * Remove a use entry (user_id, input_index) from the producer's use-def list.
 * This is the public wrapper around the internal node_remove_use helper.
 * Used by passes like DCE that need to maintain use-def list consistency
 * when disconnecting edges without going through the full add/remove input API.
 */
void vtx_node_remove_use_entry(vtx_node_t *producer, vtx_nodeid_t user_id, uint32_t input_index);

/**
 * Add a use entry (user_id, input_index) to the producer's use-def list.
 * This is the public wrapper around the internal node_add_use helper.
 * Used by passes that need to maintain use-def list consistency when
 * adding edges without going through the full add input API.
 */
void vtx_node_add_use_entry(vtx_node_t *producer, vtx_nodeid_t user_id, uint32_t input_index);

/**
 * Convenience macro: iterate over all use entries of a node.
 * Usage:
 *   vtx_node_t *n = vtx_node_get(table, id);
 *   for (vtx_use_entry_t *u = vtx_node_uses_begin(n);
 *        u != vtx_node_uses_end(n); ++u) {
 *       vtx_nodeid_t user = u->user_id;
 *       uint32_t inp_idx = u->input_index;
 *       ...
 *   }
 */

#endif /* VORTEX_NODE_H */
