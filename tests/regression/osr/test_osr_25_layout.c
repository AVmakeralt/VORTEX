/*
 * VORTEX OSR-25 Regression Test
 *
 * Bug: When the dispatch loop attempted OSR up and the compiled
 *      method's cm->frame_layout.max_locals == 0, it fell back to
 *      vtx_frame_layout_compute(method). But this PUBLIC API returns
 *      locals_base = -8, spill_base = -8, total_frame_size = 40,
 *      while the codegen's emit_prologue overrides locals_base to
 *      -24 (accounting for the saved RBX/R12 slots at [RBP-8] and
 *      [RBP-16]). Using the public layout in the OSR asm would make
 *      the asm write spills to the saved-RBX slot, corrupting the
 *      JIT epilogue's RBX restore and the caller's frame.
 *
 * Fix: The dispatch loop's OSR-up site (src/interp/dispatch.c) now
 *      uses cm->frame_layout whenever cm->frame_layout.total_frame_size
 *      > 0. The public vtx_frame_layout_compute() fallback is retained
 *      only as a defensive codegen-bug guard (with a loud warning);
 *      it should be unreachable because emit_prologue always populates
 *      total_frame_size.
 *
 * Test: Compile two methods via the baseline JIT (one with max_locals=0,
 *       one with max_locals > 0). For each method, fetch cm from the
 *       registry and verify:
 *
 *   1. cm->frame_layout.total_frame_size > 0  (codegen populated it).
 *   2. cm->frame_layout.locals_base == -24   (codegen override, NOT
 *      the public API's -8). This is the smoking gun: if the public
 *      fallback were used here, locals_base would be -8 and the OSR
 *      asm would corrupt RBX.
 *   3. cm->frame_layout.spill_base == -(24 + (max_locals + 1) * 8)
 *      for max_locals > 0, OR a smaller value for max_locals == 0.
 *
 *   For max_locals > 0, the codegen's locals_base override (-24)
 *   must be present on cm->frame_layout. The PUBLIC API
 *   vtx_frame_layout_compute(method) returns locals_base = -8 — we
 *   verify the two differ to prove cm->frame_layout is NOT the
 *   public fallback.
 */

#include "osr_test_setup.h"
#include "baseline/codegen.h"
#include "codecache/install.h"

/* VTX_FRAME_SAVED_REGS_SIZE is 16 (saved RBX + R12). The codegen's
 * emit_prologue overrides locals_base to -(16 + 8) = -24 to reserve
 * [RBP-8] for RBX and [RBP-16] for R12 before locals begin at [RBP-24]. */
#define EXPECTED_CODEGEN_LOCALS_BASE  (-(int32_t)(VTX_FRAME_SAVED_REGS_SIZE + 8))

VTX_TEST(osr25_cm_frame_layout_codegen_override_locals_base)
{
    /* Compile a method with max_locals > 0. The codegen's emit_prologue
     * overrides locals_base to -24 (accounting for saved RBX/R12). */
    vtx_arena_t arena; vtx_arena_init(&arena);
    vtx_type_system_t ts; vtx_type_system_init(&ts);
    vtx_gc_t gc; vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);

    uint8_t *code_buf = vtx_arena_alloc(&arena, 16);
    /* PC 0: LOAD_CONST_INT 0  ; push 0
     * PC 3: STORE_LOCAL 0       ; locals[0] = 0
     * PC 6: LOAD_LOCAL 0        ; push locals[0]
     * PC 9: RETURN_VALUE
     */
    code_buf[0] = VT_OP_LOAD_CONST_INT; code_buf[1] = 0; code_buf[2] = 0;
    code_buf[3] = VT_OP_STORE_LOCAL;    code_buf[4] = 0; code_buf[5] = 0;
    code_buf[6] = VT_OP_LOAD_LOCAL;     code_buf[7] = 0; code_buf[8] = 0;
    code_buf[9] = VT_OP_RETURN_VALUE;

    vtx_value_t consts[1] = { vtx_make_smi(0) };
    vtx_bytecode_t bc = {
        .code = code_buf, .length = 10,
        .constant_pool = consts, .constant_count = 1,
        .max_locals = 1, .max_stack = 2,
    };
    vtx_method_desc_t method = {
        .name = "osr25a", .signature = "()I",
        .bytecode = &bc, .compiled_code = NULL,
        .vtable_index = 2500, .arg_count = 0, .is_virtual = false,
    };

    vtx_code_cache_t cache; vtx_code_cache_init(&cache, 1 << 16);
    vtx_method_registry_t registry; vtx_method_registry_init(&registry, &arena);
    vtx_compiled_code_t *cc = vtx_baseline_compile(
        &method, NULL, &arena, &cache, &registry);
    VTX_ASSERT_NOT_NULL(cc);

    vtx_compiled_method_t *cm = vtx_method_registry_get(&registry, method.vtable_index);
    VTX_ASSERT_NOT_NULL(cm);
    VTX_ASSERT_NOT_NULL(cm->code_start);

    /* === OSR-25 assertions === */

    /* 1. The codegen populated frame_layout (total_frame_size > 0). */
    VTX_ASSERT_TRUE(cm->frame_layout.total_frame_size > 0);

    /* 2. locals_base is -24, NOT -8 (the public API's value).
     *    This is the smoking gun: cm->frame_layout carries the codegen
     *    override, so the OSR asm will reserve [RBP-8] for saved RBX
     *    and [RBP-16] for saved R12 instead of writing spills to those
     *    slots. */
    VTX_ASSERT_EQUAL(cm->frame_layout.locals_base, EXPECTED_CODEGEN_LOCALS_BASE);
    VTX_ASSERT_NOT_EQUAL(cm->frame_layout.locals_base, -8);

    /* 3. spill_base is also offset by the saved-regs prefix. */
    int32_t expected_spill_base = -(int32_t)(VTX_FRAME_SAVED_REGS_SIZE +
                                              (cm->frame_layout.max_locals + 1) * 8);
    VTX_ASSERT_EQUAL(cm->frame_layout.spill_base, expected_spill_base);

    /* 4. Negative: the public vtx_frame_layout_compute(method) returns
     *    locals_base = -8 (the value the buggy fallback would have used).
     *    Verify cm->frame_layout differs from the public API. */
    vtx_jit_frame_layout_t public_layout = vtx_frame_layout_compute(&method);
    VTX_ASSERT_EQUAL(public_layout.locals_base, -8);
    VTX_ASSERT_NOT_EQUAL(cm->frame_layout.locals_base, public_layout.locals_base);
    VTX_ASSERT_NOT_EQUAL(cm->frame_layout.spill_base, public_layout.spill_base);

    vtx_compiled_code_destroy(cc);
    vtx_method_registry_destroy(&registry);
    vtx_code_cache_destroy(&cache);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
    vtx_arena_destroy(&arena);
}

VTX_TEST(osr25_cm_frame_layout_populated_even_for_max_locals_zero)
{
    /* The original bug was specifically about max_locals == 0 (the
     * public fallback's locals_base = -8 was being used because the
     * max_locals == 0 check was the trigger). Verify the codegen
     * STILL overrides locals_base to -24 even for max_locals == 0,
     * so the OSR asm never writes spills into the saved-RBX slot. */
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
        .name = "osr25b", .signature = "()V",
        .bytecode = &bc, .compiled_code = NULL,
        .vtable_index = 2501, .arg_count = 0, .is_virtual = false,
    };

    vtx_code_cache_t cache; vtx_code_cache_init(&cache, 1 << 16);
    vtx_method_registry_t registry; vtx_method_registry_init(&registry, &arena);
    vtx_compiled_code_t *cc = vtx_baseline_compile(
        &method, NULL, &arena, &cache, &registry);
    VTX_ASSERT_NOT_NULL(cc);

    vtx_compiled_method_t *cm = vtx_method_registry_get(&registry, method.vtable_index);
    VTX_ASSERT_NOT_NULL(cm);
    VTX_ASSERT_NOT_NULL(cm->code_start);

    /* total_frame_size > 0: codegen ran emit_prologue. */
    VTX_ASSERT_TRUE(cm->frame_layout.total_frame_size > 0);

    /* locals_base == -24 even when max_locals == 0: the codegen
     * override is unconditional, so the OSR-up site's "if
     * total_frame_size > 0" branch always takes the codegen layout
     * and never falls through to the public API. */
    VTX_ASSERT_EQUAL(cm->frame_layout.locals_base, EXPECTED_CODEGEN_LOCALS_BASE);
    VTX_ASSERT_NOT_EQUAL(cm->frame_layout.locals_base, -8);

    /* And it differs from the public API for this method too. */
    vtx_jit_frame_layout_t public_layout = vtx_frame_layout_compute(&method);
    VTX_ASSERT_EQUAL(public_layout.locals_base, -8);
    VTX_ASSERT_NOT_EQUAL(cm->frame_layout.locals_base, public_layout.locals_base);

    vtx_compiled_code_destroy(cc);
    vtx_method_registry_destroy(&registry);
    vtx_code_cache_destroy(&cache);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
    vtx_arena_destroy(&arena);
}

int main(void)
{
    vtx_test_result_t result = vtx_test_run_all();
    printf("\nOSR-25 regression: %u passed, %u failed, %u total\n",
           result.pass_count, result.fail_count, result.total_count);
    return (result.fail_count > 0) ? 1 : 0;
}
