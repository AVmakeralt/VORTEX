/*
 * VORTEX OSR-2 Regression Test
 *
 * Bug: VTX_STF_OSR_ENTRY flag is defined in side_table.h:79 and read by
 * vtx_osr_up at osr.c, but no codegen path (T1/T2/T3/AOT) ever wrote it.
 * This meant the side-table OSR entry lookup in vtx_osr_up never found
 * an entry — OSR up only succeeded via the bc_pc_map fallback (T1 only).
 *
 * Fix: In T1 codegen (src/baseline/codegen.c), scan_loop_headers pre-scans
 * the bytecode for backward-branch targets (loop headers), and
 * record_bc_pc_map emits a VTX_STF_OSR_ENTRY side-table entry at each
 * loop header's native offset.
 *
 * This test compiles a method with a GOTO backedge loop and verifies:
 *   1. The side_table is non-NULL after compilation.
 *   2. At least one entry has the VTX_STF_OSR_ENTRY flag set.
 *   3. That entry's bytecode_pc matches the loop header PC.
 */

#include "osr_test_setup.h"
#include "baseline/codegen.h"
#include "codecache/install.h"

/*
 * loop_method:
 *   PC 0: LOAD_CONST_INT 0   ; push 0 (loop counter init)
 *   PC 3: STORE_LOCAL 0       ; locals[0] = counter
 *   PC 6: LOAD_LOCAL 0        ; push counter   ← loop header (PC=6)
 *   PC 9: LOAD_CONST_INT 1    ; push 1
 *   PC 12: IADD               ; counter + 1
 *   PC 13: STORE_LOCAL 0      ; locals[0] = counter+1
 *   PC 16: GOTO 6             ; backedge to PC 6
 *   PC 19: LOAD_LOCAL 0       ; push counter
 *   PC 22: RETURN_VALUE
 *
 * The GOTO at PC 16 targets PC 6 (backward branch), so PC 6 is a loop
 * header. The codegen should record a VTX_STF_OSR_ENTRY side-table
 * entry with bytecode_pc=6.
 */
static void build_loop_method(vtx_bytecode_t *bc, uint8_t *code_buf,
                               vtx_value_t *const_pool)
{
    uint32_t pc = 0;
    code_buf[pc] = VT_OP_LOAD_CONST_INT; code_buf[pc+1] = 0; code_buf[pc+2] = 0; pc += 3;  /* const 0 */
    code_buf[pc] = VT_OP_STORE_LOCAL;    code_buf[pc+1] = 0; code_buf[pc+2] = 0; pc += 3;  /* local 0 */
    /* loop header at PC=6 */
    code_buf[pc] = VT_OP_LOAD_LOCAL;     code_buf[pc+1] = 0; code_buf[pc+2] = 0; pc += 3;  /* local 0 */
    code_buf[pc] = VT_OP_LOAD_CONST_INT; code_buf[pc+1] = 0; code_buf[pc+2] = 1; pc += 3;  /* const 1 */
    code_buf[pc] = VT_OP_IADD; pc += 1;
    code_buf[pc] = VT_OP_STORE_LOCAL;    code_buf[pc+1] = 0; code_buf[pc+2] = 0; pc += 3;
    code_buf[pc] = VT_OP_GOTO;           code_buf[pc+1] = 0; code_buf[pc+2] = 6; pc += 3;  /* goto PC 6 (backedge) */
    code_buf[pc] = VT_OP_LOAD_LOCAL;     code_buf[pc+1] = 0; code_buf[pc+2] = 0; pc += 3;
    code_buf[pc] = VT_OP_RETURN_VALUE; pc += 1;

    bc->code = code_buf; bc->length = pc;
    bc->constant_pool = const_pool; bc->constant_count = 2;
    bc->max_locals = 1; bc->max_stack = 4;
}

VTX_TEST(osr2_osr_entry_flag_written_by_codegen)
{
    vtx_arena_t arena; vtx_arena_init(&arena);
    vtx_type_system_t ts; vtx_type_system_init(&ts);
    vtx_gc_t gc; vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);

    uint8_t *code_buf = vtx_arena_alloc(&arena, 32);
    vtx_value_t const_pool[2] = { vtx_make_smi(0), vtx_make_smi(1) };
    vtx_bytecode_t bc;
    build_loop_method(&bc, code_buf, const_pool);

    vtx_method_desc_t method = {
        .name = "loop", .signature = "()I",
        .bytecode = &bc, .compiled_code = NULL,
        .vtable_index = 200, .arg_count = 0, .is_virtual = false,
    };

    vtx_code_cache_t cache; vtx_code_cache_init(&cache, 1 << 16);
    vtx_method_registry_t registry; vtx_method_registry_init(&registry, &arena);
    vtx_compiled_code_t *compiled = vtx_baseline_compile(
        &method, NULL, &arena, &cache, &registry);
    VTX_ASSERT_NOT_NULL(compiled);

    /* The codegen installs the code via the cache, transferring side_table
     * ownership to the compiled_method. Look it up from the registry. */
    vtx_compiled_method_t *cm = vtx_method_registry_get(&registry, method.vtable_index);
    VTX_ASSERT_NOT_NULL(cm);
    VTX_ASSERT_NOT_NULL(cm->side_table);

    /* Scan the side_table for an entry with VTX_STF_OSR_ENTRY flag
     * at bytecode_pc=6 (the loop header). */
    bool found_osr_entry = false;
    uint32_t entry_count = vtx_side_table_entry_count(cm->side_table);
    for (uint32_t i = 0; i < entry_count; i++) {
        const vtx_side_table_entry_t *e =
            vtx_side_table_get_entry(cm->side_table, i);
        if (e && (e->flags & VTX_STF_OSR_ENTRY)) {
            /* OSR-5 fix: the bytecode_pc field must be set to the loop
             * header PC (6), not the default UINT32_MAX. */
            if (e->bytecode_pc == 6) {
                found_osr_entry = true;
                break;
            }
        }
    }

    VTX_ASSERT_TRUE(found_osr_entry);

    vtx_compiled_code_destroy(compiled);
    vtx_method_registry_destroy(&registry);
    vtx_code_cache_destroy(&cache);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
    vtx_arena_destroy(&arena);
}

VTX_TEST(osr2_osr_entry_lookup_finds_loop_header)
{
    /* Companion test: verify that vtx_side_table_lookup_osr_entry
     * (the dedicated OSR-23 lookup) finds the entry written by OSR-2. */
    vtx_arena_t arena; vtx_arena_init(&arena);
    vtx_type_system_t ts; vtx_type_system_init(&ts);
    vtx_gc_t gc; vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);

    uint8_t *code_buf = vtx_arena_alloc(&arena, 32);
    vtx_value_t const_pool[2] = { vtx_make_smi(0), vtx_make_smi(1) };
    vtx_bytecode_t bc;
    build_loop_method(&bc, code_buf, const_pool);

    vtx_method_desc_t method = {
        .name = "loop2", .signature = "()I",
        .bytecode = &bc, .compiled_code = NULL,
        .vtable_index = 201, .arg_count = 0, .is_virtual = false,
    };

    vtx_code_cache_t cache; vtx_code_cache_init(&cache, 1 << 16);
    vtx_method_registry_t registry; vtx_method_registry_init(&registry, &arena);
    vtx_compiled_code_t *compiled = vtx_baseline_compile(
        &method, NULL, &arena, &cache, &registry);
    VTX_ASSERT_NOT_NULL(compiled);

    vtx_compiled_method_t *cm = vtx_method_registry_get(&registry, method.vtable_index);
    VTX_ASSERT_NOT_NULL(cm);
    VTX_ASSERT_NOT_NULL(cm->side_table);

    /* The OSR-2 fix means lookup_osr_entry now finds the entry. */
    const vtx_side_table_entry_t *osr_e =
        vtx_side_table_lookup_osr_entry(cm->side_table, /*loop_header_pc=*/6);
    VTX_ASSERT_NOT_NULL(osr_e);
    VTX_ASSERT_TRUE((osr_e->flags & VTX_STF_OSR_ENTRY) != 0);
    VTX_ASSERT_EQUAL(osr_e->bytecode_pc, 6u);

    vtx_compiled_code_destroy(compiled);
    vtx_method_registry_destroy(&registry);
    vtx_code_cache_destroy(&cache);
    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
    vtx_arena_destroy(&arena);
}

int main(void)
{
    vtx_test_result_t result = vtx_test_run_all();
    printf("\nOSR-2 regression: %u passed, %u failed, %u total\n",
           result.pass_count, result.fail_count, result.total_count);
    return (result.fail_count > 0) ? 1 : 0;
}
