/**
 * test_arena.c — Unit tests for VORTEX arena allocator
 *
 * Tests arena init/alloc/reset/destroy. Verifies alignment, growth,
 * statistics, and multi-page behavior.
 */

#include "test_framework.h"
#include "runtime/arena.h"

#include <stdint.h>
#include <string.h>

/* ========================================================================== */
/* Test: arena init and destroy                                                 */
/* ========================================================================== */

VTX_TEST(arena_init_destroy)
{
    vtx_arena_t arena;
    int rc = vtx_arena_init(&arena);
    VTX_ASSERT_EQUAL(rc, 0);
    VTX_ASSERT_NOT_NULL(arena.first_page);
    VTX_ASSERT_NOT_NULL(arena.current_page);
    VTX_ASSERT_EQUAL(arena.total_allocated, (size_t)0);

    vtx_arena_destroy(&arena);
    VTX_ASSERT_NULL(arena.first_page);
    VTX_ASSERT_NULL(arena.current_page);
    VTX_ASSERT_EQUAL(arena.total_allocated, (size_t)0);
}

/* ========================================================================== */
/* Test: basic allocation returns aligned pointer                               */
/* ========================================================================== */

VTX_TEST(arena_alloc_alignment)
{
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    void *p1 = vtx_arena_alloc(&arena, 1);
    VTX_ASSERT_NOT_NULL(p1);
    VTX_ASSERT_EQUAL((uintptr_t)p1 % 16, (uintptr_t)0);

    void *p2 = vtx_arena_alloc(&arena, 3);
    VTX_ASSERT_NOT_NULL(p2);
    VTX_ASSERT_EQUAL((uintptr_t)p2 % 16, (uintptr_t)0);

    void *p3 = vtx_arena_alloc(&arena, 100);
    VTX_ASSERT_NOT_NULL(p3);
    VTX_ASSERT_EQUAL((uintptr_t)p3 % 16, (uintptr_t)0);

    /* Pointers should be distinct */
    VTX_ASSERT_NOT_EQUAL(p1, p2);
    VTX_ASSERT_NOT_EQUAL(p2, p3);

    vtx_arena_destroy(&arena);
}

/* ========================================================================== */
/* Test: allocations are writable                                               */
/* ========================================================================== */

VTX_TEST(arena_alloc_writable)
{
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    uint64_t *p = (uint64_t *)vtx_arena_alloc(&arena, sizeof(uint64_t) * 4);
    VTX_ASSERT_NOT_NULL(p);

    p[0] = 0xDEADBEEFCAFE1234ULL;
    p[1] = 0x0123456789ABCDEFULL;
    p[2] = 0xFFFFFFFFFFFFFFFFULL;
    p[3] = 0;

    VTX_ASSERT_EQUAL(p[0], 0xDEADBEEFCAFE1234ULL);
    VTX_ASSERT_EQUAL(p[1], 0x0123456789ABCDEFULL);
    VTX_ASSERT_EQUAL(p[2], 0xFFFFFFFFFFFFFFFFULL);
    VTX_ASSERT_EQUAL(p[3], (uint64_t)0);

    vtx_arena_destroy(&arena);
}

/* ========================================================================== */
/* Test: total_allocated tracks bytes                                           */
/* ========================================================================== */

VTX_TEST(arena_total_allocated)
{
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    /* Each allocation is rounded up to 16-byte alignment */
    vtx_arena_alloc(&arena, 1);
    VTX_ASSERT_EQUAL(vtx_arena_total_allocated(&arena), (size_t)16);

    vtx_arena_alloc(&arena, 32);
    VTX_ASSERT_EQUAL(vtx_arena_total_allocated(&arena), (size_t)48);

    vtx_arena_alloc(&arena, 7);
    VTX_ASSERT_EQUAL(vtx_arena_total_allocated(&arena), (size_t)64);

    vtx_arena_destroy(&arena);
}

/* ========================================================================== */
/* Test: large allocation that exceeds page size triggers new page              */
/* ========================================================================== */

VTX_TEST(arena_large_alloc)
{
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    /* Allocate something larger than a page (65536 bytes) */
    size_t big_size = 100000;
    void *big = vtx_arena_alloc(&arena, big_size);
    VTX_ASSERT_NOT_NULL(big);
    VTX_ASSERT_EQUAL((uintptr_t)big % 16, (uintptr_t)0);

    /* Verify we can write to it */
    memset(big, 0xAB, big_size);
    unsigned char *bytes = (unsigned char *)big;
    VTX_ASSERT_EQUAL(bytes[0], 0xAB);
    VTX_ASSERT_EQUAL(bytes[big_size - 1], 0xAB);

    /* Further small allocations should still work */
    void *small = vtx_arena_alloc(&arena, 8);
    VTX_ASSERT_NOT_NULL(small);

    vtx_arena_destroy(&arena);
}

/* ========================================================================== */
/* Test: arena reset preserves first page, frees others                         */
/* ========================================================================== */

VTX_TEST(arena_reset)
{
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    /* Fill up the first page and spill into a second */
    size_t fill_size = 60000;
    void *p1 = vtx_arena_alloc(&arena, fill_size);
    VTX_ASSERT_NOT_NULL(p1);

    /* This should force a second page */
    void *p2 = vtx_arena_alloc(&arena, fill_size);
    VTX_ASSERT_NOT_NULL(p2);

    size_t before_reset = vtx_arena_total_allocated(&arena);
    VTX_ASSERT_TRUE(before_reset > 0);

    /* Reset */
    vtx_arena_reset(&arena);

    /* After reset: first page exists, total is zero */
    VTX_ASSERT_NOT_NULL(arena.first_page);
    VTX_ASSERT_EQUAL(arena.current_page, arena.first_page);
    VTX_ASSERT_EQUAL(arena.total_allocated, (size_t)0);

    /* Can allocate again after reset */
    void *p3 = vtx_arena_alloc(&arena, 64);
    VTX_ASSERT_NOT_NULL(p3);
    VTX_ASSERT_EQUAL((uintptr_t)p3 % 16, (uintptr_t)0);
    VTX_ASSERT_TRUE(vtx_arena_total_allocated(&arena) > 0);

    vtx_arena_destroy(&arena);
}

/* ========================================================================== */
/* Test: many small allocations                                                 */
/* ========================================================================== */

VTX_TEST(arena_many_small_allocs)
{
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    /* Allocate 1000 small objects */
    void *ptrs[1000];
    for (int i = 0; i < 1000; i++) {
        ptrs[i] = vtx_arena_alloc(&arena, sizeof(int));
        VTX_ASSERT_NOT_NULL(ptrs[i]);

        /* Write a unique value */
        int *ip = (int *)ptrs[i];
        *ip = i;
    }

    /* Verify all values are still intact */
    for (int i = 0; i < 1000; i++) {
        int *ip = (int *)ptrs[i];
        VTX_ASSERT_EQUAL(*ip, i);
    }

    vtx_arena_destroy(&arena);
}

/* ========================================================================== */
/* Test: zero-size allocation gets minimum alignment                            */
/* ========================================================================== */

VTX_TEST(arena_zero_size_alloc)
{
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    void *p = vtx_arena_alloc(&arena, 0);
    VTX_ASSERT_NOT_NULL(p);
    VTX_ASSERT_EQUAL((uintptr_t)p % 16, (uintptr_t)0);

    vtx_arena_destroy(&arena);
}

/* ========================================================================== */
/* Main                                                                         */
/* ========================================================================== */

int main(void)
{
    vtx_test_result_t result = vtx_test_run_all();
    return (result.fail_count > 0) ? 1 : 0;
}
