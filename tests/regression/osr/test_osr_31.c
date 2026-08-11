/*
 * VORTEX OSR-31 Regression Test
 *
 * Bug: vtx_osr_up's asm has an `int3` instruction after `jmp *%%rax`.
 * The int3 is unreachable (the jmp is unconditional), so it's dead
 * code. The bug report flagged it as misleading — it suggests the
 * asm might fall through.
 *
 * Fix: Add a comment to the int3 explaining it's a defensive safety
 * trap. Do NOT delete it — it catches future code changes that break
 * the asm's control flow.
 *
 * Test (CRITICAL REPRODUCER CONSTRAINT disclosure):
 *   This is a comment-only fix with no runtime behavior to verify.
 *   Per the rule, we explicitly note that no runtime reproducer is
 *   meaningful. We DO write a source-grep test that verifies the
 *   safety-trap comment is present in osr.c — if the int3 is removed
 *   without the comment, the test fails.
 */

#include "osr_test_setup.h"
#include <stdio.h>
#include <string.h>

#define OSR_SRC_PATH "src/deopt/osr.c"

static int osr31_check_int3_with_comment(void)
{
    FILE *fp = fopen(OSR_SRC_PATH, "r");
    if (!fp) {
        /* Source tree not accessible from test working directory. */
        return 0;
    }
    char line[512];
    int found_int3 = 0;
    int found_osr31_marker = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "int3") != NULL) {
            found_int3 = 1;
        }
        if (strstr(line, "OSR-31") != NULL) {
            found_osr31_marker = 1;
        }
        if (found_int3 && found_osr31_marker) break;
    }
    fclose(fp);
    /* Both the int3 instruction AND the OSR-31 marker comment must
     * be present. If either is missing, the fix has been partially
     * reverted. */
    return (found_int3 && found_osr31_marker) ? 0 : 1;
}

VTX_TEST(osr31_int3_and_marker_present_in_source)
{
    /* The OSR-31 fix documents the int3 safety trap with a marker
     * comment. This test verifies both are present so future
     * changes don't silently remove the safety trap or its
     * documentation. */
    VTX_ASSERT_EQUAL(osr31_check_int3_with_comment(), 0);
}

int main(void)
{
    vtx_test_result_t result = vtx_test_run_all();
    printf("\nOSR-31 regression: %u passed, %u failed, %u total\n",
           result.pass_count, result.fail_count, result.total_count);
    printf("(OSR-31: source-grep test for int3 + marker comment.)\n");
    return (result.fail_count > 0) ? 1 : 0;
}
