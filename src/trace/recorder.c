/**
 * VORTEX Trace Recorder — Implementation
 *
 * Walks bytecode following the hot path (determined by profiling data)
 * and emits Sea-of-Nodes IR nodes into the graph. At branch points,
 * the hot path is followed and the cold path is recorded as a Guard
 * (side exit) node.
 *
 * This is the core of the trace recording pipeline. It produces
 * vtx_trace_t structures that can then be organized into trace trees
 * and stitched into hyperblocks.
 */

#include "trace/recorder.h"
#include "trace/side_exit.h"
#include "runtime/arena.h"
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* Internal: recording state                                                   */
/* ========================================================================== */

/**
 * Per-trace recording state. Maintained during a single trace recording
 * session to track operand stack, local variables, and control/memory chains.
 */
typedef struct {
    vtx_trace_t        *trace;         /* the trace being built */
    vtx_graph_t        *graph;         /* SoN graph */
    const vtx_bytecode_t *bytecode;    /* bytecode module */
    const vtx_method_desc_t *method;   /* method descriptor */
    const vtx_profiler_t *profiler;    /* interpreter profile */
    const vtx_profile_global_t *profile; /* global profile */
    vtx_arena_t        *arena;         /* arena allocator */
    vtx_side_exit_table_t *exit_table; /* side exit table for this trace */
    uint32_t            max_trace_length;

    /* Operand stack (simulated during recording) */
    vtx_nodeid_t       *stack;         /* stack of NodeIDs */
    uint32_t            stack_top;     /* index of top-of-stack + 1 */
    uint32_t            stack_capacity;

    /* Local variable map: local_index → producing NodeID */
    vtx_nodeid_t       *locals;        /* array of size max_locals */
    uint32_t            max_locals;

    /* Current control and memory chain positions */
    vtx_nodeid_t        control;       /* current control node */
    vtx_nodeid_t        memory;        /* current memory state node */

    /* Current PC being processed */
    uint32_t            pc;

    /* Whether recording should stop */
    bool                stopped;

    /* Whether a stack overflow occurred during recording */
    bool                stack_overflow;
} vtx_record_state_t;

/* ========================================================================== */
/* Operand stack operations                                                    */
/* ========================================================================== */

static int vtx_record_stack_init(vtx_record_state_t *state, uint16_t max_stack)
{
    state->stack_capacity = (max_stack > 0) ? max_stack : 16;
    state->stack = vtx_arena_alloc(state->arena,
                                    sizeof(vtx_nodeid_t) * state->stack_capacity);
    if (state->stack == NULL) return -1;
    memset(state->stack, 0, sizeof(vtx_nodeid_t) * state->stack_capacity);
    state->stack_top = 0;
    return 0;
}

static int vtx_record_stack_push(vtx_record_state_t *state, vtx_nodeid_t node)
{
    VTX_ASSERT(state->stack_top < state->stack_capacity,
               "operand stack overflow during trace recording");
    if (state->stack_top >= state->stack_capacity) {
        state->stack_overflow = true;
        return -1;
    }
    state->stack[state->stack_top++] = node;
    return 0;
}

static vtx_nodeid_t vtx_record_stack_pop(vtx_record_state_t *state)
{
    VTX_ASSERT(state->stack_top > 0,
               "operand stack underflow during trace recording");
    if (state->stack_top == 0) return VTX_NODEID_INVALID;
    return state->stack[--state->stack_top];
}

static vtx_nodeid_t vtx_record_stack_peek(vtx_record_state_t *state, uint32_t offset)
{
    VTX_ASSERT(offset < state->stack_top, "operand stack peek out of bounds");
    if (offset >= state->stack_top) return VTX_NODEID_INVALID;
    return state->stack[state->stack_top - 1 - offset];
}

/* ========================================================================== */
/* Trace node emission helpers                                                 */
/* ========================================================================== */

/**
 * Emit a SoN node and add it to the trace's node sequence.
 */
static vtx_nodeid_t vtx_record_emit_node(vtx_record_state_t *state,
                                          vtx_node_opcode_t opcode)
{
    vtx_nodeid_t nid = vtx_node_create(&state->graph->node_table, opcode);
    if (nid == VTX_NODEID_INVALID) return VTX_NODEID_INVALID;

    /* Grow trace node array if needed */
    if (state->trace->node_count >= state->trace->node_capacity) {
        uint32_t new_cap = state->trace->node_capacity * 2;
        vtx_nodeid_t *new_nodes = vtx_arena_alloc(state->arena,
                                                    sizeof(vtx_nodeid_t) * new_cap);
        if (new_nodes == NULL) return VTX_NODEID_INVALID;
        memcpy(new_nodes, state->trace->nodes,
               sizeof(vtx_nodeid_t) * state->trace->node_count);
        state->trace->nodes = new_nodes;
        state->trace->node_capacity = new_cap;
    }

    state->trace->nodes[state->trace->node_count++] = nid;
    return nid;
}

/**
 * Add a side exit to the trace.
 */
static int vtx_record_add_side_exit(vtx_record_state_t *state,
                                     vtx_side_exit_t *exit)
{
    if (state->trace->side_exit_count >= state->trace->side_exit_capacity) {
        uint32_t new_cap = state->trace->side_exit_capacity * 2;
        vtx_side_exit_t **new_exits = vtx_arena_alloc(state->arena,
                                                        sizeof(vtx_side_exit_t *) * new_cap);
        if (new_exits == NULL) return -1;
        memcpy(new_exits, state->trace->side_exits,
               sizeof(vtx_side_exit_t *) * state->trace->side_exit_count);
        state->trace->side_exits = new_exits;
        state->trace->side_exit_capacity = new_cap;
    }
    state->trace->side_exits[state->trace->side_exit_count++] = exit;
    return 0;
}

/* ========================================================================== */
/* Branch probability helpers                                                  */
/* ========================================================================== */

/**
 * Determine if a branch at the given PC is likely taken.
 * Uses profile data to determine the hot path.
 * Returns true if the branch is taken more often than not.
 */
static bool vtx_record_branch_is_taken(vtx_record_state_t *state, uint32_t pc)
{
    /* First check the interpreter profiler */
    if (state->profiler != NULL && state->method != NULL) {
        double prob = vtx_profiler_get_branch_probability(
            state->profiler, state->method, pc);
        if (prob > 0.5) return true;
        if (prob < 0.5) return false;
        /* prob == 0.5 means no data — fall through to global profile */
    }

    /* Then check the global profile */
    if (state->profile != NULL && state->trace->method_id != 0) {
        const vtx_branch_profile_t *bp = vtx_profile_get_branch(
            state->profile, state->trace->method_id, pc);
        if (bp != NULL && (bp->taken + bp->not_taken) > 0) {
            return bp->taken > bp->not_taken;
        }
    }

    /* Default: assume branch is not taken (fall-through is hot) */
    return false;
}

/* ========================================================================== */
/* Helper: count method arguments from signature string                        */
/* ========================================================================== */

/**
 * Parse a JVM-style method signature to count the number of arguments.
 * Signature format: "(ArgDescriptors)ReturnDescriptor"
 * e.g., "(II)I" has 2 args, "(Ljava/lang/String;I)V" has 2 args,
 *       "([I[D)V" has 2 args (int[] and double[]).
 *
 * Returns the number of arguments, or 0 if the signature is NULL/invalid.
 */
static uint32_t vtx_count_method_args_from_sig(const char *sig)
{
    if (sig == NULL || sig[0] != '(') return 0;

    uint32_t count = 0;
    uint32_t i = 1; /* skip '(' */

    while (sig[i] != '\0' && sig[i] != ')') {
        if (sig[i] == 'B' || sig[i] == 'C' || sig[i] == 'D' ||
            sig[i] == 'F' || sig[i] == 'I' || sig[i] == 'J' ||
            sig[i] == 'S' || sig[i] == 'Z') {
            /* Primitive type — one argument */
            count++;
            i++;
        } else if (sig[i] == 'L') {
            /* Object type: Lclassname; — skip to ';' */
            count++;
            while (sig[i] != '\0' && sig[i] != ';') i++;
            if (sig[i] == ';') i++;
        } else if (sig[i] == '[') {
            /* Array type: skip all '[' then process the element type */
            while (sig[i] == '[') i++;
            /* Now process the element type (without incrementing count) */
            if (sig[i] == 'L') {
                while (sig[i] != '\0' && sig[i] != ';') i++;
                if (sig[i] == ';') i++;
            } else if (sig[i] != '\0') {
                i++; /* primitive element type */
            }
            count++;
        } else {
            /* Unknown character — skip */
            i++;
        }
    }

    return count;
}

/* ========================================================================== */
/* Bytecode instruction handlers                                               */
/* ========================================================================== */

/**
 * Process a single bytecode instruction during trace recording.
 * Emits the corresponding SoN node(s) and updates the recording state.
 *
 * Returns 0 on success, -1 on failure, 1 if recording should stop.
 */
static int vtx_record_instruction(vtx_record_state_t *state)
{
    const vtx_bytecode_t *bc = state->bytecode;
    vtx_opcode_t opcode = vtx_bytecode_opcode_at(bc, state->pc);
    uint16_t operand = 0;
    const vtx_opcode_info_t *info = &vtx_opcode_table[opcode];

    /* Read operand if present */
    if (info->has_operand) {
        operand = vtx_bytecode_read_operand(bc, state->pc);
    }

    /* Check trace length budget */
    if (state->trace->bytecode_length >= state->max_trace_length) {
        state->trace->kind = VTX_TRACE_TRUNCATED;
        state->stopped = true;
        return 1;
    }

    state->trace->bytecode_length++;

    switch (opcode) {
    /* ---- Constants ---- */
    case VT_OP_LOAD_CONST_INT: {
        vtx_nodeid_t nid = vtx_record_emit_node(state, VTX_OP_Constant);
        if (nid == VTX_NODEID_INVALID) return -1;
        vtx_node_t *node = vtx_node_get(&state->graph->node_table, nid);
        if (node == NULL) return -1;
        /* Look up the constant value from the pool */
        if (operand < bc->constant_count) {
            vtx_value_t val = bc->constant_pool[operand];
            if (vtx_is_smi(val)) {
                node->constval = vtx_constval_int(vtx_smi_value(val));
                node->type = VTX_TYPE_Int;
            } else if (vtx_is_double(val)) {
                node->constval = vtx_constval_float(vtx_double_value(val));
                node->type = VTX_TYPE_Float;
            } else {
                node->constval = vtx_constval_int((int64_t)operand);
                node->type = VTX_TYPE_Int;
            }
        } else {
            node->constval = vtx_constval_int((int64_t)operand);
            node->type = VTX_TYPE_Int;
        }
        vtx_record_stack_push(state, nid);
        break;
    }
    case VT_OP_LOAD_CONST_FLOAT: {
        vtx_nodeid_t nid = vtx_record_emit_node(state, VTX_OP_Constant);
        if (nid == VTX_NODEID_INVALID) return -1;
        vtx_node_t *node = vtx_node_get(&state->graph->node_table, nid);
        if (node == NULL) return -1;
        if (operand < bc->constant_count) {
            vtx_value_t val = bc->constant_pool[operand];
            node->constval = vtx_constval_float(vtx_double_value(val));
        } else {
            node->constval = vtx_constval_float((double)operand);
        }
        node->type = VTX_TYPE_Float;
        vtx_record_stack_push(state, nid);
        break;
    }
    case VT_OP_LOAD_CONST_STR: {
        vtx_nodeid_t nid = vtx_record_emit_node(state, VTX_OP_Constant);
        if (nid == VTX_NODEID_INVALID) return -1;
        vtx_node_t *node = vtx_node_get(&state->graph->node_table, nid);
        if (node == NULL) return -1;
        node->constval = vtx_constval_ptr((void *)(uintptr_t)operand);
        node->type = VTX_TYPE_Ptr;
        vtx_record_stack_push(state, nid);
        break;
    }
    case VT_OP_LOAD_NULL: {
        vtx_nodeid_t nid = vtx_record_emit_node(state, VTX_OP_Constant);
        if (nid == VTX_NODEID_INVALID) return -1;
        vtx_node_t *node = vtx_node_get(&state->graph->node_table, nid);
        if (node == NULL) return -1;
        node->constval = vtx_constval_ptr(NULL);
        node->type = VTX_TYPE_Ptr;
        vtx_record_stack_push(state, nid);
        break;
    }
    case VT_OP_LOAD_TRUE:
    case VT_OP_LOAD_FALSE: {
        vtx_nodeid_t nid = vtx_record_emit_node(state, VTX_OP_Constant);
        if (nid == VTX_NODEID_INVALID) return -1;
        vtx_node_t *node = vtx_node_get(&state->graph->node_table, nid);
        if (node == NULL) return -1;
        node->constval = vtx_constval_int(opcode == VT_OP_LOAD_TRUE ? 1 : 0);
        node->type = VTX_TYPE_Int;
        vtx_record_stack_push(state, nid);
        break;
    }
    case VT_OP_LOAD_UNDEFINED: {
        vtx_nodeid_t nid = vtx_record_emit_node(state, VTX_OP_Constant);
        if (nid == VTX_NODEID_INVALID) return -1;
        vtx_node_t *node = vtx_node_get(&state->graph->node_table, nid);
        if (node == NULL) return -1;
        node->constval = vtx_constval_void();
        node->type = VTX_TYPE_Void;
        vtx_record_stack_push(state, nid);
        break;
    }

    /* ---- Local variable access ---- */
    case VT_OP_LOAD_LOCAL: {
        if (operand < state->max_locals && state->locals[operand] != VTX_NODEID_INVALID) {
            vtx_record_stack_push(state, state->locals[operand]);
        } else {
            /* Local not yet assigned — emit a Parameter node */
            vtx_nodeid_t nid = vtx_record_emit_node(state, VTX_OP_Parameter);
            if (nid == VTX_NODEID_INVALID) return -1;
            vtx_node_t *node = vtx_node_get(&state->graph->node_table, nid);
            if (node == NULL) return -1;
            node->local_index = operand;
            node->type = VTX_TYPE_Bottom;
            vtx_record_stack_push(state, nid);
        }
        break;
    }
    case VT_OP_STORE_LOCAL: {
        vtx_nodeid_t val = vtx_record_stack_pop(state);
        if (val == VTX_NODEID_INVALID) return -1;
        if (operand < state->max_locals) {
            state->locals[operand] = val;
        }
        break;
    }

    /* ---- Field access ---- */
    case VT_OP_LOAD_FIELD: {
        /* Capture stack depth before consuming inputs (Bug 1 fix) */
        uint32_t pre_sp = state->stack_top;
        vtx_nodeid_t obj = vtx_record_stack_pop(state);
        if (obj == VTX_NODEID_INVALID) return -1;

        /* Emit a null-check guard */
        vtx_nodeid_t guard_nid = vtx_record_emit_node(state, VTX_OP_Guard);
        if (guard_nid == VTX_NODEID_INVALID) return -1;
        vtx_node_t *guard = vtx_node_get(&state->graph->node_table, guard_nid);
        if (guard == NULL) return -1;
        guard->cond = VTX_COND_NE;
        vtx_node_add_input(&state->graph->node_table, guard_nid, obj);
        vtx_node_add_input(&state->graph->node_table, guard_nid, state->control);

        /* Create side exit for null check failure */
        vtx_side_exit_t *exit = vtx_side_exit_create(
            state->exit_table, state->arena,
            state->pc, /* target PC: re-execute this instruction in interpreter */
            state->stack, pre_sp,
            state->locals, state->max_locals,
            VTX_EXIT_NULL_CHECK_FAILED, guard_nid, state->trace->trace_id);
        if (exit != NULL) {
            vtx_record_add_side_exit(state, exit);
        }

        /* Update control chain (Bug 7 fix) */
        state->control = guard_nid;

        /* Emit the field load */
        vtx_nodeid_t nid = vtx_record_emit_node(state, VTX_OP_LoadField);
        if (nid == VTX_NODEID_INVALID) return -1;
        vtx_node_t *node = vtx_node_get(&state->graph->node_table, nid);
        if (node == NULL) return -1;
        node->field_offset = operand;
        node->type = VTX_TYPE_Bottom;
        vtx_node_add_input(&state->graph->node_table, nid, obj);
        vtx_node_add_input(&state->graph->node_table, nid, state->memory);
        vtx_record_stack_push(state, nid);

        /* Update memory chain */
        state->memory = nid;
        break;
    }
    case VT_OP_STORE_FIELD: {
        /* Capture stack depth before consuming inputs (Bug 1 fix) */
        uint32_t pre_sp = state->stack_top;
        vtx_nodeid_t val = vtx_record_stack_pop(state);
        vtx_nodeid_t obj = vtx_record_stack_pop(state);
        if (val == VTX_NODEID_INVALID || obj == VTX_NODEID_INVALID) return -1;

        /* Emit a null-check guard */
        vtx_nodeid_t guard_nid = vtx_record_emit_node(state, VTX_OP_Guard);
        if (guard_nid == VTX_NODEID_INVALID) return -1;
        vtx_node_t *guard = vtx_node_get(&state->graph->node_table, guard_nid);
        if (guard == NULL) return -1;
        guard->cond = VTX_COND_NE;
        vtx_node_add_input(&state->graph->node_table, guard_nid, obj);
        vtx_node_add_input(&state->graph->node_table, guard_nid, state->control);

        vtx_side_exit_t *exit = vtx_side_exit_create(
            state->exit_table, state->arena,
            state->pc,
            state->stack, pre_sp,
            state->locals, state->max_locals,
            VTX_EXIT_NULL_CHECK_FAILED, guard_nid, state->trace->trace_id);
        if (exit != NULL) {
            vtx_record_add_side_exit(state, exit);
        }

        /* Update control chain (Bug 7 fix) */
        state->control = guard_nid;

        /* Emit the field store */
        vtx_nodeid_t nid = vtx_record_emit_node(state, VTX_OP_StoreField);
        if (nid == VTX_NODEID_INVALID) return -1;
        vtx_node_t *node = vtx_node_get(&state->graph->node_table, nid);
        if (node == NULL) return -1;
        node->field_offset = operand;
        node->type = VTX_TYPE_Void;
        vtx_node_add_input(&state->graph->node_table, nid, obj);
        vtx_node_add_input(&state->graph->node_table, nid, val);
        vtx_node_add_input(&state->graph->node_table, nid, state->memory);

        state->memory = nid;
        break;
    }

    /* ---- Integer arithmetic ---- */
    case VT_OP_IADD: {
        vtx_nodeid_t b = vtx_record_stack_pop(state);
        vtx_nodeid_t a = vtx_record_stack_pop(state);
        if (a == VTX_NODEID_INVALID || b == VTX_NODEID_INVALID) return -1;
        vtx_nodeid_t nid = vtx_record_emit_node(state, VTX_OP_Add);
        if (nid == VTX_NODEID_INVALID) return -1;
        vtx_node_add_input(&state->graph->node_table, nid, a);
        vtx_node_add_input(&state->graph->node_table, nid, b);
        vtx_node_t *node = vtx_node_get(&state->graph->node_table, nid);
        if (node) node->type = VTX_TYPE_Int;
        vtx_record_stack_push(state, nid);
        break;
    }
    case VT_OP_ISUB: {
        vtx_nodeid_t b = vtx_record_stack_pop(state);
        vtx_nodeid_t a = vtx_record_stack_pop(state);
        if (a == VTX_NODEID_INVALID || b == VTX_NODEID_INVALID) return -1;
        vtx_nodeid_t nid = vtx_record_emit_node(state, VTX_OP_Sub);
        if (nid == VTX_NODEID_INVALID) return -1;
        vtx_node_add_input(&state->graph->node_table, nid, a);
        vtx_node_add_input(&state->graph->node_table, nid, b);
        vtx_node_t *node = vtx_node_get(&state->graph->node_table, nid);
        if (node) node->type = VTX_TYPE_Int;
        vtx_record_stack_push(state, nid);
        break;
    }
    case VT_OP_IMUL: {
        vtx_nodeid_t b = vtx_record_stack_pop(state);
        vtx_nodeid_t a = vtx_record_stack_pop(state);
        if (a == VTX_NODEID_INVALID || b == VTX_NODEID_INVALID) return -1;
        vtx_nodeid_t nid = vtx_record_emit_node(state, VTX_OP_Mul);
        if (nid == VTX_NODEID_INVALID) return -1;
        vtx_node_add_input(&state->graph->node_table, nid, a);
        vtx_node_add_input(&state->graph->node_table, nid, b);
        vtx_node_t *node = vtx_node_get(&state->graph->node_table, nid);
        if (node) node->type = VTX_TYPE_Int;
        vtx_record_stack_push(state, nid);
        break;
    }
    case VT_OP_IDIV: {
        /* Capture stack depth before consuming inputs (Bug 1 fix) */
        uint32_t pre_sp = state->stack_top;
        vtx_nodeid_t b = vtx_record_stack_pop(state);
        vtx_nodeid_t a = vtx_record_stack_pop(state);
        if (a == VTX_NODEID_INVALID || b == VTX_NODEID_INVALID) return -1;

        /* Bug 4 fix: Emit divide-by-zero guard */
        vtx_nodeid_t dz_guard = vtx_record_emit_node(state, VTX_OP_Guard);
        if (dz_guard == VTX_NODEID_INVALID) return -1;
        vtx_node_t *dz_guard_node = vtx_node_get(&state->graph->node_table, dz_guard);
        if (dz_guard_node) dz_guard_node->cond = VTX_COND_NE;
        vtx_node_add_input(&state->graph->node_table, dz_guard, b);
        vtx_node_add_input(&state->graph->node_table, dz_guard, state->control);

        vtx_side_exit_t *dz_exit = vtx_side_exit_create(
            state->exit_table, state->arena,
            state->pc,
            state->stack, pre_sp,
            state->locals, state->max_locals,
            VTX_EXIT_DIVISION_BY_ZERO, dz_guard, state->trace->trace_id);
        if (dz_exit != NULL) {
            vtx_record_add_side_exit(state, dz_exit);
        }

        /* Update control chain (Bug 7 fix) */
        state->control = dz_guard;

        vtx_nodeid_t nid = vtx_record_emit_node(state, VTX_OP_Div);
        if (nid == VTX_NODEID_INVALID) return -1;
        vtx_node_add_input(&state->graph->node_table, nid, a);
        vtx_node_add_input(&state->graph->node_table, nid, b);
        vtx_node_t *node = vtx_node_get(&state->graph->node_table, nid);
        if (node) node->type = VTX_TYPE_Int;
        vtx_record_stack_push(state, nid);
        break;
    }
    case VT_OP_IMOD: {
        /* Capture stack depth before consuming inputs (Bug 1 fix) */
        uint32_t pre_sp = state->stack_top;
        vtx_nodeid_t b = vtx_record_stack_pop(state);
        vtx_nodeid_t a = vtx_record_stack_pop(state);
        if (a == VTX_NODEID_INVALID || b == VTX_NODEID_INVALID) return -1;

        /* Bug 4 fix: Emit divide-by-zero guard */
        vtx_nodeid_t dz_guard = vtx_record_emit_node(state, VTX_OP_Guard);
        if (dz_guard == VTX_NODEID_INVALID) return -1;
        vtx_node_t *dz_guard_node = vtx_node_get(&state->graph->node_table, dz_guard);
        if (dz_guard_node) dz_guard_node->cond = VTX_COND_NE;
        vtx_node_add_input(&state->graph->node_table, dz_guard, b);
        vtx_node_add_input(&state->graph->node_table, dz_guard, state->control);

        vtx_side_exit_t *dz_exit = vtx_side_exit_create(
            state->exit_table, state->arena,
            state->pc,
            state->stack, pre_sp,
            state->locals, state->max_locals,
            VTX_EXIT_DIVISION_BY_ZERO, dz_guard, state->trace->trace_id);
        if (dz_exit != NULL) {
            vtx_record_add_side_exit(state, dz_exit);
        }

        /* Update control chain (Bug 7 fix) */
        state->control = dz_guard;

        vtx_nodeid_t nid = vtx_record_emit_node(state, VTX_OP_Mod);
        if (nid == VTX_NODEID_INVALID) return -1;
        vtx_node_add_input(&state->graph->node_table, nid, a);
        vtx_node_add_input(&state->graph->node_table, nid, b);
        vtx_node_t *node = vtx_node_get(&state->graph->node_table, nid);
        if (node) node->type = VTX_TYPE_Int;
        vtx_record_stack_push(state, nid);
        break;
    }

    /* ---- Float arithmetic ---- */
    case VT_OP_FADD: {
        vtx_nodeid_t b = vtx_record_stack_pop(state);
        vtx_nodeid_t a = vtx_record_stack_pop(state);
        if (a == VTX_NODEID_INVALID || b == VTX_NODEID_INVALID) return -1;
        vtx_nodeid_t nid = vtx_record_emit_node(state, VTX_OP_Add);
        if (nid == VTX_NODEID_INVALID) return -1;
        vtx_node_add_input(&state->graph->node_table, nid, a);
        vtx_node_add_input(&state->graph->node_table, nid, b);
        vtx_node_t *node = vtx_node_get(&state->graph->node_table, nid);
        if (node) node->type = VTX_TYPE_Float;
        vtx_record_stack_push(state, nid);
        break;
    }
    case VT_OP_FSUB: {
        vtx_nodeid_t b = vtx_record_stack_pop(state);
        vtx_nodeid_t a = vtx_record_stack_pop(state);
        if (a == VTX_NODEID_INVALID || b == VTX_NODEID_INVALID) return -1;
        vtx_nodeid_t nid = vtx_record_emit_node(state, VTX_OP_Sub);
        if (nid == VTX_NODEID_INVALID) return -1;
        vtx_node_add_input(&state->graph->node_table, nid, a);
        vtx_node_add_input(&state->graph->node_table, nid, b);
        vtx_node_t *node = vtx_node_get(&state->graph->node_table, nid);
        if (node) node->type = VTX_TYPE_Float;
        vtx_record_stack_push(state, nid);
        break;
    }
    case VT_OP_FMUL: {
        vtx_nodeid_t b = vtx_record_stack_pop(state);
        vtx_nodeid_t a = vtx_record_stack_pop(state);
        if (a == VTX_NODEID_INVALID || b == VTX_NODEID_INVALID) return -1;
        vtx_nodeid_t nid = vtx_record_emit_node(state, VTX_OP_Mul);
        if (nid == VTX_NODEID_INVALID) return -1;
        vtx_node_add_input(&state->graph->node_table, nid, a);
        vtx_node_add_input(&state->graph->node_table, nid, b);
        vtx_node_t *node = vtx_node_get(&state->graph->node_table, nid);
        if (node) node->type = VTX_TYPE_Float;
        vtx_record_stack_push(state, nid);
        break;
    }
    case VT_OP_FDIV: {
        vtx_nodeid_t b = vtx_record_stack_pop(state);
        vtx_nodeid_t a = vtx_record_stack_pop(state);
        if (a == VTX_NODEID_INVALID || b == VTX_NODEID_INVALID) return -1;
        vtx_nodeid_t nid = vtx_record_emit_node(state, VTX_OP_Div);
        if (nid == VTX_NODEID_INVALID) return -1;
        vtx_node_add_input(&state->graph->node_table, nid, a);
        vtx_node_add_input(&state->graph->node_table, nid, b);
        vtx_node_t *node = vtx_node_get(&state->graph->node_table, nid);
        if (node) node->type = VTX_TYPE_Float;
        vtx_record_stack_push(state, nid);
        break;
    }

    /* ---- Bitwise operations ---- */
    case VT_OP_ISHL: {
        vtx_nodeid_t b = vtx_record_stack_pop(state);
        vtx_nodeid_t a = vtx_record_stack_pop(state);
        if (a == VTX_NODEID_INVALID || b == VTX_NODEID_INVALID) return -1;
        vtx_nodeid_t nid = vtx_record_emit_node(state, VTX_OP_Shl);
        if (nid == VTX_NODEID_INVALID) return -1;
        vtx_node_add_input(&state->graph->node_table, nid, a);
        vtx_node_add_input(&state->graph->node_table, nid, b);
        vtx_record_stack_push(state, nid);
        break;
    }
    case VT_OP_ISHR: {
        vtx_nodeid_t b = vtx_record_stack_pop(state);
        vtx_nodeid_t a = vtx_record_stack_pop(state);
        if (a == VTX_NODEID_INVALID || b == VTX_NODEID_INVALID) return -1;
        vtx_nodeid_t nid = vtx_record_emit_node(state, VTX_OP_Shr);
        if (nid == VTX_NODEID_INVALID) return -1;
        vtx_node_add_input(&state->graph->node_table, nid, a);
        vtx_node_add_input(&state->graph->node_table, nid, b);
        vtx_record_stack_push(state, nid);
        break;
    }
    case VT_OP_IAND: {
        vtx_nodeid_t b = vtx_record_stack_pop(state);
        vtx_nodeid_t a = vtx_record_stack_pop(state);
        if (a == VTX_NODEID_INVALID || b == VTX_NODEID_INVALID) return -1;
        vtx_nodeid_t nid = vtx_record_emit_node(state, VTX_OP_And);
        if (nid == VTX_NODEID_INVALID) return -1;
        vtx_node_add_input(&state->graph->node_table, nid, a);
        vtx_node_add_input(&state->graph->node_table, nid, b);
        vtx_record_stack_push(state, nid);
        break;
    }
    case VT_OP_IOR: {
        vtx_nodeid_t b = vtx_record_stack_pop(state);
        vtx_nodeid_t a = vtx_record_stack_pop(state);
        if (a == VTX_NODEID_INVALID || b == VTX_NODEID_INVALID) return -1;
        vtx_nodeid_t nid = vtx_record_emit_node(state, VTX_OP_Or);
        if (nid == VTX_NODEID_INVALID) return -1;
        vtx_node_add_input(&state->graph->node_table, nid, a);
        vtx_node_add_input(&state->graph->node_table, nid, b);
        vtx_record_stack_push(state, nid);
        break;
    }
    case VT_OP_IXOR: {
        vtx_nodeid_t b = vtx_record_stack_pop(state);
        vtx_nodeid_t a = vtx_record_stack_pop(state);
        if (a == VTX_NODEID_INVALID || b == VTX_NODEID_INVALID) return -1;
        vtx_nodeid_t nid = vtx_record_emit_node(state, VTX_OP_Xor);
        if (nid == VTX_NODEID_INVALID) return -1;
        vtx_node_add_input(&state->graph->node_table, nid, a);
        vtx_node_add_input(&state->graph->node_table, nid, b);
        vtx_record_stack_push(state, nid);
        break;
    }
    case VT_OP_INEG: {
        vtx_nodeid_t a = vtx_record_stack_pop(state);
        if (a == VTX_NODEID_INVALID) return -1;
        vtx_nodeid_t nid = vtx_record_emit_node(state, VTX_OP_Neg);
        if (nid == VTX_NODEID_INVALID) return -1;
        vtx_node_add_input(&state->graph->node_table, nid, a);
        vtx_record_stack_push(state, nid);
        break;
    }
    case VT_OP_INOT: {
        vtx_nodeid_t a = vtx_record_stack_pop(state);
        if (a == VTX_NODEID_INVALID) return -1;
        vtx_nodeid_t nid = vtx_record_emit_node(state, VTX_OP_Not);
        if (nid == VTX_NODEID_INVALID) return -1;
        vtx_node_add_input(&state->graph->node_table, nid, a);
        vtx_record_stack_push(state, nid);
        break;
    }

    /* ---- Integer comparisons ---- */
    case VT_OP_ICMP_EQ: case VT_OP_ICMP_NE:
    case VT_OP_ICMP_LT: case VT_OP_ICMP_LE:
    case VT_OP_ICMP_GT: case VT_OP_ICMP_GE: {
        vtx_nodeid_t b = vtx_record_stack_pop(state);
        vtx_nodeid_t a = vtx_record_stack_pop(state);
        if (a == VTX_NODEID_INVALID || b == VTX_NODEID_INVALID) return -1;

        vtx_nodeid_t cmp_nid = vtx_record_emit_node(state, VTX_OP_Cmp);
        if (cmp_nid == VTX_NODEID_INVALID) return -1;
        vtx_node_add_input(&state->graph->node_table, cmp_nid, a);
        vtx_node_add_input(&state->graph->node_table, cmp_nid, b);

        vtx_cond_t cond;
        switch (opcode) {
        case VT_OP_ICMP_EQ: cond = VTX_COND_EQ; break;
        case VT_OP_ICMP_NE: cond = VTX_COND_NE; break;
        case VT_OP_ICMP_LT: cond = VTX_COND_LT; break;
        case VT_OP_ICMP_LE: cond = VTX_COND_LE; break;
        case VT_OP_ICMP_GT: cond = VTX_COND_GT; break;
        case VT_OP_ICMP_GE: cond = VTX_COND_GE; break;
        default: cond = VTX_COND_NEVER; break;
        }

        vtx_node_t *cmp_node = vtx_node_get(&state->graph->node_table, cmp_nid);
        if (cmp_node) cmp_node->cond = cond;

        vtx_record_stack_push(state, cmp_nid);
        break;
    }

    /* ---- Float comparisons ---- */
    case VT_OP_FCMP_EQ: case VT_OP_FCMP_NE:
    case VT_OP_FCMP_LT: case VT_OP_FCMP_LE:
    case VT_OP_FCMP_GT: case VT_OP_FCMP_GE: {
        vtx_nodeid_t b = vtx_record_stack_pop(state);
        vtx_nodeid_t a = vtx_record_stack_pop(state);
        if (a == VTX_NODEID_INVALID || b == VTX_NODEID_INVALID) return -1;

        vtx_nodeid_t cmp_nid = vtx_record_emit_node(state, VTX_OP_CmpF);
        if (cmp_nid == VTX_NODEID_INVALID) return -1;
        vtx_node_add_input(&state->graph->node_table, cmp_nid, a);
        vtx_node_add_input(&state->graph->node_table, cmp_nid, b);

        vtx_cond_t cond;
        switch (opcode) {
        case VT_OP_FCMP_EQ: cond = VTX_COND_EQ; break;
        case VT_OP_FCMP_NE: cond = VTX_COND_NE; break;
        case VT_OP_FCMP_LT: cond = VTX_COND_LT; break;
        case VT_OP_FCMP_LE: cond = VTX_COND_LE; break;
        case VT_OP_FCMP_GT: cond = VTX_COND_GT; break;
        case VT_OP_FCMP_GE: cond = VTX_COND_GE; break;
        default: cond = VTX_COND_NEVER; break;
        }

        vtx_node_t *cmp_node = vtx_node_get(&state->graph->node_table, cmp_nid);
        if (cmp_node) cmp_node->cond = cond;

        vtx_record_stack_push(state, cmp_nid);
        break;
    }

    /* ---- Control flow ---- */
    case VT_OP_GOTO: {
        uint32_t target_pc = operand;

        /* Check for backward branch (loop back-edge).
         * A backward GOTO is a loop back-edge if the target PC is
         * between entry_pc (inclusive) and the current PC (exclusive).
         * The original code only checked target_pc <= entry_pc, which
         * missed backward branches to PCs between entry_pc and current_pc. */
        if (target_pc >= state->trace->entry_pc && target_pc < state->pc) {
            /* Loop back-edge within the trace — closed trace */
            state->trace->kind = VTX_TRACE_CLOSED;
            state->trace->exit_pc = state->pc;
            state->stopped = true;
            return 1;
        }

        /* Backward branch to before entry — also close */
        if (target_pc < state->trace->entry_pc) {
            state->trace->kind = VTX_TRACE_CLOSED;
            state->trace->exit_pc = state->pc;
            state->stopped = true;
            return 1;
        }

        /* Forward GOTO — just follow it */
        state->pc = target_pc;
        return 0; /* don't advance PC — already set */
    }

    case VT_OP_IF_TRUE: {
        /* Capture stack depth before consuming condition (Bug 1 fix) */
        uint32_t pre_sp = state->stack_top;
        vtx_nodeid_t cond = vtx_record_stack_pop(state);
        if (cond == VTX_NODEID_INVALID) return -1;

        uint32_t branch_target = operand; /* target if condition is true */
        uint32_t fall_through = (uint32_t)(state->pc + vtx_bytecode_insn_length(bc, state->pc));

        bool taken = vtx_record_branch_is_taken(state, state->pc);

        if (taken) {
            /* Hot path: branch is taken. Emit guard for the NOT-taken case. */
            vtx_nodeid_t guard_nid = vtx_record_emit_node(state, VTX_OP_Guard);
            if (guard_nid == VTX_NODEID_INVALID) return -1;
            vtx_node_t *guard = vtx_node_get(&state->graph->node_table, guard_nid);
            if (guard == NULL) return -1;
            /* Guard: condition must be true (if false, side exit) */
            guard->cond = VTX_COND_NE; /* guard fails if condition is falsy (==0) */
            vtx_node_add_input(&state->graph->node_table, guard_nid, cond);
            vtx_node_add_input(&state->graph->node_table, guard_nid, state->control);

            vtx_side_exit_t *exit = vtx_side_exit_create(
                state->exit_table, state->arena,
                fall_through, /* cold path starts at fall-through PC */
                state->stack, pre_sp,
                state->locals, state->max_locals,
                VTX_EXIT_BRANCH_NOT_TAKEN, guard_nid, state->trace->trace_id);
            if (exit != NULL) {
                vtx_record_add_side_exit(state, exit);
            }

            /* Update control chain (Bug 7 fix) */
            state->control = guard_nid;

            state->pc = branch_target;
            return 0; /* PC already set */
        } else {
            /* Hot path: fall through. Emit guard for the taken case. */
            vtx_nodeid_t guard_nid = vtx_record_emit_node(state, VTX_OP_Guard);
            if (guard_nid == VTX_NODEID_INVALID) return -1;
            vtx_node_t *guard = vtx_node_get(&state->graph->node_table, guard_nid);
            if (guard == NULL) return -1;
            /* Guard: condition must be false (if true, side exit) */
            guard->cond = VTX_COND_EQ; /* guard fails if condition is truthy (!=0) */
            vtx_node_add_input(&state->graph->node_table, guard_nid, cond);
            vtx_node_add_input(&state->graph->node_table, guard_nid, state->control);

            vtx_side_exit_t *exit = vtx_side_exit_create(
                state->exit_table, state->arena,
                branch_target, /* cold path starts at branch target PC */
                state->stack, pre_sp,
                state->locals, state->max_locals,
                VTX_EXIT_BRANCH_NOT_TAKEN, guard_nid, state->trace->trace_id);
            if (exit != NULL) {
                vtx_record_add_side_exit(state, exit);
            }

            /* Update control chain (Bug 7 fix) */
            state->control = guard_nid;

            /* Fall through — just advance PC normally */
            break;
        }
    }

    case VT_OP_IF_FALSE: {
        /* Capture stack depth before consuming condition (Bug 1 fix) */
        uint32_t pre_sp = state->stack_top;
        vtx_nodeid_t cond = vtx_record_stack_pop(state);
        if (cond == VTX_NODEID_INVALID) return -1;

        uint32_t branch_target = operand; /* target if condition is false */
        uint32_t fall_through = (uint32_t)(state->pc + vtx_bytecode_insn_length(bc, state->pc));

        bool taken = vtx_record_branch_is_taken(state, state->pc);

        if (taken) {
            /* Hot path: branch is taken (condition is false). */
            vtx_nodeid_t guard_nid = vtx_record_emit_node(state, VTX_OP_Guard);
            if (guard_nid == VTX_NODEID_INVALID) return -1;
            vtx_node_t *guard = vtx_node_get(&state->graph->node_table, guard_nid);
            if (guard == NULL) return -1;
            /* Guard: condition must be false */
            guard->cond = VTX_COND_EQ; /* guard fails if condition is truthy */
            vtx_node_add_input(&state->graph->node_table, guard_nid, cond);
            vtx_node_add_input(&state->graph->node_table, guard_nid, state->control);

            vtx_side_exit_t *exit = vtx_side_exit_create(
                state->exit_table, state->arena,
                fall_through,
                state->stack, pre_sp,
                state->locals, state->max_locals,
                VTX_EXIT_BRANCH_NOT_TAKEN, guard_nid, state->trace->trace_id);
            if (exit != NULL) {
                vtx_record_add_side_exit(state, exit);
            }

            /* Update control chain (Bug 7 fix) */
            state->control = guard_nid;

            state->pc = branch_target;
            return 0;
        } else {
            /* Hot path: fall through (condition is true). */
            vtx_nodeid_t guard_nid = vtx_record_emit_node(state, VTX_OP_Guard);
            if (guard_nid == VTX_NODEID_INVALID) return -1;
            vtx_node_t *guard = vtx_node_get(&state->graph->node_table, guard_nid);
            if (guard == NULL) return -1;
            /* Guard: condition must be true */
            guard->cond = VTX_COND_NE; /* guard fails if condition is falsy */
            vtx_node_add_input(&state->graph->node_table, guard_nid, cond);
            vtx_node_add_input(&state->graph->node_table, guard_nid, state->control);

            vtx_side_exit_t *exit = vtx_side_exit_create(
                state->exit_table, state->arena,
                branch_target,
                state->stack, pre_sp,
                state->locals, state->max_locals,
                VTX_EXIT_BRANCH_NOT_TAKEN, guard_nid, state->trace->trace_id);
            if (exit != NULL) {
                vtx_record_add_side_exit(state, exit);
            }

            /* Update control chain (Bug 7 fix) */
            state->control = guard_nid;

            break;
        }
    }

    /* ---- Calls ---- */
    case VT_OP_CALL_STATIC: {
        /* Capture stack depth for side exits (Bug 1 fix) */
        uint32_t pre_sp = state->stack_top;

        /* Bug 6 fix: Determine argument count and pop arguments from stack */
        uint32_t arg_count = 0;
        if (state->profiler != NULL && state->method != NULL) {
            const vtx_call_site_profile_t *csp = vtx_profiler_get_call_site_profile(
                state->profiler, state->method, state->pc);
            if (csp != NULL && csp->count > 0 && csp->entries[0].method != NULL) {
                arg_count = vtx_count_method_args_from_sig(csp->entries[0].method->signature);
            }
        }

        /* Pop arguments from stack (in reverse order) and connect to Call node */
        vtx_nodeid_t args[16]; /* max 16 args */
        if (arg_count > 16) arg_count = 16;
        for (uint32_t i = 0; i < arg_count; i++) {
            vtx_nodeid_t arg = vtx_record_stack_pop(state);
            if (arg == VTX_NODEID_INVALID) return -1;
            args[arg_count - 1 - i] = arg; /* reverse to get correct order */
        }

        vtx_nodeid_t nid = vtx_record_emit_node(state, VTX_OP_CallStatic);
        if (nid == VTX_NODEID_INVALID) return -1;
        vtx_node_t *node = vtx_node_get(&state->graph->node_table, nid);
        if (node) {
            node->method_index = operand;
            node->type = VTX_TYPE_Bottom;
            node->bytecode_pc = state->pc;
        }
        vtx_node_add_input(&state->graph->node_table, nid, state->control);
        vtx_node_add_input(&state->graph->node_table, nid, state->memory);

        /* Bug 6 fix: Add arguments as inputs to the Call node */
        for (uint32_t i = 0; i < arg_count; i++) {
            vtx_node_add_input(&state->graph->node_table, nid, args[i]);
        }

        /* Emit FrameState for deopt at call */
        vtx_nodeid_t fs_nid = vtx_record_emit_node(state, VTX_OP_FrameState);
        if (fs_nid != VTX_NODEID_INVALID) {
            vtx_node_t *fs = vtx_node_get(&state->graph->node_table, fs_nid);
            if (fs) fs->bytecode_pc = state->pc;
            if (node) node->frame_state = fs_nid;
        }

        state->memory = nid;
        state->control = nid;
        vtx_record_stack_push(state, nid);
        break;
    }
    case VT_OP_CALL_VIRTUAL: {
        /* Capture stack depth for side exits (Bug 1 fix) */
        uint32_t pre_sp = state->stack_top;

        /* Bug 6 fix: Determine argument count (including receiver) and pop from stack */
        uint32_t arg_count = 1; /* at minimum the receiver */
        const vtx_call_site_profile_t *csp = NULL;
        if (state->profiler != NULL && state->method != NULL) {
            csp = vtx_profiler_get_call_site_profile(
                state->profiler, state->method, state->pc);
            if (csp != NULL && csp->count > 0 && csp->entries[0].method != NULL) {
                arg_count = vtx_count_method_args_from_sig(csp->entries[0].method->signature) + 1;
            }
        }

        /* Pop arguments from stack (in reverse order: last arg first) */
        vtx_nodeid_t args[16]; /* max 16 args including receiver */
        if (arg_count > 16) arg_count = 16;
        for (uint32_t i = 0; i < arg_count; i++) {
            vtx_nodeid_t arg = vtx_record_stack_pop(state);
            if (arg == VTX_NODEID_INVALID) return -1;
            args[arg_count - 1 - i] = arg; /* reverse: args[0]=receiver, args[1..]=arguments */
        }

        /* Bug 2 fix: Emit type-check guard BEFORE the call, with proper connections */
        if (csp != NULL && csp->count > 0) {
            /* Monomorphic guard on the first observed type */
            vtx_typeid_t expected_type = csp->entries[0].typeid_;
            vtx_nodeid_t guard_nid = vtx_record_emit_node(state, VTX_OP_Guard);
            if (guard_nid != VTX_NODEID_INVALID) {
                vtx_node_t *guard = vtx_node_get(&state->graph->node_table, guard_nid);
                if (guard) {
                    guard->cond = VTX_COND_EQ;
                    guard->type_id = expected_type;
                }
                /* Bug 2 fix: Connect receiver as input to the guard */
                vtx_node_add_input(&state->graph->node_table, guard_nid, args[0]);
                /* Bug 2 fix: Connect guard to control flow */
                vtx_node_add_input(&state->graph->node_table, guard_nid, state->control);

                /* Bug 2 fix: Create side exit for type check failure */
                vtx_side_exit_t *exit = vtx_side_exit_create(
                    state->exit_table, state->arena,
                    state->pc,
                    state->stack, pre_sp,
                    state->locals, state->max_locals,
                    VTX_EXIT_TYPE_CHECK_FAILED, guard_nid, state->trace->trace_id);
                if (exit != NULL) {
                    vtx_record_add_side_exit(state, exit);
                }

                /* Bug 7 fix: Update control chain */
                state->control = guard_nid;
            }
        }

        vtx_nodeid_t nid = vtx_record_emit_node(state, VTX_OP_CallVirtual);
        if (nid == VTX_NODEID_INVALID) return -1;
        vtx_node_t *node = vtx_node_get(&state->graph->node_table, nid);
        if (node) {
            node->method_index = operand;
            node->type = VTX_TYPE_Bottom;
            node->bytecode_pc = state->pc;
        }
        vtx_node_add_input(&state->graph->node_table, nid, state->control);
        vtx_node_add_input(&state->graph->node_table, nid, state->memory);

        /* Bug 6 fix: Add receiver and arguments as inputs to the Call node */
        for (uint32_t i = 0; i < arg_count; i++) {
            vtx_node_add_input(&state->graph->node_table, nid, args[i]);
        }

        vtx_nodeid_t fs_nid = vtx_record_emit_node(state, VTX_OP_FrameState);
        if (fs_nid != VTX_NODEID_INVALID) {
            vtx_node_t *fs = vtx_node_get(&state->graph->node_table, fs_nid);
            if (fs) fs->bytecode_pc = state->pc;
            if (node) node->frame_state = fs_nid;
        }

        state->memory = nid;
        state->control = nid;
        vtx_record_stack_push(state, nid);
        break;
    }
    case VT_OP_CALL_INTERFACE: {
        /* Capture stack depth for side exits (Bug 1 fix) */
        uint32_t pre_sp = state->stack_top;

        /* Bug 6 fix: Determine argument count (including receiver) and pop from stack */
        uint32_t arg_count = 1; /* at minimum the receiver */
        if (state->profiler != NULL && state->method != NULL) {
            const vtx_call_site_profile_t *csp = vtx_profiler_get_call_site_profile(
                state->profiler, state->method, state->pc);
            if (csp != NULL && csp->count > 0 && csp->entries[0].method != NULL) {
                arg_count = vtx_count_method_args_from_sig(csp->entries[0].method->signature) + 1;
            }
        }

        /* Pop arguments from stack (in reverse order: last arg first) */
        vtx_nodeid_t args[16]; /* max 16 args including receiver */
        if (arg_count > 16) arg_count = 16;
        for (uint32_t i = 0; i < arg_count; i++) {
            vtx_nodeid_t arg = vtx_record_stack_pop(state);
            if (arg == VTX_NODEID_INVALID) return -1;
            args[arg_count - 1 - i] = arg; /* reverse: args[0]=receiver, args[1..]=arguments */
        }

        vtx_nodeid_t nid = vtx_record_emit_node(state, VTX_OP_CallInterface);
        if (nid == VTX_NODEID_INVALID) return -1;
        vtx_node_t *node = vtx_node_get(&state->graph->node_table, nid);
        if (node) {
            node->method_index = operand;
            node->type = VTX_TYPE_Bottom;
            node->bytecode_pc = state->pc;
        }
        vtx_node_add_input(&state->graph->node_table, nid, state->control);
        vtx_node_add_input(&state->graph->node_table, nid, state->memory);

        /* Bug 6 fix: Add receiver and arguments as inputs to the Call node */
        for (uint32_t i = 0; i < arg_count; i++) {
            vtx_node_add_input(&state->graph->node_table, nid, args[i]);
        }

        vtx_nodeid_t fs_nid = vtx_record_emit_node(state, VTX_OP_FrameState);
        if (fs_nid != VTX_NODEID_INVALID) {
            vtx_node_t *fs = vtx_node_get(&state->graph->node_table, fs_nid);
            if (fs) fs->bytecode_pc = state->pc;
            if (node) node->frame_state = fs_nid;
        }

        state->memory = nid;
        state->control = nid;
        vtx_record_stack_push(state, nid);
        break;
    }

    /* ---- Returns ---- */
    case VT_OP_RETURN: {
        state->trace->kind = VTX_TRACE_OPEN;
        state->trace->exit_pc = state->pc;

        vtx_nodeid_t ret_nid = vtx_record_emit_node(state, VTX_OP_Return);
        if (ret_nid == VTX_NODEID_INVALID) return -1;
        vtx_node_add_input(&state->graph->node_table, ret_nid, state->control);

        state->stopped = true;
        return 1;
    }
    case VT_OP_RETURN_VALUE: {
        vtx_nodeid_t val = vtx_record_stack_pop(state);
        if (val == VTX_NODEID_INVALID) return -1;

        state->trace->kind = VTX_TRACE_OPEN;
        state->trace->exit_pc = state->pc;

        vtx_nodeid_t ret_nid = vtx_record_emit_node(state, VTX_OP_Return);
        if (ret_nid == VTX_NODEID_INVALID) return -1;
        vtx_node_add_input(&state->graph->node_table, ret_nid, state->control);
        vtx_node_add_input(&state->graph->node_table, ret_nid, val);

        state->stopped = true;
        return 1;
    }

    /* ---- Object creation ---- */
    case VT_OP_NEW: {
        vtx_nodeid_t nid = vtx_record_emit_node(state, VTX_OP_NewObject);
        if (nid == VTX_NODEID_INVALID) return -1;
        vtx_node_t *node = vtx_node_get(&state->graph->node_table, nid);
        if (node) {
            node->type_id = operand;
            node->type = VTX_TYPE_Ptr;
        }
        vtx_node_add_input(&state->graph->node_table, nid, state->control);
        vtx_node_add_input(&state->graph->node_table, nid, state->memory);

        state->memory = nid;
        vtx_record_stack_push(state, nid);
        break;
    }
    case VT_OP_NEWARRAY: {
        vtx_nodeid_t count = vtx_record_stack_pop(state);
        if (count == VTX_NODEID_INVALID) return -1;

        vtx_nodeid_t nid = vtx_record_emit_node(state, VTX_OP_NewArray);
        if (nid == VTX_NODEID_INVALID) return -1;
        vtx_node_t *node = vtx_node_get(&state->graph->node_table, nid);
        if (node) {
            node->type_id = operand;
            node->type = VTX_TYPE_Ptr;
        }
        vtx_node_add_input(&state->graph->node_table, nid, state->control);
        vtx_node_add_input(&state->graph->node_table, nid, count);
        vtx_node_add_input(&state->graph->node_table, nid, state->memory);

        state->memory = nid;
        vtx_record_stack_push(state, nid);
        break;
    }

    /* ---- Type checks ---- */
    case VT_OP_CHECKCAST: {
        /* Capture stack depth before consuming inputs (Bug 1 fix) */
        uint32_t pre_sp = state->stack_top;
        vtx_nodeid_t obj = vtx_record_stack_pop(state);
        if (obj == VTX_NODEID_INVALID) return -1;

        vtx_nodeid_t nid = vtx_record_emit_node(state, VTX_OP_CheckCast);
        if (nid == VTX_NODEID_INVALID) return -1;
        vtx_node_t *node = vtx_node_get(&state->graph->node_table, nid);
        if (node) {
            node->type_id = operand;
            node->type = VTX_TYPE_Ptr;
        }
        vtx_node_add_input(&state->graph->node_table, nid, obj);
        vtx_node_add_input(&state->graph->node_table, nid, state->control);

        /* Emit guard for checkcast failure */
        vtx_nodeid_t guard_nid = vtx_record_emit_node(state, VTX_OP_Guard);
        if (guard_nid != VTX_NODEID_INVALID) {
            vtx_node_t *guard = vtx_node_get(&state->graph->node_table, guard_nid);
            if (guard) {
                guard->cond = VTX_COND_EQ;
                guard->type_id = operand;
            }
            vtx_node_add_input(&state->graph->node_table, guard_nid, obj);
            vtx_node_add_input(&state->graph->node_table, guard_nid, state->control);

            vtx_side_exit_t *exit = vtx_side_exit_create(
                state->exit_table, state->arena,
                state->pc,
                state->stack, pre_sp,
                state->locals, state->max_locals,
                VTX_EXIT_TYPE_CHECK_FAILED, guard_nid, state->trace->trace_id);
            if (exit != NULL) vtx_record_add_side_exit(state, exit);

            /* Bug 7 fix: Update control chain */
            state->control = guard_nid;
        }

        vtx_record_stack_push(state, nid);
        break;
    }
    case VT_OP_INSTANCEOF: {
        vtx_nodeid_t obj = vtx_record_stack_pop(state);
        if (obj == VTX_NODEID_INVALID) return -1;

        vtx_nodeid_t nid = vtx_record_emit_node(state, VTX_OP_InstanceOf);
        if (nid == VTX_NODEID_INVALID) return -1;
        vtx_node_t *node = vtx_node_get(&state->graph->node_table, nid);
        if (node) {
            node->type_id = operand;
            node->type = VTX_TYPE_Int;
        }
        vtx_node_add_input(&state->graph->node_table, nid, obj);
        vtx_record_stack_push(state, nid);
        break;
    }

    /* ---- Array operations ---- */
    case VT_OP_ARRAY_LOAD: {
        /* Capture stack depth before consuming inputs (Bug 1 fix) */
        uint32_t pre_sp = state->stack_top;
        vtx_nodeid_t idx = vtx_record_stack_pop(state);
        vtx_nodeid_t arr = vtx_record_stack_pop(state);
        if (arr == VTX_NODEID_INVALID || idx == VTX_NODEID_INVALID) return -1;

        /* Bounds check guard */
        vtx_nodeid_t guard_nid = vtx_record_emit_node(state, VTX_OP_Guard);
        if (guard_nid == VTX_NODEID_INVALID) return -1;
        vtx_node_t *guard = vtx_node_get(&state->graph->node_table, guard_nid);
        if (guard) guard->cond = VTX_COND_ULT;
        vtx_node_add_input(&state->graph->node_table, guard_nid, idx);
        vtx_node_add_input(&state->graph->node_table, guard_nid, state->control);

        vtx_side_exit_t *exit = vtx_side_exit_create(
            state->exit_table, state->arena,
            state->pc,
            state->stack, pre_sp,
            state->locals, state->max_locals,
            VTX_EXIT_BOUNDS_CHECK_FAILED, guard_nid, state->trace->trace_id);
        if (exit != NULL) vtx_record_add_side_exit(state, exit);

        /* Bug 7 fix: Update control chain */
        state->control = guard_nid;

        vtx_nodeid_t nid = vtx_record_emit_node(state, VTX_OP_LoadIndexed);
        if (nid == VTX_NODEID_INVALID) return -1;
        vtx_node_add_input(&state->graph->node_table, nid, arr);
        vtx_node_add_input(&state->graph->node_table, nid, idx);
        vtx_node_add_input(&state->graph->node_table, nid, state->memory);

        state->memory = nid;
        vtx_record_stack_push(state, nid);
        break;
    }
    case VT_OP_ARRAY_STORE: {
        /* Capture stack depth before consuming inputs (Bug 1 fix) */
        uint32_t pre_sp = state->stack_top;
        vtx_nodeid_t val = vtx_record_stack_pop(state);
        vtx_nodeid_t idx = vtx_record_stack_pop(state);
        vtx_nodeid_t arr = vtx_record_stack_pop(state);
        if (arr == VTX_NODEID_INVALID || idx == VTX_NODEID_INVALID ||
            val == VTX_NODEID_INVALID) return -1;

        /* Bounds check guard */
        vtx_nodeid_t guard_nid = vtx_record_emit_node(state, VTX_OP_Guard);
        if (guard_nid == VTX_NODEID_INVALID) return -1;
        vtx_node_t *guard = vtx_node_get(&state->graph->node_table, guard_nid);
        if (guard) guard->cond = VTX_COND_ULT;
        vtx_node_add_input(&state->graph->node_table, guard_nid, idx);
        vtx_node_add_input(&state->graph->node_table, guard_nid, state->control);

        vtx_side_exit_t *exit = vtx_side_exit_create(
            state->exit_table, state->arena,
            state->pc,
            state->stack, pre_sp,
            state->locals, state->max_locals,
            VTX_EXIT_BOUNDS_CHECK_FAILED, guard_nid, state->trace->trace_id);
        if (exit != NULL) vtx_record_add_side_exit(state, exit);

        /* Bug 7 fix: Update control chain */
        state->control = guard_nid;

        vtx_nodeid_t nid = vtx_record_emit_node(state, VTX_OP_StoreIndexed);
        if (nid == VTX_NODEID_INVALID) return -1;
        vtx_node_add_input(&state->graph->node_table, nid, arr);
        vtx_node_add_input(&state->graph->node_table, nid, idx);
        vtx_node_add_input(&state->graph->node_table, nid, val);
        vtx_node_add_input(&state->graph->node_table, nid, state->memory);

        state->memory = nid;
        break;
    }
    case VT_OP_ARRAY_LENGTH: {
        /* Capture stack depth before consuming inputs (Bug 1 fix) */
        uint32_t pre_sp = state->stack_top;
        vtx_nodeid_t arr = vtx_record_stack_pop(state);
        if (arr == VTX_NODEID_INVALID) return -1;

        /* Null check */
        vtx_nodeid_t guard_nid = vtx_record_emit_node(state, VTX_OP_Guard);
        if (guard_nid == VTX_NODEID_INVALID) return -1;
        vtx_node_t *guard = vtx_node_get(&state->graph->node_table, guard_nid);
        if (guard) guard->cond = VTX_COND_NE;
        vtx_node_add_input(&state->graph->node_table, guard_nid, arr);
        vtx_node_add_input(&state->graph->node_table, guard_nid, state->control);

        /* Bug 3 fix: Add side exit for null check failure */
        vtx_side_exit_t *exit = vtx_side_exit_create(
            state->exit_table, state->arena,
            state->pc,
            state->stack, pre_sp,
            state->locals, state->max_locals,
            VTX_EXIT_NULL_CHECK_FAILED, guard_nid, state->trace->trace_id);
        if (exit != NULL) {
            vtx_record_add_side_exit(state, exit);
        }

        /* Bug 7 fix: Update control chain */
        state->control = guard_nid;

        /* Array length is a field load at a well-known offset */
        vtx_nodeid_t nid = vtx_record_emit_node(state, VTX_OP_LoadField);
        if (nid == VTX_NODEID_INVALID) return -1;
        vtx_node_t *node = vtx_node_get(&state->graph->node_table, nid);
        if (node) {
            node->field_offset = 0; /* length is at offset 0 in array header */
            node->type = VTX_TYPE_Int;
        }
        vtx_node_add_input(&state->graph->node_table, nid, arr);
        vtx_record_stack_push(state, nid);
        break;
    }

    /* ---- Stack manipulation ---- */
    case VT_OP_DUP: {
        vtx_nodeid_t val = vtx_record_stack_peek(state, 0);
        if (val == VTX_NODEID_INVALID) return -1;
        vtx_record_stack_push(state, val);
        break;
    }
    case VT_OP_POP: {
        vtx_record_stack_pop(state);
        break;
    }
    case VT_OP_SWAP: {
        vtx_nodeid_t a = vtx_record_stack_pop(state);
        vtx_nodeid_t b = vtx_record_stack_pop(state);
        if (a == VTX_NODEID_INVALID || b == VTX_NODEID_INVALID) return -1;
        vtx_record_stack_push(state, a);
        vtx_record_stack_push(state, b);
        break;
    }

    /* ---- NOP / HALT ---- */
    case VT_OP_NOP:
        break;

    case VT_OP_HALT:
        state->trace->kind = VTX_TRACE_TRUNCATED;
        state->trace->exit_pc = state->pc;
        state->stopped = true;
        return 1;

    /* ---- Unsupported opcodes — truncate trace ---- */
    case VT_OP_THROW:
    case VT_OP_CATCH:
    case VT_OP_MONITOR_ENTER:
    case VT_OP_MONITOR_EXIT:
    case VT_OP_ISNULL:
    case VT_OP_TYPEOF:
    case VT_OP_CALL_RUNTIME:
        /* These opcodes are not supported in trace recording.
         * The trace is truncated at this point. */
        state->trace->kind = VTX_TRACE_TRUNCATED;
        state->trace->exit_pc = state->pc;
        state->stopped = true;
        return 1;

    default:
        /* Unknown opcode — stop recording */
        state->trace->kind = VTX_TRACE_TRUNCATED;
        state->trace->exit_pc = state->pc;
        state->stopped = true;
        return 1;
    }

    /* Advance to the next instruction */
    state->pc = (uint32_t)(state->pc + vtx_bytecode_insn_length(bc, state->pc));
    return 0;
}

/* ========================================================================== */
/* Recorder lifecycle                                                          */
/* ========================================================================== */

int vtx_trace_recorder_init(vtx_trace_recorder_t *recorder)
{
    VTX_ASSERT(recorder != NULL, "recorder must not be NULL");
    recorder->next_trace_id = 0;
    recorder->max_trace_length = VTX_MAX_TRACE_LENGTH;
    return 0;
}

void vtx_trace_recorder_destroy(vtx_trace_recorder_t *recorder)
{
    /* No dynamically allocated memory in the recorder itself */
    (void)recorder;
}

/* ========================================================================== */
/* Main trace recording entry point                                            */
/* ========================================================================== */

vtx_trace_t *vtx_trace_recorder_record(
    vtx_trace_recorder_t *recorder,
    vtx_graph_t *graph,
    const vtx_bytecode_t *bytecode,
    const vtx_method_desc_t *method,
    uint32_t entry_pc,
    const vtx_profiler_t *profiler,
    const vtx_profile_global_t *profile,
    vtx_arena_t *arena)
{
    VTX_ASSERT(recorder != NULL, "recorder must not be NULL");
    VTX_ASSERT(graph != NULL, "graph must not be NULL");
    VTX_ASSERT(bytecode != NULL, "bytecode must not be NULL");
    VTX_ASSERT(method != NULL, "method must not be NULL");
    VTX_ASSERT(arena != NULL, "arena must not be NULL");

    /* Allocate the trace from the arena */
    vtx_trace_t *trace = vtx_arena_alloc(arena, sizeof(vtx_trace_t));
    if (trace == NULL) return NULL;
    memset(trace, 0, sizeof(vtx_trace_t));

    /* Initialize trace fields */
    trace->trace_id = recorder->next_trace_id++;
    trace->kind = VTX_TRACE_OPEN; /* will be updated during recording */
    trace->entry_pc = entry_pc;
    trace->exit_pc = entry_pc;
    trace->method_id = 0; /* will be set if profile data is available */
    trace->bytecode_length = 0;

    /* Allocate the node sequence */
    trace->node_capacity = 64;
    trace->nodes = vtx_arena_alloc(arena, sizeof(vtx_nodeid_t) * trace->node_capacity);
    if (trace->nodes == NULL) return NULL;
    trace->node_count = 0;

    /* Allocate the side exit array */
    trace->side_exit_capacity = 8;
    trace->side_exits = vtx_arena_alloc(arena,
                                         sizeof(vtx_side_exit_t *) * trace->side_exit_capacity);
    if (trace->side_exits == NULL) return NULL;
    trace->side_exit_count = 0;

    /* Allocate local variable map */
    trace->local_count = bytecode->max_locals;
    if (bytecode->max_locals > 0) {
        trace->locals = vtx_arena_alloc(arena,
                                         sizeof(vtx_nodeid_t) * bytecode->max_locals);
        if (trace->locals == NULL) return NULL;
        memset(trace->locals, 0xFF, sizeof(vtx_nodeid_t) * bytecode->max_locals);
        /* 0xFF... = VTX_NODEID_INVALID — all locals start unassigned */
    }

    /* Create a side exit table for this trace */
    vtx_side_exit_table_t exit_table;
    if (vtx_side_exit_table_init(&exit_table) != 0) return NULL;

    /* Emit LoopBegin node for the trace entry */
    vtx_nodeid_t loop_begin = vtx_node_create(&graph->node_table, VTX_OP_LoopBegin);
    if (loop_begin == VTX_NODEID_INVALID) {
        vtx_side_exit_table_destroy(&exit_table);
        return NULL;
    }
    trace->start_node = loop_begin;
    trace->nodes[trace->node_count++] = loop_begin;

    /* Set up the initial Province (memory state) */
    vtx_nodeid_t province = vtx_node_create(&graph->node_table, VTX_OP_Province);
    if (province == VTX_NODEID_INVALID) {
        vtx_side_exit_table_destroy(&exit_table);
        return NULL;
    }

    /* Initialize recording state */
    vtx_record_state_t state;
    memset(&state, 0, sizeof(state));
    state.trace = trace;
    state.graph = graph;
    state.bytecode = bytecode;
    state.method = method;
    state.profiler = profiler;
    state.profile = profile;
    state.arena = arena;
    state.exit_table = &exit_table;
    state.max_trace_length = recorder->max_trace_length;
    state.control = loop_begin;
    state.memory = province;
    state.pc = entry_pc;
    state.stopped = false;
    state.stack_overflow = false;
    state.max_locals = bytecode->max_locals;
    state.locals = trace->locals;

    /* Initialize simulated operand stack */
    if (vtx_record_stack_init(&state, bytecode->max_stack) != 0) {
        vtx_side_exit_table_destroy(&exit_table);
        return NULL;
    }

    /* Main recording loop */
    while (!state.stopped && state.pc < bytecode->length) {
        int result = vtx_record_instruction(&state);
        if (result < 0 || state.stack_overflow) {
            /* Error during recording — return what we have so far */
            trace->kind = VTX_TRACE_TRUNCATED;
            break;
        }
        if (result == 1) {
            /* Recording stopped normally (closed trace, return, etc.) */
            break;
        }
        /* result == 0: continue recording */
    }

    /* If we reached the end of bytecode without closing, mark truncated */
    if (!state.stopped && state.pc >= bytecode->length) {
        trace->kind = VTX_TRACE_TRUNCATED;
        trace->exit_pc = state.pc;
    }

    /* Store final control and memory positions */
    trace->control_node = state.control;
    trace->memory_node = state.memory;

    /* Clean up */
    vtx_side_exit_table_destroy(&exit_table);

    return trace;
}

/* ========================================================================== */
/* Trace access                                                                */
/* ========================================================================== */

uint32_t vtx_trace_node_count(const vtx_trace_t *trace)
{
    return trace != NULL ? trace->node_count : 0;
}

uint32_t vtx_trace_side_exit_count(const vtx_trace_t *trace)
{
    return trace != NULL ? trace->side_exit_count : 0;
}

bool vtx_trace_is_closed(const vtx_trace_t *trace)
{
    return trace != NULL && trace->kind == VTX_TRACE_CLOSED;
}

vtx_nodeid_t vtx_trace_get_node(const vtx_trace_t *trace, uint32_t index)
{
    if (trace == NULL || index >= trace->node_count) return VTX_NODEID_INVALID;
    return trace->nodes[index];
}

vtx_side_exit_t *vtx_trace_get_side_exit(const vtx_trace_t *trace, uint32_t index)
{
    if (trace == NULL || index >= trace->side_exit_count) return NULL;
    return trace->side_exits[index];
}

/* ========================================================================== */
/* Trace list operations                                                       */
/* ========================================================================== */

int vtx_trace_list_init(vtx_trace_list_t *list)
{
    list->capacity = VTX_TRACE_LIST_INITIAL_CAPACITY;
    list->traces = malloc(sizeof(vtx_trace_t *) * list->capacity);
    if (list->traces == NULL) return -1;
    list->count = 0;
    return 0;
}

void vtx_trace_list_destroy(vtx_trace_list_t *list)
{
    if (list == NULL) return;
    free(list->traces);
    list->traces = NULL;
    list->count = 0;
    list->capacity = 0;
}

int vtx_trace_list_append(vtx_trace_list_t *list, vtx_trace_t *trace)
{
    if (list->count >= list->capacity) {
        uint32_t new_cap = list->capacity * 2;
        vtx_trace_t **new_traces = realloc(list->traces,
                                            sizeof(vtx_trace_t *) * new_cap);
        if (new_traces == NULL) return -1;
        list->traces = new_traces;
        list->capacity = new_cap;
    }
    list->traces[list->count++] = trace;
    return 0;
}
