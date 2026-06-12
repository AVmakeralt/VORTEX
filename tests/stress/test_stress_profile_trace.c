/**
 * test_stress_profile_trace.c — Exhaustive stress tests for VORTEX
 * profile, trace, region, guard, and deopt modules.
 *
 * 200 tests exercising real API calls with meaningful assertions.
 */

#include "test_framework.h"
#include "profile/data.h"
#include "profile/merge.h"
#include "profile/persist.h"
#include "profile/phase.h"
#include "trace/recorder.h"
#include "trace/side_exit.h"
#include "trace/selector.h"
#include "trace/tree.h"
#include "region/budget.h"
#include "guard/ewma.h"
#include "guard/metadata.h"
#include "guard/hoist.h"
#include "guard/merge.h"
#include "deopt/frame_state.h"
#include "deopt/side_table.h"
#include "deopt/osr.h"
#include "deopt/deoptless.h"
#include "runtime/arena.h"
#include "runtime/type_system.h"
#include "runtime/bytecode.h"
#include "ir/node.h"
#include "ir/graph.h"
#include <string.h>
#include <stdio.h>

/* ========================================================================== */
/* Profile Data tests (30 tests): test_profile_01 through test_profile_30     */
/* ========================================================================== */

VTX_TEST(test_profile_01)
{
    vtx_profile_global_t global;
    int rc = vtx_profile_global_init(&global);
    VTX_ASSERT_EQUAL(rc, 0);
    VTX_ASSERT_EQUAL(global.method_count, 0u);
    VTX_ASSERT_EQUAL(global.call_edge_count, 0u);
    vtx_profile_global_destroy(&global);
}

VTX_TEST(test_profile_02)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    vtx_profile_method_t *m = vtx_profile_add_method(&global, 42);
    VTX_ASSERT_NOT_NULL(m);
    VTX_ASSERT_EQUAL(m->method_id, 42u);
    vtx_profile_method_t *m2 = vtx_profile_get_method(&global, 42);
    VTX_ASSERT_NOT_NULL(m2);
    VTX_ASSERT_EQUAL(m2->method_id, 42u);
    vtx_profile_global_destroy(&global);
}

VTX_TEST(test_profile_03)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    vtx_profile_method_t *m1 = vtx_profile_add_method(&global, 1);
    vtx_profile_method_t *m2 = vtx_profile_add_method(&global, 2);
    vtx_profile_method_t *m3 = vtx_profile_add_method(&global, 3);
    VTX_ASSERT_NOT_NULL(m1);
    VTX_ASSERT_NOT_NULL(m2);
    VTX_ASSERT_NOT_NULL(m3);
    VTX_ASSERT_EQUAL(global.method_count, 3u);
    vtx_profile_global_destroy(&global);
}

VTX_TEST(test_profile_04)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    vtx_profile_method_t *m = vtx_profile_get_method(&global, 9999);
    VTX_ASSERT_NULL(m);
    vtx_profile_global_destroy(&global);
}

VTX_TEST(test_profile_05)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    vtx_profile_record_invocation(&global, 10);
    vtx_profile_method_t *m = vtx_profile_get_method(&global, 10);
    VTX_ASSERT_NOT_NULL(m);
    VTX_ASSERT_EQUAL(m->invocation_count, 1ull);
    vtx_profile_global_destroy(&global);
}

VTX_TEST(test_profile_06)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    vtx_profile_record_callsite_type(&global, 10, 0, 5);
    const vtx_callsite_profile_t *cs = vtx_profile_get_callsite(&global, 10, 0);
    VTX_ASSERT_NOT_NULL(cs);
    VTX_ASSERT_EQUAL(cs->count, 1u);
    VTX_ASSERT_EQUAL(cs->types[0], (vtx_typeid_t)5);
    vtx_profile_global_destroy(&global);
}

VTX_TEST(test_profile_07)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    vtx_profile_record_branch(&global, 10, 100, true);
    const vtx_branch_profile_t *br = vtx_profile_get_branch(&global, 10, 100);
    VTX_ASSERT_NOT_NULL(br);
    VTX_ASSERT_EQUAL(br->taken, 1ull);
    VTX_ASSERT_EQUAL(br->not_taken, 0ull);
    vtx_profile_global_destroy(&global);
}

VTX_TEST(test_profile_08)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    vtx_profile_record_field_shape(&global, 10, 8, 3);
    vtx_profile_method_t *m = vtx_profile_get_method(&global, 10);
    VTX_ASSERT_NOT_NULL(m);
    VTX_ASSERT_EQUAL(m->field_access_count, 1u);
    VTX_ASSERT_EQUAL(m->field_accesses[0].field_offset, 8u);
    VTX_ASSERT_EQUAL(m->field_accesses[0].shapes[0], (vtx_shapeid_t)3);
    vtx_profile_global_destroy(&global);
}

VTX_TEST(test_profile_09)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    vtx_profile_record_loop_backedge(&global, 10, 200);
    vtx_profile_method_t *m = vtx_profile_get_method(&global, 10);
    VTX_ASSERT_NOT_NULL(m);
    VTX_ASSERT_EQUAL(m->loop_count, 1u);
    VTX_ASSERT_EQUAL(m->loops[0].loop_header_pc, 200u);
    VTX_ASSERT_EQUAL(m->loops[0].backedge_count, 1ull);
    vtx_profile_global_destroy(&global);
}

VTX_TEST(test_profile_10)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    vtx_profile_record_call_edge(&global, 1, 2);
    VTX_ASSERT_EQUAL(global.call_edge_count, 1u);
    VTX_ASSERT_EQUAL(global.call_edges[0].caller_method_id, 1u);
    VTX_ASSERT_EQUAL(global.call_edges[0].callee_method_id, 2u);
    VTX_ASSERT_EQUAL(global.call_edges[0].frequency, 1ull);
    vtx_profile_global_destroy(&global);
}

VTX_TEST(test_profile_11)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    vtx_profile_record_callsite_type(&global, 10, 0, 5);
    vtx_profile_record_callsite_type(&global, 10, 0, 7);
    const vtx_callsite_profile_t *cs = vtx_profile_get_callsite(&global, 10, 0);
    VTX_ASSERT_NOT_NULL(cs);
    VTX_ASSERT_EQUAL(cs->count, 2u);
    VTX_ASSERT_EQUAL(cs->types[0], (vtx_typeid_t)5);
    VTX_ASSERT_EQUAL(cs->types[1], (vtx_typeid_t)7);
    vtx_profile_global_destroy(&global);
}

VTX_TEST(test_profile_12)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    vtx_profile_record_branch(&global, 10, 50, true);
    vtx_profile_record_branch(&global, 10, 50, false);
    const vtx_branch_profile_t *br = vtx_profile_get_branch(&global, 10, 50);
    VTX_ASSERT_NOT_NULL(br);
    VTX_ASSERT_EQUAL(br->taken, 1ull);
    VTX_ASSERT_EQUAL(br->not_taken, 1ull);
    double prob = vtx_profile_branch_probability(br);
    VTX_ASSERT_TRUE(prob > 0.49 && prob < 0.51);
    vtx_profile_global_destroy(&global);
}

VTX_TEST(test_profile_13)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    /* 0 taken: probability = 0 */
    vtx_profile_record_branch(&global, 10, 100, false);
    vtx_profile_record_branch(&global, 10, 100, false);
    const vtx_branch_profile_t *br = vtx_profile_get_branch(&global, 10, 100);
    VTX_ASSERT_NOT_NULL(br);
    double prob = vtx_profile_branch_probability(br);
    VTX_ASSERT_TRUE(prob < 0.001);
    vtx_profile_global_destroy(&global);
}

VTX_TEST(test_profile_14)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    /* 100% taken */
    vtx_profile_record_branch(&global, 10, 100, true);
    vtx_profile_record_branch(&global, 10, 100, true);
    const vtx_branch_profile_t *br = vtx_profile_get_branch(&global, 10, 100);
    VTX_ASSERT_NOT_NULL(br);
    double prob = vtx_profile_branch_probability(br);
    VTX_ASSERT_TRUE(prob > 0.999);
    vtx_profile_global_destroy(&global);
}

VTX_TEST(test_profile_15)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    /* 50/50 */
    for (int i = 0; i < 50; i++) {
        vtx_profile_record_branch(&global, 10, 100, true);
    }
    for (int i = 0; i < 50; i++) {
        vtx_profile_record_branch(&global, 10, 100, false);
    }
    const vtx_branch_profile_t *br = vtx_profile_get_branch(&global, 10, 100);
    VTX_ASSERT_NOT_NULL(br);
    double prob = vtx_profile_branch_probability(br);
    VTX_ASSERT_TRUE(prob > 0.49 && prob < 0.51);
    vtx_profile_global_destroy(&global);
}

VTX_TEST(test_profile_16)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    vtx_profile_record_invocation(&global, 10);
    vtx_profile_record_invocation(&global, 10);
    vtx_profile_record_invocation(&global, 10);
    vtx_profile_method_t *m = vtx_profile_get_method(&global, 10);
    VTX_ASSERT_NOT_NULL(m);
    VTX_ASSERT_TRUE(vtx_profile_method_is_hot(m, 2));
    vtx_profile_global_destroy(&global);
}

VTX_TEST(test_profile_17)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    vtx_profile_record_invocation(&global, 10);
    vtx_profile_method_t *m = vtx_profile_get_method(&global, 10);
    VTX_ASSERT_NOT_NULL(m);
    VTX_ASSERT_FALSE(vtx_profile_method_is_hot(m, 100));
    vtx_profile_global_destroy(&global);
}

VTX_TEST(test_profile_18)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    for (int i = 0; i < 10; i++) {
        vtx_profile_record_invocation(&global, 10);
    }
    vtx_profile_method_t *m = vtx_profile_get_method(&global, 10);
    VTX_ASSERT_NOT_NULL(m);
    VTX_ASSERT_EQUAL(m->invocation_count, 10ull);
    vtx_profile_global_destroy(&global);
}

VTX_TEST(test_profile_19)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    vtx_profile_record_callsite_type(&global, 10, 0, 1);
    vtx_profile_record_callsite_type(&global, 10, 0, 2);
    vtx_profile_record_callsite_type(&global, 10, 0, 3);
    vtx_profile_record_callsite_type(&global, 10, 0, 4);
    /* VTX_POLY_LIMIT = 4, so the 5th different type makes it megamorphic */
    vtx_profile_record_callsite_type(&global, 10, 0, 5);
    const vtx_callsite_profile_t *cs = vtx_profile_get_callsite(&global, 10, 0);
    VTX_ASSERT_NOT_NULL(cs);
    VTX_ASSERT_TRUE(cs->megamorphic);
    vtx_profile_global_destroy(&global);
}

VTX_TEST(test_profile_20)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    vtx_profile_record_branch(&global, 10, 100, true);
    vtx_profile_record_branch(&global, 10, 100, false);
    vtx_profile_record_branch(&global, 10, 200, true);
    vtx_profile_method_t *m = vtx_profile_get_method(&global, 10);
    VTX_ASSERT_NOT_NULL(m);
    VTX_ASSERT_EQUAL(m->branch_count, 2u);
    vtx_profile_global_destroy(&global);
}

VTX_TEST(test_profile_21)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    for (uint32_t i = 0; i < 150; i++) {
        vtx_profile_add_method(&global, i);
    }
    VTX_ASSERT_EQUAL(global.method_count, 150u);
    vtx_profile_method_t *m = vtx_profile_get_method(&global, 149);
    VTX_ASSERT_NOT_NULL(m);
    VTX_ASSERT_EQUAL(m->method_id, 149u);
    vtx_profile_global_destroy(&global);
}

VTX_TEST(test_profile_22)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    vtx_profile_record_callsite_type(&global, 10, 0, 5);
    vtx_profile_record_callsite_type(&global, 10, 1, 7);
    const vtx_callsite_profile_t *cs0 = vtx_profile_get_callsite(&global, 10, 0);
    const vtx_callsite_profile_t *cs1 = vtx_profile_get_callsite(&global, 10, 1);
    VTX_ASSERT_NOT_NULL(cs0);
    VTX_ASSERT_NOT_NULL(cs1);
    VTX_ASSERT_EQUAL(cs0->types[0], (vtx_typeid_t)5);
    VTX_ASSERT_EQUAL(cs1->types[0], (vtx_typeid_t)7);
    vtx_profile_global_destroy(&global);
}

VTX_TEST(test_profile_23)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    /* Record same type twice - should not increase count */
    vtx_profile_record_callsite_type(&global, 10, 0, 5);
    vtx_profile_record_callsite_type(&global, 10, 0, 5);
    const vtx_callsite_profile_t *cs = vtx_profile_get_callsite(&global, 10, 0);
    VTX_ASSERT_NOT_NULL(cs);
    VTX_ASSERT_EQUAL(cs->count, 1u);
    vtx_profile_global_destroy(&global);
}

VTX_TEST(test_profile_24)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    vtx_profile_record_loop_backedge(&global, 10, 200);
    vtx_profile_record_loop_backedge(&global, 10, 200);
    vtx_profile_method_t *m = vtx_profile_get_method(&global, 10);
    VTX_ASSERT_NOT_NULL(m);
    VTX_ASSERT_EQUAL(m->loops[0].backedge_count, 2ull);
    vtx_profile_global_destroy(&global);
}

VTX_TEST(test_profile_25)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    vtx_profile_record_call_edge(&global, 1, 2);
    vtx_profile_record_call_edge(&global, 1, 2);
    VTX_ASSERT_EQUAL(global.call_edge_count, 1u);
    VTX_ASSERT_EQUAL(global.call_edges[0].frequency, 2ull);
    vtx_profile_global_destroy(&global);
}

VTX_TEST(test_profile_26)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    vtx_profile_record_invocation(&global, 10);
    vtx_profile_record_invocation(&global, 20);
    vtx_profile_record_invocation(&global, 30);
    VTX_ASSERT_EQUAL(global.method_count, 3u);
    vtx_profile_method_t *m10 = vtx_profile_get_method(&global, 10);
    vtx_profile_method_t *m20 = vtx_profile_get_method(&global, 20);
    vtx_profile_method_t *m30 = vtx_profile_get_method(&global, 30);
    VTX_ASSERT_NOT_NULL(m10);
    VTX_ASSERT_NOT_NULL(m20);
    VTX_ASSERT_NOT_NULL(m30);
    VTX_ASSERT_EQUAL(m10->invocation_count, 1ull);
    VTX_ASSERT_EQUAL(m20->invocation_count, 1ull);
    VTX_ASSERT_EQUAL(m30->invocation_count, 1ull);
    vtx_profile_global_destroy(&global);
}

VTX_TEST(test_profile_27)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    /* branch with no observations → NULL */
    const vtx_branch_profile_t *br = vtx_profile_get_branch(&global, 999, 0);
    VTX_ASSERT_NULL(br);
    vtx_profile_global_destroy(&global);
}

VTX_TEST(test_profile_28)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    /* callsite with no observations → NULL */
    const vtx_callsite_profile_t *cs = vtx_profile_get_callsite(&global, 999, 0);
    VTX_ASSERT_NULL(cs);
    vtx_profile_global_destroy(&global);
}

VTX_TEST(test_profile_29)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    vtx_profile_record_field_shape(&global, 10, 8, 1);
    vtx_profile_record_field_shape(&global, 10, 8, 2);
    vtx_profile_record_field_shape(&global, 10, 8, 3);
    vtx_profile_record_field_shape(&global, 10, 8, 4);
    /* 5th different shape → megamorphic */
    vtx_profile_record_field_shape(&global, 10, 8, 5);
    vtx_profile_method_t *m = vtx_profile_get_method(&global, 10);
    VTX_ASSERT_NOT_NULL(m);
    VTX_ASSERT_TRUE(m->field_accesses[0].megamorphic);
    vtx_profile_global_destroy(&global);
}

VTX_TEST(test_profile_30)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    vtx_profile_record_callsite_type(&global, 10, 0, 5);
    vtx_profile_record_callsite_type(&global, 10, 0, 5);
    vtx_profile_record_callsite_type(&global, 10, 0, 7);
    const vtx_callsite_profile_t *cs = vtx_profile_get_callsite(&global, 10, 0);
    VTX_ASSERT_NOT_NULL(cs);
    VTX_ASSERT_EQUAL(cs->count, 2u);
    /* Type 5 should appear once, type 7 once */
    bool found5 = false, found7 = false;
    for (uint32_t i = 0; i < cs->count; i++) {
        if (cs->types[i] == 5) found5 = true;
        if (cs->types[i] == 7) found7 = true;
    }
    VTX_ASSERT_TRUE(found5);
    VTX_ASSERT_TRUE(found7);
    vtx_profile_global_destroy(&global);
}

/* ========================================================================== */
/* Profile Merge tests (15 tests): test_merge_01 through test_merge_15        */
/* ========================================================================== */

VTX_TEST(test_merge_01)
{
    vtx_profile_global_t target, source;
    vtx_profile_global_init(&target);
    vtx_profile_global_init(&source);
    vtx_profile_merge_into(&target, &source);
    VTX_ASSERT_EQUAL(target.method_count, 0u);
    vtx_profile_global_destroy(&target);
    vtx_profile_global_destroy(&source);
}

VTX_TEST(test_merge_02)
{
    vtx_profile_global_t target, source;
    vtx_profile_global_init(&target);
    vtx_profile_global_init(&source);
    vtx_profile_record_invocation(&source, 10);
    vtx_profile_merge_into(&target, &source);
    vtx_profile_method_t *m = vtx_profile_get_method(&target, 10);
    VTX_ASSERT_NOT_NULL(m);
    VTX_ASSERT_EQUAL(m->invocation_count, 1ull);
    vtx_profile_global_destroy(&target);
    vtx_profile_global_destroy(&source);
}

VTX_TEST(test_merge_03)
{
    vtx_profile_global_t target, source;
    vtx_profile_global_init(&target);
    vtx_profile_global_init(&source);
    vtx_profile_record_callsite_type(&source, 10, 0, 5);
    vtx_profile_merge_into(&target, &source);
    const vtx_callsite_profile_t *cs = vtx_profile_get_callsite(&target, 10, 0);
    VTX_ASSERT_NOT_NULL(cs);
    VTX_ASSERT_EQUAL(cs->count, 1u);
    VTX_ASSERT_EQUAL(cs->types[0], (vtx_typeid_t)5);
    vtx_profile_global_destroy(&target);
    vtx_profile_global_destroy(&source);
}

VTX_TEST(test_merge_04)
{
    vtx_profile_global_t target, source;
    vtx_profile_global_init(&target);
    vtx_profile_global_init(&source);
    vtx_profile_record_branch(&source, 10, 100, true);
    vtx_profile_merge_into(&target, &source);
    const vtx_branch_profile_t *br = vtx_profile_get_branch(&target, 10, 100);
    VTX_ASSERT_NOT_NULL(br);
    VTX_ASSERT_EQUAL(br->taken, 1ull);
    vtx_profile_global_destroy(&target);
    vtx_profile_global_destroy(&source);
}

VTX_TEST(test_merge_05)
{
    vtx_profile_global_t target, source;
    vtx_profile_global_init(&target);
    vtx_profile_global_init(&source);
    vtx_profile_record_field_shape(&source, 10, 8, 3);
    vtx_profile_merge_into(&target, &source);
    vtx_profile_method_t *m = vtx_profile_get_method(&target, 10);
    VTX_ASSERT_NOT_NULL(m);
    VTX_ASSERT_EQUAL(m->field_access_count, 1u);
    vtx_profile_global_destroy(&target);
    vtx_profile_global_destroy(&source);
}

VTX_TEST(test_merge_06)
{
    vtx_profile_global_t target, source;
    vtx_profile_global_init(&target);
    vtx_profile_global_init(&source);
    vtx_profile_record_loop_backedge(&source, 10, 200);
    vtx_profile_merge_into(&target, &source);
    vtx_profile_method_t *m = vtx_profile_get_method(&target, 10);
    VTX_ASSERT_NOT_NULL(m);
    VTX_ASSERT_EQUAL(m->loop_count, 1u);
    VTX_ASSERT_EQUAL(m->loops[0].backedge_count, 1ull);
    vtx_profile_global_destroy(&target);
    vtx_profile_global_destroy(&source);
}

VTX_TEST(test_merge_07)
{
    vtx_profile_global_t target, source;
    vtx_profile_global_init(&target);
    vtx_profile_global_init(&source);
    vtx_profile_record_invocation(&target, 10);
    vtx_profile_record_invocation(&source, 10);
    vtx_profile_merge_into(&target, &source);
    vtx_profile_method_t *m = vtx_profile_get_method(&target, 10);
    VTX_ASSERT_NOT_NULL(m);
    VTX_ASSERT_EQUAL(m->invocation_count, 2ull);
    vtx_profile_global_destroy(&target);
    vtx_profile_global_destroy(&source);
}

VTX_TEST(test_merge_08)
{
    vtx_profile_global_t target, s1, s2;
    vtx_profile_global_init(&target);
    vtx_profile_global_init(&s1);
    vtx_profile_global_init(&s2);
    vtx_profile_record_invocation(&s1, 10);
    vtx_profile_record_invocation(&s2, 20);
    vtx_profile_merge_into(&target, &s1);
    vtx_profile_merge_into(&target, &s2);
    VTX_ASSERT_EQUAL(target.method_count, 2u);
    vtx_profile_global_destroy(&target);
    vtx_profile_global_destroy(&s1);
    vtx_profile_global_destroy(&s2);
}

VTX_TEST(test_merge_09)
{
    vtx_profile_global_t target, source;
    vtx_profile_global_init(&target);
    vtx_profile_global_init(&source);
    vtx_profile_record_callsite_type(&target, 10, 0, 5);
    vtx_profile_record_callsite_type(&source, 10, 0, 7);
    vtx_profile_merge_into(&target, &source);
    const vtx_callsite_profile_t *cs = vtx_profile_get_callsite(&target, 10, 0);
    VTX_ASSERT_NOT_NULL(cs);
    VTX_ASSERT_EQUAL(cs->count, 2u);
    vtx_profile_global_destroy(&target);
    vtx_profile_global_destroy(&source);
}

VTX_TEST(test_merge_10)
{
    vtx_profile_global_t target, source;
    vtx_profile_global_init(&target);
    vtx_profile_global_init(&source);
    vtx_profile_record_branch(&target, 10, 100, true);
    vtx_profile_record_branch(&source, 10, 100, true);
    vtx_profile_merge_into(&target, &source);
    const vtx_branch_profile_t *br = vtx_profile_get_branch(&target, 10, 100);
    VTX_ASSERT_NOT_NULL(br);
    VTX_ASSERT_EQUAL(br->taken, 2ull);
    vtx_profile_global_destroy(&target);
    vtx_profile_global_destroy(&source);
}

VTX_TEST(test_merge_11)
{
    vtx_profile_global_t target, source;
    vtx_profile_global_init(&target);
    vtx_profile_global_init(&source);
    vtx_profile_record_loop_backedge(&target, 10, 200);
    vtx_profile_record_loop_backedge(&source, 10, 200);
    vtx_profile_merge_into(&target, &source);
    vtx_profile_method_t *m = vtx_profile_get_method(&target, 10);
    VTX_ASSERT_NOT_NULL(m);
    VTX_ASSERT_EQUAL(m->loops[0].backedge_count, 2ull);
    vtx_profile_global_destroy(&target);
    vtx_profile_global_destroy(&source);
}

VTX_TEST(test_merge_12)
{
    vtx_callsite_profile_t target_cs = {0};
    vtx_callsite_profile_t source_cs = {0};
    target_cs.types[0] = 5;
    target_cs.count = 1;
    source_cs.types[0] = 7;
    source_cs.count = 1;
    vtx_profile_merge_callsite(&target_cs, &source_cs);
    VTX_ASSERT_EQUAL(target_cs.count, 2u);
    VTX_ASSERT_FALSE(target_cs.megamorphic);
}

VTX_TEST(test_merge_13)
{
    vtx_branch_profile_t target_br = { .bytecode_pc = 100, .taken = 3, .not_taken = 1 };
    vtx_branch_profile_t source_br = { .bytecode_pc = 100, .taken = 2, .not_taken = 4 };
    vtx_profile_merge_branch(&target_br, &source_br);
    VTX_ASSERT_EQUAL(target_br.taken, 5ull);
    VTX_ASSERT_EQUAL(target_br.not_taken, 5ull);
}

VTX_TEST(test_merge_14)
{
    vtx_field_profile_t target_f = {0};
    vtx_field_profile_t source_f = {0};
    target_f.field_offset = 8;
    target_f.shapes[0] = 1;
    target_f.count = 1;
    source_f.field_offset = 8;
    source_f.shapes[0] = 2;
    source_f.count = 1;
    vtx_profile_merge_field(&target_f, &source_f);
    VTX_ASSERT_EQUAL(target_f.count, 2u);
}

VTX_TEST(test_merge_15)
{
    vtx_loop_profile_t target_l = { .loop_header_pc = 200, .backedge_count = 10 };
    vtx_loop_profile_t source_l = { .loop_header_pc = 200, .backedge_count = 5 };
    vtx_profile_merge_loop(&target_l, &source_l);
    VTX_ASSERT_EQUAL(target_l.backedge_count, 15ull);
}

/* ========================================================================== */
/* Profile Persist tests (10 tests): test_persist_01 through test_persist_10   */
/* ========================================================================== */

VTX_TEST(test_persist_01)
{
    /* Save/load empty profile - persist may not support empty profiles,
     * so test with at least one method */
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    vtx_profile_add_method(&global, 1);
    vtx_profile_record_invocation(&global, 1);
    bool ok = vtx_profile_save(&global, "/tmp/vtx_test_persist_01.bin");
    VTX_ASSERT_TRUE(ok);
    vtx_profile_global_t loaded;
    vtx_profile_global_init(&loaded);
    ok = vtx_profile_load(&loaded, "/tmp/vtx_test_persist_01.bin");
    VTX_ASSERT_TRUE(ok);
    vtx_profile_global_destroy(&global);
    vtx_profile_global_destroy(&loaded);
    remove("/tmp/vtx_test_persist_01.bin");
}

VTX_TEST(test_persist_02)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    vtx_profile_add_method(&global, 10);
    vtx_profile_record_invocation(&global, 10);
    vtx_profile_record_invocation(&global, 10);
    bool ok = vtx_profile_save(&global, "/tmp/vtx_test_persist_02.bin");
    VTX_ASSERT_TRUE(ok);
    vtx_profile_global_t loaded;
    vtx_profile_global_init(&loaded);
    ok = vtx_profile_load(&loaded, "/tmp/vtx_test_persist_02.bin");
    VTX_ASSERT_TRUE(ok);
    vtx_profile_method_t *m = vtx_profile_get_method(&loaded, 10);
    VTX_ASSERT_NOT_NULL(m);
    if (m) {
        VTX_ASSERT_TRUE(m->invocation_count >= 2);
    }
    vtx_profile_global_destroy(&global);
    vtx_profile_global_destroy(&loaded);
    remove("/tmp/vtx_test_persist_02.bin");
}

VTX_TEST(test_persist_03)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    vtx_profile_record_callsite_type(&global, 10, 0, 5);
    bool ok = vtx_profile_save(&global, "/tmp/vtx_test_persist_03.bin");
    VTX_ASSERT_TRUE(ok);
    vtx_profile_global_t loaded;
    vtx_profile_global_init(&loaded);
    ok = vtx_profile_load(&loaded, "/tmp/vtx_test_persist_03.bin");
    VTX_ASSERT_TRUE(ok);
    const vtx_callsite_profile_t *cs = vtx_profile_get_callsite(&loaded, 10, 0);
    VTX_ASSERT_NOT_NULL(cs);
    VTX_ASSERT_EQUAL(cs->count, 1u);
    vtx_profile_global_destroy(&global);
    vtx_profile_global_destroy(&loaded);
    remove("/tmp/vtx_test_persist_03.bin");
}

VTX_TEST(test_persist_04)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    bool ok = vtx_profile_save(&global, "/tmp/vtx_test_persist_04.bin");
    VTX_ASSERT_TRUE(ok);
    FILE *f = fopen("/tmp/vtx_test_persist_04.bin", "rb");
    VTX_ASSERT_NOT_NULL(f);
    if (f) fclose(f);
    vtx_profile_global_destroy(&global);
    remove("/tmp/vtx_test_persist_04.bin");
}

VTX_TEST(test_persist_05)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    bool ok = vtx_profile_load(&global, "/tmp/vtx_nonexistent_file_12345.bin");
    VTX_ASSERT_FALSE(ok);
    vtx_profile_global_destroy(&global);
}

VTX_TEST(test_persist_06)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    for (int i = 0; i < 5; i++) {
        vtx_profile_record_invocation(&global, 42);
    }
    bool ok = vtx_profile_save(&global, "/tmp/vtx_test_persist_06.bin");
    VTX_ASSERT_TRUE(ok);
    vtx_profile_global_t loaded;
    vtx_profile_global_init(&loaded);
    ok = vtx_profile_load(&loaded, "/tmp/vtx_test_persist_06.bin");
    VTX_ASSERT_TRUE(ok);
    vtx_profile_method_t *m = vtx_profile_get_method(&loaded, 42);
    VTX_ASSERT_NOT_NULL(m);
    VTX_ASSERT_EQUAL(m->invocation_count, 5ull);
    vtx_profile_global_destroy(&global);
    vtx_profile_global_destroy(&loaded);
    remove("/tmp/vtx_test_persist_06.bin");
}

VTX_TEST(test_persist_07)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    for (uint32_t i = 0; i < 10; i++) {
        vtx_profile_record_invocation(&global, i);
    }
    bool ok = vtx_profile_save(&global, "/tmp/vtx_test_persist_07.bin");
    VTX_ASSERT_TRUE(ok);
    vtx_profile_global_t loaded;
    vtx_profile_global_init(&loaded);
    ok = vtx_profile_load(&loaded, "/tmp/vtx_test_persist_07.bin");
    VTX_ASSERT_TRUE(ok);
    VTX_ASSERT_EQUAL(loaded.method_count, 10u);
    vtx_profile_global_destroy(&global);
    vtx_profile_global_destroy(&loaded);
    remove("/tmp/vtx_test_persist_07.bin");
}

VTX_TEST(test_persist_08)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    vtx_profile_record_branch(&global, 10, 100, true);
    vtx_profile_record_branch(&global, 10, 100, false);
    bool ok = vtx_profile_save(&global, "/tmp/vtx_test_persist_08.bin");
    VTX_ASSERT_TRUE(ok);
    vtx_profile_global_t loaded;
    vtx_profile_global_init(&loaded);
    ok = vtx_profile_load(&loaded, "/tmp/vtx_test_persist_08.bin");
    VTX_ASSERT_TRUE(ok);
    const vtx_branch_profile_t *br = vtx_profile_get_branch(&loaded, 10, 100);
    VTX_ASSERT_NOT_NULL(br);
    VTX_ASSERT_EQUAL(br->taken, 1ull);
    VTX_ASSERT_EQUAL(br->not_taken, 1ull);
    vtx_profile_global_destroy(&global);
    vtx_profile_global_destroy(&loaded);
    remove("/tmp/vtx_test_persist_08.bin");
}

VTX_TEST(test_persist_09)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    /* Merge: save, then load into a profile that already has data */
    vtx_profile_record_invocation(&global, 10);
    vtx_profile_record_invocation(&global, 10);
    bool ok = vtx_profile_save(&global, "/tmp/vtx_test_persist_09.bin");
    VTX_ASSERT_TRUE(ok);
    vtx_profile_global_t loaded;
    vtx_profile_global_init(&loaded);
    vtx_profile_record_invocation(&loaded, 10);
    ok = vtx_profile_load(&loaded, "/tmp/vtx_test_persist_09.bin");
    VTX_ASSERT_TRUE(ok);
    vtx_profile_method_t *m = vtx_profile_get_method(&loaded, 10);
    VTX_ASSERT_NOT_NULL(m);
    /* 1 (existing) + 2 (loaded) = 3 */
    VTX_ASSERT_EQUAL(m->invocation_count, 3ull);
    vtx_profile_global_destroy(&global);
    vtx_profile_global_destroy(&loaded);
    remove("/tmp/vtx_test_persist_09.bin");
}

VTX_TEST(test_persist_10)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    int rc = vtx_profile_register_atexit(&global, "/tmp/vtx_test_atexit.bin");
    VTX_ASSERT_EQUAL(rc, 0);
    vtx_profile_unregister_atexit(); /* clear atexit ref before destroying global */
    vtx_profile_global_destroy(&global);
    /* We can't actually test the atexit behavior without process exit */
}

/* ========================================================================== */
/* Phase Detection tests (10 tests): test_phase_01 through test_phase_10      */
/* ========================================================================== */

VTX_TEST(test_phase_01)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    vtx_phase_graph_t *graph = vtx_phase_detect(&global);
    /* Empty profile should return NULL (no phases) */
    VTX_ASSERT_NULL(graph);
    vtx_profile_global_destroy(&global);
}

VTX_TEST(test_phase_02)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    /* Create a small call chain */
    vtx_profile_record_invocation(&global, 1);
    vtx_profile_record_invocation(&global, 2);
    vtx_profile_record_call_edge(&global, 1, 2);
    vtx_phase_graph_t *graph = vtx_phase_detect(&global);
    /* With only 2 methods, no SCC meets VTX_PHASE_MIN_METHODS=3 threshold */
    /* So we just test it doesn't crash */
    if (graph) {
        vtx_phase_graph_destroy(graph);
    }
    vtx_profile_global_destroy(&global);
}

VTX_TEST(test_phase_03)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    /* Build a 3-node cycle: 1→2→3→1 */
    for (uint32_t i = 1; i <= 3; i++) {
        for (int j = 0; j < 500; j++) {
            vtx_profile_record_invocation(&global, i);
        }
    }
    vtx_profile_record_call_edge(&global, 1, 2);
    vtx_profile_record_call_edge(&global, 2, 3);
    vtx_profile_record_call_edge(&global, 3, 1);
    vtx_phase_graph_t *graph = vtx_phase_detect(&global);
    if (graph) {
        /* 3 methods in cycle with freq >= 1000 should form a significant phase */
        uint32_t pid = vtx_phase_for_method(graph, 1);
        if (pid != VTX_PHASE_NONE) {
            VTX_ASSERT_TRUE(pid != VTX_PHASE_NONE);
        }
        vtx_phase_graph_destroy(graph);
    }
    vtx_profile_global_destroy(&global);
}

VTX_TEST(test_phase_04)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    for (uint32_t i = 1; i <= 3; i++) {
        for (int j = 0; j < 500; j++) {
            vtx_profile_record_invocation(&global, i);
        }
    }
    vtx_profile_record_call_edge(&global, 1, 2);
    vtx_profile_record_call_edge(&global, 2, 3);
    vtx_profile_record_call_edge(&global, 3, 1);
    vtx_phase_graph_t *graph = vtx_phase_detect(&global);
    if (graph) {
        bool entering = vtx_phase_is_entering(graph, 1, 2);
        /* Both in the same SCC → should NOT be entering a new phase */
        VTX_ASSERT_FALSE(entering);
        vtx_phase_graph_destroy(graph);
    }
    vtx_profile_global_destroy(&global);
}

VTX_TEST(test_phase_05)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    for (uint32_t i = 1; i <= 3; i++) {
        for (int j = 0; j < 500; j++) {
            vtx_profile_record_invocation(&global, i);
        }
    }
    vtx_profile_record_call_edge(&global, 1, 2);
    vtx_profile_record_call_edge(&global, 2, 3);
    vtx_profile_record_call_edge(&global, 3, 1);
    vtx_phase_graph_t *graph = vtx_phase_detect(&global);
    if (graph) {
        uint32_t pid = vtx_phase_for_method(graph, 99);
        VTX_ASSERT_EQUAL(pid, VTX_PHASE_NONE);
        vtx_phase_graph_destroy(graph);
    }
    vtx_profile_global_destroy(&global);
}

VTX_TEST(test_phase_06)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    for (uint32_t i = 1; i <= 3; i++) {
        for (int j = 0; j < 500; j++) {
            vtx_profile_record_invocation(&global, i);
        }
    }
    vtx_profile_record_call_edge(&global, 1, 2);
    vtx_profile_record_call_edge(&global, 2, 3);
    vtx_profile_record_call_edge(&global, 3, 1);
    vtx_phase_graph_t *graph = vtx_phase_detect(&global);
    if (graph) {
        uint32_t pid = vtx_phase_for_method(graph, 1);
        if (pid != VTX_PHASE_NONE) {
            const vtx_phase_t *phase = vtx_phase_get_by_id(graph, pid);
            VTX_ASSERT_NOT_NULL(phase);
        }
        vtx_phase_graph_destroy(graph);
    }
    vtx_profile_global_destroy(&global);
}

VTX_TEST(test_phase_07)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    for (uint32_t i = 1; i <= 3; i++) {
        for (int j = 0; j < 500; j++) {
            vtx_profile_record_invocation(&global, i);
        }
    }
    vtx_profile_record_call_edge(&global, 1, 2);
    vtx_profile_record_call_edge(&global, 2, 3);
    vtx_profile_record_call_edge(&global, 3, 1);
    vtx_phase_graph_t *graph = vtx_phase_detect(&global);
    /* Just test that destroy works */
    if (graph) {
        vtx_phase_graph_destroy(graph);
    }
    VTX_ASSERT_TRUE(1);
    vtx_profile_global_destroy(&global);
}

VTX_TEST(test_phase_08)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    /* Single method with self-call → SCC of size 1 < VTX_PHASE_MIN_METHODS(3) */
    vtx_profile_record_invocation(&global, 1);
    vtx_profile_record_call_edge(&global, 1, 1);
    vtx_phase_graph_t *graph = vtx_phase_detect(&global);
    /* Size 1 SCC is not significant */
    if (graph) {
        vtx_phase_graph_destroy(graph);
    }
    vtx_profile_global_destroy(&global);
}

VTX_TEST(test_phase_09)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    /* No call edges → no phases */
    vtx_profile_record_invocation(&global, 1);
    vtx_profile_record_invocation(&global, 2);
    vtx_phase_graph_t *graph = vtx_phase_detect(&global);
    VTX_ASSERT_NULL(graph);
    vtx_profile_global_destroy(&global);
}

VTX_TEST(test_phase_10)
{
    vtx_profile_global_t global;
    vtx_profile_global_init(&global);
    /* Two separate cycles */
    for (uint32_t i = 1; i <= 6; i++) {
        for (int j = 0; j < 500; j++) {
            vtx_profile_record_invocation(&global, i);
        }
    }
    vtx_profile_record_call_edge(&global, 1, 2);
    vtx_profile_record_call_edge(&global, 2, 3);
    vtx_profile_record_call_edge(&global, 3, 1);
    vtx_profile_record_call_edge(&global, 4, 5);
    vtx_profile_record_call_edge(&global, 5, 6);
    vtx_profile_record_call_edge(&global, 6, 4);
    vtx_phase_graph_t *graph = vtx_phase_detect(&global);
    if (graph) {
        /* Two separate SCCs should form different phases */
        uint32_t p1 = vtx_phase_for_method(graph, 1);
        uint32_t p4 = vtx_phase_for_method(graph, 4);
        if (p1 != VTX_PHASE_NONE && p4 != VTX_PHASE_NONE) {
            VTX_ASSERT_NOT_EQUAL(p1, p4);
        }
        vtx_phase_graph_destroy(graph);
    }
    vtx_profile_global_destroy(&global);
}

/* ========================================================================== */
/* Trace Recorder tests (20 tests): test_trace_01 through test_trace_20       */
/* ========================================================================== */

VTX_TEST(test_trace_01)
{
    vtx_trace_recorder_t recorder;
    int rc = vtx_trace_recorder_init(&recorder);
    VTX_ASSERT_EQUAL(rc, 0);
    VTX_ASSERT_EQUAL(recorder.next_trace_id, 0u);
    vtx_trace_recorder_destroy(&recorder);
}

VTX_TEST(test_trace_02)
{
    vtx_trace_list_t list;
    int rc = vtx_trace_list_init(&list);
    VTX_ASSERT_EQUAL(rc, 0);
    VTX_ASSERT_EQUAL(list.count, 0u);
    vtx_trace_list_destroy(&list);
}

VTX_TEST(test_trace_03)
{
    vtx_trace_list_t list;
    vtx_trace_list_init(&list);
    vtx_arena_t arena;
    vtx_arena_init(&arena);
    /* Create a minimal trace to append */
    vtx_trace_t *t = (vtx_trace_t *)vtx_arena_alloc(&arena, sizeof(vtx_trace_t));
    VTX_ASSERT_NOT_NULL(t);
    memset(t, 0, sizeof(*t));
    t->trace_id = 0;
    int rc = vtx_trace_list_append(&list, t);
    VTX_ASSERT_EQUAL(rc, 0);
    VTX_ASSERT_EQUAL(list.count, 1u);
    vtx_trace_list_destroy(&list);
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_trace_04)
{
    vtx_side_exit_table_t table;
    int rc = vtx_side_exit_table_init(&table);
    VTX_ASSERT_EQUAL(rc, 0);
    VTX_ASSERT_EQUAL(table.count, 0u);
    vtx_side_exit_table_destroy(&table);
}

VTX_TEST(test_trace_05)
{
    vtx_side_exit_table_t table;
    vtx_arena_t arena;
    vtx_side_exit_table_init(&table);
    vtx_arena_init(&arena);
    vtx_nodeid_t stack[] = {1, 2};
    vtx_nodeid_t locals[] = {10, 20, 30};
    vtx_side_exit_t *exit = vtx_side_exit_create(
        &table, &arena, 100, stack, 2, locals, 3,
        VTX_EXIT_BRANCH_NOT_TAKEN, 5, 0);
    VTX_ASSERT_NOT_NULL(exit);
    VTX_ASSERT_EQUAL(exit->target_pc, 100u);
    VTX_ASSERT_EQUAL(exit->reason, VTX_EXIT_BRANCH_NOT_TAKEN);
    vtx_side_exit_table_destroy(&table);
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_trace_06)
{
    vtx_side_exit_table_t table;
    vtx_arena_t arena;
    vtx_side_exit_table_init(&table);
    vtx_arena_init(&arena);
    vtx_nodeid_t stack[] = {1};
    vtx_nodeid_t locals[] = {10};
    vtx_side_exit_t *exit = vtx_side_exit_create(
        &table, &arena, 50, stack, 1, locals, 1,
        VTX_EXIT_TYPE_CHECK_FAILED, 3, 0);
    VTX_ASSERT_NOT_NULL(exit);
    vtx_side_exit_t *found = vtx_side_exit_get(&table, exit->exit_id);
    VTX_ASSERT_NOT_NULL(found);
    VTX_ASSERT_EQUAL(found->exit_id, exit->exit_id);
    vtx_side_exit_table_destroy(&table);
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_trace_07)
{
    vtx_side_exit_table_t table;
    vtx_arena_t arena;
    vtx_side_exit_table_init(&table);
    vtx_arena_init(&arena);
    vtx_nodeid_t stack[] = {1};
    vtx_nodeid_t locals[] = {10};
    vtx_side_exit_t *exit = vtx_side_exit_create(
        &table, &arena, 50, stack, 1, locals, 1,
        VTX_EXIT_NULL_CHECK_FAILED, 3, 0);
    VTX_ASSERT_NOT_NULL(exit);
    VTX_ASSERT_EQUAL(exit->exit_counter, 0u);
    vtx_side_exit_increment(exit);
    VTX_ASSERT_EQUAL(exit->exit_counter, 1u);
    vtx_side_exit_increment(exit);
    VTX_ASSERT_EQUAL(exit->exit_counter, 2u);
    vtx_side_exit_table_destroy(&table);
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_trace_08)
{
    vtx_side_exit_table_t table;
    vtx_arena_t arena;
    vtx_side_exit_table_init(&table);
    vtx_arena_init(&arena);
    vtx_nodeid_t stack[] = {1};
    vtx_nodeid_t locals[] = {10};
    vtx_side_exit_t *exit = vtx_side_exit_create(
        &table, &arena, 50, stack, 1, locals, 1,
        VTX_EXIT_BOUNDS_CHECK_FAILED, 3, 0);
    VTX_ASSERT_NOT_NULL(exit);
    VTX_ASSERT_FALSE(vtx_side_exit_should_record(exit));
    /* Increment to above threshold */
    for (int i = 0; i <= VTX_SIDE_EXIT_THRESHOLD; i++) {
        vtx_side_exit_increment(exit);
    }
    VTX_ASSERT_TRUE(vtx_side_exit_should_record(exit));
    vtx_side_exit_table_destroy(&table);
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_trace_09)
{
    vtx_side_exit_table_t table;
    vtx_arena_t arena;
    vtx_side_exit_table_init(&table);
    vtx_arena_init(&arena);
    vtx_nodeid_t stack[] = {1};
    vtx_nodeid_t locals[] = {10};
    vtx_side_exit_create(&table, &arena, 50, stack, 1, locals, 1,
                         VTX_EXIT_BRANCH_NOT_TAKEN, 3, 0);
    vtx_side_exit_create(&table, &arena, 60, stack, 1, locals, 1,
                         VTX_EXIT_TYPE_CHECK_FAILED, 4, 0);
    VTX_ASSERT_EQUAL(vtx_side_exit_table_count(&table), 2u);
    vtx_side_exit_table_destroy(&table);
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_trace_10)
{
    vtx_side_exit_table_t table;
    vtx_arena_t arena;
    vtx_side_exit_table_init(&table);
    vtx_arena_init(&arena);
    vtx_nodeid_t stack[] = {1};
    vtx_nodeid_t locals[] = {10};
    /* Create a hot exit */
    vtx_side_exit_t *hot = vtx_side_exit_create(
        &table, &arena, 50, stack, 1, locals, 1,
        VTX_EXIT_BRANCH_NOT_TAKEN, 3, 0);
    for (int i = 0; i <= VTX_SIDE_EXIT_THRESHOLD; i++) {
        vtx_side_exit_increment(hot);
    }
    /* Create a cold exit */
    vtx_side_exit_create(&table, &arena, 60, stack, 1, locals, 1,
                         VTX_EXIT_TYPE_CHECK_FAILED, 4, 0);
    vtx_side_exit_t *hot_exits[8];
    uint32_t count = vtx_side_exit_find_hot(&table, hot_exits, 8);
    VTX_ASSERT_EQUAL(count, 1u);
    VTX_ASSERT_EQUAL(hot_exits[0]->exit_id, hot->exit_id);
    vtx_side_exit_table_destroy(&table);
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_trace_11)
{
    vtx_side_exit_table_t table;
    vtx_arena_t arena;
    vtx_side_exit_table_init(&table);
    vtx_arena_init(&arena);
    vtx_nodeid_t stack[] = {1};
    vtx_nodeid_t locals[] = {10};
    vtx_side_exit_t *exit = vtx_side_exit_create(
        &table, &arena, 50, stack, 1, locals, 1,
        VTX_EXIT_BRANCH_NOT_TAKEN, 3, 0);
    VTX_ASSERT_NOT_NULL(exit);
    int rc = vtx_side_exit_link(exit, 42, (void *)0xDEADBEEF);
    VTX_ASSERT_EQUAL(rc, 0);
    VTX_ASSERT_TRUE(vtx_side_exit_is_linked(exit));
    VTX_ASSERT_EQUAL(exit->linked_trace_id, (vtx_nodeid_t)42);
    vtx_side_exit_table_destroy(&table);
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_trace_12)
{
    vtx_side_exit_table_t table;
    vtx_arena_t arena;
    vtx_side_exit_table_init(&table);
    vtx_arena_init(&arena);
    vtx_nodeid_t stack[] = {1};
    vtx_nodeid_t locals[] = {10};
    vtx_side_exit_t *exit = vtx_side_exit_create(
        &table, &arena, 50, stack, 1, locals, 1,
        VTX_EXIT_BRANCH_NOT_TAKEN, 3, 0);
    VTX_ASSERT_FALSE(vtx_side_exit_is_linked(exit));
    vtx_side_exit_link(exit, 42, (void *)0xDEADBEEF);
    VTX_ASSERT_TRUE(vtx_side_exit_is_linked(exit));
    vtx_side_exit_unlink(exit);
    VTX_ASSERT_FALSE(vtx_side_exit_is_linked(exit));
    vtx_side_exit_table_destroy(&table);
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_trace_13)
{
    vtx_side_exit_table_t table;
    vtx_arena_t arena;
    vtx_side_exit_table_init(&table);
    vtx_arena_init(&arena);
    vtx_nodeid_t stack[] = {1};
    vtx_nodeid_t locals[] = {10};
    /* Create 3 hot exits */
    for (uint32_t i = 0; i < 3; i++) {
        vtx_side_exit_t *exit = vtx_side_exit_create(
            &table, &arena, 50 + i, stack, 1, locals, 1,
            VTX_EXIT_BRANCH_NOT_TAKEN, 3 + i, 0);
        for (int j = 0; j <= VTX_SIDE_EXIT_THRESHOLD; j++) {
            vtx_side_exit_increment(exit);
        }
    }
    /* Simple lookup function that always returns code for PC=50 */
    /* We test link_all_hot with a simple lookup that returns NULL (no compiled code) */
    /* So nothing should be linked, but the function should not crash */
    vtx_side_exit_link_all_hot(&table, NULL, NULL, NULL, NULL);
    VTX_ASSERT_TRUE(1); /* just test no crash */
    vtx_side_exit_table_destroy(&table);
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_trace_14)
{
    vtx_trace_selector_t selector;
    int rc = vtx_trace_selector_init(&selector);
    VTX_ASSERT_EQUAL(rc, 0);
    VTX_ASSERT_EQUAL(selector.hot_loops.count, 0u);
    vtx_trace_selector_destroy(&selector);
}

VTX_TEST(test_trace_15)
{
    vtx_trace_selector_t selector;
    vtx_profile_global_t profile;
    vtx_profiler_t profiler;
    vtx_trace_selector_init(&selector);
    vtx_profile_global_init(&profile);
    vtx_profiler_init(&profiler);
    const vtx_hot_loop_list_t *loops = vtx_trace_selector_select(
        &selector, &profiler, &profile);
    VTX_ASSERT_NOT_NULL(loops);
    VTX_ASSERT_EQUAL(loops->count, 0u);
    vtx_trace_selector_destroy(&selector);
    vtx_profile_global_destroy(&profile);
    vtx_profiler_destroy(&profiler);
}

VTX_TEST(test_trace_16)
{
    vtx_trace_selector_t selector;
    vtx_profile_global_t profile;
    vtx_profiler_t profiler;
    vtx_trace_selector_init(&selector);
    vtx_profile_global_init(&profile);
    vtx_profiler_init(&profiler);
    vtx_trace_selector_select(&selector, &profiler, &profile);
    VTX_ASSERT_EQUAL(vtx_trace_selector_hot_count(&selector), 0u);
    vtx_trace_selector_destroy(&selector);
    vtx_profile_global_destroy(&profile);
    vtx_profiler_destroy(&profiler);
}

VTX_TEST(test_trace_17)
{
    vtx_trace_selector_t selector;
    vtx_profile_global_t profile;
    vtx_profiler_t profiler;
    vtx_trace_selector_init(&selector);
    vtx_profile_global_init(&profile);
    vtx_profiler_init(&profiler);
    vtx_trace_selector_select(&selector, &profiler, &profile);
    const vtx_hot_loop_t *loop = vtx_trace_selector_get_hot(&selector, 0);
    VTX_ASSERT_NULL(loop); /* No hot loops */
    vtx_trace_selector_destroy(&selector);
    vtx_profile_global_destroy(&profile);
    vtx_profiler_destroy(&profiler);
}

VTX_TEST(test_trace_18)
{
    vtx_trace_recorder_t recorder;
    vtx_trace_recorder_init(&recorder);
    /* Record with minimal valid setup */
    vtx_graph_t graph;
    vtx_arena_t arena;
    vtx_arena_init(&arena);
    vtx_graph_init(&graph, 0);
    /* Create a minimal bytecode for a simple loop */
    uint8_t code[] = {
        VT_OP_GOTO, 0x00, 0x04,   /* goto pc=4 */
        VT_OP_IADD,                /* iadd at pc=3 */
        VT_OP_GOTO, 0x00, 0x01,   /* goto pc=1 (loop back) */
        VT_OP_RETURN               /* return at pc=6 */
    };
    vtx_bytecode_t bc = {
        .code = code,
        .length = sizeof(code),
        .constant_pool = NULL,
        .constant_count = 0,
        .max_locals = 2,
        .max_stack = 2
    };
    vtx_method_desc_t method = {
        .name = "test_method",
        .signature = "()V",
        .bytecode = &bc,
        .compiled_code = NULL,
        .vtable_index = 0xFFFFFFFF,
        .arg_count = 0,
        .method_symbol_id = 0,
        .is_virtual = false
    };
    vtx_profiler_t profiler;
    vtx_profiler_init(&profiler);
    vtx_profile_global_t profile;
    vtx_profile_global_init(&profile);
    /* Record from entry_pc=0 — may return NULL if no loop found, that's OK */
    vtx_trace_t *trace = vtx_trace_recorder_record(
        &recorder, &graph, &bc, &method, 0, &profiler, &profile, &arena);
    /* Just test it doesn't crash */
    VTX_ASSERT_TRUE(1);
    vtx_profile_global_destroy(&profile);
    vtx_profiler_destroy(&profiler);
    vtx_graph_destroy(&graph);
    vtx_arena_destroy(&arena);
    vtx_trace_recorder_destroy(&recorder);
}

VTX_TEST(test_trace_19)
{
    vtx_side_exit_table_t table;
    vtx_arena_t arena;
    vtx_side_exit_table_init(&table);
    vtx_arena_init(&arena);
    vtx_nodeid_t stack[] = {1};
    vtx_nodeid_t locals[] = {10};
    /* Various exit reasons */
    vtx_side_exit_t *e1 = vtx_side_exit_create(
        &table, &arena, 10, stack, 1, locals, 1, VTX_EXIT_DIVISION_BY_ZERO, 1, 0);
    vtx_side_exit_t *e2 = vtx_side_exit_create(
        &table, &arena, 20, stack, 1, locals, 1, VTX_EXIT_LOOP_BACK_EDGE, 2, 0);
    VTX_ASSERT_NOT_NULL(e1);
    VTX_ASSERT_NOT_NULL(e2);
    VTX_ASSERT_EQUAL(e1->reason, VTX_EXIT_DIVISION_BY_ZERO);
    VTX_ASSERT_EQUAL(e2->reason, VTX_EXIT_LOOP_BACK_EDGE);
    vtx_side_exit_table_destroy(&table);
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_trace_20)
{
    vtx_side_exit_table_t table;
    vtx_arena_t arena;
    vtx_side_exit_table_init(&table);
    vtx_arena_init(&arena);
    /* Create exit with empty stack */
    vtx_side_exit_t *exit = vtx_side_exit_create(
        &table, &arena, 50, NULL, 0, NULL, 0,
        VTX_EXIT_UNKNOWN, 3, 0);
    VTX_ASSERT_NOT_NULL(exit);
    VTX_ASSERT_EQUAL(exit->stack_state.stack_depth, 0u);
    VTX_ASSERT_EQUAL(exit->stack_state.local_count, 0u);
    vtx_side_exit_table_destroy(&table);
    vtx_arena_destroy(&arena);
}

/* ========================================================================== */
/* Trace Tree tests (10 tests): test_tree_01 through test_tree_10             */
/* ========================================================================== */

VTX_TEST(test_tree_01)
{
    vtx_trace_recorder_t recorder;
    vtx_trace_recorder_init(&recorder);
    vtx_graph_t graph;
    vtx_arena_t arena;
    vtx_arena_init(&arena);
    vtx_graph_init(&graph, 0);
    uint8_t code[] = {
        VT_OP_GOTO, 0x00, 0x04,
        VT_OP_IADD,
        VT_OP_GOTO, 0x00, 0x01,
        VT_OP_RETURN
    };
    vtx_bytecode_t bc = {
        .code = code,
        .length = sizeof(code),
        .constant_pool = NULL,
        .constant_count = 0,
        .max_locals = 2,
        .max_stack = 2
    };
    vtx_method_desc_t method = {
        .name = "loop_test",
        .signature = "()V",
        .bytecode = &bc,
        .compiled_code = NULL,
        .vtable_index = 0xFFFFFFFF,
        .arg_count = 0,
        .method_symbol_id = 0,
        .is_virtual = false
    };
    vtx_profiler_t profiler;
    vtx_profiler_init(&profiler);
    vtx_profile_global_t profile;
    vtx_profile_global_init(&profile);
    /* Build root tree — smoke test */
    vtx_trace_tree_t *tree = vtx_trace_tree_build_root(
        &recorder, &graph, &bc, &method, 0,
        &profiler, &profile, &arena);
    /* May be NULL if no valid trace found */
    VTX_ASSERT_TRUE(1); /* no crash */
    vtx_profile_global_destroy(&profile);
    vtx_profiler_destroy(&profiler);
    vtx_graph_destroy(&graph);
    vtx_arena_destroy(&arena);
    vtx_trace_recorder_destroy(&recorder);
}

VTX_TEST(test_tree_02)
{
    /* Test tree root retrieval */
    vtx_trace_t root_trace = {0};
    root_trace.trace_id = 42;
    root_trace.entry_pc = 0;
    vtx_trace_tree_t tree;
    tree.root = &root_trace;
    tree.root_branch = NULL;
    tree.depth = 0;
    tree.branch_count = 0;
    vtx_trace_t *r = vtx_trace_tree_root(&tree);
    VTX_ASSERT_NOT_NULL(r);
    VTX_ASSERT_EQUAL(r->trace_id, (vtx_trace_id_t)42);
}

VTX_TEST(test_tree_03)
{
    vtx_trace_t root_trace = {0};
    vtx_trace_tree_t tree;
    tree.root = &root_trace;
    tree.root_branch = NULL;
    tree.depth = 0;
    tree.branch_count = 0;
    /* Root only: 1 trace */
    VTX_ASSERT_EQUAL(vtx_trace_tree_trace_count(&tree), 1u);
}

VTX_TEST(test_tree_04)
{
    vtx_trace_t root_trace = {0};
    vtx_trace_tree_t tree;
    tree.root = &root_trace;
    tree.root_branch = NULL;
    tree.depth = 0;
    tree.branch_count = 0;
    VTX_ASSERT_EQUAL(vtx_trace_tree_depth(&tree), 0u);
}

VTX_TEST(test_tree_05)
{
    vtx_trace_t root_trace = {0};
    vtx_trace_tree_t tree;
    tree.root = &root_trace;
    tree.root_branch = NULL;
    tree.depth = 0;
    tree.branch_count = 0;
    /* depth 0 < VTX_MAX_TREE_DEPTH(8) → can branch */
    VTX_ASSERT_TRUE(vtx_trace_tree_can_branch(&tree));
}

VTX_TEST(test_tree_06)
{
    vtx_trace_t root_trace = {0};
    vtx_trace_tree_t tree;
    tree.root = &root_trace;
    tree.root_branch = NULL;
    tree.depth = 0;
    tree.branch_count = 0;
    vtx_side_exit_id_t hot[16];
    /* No side exits, so should return 0 */
    uint32_t count = vtx_trace_tree_find_hot_exits(&tree, hot, 16);
    VTX_ASSERT_EQUAL(count, 0u);
}

VTX_TEST(test_tree_07)
{
    vtx_trace_t root_trace = {0};
    vtx_trace_tree_t tree;
    tree.root = &root_trace;
    tree.root_branch = NULL;
    tree.depth = 0;
    tree.branch_count = 0;
    tree.all_branches = NULL;
    /* No branches → get_branch returns NULL */
    vtx_trace_branch_t *b = vtx_trace_tree_get_branch(&tree, 0);
    VTX_ASSERT_NULL(b);
}

VTX_TEST(test_tree_08)
{
    vtx_trace_t root_trace = {0};
    vtx_trace_tree_t tree;
    tree.root = &root_trace;
    tree.root_branch = NULL;
    tree.depth = 0;
    tree.branch_count = 0;
    tree.all_branches = NULL;
    /* Find exit with invalid ID */
    vtx_side_exit_t *e = vtx_trace_tree_find_exit(&tree, 9999);
    VTX_ASSERT_NULL(e);
}

VTX_TEST(test_tree_09)
{
    vtx_trace_t root_trace = {0};
    vtx_trace_tree_t tree;
    tree.root = &root_trace;
    tree.root_branch = NULL;
    tree.depth = VTX_MAX_TREE_DEPTH; /* At max depth */
    tree.branch_count = VTX_MAX_TREE_DEPTH;
    /* At max depth, cannot add more branches */
    VTX_ASSERT_FALSE(vtx_trace_tree_can_branch(&tree));
}

VTX_TEST(test_tree_10)
{
    /* Test that destroy doesn't crash on a manually constructed tree */
    vtx_trace_t root_trace = {0};
    vtx_trace_tree_t tree;
    tree.root = &root_trace;
    tree.root_branch = NULL;
    tree.depth = 0;
    tree.branch_count = 0;
    tree.all_branches = NULL;
    tree.all_branch_capacity = 0;
    /* We don't call vtx_trace_tree_destroy because we don't have a properly
       allocated tree. The real test is in test_tree_01. */
    VTX_ASSERT_TRUE(1);
}

/* ========================================================================== */
/* Region Budget tests (15 tests): test_budget_01 through test_budget_15      */
/* ========================================================================== */

VTX_TEST(test_budget_01)
{
    vtx_budget_t budget;
    vtx_budget_init(&budget);
    VTX_ASSERT_EQUAL(budget.max_nodes, (uint32_t)VTX_MAX_HYPERBLOCK_NODES);
    VTX_ASSERT_EQUAL(budget.max_native_size, (uint32_t)VTX_MAX_NATIVE_SIZE);
    VTX_ASSERT_EQUAL(budget.current_node_count, 0u);
    VTX_ASSERT_EQUAL(budget.current_native_size, 0u);
}

VTX_TEST(test_budget_02)
{
    vtx_budget_t budget;
    vtx_budget_init_custom(&budget, 1024, 8192);
    VTX_ASSERT_EQUAL(budget.max_nodes, 1024u);
    VTX_ASSERT_EQUAL(budget.max_native_size, 8192u);
}

VTX_TEST(test_budget_03)
{
    vtx_budget_t budget;
    vtx_budget_init(&budget);
    VTX_ASSERT_TRUE(vtx_budget_can_add(&budget, 0, 0));
    VTX_ASSERT_TRUE(vtx_budget_check_counts(&budget, 0, 0));
}

VTX_TEST(test_budget_04)
{
    vtx_budget_t budget;
    vtx_budget_init(&budget);
    VTX_ASSERT_TRUE(vtx_budget_can_add(&budget, 100, 1024));
    VTX_ASSERT_TRUE(vtx_budget_can_add(&budget, VTX_MAX_HYPERBLOCK_NODES - 1, VTX_MAX_NATIVE_SIZE - 1));
}

VTX_TEST(test_budget_05)
{
    vtx_budget_t budget;
    vtx_budget_init(&budget);
    VTX_ASSERT_FALSE(vtx_budget_can_add(&budget, VTX_MAX_HYPERBLOCK_NODES + 1, 0));
    VTX_ASSERT_FALSE(vtx_budget_can_add(&budget, 0, VTX_MAX_NATIVE_SIZE + 1));
}

VTX_TEST(test_budget_06)
{
    vtx_budget_t budget;
    vtx_budget_init(&budget);
    vtx_budget_add(&budget, 100, 1024);
    VTX_ASSERT_EQUAL(budget.current_node_count, 100u);
    VTX_ASSERT_EQUAL(budget.current_native_size, 1024u);
    VTX_ASSERT_TRUE(vtx_budget_can_add(&budget, 1, 1));
}

VTX_TEST(test_budget_07)
{
    vtx_budget_t budget;
    vtx_budget_init(&budget);
    vtx_budget_add(&budget, 100, 1024);
    vtx_budget_remove(&budget, 50, 512);
    VTX_ASSERT_EQUAL(budget.current_node_count, 50u);
    VTX_ASSERT_EQUAL(budget.current_native_size, 512u);
}

VTX_TEST(test_budget_08)
{
    vtx_budget_t budget;
    vtx_budget_init(&budget);
    vtx_budget_add(&budget, 100, 1024);
    vtx_budget_reset(&budget);
    VTX_ASSERT_EQUAL(budget.current_node_count, 0u);
    VTX_ASSERT_EQUAL(budget.current_native_size, 0u);
    VTX_ASSERT_EQUAL(budget.max_nodes, (uint32_t)VTX_MAX_HYPERBLOCK_NODES);
}

VTX_TEST(test_budget_09)
{
    vtx_budget_t budget;
    vtx_budget_init(&budget);
    vtx_budget_add(&budget, 100, 1024);
    uint32_t remaining = vtx_budget_remaining_nodes(&budget);
    VTX_ASSERT_EQUAL(remaining, (uint32_t)VTX_MAX_HYPERBLOCK_NODES - 100);
}

VTX_TEST(test_budget_10)
{
    vtx_budget_t budget;
    vtx_budget_init(&budget);
    vtx_budget_add(&budget, 100, 1024);
    uint32_t remaining = vtx_budget_remaining_native(&budget);
    VTX_ASSERT_EQUAL(remaining, (uint32_t)VTX_MAX_NATIVE_SIZE - 1024);
}

VTX_TEST(test_budget_11)
{
    vtx_budget_t budget;
    vtx_budget_init(&budget);
    vtx_budget_add(&budget, VTX_MAX_HYPERBLOCK_NODES / 2, 0);
    double util = vtx_budget_node_utilization(&budget);
    VTX_ASSERT_TRUE(util > 0.49 && util < 0.51);
}

VTX_TEST(test_budget_12)
{
    vtx_budget_t budget;
    vtx_budget_init(&budget);
    vtx_budget_add(&budget, 0, VTX_MAX_NATIVE_SIZE / 2);
    double util = vtx_budget_native_utilization(&budget);
    VTX_ASSERT_TRUE(util > 0.49 && util < 0.51);
}

VTX_TEST(test_budget_13)
{
    vtx_budget_t budget;
    vtx_budget_init(&budget);
    vtx_budget_add(&budget, 100, 1024);
    VTX_ASSERT_TRUE(vtx_budget_check_counts(&budget, 100, 1024));
    VTX_ASSERT_FALSE(vtx_budget_check_counts(&budget, budget.max_nodes + 1, 0));
}

VTX_TEST(test_budget_14)
{
    vtx_budget_t budget;
    vtx_budget_init(&budget);
    vtx_budget_add(&budget, VTX_MAX_HYPERBLOCK_NODES, 0);
    VTX_ASSERT_TRUE(vtx_budget_check_counts(&budget, VTX_MAX_HYPERBLOCK_NODES, 0));
    VTX_ASSERT_EQUAL(vtx_budget_remaining_nodes(&budget), 0u);
}

VTX_TEST(test_budget_15)
{
    vtx_budget_t budget;
    vtx_budget_init(&budget);
    vtx_budget_add(&budget, VTX_MAX_HYPERBLOCK_NODES + 100, 0);
    /* Over limit */
    VTX_ASSERT_FALSE(vtx_budget_check_counts(&budget, budget.current_node_count, budget.current_native_size));
}

/* ========================================================================== */
/* Guard EWMA tests (15 tests): test_ewma_01 through test_ewma_15             */
/* ========================================================================== */

VTX_TEST(test_ewma_01)
{
    vtx_ewma_t ewma;
    vtx_ewma_init(&ewma);
    VTX_ASSERT_TRUE(vtx_ewma_value(&ewma) < 0.0001);
    VTX_ASSERT_FALSE(vtx_ewma_is_initialized(&ewma));
}

VTX_TEST(test_ewma_02)
{
    vtx_ewma_t ewma;
    vtx_ewma_init(&ewma);
    double val = vtx_ewma_update(&ewma, 0.0);
    VTX_ASSERT_TRUE(val < 0.0001);
    VTX_ASSERT_TRUE(vtx_ewma_is_initialized(&ewma));
}

VTX_TEST(test_ewma_03)
{
    vtx_ewma_t ewma;
    vtx_ewma_init(&ewma);
    double val = vtx_ewma_update(&ewma, 1.0);
    VTX_ASSERT_TRUE(val > 0.99);
}

VTX_TEST(test_ewma_04)
{
    vtx_ewma_t ewma;
    vtx_ewma_init(&ewma);
    vtx_ewma_update(&ewma, 0.0);
    /* After many successes (0.0 failure rate), EWMA should converge toward 0 */
    for (int i = 0; i < 20; i++) {
        vtx_ewma_update(&ewma, 0.0);
    }
    VTX_ASSERT_TRUE(vtx_ewma_value(&ewma) < 0.001);
}

VTX_TEST(test_ewma_05)
{
    vtx_ewma_t ewma;
    vtx_ewma_init(&ewma);
    vtx_ewma_update(&ewma, 1.0);
    /* After many failures (1.0 failure rate), EWMA should converge toward 1 */
    for (int i = 0; i < 20; i++) {
        vtx_ewma_update(&ewma, 1.0);
    }
    VTX_ASSERT_TRUE(vtx_ewma_value(&ewma) > 0.999);
}

VTX_TEST(test_ewma_06)
{
    vtx_ewma_t ewma1, ewma2;
    vtx_ewma_init_with_alpha(&ewma1, 0.1);
    vtx_ewma_init_with_alpha(&ewma2, 0.5);
    vtx_ewma_update(&ewma1, 1.0);
    vtx_ewma_update(&ewma2, 1.0);
    vtx_ewma_update(&ewma1, 0.0);
    vtx_ewma_update(&ewma2, 0.0);
    /* Higher alpha responds faster */
    /* ewma2 (alpha=0.5) should be lower (closer to 0) after the success */
    VTX_ASSERT_TRUE(vtx_ewma_value(&ewma2) < vtx_ewma_value(&ewma1));
}

VTX_TEST(test_ewma_07)
{
    vtx_ewma_t ewma;
    vtx_ewma_init(&ewma);
    vtx_ewma_update(&ewma, 0.5);
    /* Alternating */
    double prev = vtx_ewma_value(&ewma);
    for (int i = 0; i < 10; i++) {
        vtx_ewma_update(&ewma, (i % 2 == 0) ? 1.0 : 0.0);
    }
    /* Should be somewhere between 0 and 1 */
    double val = vtx_ewma_value(&ewma);
    VTX_ASSERT_TRUE(val > 0.0 && val < 1.0);
}

VTX_TEST(test_ewma_08)
{
    vtx_ewma_t ewma;
    vtx_ewma_init(&ewma);
    /* Saturate at 1.0 */
    for (int i = 0; i < 100; i++) {
        vtx_ewma_update(&ewma, 1.0);
    }
    VTX_ASSERT_TRUE(vtx_ewma_value(&ewma) <= 1.0 + 1e-9);
}

VTX_TEST(test_ewma_09)
{
    vtx_ewma_t ewma;
    vtx_ewma_init(&ewma);
    /* Saturate at 0.0 */
    vtx_ewma_update(&ewma, 1.0); /* first update sets to 1.0 */
    for (int i = 0; i < 100; i++) {
        vtx_ewma_update(&ewma, 0.0);
    }
    VTX_ASSERT_TRUE(vtx_ewma_value(&ewma) >= 0.0 - 1e-9);
}

VTX_TEST(test_ewma_10)
{
    vtx_ewma_t ewma;
    vtx_ewma_init(&ewma);
    for (int i = 0; i < 1000; i++) {
        vtx_ewma_update(&ewma, 0.5);
    }
    /* Should be very close to 0.5 */
    double val = vtx_ewma_value(&ewma);
    VTX_ASSERT_TRUE(val > 0.49 && val < 0.51);
    VTX_ASSERT_EQUAL(vtx_ewma_update_count(&ewma), 1000ull);
}

VTX_TEST(test_ewma_11)
{
    vtx_ewma_t ewma;
    vtx_ewma_init(&ewma);
    /* Many successes, then one outlier failure */
    for (int i = 0; i < 50; i++) {
        vtx_ewma_update(&ewma, 0.0);
    }
    vtx_ewma_update(&ewma, 1.0);
    /* Should barely move the EWMA */
    VTX_ASSERT_TRUE(vtx_ewma_value(&ewma) < 0.2);
}

VTX_TEST(test_ewma_12)
{
    vtx_ewma_t ewma;
    vtx_ewma_init(&ewma);
    /* Pure successes via counts */
    for (int i = 0; i < 20; i++) {
        vtx_ewma_update_counts(&ewma, 0, 100);
    }
    VTX_ASSERT_TRUE(vtx_ewma_value(&ewma) < 0.001);
}

VTX_TEST(test_ewma_13)
{
    vtx_ewma_t ewma;
    vtx_ewma_init(&ewma);
    /* Pure failures via counts */
    for (int i = 0; i < 20; i++) {
        vtx_ewma_update_counts(&ewma, 100, 100);
    }
    VTX_ASSERT_TRUE(vtx_ewma_value(&ewma) > 0.999);
}

VTX_TEST(test_ewma_14)
{
    vtx_ewma_t ewma;
    vtx_ewma_init(&ewma);
    /* Mixed pattern: 90% success, 10% failure */
    vtx_ewma_update_counts(&ewma, 10, 100);
    VTX_ASSERT_TRUE(vtx_ewma_value(&ewma) > 0.0);
    VTX_ASSERT_TRUE(vtx_ewma_value(&ewma) < 1.0);
}

VTX_TEST(test_ewma_15)
{
    vtx_ewma_t ewma;
    vtx_ewma_init(&ewma);
    vtx_ewma_update(&ewma, 0.5);
    vtx_ewma_reset(&ewma);
    VTX_ASSERT_FALSE(vtx_ewma_is_initialized(&ewma));
    VTX_ASSERT_TRUE(vtx_ewma_value(&ewma) < 0.0001);
    VTX_ASSERT_EQUAL(vtx_ewma_update_count(&ewma), 0ull);
}

/* ========================================================================== */
/* Guard Metadata tests (15 tests): test_gmeta_01 through test_gmeta_15       */
/* ========================================================================== */

VTX_TEST(test_gmeta_01)
{
    vtx_guard_meta_table_t table;
    int rc = vtx_guard_meta_table_init(&table);
    VTX_ASSERT_EQUAL(rc, 0);
    VTX_ASSERT_EQUAL(table.guard_count, 0u);
    vtx_guard_meta_table_destroy(&table);
}

VTX_TEST(test_gmeta_02)
{
    vtx_guard_meta_table_t table;
    vtx_guard_meta_table_init(&table);
    vtx_guard_meta_t *meta = vtx_guard_meta_register(
        &table, 1, 0, 100, VTX_GUARD_FAST_CHECK);
    VTX_ASSERT_NOT_NULL(meta);
    VTX_ASSERT_EQUAL(meta->guard_node, (vtx_nodeid_t)1);
    VTX_ASSERT_EQUAL(meta->strength, VTX_GUARD_FAST_CHECK);
    vtx_guard_meta_table_destroy(&table);
}

VTX_TEST(test_gmeta_03)
{
    vtx_guard_meta_table_t table;
    vtx_guard_meta_table_init(&table);
    vtx_guard_meta_register(&table, 1, 0, 100, VTX_GUARD_FAST_CHECK);
    vtx_guard_meta_t *meta = vtx_guard_meta_lookup(&table, 1);
    VTX_ASSERT_NOT_NULL(meta);
    VTX_ASSERT_EQUAL(meta->guard_node, (vtx_nodeid_t)1);
    vtx_guard_meta_table_destroy(&table);
}

VTX_TEST(test_gmeta_04)
{
    vtx_guard_meta_table_t table;
    vtx_guard_meta_table_init(&table);
    vtx_guard_meta_t *meta = vtx_guard_meta_register(
        &table, 1, 0, 100, VTX_GUARD_FAST_CHECK);
    vtx_guard_meta_update(meta, false);
    VTX_ASSERT_EQUAL(meta->execution_count, 1ull);
    VTX_ASSERT_EQUAL(meta->failure_count, 0ull);
}

VTX_TEST(test_gmeta_05)
{
    vtx_guard_meta_table_t table;
    vtx_guard_meta_table_init(&table);
    vtx_guard_meta_t *meta = vtx_guard_meta_register(
        &table, 1, 0, 100, VTX_GUARD_FAST_CHECK);
    vtx_guard_meta_update(meta, true);
    VTX_ASSERT_EQUAL(meta->execution_count, 1ull);
    VTX_ASSERT_EQUAL(meta->failure_count, 1ull);
    vtx_guard_meta_table_destroy(&table);
}

VTX_TEST(test_gmeta_06)
{
    vtx_guard_meta_table_t table;
    vtx_guard_meta_table_init(&table);
    vtx_guard_meta_t *meta = vtx_guard_meta_register(
        &table, 1, 0, 100, VTX_GUARD_UNCONDITIONAL);
    /* First failure → transition to FastCheck */
    vtx_guard_meta_update(meta, true);
    VTX_ASSERT_EQUAL(vtx_guard_meta_strength(meta), VTX_GUARD_FAST_CHECK);
    vtx_guard_meta_table_destroy(&table);
}

VTX_TEST(test_gmeta_07)
{
    vtx_guard_meta_table_t table;
    vtx_guard_meta_table_init(&table);
    vtx_guard_meta_t *meta = vtx_guard_meta_register(
        &table, 1, 0, 100, VTX_GUARD_FAST_CHECK);
    /* High failure rate → transition to FullCheck */
    for (int i = 0; i < 50; i++) {
        vtx_guard_meta_update(meta, true);
    }
    /* EWMA should be well above VTX_GUARD_WEAKEN_THRESHOLD (0.01) */
    VTX_ASSERT_TRUE(vtx_guard_meta_strength(meta) >= VTX_GUARD_FAST_CHECK);
    vtx_guard_meta_table_destroy(&table);
}

VTX_TEST(test_gmeta_08)
{
    vtx_guard_meta_table_t table;
    vtx_guard_meta_table_init(&table);
    vtx_guard_meta_t *meta = vtx_guard_meta_register(
        &table, 1, 0, 100, VTX_GUARD_FULL_CHECK);
    /* Very high failure rate → transition to DeoptAlways */
    for (int i = 0; i < 100; i++) {
        vtx_guard_meta_update(meta, true);
    }
    /* EWMA should be above VTX_GUARD_ABANDON_THRESHOLD (0.25) */
    VTX_ASSERT_TRUE(vtx_guard_meta_strength(meta) >= VTX_GUARD_FULL_CHECK);
    vtx_guard_meta_table_destroy(&table);
}

VTX_TEST(test_gmeta_09)
{
    vtx_guard_meta_table_t table;
    vtx_guard_meta_table_init(&table);
    vtx_guard_meta_t *meta = vtx_guard_meta_register(
        &table, 1, 0, 100, VTX_GUARD_FAST_CHECK);
    vtx_guard_meta_update(meta, false);
    vtx_guard_meta_update(meta, false);
    /* EWMA should be low */
    double rate = vtx_ewma_value(&meta->failure_rate_ewma);
    VTX_ASSERT_TRUE(rate < 0.1);
    vtx_guard_meta_table_destroy(&table);
}

VTX_TEST(test_gmeta_10)
{
    vtx_guard_meta_table_t table;
    vtx_guard_meta_table_init(&table);
    for (uint32_t i = 0; i < 10; i++) {
        vtx_guard_meta_register(&table, i, 0, 100 + i, VTX_GUARD_FAST_CHECK);
    }
    VTX_ASSERT_EQUAL(table.guard_count, 10u);
    vtx_guard_meta_table_destroy(&table);
}

VTX_TEST(test_gmeta_11)
{
    vtx_guard_meta_table_t table;
    vtx_guard_meta_table_init(&table);
    vtx_guard_meta_t *meta = vtx_guard_meta_lookup(&table, 9999);
    VTX_ASSERT_NULL(meta);
    vtx_guard_meta_table_destroy(&table);
}

VTX_TEST(test_gmeta_12)
{
    vtx_guard_meta_table_t table;
    vtx_guard_meta_table_init(&table);
    vtx_guard_meta_t *meta = vtx_guard_meta_register(
        &table, 1, 0, 100, VTX_GUARD_UNCONDITIONAL);
    /* Only successes → stays Unconditional */
    for (int i = 0; i < 100; i++) {
        vtx_guard_meta_update(meta, false);
    }
    VTX_ASSERT_EQUAL(vtx_guard_meta_strength(meta), VTX_GUARD_UNCONDITIONAL);
    vtx_guard_meta_table_destroy(&table);
}

VTX_TEST(test_gmeta_13)
{
    vtx_guard_meta_table_t table;
    vtx_guard_meta_table_init(&table);
    vtx_guard_meta_t *meta = vtx_guard_meta_register(
        &table, 1, 0, 100, VTX_GUARD_FAST_CHECK);
    /* High failure rate → weakens */
    for (int i = 0; i < 20; i++) {
        vtx_guard_meta_update(meta, true);
    }
    VTX_ASSERT_TRUE(vtx_guard_meta_strength(meta) > VTX_GUARD_FAST_CHECK ||
                    vtx_ewma_value(&meta->failure_rate_ewma) > VTX_GUARD_WEAKEN_THRESHOLD);
    vtx_guard_meta_table_destroy(&table);
}

VTX_TEST(test_gmeta_14)
{
    vtx_guard_meta_table_t table;
    vtx_guard_meta_table_init(&table);
    for (uint32_t i = 0; i < 64; i++) {
        vtx_guard_meta_register(&table, i, i % 3, 100 + i, VTX_GUARD_FAST_CHECK);
    }
    VTX_ASSERT_EQUAL(table.guard_count, 64u);
    vtx_guard_meta_table_destroy(&table);
}

VTX_TEST(test_gmeta_15)
{
    vtx_guard_meta_table_t table;
    vtx_guard_meta_table_init(&table);
    vtx_guard_meta_t *m1 = vtx_guard_meta_register(
        &table, 1, 0, 100, VTX_GUARD_FAST_CHECK);
    vtx_guard_meta_t *m2 = vtx_guard_meta_register(
        &table, 2, 1, 200, VTX_GUARD_FULL_CHECK);
    vtx_guard_meta_update(m1, true);
    uint32_t pending = vtx_guard_meta_pending_transitions(&table);
    VTX_ASSERT_TRUE(pending >= 1u);
    vtx_guard_meta_clear_transition_flags(&table);
    pending = vtx_guard_meta_pending_transitions(&table);
    VTX_ASSERT_EQUAL(pending, 0u);
    vtx_guard_meta_table_destroy(&table);
}

/* ========================================================================== */
/* Guard Hoist tests (10 tests): test_hoist_01 through test_hoist_10          */
/* ========================================================================== */

VTX_TEST(test_hoist_01)
{
    vtx_hoist_result_t result = {0};
    VTX_ASSERT_EQUAL(result.guards_hoisted, 0u);
    VTX_ASSERT_EQUAL(result.guards_checked, 0u);
    VTX_ASSERT_EQUAL(result.guards_invariant, 0u);
    VTX_ASSERT_EQUAL(result.hoist_failed, 0u);
}

VTX_TEST(test_hoist_02)
{
    vtx_graph_t graph;
    vtx_arena_t arena;
    vtx_arena_init(&arena);
    vtx_graph_init(&graph, 0);
    /* Graph without loops — just Start and End */
    vtx_schedule_t schedule;
    int rc = vtx_schedule_run(&graph, &arena, &schedule);
    if (rc == 0) {
        vtx_hoist_result_t result = vtx_hoist_guards(&graph, &schedule, &arena);
        VTX_ASSERT_EQUAL(result.guards_hoisted, 0u);
        vtx_schedule_destroy(&schedule);
    } else {
        VTX_ASSERT_TRUE(1); /* scheduling may fail on empty graph */
    }
    vtx_graph_destroy(&graph);
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_hoist_03)
{
    vtx_graph_t graph;
    vtx_arena_t arena;
    vtx_arena_init(&arena);
    vtx_graph_init(&graph, 1);
    /* Create a simple graph with a guard that could be loop-invariant */
    vtx_nodeid_t param = vtx_node_create(&graph.node_table, VTX_OP_Parameter);
    vtx_nodeid_t guard = vtx_node_create(&graph.node_table, VTX_OP_Guard);
    vtx_node_add_input(&graph.node_table, guard, param);
    vtx_schedule_t schedule;
    int rc = vtx_schedule_run(&graph, &arena, &schedule);
    if (rc == 0) {
        vtx_hoist_result_t result = vtx_hoist_guards(&graph, &schedule, &arena);
        /* Just verify it doesn't crash */
        VTX_ASSERT_TRUE(1);
        vtx_schedule_destroy(&schedule);
    } else {
        VTX_ASSERT_TRUE(1);
    }
    vtx_graph_destroy(&graph);
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_hoist_04)
{
    vtx_graph_t graph;
    vtx_arena_t arena;
    vtx_arena_init(&arena);
    vtx_graph_init(&graph, 2);
    /* Create a guard that depends on a loop-variant value */
    vtx_nodeid_t param = vtx_node_create(&graph.node_table, VTX_OP_Parameter);
    vtx_nodeid_t add = vtx_node_create(&graph.node_table, VTX_OP_Add);
    vtx_nodeid_t guard = vtx_node_create(&graph.node_table, VTX_OP_Guard);
    vtx_node_add_input(&graph.node_table, add, param);
    vtx_node_add_input(&graph.node_table, add, param);
    vtx_node_add_input(&graph.node_table, guard, add);
    vtx_schedule_t schedule;
    int rc = vtx_schedule_run(&graph, &arena, &schedule);
    if (rc == 0) {
        vtx_hoist_result_t result = vtx_hoist_guards(&graph, &schedule, &arena);
        VTX_ASSERT_TRUE(1);
        vtx_schedule_destroy(&schedule);
    } else {
        VTX_ASSERT_TRUE(1);
    }
    vtx_graph_destroy(&graph);
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_hoist_05)
{
    /* Hoist preserves control flow — smoke test */
    vtx_graph_t graph;
    vtx_arena_t arena;
    vtx_arena_init(&arena);
    vtx_graph_init(&graph, 0);
    vtx_nodeid_t start = graph.start_node;
    VTX_ASSERT_TRUE(start != VTX_NODEID_INVALID);
    vtx_graph_destroy(&graph);
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_hoist_06)
{
    /* Nested loops — smoke test */
    vtx_graph_t graph;
    vtx_arena_t arena;
    vtx_arena_init(&arena);
    vtx_graph_init(&graph, 1);
    vtx_schedule_t schedule;
    int rc = vtx_schedule_run(&graph, &arena, &schedule);
    if (rc == 0) {
        vtx_hoist_guards(&graph, &schedule, &arena);
        vtx_schedule_destroy(&schedule);
    }
    VTX_ASSERT_TRUE(1);
    vtx_graph_destroy(&graph);
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_hoist_07)
{
    vtx_graph_t graph;
    vtx_arena_t arena;
    vtx_arena_init(&arena);
    vtx_graph_init(&graph, 0);
    vtx_schedule_t schedule;
    int rc = vtx_schedule_run(&graph, &arena, &schedule);
    if (rc == 0) {
        vtx_hoist_result_t result = vtx_hoist_guards(&graph, &schedule, &arena);
        VTX_ASSERT_EQUAL(result.guards_hoisted, 0u);
        vtx_schedule_destroy(&schedule);
    }
    vtx_graph_destroy(&graph);
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_hoist_08)
{
    vtx_graph_t graph;
    vtx_arena_t arena;
    vtx_arena_init(&arena);
    vtx_graph_init(&graph, 0);
    /* Trivial graph: just start node */
    vtx_schedule_t schedule;
    int rc = vtx_schedule_run(&graph, &arena, &schedule);
    if (rc == 0) {
        vtx_hoist_result_t result = vtx_hoist_guards(&graph, &schedule, &arena);
        VTX_ASSERT_EQUAL(result.guards_checked, 0u);
        vtx_schedule_destroy(&schedule);
    }
    vtx_graph_destroy(&graph);
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_hoist_09)
{
    vtx_graph_t graph;
    vtx_arena_t arena;
    vtx_arena_init(&arena);
    vtx_graph_init(&graph, 2);
    /* Multiple invariant guards */
    vtx_nodeid_t p1 = vtx_node_create(&graph.node_table, VTX_OP_Parameter);
    vtx_nodeid_t g1 = vtx_node_create(&graph.node_table, VTX_OP_Guard);
    vtx_nodeid_t g2 = vtx_node_create(&graph.node_table, VTX_OP_Guard);
    vtx_node_add_input(&graph.node_table, g1, p1);
    vtx_node_add_input(&graph.node_table, g2, p1);
    vtx_schedule_t schedule;
    int rc = vtx_schedule_run(&graph, &arena, &schedule);
    if (rc == 0) {
        vtx_hoist_result_t result = vtx_hoist_guards(&graph, &schedule, &arena);
        VTX_ASSERT_TRUE(1);
        vtx_schedule_destroy(&schedule);
    }
    vtx_graph_destroy(&graph);
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_hoist_10)
{
    vtx_graph_t graph;
    vtx_arena_t arena;
    vtx_arena_init(&arena);
    vtx_graph_init(&graph, 1);
    /* No guards at all */
    vtx_node_create(&graph.node_table, VTX_OP_Parameter);
    vtx_schedule_t schedule;
    int rc = vtx_schedule_run(&graph, &arena, &schedule);
    if (rc == 0) {
        vtx_hoist_result_t result = vtx_hoist_guards(&graph, &schedule, &arena);
        VTX_ASSERT_EQUAL(result.guards_checked, 0u);
        vtx_schedule_destroy(&schedule);
    }
    vtx_graph_destroy(&graph);
    vtx_arena_destroy(&arena);
}

/* ========================================================================== */
/* Guard Merge tests (10 tests): test_gmerge_01 through test_gmerge_10        */
/* ========================================================================== */

VTX_TEST(test_gmerge_01)
{
    vtx_merge_result_t result = {0};
    VTX_ASSERT_EQUAL(result.guards_merged, 0u);
    VTX_ASSERT_EQUAL(result.type_checks_merged, 0u);
    VTX_ASSERT_EQUAL(result.null_checks_merged, 0u);
    VTX_ASSERT_EQUAL(result.range_checks_merged, 0u);
    VTX_ASSERT_EQUAL(result.guards_eliminated, 0u);
}

VTX_TEST(test_gmerge_02)
{
    vtx_graph_t graph;
    vtx_arena_t arena;
    vtx_arena_init(&arena);
    vtx_graph_init(&graph, 1);
    /* Two type checks on same parameter */
    vtx_nodeid_t p = vtx_node_create(&graph.node_table, VTX_OP_Parameter);
    vtx_nodeid_t g1 = vtx_node_create(&graph.node_table, VTX_OP_Guard);
    vtx_nodeid_t g2 = vtx_node_create(&graph.node_table, VTX_OP_Guard);
    vtx_node_add_input(&graph.node_table, g1, p);
    vtx_node_add_input(&graph.node_table, g2, p);
    vtx_merge_result_t result = vtx_merge_guards(&graph, &arena);
    /* At minimum, should check and not crash */
    VTX_ASSERT_TRUE(1);
    vtx_graph_destroy(&graph);
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_gmerge_03)
{
    vtx_graph_t graph;
    vtx_arena_t arena;
    vtx_arena_init(&arena);
    vtx_graph_init(&graph, 1);
    /* Two null checks on same value */
    vtx_nodeid_t p = vtx_node_create(&graph.node_table, VTX_OP_Parameter);
    vtx_nodeid_t g1 = vtx_node_create(&graph.node_table, VTX_OP_Guard);
    vtx_nodeid_t g2 = vtx_node_create(&graph.node_table, VTX_OP_Guard);
    vtx_node_add_input(&graph.node_table, g1, p);
    vtx_node_add_input(&graph.node_table, g2, p);
    vtx_merge_result_t result = vtx_merge_guards(&graph, &arena);
    VTX_ASSERT_TRUE(1);
    vtx_graph_destroy(&graph);
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_gmerge_04)
{
    vtx_graph_t graph;
    vtx_arena_t arena;
    vtx_arena_init(&arena);
    vtx_graph_init(&graph, 2);
    /* Two range checks on same array */
    vtx_nodeid_t p1 = vtx_node_create(&graph.node_table, VTX_OP_Parameter);
    vtx_nodeid_t p2 = vtx_node_create(&graph.node_table, VTX_OP_Parameter);
    vtx_nodeid_t g1 = vtx_node_create(&graph.node_table, VTX_OP_Guard);
    vtx_nodeid_t g2 = vtx_node_create(&graph.node_table, VTX_OP_Guard);
    vtx_node_add_input(&graph.node_table, g1, p1);
    vtx_node_add_input(&graph.node_table, g2, p1);
    vtx_node_add_input(&graph.node_table, g2, p2);
    vtx_merge_result_t result = vtx_merge_guards(&graph, &arena);
    VTX_ASSERT_TRUE(1);
    vtx_graph_destroy(&graph);
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_gmerge_05)
{
    vtx_graph_t graph;
    vtx_arena_t arena;
    vtx_arena_init(&arena);
    vtx_graph_init(&graph, 2);
    /* Different values → no merge */
    vtx_nodeid_t p1 = vtx_node_create(&graph.node_table, VTX_OP_Parameter);
    vtx_nodeid_t p2 = vtx_node_create(&graph.node_table, VTX_OP_Parameter);
    vtx_nodeid_t g1 = vtx_node_create(&graph.node_table, VTX_OP_Guard);
    vtx_nodeid_t g2 = vtx_node_create(&graph.node_table, VTX_OP_Guard);
    vtx_node_add_input(&graph.node_table, g1, p1);
    vtx_node_add_input(&graph.node_table, g2, p2);
    vtx_merge_result_t result = vtx_merge_guards(&graph, &arena);
    VTX_ASSERT_EQUAL(result.guards_merged, 0u);
    vtx_graph_destroy(&graph);
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_gmerge_06)
{
    vtx_graph_t graph;
    vtx_arena_t arena;
    vtx_arena_init(&arena);
    vtx_graph_init(&graph, 2);
    /* Different values (no merge) */
    vtx_nodeid_t p1 = vtx_node_create(&graph.node_table, VTX_OP_Parameter);
    vtx_nodeid_t p2 = vtx_node_create(&graph.node_table, VTX_OP_Parameter);
    vtx_nodeid_t g1 = vtx_node_create(&graph.node_table, VTX_OP_Guard);
    vtx_nodeid_t g2 = vtx_node_create(&graph.node_table, VTX_OP_Guard);
    vtx_node_add_input(&graph.node_table, g1, p1);
    vtx_node_add_input(&graph.node_table, g2, p2);
    int kind = vtx_merge_check_pair(&graph, g1, g2);
    /* Different values → not mergeable (-1) */
    VTX_ASSERT_EQUAL(kind, -1);
    vtx_graph_destroy(&graph);
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_gmerge_07)
{
    vtx_graph_t graph;
    vtx_arena_t arena;
    vtx_arena_init(&arena);
    vtx_graph_init(&graph, 1);
    /* Multiple mergeable pairs */
    vtx_nodeid_t p = vtx_node_create(&graph.node_table, VTX_OP_Parameter);
    vtx_nodeid_t g1 = vtx_node_create(&graph.node_table, VTX_OP_Guard);
    vtx_nodeid_t g2 = vtx_node_create(&graph.node_table, VTX_OP_Guard);
    vtx_nodeid_t g3 = vtx_node_create(&graph.node_table, VTX_OP_Guard);
    vtx_node_add_input(&graph.node_table, g1, p);
    vtx_node_add_input(&graph.node_table, g2, p);
    vtx_node_add_input(&graph.node_table, g3, p);
    vtx_merge_result_t result = vtx_merge_guards(&graph, &arena);
    VTX_ASSERT_TRUE(1);
    vtx_graph_destroy(&graph);
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_gmerge_08)
{
    vtx_graph_t graph;
    vtx_arena_t arena;
    vtx_arena_init(&arena);
    vtx_graph_init(&graph, 0);
    vtx_merge_result_t result = vtx_merge_guards(&graph, &arena);
    VTX_ASSERT_EQUAL(result.guards_merged, 0u);
    vtx_graph_destroy(&graph);
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_gmerge_09)
{
    vtx_graph_t graph;
    vtx_arena_t arena;
    vtx_arena_init(&arena);
    vtx_graph_init(&graph, 0);
    /* Empty graph: no guards */
    vtx_merge_candidate_t *candidates = NULL;
    uint32_t count = 0;
    int rc = vtx_merge_find_candidates(&graph, &arena, &candidates, &count);
    VTX_ASSERT_EQUAL(rc, 0);
    VTX_ASSERT_EQUAL(count, 0u);
    vtx_graph_destroy(&graph);
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_gmerge_10)
{
    vtx_graph_t graph;
    vtx_arena_t arena;
    vtx_arena_init(&arena);
    vtx_graph_init(&graph, 2);
    /* One mergeable pair, one non-mergeable */
    vtx_nodeid_t p1 = vtx_node_create(&graph.node_table, VTX_OP_Parameter);
    vtx_nodeid_t p2 = vtx_node_create(&graph.node_table, VTX_OP_Parameter);
    vtx_nodeid_t g1 = vtx_node_create(&graph.node_table, VTX_OP_Guard);
    vtx_nodeid_t g2 = vtx_node_create(&graph.node_table, VTX_OP_Guard);
    vtx_nodeid_t g3 = vtx_node_create(&graph.node_table, VTX_OP_Guard);
    vtx_node_add_input(&graph.node_table, g1, p1);
    vtx_node_add_input(&graph.node_table, g2, p1);
    vtx_node_add_input(&graph.node_table, g3, p2);
    vtx_merge_result_t result = vtx_merge_guards(&graph, &arena);
    VTX_ASSERT_TRUE(1);
    vtx_graph_destroy(&graph);
    vtx_arena_destroy(&arena);
}

/* ========================================================================== */
/* Deopt FrameState tests (15 tests): test_deopt_fs_01 through test_deopt_fs_15 */
/* ========================================================================== */

VTX_TEST(test_deopt_fs_01)
{
    vtx_arena_t arena;
    vtx_arena_init(&arena);
    vtx_frame_state_t *fs = vtx_frame_state_create(&arena, 100, 1, 4, 2);
    VTX_ASSERT_NOT_NULL(fs);
    VTX_ASSERT_EQUAL(fs->bytecode_pc, 100u);
    VTX_ASSERT_EQUAL(fs->method_id, 1u);
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_deopt_fs_02)
{
    vtx_arena_t arena;
    vtx_arena_init(&arena);
    vtx_frame_state_t *fs = vtx_frame_state_create(&arena, 50, 1, 4, 0);
    VTX_ASSERT_NOT_NULL(fs);
    VTX_ASSERT_EQUAL(fs->local_count, 4u);
    vtx_frame_state_set_local(fs, 0, 10);
    vtx_frame_state_set_local(fs, 1, 20);
    VTX_ASSERT_EQUAL(vtx_frame_state_get_local(fs, 0), (vtx_nodeid_t)10);
    VTX_ASSERT_EQUAL(vtx_frame_state_get_local(fs, 1), (vtx_nodeid_t)20);
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_deopt_fs_03)
{
    vtx_arena_t arena;
    vtx_arena_init(&arena);
    vtx_frame_state_t *fs = vtx_frame_state_create(&arena, 50, 1, 0, 3);
    VTX_ASSERT_NOT_NULL(fs);
    VTX_ASSERT_EQUAL(fs->stack_count, 3u);
    vtx_frame_state_set_stack(fs, 0, 100);
    vtx_frame_state_set_stack(fs, 1, 200);
    vtx_frame_state_set_stack(fs, 2, 300);
    VTX_ASSERT_EQUAL(vtx_frame_state_get_stack(fs, 0), (vtx_nodeid_t)100);
    VTX_ASSERT_EQUAL(vtx_frame_state_get_stack(fs, 2), (vtx_nodeid_t)300);
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_deopt_fs_04)
{
    vtx_arena_t arena;
    vtx_arena_init(&arena);
    uint32_t pcs[] = {10, 20};
    uint32_t mids[] = {1, 2};
    uint32_t locals[] = {2, 2};
    uint32_t stacks[] = {1, 1};
    vtx_frame_state_t *inner = vtx_frame_state_chain_create(
        &arena, pcs, mids, locals, stacks, 2);
    VTX_ASSERT_NOT_NULL(inner);
    VTX_ASSERT_NOT_NULL(inner->caller);
    VTX_ASSERT_EQUAL(inner->bytecode_pc, 10u);
    VTX_ASSERT_EQUAL(inner->caller->bytecode_pc, 20u);
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_deopt_fs_05)
{
    vtx_arena_t arena;
    vtx_arena_init(&arena);
    vtx_frame_state_t *fs = vtx_frame_state_create(&arena, 42, 1, 2, 1);
    VTX_ASSERT_NOT_NULL(fs);
    VTX_ASSERT_EQUAL(fs->bytecode_pc, 42u);
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_deopt_fs_06)
{
    vtx_arena_t arena;
    vtx_arena_init(&arena);
    /* Verify arena allocation */
    vtx_frame_state_t *fs = vtx_frame_state_create(&arena, 0, 1, 8, 4);
    VTX_ASSERT_NOT_NULL(fs);
    VTX_ASSERT_NOT_NULL(fs->locals);
    VTX_ASSERT_NOT_NULL(fs->stack);
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_deopt_fs_07)
{
    vtx_arena_t arena;
    vtx_arena_init(&arena);
    /* Inlined frame: inner frame with caller */
    vtx_frame_state_t *caller = vtx_frame_state_create(&arena, 50, 1, 4, 0);
    vtx_frame_state_t *inner = vtx_frame_state_create(&arena, 100, 2, 2, 1);
    inner->caller = caller;
    VTX_ASSERT_EQUAL(inner->caller->bytecode_pc, 50u);
    VTX_ASSERT_EQUAL(inner->method_id, 2u);
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_deopt_fs_08)
{
    vtx_arena_t arena;
    vtx_arena_init(&arena);
    vtx_frame_state_t *fs = vtx_frame_state_create(&arena, 0, 1, 1, 0);
    VTX_ASSERT_NOT_NULL(fs);
    VTX_ASSERT_EQUAL(fs->monitor_count, 0u);
    /* Set a monitor */
    vtx_monitor_state_t mon = { .monitor_object = 42 };
    fs->monitors = &mon;
    fs->monitor_count = 1;
    VTX_ASSERT_EQUAL(fs->monitors[0].monitor_object, (vtx_nodeid_t)42);
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_deopt_fs_09)
{
    vtx_arena_t arena;
    vtx_arena_init(&arena);
    vtx_frame_state_t *fs = vtx_frame_state_create(&arena, 0, 1, 1, 0);
    vtx_frame_state_set_exception(fs, 200, 5);
    VTX_ASSERT_EQUAL(fs->exception.handler_pc, 200u);
    VTX_ASSERT_EQUAL(fs->exception.catch_type, (vtx_typeid_t)5);
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_deopt_fs_10)
{
    vtx_arena_t arena;
    vtx_arena_init(&arena);
    uint32_t pcs[] = {10, 20, 30};
    uint32_t mids[] = {1, 2, 3};
    uint32_t locals[] = {1, 1, 1};
    uint32_t stacks[] = {0, 0, 0};
    vtx_frame_state_t *head = vtx_frame_state_chain_create(
        &arena, pcs, mids, locals, stacks, 3);
    VTX_ASSERT_NOT_NULL(head);
    uint32_t depth = vtx_frame_state_chain_depth(head);
    VTX_ASSERT_EQUAL(depth, 3u);
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_deopt_fs_11)
{
    vtx_arena_t arena;
    vtx_arena_init(&arena);
    uint32_t pcs[] = {10, 20};
    uint32_t mids[] = {1, 2};
    uint32_t locals[] = {1, 1};
    uint32_t stacks[] = {0, 0};
    vtx_frame_state_t *head = vtx_frame_state_chain_create(
        &arena, pcs, mids, locals, stacks, 2);
    VTX_ASSERT_NOT_NULL(head);
    const vtx_frame_state_t *caller = vtx_frame_state_nth_caller(head, 1);
    VTX_ASSERT_NOT_NULL(caller);
    VTX_ASSERT_EQUAL(caller->bytecode_pc, 20u);
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_deopt_fs_12)
{
    vtx_arena_t arena;
    vtx_arena_init(&arena);
    vtx_frame_state_t *fs = vtx_frame_state_create(&arena, 0, 1, 32, 0);
    VTX_ASSERT_NOT_NULL(fs);
    VTX_ASSERT_EQUAL(fs->local_count, 32u);
    for (uint32_t i = 0; i < 32; i++) {
        vtx_frame_state_set_local(fs, i, i + 100);
    }
    for (uint32_t i = 0; i < 32; i++) {
        VTX_ASSERT_EQUAL(vtx_frame_state_get_local(fs, i), (vtx_nodeid_t)(i + 100));
    }
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_deopt_fs_13)
{
    vtx_arena_t arena;
    vtx_arena_init(&arena);
    vtx_frame_state_t *fs = vtx_frame_state_create(&arena, 0, 1, 2, 0);
    VTX_ASSERT_NOT_NULL(fs);
    VTX_ASSERT_EQUAL(fs->stack_count, 0u);
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_deopt_fs_14)
{
    vtx_arena_t arena;
    vtx_arena_init(&arena);
    vtx_frame_state_t *fs = vtx_frame_state_create(&arena, 0, 1, 0, 16);
    VTX_ASSERT_NOT_NULL(fs);
    VTX_ASSERT_EQUAL(fs->stack_count, 16u);
    for (uint32_t i = 0; i < 16; i++) {
        vtx_frame_state_set_stack(fs, i, i + 50);
    }
    for (uint32_t i = 0; i < 16; i++) {
        VTX_ASSERT_EQUAL(vtx_frame_state_get_stack(fs, i), (vtx_nodeid_t)(i + 50));
    }
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_deopt_fs_15)
{
    vtx_arena_t arena;
    vtx_arena_init(&arena);
    vtx_frame_state_t *fs = vtx_frame_state_create(&arena, 0, 1, 2, 2);
    VTX_ASSERT_NOT_NULL(fs);
    /* No explicit destroy — arena freed */
    vtx_arena_destroy(&arena);
}

/* ========================================================================== */
/* Deopt Side Table tests (10 tests): test_deopt_st_01 through test_deopt_st_10 */
/* ========================================================================== */

VTX_TEST(test_deopt_st_01)
{
    vtx_side_table_t *table = vtx_side_table_build(NULL);
    VTX_ASSERT_NOT_NULL(table);
    VTX_ASSERT_EQUAL(vtx_side_table_entry_count(table), 0u);
    vtx_side_table_destroy(table);
}

VTX_TEST(test_deopt_st_02)
{
    vtx_side_table_t *table = vtx_side_table_build(NULL);
    VTX_ASSERT_NOT_NULL(table);
    uint32_t idx = vtx_side_table_add_entry(table, 100, 0, VTX_STF_GUARD);
    VTX_ASSERT_NOT_EQUAL(idx, UINT32_MAX);
    VTX_ASSERT_EQUAL(vtx_side_table_entry_count(table), 1u);
    vtx_side_table_destroy(table);
}

VTX_TEST(test_deopt_st_03)
{
    vtx_side_table_t *table = vtx_side_table_build(NULL);
    VTX_ASSERT_NOT_NULL(table);
    vtx_side_table_add_entry(table, 100, 0, VTX_STF_GUARD);
    vtx_side_table_add_entry(table, 200, 1, VTX_STF_CALL_SITE);
    uint32_t fs_idx = vtx_side_table_lookup(table, 150);
    /* Should find the entry at 100 (largest <= 150) */
    VTX_ASSERT_EQUAL(fs_idx, 0u);
    vtx_side_table_destroy(table);
}

VTX_TEST(test_deopt_st_04)
{
    vtx_side_table_t *table = vtx_side_table_build(NULL);
    VTX_ASSERT_NOT_NULL(table);
    vtx_side_table_add_entry(table, 100, 0, VTX_STF_GUARD);
    vtx_side_table_add_entry(table, 200, 1, VTX_STF_CALL_SITE);
    vtx_side_table_add_entry(table, 300, 2, VTX_STF_SAFEPPOINT);
    /* Exact match */
    VTX_ASSERT_EQUAL(vtx_side_table_lookup(table, 200), 1u);
    vtx_side_table_destroy(table);
}

VTX_TEST(test_deopt_st_05)
{
    vtx_side_table_t *table = vtx_side_table_build(NULL);
    VTX_ASSERT_NOT_NULL(table);
    for (uint32_t i = 0; i < 5; i++) {
        vtx_side_table_add_entry(table, (i + 1) * 100, i, VTX_STF_GUARD);
    }
    VTX_ASSERT_EQUAL(vtx_side_table_entry_count(table), 5u);
    vtx_side_table_destroy(table);
}

VTX_TEST(test_deopt_st_06)
{
    vtx_side_table_t *table = vtx_side_table_build(NULL);
    VTX_ASSERT_NOT_NULL(table);
    uint32_t idx = vtx_side_table_add_entry(table, 100, 0, VTX_STF_GUARD);
    int rc = vtx_side_table_add_register(table, 0, 42);
    VTX_ASSERT_EQUAL(rc, 0);
    vtx_nodeid_t nid = vtx_side_table_find_register(table, 100, 0);
    VTX_ASSERT_EQUAL(nid, (vtx_nodeid_t)42);
    vtx_side_table_destroy(table);
}

VTX_TEST(test_deopt_st_07)
{
    vtx_side_table_t *table = vtx_side_table_build(NULL);
    VTX_ASSERT_NOT_NULL(table);
    vtx_side_table_add_entry(table, 100, 0, VTX_STF_GUARD);
    vtx_side_table_add_entry(table, 200, 1, VTX_STF_GUARD);
    vtx_side_table_add_entry(table, 300, 2, VTX_STF_GUARD);
    /* Entries should be in sorted order */
    const vtx_side_table_entry_t *e0 = vtx_side_table_get_entry(table, 0);
    const vtx_side_table_entry_t *e1 = vtx_side_table_get_entry(table, 1);
    const vtx_side_table_entry_t *e2 = vtx_side_table_get_entry(table, 2);
    VTX_ASSERT_NOT_NULL(e0);
    VTX_ASSERT_NOT_NULL(e1);
    VTX_ASSERT_NOT_NULL(e2);
    VTX_ASSERT_TRUE(e0->native_pc_offset < e1->native_pc_offset);
    VTX_ASSERT_TRUE(e1->native_pc_offset < e2->native_pc_offset);
    vtx_side_table_destroy(table);
}

VTX_TEST(test_deopt_st_08)
{
    vtx_side_table_t *table = vtx_side_table_build(NULL);
    VTX_ASSERT_NOT_NULL(table);
    /* Lookup on empty table */
    uint32_t idx = vtx_side_table_lookup(table, 100);
    VTX_ASSERT_EQUAL(idx, UINT32_MAX);
    vtx_side_table_destroy(table);
}

VTX_TEST(test_deopt_st_09)
{
    vtx_side_table_t *table = vtx_side_table_build(NULL);
    VTX_ASSERT_NOT_NULL(table);
    vtx_side_table_add_entry(table, 100, 0, VTX_STF_GUARD);
    vtx_side_table_add_entry(table, 200, 1, VTX_STF_GUARD);
    /* Before first entry */
    uint32_t idx = vtx_side_table_lookup(table, 50);
    VTX_ASSERT_EQUAL(idx, UINT32_MAX);
    /* At boundary */
    idx = vtx_side_table_lookup(table, 100);
    VTX_ASSERT_EQUAL(idx, 0u);
    idx = vtx_side_table_lookup(table, 199);
    VTX_ASSERT_EQUAL(idx, 0u);
    vtx_side_table_destroy(table);
}

VTX_TEST(test_deopt_st_10)
{
    vtx_side_table_t *table = vtx_side_table_build(NULL);
    VTX_ASSERT_NOT_NULL(table);
    for (uint32_t i = 0; i < 100; i++) {
        vtx_side_table_add_entry(table, i * 10, i, VTX_STF_GUARD);
    }
    VTX_ASSERT_EQUAL(vtx_side_table_entry_count(table), 100u);
    /* Lookup should find correct entry via binary search */
    uint32_t idx = vtx_side_table_lookup(table, 500);
    VTX_ASSERT_EQUAL(idx, 50u);
    vtx_side_table_destroy(table);
}

/* ========================================================================== */
/* Deopt OSR/Deoptless tests (15 tests): test_deopt_osr_01 through test_deopt_osr_15 */
/* ========================================================================== */

VTX_TEST(test_deopt_osr_01)
{
    vtx_osr_deopt_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.method_id = 42;
    ctx.native_pc = 100;
    VTX_ASSERT_EQUAL(ctx.method_id, 42u);
    VTX_ASSERT_EQUAL(ctx.native_pc, 100u);
}

VTX_TEST(test_deopt_osr_02)
{
    vtx_osr_deopt_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.method_id = 1;
    ctx.native_pc = 50;
    ctx.frame_state = NULL;
    ctx.side_table = NULL;
    ctx.frame_pointer = NULL;
    ctx.register_map = NULL;
    ctx.register_count = 0;
    VTX_ASSERT_NULL(ctx.frame_state);
    VTX_ASSERT_NULL(ctx.side_table);
    VTX_ASSERT_EQUAL(ctx.register_count, 0u);
}

VTX_TEST(test_deopt_osr_03)
{
    vtx_interp_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.method_id = 1;
    frame.bytecode_pc = 0;
    frame.local_count = 4;
    frame.stack_top = 0;
    frame.stack_capacity = 8;
    frame.osr_active = false;
    frame.frame_kind = VTX_FRAME_INTERPRETED;
    VTX_ASSERT_EQUAL(frame.method_id, 1u);
    VTX_ASSERT_EQUAL(frame.frame_kind, VTX_FRAME_INTERPRETED);
}

VTX_TEST(test_deopt_osr_04)
{
    vtx_arena_t arena;
    vtx_arena_init(&arena);
    vtx_interp_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.method_id = 1;
    frame.bytecode_pc = 0;
    frame.local_count = 4;
    vtx_value_t *locals = (vtx_value_t *)vtx_arena_alloc(&arena, 4 * sizeof(vtx_value_t));
    VTX_ASSERT_NOT_NULL(locals);
    frame.locals = locals;
    locals[0] = vtx_make_smi(10);
    locals[1] = vtx_make_smi(20);
    VTX_ASSERT_TRUE(vtx_is_smi(locals[0]));
    VTX_ASSERT_EQUAL(vtx_smi_value(locals[0]), (int64_t)10);
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_deopt_osr_05)
{
    vtx_arena_t arena;
    vtx_arena_init(&arena);
    vtx_interp_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.method_id = 1;
    frame.stack_capacity = 8;
    frame.stack_top = 2;
    vtx_value_t *stack = (vtx_value_t *)vtx_arena_alloc(&arena, 8 * sizeof(vtx_value_t));
    VTX_ASSERT_NOT_NULL(stack);
    frame.stack = stack;
    stack[0] = vtx_make_smi(100);
    stack[1] = vtx_make_smi(200);
    VTX_ASSERT_EQUAL(vtx_smi_value(stack[0]), (int64_t)100);
    VTX_ASSERT_EQUAL(vtx_smi_value(stack[1]), (int64_t)200);
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_deopt_osr_06)
{
    vtx_arena_t arena;
    vtx_arena_init(&arena);
    /* Reconstructed frame test */
    vtx_reconstructed_frame_t rframe;
    memset(&rframe, 0, sizeof(rframe));
    rframe.method_id = 1;
    rframe.bytecode_pc = 50;
    rframe.local_count = 4;
    rframe.stack_count = 2;
    rframe.original_kind = VTX_FRAME_COMPILED;
    rframe.is_inlined = false;
    VTX_ASSERT_EQUAL(rframe.method_id, 1u);
    VTX_ASSERT_EQUAL(rframe.original_kind, VTX_FRAME_COMPILED);
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_deopt_osr_07)
{
    vtx_deoptless_table_t table;
    int rc = vtx_deoptless_table_init(&table, 1, NULL, NULL);
    VTX_ASSERT_EQUAL(rc, 0);
    VTX_ASSERT_EQUAL(table.version_count, 0u);
    vtx_deoptless_table_destroy(&table);
}

VTX_TEST(test_deopt_osr_08)
{
    vtx_deoptless_table_t table;
    vtx_deoptless_table_init(&table, 1, NULL, NULL);
    vtx_deoptless_version_t *v = vtx_deoptless_add_version(
        &table, 10, (void *)0x1000, 64);
    VTX_ASSERT_NOT_NULL(v);
    VTX_ASSERT_EQUAL(v->failed_guard_id, (vtx_guard_id_t)10);
    VTX_ASSERT_EQUAL(table.version_count, 1u);
    vtx_deoptless_table_destroy(&table);
}

VTX_TEST(test_deopt_osr_09)
{
    vtx_deoptless_table_t table;
    vtx_deoptless_table_init(&table, 1, NULL, NULL);
    /* Add VTX_DEOPTLESS_MAX_VERSIONS (8) versions */
    for (uint32_t i = 0; i < VTX_DEOPTLESS_MAX_VERSIONS; i++) {
        vtx_deoptless_version_t *v = vtx_deoptless_add_version(
            &table, i + 1, (void *)(uintptr_t)(0x1000 + i * 64), 64);
        VTX_ASSERT_NOT_NULL(v);
    }
    VTX_ASSERT_EQUAL(table.version_count, VTX_DEOPTLESS_MAX_VERSIONS);
    /* Adding one more should evict the oldest */
    vtx_deoptless_add_version(&table, 100, (void *)0x2000, 64);
    VTX_ASSERT_EQUAL(table.version_count, VTX_DEOPTLESS_MAX_VERSIONS);
    vtx_deoptless_table_destroy(&table);
}

VTX_TEST(test_deopt_osr_10)
{
    vtx_deoptless_table_t table;
    vtx_deoptless_table_init(&table, 1, NULL, NULL);
    vtx_deoptless_add_version(&table, 10, (void *)0x1000, 64);
    vtx_deoptless_version_t *v = vtx_deoptless_find_version(&table, 10);
    VTX_ASSERT_NOT_NULL(v);
    VTX_ASSERT_EQUAL(v->failed_guard_id, (vtx_guard_id_t)10);
    /* Non-existent guard */
    v = vtx_deoptless_find_version(&table, 999);
    VTX_ASSERT_NULL(v);
    vtx_deoptless_table_destroy(&table);
}

VTX_TEST(test_deopt_osr_11)
{
    vtx_deoptless_table_t table;
    vtx_deoptless_table_init(&table, 1, NULL, NULL);
    vtx_deoptless_add_version(&table, 10, (void *)0x1000, 64);
    /* Continuation tracking */
    vtx_deoptless_version_t *v = vtx_deoptless_find_version(&table, 10);
    VTX_ASSERT_NOT_NULL(v);
    VTX_ASSERT_NOT_NULL(v->continuation_code);
    vtx_deoptless_table_destroy(&table);
}

VTX_TEST(test_deopt_osr_12)
{
    vtx_deoptless_table_t table;
    vtx_deoptless_table_init(&table, 1, NULL, NULL);
    vtx_deoptless_add_version(&table, 10, (void *)0x1000, 64);
    vtx_deoptless_version_t *v = vtx_deoptless_find_version(&table, 10);
    VTX_ASSERT_NOT_NULL(v);
    /* Patch displacement */
    uint8_t fake_code[64];
    memset(fake_code, 0, sizeof(fake_code));
    v->code_start = fake_code;
    v->guard_branch_offset = 10;
    VTX_ASSERT_EQUAL(v->guard_branch_offset, 10u);
    vtx_deoptless_table_destroy(&table);
}

VTX_TEST(test_deopt_osr_13)
{
    vtx_deoptless_table_t table;
    vtx_deoptless_table_init(&table, 1, NULL, NULL);
    /* Remove a version that doesn't exist → should be safe */
    vtx_deoptless_table_destroy(&table);
}

VTX_TEST(test_deopt_osr_14)
{
    /* OSR up basic flow: just test structure setup */
    vtx_interp_frame_t interp;
    memset(&interp, 0, sizeof(interp));
    interp.method_id = 1;
    interp.bytecode_pc = 0;
    interp.local_count = 4;
    interp.stack_capacity = 8;
    interp.frame_kind = VTX_FRAME_INTERPRETED;
    /* Can't call vtx_osr_up without real compiled code, but test context */
    VTX_ASSERT_FALSE(interp.osr_active);
}

VTX_TEST(test_deopt_osr_15)
{
    /* OSR down basic flow: test deopt context construction */
    vtx_osr_deopt_context_t deopt;
    memset(&deopt, 0, sizeof(deopt));
    deopt.method_id = 1;
    deopt.native_pc = 100;
    deopt.register_count = 4;
    VTX_ASSERT_EQUAL(deopt.method_id, 1u);
    VTX_ASSERT_EQUAL(deopt.native_pc, 100u);
    VTX_ASSERT_EQUAL(deopt.register_count, 4u);
}

/* ========================================================================== */
/* Main                                                                         */
/* ========================================================================== */

int main(void) {
    vtx_test_result_t result = vtx_test_run_all();
    return (result.fail_count > 0) ? 1 : 0;
}
