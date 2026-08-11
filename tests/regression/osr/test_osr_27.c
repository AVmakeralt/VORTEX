/*
 * VORTEX OSR-27 Regression Test
 *
 * Bug: vtx_osr_up used compiled_code->code without a NULL check at
 * osr.c:269 (the bc_pc_map loop). The dispatch.c caller papers over
 * this by setting cc.code = cm->code_start, but the function's
 * contract doesn't enforce it. A future caller that forgets to set
 * cc.code would dereference NULL.
 *
 * Fix: Add a NULL check on compiled_code->code inside the bc_pc_map
 * loop, falling through to the failure path if NULL.
 *
 * Test: Construct a vtx_compiled_code_t with code = NULL but a
 * matching bc_pc_map entry. vtx_osr_up must return at the NULL check
 * (not crash with a NULL deref, not jump to the trap).
 */

#include "osr_test_setup.h"

VTX_TEST(osr27_null_code_returns_instead_of_deref)
{
    vtx_arena_t arena; vtx_arena_init(&arena);
    vtx_type_system_t ts; vtx_type_system_init(&ts);
    vtx_gc_t gc; vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);

    uint8_t *code_buf = vtx_arena_alloc(&arena, 4);
    code_buf[0] = VT_OP_RETURN_VALUE;
    vtx_bytecode_t bc = {
        .code = code_buf, .length = 1,
        .constant_pool = NULL, .constant_count = 0,
        .max_locals = 0, .max_stack = 1,
    };
    vtx_method_desc_t method = {
        .name = "ret", .signature = "()V",
        .bytecode = &bc, .compiled_code = NULL,
        .vtable_index = 1000, .arg_count = 0, .is_virtual = false,
    };

    vtx_compiled_code_t cc;
    vtx_osr_test_make_cc(&cc, &method, code_buf, 1);

    /* Populate bc_pc_map so the bc_pc_map loop is entered. The
     * stack_depth matches stack_top=0 so the OSR-24 check passes. */
    static vtx_bc_pc_map_entry_t bc_map[1];
    bc_map[0].bytecode_pc = 0;
    bc_map[0].native_offset = 0;
    bc_map[0].stack_depth = 0;
    cc.bc_pc_map = bc_map;
    cc.bc_pc_map_count = 1;

    /* The bug trigger: compiled_code->code is NULL. The pre-OSR-27
     * code dereferences NULL here: (uint8_t*)NULL + native_offset. */
    cc.code = NULL;

    vtx_interp_frame_t frame;
    vtx_value_t locals_buf[1] = { VTX_VALUE_UNDEFINED };
    vtx_osr_test_make_frame(&frame, &method, 0, locals_buf, 0);
    frame.stack_top = 0;

    /* vtx_osr_up must return at the NULL check (OSR-27 fix).
     * Pre-fix, this would either dereference NULL (segfault) or
     * compute osr_entry = (NULL + 0) = NULL and proceed toward
     * the asm, jumping to NULL (segfault). */
    vtx_osr_up(&frame, frame.method_id, &cc, 0, NULL, &gc);

    VTX_ASSERT_TRUE(1);

    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
    vtx_arena_destroy(&arena);
}

int main(void)
{
    vtx_test_result_t result = vtx_test_run_all();
    printf("\nOSR-27 regression: %u passed, %u failed, %u total\n",
           result.pass_count, result.fail_count, result.total_count);
    return (result.fail_count > 0) ? 1 : 0;
}
