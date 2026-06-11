#include "ir/graph.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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

    if (block_idx == 0) {
        /* Entry block: Region is just Start */
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

    /* Create Region or LoopBegin */
    vtx_node_opcode_t region_op = block->is_loop_header ? VTX_OP_LoopBegin : VTX_OP_Region;
    vtx_nodeid_t region = vtx_node_create(nt, region_op);
    if (region == VTX_NODEID_INVALID) return -1;
    block->region_node = region;

    /* Connect predecessor control outputs as Region inputs */
    for (uint32_t p = 0; p < block->pred_count; p++) {
        uint32_t pred_idx = block->pred_indices[p];
        vtx_nodeid_t pred_ctrl = blocks[pred_idx].control_node;
        if (pred_ctrl != VTX_NODEID_INVALID) {
            vtx_node_add_input(nt, region, pred_ctrl);
        }
    }

    block->control_node = region;

    /* Create initial memory Phi for merge points with >1 predecessor */
    if (block->pred_count > 1) {
        vtx_nodeid_t mem_phi = vtx_node_create(nt, VTX_OP_Phi);
        if (mem_phi == VTX_NODEID_INVALID) return -1;
        vtx_node_t *phi_node = vtx_node_get(nt, mem_phi);
        phi_node->flags = vtx_nf_union(phi_node->flags, VTX_NF_MEMORY);

        /* Add memory inputs from each predecessor */
        for (uint32_t p = 0; p < block->pred_count; p++) {
            uint32_t pred_idx = block->pred_indices[p];
            vtx_nodeid_t pred_mem = blocks[pred_idx].memory_node;
            if (pred_mem == VTX_NODEID_INVALID) {
                pred_mem = graph->entry_memory;
            }
            vtx_node_add_input(nt, mem_phi, pred_mem);
        }
        /* Memory Phi depends on the Region for control */
        vtx_node_add_input(nt, mem_phi, region);
        block->memory_node = mem_phi;
    } else if (block->pred_count == 1) {
        /* Single predecessor: inherit memory directly */
        block->memory_node = blocks[block->pred_indices[0]].memory_node;
        if (block->memory_node == VTX_NODEID_INVALID) {
            block->memory_node = graph->entry_memory;
        }
    } else {
        block->memory_node = graph->entry_memory;
    }

    /* Allocate locals for this block */
    block->locals = (vtx_nodeid_t *)vtx_arena_alloc(arena, max_locals * sizeof(vtx_nodeid_t));
    if (block->locals == NULL) return -1;

    if (block->pred_count == 1) {
        /* Single predecessor: inherit locals directly */
        uint32_t pred_idx = block->pred_indices[0];
        for (uint16_t i = 0; i < max_locals; i++) {
            block->locals[i] = blocks[pred_idx].locals[i];
        }
    } else if (block->pred_count > 1) {
        /* Multiple predecessors: create Phi nodes for locals that differ */
        for (uint16_t li = 0; li < max_locals; li++) {
            /* Check if all predecessors agree on this local */
            bool all_same = true;
            vtx_nodeid_t first_val = VTX_NODEID_INVALID;
            for (uint32_t p = 0; p < block->pred_count; p++) {
                uint32_t pred_idx = block->pred_indices[p];
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

                /* Add one input per predecessor */
                for (uint32_t p = 0; p < block->pred_count; p++) {
                    uint32_t pred_idx = block->pred_indices[p];
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
     * We simulate the operand stack per block using a simple array. */
    vtx_nodeid_t *op_stack = (vtx_nodeid_t *)vtx_arena_alloc(
        arena, (max_stack + 2) * sizeof(vtx_nodeid_t));
    if (op_stack == NULL) return -1;

    for (uint32_t bi = 0; bi < nblocks; bi++) {
        vtx_block_info_t *block = &blocks[bi];
        int32_t sp = 0; /* stack pointer (index into op_stack) */

        /* If this is a catch handler, the exception is on the stack */
        if (block->is_catch_handler && block->region_node != VTX_NODEID_INVALID) {
            /* The Catch node produces the exception object */
            op_stack[sp++] = block->control_node;
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
                VTX_ASSERT(operand < max_locals, "local index out of range");
                op_stack[sp++] = block->locals[operand];
                break;
            }
            case VT_OP_STORE_LOCAL: {
                VTX_ASSERT(operand < max_locals, "local index out of range");
                VTX_ASSERT(sp >= 1, "stack underflow on STORE_LOCAL");
                sp--;
                block->locals[operand] = op_stack[sp];
                break;
            }

            /* ---- Field access ---- */
            case VT_OP_LOAD_FIELD: {
                VTX_ASSERT(sp >= 1, "stack underflow on LOAD_FIELD");
                vtx_nodeid_t obj = op_stack[--sp];
                result = vtx_node_create(&graph->node_table, VTX_OP_LoadField);
                if (result == VTX_NODEID_INVALID) return -1;
                vtx_node_t *n = vtx_node_get(&graph->node_table, result);
                n->field_offset = operand;
                n->type = VTX_TYPE_Bottom;
                vtx_node_add_input(&graph->node_table, result, block->memory_node);
                vtx_node_add_input(&graph->node_table, result, obj);
                block->memory_node = result;
                op_stack[sp++] = result;
                break;
            }
            case VT_OP_STORE_FIELD: {
                VTX_ASSERT(sp >= 2, "stack underflow on STORE_FIELD");
                vtx_nodeid_t val = op_stack[--sp];
                vtx_nodeid_t obj = op_stack[--sp];
                result = vtx_node_create(&graph->node_table, VTX_OP_StoreField);
                if (result == VTX_NODEID_INVALID) return -1;
                vtx_node_t *n = vtx_node_get(&graph->node_table, result);
                n->field_offset = operand;
                vtx_node_add_input(&graph->node_table, result, block->memory_node);
                vtx_node_add_input(&graph->node_table, result, obj);
                vtx_node_add_input(&graph->node_table, result, val);
                block->memory_node = result;
                break;
            }

            /* ---- Integer arithmetic (pop 2, push 1) ---- */
            case VT_OP_IADD: case VT_OP_ISUB: case VT_OP_IMUL:
            case VT_OP_IDIV: case VT_OP_IMOD: {
                VTX_ASSERT(sp >= 2, "stack underflow on binary arith");
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
                op_stack[sp++] = result;
                break;
            }

            /* ---- Float arithmetic ---- */
            case VT_OP_FADD: case VT_OP_FSUB: case VT_OP_FMUL: case VT_OP_FDIV: {
                VTX_ASSERT(sp >= 2, "stack underflow on float arith");
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
                break;
            }

            /* ---- Bitwise / unary ---- */
            case VT_OP_ISHL: case VT_OP_ISHR:
            case VT_OP_IAND: case VT_OP_IOR: case VT_OP_IXOR: {
                VTX_ASSERT(sp >= 2, "stack underflow on bitwise");
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
                VTX_ASSERT(sp >= 1, "stack underflow on INEG");
                vtx_nodeid_t v = op_stack[--sp];
                result = vtx_node_create(&graph->node_table, VTX_OP_Neg);
                if (result == VTX_NODEID_INVALID) return -1;
                vtx_node_add_input(&graph->node_table, result, v);
                op_stack[sp++] = result;
                break;
            }
            case VT_OP_INOT: {
                VTX_ASSERT(sp >= 1, "stack underflow on INOT");
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
                VTX_ASSERT(sp >= 2, "stack underflow on comparison");
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
                VTX_ASSERT(sp >= 2, "stack underflow on float comparison");
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
                VTX_ASSERT(sp >= 1, "stack underflow on IF");
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
                VTX_ASSERT(sp >= 1, "stack underflow on RETURN_VALUE");
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
                /* Stack: arg0, arg1, ..., argN → result
                 * The operand is the method index; arg count comes from
                 * the method descriptor. For simplicity, we consume args
                 * from the stack based on what the method descriptor says,
                 * or we use a fixed heuristic. */
                vtx_nodeid_t call = vtx_node_create(&graph->node_table, VTX_OP_CallStatic);
                if (call == VTX_NODEID_INVALID) return -1;
                vtx_node_t *n = vtx_node_get(&graph->node_table, call);
                n->method_index = operand;
                vtx_node_add_input(&graph->node_table, call, block->control_node);
                vtx_node_add_input(&graph->node_table, call, block->memory_node);
                block->control_node = call;
                block->memory_node = call;
                op_stack[sp++] = call;
                break;
            }
            case VT_OP_CALL_VIRTUAL: {
                VTX_ASSERT(sp >= 1, "stack underflow on CALL_VIRTUAL (need receiver)");
                vtx_nodeid_t receiver = op_stack[--sp];
                vtx_nodeid_t call = vtx_node_create(&graph->node_table, VTX_OP_CallVirtual);
                if (call == VTX_NODEID_INVALID) return -1;
                vtx_node_t *n = vtx_node_get(&graph->node_table, call);
                n->method_index = operand;
                vtx_node_add_input(&graph->node_table, call, block->control_node);
                vtx_node_add_input(&graph->node_table, call, block->memory_node);
                vtx_node_add_input(&graph->node_table, call, receiver);
                block->control_node = call;
                block->memory_node = call;
                op_stack[sp++] = call;
                break;
            }
            case VT_OP_CALL_INTERFACE: {
                VTX_ASSERT(sp >= 1, "stack underflow on CALL_INTERFACE (need receiver)");
                vtx_nodeid_t receiver = op_stack[--sp];
                vtx_nodeid_t call = vtx_node_create(&graph->node_table, VTX_OP_CallInterface);
                if (call == VTX_NODEID_INVALID) return -1;
                vtx_node_t *n = vtx_node_get(&graph->node_table, call);
                n->method_index = operand;
                vtx_node_add_input(&graph->node_table, call, block->control_node);
                vtx_node_add_input(&graph->node_table, call, block->memory_node);
                vtx_node_add_input(&graph->node_table, call, receiver);
                block->control_node = call;
                block->memory_node = call;
                op_stack[sp++] = call;
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
                break;
            }
            case VT_OP_NEWARRAY: {
                VTX_ASSERT(sp >= 1, "stack underflow on NEWARRAY (need size)");
                vtx_nodeid_t size = op_stack[--sp];
                result = vtx_node_create(&graph->node_table, VTX_OP_NewArray);
                if (result == VTX_NODEID_INVALID) return -1;
                vtx_node_t *n = vtx_node_get(&graph->node_table, result);
                n->type_id = operand;
                vtx_node_add_input(&graph->node_table, result, block->memory_node);
                vtx_node_add_input(&graph->node_table, result, size);
                block->memory_node = result;
                op_stack[sp++] = result;
                break;
            }

            /* ---- Type checks ---- */
            case VT_OP_CHECKCAST: {
                VTX_ASSERT(sp >= 1, "stack underflow on CHECKCAST");
                vtx_nodeid_t obj = op_stack[--sp];
                result = vtx_node_create(&graph->node_table, VTX_OP_CheckCast);
                if (result == VTX_NODEID_INVALID) return -1;
                vtx_node_t *n = vtx_node_get(&graph->node_table, result);
                n->type_id = operand;
                vtx_node_add_input(&graph->node_table, result, obj);
                op_stack[sp++] = result;
                break;
            }
            case VT_OP_INSTANCEOF: {
                VTX_ASSERT(sp >= 1, "stack underflow on INSTANCEOF");
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
                VTX_ASSERT(sp >= 2, "stack underflow on ARRAY_LOAD");
                vtx_nodeid_t idx = op_stack[--sp];
                vtx_nodeid_t arr = op_stack[--sp];
                result = vtx_node_create(&graph->node_table, VTX_OP_LoadIndexed);
                if (result == VTX_NODEID_INVALID) return -1;
                vtx_node_add_input(&graph->node_table, result, block->memory_node);
                vtx_node_add_input(&graph->node_table, result, arr);
                vtx_node_add_input(&graph->node_table, result, idx);
                block->memory_node = result;
                op_stack[sp++] = result;
                break;
            }
            case VT_OP_ARRAY_STORE: {
                VTX_ASSERT(sp >= 3, "stack underflow on ARRAY_STORE");
                vtx_nodeid_t val = op_stack[--sp];
                vtx_nodeid_t idx = op_stack[--sp];
                vtx_nodeid_t arr = op_stack[--sp];
                result = vtx_node_create(&graph->node_table, VTX_OP_StoreIndexed);
                if (result == VTX_NODEID_INVALID) return -1;
                vtx_node_add_input(&graph->node_table, result, block->memory_node);
                vtx_node_add_input(&graph->node_table, result, arr);
                vtx_node_add_input(&graph->node_table, result, idx);
                vtx_node_add_input(&graph->node_table, result, val);
                block->memory_node = result;
                break;
            }
            case VT_OP_ARRAY_LENGTH: {
                VTX_ASSERT(sp >= 1, "stack underflow on ARRAY_LENGTH");
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
                VTX_ASSERT(sp >= 1, "stack underflow on THROW");
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
                op_stack[sp++] = catch_n;
                break;
            }

            /* ---- Monitors ---- */
            case VT_OP_MONITOR_ENTER: {
                VTX_ASSERT(sp >= 1, "stack underflow on MONITOR_ENTER");
                vtx_nodeid_t obj = op_stack[--sp];
                vtx_nodeid_t rt = vtx_node_create(&graph->node_table, VTX_OP_CallRuntime);
                if (rt == VTX_NODEID_INVALID) return -1;
                vtx_node_add_input(&graph->node_table, rt, block->control_node);
                vtx_node_add_input(&graph->node_table, rt, block->memory_node);
                vtx_node_add_input(&graph->node_table, rt, obj);
                block->control_node = rt;
                block->memory_node = rt;
                break;
            }
            case VT_OP_MONITOR_EXIT: {
                VTX_ASSERT(sp >= 1, "stack underflow on MONITOR_EXIT");
                vtx_nodeid_t obj = op_stack[--sp];
                vtx_nodeid_t rt = vtx_node_create(&graph->node_table, VTX_OP_CallRuntime);
                if (rt == VTX_NODEID_INVALID) return -1;
                vtx_node_add_input(&graph->node_table, rt, block->control_node);
                vtx_node_add_input(&graph->node_table, rt, block->memory_node);
                vtx_node_add_input(&graph->node_table, rt, obj);
                block->control_node = rt;
                block->memory_node = rt;
                break;
            }

            /* ---- Stack manipulation ---- */
            case VT_OP_DUP: {
                VTX_ASSERT(sp >= 1, "stack underflow on DUP");
                op_stack[sp] = op_stack[sp - 1];
                sp++;
                break;
            }
            case VT_OP_POP: {
                VTX_ASSERT(sp >= 1, "stack underflow on POP");
                sp--;
                break;
            }
            case VT_OP_SWAP: {
                VTX_ASSERT(sp >= 2, "stack underflow on SWAP");
                vtx_nodeid_t tmp = op_stack[sp - 1];
                op_stack[sp - 1] = op_stack[sp - 2];
                op_stack[sp - 2] = tmp;
                break;
            }

            /* ---- Type queries ---- */
            case VT_OP_ISNULL: {
                VTX_ASSERT(sp >= 1, "stack underflow on ISNULL");
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
                VTX_ASSERT(sp >= 1, "stack underflow on TYPEOF");
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
    }

    /* Phase 4: For loop headers, create LoopEnd in the latch block and
     * connect the back edge to the LoopBegin. */
    for (uint32_t bi = 0; bi < nblocks; bi++) {
        vtx_block_info_t *block = &blocks[bi];
        if (block->is_loop_header && block->region_node != VTX_NODEID_INVALID) {
            vtx_node_t *region_n = vtx_node_get(&graph->node_table, block->region_node);
            if (region_n != NULL && region_n->opcode == VTX_OP_LoopBegin) {
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

                /* Connect projections to successor Region nodes */
                if (block->succ_count >= 2) {
                    /* Branch target (successor 0) gets the true projection */
                    uint32_t target_idx = block->succ_indices[0];
                    vtx_nodeid_t target_region = blocks[target_idx].region_node;
                    if (target_region != VTX_NODEID_INVALID) {
                        /* Find the input in the Region that corresponds to this
                         * predecessor and replace it with the projection */
                        vtx_node_t *region_n = vtx_node_get(&graph->node_table, target_region);
                        if (region_n != NULL) {
                            for (uint32_t i = 0; i < region_n->input_count; i++) {
                                vtx_nodeid_t inp = region_n->inputs[i];
                                const vtx_node_t *inp_n = vtx_node_get_const(&graph->node_table, inp);
                                if (inp_n != NULL && inp == block->control_node) {
                                    vtx_node_replace_input(&graph->node_table, target_region, i, proj_true);
                                    break;
                                }
                            }
                        }
                    }

                    /* Fall-through (successor 1) gets the false projection */
                    uint32_t fall_idx = block->succ_indices[1];
                    vtx_nodeid_t fall_region = blocks[fall_idx].region_node;
                    if (fall_region != VTX_NODEID_INVALID) {
                        vtx_node_t *region_n = vtx_node_get(&graph->node_table, fall_region);
                        if (region_n != NULL) {
                            for (uint32_t i = 0; i < region_n->input_count; i++) {
                                vtx_nodeid_t inp = region_n->inputs[i];
                                const vtx_node_t *inp_n = vtx_node_get_const(&graph->node_table, inp);
                                if (inp_n != NULL && inp == block->control_node) {
                                    vtx_node_replace_input(&graph->node_table, fall_region, i, proj_false);
                                    break;
                                }
                            }
                        }
                    }
                }
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
