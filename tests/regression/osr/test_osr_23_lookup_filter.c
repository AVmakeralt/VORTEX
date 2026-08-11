/*
 * VORTEX OSR-23 Regression Test
 *
 * Bug: vtx_side_table_lookup(table, native_pc_offset) used "largest
 *      native_pc_offset <= target" semantics — correct for deopt
 *      (state at or before the PC), but WRONG for OSR entry lookup.
 *      It could return a non-OSR-ENTRY entry (e.g., a safepoint or
 *      guard) that happened to be near the requested PC.
 *
 *      Impact: vtx_osr_up might enter the JIT at the wrong native PC
 *              (a safepoint offset instead of the loop-header offset).
 *
 * Fix: A dedicated lookup function vtx_side_table_lookup_osr_entry
 *      (in src/deopt/side_table.c) filters by:
 *        (1) the VTX_STF_OSR_ENTRY flag, AND
 *        (2) the bytecode_pc field (OSR-5 fix — matches the requested
 *            loop header when a method has multiple OSR entry points).
 *      Returns NULL if no matching entry exists.
 *
 * Test: Construct a side table with a mix of OSR-flagged and
 *       non-OSR-flagged entries. Verify:
 *
 *   1. vtx_side_table_lookup returns the wrong (non-OSR) entry when
 *      asked for a PC near an OSR entry — this is the bug.
 *
 *   2. vtx_side_table_lookup_osr_entry returns NULL for a
 *      bytecode_pc that matches NO OSR-flagged entry (even if a
 *      non-OSR entry is near the requested PC).
 *
 *   3. vtx_side_table_lookup_osr_entry returns the OSR-flagged entry
 *      when one exists at the requested bytecode_pc.
 *
 *   4. vtx_side_table_lookup_osr_entry returns NULL when an OSR entry
 *      exists at the right bytecode_pc but without the VTX_STF_OSR_ENTRY
 *      flag (i.e., it filters by flag, not by PC alone).
 */

#include "osr_test_setup.h"
#include "deopt/side_table.h"

VTX_TEST(osr23_generic_lookup_returns_non_osr_entry_near_target_pc)
{
    /* This is the BUG: the generic vtx_side_table_lookup uses
     * "largest ≤ target" semantics and returns whatever entry is
     * closest, regardless of its flag. We document the bug here so
     * the fix is provably necessary. */
    vtx_side_table_t *table = vtx_side_table_build(NULL);
    VTX_ASSERT_NOT_NULL(table);

    /* Add a SAFEPPOINT entry at native_pc_offset=100 with bytecode_pc
     * = UINT32_MAX (sentinel: "not an OSR entry"). */
    uint32_t idx_safe = vtx_side_table_add_entry(table,
        /*native_pc_offset=*/100, /*frame_state_index=*/UINT32_MAX,
        VTX_STF_SAFEPPOINT, /*bytecode_pc=*/UINT32_MAX);
    VTX_ASSERT_NOT_EQUAL(idx_safe, UINT32_MAX);

    /* Add an OSR_ENTRY at native_pc_offset=120 with bytecode_pc=6
     * (the loop header). */
    uint32_t idx_osr = vtx_side_table_add_entry(table,
        /*native_pc_offset=*/120, /*frame_state_index=*/UINT32_MAX,
        VTX_STF_OSR_ENTRY, /*bytecode_pc=*/6);
    VTX_ASSERT_NOT_EQUAL(idx_osr, UINT32_MAX);

    /* Generic lookup for native_pc_offset=110 returns the SAFEPPOINT
     * entry (offset 100), NOT the OSR entry (offset 120). This is
     * CORRECT for deopt (state at or before the PC) but WRONG for
     * OSR entry lookup — the caller wanted an OSR entry, not a
     * safepoint. The OSR-23 fix is the dedicated lookup_osr_entry
     * that filters by flag. */
    const vtx_side_table_entry_t *e =
        vtx_side_table_lookup_entry(table, /*native_pc_offset=*/110);
    VTX_ASSERT_NOT_NULL(e);
    VTX_ASSERT_EQUAL(e->native_pc_offset, 100u);
    VTX_ASSERT_TRUE((e->flags & VTX_STF_SAFEPPOINT) != 0);
    VTX_ASSERT_TRUE((e->flags & VTX_STF_OSR_ENTRY) == 0);

    vtx_side_table_destroy(table);
}

VTX_TEST(osr23_lookup_osr_entry_returns_null_when_only_non_osr_entries_exist)
{
    /* OSR-23: when no VTX_STF_OSR_ENTRY-flagged entry matches the
     * requested bytecode_pc, lookup_osr_entry must return NULL —
     * NOT fall back to "largest ≤ target" like the generic lookup. */
    vtx_side_table_t *table = vtx_side_table_build(NULL);
    VTX_ASSERT_NOT_NULL(table);

    /* Populate the table with non-OSR entries ONLY. */
    vtx_side_table_add_entry(table, 100, UINT32_MAX,
                              VTX_STF_SAFEPPOINT, UINT32_MAX);
    vtx_side_table_add_entry(table, 120, UINT32_MAX,
                              VTX_STF_GUARD, UINT32_MAX);
    vtx_side_table_add_entry(table, 140, UINT32_MAX,
                              VTX_STF_CALL_SITE, UINT32_MAX);

    /* Request the OSR entry for bytecode_pc=6. There are NO OSR
     * entries in the table. The pre-fix bug would have returned the
     * largest ≤ entry (the call_site at 140 if we'd passed 150);
     * here we pass a bytecode_pc so lookup_osr_entry (which scans
     * by bytecode_pc, not native_pc) returns NULL cleanly. */
    const vtx_side_table_entry_t *e =
        vtx_side_table_lookup_osr_entry(table, /*bytecode_pc=*/6);
    VTX_ASSERT_NULL(e);

    vtx_side_table_destroy(table);
}

VTX_TEST(osr23_lookup_osr_entry_returns_matching_osr_entry)
{
    /* Positive test: when an OSR-flagged entry at the requested
     * bytecode_pc exists, lookup_osr_entry returns it. */
    vtx_side_table_t *table = vtx_side_table_build(NULL);
    VTX_ASSERT_NOT_NULL(table);

    /* Mix of entries — including one OSR entry at bytecode_pc=6. */
    vtx_side_table_add_entry(table,  80, UINT32_MAX,
                              VTX_STF_GUARD, UINT32_MAX);
    vtx_side_table_add_entry(table, 100, UINT32_MAX,
                              VTX_STF_SAFEPPOINT, UINT32_MAX);
    vtx_side_table_add_entry(table, 120, UINT32_MAX,
                              VTX_STF_OSR_ENTRY, /*bytecode_pc=*/6);
    vtx_side_table_add_entry(table, 140, UINT32_MAX,
                              VTX_STF_CALL_SITE, UINT32_MAX);

    const vtx_side_table_entry_t *e =
        vtx_side_table_lookup_osr_entry(table, /*bytecode_pc=*/6);
    VTX_ASSERT_NOT_NULL(e);
    VTX_ASSERT_TRUE((e->flags & VTX_STF_OSR_ENTRY) != 0);
    VTX_ASSERT_EQUAL(e->bytecode_pc, 6u);
    VTX_ASSERT_EQUAL(e->native_pc_offset, 120u);

    vtx_side_table_destroy(table);
}

VTX_TEST(osr23_lookup_osr_entry_filters_by_flag_not_just_bytecode_pc)
{
    /* OSR-23 core invariant: the lookup filters by the
     * VTX_STF_OSR_ENTRY flag, NOT just by bytecode_pc. If an entry
     * has bytecode_pc=6 but lacks the OSR flag, it must NOT be
     * returned by lookup_osr_entry. (E.g., a safepoint recorded at
     * the same PC as the loop header.) */
    vtx_side_table_t *table = vtx_side_table_build(NULL);
    VTX_ASSERT_NOT_NULL(table);

    /* Add a GUARD entry at bytecode_pc=6 (NOT flagged as OSR). */
    vtx_side_table_add_entry(table, 100, UINT32_MAX,
                              VTX_STF_GUARD, /*bytecode_pc=*/6);
    /* Add an OSR entry at the same bytecode_pc=6. */
    vtx_side_table_add_entry(table, 120, UINT32_MAX,
                              VTX_STF_OSR_ENTRY, /*bytecode_pc=*/6);

    const vtx_side_table_entry_t *e =
        vtx_side_table_lookup_osr_entry(table, /*bytecode_pc=*/6);
    VTX_ASSERT_NOT_NULL(e);
    /* Must return the OSR-flagged entry (native_offset 120), not the
     * GUARD-flagged entry at the same bytecode_pc. */
    VTX_ASSERT_TRUE((e->flags & VTX_STF_OSR_ENTRY) != 0);
    VTX_ASSERT_EQUAL(e->native_pc_offset, 120u);
    /* Negative assertion: the returned entry is NOT the guard. */
    VTX_ASSERT_TRUE((e->flags & VTX_STF_GUARD) == 0);

    vtx_side_table_destroy(table);
}

VTX_TEST(osr23_lookup_osr_entry_returns_null_when_no_match_at_bytecode_pc)
{
    /* OSR-23 + OSR-5: when OSR entries exist at OTHER bytecode_pcs,
     * but not at the requested one, lookup returns NULL. The old
     * "first OSR-flagged entry" bug (OSR-5) would have returned
     * the wrong entry; OSR-23's lookup_osr_entry combined with
     * OSR-5's bytecode_pc filter correctly returns NULL. */
    vtx_side_table_t *table = vtx_side_table_build(NULL);
    VTX_ASSERT_NOT_NULL(table);

    vtx_side_table_add_entry(table, 100, UINT32_MAX,
                              VTX_STF_OSR_ENTRY, /*bytecode_pc=*/6);
    vtx_side_table_add_entry(table, 200, UINT32_MAX,
                              VTX_STF_OSR_ENTRY, /*bytecode_pc=*/24);

    /* No OSR entry at bytecode_pc=99. */
    const vtx_side_table_entry_t *e =
        vtx_side_table_lookup_osr_entry(table, /*bytecode_pc=*/99);
    VTX_ASSERT_NULL(e);

    vtx_side_table_destroy(table);
}

int main(void)
{
    vtx_test_result_t result = vtx_test_run_all();
    printf("\nOSR-23 regression: %u passed, %u failed, %u total\n",
           result.pass_count, result.fail_count, result.total_count);
    return (result.fail_count > 0) ? 1 : 0;
}
