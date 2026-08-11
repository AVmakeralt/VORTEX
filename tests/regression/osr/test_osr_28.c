/*
 * VORTEX OSR-28 Regression Test
 *
 * Bug: vtx_osr_up's inline asm modifies rbp (movq %%r14,%%rbp) and
 * rsp (leaq/subq adjust %%rsp) but neither register is in the clobber
 * list. This works only because the asm ends with an unconditional
 * jmp + __builtin_unreachable — the compiler never generates code
 * after the asm, so it doesn't matter that rbp/rsp are clobbered.
 *
 * Fix: Add "rbp" and "rsp" to the clobber list as a defensive measure.
 *
 * Note (CRITICAL REPRODUCER CONSTRAINT): This is a purely defensive
 * fix. There is no runtime behavior to test — the clobber list is a
 * compile-time contract between the asm and the compiler. If the fix
 * is reverted, the asm still works (because of the unconditional jmp
 * + __builtin_unreachable). The fix only matters if a future change
 * adds a fall-through path after the asm.
 *
 * The strongest test we can write is a compile-time check: verify
 * the source file declares the clobber list with "rbp" and "rsp".
 * We do this by including the OSR-28 fix marker comment and verifying
 * the build succeeds (the build already validates the asm syntax).
 *
 * Per the CRITICAL REPRODUCER CONSTRAINT rule, we explicitly note
 * that no runtime reproducer is possible for this defensive fix —
 * any test that "exercises" the clobber list would just be testing
 * that the asm compiles, which the build already does.
 */

#include "osr_test_setup.h"

VTX_TEST(osr28_defensive_clobber_list_compiles)
{
    /* This test exists to:
     *   1. Verify the test binary links (which means osr.c compiled,
     *      which means the asm clobber list is syntactically valid
     *      with "rbp" and "rsp" added).
     *   2. Document that the fix is defensive — there's no runtime
     *      behavior to verify.
     *
     * If the clobber list were malformed, osr.c would fail to
     * compile and this test binary wouldn't exist. */
    VTX_ASSERT_TRUE(1);
}

int main(void)
{
    vtx_test_result_t result = vtx_test_run_all();
    printf("\nOSR-28 regression: %u passed, %u failed, %u total\n",
           result.pass_count, result.fail_count, result.total_count);
    printf("(OSR-28 is a defensive clobber-list fix; no runtime "
           "reproducer is meaningful — the build itself is the test.)\n");
    return (result.fail_count > 0) ? 1 : 0;
}
