/*
 * VORTEX OSR-5 Regression Test
 *
 * Bug: The side-table OSR entry lookup at osr.c:247-260 picked the FIRST
 * entry with VTX_STF_OSR_ENTRY regardless of which loop header it
 * belonged to. A method with multiple OSR entry points (one per loop
 * header) would always enter the JIT at the first loop, not the right
 * one.
 *
 * Fix: Add a `bytecode_pc` field to vtx_side_table_entry_t and a new
 * vtx_side_table_lookup_osr_entry(table, bytecode_pc) function that
 * filters by both flag AND bytecode_pc.
 *
 * Test: Construct a side table with TWO OSR entries at different
 * bytecode_pcs. Verify that lookup_osr_entry returns the entry matching
 * the requested bytecode_pc, not just the first OSR-flagged entry.
 */

#include "osr_test_setup.h"
#include "deopt/side_table.h"

VTX_TEST(osr5_lookup_returns_entry_matching_loop_header_pc)
{
    vtx_side_table_t *table = vtx_side_table_build(NULL);
    VTX_ASSERT_NOT_NULL(table);

    /* Add two OSR entries at different bytecode_pcs.
     * Native offsets must be monotonically increasing (side-table
     * ordering invariant). */
    uint32_t idx1 = vtx_side_table_add_entry(table,
        /*native_pc_offset=*/100, /*frame_state_index=*/UINT32_MAX,
        VTX_STF_OSR_ENTRY, /*bytecode_pc=*/6);
    VTX_ASSERT_NOT_EQUAL(idx1, UINT32_MAX);

    uint32_t idx2 = vtx_side_table_add_entry(table,
        /*native_pc_offset=*/200, /*frame_state_index=*/UINT32_MAX,
        VTX_STF_OSR_ENTRY, /*bytecode_pc=*/24);
    VTX_ASSERT_NOT_EQUAL(idx2, UINT32_MAX);

    /* Lookup for loop_header_pc=24 must return the SECOND entry, not
     * the first. The old code (linear scan returning the first match)
     * would have returned the PC=6 entry regardless. */
    const vtx_side_table_entry_t *e =
        vtx_side_table_lookup_osr_entry(table, /*bytecode_pc=*/24);
    VTX_ASSERT_NOT_NULL(e);
    VTX_ASSERT_EQUAL(e->bytecode_pc, 24u);
    VTX_ASSERT_EQUAL(e->native_pc_offset, 200u);

    /* Lookup for loop_header_pc=6 returns the first entry. */
    e = vtx_side_table_lookup_osr_entry(table, /*bytecode_pc=*/6);
    VTX_ASSERT_NOT_NULL(e);
    VTX_ASSERT_EQUAL(e->bytecode_pc, 6u);
    VTX_ASSERT_EQUAL(e->native_pc_offset, 100u);

    vtx_side_table_destroy(table);
}

VTX_TEST(osr5_lookup_returns_null_for_nonexistent_loop_header)
{
    vtx_side_table_t *table = vtx_side_table_build(NULL);
    VTX_ASSERT_NOT_NULL(table);

    vtx_side_table_add_entry(table, 100, UINT32_MAX,
                              VTX_STF_OSR_ENTRY, /*bytecode_pc=*/6);
    vtx_side_table_add_entry(table, 200, UINT32_MAX,
                              VTX_STF_OSR_ENTRY, /*bytecode_pc=*/24);

    /* Lookup for a loop header that doesn't exist must return NULL,
     * not the "largest ≤ target" entry from the generic lookup. */
    const vtx_side_table_entry_t *e =
        vtx_side_table_lookup_osr_entry(table, /*bytecode_pc=*/99);
    VTX_ASSERT_NULL(e);

    vtx_side_table_destroy(table);
}

VTX_TEST(osr5_add_entry_stores_bytecode_pc_field)
{
    /* Verify the bytecode_pc field is stored on the entry, not just
     * ignored. This is the precondition for OSR-5's lookup-by-PC fix. */
    vtx_side_table_t *table = vtx_side_table_build(NULL);
    VTX_ASSERT_NOT_NULL(table);

    uint32_t idx = vtx_side_table_add_entry(table,
        /*native_pc_offset=*/50, /*frame_state_index=*/7,
        VTX_STF_OSR_ENTRY, /*bytecode_pc=*/42);
    VTX_ASSERT_NOT_EQUAL(idx, UINT32_MAX);

    const vtx_side_table_entry_t *e = vtx_side_table_get_entry(table, idx);
    VTX_ASSERT_NOT_NULL(e);
    VTX_ASSERT_EQUAL(e->bytecode_pc, 42u);

    vtx_side_table_destroy(table);
}

int main(void)
{
    vtx_test_result_t result = vtx_test_run_all();
    printf("\nOSR-5 regression: %u passed, %u failed, %u total\n",
           result.pass_count, result.fail_count, result.total_count);
    return (result.fail_count > 0) ? 1 : 0;
}
