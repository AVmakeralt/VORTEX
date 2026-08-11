/*
 * OSR-22 regression: the versioned cache's on_enter / on_exit hooks
 *                    must be wired into the dispatch loop's JIT entry
 *                    path so retired code is not freed while a thread
 *                    is executing it.
 *
 * Bug: vtx_versioned_cache_on_enter and on_exit were defined in
 *      src/codecache/versioned.c but never called from anywhere in
 *      the dispatch loop. This meant the versioned cache's safe-
 *      reclamation mechanism was completely disconnected:
 *
 *        - on_stack_count was always 0 for every version.
 *        - vtx_versioned_cache_reclaim would immediately free any
 *          retired version, even if a thread was currently executing
 *          its code.
 *        - vtx_install_method's atomic swap of method->compiled_code
 *          followed by free of old code was a UAF if another thread
 *          was in the JIT.
 *
 * Fix: src/interp/dispatch.c::vtx_dispatch_jit now calls
 *      vtx_versioned_cache_on_enter(vc, method_id) BEFORE jumping
 *      into the JIT code, and vtx_versioned_cache_on_exit(vc, method_id)
 *      AFTER the JIT returns. The pair is wrapped around the entry()
 *      call so it covers the entire JIT execution including any
 *      safepoint polls and deopt stubs.
 *
 * Reproducer:
 *
 *   1. Build a 3-instruction method (LOAD_CONST_INT 0 / CALL_RUNTIME 7 /
 *      RETURN_VALUE) and compile it via vtx_baseline_compile. Install
 *      it via vtx_install_method so method->compiled_code is set, AND
 *      install a version in the versioned cache (vtx_versioned_cache_install)
 *      so on_enter/on_exit have a target.
 *
 *   2. Register a runtime callback for func_id >= 7 (the dispatch path
 *      the JIT uses via vtx_runtime_builtin_call). The callback reads
 *      the active version's on_stack_count and stashes it in a global.
 *      This is the "observation during JIT execution."
 *
 *   3. Call vtx_interp_run, which dispatches to the JIT via
 *      vtx_dispatch_jit. vtx_dispatch_jit should call on_enter (count
 *      0→1), call the JIT code (which reaches our callback and
 *      observes count==1), then call on_exit (count 1→0).
 *
 *   4. After vtx_interp_run returns:
 *        - assert captured_during_callback == 1  → on_enter was called
 *        - assert active->on_stack_count == 0    → on_exit was called
 *
 *   If the OSR-22 fix is reverted (no on_enter/on_exit calls), the
 *   captured value would be 0 and the post-run count would also be 0
 *   — but the captured value of 0 is the smoking gun: it proves the
 *   dispatch loop did NOT bump on_stack_count before invoking JIT code.
 *
 *   Per the CRITICAL REPRODUCER CONSTRAINT: the bug's full impact
 *   (UAF of retired code while a thread is executing it) is racy and
 *   not deterministically reproducible. This test verifies the
 *   CONTRACT that prevents the race (on_enter/on_exit are actually
 *   invoked around JIT execution), which is the root-cause invariant.
 */

#include "test_framework.h"
#include "vortex_config.h"
#include "runtime/arena.h"
#include "runtime/bytecode.h"
#include "runtime/type_system.h"
#include "runtime/gc.h"
#include "runtime/object.h"
#include "interp/dispatch.h"
#include "baseline/codegen.h"
#include "baseline/frame_layout.h"
#include "codecache/cache.h"
#include "codecache/install.h"
#include "codecache/versioned.h"
#include "compile/request.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* Test fixture: state captured during JIT execution                            */
/* ========================================================================== */

/* Captured by the runtime callback while the JIT is executing.
 * Read by the test after vtx_interp_run returns. */
static int32_t           g_captured_on_stack_count = -1;
static uint32_t          g_captured_version_number = 0;
static vtx_versioned_cache_t *g_captured_vc         = NULL;

/* The versioned cache under test — set by the test, read by the callback. */
static vtx_versioned_cache_t *g_test_vc = NULL;
static uint32_t          g_test_method_id = 0;

/* Runtime callback invoked from JIT code via vtx_runtime_builtin_call
 * (helpers.c:538) for func_id >= 7. We use func_id = 7 (an otherwise-
 * unused slot) to invoke our test observation. */
static int osr22_runtime_callback(uint32_t func_id,
                                    vtx_value_t **sp_ptr,
                                    void *user_data)
{
    (void)user_data;
    if (func_id != 7 || g_test_vc == NULL) {
        return -1;  /* decline — let the default handler push undefined */
    }

    /* Observe the active version's on_stack_count WHILE the JIT is
     * executing. The OSR-22 fix should have called on_enter before
     * the JIT code reached this callback, so on_stack_count must be 1. */
    vtx_versioned_code_version_t *active =
        vtx_versioned_cache_get_active(g_test_vc, g_test_method_id);
    if (active != NULL) {
        g_captured_on_stack_count = active->on_stack_count;
        g_captured_version_number = active->version_number;
        g_captured_vc              = g_test_vc;
    }

    /* Push the same value back onto the stack so RETURN_VALUE has
     * something to return. The callback protocol: n_pushed = number
     * of values pushed. We pop 1 (the arg) and push 1 (the result). */
    /* *sp_ptr points to the current top-of-stack. We don't pop anything
     * (the JIT's CALL_RUNTIME helper already moved TOS into the `arg`
     * parameter and decremented sp). We just leave the value there. */
    return 1;
}

/* ========================================================================== */
/* Test: on_enter/on_exit are called around JIT execution                      */
/* ========================================================================== */

VTX_TEST(osr22_dispatch_jit_calls_on_enter_before_jit_code) {
    vtx_arena_t arena; vtx_arena_init(&arena);
    vtx_type_system_t ts; vtx_type_system_init(&ts);
    vtx_gc_t gc; vtx_gc_init(&gc, &ts, VTX_GC_NONE);

    vtx_code_cache_t cache;
    VTX_ASSERT_TRUE(vtx_code_cache_init(&cache, 1 << 20) == 0);

    vtx_method_registry_t registry;
    VTX_ASSERT_TRUE(vtx_method_registry_init(&registry, &arena) == 0);

    /* Build a 3-instruction method:
     *   PC 0: LOAD_CONST_INT 0   ; push consts[0] = SMI 42
     *   PC 3: CALL_RUNTIME 7     ; call our test callback (func_id=7)
     *   PC 6: RETURN_VALUE
     *
     * The JIT will execute this and invoke our callback at PC 3.
     * At that moment, on_stack_count should be 1 (proves on_enter ran). */
    static const uint8_t code_buf[] = {
        VT_OP_LOAD_CONST_INT, 0x00, 0x00,  /* const[0] */
        VT_OP_CALL_RUNTIME,  0x00, 0x07,  /* func_id = 7 */
        VT_OP_RETURN_VALUE,
    };
    vtx_value_t consts[1] = { vtx_make_smi(42) };
    vtx_bytecode_t bc = {
        .code = (uint8_t *)code_buf, .length = sizeof(code_buf),
        .constant_pool = consts, .constant_count = 1,
        .max_locals = 0, .max_stack = 4,
    };
    vtx_method_desc_t method = {
        .name = "osr22_test", .signature = "()I",
        .bytecode = &bc, .compiled_code = NULL,
        .vtable_index = 2200,  /* unique method_id */
        .arg_count = 0, .is_virtual = false,
    };

    /* Compile via the baseline JIT — produces real machine code. */
    vtx_compiled_code_t *cc = vtx_baseline_compile(&method, NULL,
                                                      &arena, &cache,
                                                      &registry);
    VTX_ASSERT_TRUE(cc != NULL);
    VTX_ASSERT_TRUE(method.compiled_code != NULL);

    /* Install a version in the versioned cache so on_enter/on_exit
     * have a target. The version wraps the same code pointer the
     * dispatch loop will jump to. */
    vtx_versioned_cache_t vc;
    VTX_ASSERT_TRUE(vtx_versioned_cache_init(&vc, &cache) == 0);
    uint32_t v_num = vtx_versioned_cache_install(&vc, method.vtable_index,
                                                    method.compiled_code, 64);
    VTX_ASSERT_TRUE(v_num >= 1);

    vtx_versioned_code_version_t *active_before =
        vtx_versioned_cache_get_active(&vc, method.vtable_index);
    VTX_ASSERT_TRUE(active_before != NULL);
    VTX_ASSERT_TRUE(active_before->on_stack_count == 0);

    /* Set up the interpreter with compile_ctx wired to the versioned cache. */
    vtx_interp_t interp;
    VTX_ASSERT_TRUE(vtx_interp_init(&interp, &ts, &gc) == 0);
    vtx_compile_context_t compile_ctx;
    VTX_ASSERT_TRUE(vtx_compile_context_init(&compile_ctx) == 0);
    compile_ctx.versioned_cache = &vc;
    vtx_interp_set_compile_ctx(&interp, &compile_ctx);

    /* Register the runtime callback and prime the test state. */
    vtx_set_runtime_callback(osr22_runtime_callback, NULL);
    g_test_vc = &vc;
    g_test_method_id = method.vtable_index;
    g_captured_on_stack_count = -1;
    g_captured_version_number = 0;
    g_captured_vc = NULL;

    /* Sanity: pre-run, on_stack_count is 0. */
    VTX_ASSERT_TRUE(active_before->on_stack_count == 0);

    /* Run the method. vtx_interp_run → vtx_dispatch_jit → JIT code →
     * our runtime callback observes the state, then JIT returns and
     * vtx_dispatch_jit calls on_exit. */
    vtx_value_t result = vtx_interp_run(&interp, &method, NULL, 0);
    (void)result;

    /* Detach callback to avoid contaminating other tests. */
    vtx_set_runtime_callback(NULL, NULL);
    g_test_vc = NULL;

    /* === OSR-22 contract assertions === */

    /* The callback ran DURING JIT execution — proves vtx_dispatch_jit
     * actually invoked the JIT code (so we observed on_stack_count
     * while the JIT was live). */
    VTX_ASSERT_TRUE(g_captured_vc == &vc);

    /* on_enter was called BEFORE the JIT code reached the callback.
     * Pre-fix: this would be 0 (on_enter never called). Post-fix: 1. */
    VTX_ASSERT_TRUE(g_captured_on_stack_count == 1);

    /* The captured version is the active version we installed. */
    VTX_ASSERT_TRUE(g_captured_version_number == v_num);

    /* on_exit was called AFTER the JIT returned.
     * Pre-fix: this would be 0 (because on_enter was never called).
     * Post-fix: 0 (because on_exit decremented it back from 1). */
    VTX_ASSERT_TRUE(active_before->on_stack_count == 0);

    /* Cleanup. */
    vtx_interp_set_compile_ctx(&interp, NULL);
    vtx_interp_destroy(&interp);
    vtx_compile_context_destroy(&compile_ctx);
    vtx_versioned_cache_destroy(&vc);
    vtx_code_cache_destroy(&cache);
    vtx_method_registry_destroy(&registry);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
    vtx_arena_destroy(&arena);
}

/* ========================================================================== */
/* Test: retired code is NOT freed while a thread is on its stack              */
/* ========================================================================== */
/* This is the OSR-22 invariant that the on_enter/on_exit wiring protects.
 * Already covered by test_versioned_cache.c::versioned_retired_not_reclaimed_while_on_stack,
 * but we re-verify it here as part of the OSR-22 contract. */

VTX_TEST(osr22_retired_version_survives_reclaim_during_execution) {
    vtx_code_cache_t cache;
    VTX_ASSERT_TRUE(vtx_code_cache_init(&cache, 1 << 20) == 0);
    vtx_versioned_cache_t vc;
    VTX_ASSERT_TRUE(vtx_versioned_cache_init(&vc, &cache) == 0);

    void *code1 = vtx_code_cache_alloc(&cache, 64);
    memset(code1, 0x90, 64);
    void *code2 = vtx_code_cache_alloc(&cache, 64);
    memset(code2, 0xCC, 64);
    vtx_code_cache_finalize(&cache);

    /* Install v1, simulate a thread entering it, then install v2. */
    vtx_versioned_cache_install(&vc, 314, code1, 64);
    vtx_versioned_cache_on_enter(&vc, 314);   /* thread on v1's stack */
    vtx_versioned_cache_install(&vc, 314, code2, 64);  /* v1 retired */

    /* Reclaim must NOT free v1 (on_stack_count > 0). */
    uint32_t reclaimed = vtx_versioned_cache_reclaim(&vc);
    VTX_ASSERT_TRUE(reclaimed == 0);
    VTX_ASSERT_TRUE(vc.total_retired == 1);

    /* Thread exits v1. */
    vtx_versioned_cache_on_exit(&vc, 314);

    /* Now reclaim should free v1. */
    reclaimed = vtx_versioned_cache_reclaim(&vc);
    VTX_ASSERT_TRUE(reclaimed == 1);
    VTX_ASSERT_TRUE(vc.total_retired == 0);

    vtx_versioned_cache_destroy(&vc);
    vtx_code_cache_destroy(&cache);
}

int main(void) {
    printf("=== OSR-22 regression: versioned cache on_enter/on_exit wiring ===\n\n");
    vtx_test_run_all();
    return 0;
}
