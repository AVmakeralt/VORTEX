/*
 * VORTEX Register Allocator Upgrade Tests
 *
 * Audit priority #5: validates loop-boundary splitting and
 * rematerialization in src/lower/regalloc.c.
 */

#include "test_framework.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "lower/regalloc.h"
#include "lower/isel.h"
#include "runtime/arena.h"

VTX_TEST(regalloc_rematerialize_constant_load) {
    /* A MOV reg, imm64 instruction should be rematerializable. */
    vtx_arena_t arena;
    vtx_arena_init(&arena);

    vtx_inst_stream_t stream;
    memset(&stream, 0, sizeof(stream));
    stream.block_count = 1;
    stream.blocks = vtx_arena_alloc(&arena, sizeof(vtx_inst_block_t));
    memset(stream.blocks, 0, sizeof(vtx_inst_block_t));

    vtx_inst_block_t *blk = &stream.blocks[0];
    blk->inst_count = 1;
    blk->inst_capacity = 4;
    blk->insts = vtx_arena_alloc(&arena, sizeof(vtx_inst_t) * 4);
    memset(blk->insts, 0, sizeof(vtx_inst_t) * 4);

    /* MOV vreg0, 0x7FF8000000000000 (SMI header constant) */
    vtx_inst_t *inst = &blk->insts[0];
    inst->opcode = VTX_X86_MOV;
    inst->opnd_kinds[0] = VTX_OPND_VREG;
    inst->operands[0] = 0;  /* vreg 0 */
    inst->opnd_kinds[1] = VTX_OPND_IMM;
    inst->imm = (int64_t)0x7FF8000000000000ULL;
    inst->flags = VTX_INST_FLAG_HAS_IMM;
    inst->source_node = 0;

    VTX_ASSERT_TRUE(vtx_regalloc_can_rematerialize(&stream, 0));
    uint32_t cost = vtx_regalloc_rematerialize_cost(&stream, 0);
    VTX_ASSERT_TRUE(cost == 1);

    vtx_arena_destroy(&arena);
}

VTX_TEST(regalloc_rematerialize_xor_zero) {
    /* XOR reg, reg (zero) should be rematerializable. */
    vtx_arena_t arena;
    vtx_arena_init(&arena);

    vtx_inst_stream_t stream;
    memset(&stream, 0, sizeof(stream));
    stream.block_count = 1;
    stream.blocks = vtx_arena_alloc(&arena, sizeof(vtx_inst_block_t));
    memset(stream.blocks, 0, sizeof(vtx_inst_block_t));

    vtx_inst_block_t *blk = &stream.blocks[0];
    blk->inst_count = 1;
    blk->inst_capacity = 4;
    blk->insts = vtx_arena_alloc(&arena, sizeof(vtx_inst_t) * 4);
    memset(blk->insts, 0, sizeof(vtx_inst_t) * 4);

    /* XOR vreg0, vreg0 (zero) */
    vtx_inst_t *inst = &blk->insts[0];
    inst->opcode = VTX_X86_XOR;
    inst->opnd_kinds[0] = VTX_OPND_VREG;
    inst->operands[0] = 0;
    inst->opnd_kinds[1] = VTX_OPND_VREG;
    inst->operands[1] = 0;  /* same reg = zero */
    inst->source_node = 0;

    VTX_ASSERT_TRUE(vtx_regalloc_can_rematerialize(&stream, 0));
    VTX_ASSERT_TRUE(vtx_regalloc_rematerialize_cost(&stream, 0) == 1);

    vtx_arena_destroy(&arena);
}

VTX_TEST(regalloc_not_rematerializable_add) {
    /* ADD reg, reg should NOT be rematerializable. */
    vtx_arena_t arena;
    vtx_arena_init(&arena);

    vtx_inst_stream_t stream;
    memset(&stream, 0, sizeof(stream));
    stream.block_count = 1;
    stream.blocks = vtx_arena_alloc(&arena, sizeof(vtx_inst_block_t));
    memset(stream.blocks, 0, sizeof(vtx_inst_block_t));

    vtx_inst_block_t *blk = &stream.blocks[0];
    blk->inst_count = 1;
    blk->inst_capacity = 4;
    blk->insts = vtx_arena_alloc(&arena, sizeof(vtx_inst_t) * 4);
    memset(blk->insts, 0, sizeof(vtx_inst_t) * 4);

    /* ADD vreg0, vreg1 */
    vtx_inst_t *inst = &blk->insts[0];
    inst->opcode = VTX_X86_ADD;
    inst->opnd_kinds[0] = VTX_OPND_VREG;
    inst->operands[0] = 0;
    inst->opnd_kinds[1] = VTX_OPND_VREG;
    inst->operands[1] = 1;
    inst->source_node = 0;

    VTX_ASSERT_TRUE(!vtx_regalloc_can_rematerialize(&stream, 0));
    VTX_ASSERT_TRUE(vtx_regalloc_rematerialize_cost(&stream, 0) == UINT32_MAX);

    vtx_arena_destroy(&arena);
}

VTX_TEST(regalloc_split_at_loop_boundaries_no_schedule) {
    /* Without a schedule, loop-boundary splitting should be a no-op. */
    vtx_arena_t arena;
    vtx_arena_init(&arena);

    vtx_regalloc_result_t result;
    memset(&result, 0, sizeof(result));
    result.interval_count = 4;
    result.intervals = vtx_arena_alloc(&arena, sizeof(vtx_live_interval_t) * 4);
    memset(result.intervals, 0, sizeof(vtx_live_interval_t) * 4);

    vtx_inst_stream_t stream;
    memset(&stream, 0, sizeof(stream));
    stream.schedule = NULL;  /* no schedule */

    uint32_t splits = vtx_regalloc_split_at_loop_boundaries(&result, &stream, &arena);
    VTX_ASSERT_TRUE(splits == 0);

    vtx_arena_destroy(&arena);
}

VTX_TEST(regalloc_rematerialize_lea) {
    /* LEA reg, [base + disp] should be rematerializable. */
    vtx_arena_t arena;
    vtx_arena_init(&arena);

    vtx_inst_stream_t stream;
    memset(&stream, 0, sizeof(stream));
    stream.block_count = 1;
    stream.blocks = vtx_arena_alloc(&arena, sizeof(vtx_inst_block_t));
    memset(stream.blocks, 0, sizeof(vtx_inst_block_t));

    vtx_inst_block_t *blk = &stream.blocks[0];
    blk->inst_count = 1;
    blk->inst_capacity = 4;
    blk->insts = vtx_arena_alloc(&arena, sizeof(vtx_inst_t) * 4);
    memset(blk->insts, 0, sizeof(vtx_inst_t) * 4);

    /* LEA vreg0, [rbp + 16] */
    vtx_inst_t *inst = &blk->insts[0];
    inst->opcode = VTX_X86_LEA;
    inst->opnd_kinds[0] = VTX_OPND_VREG;
    inst->operands[0] = 0;
    inst->opnd_kinds[1] = VTX_OPND_MEM;
    inst->source_node = 0;

    VTX_ASSERT_TRUE(vtx_regalloc_can_rematerialize(&stream, 0));
    VTX_ASSERT_TRUE(vtx_regalloc_rematerialize_cost(&stream, 0) == 1);

    vtx_arena_destroy(&arena);
}

VTX_TEST(regalloc_rematerialize_cost_comparison) {
    /* Verify that rematerialization cost (1) < spill cost (2+).
     * This is the key invariant: if rematerializable, prefer it over spill. */
    vtx_arena_t arena;
    vtx_arena_init(&arena);

    vtx_inst_stream_t stream;
    memset(&stream, 0, sizeof(stream));
    stream.block_count = 1;
    stream.blocks = vtx_arena_alloc(&arena, sizeof(vtx_inst_block_t));
    memset(stream.blocks, 0, sizeof(vtx_inst_block_t));

    vtx_inst_block_t *blk = &stream.blocks[0];
    blk->inst_count = 1;
    blk->inst_capacity = 4;
    blk->insts = vtx_arena_alloc(&arena, sizeof(vtx_inst_t) * 4);
    memset(blk->insts, 0, sizeof(vtx_inst_t) * 4);

    /* MOV vreg0, 42 */
    vtx_inst_t *inst = &blk->insts[0];
    inst->opcode = VTX_X86_MOV;
    inst->opnd_kinds[0] = VTX_OPND_VREG;
    inst->operands[0] = 0;
    inst->opnd_kinds[1] = VTX_OPND_IMM;
    inst->imm = 42;
    inst->flags = VTX_INST_FLAG_HAS_IMM;
    inst->source_node = 0;

    uint32_t remat_cost = vtx_regalloc_rematerialize_cost(&stream, 0);
    uint32_t spill_cost = 2;  /* store + load = 2 instructions minimum */

    printf("[regalloc] rematerialize cost=%u, spill cost=%u\n", remat_cost, spill_cost);
    VTX_ASSERT_TRUE(remat_cost < spill_cost);
    VTX_ASSERT_TRUE(remat_cost == 1);

    vtx_arena_destroy(&arena);
}

int main(void) {
    printf("=== VORTEX Register Allocator Upgrade Tests ===\n\n");
    vtx_test_run_all();
    return 0;
}
