/*
 * VORTEX OSR-17 Regression Test
 *
 * Bug: The dispatch loop's OSR trigger only fired on VT_OP_GOTO
 *      backedges. `for` and `while` loops typically compile to
 *      IF_TRUE / IF_FALSE backedges (the loop condition is a forward
 *      branch to the body, and the backedge is IF_FALSE to exit or
 *      IF_TRUE to continue). Without checking osr_pending at these
 *      backedges, many common loop patterns never OSR — they stay
 *      in the interpreter forever.
 *
 * Fix: src/interp/dispatch.c now checks `osr_pending` at all
 *      backward branches (IF_TRUE, IF_FALSE, GOTO). The codegen's
 *      scan_loop_headers (src/baseline/codegen.c) already pre-scans
 *      all three opcodes to identify loop headers and emits
 *      VTX_STF_OSR_ENTRY side-table entries at each.
 *
 * Test:
 *
 *  PART A — codegen side (loop-header pre-scan):
 *    Compile a method whose backedge is IF_TRUE (not GOTO). Verify
 *    the codegen's scan_loop_headers identified the IF_TRUE target
 *    as a loop header and emitted a VTX_STF_OSR_ENTRY side-table
 *    entry at that PC. This is the precondition for OSR-up at
 *    IF_TRUE/IF_FALSE backedges — without this entry, the dispatch
 *    loop's OSR-up site would find no entry and fall back to
 *    whole-method re-enter.
 *
 *  PART B — dispatch side (runtime trigger):
 *    Build a method with an IF_TRUE backedge loop whose body contains
 *    a CALL_RUNTIME hook. Register a runtime callback that, on the
 *    first invocation, JIT-compiles the method (which atomically
 *    installs compiled_code). The next IF_TRUE backedge in the
 *    interpreter dispatch loop must now see compiled_code != NULL
 *    and set interp->osr_pending = true (and osr_loop_header_pc =
 *    loop_header_pc).
 *
 *    Per the CRITICAL REPRODUCER CONSTRAINT: the OSR-up asm
 *    trampoline is brittle to exercise in isolation (it expects a
 *    fully-formed JIT frame). This test verifies the OSR-17 contract
 *    — that IF_TRUE backedges set osr_pending and osr_loop_header_pc
 *    — by observing interp state after the dispatch loop exits at
 *    the backedge. The test does NOT depend on the OSR-up asm
 *    running successfully: osr_pending is cleared by the OSR-up
 *    site (regardless of success or failure), but osr_loop_header_pc
 *    is NOT cleared — it retains the value set at the IF_TRUE
 *    backedge. This is the deterministic signal that OSR-17 fired.
 */

#include "osr_test_setup.h"
#include "baseline/codegen.h"
#include "codecache/install.h"
#include "codecache/versioned.h"
#include "compile/request.h"
#include "interp/dispatch.h"
#include "runtime/object.h"
#include "runtime/helpers.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Loop header PC for the test method. The IF_TRUE at PC 23 (PART A)
 * or PC 26 (PART B) targets this PC, making it a backward branch. */
#define OSR17_LOOP_HEADER_PC  6

/* Runtime callback func_id used by PART B's CALL_RUNTIME. */
#define OSR17_CALLBACK_FUNC_ID 7

/* Test fixture: globals captured by the runtime callback. */
static vtx_method_desc_t *g_method       = NULL;
static vtx_arena_t       *g_arena         = NULL;
static vtx_code_cache_t *g_cache         = NULL;
static vtx_method_registry_t *g_registry  = NULL;
static int                g_callback_fired = 0;
static int                g_compiled_in_callback = 0;

/* Runtime callback: invoked from the interpreter's CALL_RUNTIME
 * handler. On the first invocation, JIT-compiles the method
 * (which atomically sets method->compiled_code via vtx_install_method).
 * This makes the subsequent IF_TRUE backedge see compiled_code != NULL
 * and fire the OSR-17 trigger. */
static int osr17_runtime_callback(uint32_t func_id,
                                    vtx_value_t **sp_ptr,
                                    void *user_data)
{
    (void)user_data;
    if (func_id != OSR17_CALLBACK_FUNC_ID || g_method == NULL) {
        return -1;  /* decline */
    }
    g_callback_fired++;

    /* Compile the method the first time the callback fires. This
     * installs compiled_code atomically (vtx_install_method). The
     * next IF_TRUE backedge in the interpreter will see this and
     * set osr_pending — which is the OSR-17 fix. */
    if (g_compiled_in_callback == 0 && g_arena != NULL &&
        g_cache != NULL && g_registry != NULL) {
        vtx_compiled_code_t *cc = vtx_baseline_compile(
            g_method, NULL, g_arena, g_cache, g_registry);
        if (cc != NULL) {
            g_compiled_in_callback = 1;
            /* The cc struct wrapper is leaked here — its underlying
             * code is installed in the cache and the cm is in the
             * registry. The cc wrapper itself is arena-allocated and
             * will be freed at arena teardown. */
        }
    }

    /* Push a value onto the operand stack so RETURN_VALUE has
     * something to return if OSR-up fails and the loop exits. */
    (*sp_ptr)[0] = VTX_VALUE_UNDEFINED;
    *sp_ptr += 1;
    return 1;
}

/* ========================================================================== */
/* PART A: codegen side — IF_TRUE backedge registers side-table OSR entry      */
/* ========================================================================== */

/*
 * Test method (PART A — codegen only, no runtime callback):
 *   PC 0: LOAD_CONST_INT 0    ; push 0       (3 bytes)
 *   PC 3: STORE_LOCAL 0        ; locals[0] = 0 (3 bytes)
 *   PC 6: LOAD_LOCAL 0         ; push counter  (3 bytes) ← LOOP HEADER
 *   PC 9: LOAD_CONST_INT 1     ; push 2        (3 bytes)  (const_pool[1]=2)
 *   PC 12: IADD                  ; counter+1     (1 byte)
 *   PC 13: STORE_LOCAL 0        ; counter = +1  (3 bytes)
 *   PC 16: LOAD_LOCAL 0         ; push counter  (3 bytes)
 *   PC 19: LOAD_CONST_INT 0    ; push 2        (3 bytes)  (const_pool[1]=2)
 *   PC 22: ICMP_LT              ; counter<2     (1 byte)
 *   PC 23: IF_TRUE 6            ; if true, goto PC 6 (3 bytes) ← backedge
 *   PC 26: LOAD_LOCAL 0         ; push counter  (3 bytes)
 *   PC 29: RETURN_VALUE         (1 byte)
 *
 * The IF_TRUE at PC 23 targets PC 6 (backward branch), so PC 6 is a
 * loop header. The codegen's scan_loop_headers handles IF_TRUE, so
 * a VTX_STF_OSR_ENTRY side-table entry must be present at PC 6.
 */
static void build_if_true_loop_method(vtx_bytecode_t *bc, uint8_t *code_buf,
                                        vtx_value_t *const_pool)
{
    uint32_t pc = 0;
    code_buf[pc] = VT_OP_LOAD_CONST_INT; code_buf[pc+1] = 0; code_buf[pc+2] = 0; pc += 3;
    code_buf[pc] = VT_OP_STORE_LOCAL;    code_buf[pc+1] = 0; code_buf[pc+2] = 0; pc += 3;
    /* loop header at PC=6 */
    code_buf[pc] = VT_OP_LOAD_LOCAL;     code_buf[pc+1] = 0; code_buf[pc+2] = 0; pc += 3;
    code_buf[pc] = VT_OP_LOAD_CONST_INT; code_buf[pc+1] = 0; code_buf[pc+2] = 1; pc += 3;
    code_buf[pc] = VT_OP_IADD; pc += 1;
    code_buf[pc] = VT_OP_STORE_LOCAL;    code_buf[pc+1] = 0; code_buf[pc+2] = 0; pc += 3;
    code_buf[pc] = VT_OP_LOAD_LOCAL;     code_buf[pc+1] = 0; code_buf[pc+2] = 0; pc += 3;
    code_buf[pc] = VT_OP_LOAD_CONST_INT; code_buf[pc+1] = 0; code_buf[pc+2] = 1; pc += 3;
    code_buf[pc] = VT_OP_ICMP_LT; pc += 1;
    code_buf[pc] = VT_OP_IF_TRUE;        code_buf[pc+1] = 0; code_buf[pc+2] = OSR17_LOOP_HEADER_PC; pc += 3;
    code_buf[pc] = VT_OP_LOAD_LOCAL;     code_buf[pc+1] = 0; code_buf[pc+2] = 0; pc += 3;
    code_buf[pc] = VT_OP_RETURN_VALUE; pc += 1;

    bc->code = code_buf; bc->length = pc;
    bc->constant_pool = const_pool; bc->constant_count = 2;
    bc->max_locals = 1; bc->max_stack = 4;
}

VTX_TEST(osr17_codegen_records_osr_entry_for_if_true_backedge)
{
    vtx_arena_t arena; vtx_arena_init(&arena);
    vtx_type_system_t ts; vtx_type_system_init(&ts);
    vtx_gc_t gc; vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);

    uint8_t *code_buf = vtx_arena_alloc(&arena, 32);
    /* const_pool[1] = 2 → loop condition is `counter < 2` (true on iter 1). */
    vtx_value_t const_pool[2] = { vtx_make_smi(0), vtx_make_smi(2) };
    vtx_bytecode_t bc;
    build_if_true_loop_method(&bc, code_buf, const_pool);

    vtx_method_desc_t method = {
        .name = "osr17a", .signature = "()I",
        .bytecode = &bc, .compiled_code = NULL,
        .vtable_index = 1700, .arg_count = 0, .is_virtual = false,
    };

    vtx_code_cache_t cache; vtx_code_cache_init(&cache, 1 << 16);
    vtx_method_registry_t registry; vtx_method_registry_init(&registry, &arena);
    vtx_compiled_code_t *compiled = vtx_baseline_compile(
        &method, NULL, &arena, &cache, &registry);
    VTX_ASSERT_NOT_NULL(compiled);

    vtx_compiled_method_t *cm = vtx_method_registry_get(&registry, method.vtable_index);
    VTX_ASSERT_NOT_NULL(cm);
    VTX_ASSERT_NOT_NULL(cm->side_table);

    /* Scan the side_table for an OSR entry at loop_header_pc=6.
     * The codegen's scan_loop_headers pre-scans VT_OP_IF_TRUE
     * (alongside GOTO and IF_FALSE) — without this, the side-table
     * lookup_osr_entry would return NULL and OSR-up would never
     * find an entry for IF_TRUE-based loop headers. */
    const vtx_side_table_entry_t *osr_e =
        vtx_side_table_lookup_osr_entry(cm->side_table, OSR17_LOOP_HEADER_PC);
    VTX_ASSERT_NOT_NULL(osr_e);
    VTX_ASSERT_TRUE((osr_e->flags & VTX_STF_OSR_ENTRY) != 0);
    VTX_ASSERT_EQUAL(osr_e->bytecode_pc, (uint32_t)OSR17_LOOP_HEADER_PC);

    vtx_compiled_code_destroy(compiled);
    vtx_method_registry_destroy(&registry);
    vtx_code_cache_destroy(&cache);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
    vtx_arena_destroy(&arena);
}

/* ========================================================================== */
/* PART B: dispatch side — IF_TRUE backedge sets osr_pending at runtime        */
/* ========================================================================== */

/*
 * Test method (PART B — runtime):
 *   PC 0: LOAD_CONST_INT 0    ; push 0       (3 bytes)
 *   PC 3: STORE_LOCAL 0        ; locals[0] = 0 (3 bytes)
 *   PC 6: LOAD_LOCAL 0         ; push counter  (3 bytes) ← LOOP HEADER
 *   PC 9: LOAD_CONST_INT 1     ; push 2        (3 bytes)  (const_pool[1]=2)
 *   PC 12: IADD                  ; counter+1     (1 byte)
 *   PC 13: STORE_LOCAL 0        ; counter = +1  (3 bytes)
 *   PC 16: CALL_RUNTIME 7       ; invoke callback (3 bytes)
 *   PC 19: LOAD_LOCAL 0         ; push counter  (3 bytes)
 *   PC 22: LOAD_CONST_INT 1    ; push 2        (3 bytes)  (const_pool[1]=2)
 *   PC 25: ICMP_LT              ; counter<2     (1 byte)
 *   PC 26: IF_TRUE 6            ; if true, goto PC 6 (3 bytes) ← backedge
 *   PC 29: LOAD_LOCAL 0         ; push counter  (3 bytes)
 *   PC 32: RETURN_VALUE         (1 byte)
 *
 * The condition `counter < 2` is true on iteration 1 (counter=1), so
 * the IF_TRUE backedge fires. The callback at PC 16 compiles the
 * method, so compiled_code is set when the backedge is reached.
 * The OSR-17 fix at the IF_TRUE handler must set osr_pending.
 */
static void build_if_true_loop_with_callback(vtx_bytecode_t *bc,
                                                uint8_t *code_buf,
                                                vtx_value_t *const_pool)
{
    uint32_t pc = 0;
    code_buf[pc] = VT_OP_LOAD_CONST_INT; code_buf[pc+1] = 0; code_buf[pc+2] = 0; pc += 3;
    code_buf[pc] = VT_OP_STORE_LOCAL;    code_buf[pc+1] = 0; code_buf[pc+2] = 0; pc += 3;
    /* loop header at PC=6 */
    code_buf[pc] = VT_OP_LOAD_LOCAL;     code_buf[pc+1] = 0; code_buf[pc+2] = 0; pc += 3;
    code_buf[pc] = VT_OP_LOAD_CONST_INT; code_buf[pc+1] = 0; code_buf[pc+2] = 1; pc += 3;
    code_buf[pc] = VT_OP_IADD; pc += 1;
    code_buf[pc] = VT_OP_STORE_LOCAL;    code_buf[pc+1] = 0; code_buf[pc+2] = 0; pc += 3;
    code_buf[pc] = VT_OP_CALL_RUNTIME;   code_buf[pc+1] = 0; code_buf[pc+2] = OSR17_CALLBACK_FUNC_ID; pc += 3;
    code_buf[pc] = VT_OP_LOAD_LOCAL;     code_buf[pc+1] = 0; code_buf[pc+2] = 0; pc += 3;
    code_buf[pc] = VT_OP_LOAD_CONST_INT; code_buf[pc+1] = 0; code_buf[pc+2] = 1; pc += 3;
    code_buf[pc] = VT_OP_ICMP_LT; pc += 1;
    code_buf[pc] = VT_OP_IF_TRUE;        code_buf[pc+1] = 0; code_buf[pc+2] = OSR17_LOOP_HEADER_PC; pc += 3;
    code_buf[pc] = VT_OP_LOAD_LOCAL;     code_buf[pc+1] = 0; code_buf[pc+2] = 0; pc += 3;
    code_buf[pc] = VT_OP_RETURN_VALUE; pc += 1;

    bc->code = code_buf; bc->length = pc;
    bc->constant_pool = const_pool; bc->constant_count = 2;
    bc->max_locals = 1; bc->max_stack = 4;
}

VTX_TEST(osr17_if_true_backedge_sets_osr_loop_header_pc)
{
    vtx_arena_t arena; vtx_arena_init(&arena);
    vtx_type_system_t ts; vtx_type_system_init(&ts);
    vtx_gc_t gc; vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);

    uint8_t *code_buf = vtx_arena_alloc(&arena, 48);
    /* const_pool[1] = 2 → condition `counter < 2` is true on iter 1
     * (counter=1 after the IADD), so the IF_TRUE backedge fires. */
    vtx_value_t const_pool[2] = { vtx_make_smi(0), vtx_make_smi(2) };
    vtx_bytecode_t bc;
    build_if_true_loop_with_callback(&bc, code_buf, const_pool);

    vtx_method_desc_t method = {
        .name = "osr17b", .signature = "()I",
        .bytecode = &bc, .compiled_code = NULL,
        .vtable_index = 1701, .arg_count = 0, .is_virtual = false,
    };

    vtx_code_cache_t cache; vtx_code_cache_init(&cache, 1 << 16);
    vtx_method_registry_t registry; vtx_method_registry_init(&registry, &arena);

    vtx_interp_t interp;
    VTX_ASSERT_TRUE(vtx_interp_init(&interp, &ts, &gc) == 0);
    vtx_compile_context_t compile_ctx;
    VTX_ASSERT_TRUE(vtx_compile_context_init(&compile_ctx) == 0);
    compile_ctx.method_registry = &registry;
    vtx_interp_set_compile_ctx(&interp, &compile_ctx);

    /* Register the runtime callback and prime the globals it reads. */
    vtx_set_runtime_callback(osr17_runtime_callback, NULL);
    g_method = &method;
    g_arena = &arena;
    g_cache = &cache;
    g_registry = &registry;
    g_callback_fired = 0;
    g_compiled_in_callback = 0;

    /* Sanity: pre-run, osr_loop_header_pc is 0 and osr_pending is false. */
    VTX_ASSERT_FALSE(interp.osr_pending);
    VTX_ASSERT_EQUAL(interp.osr_loop_header_pc, 0u);

    /* Run the method. The interpreter enters the dispatch loop with
     * compiled_code == NULL (no JIT yet). On iteration 1:
     *   1. Body runs: counter goes 0 → 1.
     *   2. CALL_RUNTIME fires our callback, which JIT-compiles the
     *      method (compiled_code is now non-NULL atomically).
     *   3. The IF_TRUE at PC 26 evaluates (1 < 2) == true → backedge.
     *   4. The OSR-17 fix at the IF_TRUE handler sees compiled_code
     *      != NULL && compile_ctx != NULL → sets osr_pending = true,
     *      osr_loop_header_pc = 6, exits the dispatch loop.
     *   5. The OSR-up site runs vtx_osr_up. Whether it succeeds (asm
     *      jumps to JIT) or fails (gate refuses), osr_loop_header_pc
     *      retains the value 6.
     *
     * Per the CRITICAL REPRODUCER CONSTRAINT note above: we don't
     * depend on the OSR-up asm running successfully. The deterministic
     * signal is osr_loop_header_pc == 6 after vtx_interp_run returns. */
    vtx_value_t result = vtx_interp_run(&interp, &method, NULL, 0);
    (void)result;

    /* Detach callback and clear globals. */
    vtx_set_runtime_callback(NULL, NULL);
    g_method = NULL;
    g_arena = NULL;
    g_cache = NULL;
    g_registry = NULL;

    /* === OSR-17 assertions === */

    /* 1. The callback fired — proving the interpreter dispatch loop
     *    actually executed the loop body (we're not just hitting a
     *    short-circuit). */
    VTX_ASSERT_TRUE(g_callback_fired >= 1);

    /* 2. The callback compiled the method (compiled_code is set). */
    VTX_ASSERT_TRUE(g_compiled_in_callback == 1);
    VTX_ASSERT_TRUE(__atomic_load_n(&method.compiled_code, __ATOMIC_ACQUIRE) != NULL);

    /* 3. The OSR-17 fix: the IF_TRUE backedge set osr_loop_header_pc
     *    to the loop header PC (6). Pre-fix, only GOTO backedges
     *    would set this — and there's no GOTO in this method, so
     *    pre-fix osr_loop_header_pc would remain 0.
     *
     *    Note: osr_pending itself is cleared by the OSR-up site (which
     *    runs after the dispatch loop exits). osr_loop_header_pc is
     *    NOT cleared — it retains the value set at the IF_TRUE backedge.
     *    This is the deterministic signal that OSR-17 fired.
     *
     *    If the OSR-up succeeded (asm jumped to JIT and JIT returned),
     *    osr_loop_header_pc still holds 6. If the OSR-up failed (gate
     *    refused), osr_loop_header_pc still holds 6. Either way, the
     *    assertion verifies the IF_TRUE backedge triggered the OSR
     *    path. */
    VTX_ASSERT_EQUAL(interp.osr_loop_header_pc, (uint32_t)OSR17_LOOP_HEADER_PC);

    /* Cleanup. */
    vtx_interp_set_compile_ctx(&interp, NULL);
    vtx_interp_destroy(&interp);
    vtx_compile_context_destroy(&compile_ctx);
    vtx_method_registry_destroy(&registry);
    vtx_code_cache_destroy(&cache);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
    vtx_arena_destroy(&arena);
}

int main(void)
{
    vtx_test_result_t result = vtx_test_run_all();
    printf("\nOSR-17 regression: %u passed, %u failed, %u total\n",
           result.pass_count, result.fail_count, result.total_count);
    return (result.fail_count > 0) ? 1 : 0;
}
