#include "ir/graph.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ========================================================================== */
/* Internal helper: emit a FrameState node                                     */
/* ========================================================================== */

/**
 * Create a FrameState node that captures the current execution state for
 * deoptimization. The FrameState records the current control, memory, and
 * local variable values so the interpreter can reconstruct the execution
 * state if a guard fails.
 *
 * Inputs: [control, memory, local0, local1, ..., localN]
 * The FrameState also records the bytecode PC for deopt stack reconstruction.
 *
 * Returns the FrameState node ID, or VTX_NODEID_INVALID on failure.
 */
static vtx_nodeid_t emit_frame_state(vtx_graph_t *graph,
                                      vtx_block_info_t *block,
                                      uint16_t max_locals,
                                      size_t bytecode_pc)
{
    vtx_node_table_t *nt = &graph->node_table;

    vtx_nodeid_t fs = vtx_node_create(nt, VTX_OP_FrameState);
    if (fs == VTX_NODEID_INVALID) return VTX_NODEID_INVALID;

    vtx_node_t *fs_node = vtx_node_get(nt, fs);
    fs_node->bytecode_pc = (uint32_t)bytecode_pc;

    /* Input 0: current control */
    if (block->control_node != VTX_NODEID_INVALID) {
        vtx_node_add_input(nt, fs, block->control_node);
    }

    /* Input 1: current memory state */
    if (block->memory_node != VTX_NODEID_INVALID) {
        vtx_node_add_input(nt, fs, block->memory_node);
    }

    /* Inputs 2..2+max_locals: current local variable values */
    if (block->locals != NULL) {
        for (uint16_t i = 0; i < max_locals; i++) {
            if (block->locals[i] != VTX_NODEID_INVALID) {
                vtx_node_add_input(nt, fs, block->locals[i]);
            } else {
                /* Use a void constant as placeholder for undefined locals */
                vtx_nodeid_t undef = vtx_node_create(nt, VTX_OP_Constant);
                if (undef == VTX_NODEID_INVALID) return VTX_NODEID_INVALID;
                vtx_node_t *n = vtx_node_get(nt, undef);
                n->constval = vtx_constval_void();
                n->type = VTX_TYPE_Void;
                vtx_node_add_input(nt, fs, undef);
                block->locals[i] = undef;
            }
        }
    }

    return fs;
}

/**
 * Emit a Guard node that checks a condition and deoptimizes if the
 * condition is false. The Guard captures a FrameState for deopt.
 *
 * The Guard node:
 *   - Input 0: current control
 *   - Input 1: condition (data node that evaluates to true/false)
 *   - Input 2: FrameState (for deopt reconstruction)
 *   - cond: the condition code (e.g., VTX_COND_NE for "not null")
 *
 * On guard failure (condition is false), execution deoptimizes using
 * the captured FrameState. On success, the Guard passes control through.
 *
 * Returns the Guard node ID, or VTX_NODEID_INVALID on failure.
 */
static vtx_nodeid_t emit_guard(vtx_graph_t *graph,
                                vtx_block_info_t *block,
                                vtx_nodeid_t condition,
                                vtx_cond_t guard_cond,
                                uint16_t max_locals,
                                size_t bytecode_pc)
{
    vtx_node_table_t *nt = &graph->node_table;

    /* Create FrameState before the Guard */
    vtx_nodeid_t fs = emit_frame_state(graph, block, max_locals, bytecode_pc);
    if (fs == VTX_NODEID_INVALID) return VTX_NODEID_INVALID;

    /* Create the Guard node */
    vtx_nodeid_t guard = vtx_node_create(nt, VTX_OP_Guard);
    if (guard == VTX_NODEID_INVALID) return VTX_NODEID_INVALID;

    vtx_node_t *g = vtx_node_get(nt, guard);
    g->cond = guard_cond;
    g->bytecode_pc = (uint32_t)bytecode_pc;
    g->frame_state = fs;

    /* Input 0: control */
    vtx_node_add_input(nt, guard, block->control_node);
    /* Input 1: condition */
    vtx_node_add_input(nt, guard, condition);
    /* Input 2: FrameState */
    vtx_node_add_input(nt, guard, fs);

    /* Guard becomes the new control output */
    block->control_node = guard;

    return guard;
}

/* ========================================================================== */
/* Internal helper: emit an exception edge from a throwing node                 */
/* ========================================================================== */

/**
 * If the current block has an exception_target (a Catch handler), create an
 * ExceptProj node from the throwing node that connects to the catch handler.
 *
 * The ExceptProj is a control projection that represents the exceptional
 * control flow path. Its input is the throwing node, and it feeds into
 * the Catch node's Region (or the Catch node itself as an input).
 *
 * Returns 0 on success, -1 on failure.
 */
static int emit_exception_edge(vtx_graph_t *graph,
                                vtx_block_info_t *block,
                                vtx_nodeid_t throwing_node)
{
    if (block->exception_target == VTX_NODEID_INVALID) return 0; /* no handler */

    vtx_node_table_t *nt = &graph->node_table;

    /* Create an ExceptProj node that projects the exceptional exit
     * from the throwing node. */
    vtx_nodeid_t exc_proj = vtx_node_create(nt, VTX_OP_ExceptProj);
    if (exc_proj == VTX_NODEID_INVALID) return -1;

    /* The ExceptProj takes the throwing node as input */
    vtx_node_add_input(nt, exc_proj, throwing_node);

    /* Connect the ExceptProj to the Catch node (exception target).
     * The Catch node already has its normal control input; we add
     * the exception edge as an additional input so the Catch can
     * receive exceptions from multiple throwing sites. */
    vtx_node_add_input(nt, block->exception_target, exc_proj);

    return 0;
}

/* ========================================================================== */
/* Internal helper: count method arguments from signature                       */
/* ========================================================================== */

/**
 * Parse a JVM-style method signature to count the number of arguments.
 * IR-5 fix: needed by CALL_STATIC to consume args from operand stack.
 */
static uint32_t vtx_graph_count_method_args(const char *sig)
{
    if (sig == NULL || sig[0] != '(') return 0;
    uint32_t count = 0;
    uint32_t i = 1;
    while (sig[i] != '\0' && sig[i] != ')') {
        if (sig[i] == 'B' || sig[i] == 'C' || sig[i] == 'D' ||
            sig[i] == 'F' || sig[i] == 'I' || sig[i] == 'J' ||
            sig[i] == 'S' || sig[i] == 'Z') {
            count++; i++;
        } else if (sig[i] == 'L') {
            count++;
            while (sig[i] != '\0' && sig[i] != ';') i++;
            if (sig[i] == ';') i++;
        } else if (sig[i] == '[') {
            while (sig[i] == '[') i++;
            if (sig[i] == 'L') {
                while (sig[i] != '\0' && sig[i] != ';') i++;
                if (sig[i] == ';') i++;
            } else if (sig[i] != '\0') {
                i++;
            }
            count++;
        } else {
            i++;
        }
    }
    return count;
}

/* ========================================================================== */
/* Internal helpers: block map                                                 */
/* ========================================================================== */

/**
 * A pc→block-index mapping used during construction to find which block
 * a given bytecode PC belongs to.
 */
typedef struct {
    size_t    pc;
    uint32_t  block_index;
} vtx_pc_to_block_t;

/* Compare function for qsort of pc-to-block entries.
 * Available for future use in advanced scheduling. */
static int pc_block_cmp(const void *a, const void *b)
    __attribute__((unused));
static int pc_block_cmp(const void *a, const void *b)
{
    const vtx_pc_to_block_t *ea = (const vtx_pc_to_block_t *)a;
    const vtx_pc_to_block_t *eb = (const vtx_pc_to_block_t *)b;
    if (ea->pc < eb->pc) return -1;
    if (ea->pc > eb->pc) return 1;
    return 0;
}

/**
 * Binary search for the block that contains the given PC.
 * Returns the block index, or (uint32_t)-1 if not found.
 * Available for future use in advanced scheduling.
 */
static uint32_t find_block_for_pc(const vtx_pc_to_block_t *map,
                                  uint32_t map_count,
                                  size_t pc)
    __attribute__((unused));
static uint32_t find_block_for_pc(const vtx_pc_to_block_t *map,
                                  uint32_t map_count,
                                  size_t pc)
{
    if (map_count == 0) return (uint32_t)-1;

    uint32_t lo = 0, hi = map_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (map[mid].pc <= pc) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    /* lo is the first entry with pc > target; the block is lo-1 */
    if (lo == 0) return (uint32_t)-1;
    uint32_t idx = lo - 1;
    return map[idx].block_index;
}

/* ========================================================================== */
/* Internal: grow a dynamic uint32_t array (arena-allocated)                   */
/* ========================================================================== */

static int grow_uint32_array(vtx_arena_t *arena, uint32_t **arr,
                             uint32_t *count, uint32_t *capacity)
{
    uint32_t new_cap = (*capacity == 0) ? 4 : (*capacity * 2);
    uint32_t *new_arr = (uint32_t *)vtx_arena_alloc(arena, new_cap * sizeof(uint32_t));
    if (new_arr == NULL) return -1;

    if (*arr != NULL && *count > 0) {
        memcpy(new_arr, *arr, *count * sizeof(uint32_t));
    }
    *arr = new_arr;
    *capacity = new_cap;
    return 0;
}

static int push_pred(vtx_arena_t *arena, vtx_block_info_t *block, uint32_t pred_idx)
{
    if (block->pred_count >= block->pred_capacity) {
        if (grow_uint32_array(arena, &block->pred_indices,
                              &block->pred_count, &block->pred_capacity) != 0) {
            return -1;
        }
    }
    block->pred_indices[block->pred_count++] = pred_idx;
    return 0;
}

static int push_succ(vtx_arena_t *arena, vtx_block_info_t *block, uint32_t succ_idx)
{
    if (block->succ_count >= block->succ_capacity) {
        if (grow_uint32_array(arena, &block->succ_indices,
                              &block->succ_count, &block->succ_capacity) != 0) {
            return -1;
        }
    }
    block->succ_indices[block->succ_count++] = succ_idx;
    return 0;
}

/* ========================================================================== */
/* Graph init/destroy                                                          */
/* ========================================================================== */

int vtx_graph_init(vtx_graph_t *graph, uint32_t max_params)
{
    VTX_ASSERT(graph != NULL, "graph must not be NULL");
    memset(graph, 0, sizeof(vtx_graph_t));

    if (vtx_node_table_init(&graph->node_table, VTX_NODE_TABLE_INITIAL_CAPACITY) != 0) {
        return -1;
    }

    /* Create the Start node */
    vtx_nodeid_t start = vtx_node_create(&graph->node_table, VTX_OP_Start);
    if (start == VTX_NODEID_INVALID) {
        vtx_node_table_destroy(&graph->node_table);
        return -1;
    }
    graph->start_node = start;
    graph->entry_control = start;

    /* Create Province node (initial memory state) */
    vtx_nodeid_t province = vtx_node_create(&graph->node_table, VTX_OP_Province);
    if (province == VTX_NODEID_INVALID) {
        vtx_node_table_destroy(&graph->node_table);
        return -1;
    }
    /* Province's input is Start (control dependency) */
    vtx_node_add_input(&graph->node_table, province, start);
    graph->entry_memory = province;

    /* Create Parameter projections */
    graph->parameter_count = max_params;
    if (max_params > 0) {
        graph->parameters = (vtx_nodeid_t *)malloc(max_params * sizeof(vtx_nodeid_t));
        if (graph->parameters == NULL) {
            vtx_node_table_destroy(&graph->node_table);
            return -1;
        }
        for (uint32_t i = 0; i < max_params; i++) {
            vtx_nodeid_t param = vtx_node_create(&graph->node_table, VTX_OP_Parameter);
            if (param == VTX_NODEID_INVALID) {
                vtx_node_table_destroy(&graph->node_table);
                free(graph->parameters);
                graph->parameters = NULL;
                return -1;
            }
            vtx_node_t *p = vtx_node_get(&graph->node_table, param);
            p->local_index = i;
            /* Parameter depends on Start */
            vtx_node_add_input(&graph->node_table, param, start);
            graph->parameters[i] = param;
        }
    } else {
        graph->parameters = NULL;
    }

    return 0;
}

void vtx_graph_destroy(vtx_graph_t *graph)
{
    if (graph == NULL) return;

    vtx_node_table_destroy(&graph->node_table);
    free(graph->parameters);
    graph->parameters = NULL;
    graph->blocks = NULL; /* arena-allocated, no free needed */
    graph->block_count = 0;
    graph->block_capacity = 0;
}

vtx_node_t *vtx_graph_node(vtx_graph_t *graph, vtx_nodeid_t id)
{
    return vtx_node_get(&graph->node_table, id);
}

uint32_t vtx_graph_node_count(const vtx_graph_t *graph)
{
    return graph->node_table.count;
}

/* ========================================================================== */
/* Graph building: identify basic blocks                                        */
/* ========================================================================== */

/**
 * Phase 1: Scan bytecode to identify basic block boundaries.
 * A new block starts at:
 *   - PC 0
 *   - Target of any branch
 *   - Instruction immediately after a branch (fall-through)
 *   - Catch handler PC
 */
static uint32_t identify_blocks(vtx_arena_t *arena,
                                const vtx_bytecode_t *bc,
                                vtx_block_info_t **out_blocks,
                                uint32_t *out_count)
{
    /* We need a bitset of "block start" PCs.
     * Use a simple approach: scan all instructions, mark starts, then compact. */

    size_t code_len = bc->length;

    /* Allocate a boolean array for block-start markers */
    bool *is_block_start = (bool *)vtx_arena_alloc(arena, code_len * sizeof(bool));
    if (is_block_start == NULL) return (uint32_t)-1;
    memset(is_block_start, 0, code_len * sizeof(bool));

    /* PC 0 is always a block start */
    is_block_start[0] = true;

    /* Walk all instructions */
    size_t pc = 0;
    while (pc < code_len) {
        vtx_opcode_t op = vtx_bytecode_opcode_at(bc, pc);
        size_t insn_len = vtx_bytecode_insn_length(bc, pc);

        switch (op) {
        case VT_OP_GOTO: {
            uint16_t target = vtx_bytecode_read_operand(bc, pc);
            if (target < code_len) is_block_start[target] = true;
            /* No fall-through for unconditional jump */
            break;
        }
        case VT_OP_IF_TRUE:
        case VT_OP_IF_FALSE: {
            uint16_t target = vtx_bytecode_read_operand(bc, pc);
            if (target < code_len) is_block_start[target] = true;
            /* Fall-through is also a block start */
            size_t fall = pc + insn_len;
            if (fall < code_len) is_block_start[fall] = true;
            break;
        }
        case VT_OP_RETURN:
        case VT_OP_RETURN_VALUE:
        case VT_OP_THROW: {
            /* Fall-through after return/throw is a block start (if reachable, but mark it anyway) */
            size_t fall = pc + insn_len;
            if (fall < code_len) is_block_start[fall] = true;
            break;
        }
        case VT_OP_CATCH: {
            uint16_t handler_pc = vtx_bytecode_read_operand(bc, pc);
            if (handler_pc < code_len) is_block_start[handler_pc] = true;
            break;
        }
        default:
            break;
        }

        pc += insn_len;
    }

    /* Count blocks */
    uint32_t nblocks = 0;
    for (size_t i = 0; i < code_len; i++) {
        if (is_block_start[i]) nblocks++;
    }

    if (nblocks == 0) {
        *out_blocks = NULL;
        *out_count = 0;
        return 0;
    }

    /* Allocate block descriptors */
    vtx_block_info_t *blocks = (vtx_block_info_t *)vtx_arena_alloc(
        arena, nblocks * sizeof(vtx_block_info_t));
    if (blocks == NULL) return (uint32_t)-1;
    memset(blocks, 0, nblocks * sizeof(vtx_block_info_t));

    /* Fill in start PCs */
    uint32_t bi = 0;
    for (size_t i = 0; i < code_len; i++) {
        if (is_block_start[i]) {
            VTX_ASSERT(bi < nblocks, "block count mismatch");
            blocks[bi].start_pc = i;
            blocks[bi].region_node = VTX_NODEID_INVALID;
            blocks[bi].control_node = VTX_NODEID_INVALID;
            blocks[bi].memory_node = VTX_NODEID_INVALID;
            blocks[bi].exception_target = VTX_NODEID_INVALID;
            blocks[bi].locals = NULL;
            blocks[bi].pred_indices = NULL;
            blocks[bi].pred_count = 0;
            blocks[bi].pred_capacity = 0;
            blocks[bi].succ_indices = NULL;
            blocks[bi].succ_count = 0;
            blocks[bi].succ_capacity = 0;
            blocks[bi].is_loop_header = false;
            blocks[bi].is_loop_end = false;
            blocks[bi].is_catch_handler = false;
            blocks[bi].is_unreachable = false;
            blocks[bi].exit_stack = NULL;
            blocks[bi].exit_sp = 0;
            bi++;
        }
    }

    /* Set end PCs: each block ends where the next one begins */
    for (uint32_t i = 0; i < nblocks; i++) {
        if (i + 1 < nblocks) {
            blocks[i].end_pc = blocks[i + 1].start_pc;
        } else {
            blocks[i].end_pc = code_len;
        }
    }

    /* Detect loop headers: a backward branch target.
     * Walk all branch instructions again. */
    pc = 0;
    while (pc < code_len) {
        vtx_opcode_t op = vtx_bytecode_opcode_at(bc, pc);
        size_t insn_len = vtx_bytecode_insn_length(bc, pc);

        if (op == VT_OP_GOTO || op == VT_OP_IF_TRUE || op == VT_OP_IF_FALSE) {
            uint16_t target = vtx_bytecode_read_operand(bc, pc);
            if (target <= pc) {
                /* Backward branch → target is a loop header */
                for (uint32_t i = 0; i < nblocks; i++) {
                    if (blocks[i].start_pc == target) {
                        blocks[i].is_loop_header = true;
                        break;
                    }
                }
                /* The block containing the backward branch is the loop end */
                for (uint32_t i = 0; i < nblocks; i++) {
                    if (blocks[i].start_pc <= pc && pc < blocks[i].end_pc) {
                        blocks[i].is_loop_end = true;
                        break;
                    }
                }
            }
        }

        pc += insn_len;
    }

    /* Detect catch handlers */
    pc = 0;
    while (pc < code_len) {
        vtx_opcode_t op = vtx_bytecode_opcode_at(bc, pc);
        size_t insn_len = vtx_bytecode_insn_length(bc, pc);
        if (op == VT_OP_CATCH) {
            uint16_t handler_pc = vtx_bytecode_read_operand(bc, pc);
            for (uint32_t i = 0; i < nblocks; i++) {
                if (blocks[i].start_pc == handler_pc) {
                    blocks[i].is_catch_handler = true;
                    break;
                }
            }
        }
        pc += insn_len;
    }

    /* Build predecessor/successor edges */
    /* For each block, find its terminator and add edges */
    for (uint32_t i = 0; i < nblocks; i++) {
        /* Find the last instruction in the block */
        size_t last_pc = blocks[i].end_pc;
        if (last_pc == 0) continue;
        /* Walk back to find the last instruction's start */
        size_t scan = blocks[i].start_pc;
        size_t last_insn_pc = scan;
        while (scan < blocks[i].end_pc) {
            last_insn_pc = scan;
            scan += vtx_bytecode_insn_length(bc, scan);
        }

        vtx_opcode_t term_op = vtx_bytecode_opcode_at(bc, last_insn_pc);

        switch (term_op) {
        case VT_OP_GOTO: {
            uint16_t target = vtx_bytecode_read_operand(bc, last_insn_pc);
            for (uint32_t j = 0; j < nblocks; j++) {
                if (blocks[j].start_pc == target) {
                    push_succ(arena, &blocks[i], j);
                    push_pred(arena, &blocks[j], i);
                    break;
                }
            }
            break;
        }
        case VT_OP_IF_TRUE:
        case VT_OP_IF_FALSE: {
            uint16_t target = vtx_bytecode_read_operand(bc, last_insn_pc);
            /* Branch target */
            for (uint32_t j = 0; j < nblocks; j++) {
                if (blocks[j].start_pc == target) {
                    push_succ(arena, &blocks[i], j);
                    push_pred(arena, &blocks[j], i);
                    break;
                }
            }
            /* Fall-through */
            size_t fall = last_insn_pc + vtx_bytecode_insn_length(bc, last_insn_pc);
            for (uint32_t j = 0; j < nblocks; j++) {
                if (blocks[j].start_pc == fall) {
                    push_succ(arena, &blocks[i], j);
                    push_pred(arena, &blocks[j], i);
                    break;
                }
            }
            break;
        }
        case VT_OP_RETURN:
        case VT_OP_RETURN_VALUE:
        case VT_OP_THROW:
            /* No successors */
            break;
        default: {
            /* Fall-through to next block */
            if (i + 1 < nblocks) {
                push_succ(arena, &blocks[i], i + 1);
                push_pred(arena, &blocks[i + 1], i);
            }
            break;
        }
        }
    }

    *out_blocks = blocks;
    *out_count = nblocks;
    return 0;
}

/* ========================================================================== */
/* Graph building: create SoN nodes for each block                             */
/* ========================================================================== */

/**
 * Create a Region/LoopBegin node for a block, connect it to predecessor
 * control outputs, and create Phi nodes for locals that differ.
 */
static int create_block_entry(vtx_graph_t *graph, vtx_block_info_t *blocks,
                              uint32_t block_idx, uint16_t max_locals,
                              vtx_arena_t *arena)
{
    vtx_block_info_t *block = &blocks[block_idx];
    vtx_node_table_t *nt = &graph->node_table;

    if (block_idx == 0 && !block->is_loop_header) {
        /* Entry block (not a loop header): Region is just Start */
        block->region_node = graph->start_node;
        block->control_node = graph->start_node;
        block->memory_node = graph->entry_memory;

        /* Allocate locals for entry block: map locals to Parameter nodes */
        block->locals = (vtx_nodeid_t *)vtx_arena_alloc(arena, max_locals * sizeof(vtx_nodeid_t));
        if (block->locals == NULL) return -1;
        for (uint16_t i = 0; i < max_locals; i++) {
            if (i < graph->parameter_count) {
                block->locals[i] = graph->parameters[i];
            } else {
                /* Undefined local */
                vtx_nodeid_t undef = vtx_node_create(nt, VTX_OP_Constant);
                if (undef == VTX_NODEID_INVALID) return -1;
                vtx_node_t *n = vtx_node_get(nt, undef);
                n->constval = vtx_constval_void();
                n->type = VTX_TYPE_Void;
                block->locals[i] = undef;
            }
        }
        return 0;
    }

    if (block_idx == 0 && block->is_loop_header) {
        /* Entry block is also a loop header (backward branch to PC 0).
         * Create a LoopBegin node that takes Start as the entry control
         * input; the back-edge input will be connected in Phase 4. */
        vtx_nodeid_t loop_begin = vtx_node_create(nt, VTX_OP_LoopBegin);
        if (loop_begin == VTX_NODEID_INVALID) return -1;
        vtx_node_add_input(nt, loop_begin, graph->start_node);
        block->region_node = loop_begin;
        block->control_node = loop_begin;
        block->memory_node = graph->entry_memory;

        /* Allocate locals for entry block.
         *
         * BUGFIX (audit #3, loop hang): For entry-as-loop-header, we must
         * create Phi nodes for ALL locals, merging the initial value
         * (Parameter/Constant) with the back-edge value (which will be
         * connected in Phase 4). Without this, 'load_local' in the loop
         * body pushes the initial Parameter, not the Phi — so the loop
         * variable never changes, causing an infinite loop.
         *
         * We create the Phi now with [initial_value, placeholder, LoopBegin].
         * Phase 4 will fill in the back-edge value. */
        block->locals = (vtx_nodeid_t *)vtx_arena_alloc(arena, max_locals * sizeof(vtx_nodeid_t));
        if (block->locals == NULL) return -1;
        for (uint16_t i = 0; i < max_locals; i++) {
            vtx_nodeid_t initial_val;
            if (i < graph->parameter_count) {
                initial_val = graph->parameters[i];
            } else {
                vtx_nodeid_t undef = vtx_node_create(nt, VTX_OP_Constant);
                if (undef == VTX_NODEID_INVALID) return -1;
                vtx_node_t *n = vtx_node_get(nt, undef);
                n->constval = vtx_constval_void();
                n->type = VTX_TYPE_Void;
                initial_val = undef;
            }

            /* Create a Phi node merging initial_val with back-edge (placeholder).
             * The back-edge input (second data input) will be set in Phase 4
             * when we process the loop latch. For now, use VTX_NODEID_INVALID
             * as a placeholder — Phase 4 will replace it. */
            vtx_nodeid_t phi = vtx_node_create(nt, VTX_OP_Phi);
            if (phi == VTX_NODEID_INVALID) return -1;
            vtx_node_t *phi_n = vtx_node_get(nt, phi);
            phi_n->flags = VTX_NF_DATA | VTX_NF_PINNED;
            phi_n->type = VTX_TYPE_Bottom;
            /* Inputs: [initial_value, LoopBegin] — back-edge input added in Phase 4 */
            vtx_node_add_input(nt, phi, initial_val);
            vtx_node_add_input(nt, phi, loop_begin);

            block->locals[i] = phi;
        }
        return 0;
    }

    /* Create Region or LoopBegin */
    vtx_node_opcode_t region_op = block->is_loop_header ? VTX_OP_LoopBegin : VTX_OP_Region;

    /* Count forward predecessors FIRST so we can detect unreachable blocks. */
    uint32_t forward_pred_count = 0;
    for (uint32_t p = 0; p < block->pred_count; p++) {
        uint32_t pred_idx = block->pred_indices[p];
        /* Skip back-edge predecessors for loop headers */
        if (block->is_loop_header && blocks[pred_idx].is_loop_end) {
            continue;
        }
        forward_pred_count++;
    }

    /* Unreachable block: no forward predecessors and not the entry block.
     *
     * BUGFIX (audit #1, fuzz-discovered): The original code created a
     * Region node for every block regardless of reachability, then left
     * unreachable Regions with 0 inputs — failing verifier check #8
     * ("All Region nodes have at least one input"). The correct behavior
     * is to mark the block as unreachable, NOT create a Region, and skip
     * its Phase 3 instruction walk. Subsequent Phi-creation logic in
     * successor blocks must treat unreachable blocks as producing no
     * values (handled in the merge logic by checking exit_sp). */
    if (forward_pred_count == 0) {
        block->region_node  = VTX_NODEID_INVALID;
        block->control_node = VTX_NODEID_INVALID;
        block->memory_node  = VTX_NODEID_INVALID;
        block->is_unreachable = true;

        /* Allocate locals (all undefined) so later phases can read blocks[i].locals[j]
         * without crashing, even though this block is never executed. */
        block->locals = (vtx_nodeid_t *)vtx_arena_alloc(arena, max_locals * sizeof(vtx_nodeid_t));
        if (block->locals == NULL) return -1;
        for (uint16_t i = 0; i < max_locals; i++) {
            block->locals[i] = VTX_NODEID_INVALID;
        }
        return 0;
    }

    vtx_nodeid_t region = vtx_node_create(nt, region_op);
    if (region == VTX_NODEID_INVALID) return -1;
    block->region_node = region;

    /* Connect predecessor control outputs as Region inputs.
     *
     * BUGFIX: For loop headers, we must NOT add back-edge predecessor
     * inputs during Phase 2. The back-edge predecessor (loop latch) hasn't
     * been processed yet — its control_node is VTX_NODEID_INVALID, and
     * its real control output will only be known after Phase 3 processes
     * the latch block. We skip back-edge predecessors here and connect
     * them in Phase 4 when we create the LoopEnd.
     *
     * The original code skipped back-edge predecessors implicitly because
     * their control_node was INVALID, but then it used block->pred_count
     * (which INCLUDES the back-edge) to create Phi inputs — causing a
     * mismatch between the number of Region inputs and Phi inputs. This
     * inconsistency crashed later Phases that expect Phis to have one
     * data input per Region input. */
    for (uint32_t p = 0; p < block->pred_count; p++) {
        uint32_t pred_idx = block->pred_indices[p];
        /* Skip back-edge predecessors for loop headers */
        if (block->is_loop_header && blocks[pred_idx].is_loop_end) {
            continue;
        }
        /* Skip unreachable predecessors — they have no control output. */
        if (blocks[pred_idx].is_unreachable) {
            continue;
        }
        vtx_nodeid_t pred_ctrl = blocks[pred_idx].control_node;
        if (pred_ctrl != VTX_NODEID_INVALID) {
            vtx_node_add_input(nt, region, pred_ctrl);
        }
    }

    block->control_node = region;

    /* Create initial memory Phi for merge points with >1 predecessor.
     *
     * BUGFIX: Only count forward predecessors for Phi creation. The
     * back-edge predecessor's memory state will be added as a Phi input
     * in Phase 4. Using block->pred_count (which includes the back-edge)
     * caused Phi inputs to include VTX_NODEID_INVALID or stale values
     * from the unprocessed latch block. */
    if (forward_pred_count > 1) {
        vtx_nodeid_t mem_phi = vtx_node_create(nt, VTX_OP_Phi);
        if (mem_phi == VTX_NODEID_INVALID) return -1;
        vtx_node_t *phi_node = vtx_node_get(nt, mem_phi);
        phi_node->flags = vtx_nf_union(phi_node->flags, VTX_NF_MEMORY);

        /* Add memory inputs from each FORWARD predecessor only */
        for (uint32_t p = 0; p < block->pred_count; p++) {
            uint32_t pred_idx = block->pred_indices[p];
            if (block->is_loop_header && blocks[pred_idx].is_loop_end) {
                continue; /* back-edge input added in Phase 4 */
            }
            if (blocks[pred_idx].is_unreachable) {
                continue; /* unreachable predecessor: no memory contribution */
            }
            vtx_nodeid_t pred_mem = blocks[pred_idx].memory_node;
            if (pred_mem == VTX_NODEID_INVALID) {
                pred_mem = graph->entry_memory;
            }
            vtx_node_add_input(nt, mem_phi, pred_mem);
        }
        /* Memory Phi depends on the Region for control */
        vtx_node_add_input(nt, mem_phi, region);
        block->memory_node = mem_phi;
    } else if (forward_pred_count == 1) {
        /* Single forward predecessor: inherit memory directly */
        for (uint32_t p = 0; p < block->pred_count; p++) {
            uint32_t pred_idx = block->pred_indices[p];
            if (block->is_loop_header && blocks[pred_idx].is_loop_end) {
                continue;
            }
            if (blocks[pred_idx].is_unreachable) {
                continue;
            }
            block->memory_node = blocks[pred_idx].memory_node;
            break;
        }
        if (block->memory_node == VTX_NODEID_INVALID) {
            block->memory_node = graph->entry_memory;
        }
    } else {
        block->memory_node = graph->entry_memory;
    }

    /* Allocate locals for this block */
    block->locals = (vtx_nodeid_t *)vtx_arena_alloc(arena, max_locals * sizeof(vtx_nodeid_t));
    if (block->locals == NULL) return -1;

    if (block->is_loop_header) {
        /* BUGFIX (audit #3, loop hang): For loop headers, ALWAYS create Phi
         * nodes for locals, even with forward_pred_count == 1. The loop
         * header's locals merge the initial value (from the forward
         * predecessor) with the back-edge value (from the loop latch,
         * connected in Phase 4). Without Phi nodes, 'load_local' in the
         * loop body pushes the initial value, and the loop variable never
         * changes — causing infinite loops.
         *
         * The Phi's inputs are: [initial_value, LoopBegin].
         * Phase 4 will add the back-edge value as a third input. */
        for (uint16_t li = 0; li < max_locals; li++) {
            /* Get the initial value from the forward predecessor */
            vtx_nodeid_t initial_val = VTX_NODEID_INVALID;
            for (uint32_t p = 0; p < block->pred_count; p++) {
                uint32_t pred_idx = block->pred_indices[p];
                if (blocks[pred_idx].is_loop_end) continue;
                if (blocks[pred_idx].is_unreachable) continue;
                initial_val = blocks[pred_idx].locals[li];
                break;
            }
            if (initial_val == VTX_NODEID_INVALID) {
                initial_val = graph->entry_memory; /* fallback */
            }

            /* Create a Phi node: [initial_value, LoopBegin] */
            vtx_nodeid_t phi = vtx_node_create(nt, VTX_OP_Phi);
            if (phi == VTX_NODEID_INVALID) return -1;
            vtx_node_t *phi_n = vtx_node_get(nt, phi);
            phi_n->flags = VTX_NF_DATA | VTX_NF_PINNED;
            phi_n->type = VTX_TYPE_Bottom;
            vtx_node_add_input(nt, phi, initial_val);
            vtx_node_add_input(nt, phi, region);
            block->locals[li] = phi;
        }
    } else if (forward_pred_count == 1) {
        /* Single forward predecessor: inherit locals directly */
        for (uint32_t p = 0; p < block->pred_count; p++) {
            uint32_t pred_idx = block->pred_indices[p];
            if (block->is_loop_header && blocks[pred_idx].is_loop_end) {
                continue;
            }
            if (blocks[pred_idx].is_unreachable) {
                continue;
            }
            for (uint16_t i = 0; i < max_locals; i++) {
                block->locals[i] = blocks[pred_idx].locals[i];
            }
            break;
        }
    } else if (forward_pred_count > 1) {
        /* Multiple forward predecessors: create Phi nodes for locals that differ.
         * Back-edge predecessor inputs will be added to Phis in Phase 4. */
        for (uint16_t li = 0; li < max_locals; li++) {
            /* Check if all FORWARD predecessors agree on this local */
            bool all_same = true;
            vtx_nodeid_t first_val = VTX_NODEID_INVALID;
            for (uint32_t p = 0; p < block->pred_count; p++) {
                uint32_t pred_idx = block->pred_indices[p];
                if (block->is_loop_header && blocks[pred_idx].is_loop_end) {
                    continue;
                }
                if (blocks[pred_idx].is_unreachable) {
                    continue;
                }
                vtx_nodeid_t val = blocks[pred_idx].locals[li];
                if (first_val == VTX_NODEID_INVALID) {
                    first_val = val;
                } else if (val != first_val) {
                    all_same = false;
                    break;
                }
            }

            if (all_same) {
                block->locals[li] = first_val;
            } else {
                /* Create a Phi node */
                vtx_nodeid_t phi = vtx_node_create(nt, VTX_OP_Phi);
                if (phi == VTX_NODEID_INVALID) return -1;
                vtx_node_t *phi_n = vtx_node_get(nt, phi);

                /* Add one input per FORWARD predecessor only.
                 * Back-edge input will be added in Phase 4. */
                for (uint32_t p = 0; p < block->pred_count; p++) {
                    uint32_t pred_idx = block->pred_indices[p];
                    if (block->is_loop_header && blocks[pred_idx].is_loop_end) {
                        continue;
                    }
                    if (blocks[pred_idx].is_unreachable) {
                        continue;
                    }
                    vtx_nodeid_t val = blocks[pred_idx].locals[li];
                    vtx_node_add_input(nt, phi, val);
                }
                /* Phi also depends on Region for control */
                vtx_node_add_input(nt, phi, region);

                /* Set Phi type from inputs */
                phi_n->type = VTX_TYPE_Bottom;

                block->locals[li] = phi;
            }
        }
    } else {
        /* No predecessors (unreachable block) — initialize to undefined */
        for (uint16_t i = 0; i < max_locals; i++) {
            vtx_nodeid_t undef = vtx_node_create(nt, VTX_OP_Constant);
            if (undef == VTX_NODEID_INVALID) return -1;
            vtx_node_t *n = vtx_node_get(nt, undef);
            n->constval = vtx_constval_void();
            n->type = VTX_TYPE_Void;
            block->locals[i] = undef;
        }
    }

    /* For loop headers, create LoopEnd placeholder in the loop latch block */
    if (block->is_loop_header) {
        /* Find the predecessor that is the loop latch (backward branch) */
        for (uint32_t p = 0; p < block->pred_count; p++) {
            uint32_t pred_idx = block->pred_indices[p];
            if (blocks[pred_idx].is_loop_end) {
                /* Will create LoopEnd when processing that block */
            }
        }
    }

    return 0;
}

/**
 * Process a single bytecode instruction within a block, emitting SoN nodes.
 * (Currently unused — instruction processing is inlined in vtx_graph_build.)
 */
#if 0
static int process_instruction(vtx_graph_t *graph, vtx_block_info_t *block,
                               const vtx_bytecode_t *bc, size_t pc,
                               vtx_type_system_t *ts)
{
    vtx_node_table_t *nt = &graph->node_table;
    vtx_opcode_t op = vtx_bytecode_opcode_at(bc, pc);
    uint16_t operand = 0;
    if (vtx_opcode_table[op].has_operand) {
        operand = vtx_bytecode_read_operand(bc, pc);
    }

    /* Operand stack: we simulate a local stack for the current block */
    /* We use a simple approach: maintain a stack of NodeIDs in the block */

    /* For simplicity in this builder, we handle each opcode directly.
     * In a full implementation, we'd track the operand stack separately.
     * Here we use the block's locals and a simulated operand stack. */

    /* The operand stack is simulated per-block. We'll use a fixed-size
     * array allocated once per graph build. For now, let's use a simpler
     * approach: maintain a small stack within the block processing. */

    /* NOTE: This is a simplified but complete builder. The operand stack
     * is tracked as a local array within the build function. Each block
     * processes its instructions sequentially. */

    (void)ts; /* may be used for type lookups in a more advanced builder */

    vtx_nodeid_t result = VTX_NODEID_INVALID;

    switch (op) {
    /* ---- Constants ---- */
    case VT_OP_LOAD_CONST_INT: {
        result = vtx_node_create(nt, VTX_OP_Constant);
        if (result == VTX_NODEID_INVALID) return -1;
        vtx_node_t *n = vtx_node_get(nt, result);
        /* Look up the constant in the pool */
        vtx_value_t cv = bc->constant_pool[operand];
        if (vtx_is_smi(cv)) {
            n->constval = vtx_constval_int(vtx_smi_value(cv));
            n->type = VTX_TYPE_Int;
        } else if (vtx_is_double(cv)) {
            n->constval = vtx_constval_float(vtx_double_value(cv));
            n->type = VTX_TYPE_Float;
        } else {
            n->constval = vtx_constval_int((int64_t)operand);
            n->type = VTX_TYPE_Int;
        }
        break;
    }
    case VT_OP_LOAD_CONST_FLOAT: {
        result = vtx_node_create(nt, VTX_OP_Constant);
        if (result == VTX_NODEID_INVALID) return -1;
        vtx_node_t *n = vtx_node_get(nt, result);
        vtx_value_t cv = bc->constant_pool[operand];
        n->constval = vtx_constval_float(vtx_double_value(cv));
        n->type = VTX_TYPE_Float;
        break;
    }
    case VT_OP_LOAD_CONST_STR: {
        result = vtx_node_create(nt, VTX_OP_Constant);
        if (result == VTX_NODEID_INVALID) return -1;
        vtx_node_t *n = vtx_node_get(nt, result);
        vtx_value_t cv = bc->constant_pool[operand];
        n->constval = vtx_constval_ptr(vtx_heap_ptr(cv));
        n->type = VTX_TYPE_Ptr;
        break;
    }
    case VT_OP_LOAD_NULL: {
        result = vtx_node_create(nt, VTX_OP_Constant);
        if (result == VTX_NODEID_INVALID) return -1;
        vtx_node_t *n = vtx_node_get(nt, result);
        n->constval = vtx_constval_ptr(NULL);
        n->type = VTX_TYPE_Ptr;
        break;
    }
    case VT_OP_LOAD_TRUE:
    case VT_OP_LOAD_FALSE: {
        result = vtx_node_create(nt, VTX_OP_Constant);
        if (result == VTX_NODEID_INVALID) return -1;
        vtx_node_t *n = vtx_node_get(nt, result);
        n->constval = vtx_constval_int(op == VT_OP_LOAD_TRUE ? 1 : 0);
        n->type = VTX_TYPE_Int;
        break;
    }
    case VT_OP_LOAD_UNDEFINED: {
        result = vtx_node_create(nt, VTX_OP_Constant);
        if (result == VTX_NODEID_INVALID) return -1;
        vtx_node_t *n = vtx_node_get(nt, result);
        n->constval = vtx_constval_void();
        n->type = VTX_TYPE_Void;
        break;
    }

    /* ---- Local variables ---- */
    case VT_OP_LOAD_LOCAL: {
        /* Result is the current value of the local */
        result = block->locals[operand];
        break;
    }
    case VT_OP_STORE_LOCAL: {
        /* The top of stack goes into the local — handled by the stack simulation */
        /* This is a marker; actual value is consumed from the stack by the
         * build loop. We'll handle it there. */
        break;
    }

    /* ---- Arithmetic (pop 2, push 1) ---- */
    case VT_OP_IADD: case VT_OP_FADD: {
        result = vtx_node_create(nt, VTX_OP_Add);
        if (result == VTX_NODEID_INVALID) return -1;
        /* Inputs will be connected by the stack simulation */
        break;
    }
    case VT_OP_ISUB: case VT_OP_FSUB: {
        result = vtx_node_create(nt, VTX_OP_Sub);
        if (result == VTX_NODEID_INVALID) return -1;
        break;
    }
    case VT_OP_IMUL: case VT_OP_FMUL: {
        result = vtx_node_create(nt, VTX_OP_Mul);
        if (result == VTX_NODEID_INVALID) return -1;
        break;
    }
    case VT_OP_IDIV: {
        result = vtx_node_create(nt, VTX_OP_Div);
        if (result == VTX_NODEID_INVALID) return -1;
        /* BUGFIX: Div/Mod can cause divide-by-zero (SIGFPE), which is a
         * side effect. Mark as SIDE_EFFECT so the scheduler doesn't hoist
         * them into blocks where they might execute before a guard check. */
        vtx_node_get(nt, result)->flags |= VTX_NF_SIDE_EFFECT;
        break;
    }
    case VT_OP_FDIV: {
        result = vtx_node_create(nt, VTX_OP_Div);
        if (result == VTX_NODEID_INVALID) return -1;
        vtx_node_t *n = vtx_node_get(nt, result);
        n->type = VTX_TYPE_Float;
        break;
    }
    case VT_OP_IMOD: {
        result = vtx_node_create(nt, VTX_OP_Mod);
        if (result == VTX_NODEID_INVALID) return -1;
        /* Same as Div: Mod can cause divide-by-zero */
        vtx_node_get(nt, result)->flags |= VTX_NF_SIDE_EFFECT;
        break;
    }
    case VT_OP_ISHL: {
        result = vtx_node_create(nt, VTX_OP_Shl);
        if (result == VTX_NODEID_INVALID) return -1;
        break;
    }
    case VT_OP_ISHR: {
        result = vtx_node_create(nt, VTX_OP_Shr);
        if (result == VTX_NODEID_INVALID) return -1;
        break;
    }
    case VT_OP_IAND: {
        result = vtx_node_create(nt, VTX_OP_And);
        if (result == VTX_NODEID_INVALID) return -1;
        break;
    }
    case VT_OP_IOR: {
        result = vtx_node_create(nt, VTX_OP_Or);
        if (result == VTX_NODEID_INVALID) return -1;
        break;
    }
    case VT_OP_IXOR: {
        result = vtx_node_create(nt, VTX_OP_Xor);
        if (result == VTX_NODEID_INVALID) return -1;
        break;
    }
    case VT_OP_INEG: {
        result = vtx_node_create(nt, VTX_OP_Neg);
        if (result == VTX_NODEID_INVALID) return -1;
        break;
    }
    case VT_OP_INOT: {
        result = vtx_node_create(nt, VTX_OP_Not);
        if (result == VTX_NODEID_INVALID) return -1;
        break;
    }

    /* ---- Comparisons ---- */
    case VT_OP_ICMP_EQ: case VT_OP_ICMP_NE:
    case VT_OP_ICMP_LT: case VT_OP_ICMP_LE:
    case VT_OP_ICMP_GT: case VT_OP_ICMP_GE: {
        result = vtx_node_create(nt, VTX_OP_Cmp);
        if (result == VTX_NODEID_INVALID) return -1;
        vtx_node_t *n = vtx_node_get(nt, result);
        vtx_cond_t cond_map[] = {
            VTX_COND_EQ, VTX_COND_NE, VTX_COND_LT,
            VTX_COND_LE, VTX_COND_GT, VTX_COND_GE
        };
        n->cond = cond_map[op - VT_OP_ICMP_EQ];
        break;
    }
    case VT_OP_FCMP_EQ: case VT_OP_FCMP_NE:
    case VT_OP_FCMP_LT: case VT_OP_FCMP_LE:
    case VT_OP_FCMP_GT: case VT_OP_FCMP_GE: {
        result = vtx_node_create(nt, VTX_OP_CmpF);
        if (result == VTX_NODEID_INVALID) return -1;
        vtx_node_t *n = vtx_node_get(nt, result);
        vtx_cond_t cond_map[] = {
            VTX_COND_EQ, VTX_COND_NE, VTX_COND_LT,
            VTX_COND_LE, VTX_COND_GT, VTX_COND_GE
        };
        n->cond = cond_map[op - VT_OP_FCMP_EQ];
        break;
    }

    /* ---- Control flow ---- */
    case VT_OP_GOTO: {
        vtx_nodeid_t goto_node = vtx_node_create(nt, VTX_OP_Goto);
        if (goto_node == VTX_NODEID_INVALID) return -1;
        vtx_node_add_input(nt, goto_node, block->control_node);
        block->control_node = goto_node;
        result = VTX_NODEID_INVALID; /* no data output */
        break;
    }
    case VT_OP_IF_TRUE: case VT_OP_IF_FALSE: {
        vtx_nodeid_t if_node = vtx_node_create(nt, VTX_OP_If);
        if (if_node == VTX_NODEID_INVALID) return -1;
        vtx_node_t *n = vtx_node_get(nt, if_node);
        n->cond = (op == VT_OP_IF_TRUE) ? VTX_COND_NE : VTX_COND_EQ;
        /* Control input */
        vtx_node_add_input(nt, if_node, block->control_node);
        block->control_node = if_node;
        result = VTX_NODEID_INVALID; /* condition consumed by If node */
        break;
    }

    /* ---- Returns ---- */
    case VT_OP_RETURN: {
        vtx_nodeid_t ret = vtx_node_create(nt, VTX_OP_Return);
        if (ret == VTX_NODEID_INVALID) return -1;
        vtx_node_add_input(nt, ret, block->control_node);
        block->control_node = ret;
        result = VTX_NODEID_INVALID;
        break;
    }
    case VT_OP_RETURN_VALUE: {
        /* Value is on the stack — will be connected by stack simulation */
        vtx_nodeid_t ret = vtx_node_create(nt, VTX_OP_Return);
        if (ret == VTX_NODEID_INVALID) return -1;
        vtx_node_add_input(nt, ret, block->control_node);
        block->control_node = ret;
        result = ret; /* will get value input from stack */
        break;
    }

    /* ---- Field access ---- */
    case VT_OP_LOAD_FIELD: {
        result = vtx_node_create(nt, VTX_OP_LoadField);
        if (result == VTX_NODEID_INVALID) return -1;
        vtx_node_t *n = vtx_node_get(nt, result);
        n->field_offset = operand;
        n->type = VTX_TYPE_Ptr; /* or Bottom */
        /* Memory input */
        vtx_node_add_input(nt, result, block->memory_node);
        block->memory_node = result;
        break;
    }
    case VT_OP_STORE_FIELD: {
        result = vtx_node_create(nt, VTX_OP_StoreField);
        if (result == VTX_NODEID_INVALID) return -1;
        vtx_node_t *n = vtx_node_get(nt, result);
        n->field_offset = operand;
        vtx_node_add_input(nt, result, block->memory_node);
        block->memory_node = result;
        break;
    }

    /* ---- Calls ---- */
    case VT_OP_CALL_STATIC: {
        /* NOTE: This is inside #if 0 — not currently used.
         * IR-5 fix would require adding argument popping here
         * when this code path is re-enabled. */
        result = vtx_node_create(nt, VTX_OP_CallStatic);
        if (result == VTX_NODEID_INVALID) return -1;
        vtx_node_t *n = vtx_node_get(nt, result);
        n->method_index = operand;
        vtx_node_add_input(nt, result, block->control_node);
        vtx_node_add_input(nt, result, block->memory_node);
        block->control_node = result;
        block->memory_node = result;
        break;
    }
    case VT_OP_CALL_VIRTUAL: {
        result = vtx_node_create(nt, VTX_OP_CallVirtual);
        if (result == VTX_NODEID_INVALID) return -1;
        vtx_node_t *n = vtx_node_get(nt, result);
        n->method_index = operand;
        vtx_node_add_input(nt, result, block->control_node);
        vtx_node_add_input(nt, result, block->memory_node);
        block->control_node = result;
        block->memory_node = result;
        break;
    }
    case VT_OP_CALL_INTERFACE: {
        result = vtx_node_create(nt, VTX_OP_CallInterface);
        if (result == VTX_NODEID_INVALID) return -1;
        vtx_node_t *n = vtx_node_get(nt, result);
        n->method_index = operand;
        vtx_node_add_input(nt, result, block->control_node);
        vtx_node_add_input(nt, result, block->memory_node);
        block->control_node = result;
        block->memory_node = result;
        break;
    }

    /* ---- Object creation ---- */
    case VT_OP_NEW: {
        result = vtx_node_create(nt, VTX_OP_NewObject);
        if (result == VTX_NODEID_INVALID) return -1;
        vtx_node_t *n = vtx_node_get(nt, result);
        n->type_id = operand;
        vtx_node_add_input(nt, result, block->memory_node);
        block->memory_node = result;
        break;
    }
    case VT_OP_NEWARRAY: {
        result = vtx_node_create(nt, VTX_OP_NewArray);
        if (result == VTX_NODEID_INVALID) return -1;
        vtx_node_t *n = vtx_node_get(nt, result);
        n->type_id = operand;
        vtx_node_add_input(nt, result, block->memory_node);
        block->memory_node = result;
        break;
    }

    /* ---- Type checks ---- */
    case VT_OP_CHECKCAST: {
        result = vtx_node_create(nt, VTX_OP_CheckCast);
        if (result == VTX_NODEID_INVALID) return -1;
        vtx_node_t *n = vtx_node_get(nt, result);
        n->type_id = operand;
        break;
    }
    case VT_OP_INSTANCEOF: {
        result = vtx_node_create(nt, VTX_OP_InstanceOf);
        if (result == VTX_NODEID_INVALID) return -1;
        vtx_node_t *n = vtx_node_get(nt, result);
        n->type_id = operand;
        break;
    }

    /* ---- Array operations ---- */
    case VT_OP_ARRAY_LOAD: {
        result = vtx_node_create(nt, VTX_OP_LoadIndexed);
        if (result == VTX_NODEID_INVALID) return -1;
        vtx_node_add_input(nt, result, block->memory_node);
        block->memory_node = result;
        break;
    }
    case VT_OP_ARRAY_STORE: {
        result = vtx_node_create(nt, VTX_OP_StoreIndexed);
        if (result == VTX_NODEID_INVALID) return -1;
        vtx_node_add_input(nt, result, block->memory_node);
        block->memory_node = result;
        break;
    }
    case VT_OP_ARRAY_LENGTH: {
        result = vtx_node_create(nt, VTX_OP_LoadField);
        if (result == VTX_NODEID_INVALID) return -1;
        vtx_node_t *n = vtx_node_get(nt, result);
        n->field_offset = 0; /* length is at offset 0 in array header */
        vtx_node_add_input(nt, result, block->memory_node);
        block->memory_node = result;
        n->type = VTX_TYPE_Int;
        break;
    }

    /* ---- Exceptions ---- */
    case VT_OP_THROW: {
        vtx_nodeid_t unwind = vtx_node_create(nt, VTX_OP_Unwind);
        if (unwind == VTX_NODEID_INVALID) return -1;
        vtx_node_add_input(nt, unwind, block->control_node);
        block->control_node = unwind;
        result = unwind;
        break;
    }
    case VT_OP_CATCH: {
        vtx_nodeid_t catch_node = vtx_node_create(nt, VTX_OP_Catch);
        if (catch_node == VTX_NODEID_INVALID) return -1;
        vtx_node_add_input(nt, catch_node, block->control_node);
        block->control_node = catch_node;
        result = catch_node;
        break;
    }

    /* ---- Monitors ---- */
    case VT_OP_MONITOR_ENTER:
    case VT_OP_MONITOR_EXIT: {
        result = vtx_node_create(nt, VTX_OP_CallRuntime);
        if (result == VTX_NODEID_INVALID) return -1;
        vtx_node_add_input(nt, result, block->control_node);
        vtx_node_add_input(nt, result, block->memory_node);
        block->control_node = result;
        block->memory_node = result;
        break;
    }

    /* ---- Stack manipulation ---- */
    case VT_OP_DUP:
    case VT_OP_POP:
    case VT_OP_SWAP:
        /* Handled by the stack simulation */
        break;

    /* ---- Type queries ---- */
    case VT_OP_ISNULL: {
        result = vtx_node_create(nt, VTX_OP_CmpP);
        if (result == VTX_NODEID_INVALID) return -1;
        vtx_node_t *n = vtx_node_get(nt, result);
        n->cond = VTX_COND_EQ;
        break;
    }
    case VT_OP_TYPEOF: {
        result = vtx_node_create(nt, VTX_OP_InstanceOf);
        if (result == VTX_NODEID_INVALID) return -1;
        break;
    }

    case VT_OP_HALT:
    case VT_OP_NOP:
        result = VTX_NODEID_INVALID;
        break;

    default:
        result = VTX_NODEID_INVALID;
        break;
    }

    /* The 'result' will be pushed onto the simulated operand stack
     * by the caller. See vtx_graph_build. */
    (void)result;
    return 0;
}
#endif /* 0 — process_instruction */

/* ========================================================================== */
/* Main build function                                                         */
/* ========================================================================== */

int vtx_graph_build(vtx_graph_t *graph,
                    const vtx_bytecode_t *bytecode,
                    const vtx_method_desc_t *method,
                    vtx_arena_t *arena)
{
    VTX_ASSERT(graph != NULL, "graph must not be NULL");
    VTX_ASSERT(bytecode != NULL, "bytecode must not be NULL");
    VTX_ASSERT(arena != NULL, "arena must not be NULL");

    uint16_t max_locals = bytecode->max_locals;
    uint16_t max_stack = bytecode->max_stack;
    (void)method; /* used for type resolution in more advanced builders */

    /* Phase 1: Identify basic blocks */
    vtx_block_info_t *blocks = NULL;
    uint32_t nblocks = 0;
    if (identify_blocks(arena, bytecode, &blocks, &nblocks) != 0 || blocks == NULL) {
        return -1;
    }

    graph->blocks = blocks;
    graph->block_count = nblocks;
    graph->block_capacity = nblocks;

    /* Phase 2: Create Region/LoopBegin nodes and Phi nodes */
    for (uint32_t i = 0; i < nblocks; i++) {
        if (create_block_entry(graph, blocks, i, max_locals, arena) != 0) {
            return -1;
        }
    }

    /* Phase 3: Walk each block's instructions, emitting SoN data and control nodes.
     * We simulate the operand stack per block using a simple array.
     *
     * CRITICAL FIX: We propagate operand-stack values across block boundaries.
     * Previously, each block started with sp=0 (empty stack), which meant
     * values left on the stack at a branch were lost. This caused stack-underflow
     * errors in successor blocks that expected those values (e.g., RETURN_VALUE
     * at a merge point where both predecessors left one value on the stack).
     *
     * The fix:
     *   - At block exit, we record the operand stack state (exit_stack, exit_sp).
     *   - At block entry, we initialize the operand stack from predecessor exit
     *     states. For merge points (multiple predecessors), we create Phi nodes
     *     for each stack slot, just like we do for local variables.
     *   - This ensures the SSA invariant: every use of a value is dominated by
     *     its definition, even across control-flow merges. */
    vtx_nodeid_t *op_stack = (vtx_nodeid_t *)vtx_arena_alloc(
        arena, (max_stack + 2) * sizeof(vtx_nodeid_t));
    if (op_stack == NULL) return -1;

    /* Pre-allocate exit_stack for every block */
    for (uint32_t bi = 0; bi < nblocks; bi++) {
        blocks[bi].exit_stack = (vtx_nodeid_t *)vtx_arena_alloc(
            arena, (max_stack + 2) * sizeof(vtx_nodeid_t));
        if (blocks[bi].exit_stack == NULL) return -1;
        memset(blocks[bi].exit_stack, 0, (max_stack + 2) * sizeof(vtx_nodeid_t));
        blocks[bi].exit_sp = 0;
    }

    for (uint32_t bi = 0; bi < nblocks; bi++) {
        vtx_block_info_t *block = &blocks[bi];

        /* Skip unreachable blocks — they have no Region, no control, no
         * instructions to process. Leaving them out of Phase 3 means
         * they don't contribute to the operand stack or memory chain. */
        if (block->is_unreachable) {
            continue;
        }

        /* BUGFIX (if-then-else crash): Re-inherit locals from the predecessor's
         * EXIT state (not the INITIAL state from Phase 2).
         *
         * Phase 2 set block->locals[i] = blocks[pred_idx].locals[i] using the
         * predecessor's INITIAL locals (before Phase 3 ran). But by the time
         * Phase 3 processes this block, the predecessor has already been
         * processed (blocks are processed in order), and its locals reflect
         * the EXIT state (after store_local instructions).
         *
         * Without this re-inheritance, load_local in this block reads stale
         * initial values (e.g., void constants) instead of the updated values
         * (e.g., SMI(0) from store_local). This caused the Add node to use
         * the void constant instead of the stored value, producing wrong
         * results and crashes.
         *
         * Only applies to non-loop, non-entry blocks with exactly one forward
         * predecessor. Loop headers have their own Phi-based handling, and
         * merge blocks with multiple predecessors have Phis created in Phase 2. */
        if (bi > 0 && !block->is_loop_header && block->locals != NULL) {
            /* Count forward predecessors */
            uint32_t forward_pred_count = 0;
            uint32_t single_pred_idx = 0;
            for (uint32_t p = 0; p < block->pred_count; p++) {
                uint32_t pred_idx = block->pred_indices[p];
                if (blocks[pred_idx].is_loop_end) continue;
                if (blocks[pred_idx].is_unreachable) continue;
                forward_pred_count++;
                single_pred_idx = pred_idx;
            }
            /* If exactly one forward predecessor, re-inherit its EXIT locals.
             *
             * BUGFIX: DON'T re-inherit from a loop header — blocks inside the
             * loop body should use the Phis, and the latch/exit blocks get
             * their values from Phase 4's back-edge fix and the Region Phi
             * creation respectively. */
            if (forward_pred_count == 1 &&
                !blocks[single_pred_idx].is_loop_header &&
                blocks[single_pred_idx].locals != NULL) {
                for (uint16_t i = 0; i < max_locals; i++) {
                    /* Only update if the predecessor's exit local is valid
                     * and different from what we have. Don't overwrite locals
                     * that this block's Phase 3 has already updated (but Phase 3
                     * hasn't run for this block yet, so this is safe). */
                    vtx_nodeid_t exit_val = blocks[single_pred_idx].locals[i];
                    if (exit_val != VTX_NODEID_INVALID) {
                        block->locals[i] = exit_val;
                    }
                }
            }
        }

        int32_t sp = 0; /* stack pointer (index into op_stack) */

        /* If this is a catch handler, the exception is on the stack */
        if (block->is_catch_handler && block->region_node != VTX_NODEID_INVALID) {
            /* The Catch node produces the exception object */
            op_stack[sp++] = block->control_node;
        }

        /* Initialize operand stack from predecessor exit states.
         *
         * For non-entry blocks with predecessors, the operand stack at block
         * entry should match the exit stack of predecessors. At merge points
         * (multiple predecessors), we create Phi nodes for each stack slot.
         * This is the same approach used for local variables. */
        if (bi > 0 && block->pred_count > 0 && !block->is_catch_handler) {
            /* Count forward (non-back-edge) predecessors that have been
             * processed (have exit_sp > 0 or are the entry block). */
            uint32_t ready_pred_count = 0;
            int32_t expected_sp = -1;
            for (uint32_t p = 0; p < block->pred_count; p++) {
                uint32_t pred_idx = block->pred_indices[p];
                /* Skip back-edge predecessors for loop headers (not processed yet) */
                if (block->is_loop_header && blocks[pred_idx].is_loop_end) {
                    continue;
                }
                /* Skip unreachable predecessors — they have no exit stack. */
                if (blocks[pred_idx].is_unreachable) {
                    continue;
                }
                /* Skip predecessors that haven't been processed yet
                 * (exit_sp == 0 and not the entry block) */
                if (blocks[pred_idx].exit_sp == 0 && pred_idx > 0) {
                    continue;
                }
                if (expected_sp < 0) {
                    expected_sp = blocks[pred_idx].exit_sp;
                }
                ready_pred_count++;
            }

            if (ready_pred_count == 1 && expected_sp > 0) {
                /* Single ready predecessor: inherit its exit stack directly */
                for (uint32_t p = 0; p < block->pred_count; p++) {
                    uint32_t pred_idx = block->pred_indices[p];
                    if (block->is_loop_header && blocks[pred_idx].is_loop_end) continue;
                    if (blocks[pred_idx].is_unreachable) continue;
                    if (blocks[pred_idx].exit_sp == 0 && pred_idx > 0) continue;
                    for (int32_t si = 0; si < blocks[pred_idx].exit_sp; si++) {
                        op_stack[si] = blocks[pred_idx].exit_stack[si];
                    }
                    sp = blocks[pred_idx].exit_sp;
                    break;
                }
            } else if (ready_pred_count > 1 && expected_sp > 0) {
                /* Multiple ready predecessors: create Phi nodes for each stack slot */
                for (int32_t si = 0; si < expected_sp; si++) {
                    /* Check if all ready predecessors agree on this stack slot */
                    bool all_same = true;
                    vtx_nodeid_t first_val = VTX_NODEID_INVALID;
                    for (uint32_t p = 0; p < block->pred_count; p++) {
                        uint32_t pred_idx = block->pred_indices[p];
                        if (block->is_loop_header && blocks[pred_idx].is_loop_end) continue;
                        if (blocks[pred_idx].is_unreachable) continue;
                        if (blocks[pred_idx].exit_sp == 0 && pred_idx > 0) continue;
                        vtx_nodeid_t val = blocks[pred_idx].exit_stack[si];
                        if (first_val == VTX_NODEID_INVALID) {
                            first_val = val;
                        } else if (val != first_val) {
                            all_same = false;
                            break;
                        }
                    }

                    if (all_same) {
                        op_stack[si] = first_val;
                    } else {
                        /* Create a Phi node for this stack slot */
                        vtx_nodeid_t phi = vtx_node_create(&graph->node_table, VTX_OP_Phi);
                        if (phi == VTX_NODEID_INVALID) return -1;
                        /* Add one input per ready predecessor */
                        for (uint32_t p = 0; p < block->pred_count; p++) {
                            uint32_t pred_idx = block->pred_indices[p];
                            if (block->is_loop_header && blocks[pred_idx].is_loop_end) continue;
                            if (blocks[pred_idx].is_unreachable) continue;
                            if (blocks[pred_idx].exit_sp == 0 && pred_idx > 0) continue;
                            vtx_nodeid_t val = blocks[pred_idx].exit_stack[si];
                            vtx_node_add_input(&graph->node_table, phi, val);
                        }
                        /* Phi depends on Region for control */
                        if (block->region_node != VTX_NODEID_INVALID) {
                            vtx_node_add_input(&graph->node_table, phi, block->region_node);
                        }
                        op_stack[si] = phi;
                    }
                }
                sp = expected_sp;
            }
            /* If no ready predecessors or expected_sp == 0, stack stays empty.
             * This is correct for unreachable blocks or blocks where no
             * predecessor leaves values on the stack. */
        }

        size_t pc = block->start_pc;
        while (pc < block->end_pc) {
            vtx_opcode_t op = vtx_bytecode_opcode_at(bytecode, pc);
            const vtx_opcode_info_t *info = &vtx_opcode_table[op];
            size_t insn_len = vtx_bytecode_insn_length(bytecode, pc);
            uint16_t operand = 0;
            if (info->has_operand) {
                operand = vtx_bytecode_read_operand(bytecode, pc);
            }

            /* Process the instruction */
            vtx_nodeid_t result = VTX_NODEID_INVALID;

            switch (op) {
            /* ---- Constants ---- */
            case VT_OP_LOAD_CONST_INT: {
                result = vtx_node_create(&graph->node_table, VTX_OP_Constant);
                if (result == VTX_NODEID_INVALID) return -1;
                vtx_node_t *n = vtx_node_get(&graph->node_table, result);
                vtx_value_t cv = bytecode->constant_pool[operand];
                if (vtx_is_smi(cv)) {
                    n->constval = vtx_constval_int(vtx_smi_value(cv));
                    n->type = VTX_TYPE_Int;
                } else if (vtx_is_double(cv)) {
                    n->constval = vtx_constval_float(vtx_double_value(cv));
                    n->type = VTX_TYPE_Float;
                } else {
                    n->constval = vtx_constval_int((int64_t)operand);
                    n->type = VTX_TYPE_Int;
                }
                op_stack[sp++] = result;
                break;
            }
            case VT_OP_LOAD_CONST_FLOAT: {
                result = vtx_node_create(&graph->node_table, VTX_OP_Constant);
                if (result == VTX_NODEID_INVALID) return -1;
                vtx_node_t *n = vtx_node_get(&graph->node_table, result);
                vtx_value_t cv = bytecode->constant_pool[operand];
                n->constval = vtx_constval_float(vtx_double_value(cv));
                n->type = VTX_TYPE_Float;
                op_stack[sp++] = result;
                break;
            }
            case VT_OP_LOAD_CONST_STR: {
                result = vtx_node_create(&graph->node_table, VTX_OP_Constant);
                if (result == VTX_NODEID_INVALID) return -1;
                vtx_node_t *n = vtx_node_get(&graph->node_table, result);
                vtx_value_t cv = bytecode->constant_pool[operand];
                n->constval = vtx_constval_ptr(vtx_heap_ptr(cv));
                n->type = VTX_TYPE_Ptr;
                op_stack[sp++] = result;
                break;
            }
            case VT_OP_LOAD_NULL: {
                result = vtx_node_create(&graph->node_table, VTX_OP_Constant);
                if (result == VTX_NODEID_INVALID) return -1;
                vtx_node_t *n = vtx_node_get(&graph->node_table, result);
                n->constval = vtx_constval_ptr(NULL);
                n->type = VTX_TYPE_Ptr;
                op_stack[sp++] = result;
                break;
            }
            case VT_OP_LOAD_TRUE: {
                result = vtx_node_create(&graph->node_table, VTX_OP_Constant);
                if (result == VTX_NODEID_INVALID) return -1;
                vtx_node_t *n = vtx_node_get(&graph->node_table, result);
                n->constval = vtx_constval_int(1);
                n->type = VTX_TYPE_Int;
                op_stack[sp++] = result;
                break;
            }
            case VT_OP_LOAD_FALSE: {
                result = vtx_node_create(&graph->node_table, VTX_OP_Constant);
                if (result == VTX_NODEID_INVALID) return -1;
                vtx_node_t *n = vtx_node_get(&graph->node_table, result);
                n->constval = vtx_constval_int(0);
                n->type = VTX_TYPE_Int;
                op_stack[sp++] = result;
                break;
            }
            case VT_OP_LOAD_UNDEFINED: {
                result = vtx_node_create(&graph->node_table, VTX_OP_Constant);
                if (result == VTX_NODEID_INVALID) return -1;
                vtx_node_t *n = vtx_node_get(&graph->node_table, result);
                n->constval = vtx_constval_void();
                n->type = VTX_TYPE_Void;
                op_stack[sp++] = result;
                break;
            }

            /* ---- Local variables ---- */
            case VT_OP_LOAD_LOCAL: {
                if (operand >= max_locals) return -1; /* local index out of range */
                op_stack[sp++] = block->locals[operand];
                break;
            }
            case VT_OP_STORE_LOCAL: {
                if (operand >= max_locals) return -1; /* local index out of range */
                if (sp < 1) return -1; /* stack underflow: STORE_LOCAL */
                sp--;
                block->locals[operand] = op_stack[sp];
                break;
            }

            /* ---- Field access ---- */
            case VT_OP_LOAD_FIELD: {
                if (sp < 1) return -1; /* stack underflow: LOAD_FIELD */
                vtx_nodeid_t obj = op_stack[--sp];

                /* F3: Emit null-check Guard before field load.
                 * A field load on a null reference must throw NPE.
                 * We create a CmpP(obj, null) and Guard(NE) to guard against null. */
                {
                    vtx_nodeid_t null_const = vtx_node_create(&graph->node_table, VTX_OP_Constant);
                    if (null_const == VTX_NODEID_INVALID) return -1;
                    vtx_node_t *nc = vtx_node_get(&graph->node_table, null_const);
                    nc->constval = vtx_constval_ptr(NULL);
                    nc->type = VTX_TYPE_Ptr;

                    vtx_nodeid_t null_cmp = vtx_node_create(&graph->node_table, VTX_OP_CmpP);
                    if (null_cmp == VTX_NODEID_INVALID) return -1;
                    vtx_node_t *cmp_n = vtx_node_get(&graph->node_table, null_cmp);
                    cmp_n->cond = VTX_COND_NE; /* obj != null */
                    vtx_node_add_input(&graph->node_table, null_cmp, obj);
                    vtx_node_add_input(&graph->node_table, null_cmp, null_const);

                    vtx_nodeid_t guard = emit_guard(graph, block, null_cmp, VTX_COND_NE,
                                                     max_locals, pc);
                    if (guard == VTX_NODEID_INVALID) return -1;
                }

                result = vtx_node_create(&graph->node_table, VTX_OP_LoadField);
                if (result == VTX_NODEID_INVALID) return -1;
                vtx_node_t *n = vtx_node_get(&graph->node_table, result);
                n->field_offset = operand;
                n->type = VTX_TYPE_Bottom;
                vtx_node_add_input(&graph->node_table, result, block->memory_node);
                vtx_node_add_input(&graph->node_table, result, obj);
                block->memory_node = result;
                op_stack[sp++] = result;
                /* Exception edge: LoadField can throw NPE */
                if (emit_exception_edge(graph, block, result) != 0) return -1;
                break;
            }
            case VT_OP_STORE_FIELD: {
                if (sp < 2) return -1; /* stack underflow: STORE_FIELD */
                vtx_nodeid_t val = op_stack[--sp];
                vtx_nodeid_t obj = op_stack[--sp];

                /* F3: Emit null-check Guard before field store */
                {
                    vtx_nodeid_t null_const = vtx_node_create(&graph->node_table, VTX_OP_Constant);
                    if (null_const == VTX_NODEID_INVALID) return -1;
                    vtx_node_t *nc = vtx_node_get(&graph->node_table, null_const);
                    nc->constval = vtx_constval_ptr(NULL);
                    nc->type = VTX_TYPE_Ptr;

                    vtx_nodeid_t null_cmp = vtx_node_create(&graph->node_table, VTX_OP_CmpP);
                    if (null_cmp == VTX_NODEID_INVALID) return -1;
                    vtx_node_t *cmp_n = vtx_node_get(&graph->node_table, null_cmp);
                    cmp_n->cond = VTX_COND_NE;
                    vtx_node_add_input(&graph->node_table, null_cmp, obj);
                    vtx_node_add_input(&graph->node_table, null_cmp, null_const);

                    vtx_nodeid_t guard = emit_guard(graph, block, null_cmp, VTX_COND_NE,
                                                     max_locals, pc);
                    if (guard == VTX_NODEID_INVALID) return -1;
                }

                result = vtx_node_create(&graph->node_table, VTX_OP_StoreField);
                if (result == VTX_NODEID_INVALID) return -1;
                vtx_node_t *n = vtx_node_get(&graph->node_table, result);
                n->field_offset = operand;
                vtx_node_add_input(&graph->node_table, result, block->memory_node);
                vtx_node_add_input(&graph->node_table, result, obj);
                vtx_node_add_input(&graph->node_table, result, val);
                block->memory_node = result;
                /* Exception edge: StoreField can throw NPE */
                if (emit_exception_edge(graph, block, result) != 0) return -1;
                break;
            }

            /* ---- Integer arithmetic (pop 2, push 1) ---- */
            case VT_OP_IADD: case VT_OP_ISUB: case VT_OP_IMUL:
            case VT_OP_IDIV: case VT_OP_IMOD: {
                if (sp < 2) return -1; /* stack underflow: binary arith */
                vtx_nodeid_t b = op_stack[--sp];
                vtx_nodeid_t a = op_stack[--sp];
                vtx_node_opcode_t ir_op;
                switch (op) {
                case VT_OP_IADD: ir_op = VTX_OP_Add; break;
                case VT_OP_ISUB: ir_op = VTX_OP_Sub; break;
                case VT_OP_IMUL: ir_op = VTX_OP_Mul; break;
                case VT_OP_IDIV: ir_op = VTX_OP_Div; break;
                case VT_OP_IMOD: ir_op = VTX_OP_Mod; break;
                default: ir_op = VTX_OP_Add; break;
                }
                result = vtx_node_create(&graph->node_table, ir_op);
                if (result == VTX_NODEID_INVALID) return -1;
                vtx_node_add_input(&graph->node_table, result, a);
                vtx_node_add_input(&graph->node_table, result, b);
                /* BUGFIX: Mark Div/Mod as SIDE_EFFECT so the scheduler
                 * doesn't place them before guard checks. Also mark as
                 * PINNED so the scheduler treats them like control nodes
                 * and places them in the current block (not hoisted to
                 * a predecessor). Without PINNED, the scheduler's LCA
                 * computation may place Div/Mod in a block before the
                 * if_false guard, causing divide-by-zero. */
                if (ir_op == VTX_OP_Div || ir_op == VTX_OP_Mod) {
                    vtx_node_get(&graph->node_table, result)->flags |=
                        VTX_NF_SIDE_EFFECT | VTX_NF_PINNED;
                }
                op_stack[sp++] = result;
                /* Exception edge: IDIV/IMOD can throw ArithmeticException */
                if (ir_op == VTX_OP_Div || ir_op == VTX_OP_Mod) {
                    if (emit_exception_edge(graph, block, result) != 0) return -1;
                }
                break;
            }

            /* ---- Float arithmetic ---- */
            case VT_OP_FADD: case VT_OP_FSUB: case VT_OP_FMUL: case VT_OP_FDIV: {
                if (sp < 2) return -1; /* stack underflow: float arith */
                vtx_nodeid_t b = op_stack[--sp];
                vtx_nodeid_t a = op_stack[--sp];
                vtx_node_opcode_t ir_op;
                switch (op) {
                case VT_OP_FADD: ir_op = VTX_OP_Add; break;
                case VT_OP_FSUB: ir_op = VTX_OP_Sub; break;
                case VT_OP_FMUL: ir_op = VTX_OP_Mul; break;
                case VT_OP_FDIV: ir_op = VTX_OP_Div; break;
                default: ir_op = VTX_OP_Add; break;
                }
                result = vtx_node_create(&graph->node_table, ir_op);
                if (result == VTX_NODEID_INVALID) return -1;
                vtx_node_t *n = vtx_node_get(&graph->node_table, result);
                n->type = VTX_TYPE_Float;
                vtx_node_add_input(&graph->node_table, result, a);
                vtx_node_add_input(&graph->node_table, result, b);
                op_stack[sp++] = result;
                /* Exception edge: FDIV can throw (e.g. floating-point exception) */
                if (ir_op == VTX_OP_Div) {
                    if (emit_exception_edge(graph, block, result) != 0) return -1;
                }
                break;
            }

            /* ---- Bitwise / unary ---- */
            case VT_OP_ISHL: case VT_OP_ISHR:
            case VT_OP_IAND: case VT_OP_IOR: case VT_OP_IXOR: {
                if (sp < 2) return -1; /* stack underflow: bitwise */
                vtx_nodeid_t b_val = op_stack[--sp];
                vtx_nodeid_t a_val = op_stack[--sp];
                vtx_node_opcode_t ir_op;
                switch (op) {
                case VT_OP_ISHL: ir_op = VTX_OP_Shl; break;
                case VT_OP_ISHR: ir_op = VTX_OP_Shr; break;
                case VT_OP_IAND: ir_op = VTX_OP_And; break;
                case VT_OP_IOR:  ir_op = VTX_OP_Or;  break;
                case VT_OP_IXOR: ir_op = VTX_OP_Xor; break;
                default: ir_op = VTX_OP_And; break;
                }
                result = vtx_node_create(&graph->node_table, ir_op);
                if (result == VTX_NODEID_INVALID) return -1;
                vtx_node_add_input(&graph->node_table, result, a_val);
                vtx_node_add_input(&graph->node_table, result, b_val);
                op_stack[sp++] = result;
                break;
            }
            case VT_OP_INEG: {
                if (sp < 1) return -1; /* stack underflow: INEG */
                vtx_nodeid_t v = op_stack[--sp];
                result = vtx_node_create(&graph->node_table, VTX_OP_Neg);
                if (result == VTX_NODEID_INVALID) return -1;
                vtx_node_add_input(&graph->node_table, result, v);
                op_stack[sp++] = result;
                break;
            }
            case VT_OP_INOT: {
                if (sp < 1) return -1; /* stack underflow: INOT */
                vtx_nodeid_t v = op_stack[--sp];
                result = vtx_node_create(&graph->node_table, VTX_OP_Not);
                if (result == VTX_NODEID_INVALID) return -1;
                vtx_node_add_input(&graph->node_table, result, v);
                op_stack[sp++] = result;
                break;
            }

            /* ---- Integer comparisons ---- */
            case VT_OP_ICMP_EQ: case VT_OP_ICMP_NE:
            case VT_OP_ICMP_LT: case VT_OP_ICMP_LE:
            case VT_OP_ICMP_GT: case VT_OP_ICMP_GE: {
                if (sp < 2) return -1; /* stack underflow: comparison */
                vtx_nodeid_t b_val = op_stack[--sp];
                vtx_nodeid_t a_val = op_stack[--sp];
                result = vtx_node_create(&graph->node_table, VTX_OP_Cmp);
                if (result == VTX_NODEID_INVALID) return -1;
                vtx_node_t *n = vtx_node_get(&graph->node_table, result);
                vtx_cond_t cond_map[] = {
                    VTX_COND_EQ, VTX_COND_NE, VTX_COND_LT,
                    VTX_COND_LE, VTX_COND_GT, VTX_COND_GE
                };
                n->cond = cond_map[op - VT_OP_ICMP_EQ];
                vtx_node_add_input(&graph->node_table, result, a_val);
                vtx_node_add_input(&graph->node_table, result, b_val);
                op_stack[sp++] = result;
                break;
            }

            /* ---- Float comparisons ---- */
            case VT_OP_FCMP_EQ: case VT_OP_FCMP_NE:
            case VT_OP_FCMP_LT: case VT_OP_FCMP_LE:
            case VT_OP_FCMP_GT: case VT_OP_FCMP_GE: {
                if (sp < 2) return -1; /* stack underflow: float comparison */
                vtx_nodeid_t b_val = op_stack[--sp];
                vtx_nodeid_t a_val = op_stack[--sp];
                result = vtx_node_create(&graph->node_table, VTX_OP_CmpF);
                if (result == VTX_NODEID_INVALID) return -1;
                vtx_node_t *n = vtx_node_get(&graph->node_table, result);
                vtx_cond_t cond_map[] = {
                    VTX_COND_EQ, VTX_COND_NE, VTX_COND_LT,
                    VTX_COND_LE, VTX_COND_GT, VTX_COND_GE
                };
                n->cond = cond_map[op - VT_OP_FCMP_EQ];
                vtx_node_add_input(&graph->node_table, result, a_val);
                vtx_node_add_input(&graph->node_table, result, b_val);
                op_stack[sp++] = result;
                break;
            }

            /* ---- Control flow ---- */
            case VT_OP_GOTO: {
                vtx_nodeid_t goto_n = vtx_node_create(&graph->node_table, VTX_OP_Goto);
                if (goto_n == VTX_NODEID_INVALID) return -1;
                vtx_node_add_input(&graph->node_table, goto_n, block->control_node);
                block->control_node = goto_n;
                break;
            }
            case VT_OP_IF_TRUE:
            case VT_OP_IF_FALSE: {
                if (sp < 1) return -1; /* stack underflow: IF */
                vtx_nodeid_t cond = op_stack[--sp];
                vtx_nodeid_t if_n = vtx_node_create(&graph->node_table, VTX_OP_If);
                if (if_n == VTX_NODEID_INVALID) return -1;
                vtx_node_t *n = vtx_node_get(&graph->node_table, if_n);
                n->cond = (op == VT_OP_IF_TRUE) ? VTX_COND_NE : VTX_COND_EQ;
                vtx_node_add_input(&graph->node_table, if_n, block->control_node);
                vtx_node_add_input(&graph->node_table, if_n, cond);
                block->control_node = if_n;
                break;
            }

            /* ---- Returns ---- */
            case VT_OP_RETURN: {
                vtx_nodeid_t ret = vtx_node_create(&graph->node_table, VTX_OP_Return);
                if (ret == VTX_NODEID_INVALID) return -1;
                vtx_node_add_input(&graph->node_table, ret, block->control_node);
                block->control_node = ret;
                break;
            }
            case VT_OP_RETURN_VALUE: {
                if (sp < 1) return -1; /* stack underflow: RETURN_VALUE */
                vtx_nodeid_t val = op_stack[--sp];
                vtx_nodeid_t ret = vtx_node_create(&graph->node_table, VTX_OP_Return);
                if (ret == VTX_NODEID_INVALID) return -1;
                vtx_node_add_input(&graph->node_table, ret, block->control_node);
                vtx_node_add_input(&graph->node_table, ret, val);
                block->control_node = ret;
                break;
            }

            /* ---- Calls ---- */
            case VT_OP_CALL_STATIC: {
                /* IR-5 fix: consume arguments from the operand stack.
                 * Stack: arg0, arg1, ..., argN → result
                 * The operand is the method index; arg count comes from
                 * the method descriptor. If no descriptor is available,
                 * we don't consume any args (conservative fallback). */

                /* F3: Emit FrameState before call (deopt point) */
                {
                    vtx_nodeid_t fs = emit_frame_state(graph, block, max_locals, pc);
                    if (fs == VTX_NODEID_INVALID) return -1;
                    /* The FrameState is attached to the call for deopt */
                }

                uint32_t call_arg_count = 0;
                if (method != NULL) {
                    /* Use precomputed arg_count when available;
                     * fall back to parsing signature for methods
                     * not registered with the type system. */
                    if (method->arg_count > 0) {
                        call_arg_count = method->arg_count;
                    } else if (method->signature != NULL) {
                        call_arg_count = vtx_graph_count_method_args(method->signature);
                    }
                }
                /* Pop arguments from stack (reverse order) */
                vtx_nodeid_t call_args[16];
                if (call_arg_count > 16) call_arg_count = 16;
                if (call_arg_count > (uint32_t)sp) call_arg_count = (uint32_t)sp;
                for (uint32_t i = 0; i < call_arg_count; i++) {
                    call_args[call_arg_count - 1 - i] = op_stack[--sp];
                }
                vtx_nodeid_t call = vtx_node_create(&graph->node_table, VTX_OP_CallStatic);
                if (call == VTX_NODEID_INVALID) return -1;
                vtx_node_t *n = vtx_node_get(&graph->node_table, call);
                n->method_index = operand;
                n->bytecode_pc = (uint32_t)pc;
                vtx_node_add_input(&graph->node_table, call, block->control_node);
                vtx_node_add_input(&graph->node_table, call, block->memory_node);
                /* Add arguments as inputs */
                for (uint32_t i = 0; i < call_arg_count; i++) {
                    vtx_node_add_input(&graph->node_table, call, call_args[i]);
                }
                block->control_node = call;
                block->memory_node = call;
                op_stack[sp++] = call;
                /* Exception edge: CallStatic can throw */
                if (emit_exception_edge(graph, block, call) != 0) return -1;
                break;
            }
            case VT_OP_CALL_VIRTUAL: {
                if (sp < 1) return -1; /* stack underflow: CALL_VIRTUAL (need receiver) */
                vtx_nodeid_t receiver = op_stack[--sp];

                /* F3: Emit null-check Guard on receiver + FrameState before call */
                {
                    vtx_nodeid_t null_const = vtx_node_create(&graph->node_table, VTX_OP_Constant);
                    if (null_const == VTX_NODEID_INVALID) return -1;
                    vtx_node_t *nc = vtx_node_get(&graph->node_table, null_const);
                    nc->constval = vtx_constval_ptr(NULL);
                    nc->type = VTX_TYPE_Ptr;

                    vtx_nodeid_t null_cmp = vtx_node_create(&graph->node_table, VTX_OP_CmpP);
                    if (null_cmp == VTX_NODEID_INVALID) return -1;
                    vtx_node_t *cmp_n = vtx_node_get(&graph->node_table, null_cmp);
                    cmp_n->cond = VTX_COND_NE;
                    vtx_node_add_input(&graph->node_table, null_cmp, receiver);
                    vtx_node_add_input(&graph->node_table, null_cmp, null_const);

                    vtx_nodeid_t guard = emit_guard(graph, block, null_cmp, VTX_COND_NE,
                                                     max_locals, pc);
                    if (guard == VTX_NODEID_INVALID) return -1;
                }

                vtx_nodeid_t call = vtx_node_create(&graph->node_table, VTX_OP_CallVirtual);
                if (call == VTX_NODEID_INVALID) return -1;
                vtx_node_t *n = vtx_node_get(&graph->node_table, call);
                n->method_index = operand;
                n->bytecode_pc = (uint32_t)pc;
                vtx_node_add_input(&graph->node_table, call, block->control_node);
                vtx_node_add_input(&graph->node_table, call, block->memory_node);
                vtx_node_add_input(&graph->node_table, call, receiver);
                block->control_node = call;
                block->memory_node = call;
                op_stack[sp++] = call;
                /* Exception edge: CallVirtual can throw */
                if (emit_exception_edge(graph, block, call) != 0) return -1;
                break;
            }
            case VT_OP_CALL_INTERFACE: {
                if (sp < 1) return -1; /* stack underflow: CALL_INTERFACE (need receiver) */
                vtx_nodeid_t receiver = op_stack[--sp];

                /* F3: Emit null-check Guard on receiver + FrameState before call */
                {
                    vtx_nodeid_t null_const = vtx_node_create(&graph->node_table, VTX_OP_Constant);
                    if (null_const == VTX_NODEID_INVALID) return -1;
                    vtx_node_t *nc = vtx_node_get(&graph->node_table, null_const);
                    nc->constval = vtx_constval_ptr(NULL);
                    nc->type = VTX_TYPE_Ptr;

                    vtx_nodeid_t null_cmp = vtx_node_create(&graph->node_table, VTX_OP_CmpP);
                    if (null_cmp == VTX_NODEID_INVALID) return -1;
                    vtx_node_t *cmp_n = vtx_node_get(&graph->node_table, null_cmp);
                    cmp_n->cond = VTX_COND_NE;
                    vtx_node_add_input(&graph->node_table, null_cmp, receiver);
                    vtx_node_add_input(&graph->node_table, null_cmp, null_const);

                    vtx_nodeid_t guard = emit_guard(graph, block, null_cmp, VTX_COND_NE,
                                                     max_locals, pc);
                    if (guard == VTX_NODEID_INVALID) return -1;
                }

                vtx_nodeid_t call = vtx_node_create(&graph->node_table, VTX_OP_CallInterface);
                if (call == VTX_NODEID_INVALID) return -1;
                vtx_node_t *n = vtx_node_get(&graph->node_table, call);
                n->method_index = operand;
                n->bytecode_pc = (uint32_t)pc;
                vtx_node_add_input(&graph->node_table, call, block->control_node);
                vtx_node_add_input(&graph->node_table, call, block->memory_node);
                vtx_node_add_input(&graph->node_table, call, receiver);
                block->control_node = call;
                block->memory_node = call;
                op_stack[sp++] = call;
                /* Exception edge: CallInterface can throw */
                if (emit_exception_edge(graph, block, call) != 0) return -1;
                break;
            }

            /* ---- Object creation ---- */
            case VT_OP_NEW: {
                result = vtx_node_create(&graph->node_table, VTX_OP_NewObject);
                if (result == VTX_NODEID_INVALID) return -1;
                vtx_node_t *n = vtx_node_get(&graph->node_table, result);
                n->type_id = operand;
                vtx_node_add_input(&graph->node_table, result, block->memory_node);
                block->memory_node = result;
                op_stack[sp++] = result;
                /* Exception edge: NewObject can throw OutOfMemoryError */
                if (emit_exception_edge(graph, block, result) != 0) return -1;
                break;
            }
            case VT_OP_NEWARRAY: {
                if (sp < 1) return -1; /* stack underflow: NEWARRAY (need size) */
                vtx_nodeid_t size = op_stack[--sp];
                result = vtx_node_create(&graph->node_table, VTX_OP_NewArray);
                if (result == VTX_NODEID_INVALID) return -1;
                vtx_node_t *n = vtx_node_get(&graph->node_table, result);
                n->type_id = operand;
                vtx_node_add_input(&graph->node_table, result, block->memory_node);
                vtx_node_add_input(&graph->node_table, result, size);
                block->memory_node = result;
                op_stack[sp++] = result;
                /* Exception edge: NewArray can throw OutOfMemoryError / NegativeArraySizeException */
                if (emit_exception_edge(graph, block, result) != 0) return -1;
                break;
            }

            /* ---- Type checks ---- */
            case VT_OP_CHECKCAST: {
                if (sp < 1) return -1; /* stack underflow: CHECKCAST */
                vtx_nodeid_t obj = op_stack[--sp];

                /* F3: Emit a type-check Guard before the CheckCast.
                 * The Guard checks that obj is an instance of the target type.
                 * If the guard fails, execution deoptimizes via the captured
                 * FrameState to the interpreter, which will throw ClassCastException.
                 * We also emit a null-check Guard — CheckCast null is always OK. */

                /* First, check obj != null (null passes CheckCast) */
                {
                    vtx_nodeid_t null_const = vtx_node_create(&graph->node_table, VTX_OP_Constant);
                    if (null_const == VTX_NODEID_INVALID) return -1;
                    vtx_node_t *nc = vtx_node_get(&graph->node_table, null_const);
                    nc->constval = vtx_constval_ptr(NULL);
                    nc->type = VTX_TYPE_Ptr;

                    vtx_nodeid_t null_cmp = vtx_node_create(&graph->node_table, VTX_OP_CmpP);
                    if (null_cmp == VTX_NODEID_INVALID) return -1;
                    vtx_node_t *cmp_n = vtx_node_get(&graph->node_table, null_cmp);
                    cmp_n->cond = VTX_COND_EQ; /* obj == null → pass through */
                    vtx_node_add_input(&graph->node_table, null_cmp, obj);
                    vtx_node_add_input(&graph->node_table, null_cmp, null_const);

                    /* Guard: if obj is NOT null, then we need the type check.
                     * We emit a DeoptGuard that deopts if the type check fails.
                     * The condition is: (obj != null) AND (obj instanceof type).
                     * For now, we emit a DeoptGuard for the type check. */
                    vtx_nodeid_t fs = emit_frame_state(graph, block, max_locals, pc);
                    if (fs == VTX_NODEID_INVALID) return -1;

                    vtx_nodeid_t deopt_guard = vtx_node_create(&graph->node_table, VTX_OP_DeoptGuard);
                    if (deopt_guard == VTX_NODEID_INVALID) return -1;
                    vtx_node_t *dg = vtx_node_get(&graph->node_table, deopt_guard);
                    dg->type_id = operand;
                    dg->cond = VTX_COND_NE;
                    dg->bytecode_pc = (uint32_t)pc;
                    dg->frame_state = fs;
                    vtx_node_add_input(&graph->node_table, deopt_guard, block->control_node);
                    vtx_node_add_input(&graph->node_table, deopt_guard, obj);
                    vtx_node_add_input(&graph->node_table, deopt_guard, fs);
                    block->control_node = deopt_guard;
                }

                result = vtx_node_create(&graph->node_table, VTX_OP_CheckCast);
                if (result == VTX_NODEID_INVALID) return -1;
                vtx_node_t *n = vtx_node_get(&graph->node_table, result);
                n->type_id = operand;
                vtx_node_add_input(&graph->node_table, result, obj);
                op_stack[sp++] = result;
                /* Exception edge: CheckCast can throw ClassCastException */
                if (emit_exception_edge(graph, block, result) != 0) return -1;
                break;
            }
            case VT_OP_INSTANCEOF: {
                if (sp < 1) return -1; /* stack underflow: INSTANCEOF */
                vtx_nodeid_t obj = op_stack[--sp];
                result = vtx_node_create(&graph->node_table, VTX_OP_InstanceOf);
                if (result == VTX_NODEID_INVALID) return -1;
                vtx_node_t *n = vtx_node_get(&graph->node_table, result);
                n->type_id = operand;
                vtx_node_add_input(&graph->node_table, result, obj);
                op_stack[sp++] = result;
                break;
            }

            /* ---- Array operations ---- */
            case VT_OP_ARRAY_LOAD: {
                if (sp < 2) return -1; /* stack underflow: ARRAY_LOAD */
                vtx_nodeid_t idx = op_stack[--sp];
                vtx_nodeid_t arr = op_stack[--sp];

                /* F3: Emit null-check Guard on array reference */
                {
                    vtx_nodeid_t null_const = vtx_node_create(&graph->node_table, VTX_OP_Constant);
                    if (null_const == VTX_NODEID_INVALID) return -1;
                    vtx_node_t *nc = vtx_node_get(&graph->node_table, null_const);
                    nc->constval = vtx_constval_ptr(NULL);
                    nc->type = VTX_TYPE_Ptr;

                    vtx_nodeid_t null_cmp = vtx_node_create(&graph->node_table, VTX_OP_CmpP);
                    if (null_cmp == VTX_NODEID_INVALID) return -1;
                    vtx_node_t *cmp_n = vtx_node_get(&graph->node_table, null_cmp);
                    cmp_n->cond = VTX_COND_NE;
                    vtx_node_add_input(&graph->node_table, null_cmp, arr);
                    vtx_node_add_input(&graph->node_table, null_cmp, null_const);

                    vtx_nodeid_t guard = emit_guard(graph, block, null_cmp, VTX_COND_NE,
                                                     max_locals, pc);
                    if (guard == VTX_NODEID_INVALID) return -1;
                }

                /* F3: Emit bounds-check Guard: idx >= 0 && idx < array_length */
                {
                    /* Get array length via LoadField(offset=0) */
                    vtx_nodeid_t len_load = vtx_node_create(&graph->node_table, VTX_OP_LoadField);
                    if (len_load == VTX_NODEID_INVALID) return -1;
                    vtx_node_t *ln = vtx_node_get(&graph->node_table, len_load);
                    ln->field_offset = 0; /* length at offset 0 in array header */
                    ln->type = VTX_TYPE_Int;
                    vtx_node_add_input(&graph->node_table, len_load, block->memory_node);
                    vtx_node_add_input(&graph->node_table, len_load, arr);
                    /* Note: we don't update memory_node here since this is a
                     * speculative read for the bounds check only */

                    /* Compare: idx < length (unsigned, so also covers idx >= 0) */
                    vtx_nodeid_t bounds_cmp = vtx_node_create(&graph->node_table, VTX_OP_Cmp);
                    if (bounds_cmp == VTX_NODEID_INVALID) return -1;
                    vtx_node_t *bc_n = vtx_node_get(&graph->node_table, bounds_cmp);
                    bc_n->cond = VTX_COND_ULT; /* unsigned less-than */
                    vtx_node_add_input(&graph->node_table, bounds_cmp, idx);
                    vtx_node_add_input(&graph->node_table, bounds_cmp, len_load);

                    vtx_nodeid_t guard = emit_guard(graph, block, bounds_cmp, VTX_COND_ULT,
                                                     max_locals, pc);
                    if (guard == VTX_NODEID_INVALID) return -1;
                }

                result = vtx_node_create(&graph->node_table, VTX_OP_LoadIndexed);
                if (result == VTX_NODEID_INVALID) return -1;
                vtx_node_add_input(&graph->node_table, result, block->memory_node);
                vtx_node_add_input(&graph->node_table, result, arr);
                vtx_node_add_input(&graph->node_table, result, idx);
                block->memory_node = result;
                op_stack[sp++] = result;
                /* Exception edge: LoadIndexed can throw ArrayIndexOutOfBoundsException */
                if (emit_exception_edge(graph, block, result) != 0) return -1;
                break;
            }
            case VT_OP_ARRAY_STORE: {
                if (sp < 3) return -1; /* stack underflow: ARRAY_STORE */
                vtx_nodeid_t val = op_stack[--sp];
                vtx_nodeid_t idx = op_stack[--sp];
                vtx_nodeid_t arr = op_stack[--sp];

                /* F3: Emit null-check Guard on array reference */
                {
                    vtx_nodeid_t null_const = vtx_node_create(&graph->node_table, VTX_OP_Constant);
                    if (null_const == VTX_NODEID_INVALID) return -1;
                    vtx_node_t *nc = vtx_node_get(&graph->node_table, null_const);
                    nc->constval = vtx_constval_ptr(NULL);
                    nc->type = VTX_TYPE_Ptr;

                    vtx_nodeid_t null_cmp = vtx_node_create(&graph->node_table, VTX_OP_CmpP);
                    if (null_cmp == VTX_NODEID_INVALID) return -1;
                    vtx_node_t *cmp_n = vtx_node_get(&graph->node_table, null_cmp);
                    cmp_n->cond = VTX_COND_NE;
                    vtx_node_add_input(&graph->node_table, null_cmp, arr);
                    vtx_node_add_input(&graph->node_table, null_cmp, null_const);

                    vtx_nodeid_t guard = emit_guard(graph, block, null_cmp, VTX_COND_NE,
                                                     max_locals, pc);
                    if (guard == VTX_NODEID_INVALID) return -1;
                }

                /* F3: Emit bounds-check Guard */
                {
                    vtx_nodeid_t len_load = vtx_node_create(&graph->node_table, VTX_OP_LoadField);
                    if (len_load == VTX_NODEID_INVALID) return -1;
                    vtx_node_t *ln = vtx_node_get(&graph->node_table, len_load);
                    ln->field_offset = 0;
                    ln->type = VTX_TYPE_Int;
                    vtx_node_add_input(&graph->node_table, len_load, block->memory_node);
                    vtx_node_add_input(&graph->node_table, len_load, arr);

                    vtx_nodeid_t bounds_cmp = vtx_node_create(&graph->node_table, VTX_OP_Cmp);
                    if (bounds_cmp == VTX_NODEID_INVALID) return -1;
                    vtx_node_t *bc_n = vtx_node_get(&graph->node_table, bounds_cmp);
                    bc_n->cond = VTX_COND_ULT;
                    vtx_node_add_input(&graph->node_table, bounds_cmp, idx);
                    vtx_node_add_input(&graph->node_table, bounds_cmp, len_load);

                    vtx_nodeid_t guard = emit_guard(graph, block, bounds_cmp, VTX_COND_ULT,
                                                     max_locals, pc);
                    if (guard == VTX_NODEID_INVALID) return -1;
                }

                result = vtx_node_create(&graph->node_table, VTX_OP_StoreIndexed);
                if (result == VTX_NODEID_INVALID) return -1;
                vtx_node_add_input(&graph->node_table, result, block->memory_node);
                vtx_node_add_input(&graph->node_table, result, arr);
                vtx_node_add_input(&graph->node_table, result, idx);
                vtx_node_add_input(&graph->node_table, result, val);
                block->memory_node = result;
                /* Exception edge: StoreIndexed can throw ArrayIndexOutOfBoundsException / ArrayStoreException */
                if (emit_exception_edge(graph, block, result) != 0) return -1;
                break;
            }
            case VT_OP_ARRAY_LENGTH: {
                if (sp < 1) return -1; /* stack underflow: ARRAY_LENGTH */
                vtx_nodeid_t arr = op_stack[--sp];
                result = vtx_node_create(&graph->node_table, VTX_OP_LoadField);
                if (result == VTX_NODEID_INVALID) return -1;
                vtx_node_t *n = vtx_node_get(&graph->node_table, result);
                n->field_offset = 0;
                n->type = VTX_TYPE_Int;
                vtx_node_add_input(&graph->node_table, result, block->memory_node);
                vtx_node_add_input(&graph->node_table, result, arr);
                block->memory_node = result;
                op_stack[sp++] = result;
                break;
            }

            /* ---- Exceptions ---- */
            case VT_OP_THROW: {
                if (sp < 1) return -1; /* stack underflow: THROW */
                vtx_nodeid_t exc = op_stack[--sp];
                vtx_nodeid_t unwind = vtx_node_create(&graph->node_table, VTX_OP_Unwind);
                if (unwind == VTX_NODEID_INVALID) return -1;
                vtx_node_add_input(&graph->node_table, unwind, block->control_node);
                vtx_node_add_input(&graph->node_table, unwind, exc);
                block->control_node = unwind;
                break;
            }
            case VT_OP_CATCH: {
                vtx_nodeid_t catch_n = vtx_node_create(&graph->node_table, VTX_OP_Catch);
                if (catch_n == VTX_NODEID_INVALID) return -1;
                vtx_node_add_input(&graph->node_table, catch_n, block->control_node);
                block->control_node = catch_n;
                /* Record this Catch node as the current exception target
                 * for subsequent potentially-throwing instructions in
                 * this block. */
                block->exception_target = catch_n;
                op_stack[sp++] = catch_n;
                break;
            }

            /* ---- Monitors ---- */
            case VT_OP_MONITOR_ENTER: {
                if (sp < 1) return -1; /* stack underflow: MONITOR_ENTER */
                vtx_nodeid_t obj = op_stack[--sp];
                vtx_nodeid_t rt = vtx_node_create(&graph->node_table, VTX_OP_CallRuntime);
                if (rt == VTX_NODEID_INVALID) return -1;
                vtx_node_add_input(&graph->node_table, rt, block->control_node);
                vtx_node_add_input(&graph->node_table, rt, block->memory_node);
                vtx_node_add_input(&graph->node_table, rt, obj);
                block->control_node = rt;
                block->memory_node = rt;
                /* Exception edge: MonitorEnter can throw */
                if (emit_exception_edge(graph, block, rt) != 0) return -1;
                break;
            }
            case VT_OP_MONITOR_EXIT: {
                if (sp < 1) return -1; /* stack underflow: MONITOR_EXIT */
                vtx_nodeid_t obj = op_stack[--sp];
                vtx_nodeid_t rt = vtx_node_create(&graph->node_table, VTX_OP_CallRuntime);
                if (rt == VTX_NODEID_INVALID) return -1;
                vtx_node_add_input(&graph->node_table, rt, block->control_node);
                vtx_node_add_input(&graph->node_table, rt, block->memory_node);
                vtx_node_add_input(&graph->node_table, rt, obj);
                block->control_node = rt;
                block->memory_node = rt;
                /* Exception edge: MonitorExit can throw IllegalMonitorStateException */
                if (emit_exception_edge(graph, block, rt) != 0) return -1;
                break;
            }

            /* ---- Stack manipulation ---- */
            case VT_OP_DUP: {
                if (sp < 1) return -1; /* stack underflow: DUP */
                op_stack[sp] = op_stack[sp - 1];
                sp++;
                break;
            }
            case VT_OP_POP: {
                if (sp < 1) return -1; /* stack underflow: POP */
                sp--;
                break;
            }
            case VT_OP_SWAP: {
                if (sp < 2) return -1; /* stack underflow: SWAP */
                vtx_nodeid_t tmp = op_stack[sp - 1];
                op_stack[sp - 1] = op_stack[sp - 2];
                op_stack[sp - 2] = tmp;
                break;
            }

            /* ---- Type queries ---- */
            case VT_OP_ISNULL: {
                if (sp < 1) return -1; /* stack underflow: ISNULL */
                vtx_nodeid_t v = op_stack[--sp];
                /* Create a null-pointer comparison */
                vtx_nodeid_t null_const = vtx_node_create(&graph->node_table, VTX_OP_Constant);
                if (null_const == VTX_NODEID_INVALID) return -1;
                vtx_node_t *nc = vtx_node_get(&graph->node_table, null_const);
                nc->constval = vtx_constval_ptr(NULL);
                nc->type = VTX_TYPE_Ptr;
                result = vtx_node_create(&graph->node_table, VTX_OP_CmpP);
                if (result == VTX_NODEID_INVALID) return -1;
                vtx_node_t *n = vtx_node_get(&graph->node_table, result);
                n->cond = VTX_COND_EQ;
                vtx_node_add_input(&graph->node_table, result, v);
                vtx_node_add_input(&graph->node_table, result, null_const);
                op_stack[sp++] = result;
                break;
            }
            case VT_OP_TYPEOF: {
                if (sp < 1) return -1; /* stack underflow: TYPEOF */
                vtx_nodeid_t v = op_stack[--sp];
                result = vtx_node_create(&graph->node_table, VTX_OP_Proj);
                if (result == VTX_NODEID_INVALID) return -1;
                vtx_node_add_input(&graph->node_table, result, v);
                op_stack[sp++] = result;
                break;
            }

            case VT_OP_HALT:
            case VT_OP_NOP:
                break;

            default:
                /* Unknown opcode — skip */
                break;
            }

            pc += insn_len;
        }

        /* BUGFIX (if-then-else crash): Create a Goto for fallthrough blocks.
         *
         * If the block's last instruction is NOT a terminator (Goto, If,
         * Return, Throw), the block falls through to the next block. The
         * block finder (line 603) records the successor edge, but no Goto
         * node is created in the IR. Without a Goto, the scheduler can't
         * place a terminator in the block, leaving it empty. The emitter
         * then can't emit a JMP, causing crashes.
         *
         * Fix: If the block's control_node is not a Goto/If/Return/Throw,
         * create a Goto node as the terminator. */
        if (block->control_node != VTX_NODEID_INVALID && !block->is_unreachable) {
            vtx_node_t *ctrl_n = vtx_node_get(&graph->node_table, block->control_node);
            if (ctrl_n != NULL &&
                ctrl_n->opcode != VTX_OP_Goto &&
                ctrl_n->opcode != VTX_OP_If &&
                ctrl_n->opcode != VTX_OP_Return &&
                ctrl_n->opcode != VTX_OP_LoopEnd) {
                /* Check if this block has a successor (fallthrough) */
                if (block->succ_count > 0) {
                    vtx_nodeid_t goto_n = vtx_node_create(&graph->node_table, VTX_OP_Goto);
                    if (goto_n != VTX_NODEID_INVALID) {
                        vtx_node_add_input(&graph->node_table, goto_n, block->control_node);
                        block->control_node = goto_n;
                    }
                }
            }
        }

        /* Save exit operand stack state for this block.
         * This allows successor blocks to reconstruct their entry stack
         * from predecessor exit states. Critical for propagating values
         * across block boundaries (e.g., RETURN_VALUE at a merge point
         * where both predecessors left one value on the stack). */
        if (sp > 0) {
            block->exit_sp = sp;
            for (int32_t si = 0; si < sp; si++) {
                block->exit_stack[si] = op_stack[si];
            }
        }
    }

    /* Phase 4: For loop headers, create LoopEnd in the latch block,
     * connect the back edge to the LoopBegin, and update Phi node inputs.
     *
     * BUGFIX: The original code only connected the LoopEnd to the LoopBegin
     * but never updated the Phi nodes' inputs. After Phase 2, the loop
     * header's Phi nodes only have inputs from forward (non-back-edge)
     * predecessors. Phase 4 must add the back-edge predecessor's values
     * as additional Phi inputs, so that each Phi has one data input per
     * Region/LoopBegin input. Without this, the SSA invariant was broken:
     * the LoopBegin had N inputs but its Phis had N-1 data inputs.
     *
     * BUGFIX 2 (loop mismatch): The loop header's Phi was created in
     * Phase 2 with Input 0 = predecessor's INITIAL local (e.g., undef
     * void Constant). But Phase 3 updates the predecessor's locals to
     * the EXIT state (e.g., SMI(0) after `store_local 1`). The Phi's
     * Input 0 was never updated, so it still referenced the old undef.
     *
     * BUGFIX 3 (if-then-else crash): Non-loop Region Phis have the SAME
     * bug — they were created in Phase 2 with inputs from the predecessors'
     * INITIAL locals, but Phase 3 updates the predecessors' locals to
     * their EXIT state. The Phi inputs must be updated to reference the
     * EXIT locals. Additionally, if the predecessors' exit locals now
     * DIFFER but no Phi was created (because Phase 2 saw them as same),
     * we must create a new Phi.
     *
     * Fix: Before the loop header fix, scan all non-loop Region blocks.
     * For each local, check if predecessors' exit locals differ. If they
     * do and the local is already a Phi, update its inputs. If they differ
     * but no Phi exists, create one. */
    for (uint32_t bi = 0; bi < nblocks; bi++) {
        vtx_block_info_t *block = &blocks[bi];
        if (block->is_loop_header) continue;  /* loop headers handled below */
        if (block->region_node == VTX_NODEID_INVALID) continue;
        if (block->locals == NULL) continue;

        vtx_node_t *region_n = vtx_node_get(&graph->node_table, block->region_node);
        if (region_n == NULL || region_n->opcode != VTX_OP_Region) continue;

        /* BUGFIX: Skip blocks inside loop bodies. If any predecessor is a
         * loop header, this block is inside the loop body and its locals
         * should use the loop header's Phis, not create new merge Phis.
         * Creating merge Phis inside loop bodies causes circular references
         * and wrong back-edge values. */
        bool pred_is_loop_header = false;
        for (uint32_t p = 0; p < block->pred_count; p++) {
            uint32_t pred_idx = block->pred_indices[p];
            if (blocks[pred_idx].is_loop_header) {
                pred_is_loop_header = true;
                break;
            }
        }
        if (pred_is_loop_header) continue;

        for (uint16_t li = 0; li < max_locals; li++) {
            vtx_nodeid_t local_node = block->locals[li];
            if (local_node == VTX_NODEID_INVALID) continue;

            /* Check if predecessors' exit locals differ */
            vtx_nodeid_t first_val = VTX_NODEID_INVALID;
            bool differ = false;
            uint32_t pred_count = 0;
            for (uint32_t p = 0; p < block->pred_count; p++) {
                uint32_t pred_idx = block->pred_indices[p];
                if (blocks[pred_idx].is_unreachable) continue;
                if (blocks[pred_idx].locals == NULL) continue;
                vtx_nodeid_t val = blocks[pred_idx].locals[li];
                if (val == VTX_NODEID_INVALID) continue;
                pred_count++;
                if (first_val == VTX_NODEID_INVALID) {
                    first_val = val;
                } else if (val != first_val) {
                    differ = true;
                }
            }

            if (!differ || pred_count < 2) continue;

            vtx_node_t *local_n = vtx_node_get(&graph->node_table, local_node);
            /* BUGFIX: Check if this Phi belongs to THIS block's Region.
             * If the Phi references a DIFFERENT Region/LoopBegin (e.g., it's
             * a loop header's Phi that was inherited), DON'T modify it —
             * create a new Phi for this merge point instead. Modifying a
             * loop header's Phi corrupts its inputs and loses the back-edge. */
            bool phi_belongs_to_this_block = false;
            if (local_n != NULL && local_n->opcode == VTX_OP_Phi) {
                for (uint32_t inp = 0; inp < local_n->input_count; inp++) {
                    if (local_n->inputs[inp] == block->region_node) {
                        phi_belongs_to_this_block = true;
                        break;
                    }
                }
            }

            if (local_n != NULL && local_n->opcode == VTX_OP_Phi && phi_belongs_to_this_block) {
                /* Already a Phi for THIS block — update its inputs with exit locals */
                uint32_t data_idx = 0;
                for (uint32_t p = 0; p < block->pred_count; p++) {
                    uint32_t pred_idx = block->pred_indices[p];
                    if (blocks[pred_idx].is_unreachable) continue;
                    if (blocks[pred_idx].locals == NULL) continue;
                    vtx_nodeid_t exit_val = blocks[pred_idx].locals[li];
                    if (exit_val == VTX_NODEID_INVALID) continue;
                    /* Update data input if not the Region control input */
                    if (data_idx < local_n->input_count &&
                        local_n->inputs[data_idx] != block->region_node) {
                        if (local_n->inputs[data_idx] != exit_val) {
                            vtx_node_replace_input(&graph->node_table, local_node, data_idx, exit_val);
                        }
                        data_idx++;
                    }
                }
            } else {
                /* Not a Phi but predecessors differ — create a new Phi */
                vtx_nodeid_t phi = vtx_node_create(&graph->node_table, VTX_OP_Phi);
                if (phi == VTX_NODEID_INVALID) continue;
                vtx_node_t *phi_n = vtx_node_get(&graph->node_table, phi);
                phi_n->flags = VTX_NF_DATA | VTX_NF_PINNED;
                phi_n->type = VTX_TYPE_Bottom;
                /* Add one input per predecessor (exit locals) */
                for (uint32_t p = 0; p < block->pred_count; p++) {
                    uint32_t pred_idx = block->pred_indices[p];
                    if (blocks[pred_idx].is_unreachable) continue;
                    if (blocks[pred_idx].locals == NULL) continue;
                    vtx_nodeid_t val = blocks[pred_idx].locals[li];
                    if (val == VTX_NODEID_INVALID) val = local_node;
                    vtx_node_add_input(&graph->node_table, phi, val);
                }
                /* Add Region as control input */
                vtx_node_add_input(&graph->node_table, phi, block->region_node);
                block->locals[li] = phi;

                /* CRITICAL: Replace all references to the old local value
                 * (local_node) with the new Phi in nodes that are control-
                 * dependent on this block's Region. Without this, the
                 * Return/load_local nodes still reference the old value
                 * (e.g., the void Constant N3) instead of the new Phi (N12),
                 * producing wrong results.
                 *
                 * We scan all nodes in the graph and replace any input that
                 * matches local_node with phi, but ONLY for nodes that have
                 * the Region (or a projection of it) as a control input.
                 * This ensures we only replace uses within this block's
                 * control region, not uses in other blocks. */
                {
                    vtx_node_table_t *nt = &graph->node_table;
                    for (uint32_t ni = 0; ni < nt->count; ni++) {
                        vtx_node_t *n = &nt->nodes[ni];
                        if (n->dead) continue;
                        if (ni == phi) continue;
                        /* Check if this node is control-dependent on this block's Region */
                        bool in_region = false;
                        for (uint32_t inp = 0; inp < n->input_count; inp++) {
                            if (n->inputs[inp] == block->region_node) {
                                in_region = true;
                                break;
                            }
                        }
                        if (!in_region) continue;
                        /* Replace references to local_node with phi */
                        for (uint32_t inp = 0; inp < n->input_count; inp++) {
                            if (n->inputs[inp] == local_node) {
                                vtx_node_replace_input(nt, ni, inp, phi);
                            }
                        }
                    }
                }
            }
        }
    }

    /* Debug: Print latch locals before Phase 4 */
    for (uint32_t bi = 0; bi < nblocks; bi++) {
        if (blocks[bi].is_loop_end) {
            for (uint16_t li = 0; li < max_locals && li < 5; li++) {
                fprintf(stderr, " [%u]=N%u", li, blocks[bi].locals ? blocks[bi].locals[li] : VTX_NODEID_INVALID);
            }
            fprintf(stderr, "\n");
        }
    }

    /* Now handle loop headers: update Phi Input 0 and connect back-edges.
     *
     * The loop header's Phi was created in Phase 2 with Input 0 = the
     * predecessor's INITIAL local. Phase 3 updates the predecessor's
     * locals to the EXIT state. The Phi's Input 0 must be updated to
     * reference the EXIT local. */
    for (uint32_t bi = 0; bi < nblocks; bi++) {
        vtx_block_info_t *block = &blocks[bi];
        if (block->is_loop_header && block->region_node != VTX_NODEID_INVALID) {
            vtx_node_t *region_n = vtx_node_get(&graph->node_table, block->region_node);
            if (region_n != NULL && region_n->opcode == VTX_OP_LoopBegin) {
                /* BUGFIX 2: Update Phi Input 0 with the forward predecessor's
                 * EXIT local value (after Phase 3 ran). The Phi was created
                 * in Phase 2 with the predecessor's INITIAL local, which is
                 * now stale. */
                if (block->locals != NULL) {
                    for (uint16_t li = 0; li < max_locals; li++) {
                        vtx_nodeid_t local_node = block->locals[li];
                        if (local_node == VTX_NODEID_INVALID) continue;
                        vtx_node_t *local_n = vtx_node_get(&graph->node_table, local_node);
                        if (local_n != NULL && local_n->opcode == VTX_OP_Phi) {
                            /* Find the forward (non-loop-end) predecessor */
                            for (uint32_t p = 0; p < block->pred_count; p++) {
                                uint32_t pred_idx = block->pred_indices[p];
                                if (blocks[pred_idx].is_loop_end) continue;
                                if (blocks[pred_idx].is_unreachable) continue;
                                if (blocks[pred_idx].locals == NULL) continue;
                                vtx_nodeid_t exit_val = blocks[pred_idx].locals[li];
                                if (exit_val == VTX_NODEID_INVALID) continue;
                                /* Update Input 0 of the Phi to the predecessor's EXIT local.
                                 * Input 0 is the first data input (the Phi's inputs are:
                                 * [forward_val, LoopBegin, back_edge_val (added later)]).
                                 * Use vtx_node_replace_input to properly maintain
                                 * output counts and use-def lists. */
                                if (local_n->input_count > 0 && local_n->inputs[0] != exit_val) {
                                    vtx_node_replace_input(&graph->node_table, local_node, 0, exit_val);
                                }
                                break; /* Only one forward predecessor for a loop header */
                            }
                        }
                    }
                }

                /* Find the latch (backward-branching predecessor) */
                for (uint32_t p = 0; p < block->pred_count; p++) {
                    uint32_t pred_idx = block->pred_indices[p];
                    if (blocks[pred_idx].is_loop_end) {
                        /* Create LoopEnd in the latch block */
                        vtx_nodeid_t loop_end = vtx_node_create(&graph->node_table, VTX_OP_LoopEnd);
                        if (loop_end == VTX_NODEID_INVALID) return -1;
                        vtx_node_add_input(&graph->node_table, loop_end, blocks[pred_idx].control_node);
                        blocks[pred_idx].control_node = loop_end;

                        /* Connect LoopEnd to LoopBegin as back-edge input */
                        vtx_node_add_input(&graph->node_table, block->region_node, loop_end);

                        /* BUGFIX: Update Phi node inputs with back-edge values.
                         * The loop header's memory_node is either a Phi (if there
                         * are multiple forward predecessors) or inherited from the
                         * single forward predecessor. In both cases, if it's a Phi
                         * node, we need to add the latch block's memory state as
                         * an additional input. */
                        if (block->memory_node != VTX_NODEID_INVALID) {
                            vtx_node_t *mem_node = vtx_node_get(&graph->node_table, block->memory_node);
                            if (mem_node != NULL && mem_node->opcode == VTX_OP_Phi) {
                                /* Add the latch's memory state as a Phi input */
                                vtx_nodeid_t latch_mem = blocks[pred_idx].memory_node;
                                if (latch_mem == VTX_NODEID_INVALID) {
                                    latch_mem = graph->entry_memory;
                                }
                                vtx_node_add_input(&graph->node_table, block->memory_node, latch_mem);
                            }
                        }

                        /* Update each local variable Phi with the latch's value.
                         * The loop header's locals[i] is either:
                         *  - A Phi node (if forward predecessors disagreed)
                         *  - A direct value (if forward predecessors agreed)
                         * For Phi nodes, we add the latch's value as an input.
                         * For direct values, we need to create a Phi if the
                         * latch's value differs. */
                        /* BUGFIX: Phase 3's store_local updates block->locals[li]
                         * to the stored value, overwriting the Phi. So we can't
                         * rely on block->locals to find the Phi. Instead, scan
                         * the graph for Phi nodes that reference this LoopBegin
                         * and add the back-edge value to them.
                         *
                         * Phis are created in create_block_entry in order of
                         * local index (li = 0, 1, 2, ...). So the Nth Phi
                         * referencing the LoopBegin corresponds to local N.
                         * We add the latch's exit local[N] as the back-edge input. */
                        {
                            vtx_node_table_t *nt = &graph->node_table;
                            uint32_t phi_index = 0; /* which local index this Phi is */
                            for (uint32_t ni = 0; ni < nt->count; ni++) {
                                vtx_node_t *phi_n = &nt->nodes[ni];
                                if (phi_n->dead) continue;
                                if (phi_n->opcode != VTX_OP_Phi) continue;

                                /* Check if this Phi references our LoopBegin */
                                bool belongs_to_this_loop = false;
                                for (uint32_t inp = 0; inp < phi_n->input_count; inp++) {
                                    if (phi_n->inputs[inp] == block->region_node) {
                                        belongs_to_this_loop = true;
                                        break;
                                    }
                                }
                                if (!belongs_to_this_loop) continue;

                                /* This Phi belongs to our loop header.
                                 * Use phi_index as the local index. */
                                uint16_t li = (uint16_t)phi_index;
                                phi_index++;

                                if (li >= max_locals) continue; /* safety */

                                /* Get the latch's exit local for this index */
                                vtx_nodeid_t latch_val = VTX_NODEID_INVALID;
                                if (blocks[pred_idx].locals != NULL) {
                                    latch_val = blocks[pred_idx].locals[li];
                                }

                                /* BUGFIX: Don't add circular references.
                                 * If the latch's exit local is the Phi itself
                                 * (happens for unused locals that inherit the
                                 * loop header's Phi), adding it creates a
                                 * circular Phi → Phi reference that breaks
                                 * SCCP convergence and causes wrong codegen.
                                 * Use the forward input (Input 0) instead. */
                                if (latch_val == (vtx_nodeid_t)ni) {
                                    /* Circular — use the forward input instead */
                                    if (phi_n->input_count > 0) {
                                        latch_val = phi_n->inputs[0];
                                    } else {
                                        latch_val = VTX_NODEID_INVALID;
                                    }
                                }

                                /* Only add if not already present (avoid duplicates) */
                                bool already_has = false;
                                for (uint32_t inp = 0; inp < phi_n->input_count; inp++) {
                                    if (phi_n->inputs[inp] == latch_val) {
                                        already_has = true;
                                        break;
                                    }
                                }
                                if (!already_has && latch_val != VTX_NODEID_INVALID) {
                                    vtx_node_add_input(nt, (vtx_nodeid_t)ni, latch_val);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    /* Phase 5: For Goto/If terminators, connect control outputs to successor
     * Region nodes. We already created the Region nodes, so now we need to
     * link the If projections.
     *
     * In a Sea-of-Nodes, If produces two projections (true/false) which
     * feed into the respective successor Region nodes. We create Proj nodes
     * for the If outputs. */
    for (uint32_t bi = 0; bi < nblocks; bi++) {
        vtx_block_info_t *block = &blocks[bi];
        if (block->succ_count == 0) continue;

        /* Find the terminator */
        size_t last_insn_pc = block->start_pc;
        size_t scan = block->start_pc;
        while (scan < block->end_pc) {
            last_insn_pc = scan;
            scan += vtx_bytecode_insn_length(bytecode, scan);
        }
        vtx_opcode_t term_op = vtx_bytecode_opcode_at(bytecode, last_insn_pc);

        if (term_op == VT_OP_IF_TRUE || term_op == VT_OP_IF_FALSE) {
            /* The control_node should be the If node we created */
            vtx_node_t *if_node = vtx_node_get(&graph->node_table, block->control_node);
            if (if_node != NULL && if_node->opcode == VTX_OP_If) {
                /* Create Proj nodes for true and false branches */
                /* True projection */
                vtx_nodeid_t proj_true = vtx_node_create(&graph->node_table, VTX_OP_Proj);
                if (proj_true == VTX_NODEID_INVALID) return -1;
                vtx_node_t *pt = vtx_node_get(&graph->node_table, proj_true);
                pt->local_index = 0; /* true branch index */
                pt->cond = VTX_COND_NE; /* taken when condition is true */
                vtx_node_add_input(&graph->node_table, proj_true, block->control_node);

                /* False projection */
                vtx_nodeid_t proj_false = vtx_node_create(&graph->node_table, VTX_OP_Proj);
                if (proj_false == VTX_NODEID_INVALID) return -1;
                vtx_node_t *pf = vtx_node_get(&graph->node_table, proj_false);
                pf->local_index = 1; /* false branch index */
                pf->cond = VTX_COND_EQ;
                vtx_node_add_input(&graph->node_table, proj_false, block->control_node);

                /* Connect projections to successor Region nodes.
                 * IR-2 fix: For IF_TRUE, successor[0] = true target, successor[1] = false fallthrough.
                 * For IF_FALSE, successor[0] = false target, successor[1] = true fallthrough.
                 * The original code always sent proj_true to succ[0] regardless of
                 * branch direction, swapping the projections for IF_FALSE blocks. */
                if (block->succ_count >= 2) {
                    /* Determine which projection goes to which successor.
                     * For IF_TRUE:  succ[0] gets proj_true,  succ[1] gets proj_false
                     * For IF_FALSE: succ[0] gets proj_false, succ[1] gets proj_true */
                    vtx_nodeid_t succ0_proj = (term_op == VT_OP_IF_FALSE) ? proj_false : proj_true;
                    vtx_nodeid_t succ1_proj = (term_op == VT_OP_IF_FALSE) ? proj_true  : proj_false;

                    /* BUGFIX (audit #3, loop hang): The successor Region's input
                     * was set during Phase 2 to the predecessor's region_node
                     * (LoopBegin/Region/Start). By Phase 5, control_node is the If.
                     * Try control_node first, then fall back to region_node. */
                    vtx_nodeid_t pred_region = block->region_node;
                    if (pred_region == VTX_NODEID_INVALID) {
                        pred_region = graph->start_node;
                    }

                    uint32_t target_idx = block->succ_indices[0];
                    vtx_nodeid_t target_region = blocks[target_idx].region_node;
                    if (target_region != VTX_NODEID_INVALID) {
                        vtx_node_t *region_n = vtx_node_get(&graph->node_table, target_region);
                        if (region_n != NULL) {
                            bool replaced = false;
                            for (uint32_t try_val = 0; try_val < 2 && !replaced; try_val++) {
                                vtx_nodeid_t search_for = (try_val == 0) ? block->control_node : pred_region;
                                for (uint32_t i = 0; i < region_n->input_count; i++) {
                                    if (region_n->inputs[i] == search_for) {
                                        vtx_node_replace_input(&graph->node_table, target_region, i, succ0_proj);
                                        replaced = true;
                                        break;
                                    }
                                }
                            }
                        }
                    }

                    uint32_t fall_idx = block->succ_indices[1];
                    vtx_nodeid_t fall_region = blocks[fall_idx].region_node;
                    if (fall_region != VTX_NODEID_INVALID) {
                        vtx_node_t *region_n = vtx_node_get(&graph->node_table, fall_region);
                        if (region_n != NULL) {
                            bool replaced = false;
                            for (uint32_t try_val = 0; try_val < 2 && !replaced; try_val++) {
                                vtx_nodeid_t search_for = (try_val == 0) ? block->control_node : pred_region;
                                for (uint32_t i = 0; i < region_n->input_count; i++) {
                                    if (region_n->inputs[i] == search_for) {
                                        vtx_node_replace_input(&graph->node_table, fall_region, i, succ1_proj);
                                        replaced = true;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        } else if (term_op == VT_OP_GOTO ||
                   (block->control_node != VTX_NODEID_INVALID &&
                    vtx_node_get(&graph->node_table, block->control_node) != NULL &&
                    vtx_node_get(&graph->node_table, block->control_node)->opcode == VTX_OP_Goto)) {
            /* Goto terminator (explicit or fallthrough).
             *
             * BUGFIX (if-then-else crash): For fallthrough blocks where we
             * created a Goto in Phase 3, connect the Goto's control output
             * to the successor's Region node. The successor Region was
             * created in Phase 2 with the predecessor's region_node as
             * input. Replace it with the Goto node. */
            if (block->succ_count >= 1) {
                uint32_t target_idx = block->succ_indices[0];
                if (target_idx < nblocks) {
                    vtx_nodeid_t target_region = blocks[target_idx].region_node;
                    if (target_region != VTX_NODEID_INVALID) {
                        vtx_node_t *region_n = vtx_node_get(&graph->node_table, target_region);
                        if (region_n != NULL) {
                            /* Replace the predecessor's region_node with the Goto */
                            vtx_nodeid_t search_for = block->control_node;
                            for (uint32_t i = 0; i < region_n->input_count; i++) {
                                if (region_n->inputs[i] == block->region_node ||
                                    region_n->inputs[i] == search_for) {
                                    vtx_node_replace_input(&graph->node_table, target_region, i, search_for);
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    /* Phase 6: Create the End node and connect all Return nodes to it.
     *
     * BUGFIX (audit #1, fuzz-discovered): The End node was never created,
     * so graph_has_start_and_end() and any downstream pass that walks
     * from End (e.g. backward DCE) would fail. The End node is the
     * single sink of the control-flow graph; every Return (and every
     * Throw that doesn't have a handler) must feed into it. */
    {
        vtx_nodeid_t end = vtx_node_create(&graph->node_table, VTX_OP_End);
        if (end == VTX_NODEID_INVALID) return -1;
        for (uint32_t i = 0; i < graph->node_table.count; i++) {
            const vtx_node_t *n = &graph->node_table.nodes[i];
            if (n->dead) continue;
            if (n->opcode == VTX_OP_Return) {
                vtx_node_add_input(&graph->node_table, end, n->id);
            }
        }
    }

    return 0;
}

/* ========================================================================== */
/* Graph printing                                                              */
/* ========================================================================== */

void vtx_graph_print(const vtx_graph_t *graph)
{
    VTX_ASSERT(graph != NULL, "graph must not be NULL");

    fprintf(stderr, "=== VORTEX SoN Graph (%u nodes) ===\n", graph->node_table.count);

    for (uint32_t i = 0; i < graph->node_table.count; i++) {
        const vtx_node_t *n = &graph->node_table.nodes[i];
        if (n->dead) continue;

        fprintf(stderr, "  N%u: %s [type=%s, flags=0x%x",
                n->id, vtx_node_opcode_name(n->opcode),
                vtx_nodetype_name(n->type), (unsigned)n->flags);

        if (n->opcode == VTX_OP_Constant) {
            switch (n->constval.kind) {
            case VTX_TYPE_Int:
                fprintf(stderr, ", val=%lld", (long long)n->constval.as.int_val);
                break;
            case VTX_TYPE_Float:
                fprintf(stderr, ", val=%g", n->constval.as.float_val);
                break;
            case VTX_TYPE_Ptr:
                fprintf(stderr, ", val=ptr(%p)", n->constval.as.ptr_val);
                break;
            default:
                break;
            }
        }

        if (n->opcode == VTX_OP_If || n->opcode == VTX_OP_Cmp ||
            n->opcode == VTX_OP_CmpP || n->opcode == VTX_OP_CmpF ||
            n->opcode == VTX_OP_CmpD) {
            fprintf(stderr, ", cond=%d", (int)n->cond);
        }

        if (n->opcode == VTX_OP_Parameter) {
            fprintf(stderr, ", param=%u", n->local_index);
        }

        fprintf(stderr, "] -> [");
        for (uint32_t j = 0; j < n->input_count; j++) {
            if (j > 0) fprintf(stderr, ", ");
            fprintf(stderr, "N%u", n->inputs[j]);
        }
        fprintf(stderr, "]\n");
    }
}
