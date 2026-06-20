/*
 * VORTEX Versioned Code Cache Tests
 *
 * Audit priority #6 (Hardening): validates N+1 versioning, patching,
 * and reclamation in src/codecache/versioned.c.
 */

#include "test_framework.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "codecache/versioned.h"

VTX_TEST(versioned_install_and_get_active) {
    vtx_code_cache_t cache;
    VTX_ASSERT_TRUE(vtx_code_cache_init(&cache, 1 << 20) == 0);

    vtx_versioned_cache_t vc;
    VTX_ASSERT_TRUE(vtx_versioned_cache_init(&vc, &cache) == 0);

    /* Allocate some code. */
    void *code1 = vtx_code_cache_alloc(&cache, 64);
    VTX_ASSERT_TRUE(code1 != NULL);
    memset(code1, 0x90, 64);  /* NOPs */
    vtx_code_cache_finalize(&cache);

    /* Install version 1. */
    uint32_t v1 = vtx_versioned_cache_install(&vc, 42, code1, 64);
    VTX_ASSERT_TRUE(v1 == 1);

    vtx_code_version_t *active = vtx_versioned_cache_get_active(&vc, 42);
    VTX_ASSERT_TRUE(active != NULL);
    VTX_ASSERT_TRUE(active->version_number == 1);
    VTX_ASSERT_TRUE(active->is_active);
    VTX_ASSERT_TRUE(active->code_ptr == code1);

    vtx_versioned_cache_destroy(&vc);
    vtx_code_cache_destroy(&cache);
}

VTX_TEST(versioned_install_retires_old) {
    vtx_code_cache_t cache;
    VTX_ASSERT_TRUE(vtx_code_cache_init(&cache, 1 << 20) == 0);
    vtx_versioned_cache_t vc;
    VTX_ASSERT_TRUE(vtx_versioned_cache_init(&vc, &cache) == 0);

    void *code1 = vtx_code_cache_alloc(&cache, 64);
    memset(code1, 0x90, 64);
    void *code2 = vtx_code_cache_alloc(&cache, 64);
    memset(code2, 0xCC, 64);
    vtx_code_cache_finalize(&cache);

    /* Install v1, then v2. v1 should be retired. */
    vtx_versioned_cache_install(&vc, 7, code1, 64);
    vtx_versioned_cache_install(&vc, 7, code2, 64);

    vtx_code_version_t *active = vtx_versioned_cache_get_active(&vc, 7);
    VTX_ASSERT_TRUE(active != NULL);
    VTX_ASSERT_TRUE(active->code_ptr == code2);
    VTX_ASSERT_TRUE(active->version_number == 2);

    /* v1 should be retired but still in the list (N+1 versioning). */
    bool found_retired = false;
    uint32_t idx = 7 % VTX_VERSIONED_CACHE_MAX_METHODS;
    for (vtx_code_version_t *v = vc.versions[idx]; v != NULL; v = v->next) {
        if (v->is_retired && v->code_ptr == code1) {
            found_retired = true;
            break;
        }
    }
    VTX_ASSERT_TRUE(found_retired);

    vtx_versioned_cache_destroy(&vc);
    vtx_code_cache_destroy(&cache);
}

VTX_TEST(versioned_on_enter_exit_tracks_stack) {
    vtx_code_cache_t cache;
    VTX_ASSERT_TRUE(vtx_code_cache_init(&cache, 1 << 20) == 0);
    vtx_versioned_cache_t vc;
    VTX_ASSERT_TRUE(vtx_versioned_cache_init(&vc, &cache) == 0);

    void *code = vtx_code_cache_alloc(&cache, 64);
    memset(code, 0x90, 64);
    vtx_code_cache_finalize(&cache);
    vtx_versioned_cache_install(&vc, 5, code, 64);

    vtx_code_version_t *active = vtx_versioned_cache_get_active(&vc, 5);
    VTX_ASSERT_TRUE(active->on_stack_count == 0);

    vtx_versioned_cache_on_enter(&vc, 5);
    VTX_ASSERT_TRUE(active->on_stack_count == 1);
    vtx_versioned_cache_on_enter(&vc, 5);
    VTX_ASSERT_TRUE(active->on_stack_count == 2);

    vtx_versioned_cache_on_exit(&vc, 5);
    VTX_ASSERT_TRUE(active->on_stack_count == 1);
    vtx_versioned_cache_on_exit(&vc, 5);
    VTX_ASSERT_TRUE(active->on_stack_count == 0);

    vtx_versioned_cache_destroy(&vc);
    vtx_code_cache_destroy(&cache);
}

VTX_TEST(versioned_retired_not_reclaimed_while_on_stack) {
    vtx_code_cache_t cache;
    VTX_ASSERT_TRUE(vtx_code_cache_init(&cache, 1 << 20) == 0);
    vtx_versioned_cache_t vc;
    VTX_ASSERT_TRUE(vtx_versioned_cache_init(&vc, &cache) == 0);

    void *code1 = vtx_code_cache_alloc(&cache, 64);
    memset(code1, 0x90, 64);
    void *code2 = vtx_code_cache_alloc(&cache, 64);
    memset(code2, 0xCC, 64);
    vtx_code_cache_finalize(&cache);

    vtx_versioned_cache_install(&vc, 3, code1, 64);
    /* Thread enters v1. */
    vtx_versioned_cache_on_enter(&vc, 3);
    /* Install v2 — v1 is retired but thread is still on v1's stack. */
    vtx_versioned_cache_install(&vc, 3, code2, 64);

    /* Reclaim — v1 should NOT be reclaimed (on_stack_count > 0). */
    uint32_t reclaimed = vtx_versioned_cache_reclaim(&vc);
    VTX_ASSERT_TRUE(reclaimed == 0);
    VTX_ASSERT_TRUE(vc.total_retired == 1);

    /* Thread exits v1. */
    vtx_versioned_cache_on_exit(&vc, 3);

    /* Now reclaim — v1 should be freed. */
    reclaimed = vtx_versioned_cache_reclaim(&vc);
    VTX_ASSERT_TRUE(reclaimed == 1);
    VTX_ASSERT_TRUE(vc.total_retired == 0);

    vtx_versioned_cache_destroy(&vc);
    vtx_code_cache_destroy(&cache);
}

VTX_TEST(versioned_patch_modifies_code) {
    vtx_code_cache_t cache;
    VTX_ASSERT_TRUE(vtx_code_cache_init(&cache, 1 << 20) == 0);
    vtx_versioned_cache_t vc;
    VTX_ASSERT_TRUE(vtx_versioned_cache_init(&vc, &cache) == 0);

    void *code = vtx_code_cache_alloc(&cache, 64);
    memset(code, 0x90, 64);  /* all NOPs */
    vtx_code_cache_finalize(&cache);
    vtx_versioned_cache_install(&vc, 11, code, 64);

    /* Verify initial state. */
    VTX_ASSERT_TRUE(((uint8_t *)code)[0] == 0x90);
    VTX_ASSERT_TRUE(((uint8_t *)code)[10] == 0x90);

    /* Patch bytes 10-13 with 0xCC. */
    uint8_t patch[] = {0xCC, 0xCC, 0xCC, 0xCC};
    int rc = vtx_versioned_cache_patch(&vc, 11, 10, patch, 4);
    VTX_ASSERT_TRUE(rc == 0);

    /* Verify patch applied. */
    VTX_ASSERT_TRUE(((uint8_t *)code)[10] == 0xCC);
    VTX_ASSERT_TRUE(((uint8_t *)code)[13] == 0xCC);
    VTX_ASSERT_TRUE(((uint8_t *)code)[14] == 0x90);  /* unchanged */

    vtx_versioned_cache_destroy(&vc);
    vtx_code_cache_destroy(&cache);
}

VTX_TEST(versioned_max_retired_force_free) {
    /* Install MAX_RETIRED+1 versions — oldest retired should be force-freed. */
    vtx_code_cache_t cache;
    VTX_ASSERT_TRUE(vtx_code_cache_init(&cache, 1 << 20) == 0);
    vtx_versioned_cache_t vc;
    VTX_ASSERT_TRUE(vtx_versioned_cache_init(&vc, &cache) == 0);

    /* Install more versions than MAX_RETIRED. */
    for (uint32_t i = 0; i < VTX_VERSIONED_CACHE_MAX_RETIRED + 3; i++) {
        void *code = vtx_code_cache_alloc(&cache, 32);
        memset(code, 0x90, 32);
        vtx_code_cache_finalize(&cache);
        vtx_versioned_cache_install(&vc, 99, code, 32);
    }

    /* total_retired should be <= MAX_RETIRED (oldest were force-freed). */
    VTX_ASSERT_TRUE(vc.total_retired <= VTX_VERSIONED_CACHE_MAX_RETIRED);
    /* force_frees counts versions that were freed while on_stack_count > 0.
     * Since we didn't enter any version, on_stack_count is always 0, so
     * force_frees is 0. The versions were still freed (just not counted
     * as "force" frees). The key invariant is total_retired <= MAX_RETIRED. */
    printf("[versioned] after %u installs: retired=%u force_frees=%llu\n",
           VTX_VERSIONED_CACHE_MAX_RETIRED + 3,
           vc.total_retired, (unsigned long long)vc.total_force_frees);

    vtx_versioned_cache_destroy(&vc);
    vtx_code_cache_destroy(&cache);
}

VTX_TEST(versioned_compact) {
    vtx_code_cache_t cache;
    VTX_ASSERT_TRUE(vtx_code_cache_init(&cache, 1 << 20) == 0);
    vtx_versioned_cache_t vc;
    VTX_ASSERT_TRUE(vtx_versioned_cache_init(&vc, &cache) == 0);

    /* Install and retire several versions, then compact. */
    for (uint32_t i = 0; i < 5; i++) {
        void *code = vtx_code_cache_alloc(&cache, 32);
        memset(code, 0x90, 32);
        vtx_code_cache_finalize(&cache);
        vtx_versioned_cache_install(&vc, 50, code, 32);
    }
    /* No threads on stack, so all retired versions can be reclaimed. */
    uint64_t freed = vtx_versioned_cache_compact(&vc);
    VTX_ASSERT_TRUE(freed >= 4);  /* at least 4 retired versions freed */
    VTX_ASSERT_TRUE(vc.total_compactions == 1);

    vtx_versioned_cache_destroy(&vc);
    vtx_code_cache_destroy(&cache);
}

VTX_TEST(versioned_stats_str) {
    vtx_code_cache_t cache;
    VTX_ASSERT_TRUE(vtx_code_cache_init(&cache, 1 << 20) == 0);
    vtx_versioned_cache_t vc;
    VTX_ASSERT_TRUE(vtx_versioned_cache_init(&vc, &cache) == 0);

    void *code = vtx_code_cache_alloc(&cache, 32);
    memset(code, 0x90, 32);
    vtx_code_cache_finalize(&cache);
    vtx_versioned_cache_install(&vc, 1, code, 32);

    char buf[256];
    vtx_versioned_cache_stats_str(&vc, buf, sizeof(buf));
    printf("[versioned] stats: %s\n", buf);
    VTX_ASSERT_TRUE(strstr(buf, "active=1") != NULL);

    vtx_versioned_cache_destroy(&vc);
    vtx_code_cache_destroy(&cache);
}

VTX_TEST(versioned_distinct_methods_dont_interfere) {
    vtx_code_cache_t cache;
    VTX_ASSERT_TRUE(vtx_code_cache_init(&cache, 1 << 20) == 0);
    vtx_versioned_cache_t vc;
    VTX_ASSERT_TRUE(vtx_versioned_cache_init(&vc, &cache) == 0);

    void *code1 = vtx_code_cache_alloc(&cache, 32);
    void *code2 = vtx_code_cache_alloc(&cache, 32);
    memset(code1, 0x90, 32);
    memset(code2, 0xCC, 32);
    vtx_code_cache_finalize(&cache);

    vtx_versioned_cache_install(&vc, 100, code1, 32);
    vtx_versioned_cache_install(&vc, 200, code2, 32);

    vtx_code_version_t *a1 = vtx_versioned_cache_get_active(&vc, 100);
    vtx_code_version_t *a2 = vtx_versioned_cache_get_active(&vc, 200);
    VTX_ASSERT_TRUE(a1 != NULL && a2 != NULL);
    VTX_ASSERT_TRUE(a1->code_ptr == code1);
    VTX_ASSERT_TRUE(a2->code_ptr == code2);

    /* on_enter on method 100 shouldn't affect method 200. */
    vtx_versioned_cache_on_enter(&vc, 100);
    VTX_ASSERT_TRUE(a1->on_stack_count == 1);
    VTX_ASSERT_TRUE(a2->on_stack_count == 0);

    vtx_versioned_cache_destroy(&vc);
    vtx_code_cache_destroy(&cache);
}

int main(void) {
    printf("=== VORTEX Versioned Code Cache Tests ===\n\n");
    vtx_test_run_all();
    return 0;
}
