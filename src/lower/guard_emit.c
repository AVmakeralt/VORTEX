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
/* Deopt handler configuration (Bug 2 fix)                                     */
/* ========================================================================== */

/**
 * Default deopt handler stub — called when a guard fails and no custom
 * handler has been registered. Prints diagnostic info and aborts.
 */
extern void vtx_deopt_handler_stub(uint32_t frame_state_index,
                                    uint32_t native_pc);

/**
 * Global deopt handler function pointer. If NULL, the default
 * vtx_deopt_handler_stub is used.
 */
static void *(*vtx_deopt_handler)(uint32_t frame_state_index,
                                   uint32_t native_pc) = NULL;

void vtx_guard_emit_set_deopt_handler(void *handler)
{
    vtx_deopt_handler = handler;
}

void *vtx_guard_emit_get_deopt_handler(void)
{
    return vtx_deopt_handler;
}

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

    (void)side_entry_idx;
}

/**
 * Find the JCC instruction in the instruction stream that corresponds to
 * a given guard node. The JCC is identified by:
 *   - opcode == VTX_X86_JCC
 *   - flags & VTX_INST_FLAG_IS_GUARD
 *   - source_node == guard_node
 *
 * Returns the block index and instruction index via out_block/out_inst,
 * or returns -1 if not found.
 */
static int find_guard_jcc(vtx_inst_stream_t *stream,
                           vtx_nodeid_t guard_node,
                           uint32_t *out_block,
                           uint32_t *out_inst)
{
    for (uint32_t b = 0; b < stream->block_count; b++) {
        vtx_inst_block_t *blk = &stream->blocks[b];
        for (uint32_t i = 0; i < blk->inst_count; i++) {
            vtx_inst_t *inst = &blk->insts[i];
            if (inst->opcode == VTX_X86_JCC &&
                (inst->flags & VTX_INST_FLAG_IS_GUARD) &&
                inst->source_node == guard_node) {
                *out_block = b;
                *out_inst = i;
                return 0;
            }
        }
    }
    return -1;
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

        /* Find the JCC instruction for this guard in the instruction stream.
         * The JCC's native_offset was filled by vtx_x86_emit_function during
         * code emission. We use this to record where the JCC is in the native
         * code buffer, so we can later patch its rel32 displacement. */
        uint32_t jcc_block = 0, jcc_inst = 0;
        if (inst_stream && find_guard_jcc(inst_stream, guard->guard_node,
                                           &jcc_block, &jcc_inst) == 0) {
            vtx_inst_t *jcc = &inst_stream->blocks[jcc_block].insts[jcc_inst];
            guard->jcc_native_offset = jcc->native_offset;
        } else {
            /* Fallback: if we can't find the JCC in the stream, mark as invalid.
             * This guard will be skipped during patching. */
            guard->jcc_native_offset = UINT32_MAX;
        }

        /* Record the native PC offset of this guard for the side table.
         * Use the JCC's native offset if available, otherwise use the current
         * emitter position (which may be inaccurate). */
        uint32_t native_pc = (guard->jcc_native_offset != UINT32_MAX)
                             ? guard->jcc_native_offset
                             : vtx_x86_emit_position(emit);

        /* Add a side table entry */
        uint32_t st_idx = vtx_side_table_add_entry(side_table, native_pc,
                            guard->frame_state_index,
                            VTX_STF_GUARD);

        /* Collect live registers at this point */
        if (inst_stream) {
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
        }

        /* The guard's compare + jcc have already been emitted by isel.
         * We just need to record the side table entry and JCC offset.
         * The jcc target (deopt stub) will be patched later. */

        lowered++;
    }

    (void)arena;
    return lowered;
}

/* ========================================================================== */
/* Deopt stub emission                                                         */
/* ========================================================================== */

int vtx_guard_emit_deopt_stubs(vtx_guard_desc_array_t *guards,
                                vtx_x86_emit_t *emit,
                                vtx_side_table_t *side_table,
                                uint8_t *code_start,
                                vtx_arena_t *arena)
{
    if (!guards || !emit) return -1;

    int emitted = 0;

    for (uint32_t g = 0; g < guards->count; g++) {
        vtx_guard_desc_t *guard = &guards->guards[g];

        /* Skip guards whose JCC we couldn't locate */
        if (guard->jcc_native_offset == UINT32_MAX) continue;

        /* Record the stub offset — this is the native code offset where the
         * deopt stub begins. We store it back in the guard descriptor so
         * vtx_guard_emit_patch can compute the JCC rel32 displacement. */
        uint32_t stub_offset = vtx_x86_emit_position(emit);
        guard->deopt_stub_offset = stub_offset;

        /* Emit deopt stub:
         *   1. Save the original RDI (callee-saved in System V ABI)
         *   2. Set RDI = frame_state_index (1st argument)
         *   3. Set RSI = native_pc_offset (2nd argument)
         *   4. Load deopt handler address into RAX
         *   5. Jump to the deopt handler via JMP RAX
         *
         * Bug 2 fix: Previously emitted "mov rax, 0; push rax; ret" which
         * jumped to address 0 (NULL). Now we load the actual handler address
         * and use a proper JMP RAX.
         */

        /* Push frame state index in RDI (save original RDI) */
        vtx_x86_emit_push_r(emit, 7); /* RDI */

        /* mov rdi, frame_state_index */
        vtx_x86_emit_mov_imm32(emit, 7, (int32_t)guard->frame_state_index); /* RDI = 7 */

        /* mov rsi, native_pc_offset (for the deopt runtime to look up) */
        vtx_x86_emit_mov_imm32(emit, 6, (int32_t)guard->jcc_native_offset); /* RSI = 6 */

        /* Load the deopt handler address into RAX.
         * Use the registered handler if available, otherwise use the default stub. */
        void *handler = vtx_deopt_handler;
        if (handler == NULL) {
            handler = (void *)(uintptr_t)vtx_deopt_handler_stub;
        }

        /* mov rax, imm64 (absolute 64-bit address of the deopt handler) */
        vtx_x86_emit_mov_imm64(emit, 0, (uint64_t)(uintptr_t)handler);

        /* jmp rax — jump to the deopt handler.
         * Encoding: FF E0 (opcode FF /4, register RAX=0)
         * This replaces the old "push rax; ret" which jumped to whatever
         * was on the stack (address 0 = crash). */
        emit_byte(emit, 0xFF);
        emit_byte(emit, 0xE0); /* ModR/M: mod=11, reg=4 (/4 = JMP), rm=0 (RAX) */

        /* Record in side table */
        if (side_table) {
            uint32_t st_idx = vtx_side_table_add_entry(side_table, stub_offset,
                                guard->frame_state_index,
                                VTX_STF_GUARD);
            (void)st_idx;
        }

        emitted++;
    }

    (void)code_start;
    (void)arena;
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

    int patched = 0;

    for (uint32_t g = 0; g < guards->count; g++) {
        vtx_guard_desc_t *guard = &guards->guards[g];

        /* Skip guards with invalid offsets */
        if (guard->jcc_native_offset == UINT32_MAX ||
            guard->deopt_stub_offset == 0) {
            continue;
        }

        /* x86-64 JCC rel32 encoding: 0F 8x cd (6 bytes total)
         *   Byte 0:    0x0F (two-byte opcode prefix)
         *   Byte 1:    0x80 + condition_code
         *   Bytes 2-5: 32-bit displacement (little-endian)
         *
         * The displacement is relative to the instruction AFTER the JCC:
         *   rel32 = target - (jcc_offset + 6)
         * Where:
         *   target        = deopt_stub_offset (offset from code_start)
         *   jcc_offset    = jcc_native_offset (offset from code_start)
         *   6             = size of the JCC rel32 instruction
         *
         * The displacement is stored at code_start + jcc_native_offset + 2.
         */
        uint32_t jcc_off = guard->jcc_native_offset;
        uint32_t stub_off = guard->deopt_stub_offset;

        /* Bounds check: ensure we have room for the 6-byte JCC instruction */
        if (jcc_off + 6 > emit->position) {
            continue; /* JCC offset out of bounds, skip */
        }

        /* Verify this looks like a JCC rel32: first byte should be 0x0F */
        if (code_start[jcc_off] != 0x0F) {
            continue; /* Not a JCC rel32, skip */
        }

        /* Verify second byte is in the JCC range: 0x80-0x8F */
        uint8_t opcode2 = code_start[jcc_off + 1];
        if ((opcode2 & 0xF0) != 0x80) {
            continue; /* Not a JCC rel32, skip */
        }

        /* Compute the relative displacement */
        int32_t rel32 = (int32_t)stub_off - (int32_t)(jcc_off + 6);

        /* Patch the 32-bit displacement in the code buffer.
         * Write in little-endian order. */
        code_start[jcc_off + 2] = (uint8_t)(rel32 & 0xFF);
        code_start[jcc_off + 3] = (uint8_t)((rel32 >> 8) & 0xFF);
        code_start[jcc_off + 4] = (uint8_t)((rel32 >> 16) & 0xFF);
        code_start[jcc_off + 5] = (uint8_t)((rel32 >> 24) & 0xFF);

        patched++;
    }

    (void)arena;
    return patched;
}
