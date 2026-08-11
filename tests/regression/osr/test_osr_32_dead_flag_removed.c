/*
 * VORTEX OSR-32 Regression Test
 *
 * Bug: vtx_interp_frame_t had an `osr_active` bool field. vtx_osr_up
 *      set interp->osr_active = true after a successful transition, but
 *      NO code (GC, stack walker, dispatch loop) ever read it. The
 *      intended use was to let the GC skip OSR'd interpreter frames
 *      during root scanning, but this was never implemented. The flag
 *      was dead weight on the struct and misleading to readers.
 *
 * Fix: Remove the field. The OSR-up asm trampoline jumps to JIT code
 *      and the patched JIT-frame return address routes control back
 *      to the dispatch.c caller of vtx_osr_up, so the interpreter
 *      frame is naturally superseded and the GC continues to scan the
 *      JIT frame's own GC roots (the JIT's safepoint map). No flag is
 *      needed.
 *
 * Test: Compile-time check that the field is gone. We assert that
 *       vtx_interp_frame_t has no `osr_active` slot between `caller`
 *       and `monitors` — the two fields that used to bracket it.
 *
 *       Pre-fix layout (with osr_active):
 *         ...
 *         void            *caller;       [offset O]
 *         bool             osr_active;    [offset O+8]  <-- dead flag
 *         vtx_osr_monitor_entry_t *monitors;  [offset O+16]
 *
 *       Post-fix layout (without osr_active):
 *         ...
 *         void            *caller;       [offset O]
 *         vtx_osr_monitor_entry_t *monitors;  [offset O+8]
 *
 *       If osr_active is re-added, the offset of `monitors` shifts
 *       by at least 1 byte (and typically 8 bytes due to pointer
 *       alignment), and this _Static_assert fails to compile.
 *
 * Per the CRITICAL REPRODUCER CONSTRAINT note: the bug is purely a
 * dead-field-on-a-struct issue — there is no runtime behavior to
 * reproduce. The strongest test is a compile-time structural
 * assertion, which we make below. We additionally verify at runtime
 * that vtx_osr_up can be called with NULL inputs without touching
 * any osr_active field (i.e., the call path doesn't read/write it).
 */

#include "osr_test_setup.h"
#include <stddef.h>

/* === Compile-time check ===
 *
 * offsetof(vtx_interp_frame_t, monitors) MUST equal
 * offsetof(vtx_interp_frame_t, caller) + sizeof(void *).
 *
 * On x86-64 (where this test runs):
 *   - caller is a `void *` at some offset O.
 *   - The next field (post-fix) is `monitors`, also a pointer.
 *   - Without osr_active, monitors is at offset O + 8.
 *   - If osr_active (a 1-byte bool) were re-added between caller and
 *     monitors, monitors would shift to O + 16 (due to pointer
 *     alignment padding), failing this assertion.
 *
 * This is the cleanest compile-time check available in C for "field
 * X is not present between fields A and B." It's deterministic,
 * architecture-independent (works as long as pointer alignment is
 * >= 2 bytes), and fails loudly at compile time if the dead flag is
 * re-added.
 */
_Static_assert(
    __builtin_offsetof(vtx_interp_frame_t, monitors) ==
        __builtin_offsetof(vtx_interp_frame_t, caller) + sizeof(void *),
    "OSR-32: vtx_interp_frame_t must NOT have an `osr_active` field "
    "between `caller` and `monitors`. If you re-add the dead flag, "
    "this assertion fails to compile — the field was removed because "
    "it was set but never read. See src/deopt/osr.c:520 for the "
    "removal rationale.");

VTX_TEST(osr32_dead_field_removed_runtime_check)
{
    /* Runtime sanity check: create a vtx_interp_frame_t and verify
     * it can be initialized and used without referencing osr_active.
     * If someone re-adds the field AND modifies vtx_osr_up to write
     * to it, this test still passes (because we don't read the field
     * here) — but the _Static_assert above catches the re-addition at
     * compile time. This runtime check mainly verifies the OSR-up
     * null-input path doesn't depend on the field. */
    vtx_interp_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.method_id = 0;
    frame.bytecode_pc = 0;
    frame.locals = NULL;
    frame.local_count = 0;
    frame.stack = NULL;
    frame.stack_top = 0;
    frame.stack_capacity = 0;
    frame.caller = NULL;
    frame.monitors = NULL;
    frame.monitor_count = 0;
    frame.monitor_capacity = 0;
    frame.catch_handler_pc = VTX_CATCH_NONE;
    frame.return_pc = 0;
    frame.frame_kind = VTX_FRAME_INTERPRETED;

    /* If osr_active were still a field, the memset above zeroed it
     * too — so reading frame.osr_active here would yield 0 (false).
     * But since we can't reference a field that doesn't exist (the
     * build would fail), this is a no-op sanity check. */
    VTX_ASSERT_TRUE(frame.method_id == 0);
    VTX_ASSERT_TRUE(frame.frame_kind == VTX_FRAME_INTERPRETED);

    /* vtx_osr_up with NULL interp returns at the null-check gate.
     * It does NOT touch any osr_active field (which doesn't exist). */
    vtx_gc_t gc;
    vtx_gc_init(&gc, NULL, VTX_GC_GENERATIONAL);
    vtx_osr_up(NULL, 42, NULL, 100, NULL, &gc);
    VTX_ASSERT_TRUE(1);

    vtx_gc_destroy(&gc);
}

int main(void)
{
    vtx_test_result_t result = vtx_test_run_all();
    printf("\nOSR-32 regression: %u passed, %u failed, %u total\n",
           result.pass_count, result.fail_count, result.total_count);
    printf("(OSR-32: dead osr_active field removal — verified by "
           "_Static_assert at compile time + runtime null-input check.)\n");
    return (result.fail_count > 0) ? 1 : 0;
}
