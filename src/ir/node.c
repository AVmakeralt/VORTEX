#include "ir/node.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ========================================================================== */
/* Opcode metadata table                                                       */
/* ========================================================================== */

#define OP_INFO(name, flags, fixed_in) \
    { #name, (flags), (fixed_in) }

const vtx_node_opcode_info_t vtx_node_opcode_table[VTX_NODE_OP_COUNT] = {
    /* Control */
    OP_INFO(Start,          VTX_NF_CONTROL, 0),
    OP_INFO(End,            VTX_NF_CONTROL, 1),
    OP_INFO(If,             VTX_NF_CONTROL | VTX_NF_DATA, 2),   /* control + condition */
    OP_INFO(Goto,           VTX_NF_CONTROL, 1),
    OP_INFO(Switch,         VTX_NF_CONTROL | VTX_NF_DATA, 2),   /* control + index */
    OP_INFO(LoopBegin,      VTX_NF_CONTROL, 1),
    OP_INFO(LoopEnd,        VTX_NF_CONTROL, 1),
    OP_INFO(Return,         VTX_NF_CONTROL | VTX_NF_DATA, 2),   /* control + optional value */
    OP_INFO(Unwind,         VTX_NF_CONTROL | VTX_NF_DATA | VTX_NF_SIDE_EFFECT, 2),
    OP_INFO(Catch,          VTX_NF_CONTROL, 1),
    OP_INFO(Province,       VTX_NF_CONTROL, 0),

    /* Data: constants and parameters */
    OP_INFO(Constant,       VTX_NF_DATA, 0),
    OP_INFO(Parameter,      VTX_NF_DATA, 1),   /* input: Start */

    /* Data: arithmetic */
    OP_INFO(Add,            VTX_NF_DATA, 2),
    OP_INFO(Sub,            VTX_NF_DATA, 2),
    OP_INFO(Mul,            VTX_NF_DATA, 2),
    OP_INFO(Div,            VTX_NF_DATA, 2),
    OP_INFO(Mod,            VTX_NF_DATA, 2),
    OP_INFO(Shl,            VTX_NF_DATA, 2),
    OP_INFO(Shr,            VTX_NF_DATA, 2),
    OP_INFO(And,            VTX_NF_DATA, 2),
    OP_INFO(Or,             VTX_NF_DATA, 2),
    OP_INFO(Xor,            VTX_NF_DATA, 2),
    OP_INFO(Neg,            VTX_NF_DATA, 1),
    OP_INFO(Not,            VTX_NF_DATA, 1),

    /* Data: comparisons */
    OP_INFO(Cmp,            VTX_NF_DATA, 2),
    OP_INFO(CmpP,           VTX_NF_DATA, 2),
    OP_INFO(CmpF,           VTX_NF_DATA, 2),
    OP_INFO(CmpD,           VTX_NF_DATA, 2),

    /* Memory */
    OP_INFO(Load,           VTX_NF_MEMORY | VTX_NF_DATA, 2),    /* memory state + address */
    OP_INFO(Store,          VTX_NF_MEMORY | VTX_NF_SIDE_EFFECT, 3), /* memory + address + value */
    OP_INFO(LoadField,      VTX_NF_MEMORY | VTX_NF_DATA, 2),    /* memory + object */
    OP_INFO(StoreField,     VTX_NF_MEMORY | VTX_NF_SIDE_EFFECT, 3),
    OP_INFO(LoadIndexed,    VTX_NF_MEMORY | VTX_NF_DATA, 3),    /* memory + array + index */
    OP_INFO(StoreIndexed,   VTX_NF_MEMORY | VTX_NF_SIDE_EFFECT, 4),
    OP_INFO(MemBar,         VTX_NF_MEMORY | VTX_NF_SIDE_EFFECT, 1),
    OP_INFO(Initialize,     VTX_NF_MEMORY | VTX_NF_SIDE_EFFECT, 2),

    /* Calls */
    OP_INFO(CallStatic,     VTX_NF_CONTROL | VTX_NF_MEMORY | VTX_NF_SIDE_EFFECT | VTX_NF_DATA, 2),
    OP_INFO(CallVirtual,    VTX_NF_CONTROL | VTX_NF_MEMORY | VTX_NF_SIDE_EFFECT | VTX_NF_DATA, 3),
    OP_INFO(CallInterface,  VTX_NF_CONTROL | VTX_NF_MEMORY | VTX_NF_SIDE_EFFECT | VTX_NF_DATA, 3),
    OP_INFO(CallRuntime,    VTX_NF_CONTROL | VTX_NF_MEMORY | VTX_NF_SIDE_EFFECT | VTX_NF_DATA, 2),

    /* Type operations */
    OP_INFO(CheckCast,      VTX_NF_DATA | VTX_NF_SIDE_EFFECT, 1), /* object */
    OP_INFO(InstanceOf,     VTX_NF_DATA, 1),
    OP_INFO(Guard,          VTX_NF_CONTROL | VTX_NF_DATA | VTX_NF_SIDE_EFFECT, 2), /* control + condition */
    OP_INFO(Phi,            VTX_NF_DATA | VTX_NF_PINNED, 0),   /* variable: one per predecessor */
    OP_INFO(Region,         VTX_NF_CONTROL | VTX_NF_PINNED, 0), /* variable: one per predecessor */
    OP_INFO(Proj,           VTX_NF_DATA, 1),   /* input node + which output */

    /* Allocation */
    OP_INFO(NewObject,      VTX_NF_MEMORY | VTX_NF_SIDE_EFFECT | VTX_NF_DATA, 1), /* memory */
    OP_INFO(NewArray,       VTX_NF_MEMORY | VTX_NF_SIDE_EFFECT | VTX_NF_DATA, 2), /* memory + size */
    OP_INFO(Allocate,       VTX_NF_MEMORY | VTX_NF_SIDE_EFFECT | VTX_NF_DATA, 2), /* memory + size */
    OP_INFO(InitializeKlass,VTX_NF_MEMORY | VTX_NF_SIDE_EFFECT | VTX_NF_CONTROL, 1), /* memory */

    /* Deoptimization */
    OP_INFO(Deopt,          VTX_NF_CONTROL | VTX_NF_SIDE_EFFECT, 2), /* control + FrameState */
    OP_INFO(DeoptGuard,     VTX_NF_CONTROL | VTX_NF_DATA | VTX_NF_SIDE_EFFECT, 3),
    OP_INFO(FrameState,     VTX_NF_DATA | VTX_NF_PINNED, 0),  /* variable: locals + stack */
};

#undef OP_INFO

/* ========================================================================== */
/* Helper: classify opcodes by category                                        */
/* ========================================================================== */

bool vtx_node_is_side_effecting(vtx_node_opcode_t opcode)
{
    VTX_ASSERT(opcode < VTX_NODE_OP_COUNT, "invalid opcode");
    return vtx_nf_has(vtx_node_opcode_table[opcode].default_flags, VTX_NF_SIDE_EFFECT);
}

bool vtx_node_is_control(vtx_node_opcode_t opcode)
{
    VTX_ASSERT(opcode < VTX_NODE_OP_COUNT, "invalid opcode");
    return vtx_nf_has(vtx_node_opcode_table[opcode].default_flags, VTX_NF_CONTROL);
}

bool vtx_node_is_memory(vtx_node_opcode_t opcode)
{
    VTX_ASSERT(opcode < VTX_NODE_OP_COUNT, "invalid opcode");
    return vtx_nf_has(vtx_node_opcode_table[opcode].default_flags, VTX_NF_MEMORY);
}

bool vtx_node_is_data(vtx_node_opcode_t opcode)
{
    VTX_ASSERT(opcode < VTX_NODE_OP_COUNT, "invalid opcode");
    return vtx_nf_has(vtx_node_opcode_table[opcode].default_flags, VTX_NF_DATA);
}

vtx_nodetype_t vtx_node_default_type(vtx_node_opcode_t opcode)
{
    VTX_ASSERT(opcode < VTX_NODE_OP_COUNT, "invalid opcode");

    switch (opcode) {
    /* Control nodes produce control, represented as Void in the type lattice */
    case VTX_OP_Start:
    case VTX_OP_End:
    case VTX_OP_If:
    case VTX_OP_Goto:
    case VTX_OP_Switch:
    case VTX_OP_LoopBegin:
    case VTX_OP_LoopEnd:
    case VTX_OP_Return:
    case VTX_OP_Unwind:
    case VTX_OP_Catch:
    case VTX_OP_Province:
    case VTX_OP_Region:
    case VTX_OP_MemBar:
    case VTX_OP_Initialize:
    case VTX_OP_Store:
    case VTX_OP_StoreField:
    case VTX_OP_StoreIndexed:
    case VTX_OP_Deopt:
    case VTX_OP_DeoptGuard:
    case VTX_OP_Guard:
    case VTX_OP_InitializeKlass:
        return VTX_TYPE_Void;

    /* Integer-producing nodes */
    case VTX_OP_Parameter:
    case VTX_OP_Add:
    case VTX_OP_Sub:
    case VTX_OP_Mul:
    case VTX_OP_Div:
    case VTX_OP_Mod:
    case VTX_OP_Shl:
    case VTX_OP_Shr:
    case VTX_OP_And:
    case VTX_OP_Or:
    case VTX_OP_Xor:
    case VTX_OP_Neg:
    case VTX_OP_Not:
    case VTX_OP_Cmp:
    case VTX_OP_CmpP:
    case VTX_OP_InstanceOf:
        return VTX_TYPE_Int;

    /* Float-producing nodes */
    case VTX_OP_CmpF:
    case VTX_OP_CmpD:
        return VTX_TYPE_Float;

    /* Pointer-producing nodes */
    case VTX_OP_Load:
    case VTX_OP_LoadField:
    case VTX_OP_LoadIndexed:
    case VTX_OP_NewObject:
    case VTX_OP_NewArray:
    case VTX_OP_Allocate:
    case VTX_OP_CheckCast:
    case VTX_OP_CallStatic:
    case VTX_OP_CallVirtual:
    case VTX_OP_CallInterface:
    case VTX_OP_CallRuntime:
        return VTX_TYPE_Bottom; /* could be anything until we know more */

    /* Constant: type set from constval */
    case VTX_OP_Constant:
        return VTX_TYPE_Top;

    /* Phi: type determined by inputs */
    case VTX_OP_Phi:
    case VTX_OP_Proj:
    case VTX_OP_FrameState:
        return VTX_TYPE_Top;

    default:
        return VTX_TYPE_Bottom;
    }
}

const char *vtx_node_opcode_name(vtx_node_opcode_t opcode)
{
    if (opcode >= VTX_NODE_OP_COUNT) {
        return "<invalid-opcode>";
    }
    return vtx_node_opcode_table[opcode].name;
}

const char *vtx_nodetype_name(vtx_nodetype_t t)
{
    switch (t) {
    case VTX_TYPE_Bottom: return "Bottom";
    case VTX_TYPE_Int:    return "Int";
    case VTX_TYPE_Float:  return "Float";
    case VTX_TYPE_Ptr:    return "Ptr";
    case VTX_TYPE_Void:   return "Void";
    case VTX_TYPE_Top:    return "Top";
    }
    return "<unknown-type>";
}

/* ========================================================================== */
/* Constant value helpers                                                      */
/* ========================================================================== */

bool vtx_constval_equal(vtx_constval_t a, vtx_constval_t b)
{
    if (a.kind != b.kind) return false;
    switch (a.kind) {
    case VTX_TYPE_Int:   return a.as.int_val   == b.as.int_val;
    case VTX_TYPE_Float: return a.as.float_val  == b.as.float_val;
    case VTX_TYPE_Ptr:   return a.as.ptr_val    == b.as.ptr_val;
    case VTX_TYPE_Void:  return true;
    default:             return false;
    }
}

uint32_t vtx_constval_hash(vtx_constval_t v)
{
    /* FNV-1a-style hash combining kind and value bits */
    uint32_t h = 2166136261u;
    h ^= (uint32_t)v.kind;
    h *= 16777619u;
    switch (v.kind) {
    case VTX_TYPE_Int: {
        uint64_t x = (uint64_t)v.as.int_val;
        h ^= (uint32_t)(x & 0xFFFFFFFF);
        h *= 16777619u;
        h ^= (uint32_t)(x >> 32);
        h *= 16777619u;
        break;
    }
    case VTX_TYPE_Float: {
        union { double d; uint64_t u; } u;
        u.d = v.as.float_val;
        h ^= (uint32_t)(u.u & 0xFFFFFFFF);
        h *= 16777619u;
        h ^= (uint32_t)(u.u >> 32);
        h *= 16777619u;
        break;
    }
    case VTX_TYPE_Ptr: {
        uintptr_t p = (uintptr_t)v.as.ptr_val;
        h ^= (uint32_t)(p & 0xFFFFFFFF);
        h *= 16777619u;
        h ^= (uint32_t)(p >> 32);
        h *= 16777619u;
        break;
    }
    default:
        break;
    }
    return h;
}

/* ========================================================================== */
/* Node table operations                                                       */
/* ========================================================================== */

int vtx_node_table_init(vtx_node_table_t *table, uint32_t initial_capacity)
{
    VTX_ASSERT(table != NULL, "table must not be NULL");

    if (initial_capacity == 0) {
        initial_capacity = VTX_NODE_TABLE_INITIAL_CAPACITY;
    }

    table->nodes = (vtx_node_t *)calloc(initial_capacity, sizeof(vtx_node_t));
    if (table->nodes == NULL) {
        table->count = 0;
        table->capacity = 0;
        table->next_id = 0;
        return -1;
    }

    table->count = 0;
    table->capacity = initial_capacity;
    table->next_id = 0;
    return 0;
}

void vtx_node_table_destroy(vtx_node_table_t *table)
{
    VTX_ASSERT(table != NULL, "table must not be NULL");

    if (table->nodes != NULL) {
        /* Free each node's inputs array */
        for (uint32_t i = 0; i < table->count; i++) {
            vtx_node_t *node = &table->nodes[i];
            if (node->inputs != NULL) {
                free(node->inputs);
                node->inputs = NULL;
            }
        }
        free(table->nodes);
        table->nodes = NULL;
    }

    table->count = 0;
    table->capacity = 0;
    table->next_id = 0;
}

/**
 * Grow the node table to at least min_capacity.
 * Returns 0 on success, -1 on failure.
 */
static int node_table_grow(vtx_node_table_t *table, uint32_t min_capacity)
{
    uint32_t new_cap = table->capacity;
    while (new_cap < min_capacity) {
        uint32_t doubled = new_cap * 2;
        /* Guard against overflow */
        if (doubled <= new_cap) {
            new_cap = min_capacity;
            break;
        }
        new_cap = doubled;
    }

    vtx_node_t *new_nodes = (vtx_node_t *)realloc(table->nodes, new_cap * sizeof(vtx_node_t));
    if (new_nodes == NULL) {
        return -1;
    }

    /* Zero out the newly allocated portion */
    memset(new_nodes + table->capacity, 0, (new_cap - table->capacity) * sizeof(vtx_node_t));

    table->nodes = new_nodes;
    table->capacity = new_cap;
    return 0;
}

vtx_nodeid_t vtx_node_create(vtx_node_table_t *table, vtx_node_opcode_t opcode)
{
    VTX_ASSERT(table != NULL, "table must not be NULL");
    VTX_ASSERT(opcode < VTX_NODE_OP_COUNT, "invalid opcode");

    /* Ensure space */
    if (table->count >= table->capacity) {
        if (node_table_grow(table, table->count + 1) != 0) {
            return VTX_NODEID_INVALID;
        }
    }

    vtx_nodeid_t id = table->next_id;
    VTX_ASSERT(id == table->count, "ID must equal array index");

    vtx_node_t *node = &table->nodes[id];
    memset(node, 0, sizeof(vtx_node_t));

    node->id = id;
    node->opcode = opcode;
    node->type = vtx_node_default_type(opcode);
    node->flags = vtx_node_opcode_table[opcode].default_flags;
    node->input_count = 0;
    node->input_capacity = 0;
    node->inputs = NULL;
    node->output_count = 0;
    node->value_number = 0;
    node->dead = false;
    node->mark = false;

    /* Zero out auxiliary fields */
    node->cond = VTX_COND_ALWAYS;
    node->local_index = 0;
    node->field_offset = 0;
    node->method_index = 0;
    node->type_id = 0;
    node->bytecode_pc = 0;
    node->frame_state = VTX_NODEID_INVALID;
    node->constval.kind = VTX_TYPE_Top;
    node->constval.as.int_val = 0;

    table->count++;
    table->next_id++;
    return id;
}

vtx_node_t *vtx_node_get(vtx_node_table_t *table, vtx_nodeid_t id)
{
    if (id == VTX_NODEID_INVALID || id >= table->count) {
        return NULL;
    }
    return &table->nodes[id];
}

const vtx_node_t *vtx_node_get_const(const vtx_node_table_t *table, vtx_nodeid_t id)
{
    if (id == VTX_NODEID_INVALID || id >= table->count) {
        return NULL;
    }
    return &table->nodes[id];
}

/**
 * Ensure the inputs array of a node has room for at least min_cap entries.
 * Returns 0 on success, -1 on failure.
 */
static int node_ensure_input_capacity(vtx_node_t *node, uint32_t min_cap)
{
    if (node->input_capacity >= min_cap) {
        return 0;
    }

    uint32_t new_cap = node->input_capacity;
    if (new_cap == 0) {
        new_cap = VTX_NODE_INITIAL_INPUT_CAPACITY;
    }
    while (new_cap < min_cap) {
        uint32_t doubled = new_cap * 2;
        if (doubled <= new_cap) {
            new_cap = min_cap;
            break;
        }
        new_cap = doubled;
    }

    vtx_nodeid_t *new_inputs = (vtx_nodeid_t *)realloc(node->inputs, new_cap * sizeof(vtx_nodeid_t));
    if (new_inputs == NULL) {
        return -1;
    }

    /* Initialize new slots to INVALID */
    for (uint32_t i = node->input_capacity; i < new_cap; i++) {
        new_inputs[i] = VTX_NODEID_INVALID;
    }

    node->inputs = new_inputs;
    node->input_capacity = new_cap;
    return 0;
}

int vtx_node_add_input(vtx_node_table_t *table, vtx_nodeid_t consumer, vtx_nodeid_t producer)
{
    VTX_ASSERT(table != NULL, "table must not be NULL");
    VTX_ASSERT(consumer != VTX_NODEID_INVALID, "consumer must be valid");
    VTX_ASSERT(producer != VTX_NODEID_INVALID, "producer must be valid");

    vtx_node_t *c = vtx_node_get(table, consumer);
    if (c == NULL) return -1;

    /* Validate producer exists */
    if (producer >= table->count) return -1;

    if (node_ensure_input_capacity(c, c->input_count + 1) != 0) {
        return -1;
    }

    c->inputs[c->input_count] = producer;
    c->input_count++;

    /* Increment producer's output count */
    table->nodes[producer].output_count++;
    return 0;
}

int vtx_node_remove_input(vtx_node_table_t *table, vtx_nodeid_t consumer, uint32_t index)
{
    VTX_ASSERT(table != NULL, "table must not be NULL");

    vtx_node_t *c = vtx_node_get(table, consumer);
    if (c == NULL) return -1;
    if (index >= c->input_count) return -1;

    vtx_nodeid_t old_producer = c->inputs[index];

    /* Decrement old producer's output count */
    if (old_producer != VTX_NODEID_INVALID && old_producer < table->count) {
        vtx_node_t *old = &table->nodes[old_producer];
        VTX_ASSERT(old->output_count > 0, "output count underflow");
        old->output_count--;
    }

    /* Shift subsequent inputs down */
    for (uint32_t i = index; i + 1 < c->input_count; i++) {
        c->inputs[i] = c->inputs[i + 1];
    }
    c->input_count--;

    return 0;
}

int vtx_node_replace_input(vtx_node_table_t *table, vtx_nodeid_t consumer,
                           uint32_t index, vtx_nodeid_t new_producer)
{
    VTX_ASSERT(table != NULL, "table must not be NULL");
    VTX_ASSERT(new_producer != VTX_NODEID_INVALID, "new producer must be valid");

    vtx_node_t *c = vtx_node_get(table, consumer);
    if (c == NULL) return -1;
    if (index >= c->input_count) return -1;

    /* Validate new producer exists */
    if (new_producer >= table->count) return -1;

    vtx_nodeid_t old_producer = c->inputs[index];
    if (old_producer == new_producer) {
        return 0; /* no-op */
    }

    /* Decrement old producer's output count */
    if (old_producer != VTX_NODEID_INVALID && old_producer < table->count) {
        vtx_node_t *old = &table->nodes[old_producer];
        VTX_ASSERT(old->output_count > 0, "output count underflow");
        old->output_count--;
    }

    /* Set the new input */
    c->inputs[index] = new_producer;

    /* Increment new producer's output count */
    table->nodes[new_producer].output_count++;

    return 0;
}

int vtx_node_find_input(const vtx_node_t *node, vtx_nodeid_t producer)
{
    if (node == NULL || node->inputs == NULL) return -1;
    for (uint32_t i = 0; i < node->input_count; i++) {
        if (node->inputs[i] == producer) {
            return (int)i;
        }
    }
    return -1;
}

void vtx_node_table_clear_marks(vtx_node_table_t *table)
{
    VTX_ASSERT(table != NULL, "table must not be NULL");
    for (uint32_t i = 0; i < table->count; i++) {
        table->nodes[i].mark = false;
    }
}

void vtx_node_table_clear_dead(vtx_node_table_t *table)
{
    VTX_ASSERT(table != NULL, "table must not be NULL");
    for (uint32_t i = 0; i < table->count; i++) {
        table->nodes[i].dead = false;
    }
}
