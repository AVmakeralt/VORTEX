/*
 * OSR-4 regression: stack walker must read the return address from
 *                    [RBP+32], not [RBP+8] (profile_data).
 *
 * Bug: vtx_stack_walk_read_return_addr read from `fp + sizeof(void*)`
 *      (= [fp+8]), which is the profile_data slot in the JIT frame
 *      header — not the return address (which lives at [fp+32] per
 *      baseline/frame_layout.h:73-77).
 *
 * Impact: every caller of vtx_stack_walk_read_return_addr got back
 *         profile_data (usually NULL) instead of the return address,
 *         breaking GC root scanning, deopt frame-chain reconstruction,
 *         and JIT-frame classification.
 *
 * Reproducer: build a synthetic 5-word JIT frame header on the C stack,
 *             populate each slot with a distinct sentinel, and verify
 *             that vtx_stack_walk_read_return_addr returns the value
 *             placed at [RBP+32] (the VTX_FRAME_RETURN_ADDR_OFFSET).
 */

#include "test_framework.h"
#include "deopt/stack_walk.h"
#include "baseline/frame_layout.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>

VTX_TEST(osr4_read_return_addr_from_correct_offset) {
    /* Build a synthetic JIT frame header on the C stack.
     * The JIT pushes 5 words above RBP:
     *   [RBP+0]  = caller RBP
     *   [RBP+8]  = profile_data
     *   [RBP+16] = deopt_info
     *   [RBP+24] = method_ptr
     *   [RBP+32] = return address
     *
     * We use distinct sentinel values for each slot so that an
     * off-by-N read is obvious. */
    void *sentinel_caller_rbp   = (void *)0xAAA0;
    void *sentinel_profile_data = (void *)0xBBB1;  /* <- BUG reads this */
    void *sentinel_deopt_info   = (void *)0xCCC2;
    void *sentinel_method_ptr   = (void *)0xDDD3;
    void *sentinel_return_addr  = (void *)0xEEE4;  /* <- FIX should read this */

    /* Lay the 5 words out contiguously in memory, with [0] being the
     * "caller RBP" slot (i.e., the address we pass as `fp`). */
    void *frame[5];
    frame[VTX_FRAME_CALLER_RBP_OFFSET    / sizeof(void *)] = sentinel_caller_rbp;
    frame[VTX_FRAME_PROFILE_DATA_OFFSET  / sizeof(void *)] = sentinel_profile_data;
    frame[VTX_FRAME_DEOPT_INFO_OFFSET    / sizeof(void *)] = sentinel_deopt_info;
    frame[VTX_FRAME_METHOD_PTR_OFFSET    / sizeof(void *)] = sentinel_method_ptr;
    frame[VTX_FRAME_RETURN_ADDR_OFFSET   / sizeof(void *)] = sentinel_return_addr;

    /* Sanity check on the layout constants. */
    VTX_ASSERT_TRUE(VTX_FRAME_RETURN_ADDR_OFFSET == 32);

    /* Call the function under test. */
    void *out_return_addr = NULL;
    int rc = vtx_stack_walk_read_return_addr(frame, &out_return_addr);

    VTX_ASSERT_TRUE(rc == 0);
    VTX_ASSERT_TRUE(out_return_addr == sentinel_return_addr);

    /* The bug specifically returned sentinel_profile_data instead.
     * Make this an explicit negative assertion so the test stays
     * meaningful even if the constants change. */
    VTX_ASSERT_TRUE(out_return_addr != sentinel_profile_data);
}

VTX_TEST(osr4_read_caller_fp_still_reads_fp_zero) {
    /* Caller RBP is at [fp+0] — this wasn't broken by OSR-4, but
     * verify it explicitly so future changes don't accidentally break
     * it while fixing the return-addr offset. */
    void *sentinel_caller_rbp = (void *)0x1234;
    void *sentinel_profile   = (void *)0x5678;
    void *sentinel_deopt      = (void *)0x9ABC;
    void *sentinel_method     = (void *)0xDEF0;
    void *sentinel_retaddr    = (void *)0x1357;

    void *frame[5] = { sentinel_caller_rbp, sentinel_profile,
                       sentinel_deopt, sentinel_method, sentinel_retaddr };

    void *out_caller_fp = NULL;
    int rc = vtx_stack_walk_read_caller_fp(frame, &out_caller_fp);

    VTX_ASSERT_TRUE(rc == 0);
    VTX_ASSERT_TRUE(out_caller_fp == sentinel_caller_rbp);
}

int main(void) {
    printf("=== OSR-4 regression: stack-walker return-address offset ===\n\n");
    vtx_test_run_all();
    return 0;
}
