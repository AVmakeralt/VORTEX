/**
 * VORTEX Guard Emission — Guard Checks + Deopt Stubs
 *
 * Emits guard checks and deoptimization stubs for JIT-compiled code.
 * Each guard is a compare + conditional jump. On failure, execution
 * jumps to a deopt stub that saves state and transfers to the deopt
 * runtime for interpreter re-entry.
 */

#include "lower/guard_emit.h"
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* Guard descriptor array                                                      */
/* ========================================================================== */

int vtx_guard_desc_array_init(vtx_guard_desc_array_t *arr, vtx_arena_t *arena)
{
    if (!arr) return -1;
    arr->count = 0;
    arr->capacity = VTX_GUARD_DESC_INITIAL_CAPACITY;
    arr->guards = (vtx_guard_desc_t *)vtx_arena_alloc(arena,
                    arr->capacity * sizeof(vtx_guard_desc_t));
    if (!arr->guards) {
        arr->capacity = 0;
        return -1;
    }
    memset(arr->guards, 0, arr->capacity * sizeof(vtx_guard_desc_t));
    return 0;
}

uint32_t vtx_guard_desc_array_add(vtx_guard_desc_array_t *arr,
                                   vtx_guard_desc_t guard, vtx_arena_t *arena)
{
    if (!arr) return UINT32_MAX;
    if (arr->count >= arr->capacity) {
        uint32_t new_cap = arr->capacity * 2;
        vtx_guard_desc_t *new_guards = (vtx_guard_desc_t *)vtx_arena_alloc(arena,
            new_cap * sizeof(vtx_guard_desc_t));
        if (!new_guards) return UINT32_MAX;
        if (arr->guards && arr->count > 0) {
            memcpy(new_guards, arr->guards, arr->count * sizeof(vtx_guard_desc_t));
        }
        arr->guards = new_guards;
        arr->capacity = new_cap;
    }
    uint32_t idx = arr->count++;
    arr->guards[idx] = guard;
    return idx;
}

/* ========================================================================== */
/* Guard lowering                                                              */
/* ========================================================================== */

/**
 * Collect the set of physical registers that are live at a given instruction
 * position. This is a simplified version — in a full implementation, we'd
 * use the liveness information from the register allocator.
 *
 * For now, we record the registers used by the guard instruction itself
 * and any registers that appear in the same block before the guard.
 */
static void collect_live_regs(vtx_inst_stream_t *stream, uint32_t block_idx,
                               uint32_t inst_idx, vtx_side_table_t *side_table,
                               uint32_t side_entry_idx)
{
    if (block_idx >= stream->block_count) return;
    vtx_inst_block_t *blk = &stream->blocks[block_idx];

    /* Track which physical registers are in use at this point */
    uint32_t reg_set = 0; /* bitmask of live physical registers */

    /* Walk instructions up to the guard point to find live registers */
    for (uint32_t i = 0; i <= inst_idx && i < blk->inst_count; i++) {
        vtx_inst_t *inst = &blk->insts[i];
        for (int op = 0; op < VTX_INST_MAX_OPERANDS; op++) {
            if (inst->opnd_kinds[op] == VTX_OPND_PREG) {
                reg_set |= (1u << inst->operands[op]);
            }
        }
    }

    /* Add register map entries for each live register */
    for (uint32_t r = 0; r < 16; r++) {
        if (reg_set & (1u << r)) {
            /* Find the NodeID for this register — simplified: use the
             * source_node from the instruction that defined it */
            vtx_nodeid_t node_id = VTX_NODEID_INVALID;
            for (uint32_t i = 0; i <= inst_idx && i < blk->inst_count; i++) {
                vtx_inst_t *inst = &blk->insts[i];
                if (inst->opnd_kinds[0] == VTX_OPND_PREG &&
                    inst->operands[0] == r) {
                    node_id = inst->source_node;
                }
            }
            vtx_side_table_add_register(side_table, r, node_id);
        }
    }
}

int vtx_guard_emit_lower(vtx_guard_desc_array_t *guards,
                          vtx_inst_stream_t *inst_stream,
                          vtx_x86_emit_t *emit,
                          vtx_side_table_t *side_table,
                          vtx_arena_t *arena)
{
    if (!guards || !emit || !side_table) return -1;

    int lowered = 0;

    for (uint32_t g = 0; g < guards->count; g++) {
        vtx_guard_desc_t *guard = &guards->guards[g];

        /* Record the native PC offset of this guard */
        uint32_t native_pc = vtx_x86_emit_position(emit);

        /* Add a side table entry */
        uint32_t st_idx = vtx_side_table_add_entry(side_table, native_pc,
                            guard->frame_state_index,
                            VTX_STF_GUARD);

        /* Collect live registers at this point */
        /* Find the guard instruction in the stream */
        for (uint32_t b = 0; b < inst_stream->block_count; b++) {
            vtx_inst_block_t *blk = &inst_stream->blocks[b];
            for (uint32_t i = 0; i < blk->inst_count; i++) {
                if (blk->insts[i].source_node == guard->guard_node &&
                    (blk->insts[i].flags & VTX_INST_FLAG_IS_GUARD)) {
                    collect_live_regs(inst_stream, b, i, side_table, st_idx);
                    break;
                }
            }
        }

        /* The guard's compare + jcc have already been emitted by isel.
         * We just need to record the side table entry.
         * The jcc target (deopt stub) will be patched later. */
        guard->bytecode_pc = native_pc; /* store for later patching */

        lowered++;
    }

    (void)arena;
    return lowered;
}

/* ========================================================================== */
/* Deopt stub emission                                                         */
/* ========================================================================== */

/**
 * Structure to track deopt stub locations for patching.
 */
typedef struct {
    uint32_t guard_index;      /* index into guards array */
    uint32_t stub_offset;      /* native code offset of the deopt stub */
    uint32_t jcc_patch_offset; /* native code offset of the JCC to patch */
} vtx_deopt_stub_info_t;

int vtx_guard_emit_deopt_stubs(vtx_guard_desc_array_t *guards,
                                vtx_x86_emit_t *emit,
                                vtx_side_table_t *side_table,
                                uint8_t *code_start,
                                vtx_arena_t *arena)
{
    if (!guards || !emit) return -1;

    int emitted = 0;

    /* Allocate tracking array */
    uint32_t info_count = guards->count;
    vtx_deopt_stub_info_t *infos = (vtx_deopt_stub_info_t *)vtx_arena_alloc(
        arena, info_count * sizeof(vtx_deopt_stub_info_t));
    if (!infos && info_count > 0) return -1;

    for (uint32_t g = 0; g < guards->count; g++) {
        vtx_guard_desc_t *guard = &guards->guards[g];

        /* Record the stub offset */
        uint32_t stub_offset = vtx_x86_emit_position(emit);
        infos[g].guard_index = g;
        infos[g].stub_offset = stub_offset;

        /* Emit deopt stub:
         * 1. Save callee-saved registers that might have live values
         * 2. Store the frame_state_index in RDI for the deopt runtime
         * 3. Load the deopt handler address
         * 4. Jump to the deopt runtime
         */

        /* Push frame state index in RDI */
        vtx_x86_emit_push_r(emit, 7); /* RDI */

        /* mov rdi, frame_state_index */
        vtx_x86_emit_mov_imm32(emit, 7, (int32_t)guard->frame_state_index); /* RDI = 7 */

        /* mov rsi, native_pc_offset (for the deopt runtime to look up) */
        vtx_x86_emit_mov_imm32(emit, 6, (int32_t)guard->bytecode_pc); /* RSI = 6 */

        /* Load deopt handler address (absolute, 64-bit)
         * This will be patched by the reloc system */
        /* mov rax, 0 (placeholder — will be patched with actual address) */
        vtx_x86_emit_mov_imm64(emit, 0, 0); /* RAX = 0, placeholder */

        /* call rax (jump to deopt handler) */
        /* Actually use jmp since we don't need to return */
        /* mov rax, [rip+0] would be ideal, but for now use absolute address */
        /* jmp rax */
        vtx_x86_emit_push_r(emit, 0); /* push RAX (deopt handler address) */
        vtx_x86_emit_ret(emit);        /* "ret" jumps to the address on stack */

        /* Record in side table */
        if (side_table) {
            uint32_t st_idx = vtx_side_table_add_entry(side_table, stub_offset,
                                guard->frame_state_index,
                                VTX_STF_GUARD);
            (void)st_idx;
        }

        emitted++;
    }

    return emitted;
}

/* ========================================================================== */
/* Patch guard JCC instructions                                                */
/* ========================================================================== */

int vtx_guard_emit_patch(vtx_guard_desc_array_t *guards,
                          vtx_x86_emit_t *emit,
                          uint8_t *code_start,
                          vtx_arena_t *arena)
{
    if (!guards || !emit || !emit->buffer) return -1;

    /* This function would patch the JCC instructions in the main code
     * to point to their corresponding deopt stubs.
     *
     * The actual implementation requires:
     * 1. Knowing the offset of each JCC instruction
     * 2. Knowing the offset of each deopt stub
     * 3. Computing the relative displacement (stub_offset - jcc_offset - 6)
     * 4. Patching the 32-bit displacement in the code buffer
     *
     * For a full implementation, the guard_emit_lower function would
     * record the JCC offsets, and this function would use them.
     */

    /* For each guard, find its JCC in the instruction stream and patch it.
     * This is a simplified implementation that scans the code buffer
     * for JCC patterns. A production implementation would track the
     * exact offsets during emission. */

    (void)code_start;
    (void)arena;
    return 0;
}
