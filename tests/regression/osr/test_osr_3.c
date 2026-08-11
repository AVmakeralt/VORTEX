/*
 * VORTEX OSR-3 Regression Test
 *
 * Bug: vtx_osr_up returned bool, but on a successful OSR transition the
 * inline-asm trampoline jumps to the JIT entry and never returns to C.
 * The value left in RAX is the JIT method's NaN-boxed return value,
 * which can be SMI 0 / undefined / null (low byte of RAX = 0) and thus
 * be misread as `false`. The dispatch loop then set jit_reenter_pending
 * and re-executed the entire method, doubling side effects.
 *
 * Fix: vtx_osr_up is declared void. The caller detects failure simply
 * by the function returning — if vtx_osr_up returns at all, OSR failed.
 *
 * This test:
 *   1. _Static_assert verifies the function pointer type is
 *      `void (*)(vtx_interp_frame_t*, uint32_t, const vtx_compiled_code_t*,
 *                uint32_t, vtx_method_registry_t*, vtx_gc_t*)`.
 *      If a future change reverts the signature to bool, this fails to
 *      compile.
 *   2. Runtime check: calling vtx_osr_up with NULL inputs returns
 *      (does not crash, does not jump to a fake entry point).
 *
 * The CRITICAL REPRODUCER CONSTRAINT note: the success path (asm
 * trampoline jumping to JIT code) cannot be exercised in isolation
 * without real compiled code, so we test the contract via the type
 * signature and the failure-path return.
 */

#include "osr_test_setup.h"

/* Compile-time check: vtx_osr_up must be declared `void`, not `bool`.
 * If someone reverts the OSR-3 fix, this static assertion fails to
 * compile, breaking the build. */
_Static_assert(
    __builtin_types_compatible_p(
        typeof(&vtx_osr_up),
        void (*)(vtx_interp_frame_t *, uint32_t,
                  const vtx_compiled_code_t *, uint32_t,
                  vtx_method_registry_t *, vtx_gc_t *)),
    "OSR-3: vtx_osr_up must be declared void (not bool)");

VTX_TEST(osr3_null_inputs_return_void)
{
    /* NULL interp/code — vtx_osr_up must return without crashing.
     * vtx_gc_init requires a non-NULL type_system (it asserts), so
     * we create a real one for the GC, but pass NULL for interp/code
     * to vtx_osr_up — that's the contract under test. */
    vtx_type_system_t ts; vtx_type_system_init(&ts);
    vtx_gc_t gc; vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);

    /* Calling with all-NULL inputs must return cleanly. */
    vtx_osr_up(NULL, 42, NULL, 100, NULL, &gc);

    /* If we reach here, the function returned (did not jump to a
     * fake JIT entry). The OSR-3 fix is honored: the caller detects
     * failure by the function returning, not by a bool return value. */
    VTX_ASSERT_TRUE(1);

    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
}

VTX_TEST(osr3_failure_returns_to_caller)
{
    /* A vtx_compiled_code_t with a deliberately invalid entry_point
     * that would crash if jumped to. vtx_osr_up must refuse OSR
     * (return) when the method_id doesn't match the frame's method_id,
     * NOT jump to the trap-target entry point. */
    vtx_arena_t arena; vtx_arena_init(&arena);
    vtx_type_system_t ts; vtx_type_system_init(&ts);
    vtx_gc_t gc; vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);

    /* Build a 1-instruction method: RETURN_VALUE.
     * locals=0, max_stack=1, code = [VT_OP_RETURN_VALUE]. */
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
        .vtable_index = 200, .arg_count = 0, .is_virtual = false,
    };

    vtx_compiled_code_t cc;
    vtx_osr_test_make_cc(&cc, &method, code_buf, 1);

    vtx_interp_frame_t frame;
    vtx_value_t locals_buf[1] = { VTX_VALUE_UNDEFINED };
    vtx_osr_test_make_frame(&frame, &method, /*loop_header_pc=*/0,
                             locals_buf, 1);

    /* Inject a method_id mismatch — vtx_osr_up must return (not jump
     * to the trap target entry_point). */
    frame.method_id = method.vtable_index;
    cc.method_id = method.vtable_index + 1;  /* mismatch */

    vtx_osr_up(&frame, frame.method_id, &cc, /*loop_header_pc=*/0,
               NULL, &gc);

    /* If we reach here, vtx_osr_up returned — OSR failed safely. */
    VTX_ASSERT_TRUE(1);

    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
    vtx_arena_destroy(&arena);
}

int main(void)
{
    vtx_test_result_t result = vtx_test_run_all();
    printf("\nOSR-3 regression: %u passed, %u failed, %u total\n",
           result.pass_count, result.fail_count, result.total_count);
    return (result.fail_count > 0) ? 1 : 0;
}
