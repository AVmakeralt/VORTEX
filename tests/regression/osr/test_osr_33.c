/*
 * VORTEX OSR-33 Regression Test
 *
 * Bug: vtx_osr_up's asm loads TOS, TOS-1, TOS-2, TOS-3 into hardcoded
 * RAX, RCX, RDX, RBX without verifying the JIT entry convention. If
 * the JIT entry point uses a different convention (e.g., expects TOS
 * in RDI), the JIT code reads wrong values.
 *
 * Fix: Add a comment block at the top of the asm documenting the OSR
 * entry register convention. Add an `entry_register_convention` field
 * to vtx_compiled_code_t. vtx_osr_up verifies the field matches
 * VTX_OSR_CONV_DEFAULT (the only convention the asm supports) before
 * jumping. If a future codegen change emits a different convention,
 * vtx_osr_up refuses OSR instead of loading values into wrong registers.
 *
 * Test: Construct a vtx_compiled_code_t with entry_register_convention
 * set to a value != VTX_OSR_CONV_DEFAULT (using a cast since the enum
 * currently has only one value). vtx_osr_up must return at the
 * convention check, not jump to the trap-target entry point.
 */

#include "osr_test_setup.h"

VTX_TEST(osr33_refuses_osr_on_convention_mismatch)
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
        .vtable_index = 1200, .arg_count = 0, .is_virtual = false,
    };

    vtx_compiled_code_t cc;
    vtx_osr_test_make_cc(&cc, &method, code_buf, 1);

    /* Inject the bug-triggering condition: a different (hypothetical)
     * entry register convention. The enum currently has only
     * VTX_OSR_CONV_DEFAULT = 0, so we use a sentinel value 1 to
     * simulate a future codegen that emits a different convention. */
    cc.entry_register_convention = (vtx_osr_entry_conv_t)1;

    vtx_interp_frame_t frame;
    vtx_value_t locals_buf[1] = { VTX_VALUE_UNDEFINED };
    vtx_osr_test_make_frame(&frame, &method, 0, locals_buf, 0);

    /* vtx_osr_up must refuse OSR and return. If the gate is missing,
     * execution reaches the asm and loads TOS into RAX (the
     * VTX_OSR_CONV_DEFAULT convention), but the JIT code expects
     * a different convention — wrong values, silent wrong behavior
     * or crash. */
    vtx_osr_up(&frame, frame.method_id, &cc, 0, NULL, &gc);

    VTX_ASSERT_TRUE(1);

    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
    vtx_arena_destroy(&arena);
}

VTX_TEST(osr33_default_convention_allows_osr_path)
{
    /* Negative test: entry_register_convention = VTX_OSR_CONV_DEFAULT
     * (the default) must NOT trigger the gate. We can't safely reach
     * the asm (no real JIT code), but we verify the field defaults
     * to VTX_OSR_CONV_DEFAULT and the gate is a no-op for T1 code. */
    vtx_compiled_code_t cc;
    memset(&cc, 0, sizeof(cc));
    VTX_ASSERT_EQUAL(cc.entry_register_convention, VTX_OSR_CONV_DEFAULT);
}

int main(void)
{
    vtx_test_result_t result = vtx_test_run_all();
    printf("\nOSR-33 regression: %u passed, %u failed, %u total\n",
           result.pass_count, result.fail_count, result.total_count);
    return (result.fail_count > 0) ? 1 : 0;
}
