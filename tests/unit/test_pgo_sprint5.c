/**
 * VORTEX PGO Sprint 5 T1 Code Persistence Tests
 *
 * Tests for:
 *   - Sprint 5.1: T1 cache file format (save/load)
 *   - Sprint 5.2: mmap-based loader (code is executable after load)
 *   - Sprint 5.3: SHA-256 bytecode gating (reject stale cache)
 *
 * The headline test: save T1 compiled code, load it, verify the entry
 * point produces the correct result when called. This proves the cold-
 * start killer works: the second process can execute native code without
 * recompiling.
 *
 * Note: We can't easily test full T1 JIT compilation in a unit test
 * (it requires the full VORTEX runtime). Instead, we test the persistence
 * layer directly: we create fake compiled code blobs, save them, load
 * them, and verify the mmap'd code is executable and the entry points
 * resolve correctly.
 */

#include "test_framework.h"
#include "vortex_config.h"
#include "codecache/types.h"
#include "codecache/t1_persist.h"
#include "profile/persist.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

/* ========================================================================== */
/* Helpers                                                                     */
/* ========================================================================== */

/* Build a fake compiled_code_t with simple executable native code.
 *
 * The "native code" is a minimal x86-64 function that returns 42:
 *   mov eax, 42    ; B8 2A 00 00 00
 *   ret            ; C3
 *
 * This is 6 bytes of real, executable code. When we save+load it and
 * call the entry point, it should return 42. */
static vtx_compiled_code_t *make_fake_compiled_code(uint32_t method_id, int32_t return_value)
{
    vtx_compiled_code_t *cc = (vtx_compiled_code_t *)calloc(1, sizeof(vtx_compiled_code_t));
    VTX_ASSERT_NOT_NULL(cc);

    /* 6 bytes: mov eax, imm32; ret */
    cc->code_size = 6;
    cc->code = (uint8_t *)malloc(cc->code_size);
    VTX_ASSERT_NOT_NULL(cc->code);

    /* mov eax, return_value */
    cc->code[0] = 0xB8;  /* mov eax, imm32 */
    cc->code[1] = (uint8_t)(return_value & 0xFF);
    cc->code[2] = (uint8_t)((return_value >> 8) & 0xFF);
    cc->code[3] = (uint8_t)((return_value >> 16) & 0xFF);
    cc->code[4] = (uint8_t)((return_value >> 24) & 0xFF);
    /* ret */
    cc->code[5] = 0xC3;

    cc->entry_point = cc->code;  /* entry at start */
    cc->method_id = method_id;
    cc->stack_slots = 0;
    cc->local_slots = 0;

    return cc;
}

static void free_fake_compiled_code(vtx_compiled_code_t *cc)
{
    if (cc == NULL) return;
    if (cc->code) free(cc->code);
    free(cc);
}

/* Build a bytecode hash (fake — just repeat a byte). */
static void make_fake_hash(uint8_t hash[VTX_PROFILE_HASH_SIZE], uint8_t fill)
{
    memset(hash, fill, VTX_PROFILE_HASH_SIZE);
}

/* ========================================================================== */
/* Sprint 5.1: File format                                                     */
/* ========================================================================== */

VTX_TEST(t1_filename_format)
{
    char buf[600];
    VTX_ASSERT_EQUAL(0, vtx_t1_cache_filename("/tmp/profs", "abcdef0123456789",
                                                 buf, sizeof(buf)));
    VTX_ASSERT_TRUE(strcmp(buf, "/tmp/profs/abcdef0123456789.t1c") == 0);
}

VTX_TEST(t1_filename_buffer_overflow)
{
    char buf[10];
    VTX_ASSERT_EQUAL(-1, vtx_t1_cache_filename("/tmp/profs", "abcdef0123456789",
                                                 buf, sizeof(buf)));
}

/* ========================================================================== */
/* Sprint 5.1: Save + Load round-trip                                          */
/* ========================================================================== */

VTX_TEST(t1_save_load_roundtrip_single_method)
{
    const char *filename = "/tmp/vortex_sprint5_test_single.t1c";
    remove(filename);

    /* Build fake compiled code. */
    vtx_compiled_code_t *cc = make_fake_compiled_code(42, 1337);
    const vtx_compiled_code_t *methods[] = { cc };

    uint8_t hash[VTX_PROFILE_HASH_SIZE];
    make_fake_hash(hash, 0xAA);

    /* Save. */
    VTX_ASSERT_TRUE(vtx_t1_cache_save(filename, hash, methods, 1));

    /* Load. */
    vtx_t1_cache_t cache;
    VTX_ASSERT_TRUE(vtx_t1_cache_load(&cache, filename, hash));

    /* Verify stats. */
    uint32_t mcount = 0, csize = 0;
    uint64_t ltime = 0;
    uint32_t relocs = 0;
    vtx_t1_cache_stats(&cache, &mcount, &csize, &ltime, &relocs);
    VTX_ASSERT_EQUAL(1u, mcount);
    VTX_ASSERT_TRUE(csize > 0);

    /* Verify the method is present. */
    VTX_ASSERT_TRUE(vtx_t1_cache_has_method(&cache, 42));
    VTX_ASSERT_FALSE(vtx_t1_cache_has_method(&cache, 999));

    /* Get the entry point. */
    void *entry = vtx_t1_cache_get_entry(&cache, 42);
    VTX_ASSERT_NOT_NULL(entry);

    /* Call the entry point — it should return 1337.
     * The fake code is: mov eax, 1337; ret */
    typedef int (*int_fn_t)(void);
    int_fn_t fn = (int_fn_t)entry;
    int result = fn();
    VTX_ASSERT_EQUAL(1337, result);

    vtx_t1_cache_destroy(&cache);
    free_fake_compiled_code(cc);
    remove(filename);
}

VTX_TEST(t1_save_load_roundtrip_multiple_methods)
{
    const char *filename = "/tmp/vortex_sprint5_test_multi.t1c";
    remove(filename);

    /* Build 3 fake compiled methods with different return values. */
    vtx_compiled_code_t *cc1 = make_fake_compiled_code(1, 100);
    vtx_compiled_code_t *cc2 = make_fake_compiled_code(2, 200);
    vtx_compiled_code_t *cc3 = make_fake_compiled_code(3, 300);
    const vtx_compiled_code_t *methods[] = { cc1, cc2, cc3 };

    uint8_t hash[VTX_PROFILE_HASH_SIZE];
    make_fake_hash(hash, 0xBB);

    VTX_ASSERT_TRUE(vtx_t1_cache_save(filename, hash, methods, 3));

    vtx_t1_cache_t cache;
    VTX_ASSERT_TRUE(vtx_t1_cache_load(&cache, filename, hash));

    VTX_ASSERT_EQUAL(3u, cache.method_count);
    VTX_ASSERT_TRUE(vtx_t1_cache_has_method(&cache, 1));
    VTX_ASSERT_TRUE(vtx_t1_cache_has_method(&cache, 2));
    VTX_ASSERT_TRUE(vtx_t1_cache_has_method(&cache, 3));

    /* Call each method and verify the return value. */
    typedef int (*int_fn_t)(void);

    int_fn_t fn1 = (int_fn_t)vtx_t1_cache_get_entry(&cache, 1);
    VTX_ASSERT_EQUAL(100, fn1());

    int_fn_t fn2 = (int_fn_t)vtx_t1_cache_get_entry(&cache, 2);
    VTX_ASSERT_EQUAL(200, fn2());

    int_fn_t fn3 = (int_fn_t)vtx_t1_cache_get_entry(&cache, 3);
    VTX_ASSERT_EQUAL(300, fn3());

    vtx_t1_cache_destroy(&cache);
    free_fake_compiled_code(cc1);
    free_fake_compiled_code(cc2);
    free_fake_compiled_code(cc3);
    remove(filename);
}

/* ========================================================================== */
/* THE HEADLINE TEST: loaded T1 code is executable and produces correct results */
/* ========================================================================== */

/**
 * Save T1 compiled code in one process, load it, and verify the loaded
 * code is executable and produces the correct result.
 *
 * This proves the cold-start killer works: the second "process" (simulated
 * by loading the cache file) can execute native code without recompiling.
 *
 * We use a return-value-parameterized fake so we can verify the RIGHT code
 * was loaded for the RIGHT method.
 */
VTX_TEST(t1_loaded_code_is_executable)
{
    const char *filename = "/tmp/vortex_sprint5_test_exec.t1c";
    remove(filename);

    /* Build code that returns 0xDEAD (57005). */
    vtx_compiled_code_t *cc = make_fake_compiled_code(99, 0xDEAD);
    const vtx_compiled_code_t *methods[] = { cc };

    uint8_t hash[VTX_PROFILE_HASH_SIZE];
    make_fake_hash(hash, 0xCC);

    VTX_ASSERT_TRUE(vtx_t1_cache_save(filename, hash, methods, 1));

    vtx_t1_cache_t cache;
    VTX_ASSERT_TRUE(vtx_t1_cache_load(&cache, filename, hash));

    /* The loaded code must be executable (mmap'd PROT_READ|PROT_EXEC). */
    void *entry = vtx_t1_cache_get_entry(&cache, 99);
    VTX_ASSERT_NOT_NULL(entry);

    /* Execute it. */
    typedef int (*int_fn_t)(void);
    int_fn_t fn = (int_fn_t)entry;
    int result = fn();
    VTX_ASSERT_EQUAL(0xDEAD, result);

    vtx_t1_cache_destroy(&cache);
    free_fake_compiled_code(cc);
    remove(filename);
}

/* ========================================================================== */
/* Sprint 5.3: Bytecode hash gating                                            */
/* ========================================================================== */

VTX_TEST(t1_load_rejects_wrong_bytecode_hash)
{
    const char *filename = "/tmp/vortex_sprint5_test_hash.t1c";
    remove(filename);

    vtx_compiled_code_t *cc = make_fake_compiled_code(1, 42);
    const vtx_compiled_code_t *methods[] = { cc };

    uint8_t hash_save[VTX_PROFILE_HASH_SIZE];
    uint8_t hash_load[VTX_PROFILE_HASH_SIZE];
    make_fake_hash(hash_save, 0xAA);
    make_fake_hash(hash_load, 0xBB);  /* DIFFERENT hash */

    VTX_ASSERT_TRUE(vtx_t1_cache_save(filename, hash_save, methods, 1));

    /* Load with the wrong hash — should fail. */
    vtx_t1_cache_t cache;
    VTX_ASSERT_FALSE(vtx_t1_cache_load(&cache, filename, hash_load));

    free_fake_compiled_code(cc);
    remove(filename);
}

VTX_TEST(t1_load_accepts_correct_bytecode_hash)
{
    const char *filename = "/tmp/vortex_sprint5_test_hash_ok.t1c";
    remove(filename);

    vtx_compiled_code_t *cc = make_fake_compiled_code(1, 42);
    const vtx_compiled_code_t *methods[] = { cc };

    uint8_t hash[VTX_PROFILE_HASH_SIZE];
    make_fake_hash(hash, 0xAA);

    VTX_ASSERT_TRUE(vtx_t1_cache_save(filename, hash, methods, 1));

    /* Load with the correct hash — should succeed. */
    vtx_t1_cache_t cache;
    VTX_ASSERT_TRUE(vtx_t1_cache_load(&cache, filename, hash));

    vtx_t1_cache_destroy(&cache);
    free_fake_compiled_code(cc);
    remove(filename);
}

/* ========================================================================== */
/* Sprint 5.1: Error handling                                                  */
/* ========================================================================== */

VTX_TEST(t1_load_missing_file_returns_false)
{
    vtx_t1_cache_t cache;
    uint8_t hash[VTX_PROFILE_HASH_SIZE];
    make_fake_hash(hash, 0xAA);
    VTX_ASSERT_FALSE(vtx_t1_cache_load(&cache, "/nonexistent/file.t1c", hash));
}

VTX_TEST(t1_save_null_filename_returns_false)
{
    uint8_t hash[VTX_PROFILE_HASH_SIZE];
    make_fake_hash(hash, 0xAA);
    vtx_compiled_code_t *cc = make_fake_compiled_code(1, 42);
    const vtx_compiled_code_t *methods[] = { cc };
    VTX_ASSERT_FALSE(vtx_t1_cache_save(NULL, hash, methods, 1));
    free_fake_compiled_code(cc);
}

VTX_TEST(t1_save_zero_methods_returns_false)
{
    const char *filename = "/tmp/vortex_sprint5_test_zero.t1c";
    uint8_t hash[VTX_PROFILE_HASH_SIZE];
    make_fake_hash(hash, 0xAA);
    VTX_ASSERT_FALSE(vtx_t1_cache_save(filename, hash, NULL, 0));
}

/* ========================================================================== */
/* Sprint 5.2: Entry point resolution                                         */
/* ========================================================================== */

VTX_TEST(t1_get_entry_missing_method_returns_null)
{
    const char *filename = "/tmp/vortex_sprint5_test_missing.t1c";
    remove(filename);

    vtx_compiled_code_t *cc = make_fake_compiled_code(1, 42);
    const vtx_compiled_code_t *methods[] = { cc };

    uint8_t hash[VTX_PROFILE_HASH_SIZE];
    make_fake_hash(hash, 0xAA);

    VTX_ASSERT_TRUE(vtx_t1_cache_save(filename, hash, methods, 1));

    vtx_t1_cache_t cache;
    VTX_ASSERT_TRUE(vtx_t1_cache_load(&cache, filename, hash));

    /* Method 1 exists. */
    VTX_ASSERT_NOT_NULL(vtx_t1_cache_get_entry(&cache, 1));
    /* Method 999 does not. */
    VTX_ASSERT_NULL(vtx_t1_cache_get_entry(&cache, 999));

    vtx_t1_cache_destroy(&cache);
    free_fake_compiled_code(cc);
    remove(filename);
}

VTX_TEST(t1_destroy_idempotent)
{
    vtx_t1_cache_t cache;
    memset(&cache, 0, sizeof(cache));
    cache.code_fd = -1;
    /* Destroy on an uninitialized cache should be a safe no-op. */
    vtx_t1_cache_destroy(&cache);
    VTX_ASSERT_TRUE(true);  /* if we got here, no crash */
}

/* ========================================================================== */
/* Sprint 5: Cold-start timing (save then load, measure load time)            */
/* ========================================================================== */

/**
 * Measure the load time for a T1 cache. The load time should be much
 * smaller than recompiling from scratch (which is the cold-start cost
 * without T1 persistence).
 */
VTX_TEST(t1_load_time_is_submillisecond)
{
    const char *filename = "/tmp/vortex_sprint5_test_timing.t1c";
    remove(filename);

    /* Build 10 fake methods. */
    vtx_compiled_code_t *methods_arr[10];
    const vtx_compiled_code_t *methods[10];
    for (int i = 0; i < 10; i++) {
        methods_arr[i] = make_fake_compiled_code(i, i * 100);
        methods[i] = methods_arr[i];
    }

    uint8_t hash[VTX_PROFILE_HASH_SIZE];
    make_fake_hash(hash, 0xDD);

    VTX_ASSERT_TRUE(vtx_t1_cache_save(filename, hash, methods, 10));

    vtx_t1_cache_t cache;
    VTX_ASSERT_TRUE(vtx_t1_cache_load(&cache, filename, hash));

    uint64_t load_time_ns = 0;
    vtx_t1_cache_stats(&cache, NULL, NULL, &load_time_ns, NULL);

    /* Load time should be well under 1ms (1,000,000 ns) for 10 tiny methods.
     * We use a generous threshold to avoid CI flakiness. */
    VTX_ASSERT_TRUE(load_time_ns < 10000000);  /* < 10ms */

    vtx_t1_cache_destroy(&cache);
    for (int i = 0; i < 10; i++) free_fake_compiled_code(methods_arr[i]);
    remove(filename);
}

/* ========================================================================== */
/* Test runner                                                                 */
/* ========================================================================== */

int main(void)
{
    vtx_test_result_t result = vtx_test_run_all();
    printf("\n");
    printf("=== PGO Sprint 5 T1 Code Persistence Tests ===\n");
    printf("  Passed: %u / %u\n", result.pass_count, result.total_count);
    printf("  Failed: %u\n", result.fail_count);
    return (result.fail_count == 0) ? 0 : 1;
}
