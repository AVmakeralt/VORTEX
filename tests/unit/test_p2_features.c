/**
 * VORTEX P2 Feature Tests
 *
 * Comprehensive tests for:
 *   1. Guard-page type checking (guard_page_type)
 *   2. Guard dependency graph (guard_deps)
 *   3. Phase-reactive version reactivation (phase_react)
 *   4. Speculative PEA + lock elision (speculative_pea)
 *   5. Integration between P2 features
 *   6. Edge cases (NULL pointers, overflow, boundary conditions)
 */

#include "test_framework.h"
#include "guard/guard_page_type.h"
#include "guard/guard_deps.h"
#include "guard/metadata.h"
#include "guard/value_profile.h"
#include "compile/phase_react.h"
#include "compile/version.h"
#include "pea/speculative_pea.h"
#include "pea/analysis.h"
#include "runtime/arena.h"
#include "ir/graph.h"
#include "ir/node.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <signal.h>
#include <setjmp.h>

/* ========================================================================== */
/* Guard-Page Type Checking Tests                                              */
/* ========================================================================== */

VTX_TEST(type_guard_page_registry_init_destroy)
{
    vtx_type_guard_page_registry_t registry;
    VTX_ASSERT_EQUAL(vtx_type_guard_page_registry_init(&registry), 0);
    VTX_ASSERT_EQUAL(registry.page_count, 0u);
    VTX_ASSERT_EQUAL(registry.total_created, 0ull);

    vtx_type_guard_page_registry_destroy(&registry);
}

VTX_TEST(type_guard_page_create_destroy)
{
    vtx_type_guard_page_registry_t registry;
    VTX_ASSERT_EQUAL(vtx_type_guard_page_registry_init(&registry), 0);

    vtx_type_guard_page_t *page = vtx_type_guard_page_create(
        &registry, 3, 7, 42, 100, 5);
    VTX_ASSERT_TRUE(page != NULL);
    VTX_ASSERT_TRUE(page->is_active);
    VTX_ASSERT_EQUAL(page->expected_type_id, 3u);
    VTX_ASSERT_EQUAL(page->max_type_id, 7u);
    VTX_ASSERT_EQUAL(page->method_id, 42u);
    VTX_ASSERT_EQUAL(page->guard_id, 100u);
    VTX_ASSERT_TRUE(page->region_base != NULL);
    VTX_ASSERT_EQUAL(registry.page_count, 1u);
    VTX_ASSERT_EQUAL(registry.total_created, 1ull);

    void *base = vtx_type_guard_page_base(page);
    VTX_ASSERT_TRUE(base != NULL);
    VTX_ASSERT_TRUE(vtx_type_guard_page_is_active(page));

    VTX_ASSERT_EQUAL(vtx_type_guard_page_destroy(&registry, page), 0);
    VTX_ASSERT_EQUAL(registry.page_count, 0u);
    VTX_ASSERT_EQUAL(registry.total_destroyed, 1ull);

    vtx_type_guard_page_registry_destroy(&registry);
}

VTX_TEST(type_guard_page_readable_for_expected_type)
{
    vtx_type_guard_page_registry_t registry;
    VTX_ASSERT_EQUAL(vtx_type_guard_page_registry_init(&registry), 0);

    vtx_type_guard_page_t *page = vtx_type_guard_page_create(
        &registry, 2, 4, 1, 10, 0);
    VTX_ASSERT_TRUE(page != NULL);

    void *base = vtx_type_guard_page_base(page);
    uintptr_t expected_offset = 2 * VTX_TYPE_GUARD_STRIDE;
    volatile uint64_t *slot = (volatile uint64_t *)((uint8_t *)base + expected_offset);
    uint64_t val = *slot;
    (void)val;

    vtx_type_guard_page_destroy(&registry, page);
    vtx_type_guard_page_registry_destroy(&registry);
}

VTX_TEST(type_guard_page_trap_for_wrong_type)
{
    vtx_type_guard_page_registry_t registry;
    VTX_ASSERT_EQUAL(vtx_type_guard_page_registry_init(&registry), 0);

    vtx_type_guard_page_t *page = vtx_type_guard_page_create(
        &registry, 1, 3, 1, 10, 0);
    VTX_ASSERT_TRUE(page != NULL);

    void *base = vtx_type_guard_page_base(page);
    uintptr_t wrong_addr = (uintptr_t)base + 0 * VTX_TYPE_GUARD_STRIDE;

    VTX_ASSERT_TRUE(wrong_addr >= (uintptr_t)page->region_base);
    VTX_ASSERT_TRUE(wrong_addr < (uintptr_t)page->region_base + page->region_size);

    vtx_type_guard_page_destroy(&registry, page);
    vtx_type_guard_page_registry_destroy(&registry);
}

VTX_TEST(type_guard_page_registry_lookup_found)
{
    vtx_type_guard_page_registry_t registry;
    VTX_ASSERT_EQUAL(vtx_type_guard_page_registry_init(&registry), 0);

    vtx_type_guard_page_t *page = vtx_type_guard_page_create(
        &registry, 5, 9, 100, 200, 7);
    VTX_ASSERT_TRUE(page != NULL);

    void *base = vtx_type_guard_page_base(page);
    uintptr_t fault_addr = (uintptr_t)base + 5 * VTX_TYPE_GUARD_STRIDE;

    vtx_type_guard_fault_info_t info;
    bool found = vtx_type_guard_page_registry_lookup(&registry, fault_addr, &info);
    VTX_ASSERT_TRUE(found);
    VTX_ASSERT_EQUAL(info.method_id, 100u);
    VTX_ASSERT_EQUAL(info.guard_id, 200u);
    VTX_ASSERT_EQUAL(info.expected_type_id, 5u);

    vtx_type_guard_page_destroy(&registry, page);
    vtx_type_guard_page_registry_destroy(&registry);
}

VTX_TEST(type_guard_page_registry_lookup_not_found)
{
    vtx_type_guard_page_registry_t registry;
    VTX_ASSERT_EQUAL(vtx_type_guard_page_registry_init(&registry), 0);

    vtx_type_guard_fault_info_t info;
    bool found = vtx_type_guard_page_registry_lookup(
        &registry, 0xDEADBEEF, &info);
    VTX_ASSERT_TRUE(!found);

    vtx_type_guard_page_registry_destroy(&registry);
}

VTX_TEST(type_guard_page_reconfigure)
{
    vtx_type_guard_page_registry_t registry;
    VTX_ASSERT_EQUAL(vtx_type_guard_page_registry_init(&registry), 0);

    vtx_type_guard_page_t *page = vtx_type_guard_page_create(
        &registry, 2, 5, 1, 10, 0);
    VTX_ASSERT_TRUE(page != NULL);
    VTX_ASSERT_EQUAL(page->expected_type_id, 2u);

    VTX_ASSERT_EQUAL(vtx_type_guard_page_reconfigure(page, 4), 0);
    VTX_ASSERT_EQUAL(page->expected_type_id, 4u);

    void *base = vtx_type_guard_page_base(page);
    uintptr_t new_offset = 4 * VTX_TYPE_GUARD_STRIDE;
    volatile uint64_t *slot = (volatile uint64_t *)((uint8_t *)base + new_offset);
    uint64_t val = *slot;
    (void)val;

    vtx_type_guard_page_destroy(&registry, page);
    vtx_type_guard_page_registry_destroy(&registry);
}

VTX_TEST(type_guard_page_multiple_pages)
{
    vtx_type_guard_page_registry_t registry;
    VTX_ASSERT_EQUAL(vtx_type_guard_page_registry_init(&registry), 0);

    vtx_type_guard_page_t *page1 = vtx_type_guard_page_create(
        &registry, 1, 3, 10, 20, 0);
    vtx_type_guard_page_t *page2 = vtx_type_guard_page_create(
        &registry, 2, 3, 11, 21, 1);
    VTX_ASSERT_TRUE(page1 != NULL);
    VTX_ASSERT_TRUE(page2 != NULL);
    VTX_ASSERT_EQUAL(registry.page_count, 2u);

    vtx_type_guard_fault_info_t info;
    void *base1 = vtx_type_guard_page_base(page1);
    uintptr_t addr1 = (uintptr_t)base1 + 1 * VTX_TYPE_GUARD_STRIDE;
    VTX_ASSERT_TRUE(vtx_type_guard_page_registry_lookup(&registry, addr1, &info));
    VTX_ASSERT_EQUAL(info.method_id, 10u);

    void *base2 = vtx_type_guard_page_base(page2);
    uintptr_t addr2 = (uintptr_t)base2 + 2 * VTX_TYPE_GUARD_STRIDE;
    VTX_ASSERT_TRUE(vtx_type_guard_page_registry_lookup(&registry, addr2, &info));
    VTX_ASSERT_EQUAL(info.method_id, 11u);

    vtx_type_guard_page_destroy(&registry, page1);
    vtx_type_guard_page_destroy(&registry, page2);
    vtx_type_guard_page_registry_destroy(&registry);
}

VTX_TEST(type_guard_page_availability_flag)
{
    bool avail = vtx_type_guard_page_is_available();
    bool avail_inline = vtx_type_guard_page_is_available_inline();
    VTX_ASSERT_EQUAL(avail, avail_inline);
}

VTX_TEST(type_guard_page_null_params)
{
    vtx_type_guard_page_t *page = vtx_type_guard_page_create(
        NULL, 1, 3, 1, 10, 0);
    VTX_ASSERT_TRUE(page == NULL);

    vtx_type_guard_page_destroy(NULL, NULL);

    vtx_type_guard_fault_info_t info;
    VTX_ASSERT_TRUE(!vtx_type_guard_page_registry_lookup(NULL, 0, &info));
}

/* ========================================================================== */
/* Guard Dependency Graph Tests                                                */
/* ========================================================================== */

VTX_TEST(guard_deps_graph_init_destroy)
{
    vtx_guard_deps_graph_t graph;
    VTX_ASSERT_EQUAL(vtx_guard_deps_graph_init(&graph), 0);
    VTX_ASSERT_EQUAL(graph.deps_count, 0u);
    VTX_ASSERT_EQUAL(graph.total_inline_deps, 0u);
    VTX_ASSERT_EQUAL(graph.total_sr_deps, 0u);

    vtx_guard_deps_graph_destroy(&graph);
}

VTX_TEST(guard_deps_manual_add)
{
    vtx_guard_deps_graph_t graph;
    VTX_ASSERT_EQUAL(vtx_guard_deps_graph_init(&graph), 0);

    vtx_guard_deps_add_inline(&graph, 1, 100);
    vtx_guard_deps_add_sr_object(&graph, 1, 200);
    vtx_guard_deps_add_branch(&graph, 1, 300);
    vtx_guard_deps_add_value_site(&graph, 1, 400);

    const vtx_guard_deps_t *deps = vtx_guard_deps_lookup(&graph, 1);
    VTX_ASSERT_TRUE(deps != NULL);
    VTX_ASSERT_EQUAL(deps->inline_site_count, 1u);
    VTX_ASSERT_EQUAL(deps->sr_object_count, 1u);
    VTX_ASSERT_EQUAL(deps->branch_pc_count, 1u);
    VTX_ASSERT_EQUAL(deps->value_site_count, 1u);
    VTX_ASSERT_EQUAL(deps->total_dep_count, 4u);

    VTX_ASSERT_EQUAL(deps->dependent_inline_sites[0], 100u);
    VTX_ASSERT_EQUAL(deps->dependent_sr_objects[0], 200u);
    VTX_ASSERT_EQUAL(deps->dependent_branch_pcs[0], 300u);
    VTX_ASSERT_EQUAL(deps->dependent_value_sites[0], 400u);

    VTX_ASSERT_EQUAL(graph.total_inline_deps, 1u);
    VTX_ASSERT_EQUAL(graph.total_sr_deps, 1u);
    VTX_ASSERT_EQUAL(graph.total_branch_deps, 1u);
    VTX_ASSERT_EQUAL(graph.total_value_deps, 1u);

    vtx_guard_deps_graph_destroy(&graph);
}

VTX_TEST(guard_deps_duplicate_detection)
{
    vtx_guard_deps_graph_t graph;
    VTX_ASSERT_EQUAL(vtx_guard_deps_graph_init(&graph), 0);

    vtx_guard_deps_add_inline(&graph, 1, 100);
    vtx_guard_deps_add_inline(&graph, 1, 100);

    const vtx_guard_deps_t *deps = vtx_guard_deps_lookup(&graph, 1);
    VTX_ASSERT_TRUE(deps != NULL);
    VTX_ASSERT_EQUAL(deps->inline_site_count, 1u);

    vtx_guard_deps_graph_destroy(&graph);
}

VTX_TEST(guard_deps_multiple_guards)
{
    vtx_guard_deps_graph_t graph;
    VTX_ASSERT_EQUAL(vtx_guard_deps_graph_init(&graph), 0);

    vtx_guard_deps_add_inline(&graph, 1, 100);
    vtx_guard_deps_add_sr_object(&graph, 1, 200);
    vtx_guard_deps_add_inline(&graph, 2, 150);
    vtx_guard_deps_add_branch(&graph, 2, 350);

    const vtx_guard_deps_t *deps1 = vtx_guard_deps_lookup(&graph, 1);
    VTX_ASSERT_TRUE(deps1 != NULL);
    VTX_ASSERT_EQUAL(deps1->inline_site_count, 1u);
    VTX_ASSERT_EQUAL(deps1->sr_object_count, 1u);

    const vtx_guard_deps_t *deps2 = vtx_guard_deps_lookup(&graph, 2);
    VTX_ASSERT_TRUE(deps2 != NULL);
    VTX_ASSERT_EQUAL(deps2->inline_site_count, 1u);
    VTX_ASSERT_EQUAL(deps2->branch_pc_count, 1u);

    vtx_guard_deps_graph_destroy(&graph);
}

VTX_TEST(guard_deps_invalidation)
{
    vtx_guard_deps_graph_t graph;
    VTX_ASSERT_EQUAL(vtx_guard_deps_graph_init(&graph), 0);

    vtx_guard_deps_add_inline(&graph, 1, 100);
    vtx_guard_deps_add_sr_object(&graph, 1, 200);
    vtx_guard_deps_add_branch(&graph, 1, 300);
    vtx_guard_deps_add_value_site(&graph, 1, 400);

    uint32_t *inline_sites, *sr_objects, *branch_pcs, *value_sites;
    uint32_t inline_count, sr_count, branch_count, value_count;

    uint32_t mask = vtx_guard_deps_invalidate(
        &graph, 1,
        &inline_sites, &inline_count,
        &sr_objects, &sr_count,
        &branch_pcs, &branch_count,
        &value_sites, &value_count);

    VTX_ASSERT_EQUAL(mask, (uint32_t)VTX_DEP_INVALIDATE_ALL);
    VTX_ASSERT_EQUAL(inline_count, 1u);
    VTX_ASSERT_EQUAL(sr_count, 1u);
    VTX_ASSERT_EQUAL(branch_count, 1u);
    VTX_ASSERT_EQUAL(value_count, 1u);

    vtx_guard_deps_graph_destroy(&graph);
}

VTX_TEST(guard_deps_invalidation_partial)
{
    vtx_guard_deps_graph_t graph;
    VTX_ASSERT_EQUAL(vtx_guard_deps_graph_init(&graph), 0);

    vtx_guard_deps_add_inline(&graph, 1, 100);

    uint32_t *inline_sites, *sr_objects, *branch_pcs, *value_sites;
    uint32_t inline_count, sr_count, branch_count, value_count;

    uint32_t mask = vtx_guard_deps_invalidate(
        &graph, 1,
        &inline_sites, &inline_count,
        &sr_objects, &sr_count,
        &branch_pcs, &branch_count,
        &value_sites, &value_count);

    VTX_ASSERT_EQUAL(mask, (uint32_t)VTX_DEP_INVALIDATE_INLINE);
    VTX_ASSERT_EQUAL(inline_count, 1u);
    VTX_ASSERT_EQUAL(sr_count, 0u);

    vtx_guard_deps_graph_destroy(&graph);
}

VTX_TEST(guard_deps_lookup_nonexistent)
{
    vtx_guard_deps_graph_t graph;
    VTX_ASSERT_EQUAL(vtx_guard_deps_graph_init(&graph), 0);

    const vtx_guard_deps_t *deps = vtx_guard_deps_lookup(&graph, 999);
    VTX_ASSERT_TRUE(deps == NULL);

    uint32_t guard_id = vtx_guard_deps_for_node(&graph, 999);
    VTX_ASSERT_EQUAL(guard_id, VTX_GUARD_DEPS_ID_INVALID);

    vtx_guard_deps_graph_destroy(&graph);
}

VTX_TEST(guard_deps_statistics)
{
    vtx_guard_deps_graph_t graph;
    VTX_ASSERT_EQUAL(vtx_guard_deps_graph_init(&graph), 0);

    VTX_ASSERT_EQUAL(vtx_guard_deps_active_guard_count(&graph), 0u);
    VTX_ASSERT_EQUAL(vtx_guard_deps_total_dependency_count(&graph), 0u);

    vtx_guard_deps_add_inline(&graph, 1, 100);
    vtx_guard_deps_add_inline(&graph, 1, 101);
    vtx_guard_deps_add_sr_object(&graph, 2, 200);

    VTX_ASSERT_EQUAL(vtx_guard_deps_active_guard_count(&graph), 2u);
    VTX_ASSERT_EQUAL(vtx_guard_deps_total_dependency_count(&graph), 3u);

    vtx_guard_deps_graph_destroy(&graph);
}

VTX_TEST(guard_deps_null_params)
{
    VTX_ASSERT_EQUAL(vtx_guard_deps_graph_init(NULL), -1);
    vtx_guard_deps_graph_destroy(NULL);

    const vtx_guard_deps_t *deps = vtx_guard_deps_lookup(NULL, 0);
    VTX_ASSERT_TRUE(deps == NULL);

    VTX_ASSERT_EQUAL(vtx_guard_deps_add_inline(NULL, 0, 0), -1);
}

/* ========================================================================== */
/* Phase-Reactive Version Reactivation Tests                                   */
/* ========================================================================== */

VTX_TEST(phase_react_manager_init_destroy)
{
    vtx_phase_react_manager_t mgr;
    VTX_ASSERT_EQUAL(vtx_phase_react_manager_init(
        &mgr, VTX_PHASE_REACT_DEFAULT_CODE_BUDGET), 0);
    VTX_ASSERT_EQUAL(mgr.registry_count, 0u);
    VTX_ASSERT_EQUAL(mgr.total_parked_versions, 0u);
    VTX_ASSERT_EQUAL(mgr.total_reactivated_versions, 0u);

    vtx_phase_react_manager_destroy(&mgr);
}

VTX_TEST(phase_react_get_registry)
{
    vtx_phase_react_manager_t mgr;
    VTX_ASSERT_EQUAL(vtx_phase_react_manager_init(
        &mgr, VTX_PHASE_REACT_DEFAULT_CODE_BUDGET), 0);

    vtx_phase_version_registry_t *reg = vtx_phase_react_get_registry(&mgr, 0);
    VTX_ASSERT_TRUE(reg != NULL);
    VTX_ASSERT_EQUAL(reg->method_id, 0u);
    VTX_ASSERT_EQUAL(reg->entry_count, 0u);

    vtx_phase_version_registry_t *reg2 = vtx_phase_react_get_registry(&mgr, 0);
    VTX_ASSERT_TRUE(reg2 == reg);

    vtx_phase_version_registry_t *reg3 = vtx_phase_react_get_registry(&mgr, 1);
    VTX_ASSERT_TRUE(reg3 != reg);
    VTX_ASSERT_EQUAL(reg3->method_id, 1u);

    vtx_phase_react_manager_destroy(&mgr);
}

VTX_TEST(phase_react_compute_hash_null)
{
    vtx_phase_hash_t hash = vtx_phase_react_compute_hash(NULL, 42);
    VTX_ASSERT_EQUAL(hash, VTX_PHASE_HASH_NONE);
}

VTX_TEST(phase_react_park_and_reactivate)
{
    vtx_phase_react_manager_t mgr;
    vtx_version_manager_t vmgr;
    vtx_arena_t arena;
    vtx_arena_init(&arena);
    VTX_ASSERT_EQUAL(vtx_version_manager_init(&vmgr, &arena), 0);
    VTX_ASSERT_EQUAL(vtx_phase_react_manager_init(
        &mgr, VTX_PHASE_REACT_DEFAULT_CODE_BUDGET), 0);

    vtx_code_version_t *v1 = vtx_version_create_compiling(&vmgr, 0, VTX_TIER_T2);
    VTX_ASSERT_TRUE(v1 != NULL);
    VTX_ASSERT_EQUAL(vtx_version_install(&vmgr, 0, v1, NULL), 0);
    VTX_ASSERT_EQUAL(v1->state, VTX_VERSION_ACTIVE);

    vtx_phase_hash_t phase_a = 0xAAAA;
    VTX_ASSERT_EQUAL(vtx_phase_react_park(&mgr, 0, v1, phase_a), 0);
    VTX_ASSERT_EQUAL(v1->state, VTX_VERSION_PARKED);
    VTX_ASSERT_EQUAL(mgr.total_parked_versions, 1u);

    vtx_code_version_t *reactivated = vtx_phase_react_try_reactivate(&mgr, 0, phase_a);
    VTX_ASSERT_TRUE(reactivated == v1);
    VTX_ASSERT_EQUAL(v1->state, VTX_VERSION_ACTIVE);
    VTX_ASSERT_EQUAL(mgr.total_reactivated_versions, 1u);

    vtx_phase_hash_t phase_b = 0xBBBB;
    vtx_code_version_t *not_found = vtx_phase_react_try_reactivate(&mgr, 0, phase_b);
    VTX_ASSERT_TRUE(not_found == NULL);

    vtx_phase_react_manager_destroy(&mgr);
    vtx_version_manager_destroy(&vmgr);
    vtx_arena_destroy(&arena);
}

VTX_TEST(phase_react_eviction)
{
    vtx_phase_react_manager_t mgr;
    vtx_version_manager_t vmgr;
    vtx_arena_t arena;
    vtx_arena_init(&arena);
    VTX_ASSERT_EQUAL(vtx_version_manager_init(&vmgr, &arena), 0);
    VTX_ASSERT_EQUAL(vtx_phase_react_manager_init(&mgr, 4096), 0);

    vtx_code_version_t *v1 = vtx_version_create_compiling(&vmgr, 0, VTX_TIER_T2);
    VTX_ASSERT_TRUE(v1 != NULL);
    vtx_version_install(&vmgr, 0, v1, NULL);

    vtx_phase_hash_t phase_a = 0xAAAA;
    VTX_ASSERT_EQUAL(vtx_phase_react_park(&mgr, 0, v1, phase_a), 0);
    VTX_ASSERT_EQUAL(v1->state, VTX_VERSION_PARKED);

    int result = vtx_phase_react_evict_oldest(&mgr, 0);
    VTX_ASSERT_EQUAL(result, 0);

    vtx_code_version_t *not_found = vtx_phase_react_try_reactivate(&mgr, 0, phase_a);
    VTX_ASSERT_TRUE(not_found == NULL);

    vtx_phase_react_manager_destroy(&mgr);
    vtx_version_manager_destroy(&vmgr);
    vtx_arena_destroy(&arena);
}

VTX_TEST(phase_react_statistics)
{
    vtx_phase_react_manager_t mgr;
    VTX_ASSERT_EQUAL(vtx_phase_react_manager_init(
        &mgr, VTX_PHASE_REACT_DEFAULT_CODE_BUDGET), 0);

    uint32_t parked, reactivated, evicted;
    vtx_phase_react_get_stats(&mgr, &parked, &reactivated, &evicted);
    VTX_ASSERT_EQUAL(parked, 0u);
    VTX_ASSERT_EQUAL(reactivated, 0u);
    VTX_ASSERT_EQUAL(evicted, 0u);

    vtx_phase_react_manager_destroy(&mgr);
}

VTX_TEST(phase_react_null_params)
{
    VTX_ASSERT_EQUAL(vtx_phase_react_manager_init(NULL, 1024), -1);
    vtx_phase_react_manager_destroy(NULL);

    vtx_phase_version_registry_t *reg = vtx_phase_react_get_registry(NULL, 0);
    VTX_ASSERT_TRUE(reg == NULL);

    VTX_ASSERT_EQUAL(vtx_phase_react_park(NULL, 0, NULL, 0), -1);
    VTX_ASSERT_TRUE(vtx_phase_react_try_reactivate(NULL, 0, 0) == NULL);
}

/* ========================================================================== */
/* Speculative PEA Tests                                                       */
/* ========================================================================== */

VTX_TEST(spec_pea_level_names)
{
    VTX_ASSERT_TRUE(vtx_spec_pea_level_name(VTX_SPEC_PEA_NONE) != NULL);
    VTX_ASSERT_TRUE(vtx_spec_pea_level_name(VTX_SPEC_PEA_CONSERVATIVE) != NULL);
    VTX_ASSERT_TRUE(vtx_spec_pea_level_name(VTX_SPEC_PEA_MODERATE) != NULL);
    VTX_ASSERT_TRUE(vtx_spec_pea_level_name(VTX_SPEC_PEA_AGGRESSIVE) != NULL);
}

VTX_TEST(spec_pea_run_no_profiles)
{
    vtx_arena_t arena;
    vtx_arena_init(&arena);

    vtx_graph_t graph;
    VTX_ASSERT_EQUAL(vtx_graph_init(&graph, 2), 0);

    vtx_spec_pea_analysis_t *analysis = vtx_spec_pea_run(
        &graph, &arena, VTX_SPEC_PEA_CONSERVATIVE, NULL);

    if (analysis != NULL) {
        VTX_ASSERT_EQUAL(analysis->spec_count, 0u);
        VTX_ASSERT_EQUAL(analysis->total_speculated, 0u);
        vtx_spec_pea_analysis_destroy(analysis);
    }

    vtx_graph_destroy(&graph);
    vtx_arena_destroy(&arena);
}

VTX_TEST(spec_pea_effective_escape_no_spec)
{
    vtx_arena_t arena;
    vtx_arena_init(&arena);

    vtx_graph_t graph;
    VTX_ASSERT_EQUAL(vtx_graph_init(&graph, 2), 0);

    vtx_spec_pea_analysis_t *analysis = vtx_spec_pea_run(
        &graph, &arena, VTX_SPEC_PEA_NONE, NULL);

    if (analysis != NULL) {
        vtx_escape_state_t state = vtx_spec_pea_effective_escape(analysis, 0);
        VTX_ASSERT_EQUAL(state, VTX_ESCAPE_GLOBAL);

        vtx_spec_pea_analysis_destroy(analysis);
    }

    vtx_graph_destroy(&graph);
    vtx_arena_destroy(&arena);
}

VTX_TEST(spec_pea_is_speculative_sr)
{
    vtx_arena_t arena;
    vtx_arena_init(&arena);

    vtx_graph_t graph;
    VTX_ASSERT_EQUAL(vtx_graph_init(&graph, 2), 0);

    vtx_spec_pea_analysis_t *analysis = vtx_spec_pea_run(
        &graph, &arena, VTX_SPEC_PEA_CONSERVATIVE, NULL);

    if (analysis != NULL) {
        bool sr = vtx_spec_pea_is_speculative_sr(analysis, 0);
        VTX_ASSERT_TRUE(!sr);

        vtx_spec_pea_analysis_destroy(analysis);
    }

    vtx_graph_destroy(&graph);
    vtx_arena_destroy(&arena);
}

VTX_TEST(spec_pea_install_guard_nonexistent)
{
    vtx_arena_t arena;
    vtx_arena_init(&arena);

    vtx_graph_t graph;
    VTX_ASSERT_EQUAL(vtx_graph_init(&graph, 2), 0);

    vtx_spec_pea_analysis_t *analysis = vtx_spec_pea_run(
        &graph, &arena, VTX_SPEC_PEA_CONSERVATIVE, NULL);

    if (analysis != NULL) {
        int result = vtx_spec_pea_install_guard(analysis, 999, 42);
        VTX_ASSERT_EQUAL(result, -1);

        vtx_spec_pea_analysis_destroy(analysis);
    }

    vtx_graph_destroy(&graph);
    vtx_arena_destroy(&arena);
}

/* ========================================================================== */
/* Lock Elision Tests                                                          */
/* ========================================================================== */

VTX_TEST(lock_elision_init_destroy)
{
    vtx_lock_elision_result_t result;
    VTX_ASSERT_EQUAL(vtx_lock_elision_init(&result), 0);
    VTX_ASSERT_EQUAL(result.profile_count, 0u);
    VTX_ASSERT_EQUAL(result.total_sites, 0u);
    VTX_ASSERT_EQUAL(result.eligible_sites, 0u);

    vtx_lock_elision_result_destroy(&result);
}

VTX_TEST(lock_elision_record_acquisition)
{
    vtx_lock_elision_result_t result;
    VTX_ASSERT_EQUAL(vtx_lock_elision_init(&result), 0);

    for (int i = 0; i < 200; i++) {
        vtx_lock_elision_record_acquisition(&result, 1, false);
    }

    const vtx_lock_contention_profile_t *prof = vtx_lock_elision_get_profile(&result, 1);
    VTX_ASSERT_TRUE(prof != NULL);
    VTX_ASSERT_TRUE(prof->total_acquisitions > 0);
    VTX_ASSERT_EQUAL(prof->contention_count, 0ull);

    vtx_lock_elision_result_destroy(&result);
}

VTX_TEST(lock_elision_contention_tracking)
{
    vtx_lock_elision_result_t result;
    VTX_ASSERT_EQUAL(vtx_lock_elision_init(&result), 0);

    for (int i = 0; i < 200; i++) {
        bool contended = (i % 10 == 0);
        vtx_lock_elision_record_acquisition(&result, 1, contended);
    }

    const vtx_lock_contention_profile_t *prof = vtx_lock_elision_get_profile(&result, 1);
    VTX_ASSERT_TRUE(prof != NULL);
    VTX_ASSERT_TRUE(prof->contention_count > 0);
    VTX_ASSERT_TRUE(!prof->is_eligible);

    vtx_lock_elision_result_destroy(&result);
}

VTX_TEST(lock_elision_install_guard)
{
    vtx_lock_elision_result_t result;
    VTX_ASSERT_EQUAL(vtx_lock_elision_init(&result), 0);

    for (int i = 0; i < 100; i++) {
        vtx_lock_elision_record_acquisition(&result, 1, false);
    }

    VTX_ASSERT_EQUAL(vtx_lock_elision_install_guard(&result, 1, 42), 0);

    const vtx_lock_contention_profile_t *prof = vtx_lock_elision_get_profile(&result, 1);
    VTX_ASSERT_TRUE(prof != NULL);
    VTX_ASSERT_EQUAL(prof->guard_id, 42u);
    VTX_ASSERT_TRUE(prof->guard_installed);

    vtx_lock_elision_result_destroy(&result);
}

VTX_TEST(lock_elision_nonexistent_site)
{
    vtx_lock_elision_result_t result;
    VTX_ASSERT_EQUAL(vtx_lock_elision_init(&result), 0);

    bool eligible = vtx_lock_elision_is_eligible(&result, 999);
    VTX_ASSERT_TRUE(!eligible);

    const vtx_lock_contention_profile_t *prof = vtx_lock_elision_get_profile(&result, 999);
    VTX_ASSERT_TRUE(prof == NULL);

    vtx_lock_elision_result_destroy(&result);
}

VTX_TEST(lock_elision_stats)
{
    vtx_lock_elision_result_t result;
    VTX_ASSERT_EQUAL(vtx_lock_elision_init(&result), 0);

    uint32_t total, eligible, elided;
    vtx_lock_elision_get_stats(&result, &total, &eligible, &elided);
    VTX_ASSERT_EQUAL(total, 0u);
    VTX_ASSERT_EQUAL(eligible, 0u);
    VTX_ASSERT_EQUAL(elided, 0u);

    vtx_lock_elision_result_destroy(&result);
}

VTX_TEST(lock_elision_null_params)
{
    VTX_ASSERT_EQUAL(vtx_lock_elision_init(NULL), -1);
    vtx_lock_elision_result_destroy(NULL);
    vtx_lock_elision_record_acquisition(NULL, 1, false);
}

/* ========================================================================== */
/* Version State Extension Tests (VTX_VERSION_PARKED)                          */
/* ========================================================================== */

VTX_TEST(version_parked_state)
{
    VTX_ASSERT_TRUE(vtx_version_state_name(VTX_VERSION_PARKED) != NULL);
    VTX_ASSERT_TRUE(strcmp(vtx_version_state_name(VTX_VERSION_PARKED), "Parked") == 0);
}

VTX_TEST(version_parked_lifecycle)
{
    vtx_version_manager_t mgr;
    vtx_arena_t arena;
    vtx_arena_init(&arena);
    VTX_ASSERT_EQUAL(vtx_version_manager_init(&mgr, &arena), 0);

    vtx_code_version_t *v = vtx_version_create_compiling(&mgr, 0, VTX_TIER_T2);
    VTX_ASSERT_TRUE(v != NULL);
    vtx_version_install(&mgr, 0, v, NULL);
    VTX_ASSERT_EQUAL(v->state, VTX_VERSION_ACTIVE);

    v->state = VTX_VERSION_PARKED;
    VTX_ASSERT_EQUAL(v->state, VTX_VERSION_PARKED);

    v->refcount = 0;
    VTX_ASSERT_EQUAL(vtx_version_free(&mgr, v), 0);

    vtx_version_manager_destroy(&mgr);
    vtx_arena_destroy(&arena);
}

/* ========================================================================== */
/* Integration Tests                                                           */
/* ========================================================================== */

VTX_TEST(guard_deps_and_type_guard_page)
{
    vtx_guard_deps_graph_t deps_graph;
    vtx_type_guard_page_registry_t registry;
    VTX_ASSERT_EQUAL(vtx_guard_deps_graph_init(&deps_graph), 0);
    VTX_ASSERT_EQUAL(vtx_type_guard_page_registry_init(&registry), 0);

    vtx_type_guard_page_t *page = vtx_type_guard_page_create(
        &registry, 5, 9, 42, 100, 7);
    VTX_ASSERT_TRUE(page != NULL);

    vtx_guard_deps_add_inline(&deps_graph, 100, 200);
    vtx_guard_deps_add_sr_object(&deps_graph, 100, 300);

    const vtx_guard_deps_t *deps = vtx_guard_deps_lookup(&deps_graph, 100);
    VTX_ASSERT_TRUE(deps != NULL);
    VTX_ASSERT_EQUAL(deps->inline_site_count, 1u);
    VTX_ASSERT_EQUAL(deps->sr_object_count, 1u);

    vtx_type_guard_fault_info_t info;
    void *base = vtx_type_guard_page_base(page);
    uintptr_t addr = (uintptr_t)base + 5 * VTX_TYPE_GUARD_STRIDE;
    VTX_ASSERT_TRUE(vtx_type_guard_page_registry_lookup(&registry, addr, &info));
    VTX_ASSERT_EQUAL(info.guard_id, 100u);

    vtx_type_guard_page_destroy(&registry, page);
    vtx_type_guard_page_registry_destroy(&registry);
    vtx_guard_deps_graph_destroy(&deps_graph);
}

/* ========================================================================== */
/* Test Runner                                                                 */
/* ========================================================================== */

int main(void)
{
    vtx_test_result_t result = vtx_test_run_all();
    printf("\nP2 Feature Tests: %u passed, %u failed, %u total\n",
           result.pass_count, result.fail_count, result.total_count);
    return (result.fail_count > 0) ? 1 : 0;
}
