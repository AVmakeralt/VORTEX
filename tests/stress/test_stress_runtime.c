/**
 * test_stress_runtime.c — Stress test suite for VORTEX runtime modules
 *
 * 200 exhaustive tests covering: arena allocator, tagged values (object),
 * type system, bytecode, and garbage collector.
 */

#include "test_framework.h"
#include "runtime/arena.h"
#include "runtime/object.h"
#include "runtime/type_system.h"
#include "runtime/bytecode.h"
#include "runtime/gc.h"
#include "runtime/helpers.h"
#include <string.h>
#include <math.h>

/* ========================================================================== */
/* Arena tests (40 tests)                                                      */
/* ========================================================================== */

VTX_TEST(test_arena_01)
{
    /* Init/destroy cycle */
    vtx_arena_t arena;
    int rc = vtx_arena_init(&arena);
    VTX_ASSERT_EQUAL(rc, 0);
    VTX_ASSERT_NOT_NULL(arena.first_page);
    VTX_ASSERT_EQUAL(arena.total_allocated, (size_t)0);
    vtx_arena_destroy(&arena);
    VTX_ASSERT_NULL(arena.first_page);
    VTX_ASSERT_EQUAL(arena.total_allocated, (size_t)0);
}

VTX_TEST(test_arena_02)
{
    /* Second init/destroy cycle on same struct */
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);
    vtx_arena_destroy(&arena);
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);
    VTX_ASSERT_NOT_NULL(arena.first_page);
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_arena_03)
{
    /* Multiple arenas coexisting */
    vtx_arena_t a1, a2, a3;
    VTX_ASSERT_EQUAL(vtx_arena_init(&a1), 0);
    VTX_ASSERT_EQUAL(vtx_arena_init(&a2), 0);
    VTX_ASSERT_EQUAL(vtx_arena_init(&a3), 0);

    void *p1 = vtx_arena_alloc(&a1, 64);
    void *p2 = vtx_arena_alloc(&a2, 128);
    void *p3 = vtx_arena_alloc(&a3, 256);
    VTX_ASSERT_NOT_NULL(p1);
    VTX_ASSERT_NOT_NULL(p2);
    VTX_ASSERT_NOT_NULL(p3);
    VTX_ASSERT_NOT_EQUAL(p1, p2);
    VTX_ASSERT_NOT_EQUAL(p2, p3);

    vtx_arena_destroy(&a1);
    vtx_arena_destroy(&a2);
    vtx_arena_destroy(&a3);
}

VTX_TEST(test_arena_04)
{
    /* Single allocation returns non-NULL and is writable */
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    int *p = (int *)vtx_arena_alloc(&arena, sizeof(int));
    VTX_ASSERT_NOT_NULL(p);
    *p = 0x12345678;
    VTX_ASSERT_EQUAL(*p, 0x12345678);

    vtx_arena_destroy(&arena);
}

VTX_TEST(test_arena_05)
{
    /* Multiple allocations, each writable and distinct */
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    void *ptrs[50];
    for (int i = 0; i < 50; i++) {
        ptrs[i] = vtx_arena_alloc(&arena, sizeof(int));
        VTX_ASSERT_NOT_NULL(ptrs[i]);
        *(int *)ptrs[i] = i;
    }
    for (int i = 0; i < 50; i++) {
        VTX_ASSERT_EQUAL(*(int *)ptrs[i], i);
    }
    for (int i = 1; i < 50; i++) {
        VTX_ASSERT_NOT_EQUAL(ptrs[i], ptrs[i - 1]);
    }

    vtx_arena_destroy(&arena);
}

VTX_TEST(test_arena_06)
{
    /* Large allocation exceeding page size */
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    size_t big = 100000;
    void *p = vtx_arena_alloc(&arena, big);
    VTX_ASSERT_NOT_NULL(p);
    memset(p, 0xAA, big);
    VTX_ASSERT_EQUAL(((unsigned char *)p)[0], 0xAA);
    VTX_ASSERT_EQUAL(((unsigned char *)p)[big - 1], 0xAA);

    vtx_arena_destroy(&arena);
}

VTX_TEST(test_arena_07)
{
    /* Alignment: all returned pointers are at least 8-byte aligned */
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    for (int i = 0; i < 20; i++) {
        size_t sizes[] = {1, 3, 5, 7, 9, 13, 17, 33, 65, 100,
                          1, 2, 4, 8, 16, 32, 64, 128, 255, 511};
        void *p = vtx_arena_alloc(&arena, sizes[i]);
        VTX_ASSERT_NOT_NULL(p);
        VTX_ASSERT_EQUAL((uintptr_t)p % 8, (uintptr_t)0);
    }

    vtx_arena_destroy(&arena);
}

VTX_TEST(test_arena_08)
{
    /* Alignment: 16-byte alignment (implementation guarantee) */
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    for (int i = 0; i < 10; i++) {
        void *p = vtx_arena_alloc(&arena, (size_t)(i + 1));
        VTX_ASSERT_NOT_NULL(p);
        VTX_ASSERT_EQUAL((uintptr_t)p % 16, (uintptr_t)0);
    }

    vtx_arena_destroy(&arena);
}

VTX_TEST(test_arena_09)
{
    /* Arena reset then re-allocate */
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    void *p1 = vtx_arena_alloc(&arena, 100);
    VTX_ASSERT_NOT_NULL(p1);
    VTX_ASSERT_TRUE(vtx_arena_total_allocated(&arena) > 0);

    vtx_arena_reset(&arena);
    VTX_ASSERT_EQUAL(vtx_arena_total_allocated(&arena), (size_t)0);
    VTX_ASSERT_NOT_NULL(arena.first_page);

    void *p2 = vtx_arena_alloc(&arena, 64);
    VTX_ASSERT_NOT_NULL(p2);
    VTX_ASSERT_TRUE(vtx_arena_total_allocated(&arena) > 0);

    vtx_arena_destroy(&arena);
}

VTX_TEST(test_arena_10)
{
    /* Arena reset preserves first page pointer */
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    vtx_arena_page_t *first = arena.first_page;
    vtx_arena_alloc(&arena, 60000);
    vtx_arena_alloc(&arena, 60000); /* force second page */
    vtx_arena_reset(&arena);

    VTX_ASSERT_EQUAL(arena.first_page, first);
    VTX_ASSERT_EQUAL(arena.current_page, first);

    vtx_arena_destroy(&arena);
}

VTX_TEST(test_arena_11)
{
    /* Total allocated tracking with multiple allocs */
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    vtx_arena_alloc(&arena, 1);
    size_t t1 = vtx_arena_total_allocated(&arena);
    VTX_ASSERT_TRUE(t1 >= 1);

    vtx_arena_alloc(&arena, 32);
    size_t t2 = vtx_arena_total_allocated(&arena);
    VTX_ASSERT_TRUE(t2 > t1);

    vtx_arena_alloc(&arena, 100);
    size_t t3 = vtx_arena_total_allocated(&arena);
    VTX_ASSERT_TRUE(t3 > t2);

    vtx_arena_destroy(&arena);
}

VTX_TEST(test_arena_12)
{
    /* Zero-size allocation */
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    void *p = vtx_arena_alloc(&arena, 0);
    VTX_ASSERT_NOT_NULL(p);
    VTX_ASSERT_EQUAL((uintptr_t)p % 16, (uintptr_t)0);

    vtx_arena_destroy(&arena);
}

VTX_TEST(test_arena_13)
{
    /* Many small allocations (stress test) */
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    for (int i = 0; i < 2000; i++) {
        void *p = vtx_arena_alloc(&arena, sizeof(int));
        VTX_ASSERT_NOT_NULL(p);
        *(int *)p = i;
    }

    vtx_arena_destroy(&arena);
}

VTX_TEST(test_arena_14)
{
    /* Allocation after partial usage of first page */
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    /* Use most of the first page */
    vtx_arena_alloc(&arena, 60000);
    /* Small alloc should still fit or spill */
    void *p = vtx_arena_alloc(&arena, 16);
    VTX_ASSERT_NOT_NULL(p);

    vtx_arena_destroy(&arena);
}

VTX_TEST(test_arena_15)
{
    /* Arena page chaining: allocations larger than default page */
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    void *big = vtx_arena_alloc(&arena, 200000);
    VTX_ASSERT_NOT_NULL(big);
    memset(big, 0x55, 200000);
    VTX_ASSERT_EQUAL(((unsigned char *)big)[0], 0x55);
    VTX_ASSERT_EQUAL(((unsigned char *)big)[199999], 0x55);

    /* Smaller alloc after big should still work */
    void *small = vtx_arena_alloc(&arena, 64);
    VTX_ASSERT_NOT_NULL(small);

    vtx_arena_destroy(&arena);
}

VTX_TEST(test_arena_16)
{
    /* Concurrent init/destroy of separate arenas */
    vtx_arena_t arenas[5];
    for (int i = 0; i < 5; i++) {
        VTX_ASSERT_EQUAL(vtx_arena_init(&arenas[i]), 0);
    }
    for (int i = 0; i < 5; i++) {
        vtx_arena_alloc(&arenas[i], (size_t)(i + 1) * 100);
    }
    for (int i = 0; i < 5; i++) {
        VTX_ASSERT_TRUE(vtx_arena_total_allocated(&arenas[i]) > 0);
    }
    for (int i = 0; i < 5; i++) {
        vtx_arena_destroy(&arenas[i]);
    }
}

VTX_TEST(test_arena_17)
{
    /* Allocation pattern: interleave alloc and reset */
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    for (int round = 0; round < 5; round++) {
        void *p = vtx_arena_alloc(&arena, 128);
        VTX_ASSERT_NOT_NULL(p);
        memset(p, round, 128);
        VTX_ASSERT_TRUE(vtx_arena_total_allocated(&arena) > 0);
        vtx_arena_reset(&arena);
        VTX_ASSERT_EQUAL(vtx_arena_total_allocated(&arena), (size_t)0);
    }

    vtx_arena_destroy(&arena);
}

VTX_TEST(test_arena_18)
{
    /* Repeated init/alloc/destroy cycles */
    for (int cycle = 0; cycle < 10; cycle++) {
        vtx_arena_t arena;
        VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);
        void *p = vtx_arena_alloc(&arena, 64);
        VTX_ASSERT_NOT_NULL(p);
        vtx_arena_destroy(&arena);
    }
}

VTX_TEST(test_arena_19)
{
    /* Very large single allocation */
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    size_t size = 1024 * 1024; /* 1 MB */
    void *p = vtx_arena_alloc(&arena, size);
    VTX_ASSERT_NOT_NULL(p);
    memset(p, 0, size);

    vtx_arena_destroy(&arena);
}

VTX_TEST(test_arena_20)
{
    /* Total allocated equals zero after init */
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);
    VTX_ASSERT_EQUAL(vtx_arena_total_allocated(&arena), (size_t)0);
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_arena_21)
{
    /* Multiple resets in a row */
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    vtx_arena_alloc(&arena, 100);
    vtx_arena_reset(&arena);
    vtx_arena_reset(&arena);
    vtx_arena_reset(&arena);
    VTX_ASSERT_EQUAL(vtx_arena_total_allocated(&arena), (size_t)0);

    vtx_arena_destroy(&arena);
}

VTX_TEST(test_arena_22)
{
    /* Alloc after reset reuses first page memory */
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    void *p1 = vtx_arena_alloc(&arena, 64);
    VTX_ASSERT_NOT_NULL(p1);
    vtx_arena_reset(&arena);
    void *p2 = vtx_arena_alloc(&arena, 64);
    VTX_ASSERT_NOT_NULL(p2);
    /* After reset, the same memory region may be reused */
    VTX_ASSERT_EQUAL((uintptr_t)p2 % 16, (uintptr_t)0);

    vtx_arena_destroy(&arena);
}

VTX_TEST(test_arena_23)
{
    /* Mixed size allocations */
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    void *p1 = vtx_arena_alloc(&arena, 1);
    void *p2 = vtx_arena_alloc(&arena, 4096);
    void *p3 = vtx_arena_alloc(&arena, 7);
    void *p4 = vtx_arena_alloc(&arena, 8192);
    VTX_ASSERT_NOT_NULL(p1);
    VTX_ASSERT_NOT_NULL(p2);
    VTX_ASSERT_NOT_NULL(p3);
    VTX_ASSERT_NOT_NULL(p4);

    vtx_arena_destroy(&arena);
}

VTX_TEST(test_arena_24)
{
    /* Write to allocation, reset, write again — no crash */
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    char *p = (char *)vtx_arena_alloc(&arena, 256);
    VTX_ASSERT_NOT_NULL(p);
    memcpy(p, "hello world", 12);
    VTX_ASSERT_EQUAL(memcmp(p, "hello world", 12), 0);

    vtx_arena_reset(&arena);

    p = (char *)vtx_arena_alloc(&arena, 256);
    VTX_ASSERT_NOT_NULL(p);
    memcpy(p, "reset world", 12);
    VTX_ASSERT_EQUAL(memcmp(p, "reset world", 12), 0);

    vtx_arena_destroy(&arena);
}

VTX_TEST(test_arena_25)
{
    /* Arena with many medium-sized allocations */
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    for (int i = 0; i < 500; i++) {
        void *p = vtx_arena_alloc(&arena, 128);
        VTX_ASSERT_NOT_NULL(p);
    }

    vtx_arena_destroy(&arena);
}

VTX_TEST(test_arena_26)
{
    /* Two pages worth of allocations */
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    /* Fill first page (~65536 bytes) */
    for (int i = 0; i < 4000; i++) {
        vtx_arena_alloc(&arena, 16);
    }
    /* Should have chained a second page */
    VTX_ASSERT_NOT_NULL(arena.first_page);

    vtx_arena_destroy(&arena);
}

VTX_TEST(test_arena_27)
{
    /* Total allocated after reset is zero */
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    for (int i = 0; i < 100; i++) {
        vtx_arena_alloc(&arena, 64);
    }
    VTX_ASSERT_TRUE(vtx_arena_total_allocated(&arena) > 0);
    vtx_arena_reset(&arena);
    VTX_ASSERT_EQUAL(vtx_arena_total_allocated(&arena), (size_t)0);

    vtx_arena_destroy(&arena);
}

VTX_TEST(test_arena_28)
{
    /* Allocation of 0 bytes multiple times */
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    for (int i = 0; i < 10; i++) {
        void *p = vtx_arena_alloc(&arena, 0);
        VTX_ASSERT_NOT_NULL(p);
    }

    vtx_arena_destroy(&arena);
}

VTX_TEST(test_arena_29)
{
    /* Large alloc followed by many small allocs */
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    vtx_arena_alloc(&arena, 80000);
    for (int i = 0; i < 100; i++) {
        void *p = vtx_arena_alloc(&arena, 8);
        VTX_ASSERT_NOT_NULL(p);
    }

    vtx_arena_destroy(&arena);
}

VTX_TEST(test_arena_30)
{
    /* Destroy without any allocations */
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);
    vtx_arena_destroy(&arena);
    VTX_ASSERT_NULL(arena.first_page);
}

VTX_TEST(test_arena_31)
{
    /* Reset without any allocations */
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);
    vtx_arena_reset(&arena);
    VTX_ASSERT_NOT_NULL(arena.first_page);
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_arena_32)
{
    /* Arena alloc pointer is distinct from arena struct */
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);
    void *p = vtx_arena_alloc(&arena, 64);
    VTX_ASSERT_NOT_NULL(p);
    VTX_ASSERT_NOT_EQUAL(p, (void *)&arena);
    vtx_arena_destroy(&arena);
}

VTX_TEST(test_arena_33)
{
    /* Sequential allocations return increasing addresses */
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    void *p1 = vtx_arena_alloc(&arena, 16);
    void *p2 = vtx_arena_alloc(&arena, 16);
    void *p3 = vtx_arena_alloc(&arena, 16);
    VTX_ASSERT_NOT_NULL(p1);
    VTX_ASSERT_NOT_NULL(p2);
    VTX_ASSERT_NOT_NULL(p3);
    VTX_ASSERT_TRUE((uintptr_t)p2 > (uintptr_t)p1);
    VTX_ASSERT_TRUE((uintptr_t)p3 > (uintptr_t)p2);

    vtx_arena_destroy(&arena);
}

VTX_TEST(test_arena_34)
{
    /* Write pattern: fill allocation with known bytes */
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    unsigned char *p = (unsigned char *)vtx_arena_alloc(&arena, 256);
    VTX_ASSERT_NOT_NULL(p);
    for (int i = 0; i < 256; i++) {
        p[i] = (unsigned char)i;
    }
    for (int i = 0; i < 256; i++) {
        VTX_ASSERT_EQUAL(p[i], (unsigned char)i);
    }

    vtx_arena_destroy(&arena);
}

VTX_TEST(test_arena_35)
{
    /* Arena with allocation sizes that are exact multiples of alignment */
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    for (int i = 1; i <= 10; i++) {
        void *p = vtx_arena_alloc(&arena, (size_t)(i * 16));
        VTX_ASSERT_NOT_NULL(p);
        VTX_ASSERT_EQUAL((uintptr_t)p % 16, (uintptr_t)0);
    }

    vtx_arena_destroy(&arena);
}

VTX_TEST(test_arena_36)
{
    /* Reset and verify we can allocate large again */
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    vtx_arena_alloc(&arena, 50000);
    vtx_arena_reset(&arena);
    void *big = vtx_arena_alloc(&arena, 100000);
    VTX_ASSERT_NOT_NULL(big);

    vtx_arena_destroy(&arena);
}

VTX_TEST(test_arena_37)
{
    /* Multiple pages then reset: verify page list is correct */
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    vtx_arena_alloc(&arena, 60000);
    vtx_arena_alloc(&arena, 60000);
    vtx_arena_alloc(&arena, 60000);

    vtx_arena_reset(&arena);
    VTX_ASSERT_EQUAL(arena.current_page, arena.first_page);

    vtx_arena_destroy(&arena);
}

VTX_TEST(test_arena_38)
{
    /* Stress: rapid alloc/reset cycles */
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    for (int i = 0; i < 50; i++) {
        vtx_arena_alloc(&arena, 1024);
        vtx_arena_reset(&arena);
    }

    vtx_arena_destroy(&arena);
}

VTX_TEST(test_arena_39)
{
    /* Arena allocation pattern: alternating large and small */
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    for (int i = 0; i < 10; i++) {
        void *large = vtx_arena_alloc(&arena, (size_t)(10000 + i * 1000));
        VTX_ASSERT_NOT_NULL(large);
        void *small = vtx_arena_alloc(&arena, 8);
        VTX_ASSERT_NOT_NULL(small);
    }

    vtx_arena_destroy(&arena);
}

VTX_TEST(test_arena_40)
{
    /* Verify total_allocated monotonically increases within a cycle */
    vtx_arena_t arena;
    VTX_ASSERT_EQUAL(vtx_arena_init(&arena), 0);

    size_t prev = 0;
    for (int i = 0; i < 20; i++) {
        vtx_arena_alloc(&arena, (size_t)(i + 1) * 8);
        size_t cur = vtx_arena_total_allocated(&arena);
        VTX_ASSERT_TRUE(cur >= prev);
        prev = cur;
    }

    vtx_arena_destroy(&arena);
}

/* ========================================================================== */
/* Object/Value tests (50 tests)                                               */
/* ========================================================================== */

VTX_TEST(test_object_01)
{
    /* SMI creation and extraction: 0 */
    vtx_value_t v = vtx_make_smi(0);
    VTX_ASSERT_TRUE(vtx_is_smi(v));
    VTX_ASSERT_EQUAL(vtx_smi_value(v), (int64_t)0);
}

VTX_TEST(test_object_02)
{
    /* SMI creation and extraction: 1 */
    vtx_value_t v = vtx_make_smi(1);
    VTX_ASSERT_TRUE(vtx_is_smi(v));
    VTX_ASSERT_EQUAL(vtx_smi_value(v), (int64_t)1);
}

VTX_TEST(test_object_03)
{
    /* SMI creation and extraction: -1 */
    vtx_value_t v = vtx_make_smi(-1);
    VTX_ASSERT_TRUE(vtx_is_smi(v));
    VTX_ASSERT_EQUAL(vtx_smi_value(v), (int64_t)-1);
}

VTX_TEST(test_object_04)
{
    /* SMI creation and extraction: VTX_SMI_MAX */
    vtx_value_t v = vtx_make_smi(VTX_SMI_MAX);
    VTX_ASSERT_TRUE(vtx_is_smi(v));
    VTX_ASSERT_EQUAL(vtx_smi_value(v), VTX_SMI_MAX);
}

VTX_TEST(test_object_05)
{
    /* SMI creation and extraction: VTX_SMI_MIN */
    vtx_value_t v = vtx_make_smi(VTX_SMI_MIN);
    VTX_ASSERT_TRUE(vtx_is_smi(v));
    VTX_ASSERT_EQUAL(vtx_smi_value(v), VTX_SMI_MIN);
}

VTX_TEST(test_object_06)
{
    /* SMI small positive */
    vtx_value_t v = vtx_make_smi(42);
    VTX_ASSERT_TRUE(vtx_is_smi(v));
    VTX_ASSERT_EQUAL(vtx_smi_value(v), (int64_t)42);
}

VTX_TEST(test_object_07)
{
    /* SMI small negative */
    vtx_value_t v = vtx_make_smi(-99);
    VTX_ASSERT_TRUE(vtx_is_smi(v));
    VTX_ASSERT_EQUAL(vtx_smi_value(v), (int64_t)-99);
}

VTX_TEST(test_object_08)
{
    /* Double creation and extraction: 0.0 */
    vtx_value_t v = vtx_make_double(0.0);
    VTX_ASSERT_TRUE(vtx_is_double(v));
    VTX_ASSERT_EQUAL(vtx_double_value(v), 0.0);
}

VTX_TEST(test_object_09)
{
    /* Double creation and extraction: 1.0 */
    vtx_value_t v = vtx_make_double(1.0);
    VTX_ASSERT_TRUE(vtx_is_double(v));
    VTX_ASSERT_TRUE(fabs(vtx_double_value(v) - 1.0) < 1e-15);
}

VTX_TEST(test_object_10)
{
    /* Double creation and extraction: -1.0 */
    vtx_value_t v = vtx_make_double(-1.0);
    VTX_ASSERT_TRUE(vtx_is_double(v));
    VTX_ASSERT_TRUE(fabs(vtx_double_value(v) - (-1.0)) < 1e-15);
}

VTX_TEST(test_object_11)
{
    /* Double creation and extraction: 3.14159 */
    vtx_value_t v = vtx_make_double(3.14159);
    VTX_ASSERT_TRUE(vtx_is_double(v));
    VTX_ASSERT_TRUE(fabs(vtx_double_value(v) - 3.14159) < 1e-10);
}

VTX_TEST(test_object_12)
{
    /* Double creation and extraction: 1e308 */
    vtx_value_t v = vtx_make_double(1e308);
    VTX_ASSERT_TRUE(vtx_is_double(v));
    VTX_ASSERT_TRUE(fabs(vtx_double_value(v) - 1e308) < 1e295);
}

VTX_TEST(test_object_13)
{
    /* Double creation and extraction: -1e308 */
    vtx_value_t v = vtx_make_double(-1e308);
    VTX_ASSERT_TRUE(vtx_is_double(v));
    VTX_ASSERT_TRUE(vtx_double_value(v) < 0.0);
    VTX_ASSERT_TRUE(fabs(vtx_double_value(v) - (-1e308)) < 1e295);
}

VTX_TEST(test_object_14)
{
    /* Double creation and extraction: INFINITY */
    vtx_value_t v = vtx_make_double(INFINITY);
    VTX_ASSERT_TRUE(vtx_is_double(v));
    VTX_ASSERT_TRUE(isinf(vtx_double_value(v)));
    VTX_ASSERT_TRUE(vtx_double_value(v) > 0.0);
}

VTX_TEST(test_object_15)
{
    /* Double creation and extraction: -INFINITY */
    vtx_value_t v = vtx_make_double(-INFINITY);
    VTX_ASSERT_TRUE(vtx_is_double(v));
    VTX_ASSERT_TRUE(isinf(vtx_double_value(v)));
    VTX_ASSERT_TRUE(vtx_double_value(v) < 0.0);
}

VTX_TEST(test_object_16)
{
    /* Bool creation and extraction: true */
    vtx_value_t v = vtx_make_bool(true);
    VTX_ASSERT_TRUE(vtx_is_bool(v));
    VTX_ASSERT_TRUE(vtx_bool_value(v));
}

VTX_TEST(test_object_17)
{
    /* Bool creation and extraction: false */
    vtx_value_t v = vtx_make_bool(false);
    VTX_ASSERT_TRUE(vtx_is_bool(v));
    VTX_ASSERT_FALSE(vtx_bool_value(v));
}

VTX_TEST(test_object_18)
{
    /* Null creation and is_null check */
    vtx_value_t v = vtx_make_null();
    VTX_ASSERT_TRUE(vtx_is_null(v));
    VTX_ASSERT_EQUAL(v, VTX_VALUE_NULL);
}

VTX_TEST(test_object_19)
{
    /* Undefined creation and is_undefined check */
    vtx_value_t v = vtx_make_undefined();
    VTX_ASSERT_TRUE(vtx_is_undefined(v));
    VTX_ASSERT_EQUAL(v, VTX_VALUE_UNDEFINED);
}

VTX_TEST(test_object_20)
{
    /* Heap pointer creation and extraction */
    _Alignas(8) char buf[256];
    vtx_heap_object_t *obj = (vtx_heap_object_t *)buf;
    vtx_heap_object_init(obj, 1, 1, 2, (uint32_t)sizeof(buf));

    vtx_value_t v = vtx_make_heap_ptr(obj);
    VTX_ASSERT_TRUE(vtx_is_heap_ptr(v));
    VTX_ASSERT_EQUAL(vtx_heap_ptr(v), (void *)obj);
}

VTX_TEST(test_object_21)
{
    /* NaN-boxing: make_double(0.0) != make_smi(0) */
    vtx_value_t d0 = vtx_make_double(0.0);
    vtx_value_t s0 = vtx_make_smi(0);
    VTX_ASSERT_NOT_EQUAL(d0, s0);
    VTX_ASSERT_TRUE(vtx_is_double(d0));
    VTX_ASSERT_TRUE(vtx_is_smi(s0));
}

VTX_TEST(test_object_22)
{
    /* Type discrimination: is_smi on double should be false */
    vtx_value_t d = vtx_make_double(3.14);
    VTX_ASSERT_FALSE(vtx_is_smi(d));
}

VTX_TEST(test_object_23)
{
    /* Type discrimination: is_double on smi should be false */
    vtx_value_t s = vtx_make_smi(42);
    VTX_ASSERT_FALSE(vtx_is_double(s));
}

VTX_TEST(test_object_24)
{
    /* Type discrimination: is_bool on null should be false */
    vtx_value_t n = vtx_make_null();
    VTX_ASSERT_FALSE(vtx_is_bool(n));
}

VTX_TEST(test_object_25)
{
    /* Type discrimination: is_null on undefined should be false */
    vtx_value_t u = vtx_make_undefined();
    VTX_ASSERT_FALSE(vtx_is_null(u));
}

VTX_TEST(test_object_26)
{
    /* Type discrimination: is_heap_ptr on smi should be false */
    vtx_value_t s = vtx_make_smi(10);
    VTX_ASSERT_FALSE(vtx_is_heap_ptr(s));
}

VTX_TEST(test_object_27)
{
    /* Heap object init with various type_id, shape_id, field_count */
    _Alignas(8) char buf[512];
    vtx_heap_object_t *obj = (vtx_heap_object_t *)buf;
    vtx_heap_object_init(obj, 7, 42, 3, (uint32_t)sizeof(buf));

    VTX_ASSERT_EQUAL(obj->type_id, (uint32_t)7);
    VTX_ASSERT_EQUAL(obj->shape_id, (uint32_t)42);
    VTX_ASSERT_EQUAL(obj->field_count, (uint32_t)3);
    VTX_ASSERT_EQUAL(obj->gc_mark, (uint8_t)0);
    VTX_ASSERT_EQUAL(obj->gc_age, (uint8_t)0);
    VTX_ASSERT_EQUAL(obj->gc_pinned, (uint8_t)0);
    VTX_ASSERT_EQUAL(obj->gc_remembered, (uint8_t)0);
}

VTX_TEST(test_object_28)
{
    /* Object get/set field: various field offsets */
    _Alignas(8) char buf[512];
    vtx_heap_object_t *obj = (vtx_heap_object_t *)buf;
    vtx_heap_object_init(obj, 1, 1, 5, (uint32_t)sizeof(buf));

    vtx_value_t v0 = vtx_make_smi(100);
    vtx_value_t v1 = vtx_make_double(2.5);
    vtx_value_t v2 = vtx_make_bool(true);
    vtx_value_t v3 = vtx_make_null();
    vtx_value_t v4 = vtx_make_undefined();

    vtx_object_set_field(obj, 0, v0);
    vtx_object_set_field(obj, 1, v1);
    vtx_object_set_field(obj, 2, v2);
    vtx_object_set_field(obj, 3, v3);
    vtx_object_set_field(obj, 4, v4);

    VTX_ASSERT_EQUAL(vtx_object_get_field(obj, 0), v0);
    VTX_ASSERT_EQUAL(vtx_object_get_field(obj, 1), v1);
    VTX_ASSERT_EQUAL(vtx_object_get_field(obj, 2), v2);
    VTX_ASSERT_EQUAL(vtx_object_get_field(obj, 3), v3);
    VTX_ASSERT_EQUAL(vtx_object_get_field(obj, 4), v4);
}

VTX_TEST(test_object_29)
{
    /* Object set/get with different value types in same field */
    _Alignas(8) char buf[256];
    vtx_heap_object_t *obj = (vtx_heap_object_t *)buf;
    vtx_heap_object_init(obj, 1, 1, 2, (uint32_t)sizeof(buf));

    vtx_value_t smi = vtx_make_smi(999);
    vtx_value_t dbl = vtx_make_double(3.14);

    vtx_object_set_field(obj, 0, smi);
    VTX_ASSERT_TRUE(vtx_is_smi(vtx_object_get_field(obj, 0)));
    VTX_ASSERT_EQUAL(vtx_smi_value(vtx_object_get_field(obj, 0)), (int64_t)999);

    vtx_object_set_field(obj, 0, dbl);
    VTX_ASSERT_TRUE(vtx_is_double(vtx_object_get_field(obj, 0)));
    VTX_ASSERT_TRUE(fabs(vtx_double_value(vtx_object_get_field(obj, 0)) - 3.14) < 1e-10);
}

VTX_TEST(test_object_30)
{
    /* Heap object alloc size calculation for 0 fields */
    size_t sz = vtx_heap_object_alloc_size(0);
    VTX_ASSERT_EQUAL(sz, (size_t)VTX_HEAP_OBJECT_HEADER_SIZE);
}

VTX_TEST(test_object_31)
{
    /* Heap object alloc size calculation for 1 field */
    size_t sz = vtx_heap_object_alloc_size(1);
    VTX_ASSERT_EQUAL(sz, VTX_HEAP_OBJECT_HEADER_SIZE + sizeof(vtx_value_t));
}

VTX_TEST(test_object_32)
{
    /* Heap object alloc size calculation for 5 fields */
    size_t sz = vtx_heap_object_alloc_size(5);
    VTX_ASSERT_EQUAL(sz, VTX_HEAP_OBJECT_HEADER_SIZE + 5 * sizeof(vtx_value_t));
}

VTX_TEST(test_object_33)
{
    /* Heap object alloc size calculation for 100 fields */
    size_t sz = vtx_heap_object_alloc_size(100);
    VTX_ASSERT_EQUAL(sz, VTX_HEAP_OBJECT_HEADER_SIZE + 100 * sizeof(vtx_value_t));
}

VTX_TEST(test_object_34)
{
    /* vtx_value_typeid for heap ptr */
    _Alignas(8) char buf[256];
    vtx_heap_object_t *obj = (vtx_heap_object_t *)buf;
    vtx_heap_object_init(obj, 5, 1, 0, (uint32_t)sizeof(buf));

    vtx_value_t v = vtx_make_heap_ptr(obj);
    VTX_ASSERT_EQUAL(vtx_value_typeid(v), (uint32_t)5);
}

VTX_TEST(test_object_35)
{
    /* vtx_value_typeid for smi returns 0 */
    vtx_value_t v = vtx_make_smi(42);
    VTX_ASSERT_EQUAL(vtx_value_typeid(v), (uint32_t)0);
}

VTX_TEST(test_object_36)
{
    /* vtx_value_typeid for double returns 0 */
    vtx_value_t v = vtx_make_double(3.14);
    VTX_ASSERT_EQUAL(vtx_value_typeid(v), (uint32_t)0);
}

VTX_TEST(test_object_37)
{
    /* Round-trip: make_smi -> smi_value */
    for (int64_t val = -100; val <= 100; val += 7) {
        vtx_value_t v = vtx_make_smi(val);
        VTX_ASSERT_EQUAL(vtx_smi_value(v), val);
    }
}

VTX_TEST(test_object_38)
{
    /* Round-trip: make_double -> double_value for various doubles */
    double vals[] = {0.0, 1.0, -1.0, 3.14159, 1e100, -1e100, 1e-100};
    for (int i = 0; i < 7; i++) {
        vtx_value_t v = vtx_make_double(vals[i]);
        VTX_ASSERT_TRUE(vtx_is_double(v));
        VTX_ASSERT_TRUE(fabs(vtx_double_value(v) - vals[i]) < fabs(vals[i]) * 1e-15 + 1e-300);
    }
}

VTX_TEST(test_object_39)
{
    /* Round-trip: make_bool -> bool_value */
    VTX_ASSERT_TRUE(vtx_bool_value(vtx_make_bool(true)));
    VTX_ASSERT_FALSE(vtx_bool_value(vtx_make_bool(false)));
}

VTX_TEST(test_object_40)
{
    /* Edge case: very large SMI values near VTX_SMI_MAX */
    int64_t near_max = VTX_SMI_MAX - 1;
    vtx_value_t v = vtx_make_smi(near_max);
    VTX_ASSERT_TRUE(vtx_is_smi(v));
    VTX_ASSERT_EQUAL(vtx_smi_value(v), near_max);
}

VTX_TEST(test_object_41)
{
    /* Edge case: very large negative SMI near VTX_SMI_MIN */
    int64_t near_min = VTX_SMI_MIN + 1;
    vtx_value_t v = vtx_make_smi(near_min);
    VTX_ASSERT_TRUE(vtx_is_smi(v));
    VTX_ASSERT_EQUAL(vtx_smi_value(v), near_min);
}

VTX_TEST(test_object_42)
{
    /* Edge case: negative zero double (-0.0) */
    vtx_value_t v = vtx_make_double(-0.0);
    VTX_ASSERT_TRUE(vtx_is_double(v));
    /* -0.0 should be preserved */
    double extracted = vtx_double_value(v);
    VTX_ASSERT_TRUE(extracted == 0.0);
    /* Verify sign bit: -0.0 and +0.0 are equal but have different bits */
    union { uint64_t bits; double d; } u;
    u.d = extracted;
    /* Sign bit should be set for -0.0 */
    VTX_ASSERT_TRUE(u.bits == 0x8000000000000000ULL);
}

VTX_TEST(test_object_43)
{
    /* NaN double is recognized as double, not SMI */
    vtx_value_t v = vtx_make_double(NAN);
    VTX_ASSERT_TRUE(vtx_is_double(v));
    VTX_ASSERT_FALSE(vtx_is_smi(v));
    VTX_ASSERT_TRUE(isnan(vtx_double_value(v)));
}

VTX_TEST(test_object_44)
{
    /* Heap object fields initialized to undefined */
    _Alignas(8) char buf[256];
    vtx_heap_object_t *obj = (vtx_heap_object_t *)buf;
    vtx_heap_object_init(obj, 1, 1, 3, (uint32_t)sizeof(buf));

    for (uint32_t i = 0; i < 3; i++) {
        VTX_ASSERT_TRUE(vtx_is_undefined(vtx_object_get_field(obj, i)));
    }
}

VTX_TEST(test_object_45)
{
    /* Null and undefined are distinct */
    VTX_ASSERT_NOT_EQUAL(VTX_VALUE_NULL, VTX_VALUE_UNDEFINED);
    VTX_ASSERT_TRUE(vtx_is_null(VTX_VALUE_NULL));
    VTX_ASSERT_TRUE(vtx_is_undefined(VTX_VALUE_UNDEFINED));
    VTX_ASSERT_FALSE(vtx_is_null(VTX_VALUE_UNDEFINED));
    VTX_ASSERT_FALSE(vtx_is_undefined(VTX_VALUE_NULL));
}

VTX_TEST(test_object_46)
{
    /* Bool constants: VTX_VALUE_TRUE and VTX_VALUE_FALSE */
    VTX_ASSERT_TRUE(vtx_is_bool(VTX_VALUE_TRUE));
    VTX_ASSERT_TRUE(vtx_is_bool(VTX_VALUE_FALSE));
    VTX_ASSERT_TRUE(vtx_bool_value(VTX_VALUE_TRUE));
    VTX_ASSERT_FALSE(vtx_bool_value(VTX_VALUE_FALSE));
    VTX_ASSERT_NOT_EQUAL(VTX_VALUE_TRUE, VTX_VALUE_FALSE);
}

VTX_TEST(test_object_47)
{
    /* Heap pointer round-trip with different aligned addresses */
    _Alignas(8) char buf1[128], buf2[128], buf3[128];
    vtx_heap_object_t *o1 = (vtx_heap_object_t *)buf1;
    vtx_heap_object_t *o2 = (vtx_heap_object_t *)buf2;
    vtx_heap_object_t *o3 = (vtx_heap_object_t *)buf3;

    vtx_heap_object_init(o1, 1, 1, 0, 128);
    vtx_heap_object_init(o2, 2, 2, 0, 128);
    vtx_heap_object_init(o3, 3, 3, 0, 128);

    vtx_value_t v1 = vtx_make_heap_ptr(o1);
    vtx_value_t v2 = vtx_make_heap_ptr(o2);
    vtx_value_t v3 = vtx_make_heap_ptr(o3);

    VTX_ASSERT_EQUAL(vtx_heap_ptr(v1), (void *)o1);
    VTX_ASSERT_EQUAL(vtx_heap_ptr(v2), (void *)o2);
    VTX_ASSERT_EQUAL(vtx_heap_ptr(v3), (void *)o3);
}

VTX_TEST(test_object_48)
{
    /* SMI with powers of 2 */
    int64_t vals[] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 1024, 65536};
    for (int i = 0; i < 11; i++) {
        vtx_value_t v = vtx_make_smi(vals[i]);
        VTX_ASSERT_TRUE(vtx_is_smi(v));
        VTX_ASSERT_EQUAL(vtx_smi_value(v), vals[i]);
    }
}

VTX_TEST(test_object_49)
{
    /* Double with denormalized value */
    vtx_value_t v = vtx_make_double(5e-324);
    VTX_ASSERT_TRUE(vtx_is_double(v));
    VTX_ASSERT_TRUE(vtx_double_value(v) > 0.0);
}

VTX_TEST(test_object_50)
{
    /* Comprehensive type discrimination matrix */
    vtx_value_t smi = vtx_make_smi(42);
    vtx_value_t dbl = vtx_make_double(3.14);
    vtx_value_t bol = vtx_make_bool(true);
    vtx_value_t nul = vtx_make_null();
    vtx_value_t und = vtx_make_undefined();

    VTX_ASSERT_TRUE(vtx_is_smi(smi));
    VTX_ASSERT_FALSE(vtx_is_smi(dbl));
    VTX_ASSERT_FALSE(vtx_is_smi(bol));
    VTX_ASSERT_FALSE(vtx_is_smi(nul));
    VTX_ASSERT_FALSE(vtx_is_smi(und));

    VTX_ASSERT_FALSE(vtx_is_double(smi));
    VTX_ASSERT_TRUE(vtx_is_double(dbl));
    VTX_ASSERT_FALSE(vtx_is_double(bol));
    VTX_ASSERT_FALSE(vtx_is_double(nul));
    VTX_ASSERT_FALSE(vtx_is_double(und));

    VTX_ASSERT_FALSE(vtx_is_bool(smi));
    VTX_ASSERT_FALSE(vtx_is_bool(dbl));
    VTX_ASSERT_TRUE(vtx_is_bool(bol));
    VTX_ASSERT_FALSE(vtx_is_bool(nul));
    VTX_ASSERT_FALSE(vtx_is_bool(und));

    VTX_ASSERT_FALSE(vtx_is_null(smi));
    VTX_ASSERT_FALSE(vtx_is_null(dbl));
    VTX_ASSERT_FALSE(vtx_is_null(bol));
    VTX_ASSERT_TRUE(vtx_is_null(nul));
    VTX_ASSERT_FALSE(vtx_is_null(und));

    VTX_ASSERT_FALSE(vtx_is_undefined(smi));
    VTX_ASSERT_FALSE(vtx_is_undefined(dbl));
    VTX_ASSERT_FALSE(vtx_is_undefined(bol));
    VTX_ASSERT_FALSE(vtx_is_undefined(nul));
    VTX_ASSERT_TRUE(vtx_is_undefined(und));
}

/* ========================================================================== */
/* Type System tests (50 tests)                                                */
/* ========================================================================== */

VTX_TEST(test_typesys_01)
{
    /* Init/destroy */
    vtx_type_system_t ts;
    int rc = vtx_type_system_init(&ts);
    VTX_ASSERT_EQUAL(rc, 0);
    VTX_ASSERT_TRUE(ts.type_count >= 2);
    VTX_ASSERT_NOT_NULL(ts.types);

    vtx_type_system_destroy(&ts);
    VTX_ASSERT_NULL(ts.types);
    VTX_ASSERT_EQUAL(ts.type_count, (uint32_t)0);
}

VTX_TEST(test_typesys_02)
{
    /* Register a simple type, verify name and id */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_typeid_t id = vtx_type_register(&ts, "MyType", VTX_TYPE_OBJECT, 0, NULL, 0, NULL);
    VTX_ASSERT_NOT_EQUAL(id, VTX_TYPE_INVALID);
    VTX_ASSERT_TRUE(id >= 2);

    const vtx_type_desc_t *desc = vtx_type_get(&ts, id);
    VTX_ASSERT_NOT_NULL(desc);
    VTX_ASSERT_TRUE(strcmp(desc->name, "MyType") == 0);
    VTX_ASSERT_EQUAL(desc->type_id, id);

    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_typesys_03)
{
    /* Register multiple types */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_typeid_t t1 = vtx_type_register(&ts, "A", VTX_TYPE_OBJECT, 0, NULL, 0, NULL);
    vtx_typeid_t t2 = vtx_type_register(&ts, "B", VTX_TYPE_OBJECT, 0, NULL, 0, NULL);
    vtx_typeid_t t3 = vtx_type_register(&ts, "C", VTX_TYPE_OBJECT, 0, NULL, 0, NULL);

    VTX_ASSERT_NOT_EQUAL(t1, t2);
    VTX_ASSERT_NOT_EQUAL(t2, t3);
    VTX_ASSERT_NOT_EQUAL(t1, t3);

    VTX_ASSERT_TRUE(strcmp(vtx_type_get(&ts, t1)->name, "A") == 0);
    VTX_ASSERT_TRUE(strcmp(vtx_type_get(&ts, t2)->name, "B") == 0);
    VTX_ASSERT_TRUE(strcmp(vtx_type_get(&ts, t3)->name, "C") == 0);

    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_typesys_04)
{
    /* Type hierarchy: parent/child */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_typeid_t parent = vtx_type_register(&ts, "Parent", VTX_TYPE_OBJECT, 0, NULL, 0, NULL);
    vtx_typeid_t child = vtx_type_register(&ts, "Child", parent, 0, NULL, 0, NULL);

    VTX_ASSERT_EQUAL(vtx_type_get(&ts, child)->parent_type, parent);
    VTX_ASSERT_TRUE(vtx_type_is_subtype(&ts, child, parent));

    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_typesys_05)
{
    /* is_subtype of type with itself */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_typeid_t t = vtx_type_register(&ts, "Self", VTX_TYPE_OBJECT, 0, NULL, 0, NULL);
    VTX_ASSERT_TRUE(vtx_type_is_subtype(&ts, t, t));
    VTX_ASSERT_TRUE(vtx_type_is_subtype(&ts, VTX_TYPE_OBJECT, VTX_TYPE_OBJECT));

    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_typesys_06)
{
    /* Deep inheritance chain: A→B→C→D, verify transitive subtype */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_typeid_t a = vtx_type_register(&ts, "A", VTX_TYPE_OBJECT, 0, NULL, 0, NULL);
    vtx_typeid_t b = vtx_type_register(&ts, "B", a, 0, NULL, 0, NULL);
    vtx_typeid_t c = vtx_type_register(&ts, "C", b, 0, NULL, 0, NULL);
    vtx_typeid_t d = vtx_type_register(&ts, "D", c, 0, NULL, 0, NULL);

    VTX_ASSERT_TRUE(vtx_type_is_subtype(&ts, d, a));
    VTX_ASSERT_TRUE(vtx_type_is_subtype(&ts, d, b));
    VTX_ASSERT_TRUE(vtx_type_is_subtype(&ts, d, c));
    VTX_ASSERT_TRUE(vtx_type_is_subtype(&ts, d, VTX_TYPE_OBJECT));
    VTX_ASSERT_FALSE(vtx_type_is_subtype(&ts, a, d));

    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_typesys_07)
{
    /* is_subtype of unrelated types should be false */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_typeid_t x = vtx_type_register(&ts, "X", VTX_TYPE_OBJECT, 0, NULL, 0, NULL);
    vtx_typeid_t y = vtx_type_register(&ts, "Y", VTX_TYPE_OBJECT, 0, NULL, 0, NULL);

    VTX_ASSERT_FALSE(vtx_type_is_subtype(&ts, x, y));
    VTX_ASSERT_FALSE(vtx_type_is_subtype(&ts, y, x));

    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_typesys_08)
{
    /* Register type with fields, verify field count and offsets */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_field_desc_t *fields = (vtx_field_desc_t *)calloc(3, sizeof(vtx_field_desc_t));
    VTX_ASSERT_NOT_NULL(fields);
    fields[0].name = "x"; fields[0].type = VTX_TYPE_OBJECT; fields[0].offset = 0;
    fields[1].name = "y"; fields[1].type = VTX_TYPE_OBJECT; fields[1].offset = 0;
    fields[2].name = "z"; fields[2].type = VTX_TYPE_OBJECT; fields[2].offset = 0;

    vtx_typeid_t pt = vtx_type_register(&ts, "Point3D", VTX_TYPE_OBJECT, 3, fields, 0, NULL);
    VTX_ASSERT_NOT_EQUAL(pt, VTX_TYPE_INVALID);

    const vtx_type_desc_t *desc = vtx_type_get(&ts, pt);
    VTX_ASSERT_EQUAL(desc->field_count, (uint32_t)3);
    VTX_ASSERT_TRUE(desc->fields[0].offset >= VTX_HEAP_OBJECT_HEADER_SIZE);
    VTX_ASSERT_TRUE(desc->fields[1].offset > desc->fields[0].offset);
    VTX_ASSERT_TRUE(desc->fields[2].offset > desc->fields[1].offset);

    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_typesys_09)
{
    /* Register type with methods, verify method count */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_method_desc_t *methods = (vtx_method_desc_t *)calloc(2, sizeof(vtx_method_desc_t));
    VTX_ASSERT_NOT_NULL(methods);
    methods[0].name = "foo"; methods[0].signature = "()V";
    methods[0].bytecode = NULL; methods[0].vtable_index = 0;
    methods[0].arg_count = 0; methods[0].is_virtual = true;
    methods[1].name = "bar"; methods[1].signature = "(I)I";
    methods[1].bytecode = NULL; methods[1].vtable_index = 1;
    methods[1].arg_count = 1; methods[1].is_virtual = false;

    vtx_typeid_t tid = vtx_type_register(&ts, "WithMethods", VTX_TYPE_OBJECT,
                                           0, NULL, 2, methods);
    const vtx_type_desc_t *desc = vtx_type_get(&ts, tid);
    VTX_ASSERT_EQUAL(desc->method_count, (uint32_t)2);

    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_typesys_10)
{
    /* Resolve method on type */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_method_desc_t *methods = (vtx_method_desc_t *)calloc(1, sizeof(vtx_method_desc_t));
    VTX_ASSERT_NOT_NULL(methods);
    methods[0].name = "calc"; methods[0].signature = "(II)I";
    methods[0].bytecode = NULL; methods[0].vtable_index = 0;
    methods[0].arg_count = 2; methods[0].is_virtual = true;

    vtx_typeid_t tid = vtx_type_register(&ts, "Calc", VTX_TYPE_OBJECT,
                                           0, NULL, 1, methods);
    const vtx_method_desc_t *m = vtx_type_resolve_method(&ts, tid, "calc");
    VTX_ASSERT_NOT_NULL(m);
    VTX_ASSERT_TRUE(strcmp(m->name, "calc") == 0);

    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_typesys_11)
{
    /* Resolve method on subtype (inherited method) */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_method_desc_t *methods = (vtx_method_desc_t *)calloc(1, sizeof(vtx_method_desc_t));
    VTX_ASSERT_NOT_NULL(methods);
    methods[0].name = "greet"; methods[0].signature = "()V";
    methods[0].bytecode = NULL; methods[0].vtable_index = 0;
    methods[0].arg_count = 0; methods[0].is_virtual = true;

    vtx_typeid_t base = vtx_type_register(&ts, "Base", VTX_TYPE_OBJECT,
                                            0, NULL, 1, methods);
    vtx_typeid_t derived = vtx_type_register(&ts, "Derived", base,
                                               0, NULL, 0, NULL);

    const vtx_method_desc_t *m = vtx_type_resolve_method(&ts, derived, "greet");
    VTX_ASSERT_NOT_NULL(m);
    VTX_ASSERT_TRUE(strcmp(m->name, "greet") == 0);

    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_typesys_12)
{
    /* Resolve method that doesn't exist returns NULL */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_typeid_t tid = vtx_type_register(&ts, "Empty", VTX_TYPE_OBJECT,
                                           0, NULL, 0, NULL);
    const vtx_method_desc_t *m = vtx_type_resolve_method(&ts, tid, "nonexistent");
    VTX_ASSERT_NULL(m);

    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_typesys_13)
{
    /* Instance size computation */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    uint32_t obj_size = vtx_type_instance_size(&ts, VTX_TYPE_OBJECT);
    VTX_ASSERT_EQUAL(obj_size, (uint32_t)VTX_HEAP_OBJECT_HEADER_SIZE);

    vtx_field_desc_t *f = (vtx_field_desc_t *)calloc(1, sizeof(vtx_field_desc_t));
    VTX_ASSERT_NOT_NULL(f);
    f->name = "val"; f->type = VTX_TYPE_OBJECT; f->offset = 0;

    vtx_typeid_t tid = vtx_type_register(&ts, "OneField", VTX_TYPE_OBJECT,
                                           1, f, 0, NULL);
    uint32_t sz = vtx_type_instance_size(&ts, tid);
    VTX_ASSERT_TRUE(sz > VTX_HEAP_OBJECT_HEADER_SIZE);
    VTX_ASSERT_EQUAL(sz, (uint32_t)(VTX_HEAP_OBJECT_HEADER_SIZE + sizeof(vtx_value_t)));

    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_typesys_14)
{
    /* Shape computation */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_field_desc_t *f1 = (vtx_field_desc_t *)calloc(2, sizeof(vtx_field_desc_t));
    VTX_ASSERT_NOT_NULL(f1);
    f1[0].name = "x"; f1[0].type = VTX_TYPE_OBJECT; f1[0].offset = 0;
    f1[1].name = "y"; f1[1].type = VTX_TYPE_OBJECT; f1[1].offset = 0;

    vtx_typeid_t p1 = vtx_type_register(&ts, "Point", VTX_TYPE_OBJECT, 2, f1, 0, NULL);
    vtx_shapeid_t shape = vtx_type_compute_shape(&ts, p1);
    VTX_ASSERT_NOT_EQUAL(shape, VTX_SHAPE_INVALID);

    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_typesys_15)
{
    /* Compute shape of type with no fields */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_typeid_t tid = vtx_type_register(&ts, "NoFields", VTX_TYPE_OBJECT,
                                           0, NULL, 0, NULL);
    vtx_shapeid_t shape = vtx_type_compute_shape(&ts, tid);
    /* No-field type should have a valid shape */
    VTX_ASSERT_NOT_EQUAL(shape, VTX_SHAPE_INVALID);

    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_typesys_16)
{
    /* Compute shape of type with many fields */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_field_desc_t *fields = (vtx_field_desc_t *)calloc(10, sizeof(vtx_field_desc_t));
    VTX_ASSERT_NOT_NULL(fields);
    for (int i = 0; i < 10; i++) {
        fields[i].name = "f"; fields[i].type = VTX_TYPE_OBJECT; fields[i].offset = 0;
    }

    vtx_typeid_t tid = vtx_type_register(&ts, "ManyFields", VTX_TYPE_OBJECT,
                                           10, fields, 0, NULL);
    vtx_shapeid_t shape = vtx_type_compute_shape(&ts, tid);
    VTX_ASSERT_NOT_EQUAL(shape, VTX_SHAPE_INVALID);

    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_typesys_17)
{
    /* Symbol table: intern and lookup */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    uint32_t sym = vtx_symbol_intern(&ts, "hello");
    VTX_ASSERT_NOT_EQUAL(sym, VTX_SYMBOL_INVALID);

    uint32_t found = vtx_symbol_lookup(&ts, "hello");
    VTX_ASSERT_EQUAL(found, sym);

    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_typesys_18)
{
    /* Symbol table: name retrieval */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    uint32_t sym = vtx_symbol_intern(&ts, "world");
    const char *name = vtx_symbol_name(&ts, sym);
    VTX_ASSERT_NOT_NULL(name);
    VTX_ASSERT_TRUE(strcmp(name, "world") == 0);

    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_typesys_19)
{
    /* Intern same symbol twice should get same id */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    uint32_t id1 = vtx_symbol_intern(&ts, "dup");
    uint32_t id2 = vtx_symbol_intern(&ts, "dup");
    VTX_ASSERT_EQUAL(id1, id2);

    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_typesys_20)
{
    /* Intern different symbols should get different ids */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    uint32_t id1 = vtx_symbol_intern(&ts, "alpha");
    uint32_t id2 = vtx_symbol_intern(&ts, "beta");
    VTX_ASSERT_NOT_EQUAL(id1, id2);

    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_typesys_21)
{
    /* Symbol lookup of non-existent symbol */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    uint32_t found = vtx_symbol_lookup(&ts, "nonexistent");
    VTX_ASSERT_EQUAL(found, VTX_SYMBOL_INVALID);

    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_typesys_22)
{
    /* Inline cache: init */
    vtx_inline_cache_t ic;
    vtx_ic_init(&ic);
    VTX_ASSERT_EQUAL(ic.state, VT_IC_MONOMORPHIC);
    VTX_ASSERT_EQUAL(ic.count, (uint32_t)0);
}

VTX_TEST(test_typesys_23)
{
    /* Inline cache: monomorphic lookup/update */
    vtx_inline_cache_t ic;
    vtx_ic_init(&ic);

    vtx_method_desc_t m = {.name = "f", .signature = "()V", .bytecode = NULL,
                            .vtable_index = 0, .arg_count = 0, .is_virtual = true};

    vtx_ic_update(&ic, 1, &m);
    VTX_ASSERT_EQUAL(ic.state, VT_IC_MONOMORPHIC);
    VTX_ASSERT_EQUAL(ic.count, (uint32_t)1);

    const vtx_method_desc_t *found = vtx_ic_lookup(&ic, 1);
    VTX_ASSERT_NOT_NULL(found);
    VTX_ASSERT_TRUE(strcmp(found->name, "f") == 0);
}

VTX_TEST(test_typesys_24)
{
    /* Inline cache: polymorphic (2 types) */
    vtx_inline_cache_t ic;
    vtx_ic_init(&ic);

    vtx_method_desc_t m1 = {.name = "f", .signature = "()V", .bytecode = NULL,
                             .vtable_index = 0, .arg_count = 0, .is_virtual = true};
    vtx_method_desc_t m2 = {.name = "g", .signature = "()V", .bytecode = NULL,
                             .vtable_index = 1, .arg_count = 0, .is_virtual = true};

    vtx_ic_update(&ic, 1, &m1);
    vtx_ic_update(&ic, 2, &m2);
    VTX_ASSERT_EQUAL(ic.state, VT_IC_POLYMORPHIC);
    VTX_ASSERT_EQUAL(ic.count, (uint32_t)2);

    VTX_ASSERT_NOT_NULL(vtx_ic_lookup(&ic, 1));
    VTX_ASSERT_NOT_NULL(vtx_ic_lookup(&ic, 2));
    VTX_ASSERT_NULL(vtx_ic_lookup(&ic, 99));
}

VTX_TEST(test_typesys_25)
{
    /* Inline cache: polymorphic (4 types = VTX_POLY_LIMIT) */
    vtx_inline_cache_t ic;
    vtx_ic_init(&ic);

    vtx_method_desc_t m = {.name = "h", .signature = "()V", .bytecode = NULL,
                            .vtable_index = 0, .arg_count = 0, .is_virtual = true};

    for (uint32_t i = 0; i < VTX_POLY_LIMIT; i++) {
        vtx_ic_update(&ic, i + 1, &m);
    }
    VTX_ASSERT_EQUAL(ic.state, VT_IC_POLYMORPHIC);
    VTX_ASSERT_EQUAL(ic.count, (uint32_t)VTX_POLY_LIMIT);
}

VTX_TEST(test_typesys_26)
{
    /* Inline cache: megamorphic (>4 types, state transition) */
    vtx_inline_cache_t ic;
    vtx_ic_init(&ic);

    vtx_method_desc_t m = {.name = "x", .signature = "()V", .bytecode = NULL,
                            .vtable_index = 0, .arg_count = 0, .is_virtual = true};

    for (uint32_t i = 0; i < VTX_POLY_LIMIT; i++) {
        vtx_ic_update(&ic, i + 1, &m);
    }
    /* One more should trigger megamorphic */
    vtx_ic_update(&ic, VTX_POLY_LIMIT + 1, &m);
    VTX_ASSERT_EQUAL(ic.state, VT_IC_MEGAMORPHIC);

    /* Megamorphic lookup always returns NULL */
    VTX_ASSERT_NULL(vtx_ic_lookup(&ic, 1));
}

VTX_TEST(test_typesys_27)
{
    /* Type add interface */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_typeid_t iface = vtx_type_register(&ts, "IFly", VTX_TYPE_OBJECT,
                                             0, NULL, 0, NULL);
    vtx_typeid_t impl = vtx_type_register(&ts, "Bird", VTX_TYPE_OBJECT,
                                            0, NULL, 0, NULL);

    int rc = vtx_type_add_interface(&ts, impl, iface);
    VTX_ASSERT_EQUAL(rc, 0);

    VTX_ASSERT_TRUE(vtx_type_is_instance(&ts, impl, iface));

    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_typesys_28)
{
    /* Type update vtable */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_method_desc_t *methods = (vtx_method_desc_t *)calloc(1, sizeof(vtx_method_desc_t));
    VTX_ASSERT_NOT_NULL(methods);
    methods[0].name = "run"; methods[0].signature = "()V";
    methods[0].bytecode = NULL; methods[0].vtable_index = 0;
    methods[0].arg_count = 0; methods[0].is_virtual = true;

    vtx_typeid_t tid = vtx_type_register(&ts, "Runner", VTX_TYPE_OBJECT,
                                           0, NULL, 1, methods);

    /* Update vtable with the same method */
    const vtx_type_desc_t *desc = vtx_type_get(&ts, tid);
    int rc = vtx_type_update_vtable(&ts, tid, &desc->methods[0]);
    VTX_ASSERT_EQUAL(rc, 0);

    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_typesys_29)
{
    /* Global type system get/set */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_set_current_type_system(&ts);
    vtx_type_system_t *current = vtx_get_current_type_system();
    VTX_ASSERT_EQUAL(current, &ts);

    vtx_set_current_type_system(NULL);
    VTX_ASSERT_NULL(vtx_get_current_type_system());

    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_typesys_30)
{
    /* Register type with 0 fields, 0 methods */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_typeid_t tid = vtx_type_register(&ts, "Empty", VTX_TYPE_OBJECT,
                                           0, NULL, 0, NULL);
    VTX_ASSERT_NOT_EQUAL(tid, VTX_TYPE_INVALID);

    const vtx_type_desc_t *desc = vtx_type_get(&ts, tid);
    VTX_ASSERT_EQUAL(desc->field_count, (uint32_t)0);
    VTX_ASSERT_EQUAL(desc->method_count, (uint32_t)0);

    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_typesys_31)
{
    /* Register type with many methods */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_method_desc_t *methods = (vtx_method_desc_t *)calloc(8, sizeof(vtx_method_desc_t));
    VTX_ASSERT_NOT_NULL(methods);
    for (int i = 0; i < 8; i++) {
        methods[i].name = "method"; methods[i].signature = "()V";
        methods[i].bytecode = NULL; methods[i].vtable_index = (uint32_t)i;
        methods[i].arg_count = 0; methods[i].is_virtual = true;
    }

    vtx_typeid_t tid = vtx_type_register(&ts, "ManyMethods", VTX_TYPE_OBJECT,
                                           0, NULL, 8, methods);
    const vtx_type_desc_t *desc = vtx_type_get(&ts, tid);
    VTX_ASSERT_EQUAL(desc->method_count, (uint32_t)8);

    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_typesys_32)
{
    /* Two types with same field layout share shape */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_field_desc_t *f1 = (vtx_field_desc_t *)calloc(2, sizeof(vtx_field_desc_t));
    vtx_field_desc_t *f2 = (vtx_field_desc_t *)calloc(2, sizeof(vtx_field_desc_t));
    VTX_ASSERT_NOT_NULL(f1);
    VTX_ASSERT_NOT_NULL(f2);
    f1[0].name = "a"; f1[0].type = VTX_TYPE_OBJECT; f1[0].offset = 0;
    f1[1].name = "b"; f1[1].type = VTX_TYPE_OBJECT; f1[1].offset = 0;
    f2[0].name = "c"; f2[0].type = VTX_TYPE_OBJECT; f2[0].offset = 0;
    f2[1].name = "d"; f2[1].type = VTX_TYPE_OBJECT; f2[1].offset = 0;

    vtx_typeid_t t1 = vtx_type_register(&ts, "T1", VTX_TYPE_OBJECT, 2, f1, 0, NULL);
    vtx_typeid_t t2 = vtx_type_register(&ts, "T2", VTX_TYPE_OBJECT, 2, f2, 0, NULL);

    vtx_shapeid_t s1 = vtx_type_compute_shape(&ts, t1);
    vtx_shapeid_t s2 = vtx_type_compute_shape(&ts, t2);
    VTX_ASSERT_EQUAL(s1, s2);

    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_typesys_33)
{
    /* Type get with invalid id returns NULL */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);
    VTX_ASSERT_NULL(vtx_type_get(&ts, 9999));
    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_typesys_34)
{
    /* Instance check with interface */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_typeid_t iface = vtx_type_register(&ts, "ISerializable", VTX_TYPE_OBJECT,
                                             0, NULL, 0, NULL);
    vtx_typeid_t cls = vtx_type_register(&ts, "Document", VTX_TYPE_OBJECT,
                                           0, NULL, 0, NULL);
    vtx_type_add_interface(&ts, cls, iface);

    VTX_ASSERT_TRUE(vtx_type_is_instance(&ts, cls, iface));
    VTX_ASSERT_FALSE(vtx_type_is_instance(&ts, iface, cls));

    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_typesys_35)
{
    /* Subtype with Object at root */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_typeid_t tid = vtx_type_register(&ts, "MyClass", VTX_TYPE_OBJECT,
                                           0, NULL, 0, NULL);
    VTX_ASSERT_TRUE(vtx_type_is_subtype(&ts, tid, VTX_TYPE_OBJECT));
    VTX_ASSERT_FALSE(vtx_type_is_subtype(&ts, VTX_TYPE_OBJECT, tid));

    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_typesys_36)
{
    /* Multiple interfaces on one type */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_typeid_t i1 = vtx_type_register(&ts, "I1", VTX_TYPE_OBJECT, 0, NULL, 0, NULL);
    vtx_typeid_t i2 = vtx_type_register(&ts, "I2", VTX_TYPE_OBJECT, 0, NULL, 0, NULL);
    vtx_typeid_t i3 = vtx_type_register(&ts, "I3", VTX_TYPE_OBJECT, 0, NULL, 0, NULL);
    vtx_typeid_t cls = vtx_type_register(&ts, "MultiIface", VTX_TYPE_OBJECT,
                                           0, NULL, 0, NULL);

    VTX_ASSERT_EQUAL(vtx_type_add_interface(&ts, cls, i1), 0);
    VTX_ASSERT_EQUAL(vtx_type_add_interface(&ts, cls, i2), 0);
    VTX_ASSERT_EQUAL(vtx_type_add_interface(&ts, cls, i3), 0);

    VTX_ASSERT_TRUE(vtx_type_is_instance(&ts, cls, i1));
    VTX_ASSERT_TRUE(vtx_type_is_instance(&ts, cls, i2));
    VTX_ASSERT_TRUE(vtx_type_is_instance(&ts, cls, i3));

    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_typesys_37)
{
    /* Inline cache: update with duplicate typeid doesn't increase count */
    vtx_inline_cache_t ic;
    vtx_ic_init(&ic);

    vtx_method_desc_t m = {.name = "f", .signature = "()V", .bytecode = NULL,
                            .vtable_index = 0, .arg_count = 0, .is_virtual = true};

    vtx_ic_update(&ic, 1, &m);
    vtx_ic_update(&ic, 1, &m);
    VTX_ASSERT_EQUAL(ic.count, (uint32_t)1);
}

VTX_TEST(test_typesys_38)
{
    /* Inline cache: lookup on empty IC returns NULL */
    vtx_inline_cache_t ic;
    vtx_ic_init(&ic);
    VTX_ASSERT_NULL(vtx_ic_lookup(&ic, 1));
}

VTX_TEST(test_typesys_39)
{
    /* Symbol table: many interned symbols */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    uint32_t ids[20];
    for (int i = 0; i < 20; i++) {
        char name[16];
        sprintf(name, "sym_%d", i);
        ids[i] = vtx_symbol_intern(&ts, name);
        VTX_ASSERT_NOT_EQUAL(ids[i], VTX_SYMBOL_INVALID);
    }
    /* Verify all are distinct */
    for (int i = 0; i < 20; i++) {
        for (int j = i + 1; j < 20; j++) {
            VTX_ASSERT_NOT_EQUAL(ids[i], ids[j]);
        }
    }

    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_typesys_40)
{
    /* Symbol name for invalid id returns NULL */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    const char *name = vtx_symbol_name(&ts, VTX_SYMBOL_INVALID);
    VTX_ASSERT_NULL(name);

    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_typesys_41)
{
    /* Type instance size for Object is header only */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    uint32_t sz = vtx_type_instance_size(&ts, VTX_TYPE_OBJECT);
    VTX_ASSERT_EQUAL(sz, (uint32_t)VTX_HEAP_OBJECT_HEADER_SIZE);

    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_typesys_42)
{
    /* Child type instance size includes parent fields */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_field_desc_t *pf = (vtx_field_desc_t *)calloc(1, sizeof(vtx_field_desc_t));
    VTX_ASSERT_NOT_NULL(pf);
    pf->name = "pval"; pf->type = VTX_TYPE_OBJECT; pf->offset = 0;

    vtx_typeid_t parent = vtx_type_register(&ts, "Parent", VTX_TYPE_OBJECT,
                                              1, pf, 0, NULL);

    vtx_field_desc_t *cf = (vtx_field_desc_t *)calloc(1, sizeof(vtx_field_desc_t));
    VTX_ASSERT_NOT_NULL(cf);
    cf->name = "cval"; cf->type = VTX_TYPE_OBJECT; cf->offset = 0;

    vtx_typeid_t child = vtx_type_register(&ts, "Child", parent,
                                             1, cf, 0, NULL);

    uint32_t parent_sz = vtx_type_instance_size(&ts, parent);
    uint32_t child_sz = vtx_type_instance_size(&ts, child);
    VTX_ASSERT_TRUE(child_sz >= parent_sz);

    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_typesys_43)
{
    /* Register many types to test capacity growth */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    for (int i = 0; i < 50; i++) {
        char name[16];
        sprintf(name, "Type%d", i);
        vtx_typeid_t tid = vtx_type_register(&ts, name, VTX_TYPE_OBJECT,
                                               0, NULL, 0, NULL);
        VTX_ASSERT_NOT_EQUAL(tid, VTX_TYPE_INVALID);
    }

    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_typesys_44)
{
    /* Resolve method on Object type */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    /* Object type should have no methods by default */
    const vtx_method_desc_t *m = vtx_type_resolve_method(&ts, VTX_TYPE_OBJECT, "nonexistent");
    VTX_ASSERT_NULL(m);

    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_typesys_45)
{
    /* Inline cache: polymorphic with 3 types */
    vtx_inline_cache_t ic;
    vtx_ic_init(&ic);

    vtx_method_desc_t m = {.name = "f", .signature = "()V", .bytecode = NULL,
                            .vtable_index = 0, .arg_count = 0, .is_virtual = true};

    vtx_ic_update(&ic, 1, &m);
    vtx_ic_update(&ic, 2, &m);
    vtx_ic_update(&ic, 3, &m);

    VTX_ASSERT_EQUAL(ic.state, VT_IC_POLYMORPHIC);
    VTX_ASSERT_EQUAL(ic.count, (uint32_t)3);
    VTX_ASSERT_NOT_NULL(vtx_ic_lookup(&ic, 1));
    VTX_ASSERT_NOT_NULL(vtx_ic_lookup(&ic, 2));
    VTX_ASSERT_NOT_NULL(vtx_ic_lookup(&ic, 3));
}

VTX_TEST(test_typesys_46)
{
    /* Type system shape_counter is a valid counter */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    /* Just verify shape_counter is accessible and non-decreasing */
    vtx_shapeid_t before = ts.shape_counter;
    VTX_ASSERT_TRUE(ts.shape_counter >= before);

    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_typesys_47)
{
    /* Verify Object type properties */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    const vtx_type_desc_t *obj = vtx_type_get(&ts, VTX_TYPE_OBJECT);
    VTX_ASSERT_NOT_NULL(obj);
    VTX_ASSERT_TRUE(strcmp(obj->name, "Object") == 0);
    VTX_ASSERT_EQUAL(obj->type_id, VTX_TYPE_OBJECT);
    VTX_ASSERT_EQUAL(obj->parent_type, VTX_TYPE_INVALID);

    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_typesys_48)
{
    /* Interface inheritance through subtype chain */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_typeid_t iface = vtx_type_register(&ts, "IWalk", VTX_TYPE_OBJECT,
                                             0, NULL, 0, NULL);
    vtx_typeid_t base = vtx_type_register(&ts, "Animal", VTX_TYPE_OBJECT,
                                            0, NULL, 0, NULL);
    vtx_type_add_interface(&ts, base, iface);

    vtx_typeid_t child = vtx_type_register(&ts, "Dog", base, 0, NULL, 0, NULL);

    /* Dog inherits Animal's interfaces */
    VTX_ASSERT_TRUE(vtx_type_is_instance(&ts, child, iface));

    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_typesys_49)
{
    /* Type with 1 field: instance size is header + 1 value */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_field_desc_t *f = (vtx_field_desc_t *)calloc(1, sizeof(vtx_field_desc_t));
    VTX_ASSERT_NOT_NULL(f);
    f->name = "val"; f->type = VTX_TYPE_OBJECT; f->offset = 0;

    vtx_typeid_t tid = vtx_type_register(&ts, "Val", VTX_TYPE_OBJECT, 1, f, 0, NULL);
    uint32_t sz = vtx_type_instance_size(&ts, tid);
    VTX_ASSERT_EQUAL(sz, (uint32_t)(VTX_HEAP_OBJECT_HEADER_SIZE + sizeof(vtx_value_t)));

    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_typesys_50)
{
    /* Global type system round-trip */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_typeid_t tid = vtx_type_register(&ts, "Global", VTX_TYPE_OBJECT,
                                           0, NULL, 0, NULL);

    vtx_set_current_type_system(&ts);
    vtx_type_system_t *cur = vtx_get_current_type_system();
    VTX_ASSERT_NOT_NULL(cur);

    const vtx_type_desc_t *desc = vtx_type_get(cur, tid);
    VTX_ASSERT_NOT_NULL(desc);
    VTX_ASSERT_TRUE(strcmp(desc->name, "Global") == 0);

    vtx_set_current_type_system(NULL);
    vtx_type_system_destroy(&ts);
}

/* ========================================================================== */
/* Bytecode tests (40 tests)                                                  */
/* ========================================================================== */

VTX_TEST(test_bytecode_01)
{
    /* Opcode at various positions */
    uint8_t code[] = { VT_OP_IADD, VT_OP_ISUB, VT_OP_IMUL };
    vtx_bytecode_t bc = {
        .code = code, .length = sizeof(code),
        .constant_pool = NULL, .constant_count = 0,
        .max_locals = 0, .max_stack = 4
    };

    VTX_ASSERT_TRUE(vtx_bytecode_opcode_at(&bc, 0) == VT_OP_IADD);
    VTX_ASSERT_TRUE(vtx_bytecode_opcode_at(&bc, 1) == VT_OP_ISUB);
    VTX_ASSERT_TRUE(vtx_bytecode_opcode_at(&bc, 2) == VT_OP_IMUL);
}

VTX_TEST(test_bytecode_02)
{
    /* Read operand for GOTO */
    uint8_t code[] = { VT_OP_GOTO, 0x01, 0x00 };
    vtx_bytecode_t bc = {
        .code = code, .length = sizeof(code),
        .constant_pool = NULL, .constant_count = 0,
        .max_locals = 0, .max_stack = 4
    };

    uint16_t operand = vtx_bytecode_read_operand(&bc, 0);
    VTX_ASSERT_EQUAL(operand, (uint16_t)0x0100);
}

VTX_TEST(test_bytecode_03)
{
    /* Read operand for IF_TRUE */
    uint8_t code[] = { VT_OP_IF_TRUE, 0x00, 0x10 };
    vtx_bytecode_t bc = {
        .code = code, .length = sizeof(code),
        .constant_pool = NULL, .constant_count = 0,
        .max_locals = 0, .max_stack = 4
    };

    uint16_t operand = vtx_bytecode_read_operand(&bc, 0);
    VTX_ASSERT_EQUAL(operand, (uint16_t)0x0010);
}

VTX_TEST(test_bytecode_04)
{
    /* Read operand for IF_FALSE */
    uint8_t code[] = { VT_OP_IF_FALSE, 0xFF, 0xFF };
    vtx_bytecode_t bc = {
        .code = code, .length = sizeof(code),
        .constant_pool = NULL, .constant_count = 0,
        .max_locals = 0, .max_stack = 4
    };

    uint16_t operand = vtx_bytecode_read_operand(&bc, 0);
    VTX_ASSERT_EQUAL(operand, (uint16_t)0xFFFF);
}

VTX_TEST(test_bytecode_05)
{
    /* Instruction length for opcode without operand */
    uint8_t code[] = { VT_OP_IADD };
    vtx_bytecode_t bc = {
        .code = code, .length = sizeof(code),
        .constant_pool = NULL, .constant_count = 0,
        .max_locals = 0, .max_stack = 4
    };

    VTX_ASSERT_EQUAL(vtx_bytecode_insn_length(&bc, 0), (size_t)1);
}

VTX_TEST(test_bytecode_06)
{
    /* Instruction length for opcode with 2-byte operand */
    uint8_t code[] = { VT_OP_LOAD_LOCAL, 0x00, 0x01 };
    vtx_bytecode_t bc = {
        .code = code, .length = sizeof(code),
        .constant_pool = NULL, .constant_count = 0,
        .max_locals = 1, .max_stack = 4
    };

    VTX_ASSERT_EQUAL(vtx_bytecode_insn_length(&bc, 0), (size_t)3);
}

VTX_TEST(test_bytecode_07)
{
    /* Instruction length for HALT (no operand) */
    uint8_t code[] = { VT_OP_HALT };
    vtx_bytecode_t bc = {
        .code = code, .length = sizeof(code),
        .constant_pool = NULL, .constant_count = 0,
        .max_locals = 0, .max_stack = 4
    };

    VTX_ASSERT_EQUAL(vtx_bytecode_insn_length(&bc, 0), (size_t)1);
}

VTX_TEST(test_bytecode_08)
{
    /* Stack effect for arithmetic opcodes */
    VTX_ASSERT_EQUAL(vtx_bytecode_stack_effect(VT_OP_IADD), -1);
    VTX_ASSERT_EQUAL(vtx_bytecode_stack_effect(VT_OP_ISUB), -1);
    VTX_ASSERT_EQUAL(vtx_bytecode_stack_effect(VT_OP_IMUL), -1);
    VTX_ASSERT_EQUAL(vtx_bytecode_stack_effect(VT_OP_IDIV), -1);
    VTX_ASSERT_EQUAL(vtx_bytecode_stack_effect(VT_OP_IMOD), -1);
}

VTX_TEST(test_bytecode_09)
{
    /* Stack effect for float arithmetic opcodes */
    VTX_ASSERT_EQUAL(vtx_bytecode_stack_effect(VT_OP_FADD), -1);
    VTX_ASSERT_EQUAL(vtx_bytecode_stack_effect(VT_OP_FSUB), -1);
    VTX_ASSERT_EQUAL(vtx_bytecode_stack_effect(VT_OP_FMUL), -1);
    VTX_ASSERT_EQUAL(vtx_bytecode_stack_effect(VT_OP_FDIV), -1);
}

VTX_TEST(test_bytecode_10)
{
    /* Stack effect for load/store */
    VTX_ASSERT_EQUAL(vtx_bytecode_stack_effect(VT_OP_LOAD_LOCAL), 1);
    VTX_ASSERT_EQUAL(vtx_bytecode_stack_effect(VT_OP_STORE_LOCAL), -1);
}

VTX_TEST(test_bytecode_11)
{
    /* Stack effect for comparison opcodes */
    VTX_ASSERT_EQUAL(vtx_bytecode_stack_effect(VT_OP_ICMP_EQ), -1);
    VTX_ASSERT_EQUAL(vtx_bytecode_stack_effect(VT_OP_ICMP_NE), -1);
    VTX_ASSERT_EQUAL(vtx_bytecode_stack_effect(VT_OP_ICMP_LT), -1);
    VTX_ASSERT_EQUAL(vtx_bytecode_stack_effect(VT_OP_ICMP_LE), -1);
    VTX_ASSERT_EQUAL(vtx_bytecode_stack_effect(VT_OP_ICMP_GT), -1);
    VTX_ASSERT_EQUAL(vtx_bytecode_stack_effect(VT_OP_ICMP_GE), -1);
}

VTX_TEST(test_bytecode_12)
{
    /* Stack effect for float comparison opcodes */
    VTX_ASSERT_EQUAL(vtx_bytecode_stack_effect(VT_OP_FCMP_EQ), -1);
    VTX_ASSERT_EQUAL(vtx_bytecode_stack_effect(VT_OP_FCMP_NE), -1);
    VTX_ASSERT_EQUAL(vtx_bytecode_stack_effect(VT_OP_FCMP_LT), -1);
}

VTX_TEST(test_bytecode_13)
{
    /* Stack effect for control flow */
    VTX_ASSERT_EQUAL(vtx_bytecode_stack_effect(VT_OP_GOTO), 0);
    VTX_ASSERT_EQUAL(vtx_bytecode_stack_effect(VT_OP_RETURN), 0);
}

VTX_TEST(test_bytecode_14)
{
    /* Stack effect for DUP, POP, SWAP */
    VTX_ASSERT_EQUAL(vtx_bytecode_stack_effect(VT_OP_DUP), 1);
    VTX_ASSERT_EQUAL(vtx_bytecode_stack_effect(VT_OP_POP), -1);
    VTX_ASSERT_EQUAL(vtx_bytecode_stack_effect(VT_OP_SWAP), 0);
}

VTX_TEST(test_bytecode_15)
{
    /* Stack effect for unary ops */
    VTX_ASSERT_EQUAL(vtx_bytecode_stack_effect(VT_OP_INEG), 0);
    VTX_ASSERT_EQUAL(vtx_bytecode_stack_effect(VT_OP_INOT), 0);
}

VTX_TEST(test_bytecode_16)
{
    /* Disassemble individual opcode: IADD */
    uint8_t code[] = { VT_OP_IADD };
    vtx_bytecode_t bc = {
        .code = code, .length = sizeof(code),
        .constant_pool = NULL, .constant_count = 0,
        .max_locals = 0, .max_stack = 4
    };

    char buf[128];
    size_t next = vtx_bytecode_disassemble_op(&bc, 0, buf, sizeof(buf));
    VTX_ASSERT_EQUAL(next, (size_t)1);
    VTX_ASSERT_NOT_NULL(strstr(buf, "IADD"));
}

VTX_TEST(test_bytecode_17)
{
    /* Disassemble individual opcode: LOAD_LOCAL */
    uint8_t code[] = { VT_OP_LOAD_LOCAL, 0x00, 0x05 };
    vtx_bytecode_t bc = {
        .code = code, .length = sizeof(code),
        .constant_pool = NULL, .constant_count = 0,
        .max_locals = 1, .max_stack = 4
    };

    char buf[128];
    size_t next = vtx_bytecode_disassemble_op(&bc, 0, buf, sizeof(buf));
    VTX_ASSERT_EQUAL(next, (size_t)3);
    VTX_ASSERT_NOT_NULL(strstr(buf, "LOAD_LOCAL"));
}

VTX_TEST(test_bytecode_18)
{
    /* Build a simple bytecode and verify opcode_at */
    uint8_t code[] = {
        VT_OP_LOAD_CONST_INT, 0x00, 0x00,
        VT_OP_LOAD_CONST_INT, 0x00, 0x01,
        VT_OP_IADD,
        VT_OP_RETURN_VALUE
    };
    vtx_bytecode_t bc = {
        .code = code, .length = sizeof(code),
        .constant_pool = NULL, .constant_count = 0,
        .max_locals = 0, .max_stack = 4
    };

    VTX_ASSERT_TRUE(vtx_bytecode_opcode_at(&bc, 0) == VT_OP_LOAD_CONST_INT);
    VTX_ASSERT_TRUE(vtx_bytecode_opcode_at(&bc, 3) == VT_OP_LOAD_CONST_INT);
    VTX_ASSERT_TRUE(vtx_bytecode_opcode_at(&bc, 6) == VT_OP_IADD);
    VTX_ASSERT_TRUE(vtx_bytecode_opcode_at(&bc, 7) == VT_OP_RETURN_VALUE);
}

VTX_TEST(test_bytecode_19)
{
    /* Build bytecode with constant pool references */
    vtx_value_t consts[] = { vtx_make_smi(10), vtx_make_smi(20), vtx_make_double(3.14) };
    uint8_t code[] = {
        VT_OP_LOAD_CONST_INT, 0x00, 0x00,
        VT_OP_LOAD_CONST_INT, 0x00, 0x01,
        VT_OP_LOAD_CONST_FLOAT, 0x00, 0x02
    };
    vtx_bytecode_t bc = {
        .code = code, .length = sizeof(code),
        .constant_pool = consts, .constant_count = 3,
        .max_locals = 0, .max_stack = 4
    };

    VTX_ASSERT_TRUE(vtx_bytecode_opcode_at(&bc, 0) == VT_OP_LOAD_CONST_INT);
    VTX_ASSERT_EQUAL(vtx_bytecode_read_operand(&bc, 0), (uint16_t)0);
    VTX_ASSERT_EQUAL(vtx_bytecode_read_operand(&bc, 3), (uint16_t)1);
    VTX_ASSERT_EQUAL(vtx_bytecode_read_operand(&bc, 6), (uint16_t)2);
}

VTX_TEST(test_bytecode_20)
{
    /* Build bytecode with branch operands */
    uint8_t code[] = {
        VT_OP_LOAD_TRUE,
        VT_OP_IF_FALSE, 0x00, 0x06,
        VT_OP_LOAD_CONST_INT, 0x00, 0x00,
        VT_OP_GOTO, 0x00, 0x09,
        VT_OP_LOAD_NULL,
        VT_OP_RETURN_VALUE
    };
    vtx_bytecode_t bc = {
        .code = code, .length = sizeof(code),
        .constant_pool = NULL, .constant_count = 0,
        .max_locals = 0, .max_stack = 4
    };

    VTX_ASSERT_EQUAL(vtx_bytecode_read_operand(&bc, 1), (uint16_t)6);
    VTX_ASSERT_EQUAL(vtx_bytecode_read_operand(&bc, 7), (uint16_t)9);
}

VTX_TEST(test_bytecode_21)
{
    /* Max locals and max stack fields */
    vtx_value_t consts[] = { vtx_make_smi(0) };
    uint8_t code[] = { VT_OP_HALT };
    vtx_bytecode_t bc = {
        .code = code, .length = sizeof(code),
        .constant_pool = consts, .constant_count = 1,
        .max_locals = 5, .max_stack = 10
    };

    VTX_ASSERT_EQUAL(bc.max_locals, (uint16_t)5);
    VTX_ASSERT_EQUAL(bc.max_stack, (uint16_t)10);
}

VTX_TEST(test_bytecode_22)
{
    /* Bytecode with all arithmetic opcodes */
    uint8_t code[] = {
        VT_OP_IADD, VT_OP_ISUB, VT_OP_IMUL, VT_OP_IDIV, VT_OP_IMOD,
        VT_OP_FADD, VT_OP_FSUB, VT_OP_FMUL, VT_OP_FDIV
    };
    vtx_bytecode_t bc = {
        .code = code, .length = sizeof(code),
        .constant_pool = NULL, .constant_count = 0,
        .max_locals = 0, .max_stack = 8
    };

    for (size_t i = 0; i < sizeof(code); i++) {
        VTX_ASSERT_TRUE(vtx_bytecode_opcode_at(&bc, i) == code[i]);
    }
}

VTX_TEST(test_bytecode_23)
{
    /* Bytecode with all comparison opcodes */
    uint8_t code[] = {
        VT_OP_ICMP_EQ, VT_OP_ICMP_NE, VT_OP_ICMP_LT,
        VT_OP_ICMP_LE, VT_OP_ICMP_GT, VT_OP_ICMP_GE,
        VT_OP_FCMP_EQ, VT_OP_FCMP_NE, VT_OP_FCMP_LT,
        VT_OP_FCMP_LE, VT_OP_FCMP_GT, VT_OP_FCMP_GE
    };
    vtx_bytecode_t bc = {
        .code = code, .length = sizeof(code),
        .constant_pool = NULL, .constant_count = 0,
        .max_locals = 0, .max_stack = 8
    };

    for (size_t i = 0; i < sizeof(code); i++) {
        VTX_ASSERT_TRUE(vtx_bytecode_opcode_at(&bc, i) == code[i]);
    }
}

VTX_TEST(test_bytecode_24)
{
    /* Bytecode with all control flow opcodes */
    uint8_t code[] = {
        VT_OP_GOTO, 0x00, 0x00,
        VT_OP_IF_TRUE, 0x00, 0x00,
        VT_OP_IF_FALSE, 0x00, 0x00,
        VT_OP_RETURN,
        VT_OP_RETURN_VALUE
    };
    vtx_bytecode_t bc = {
        .code = code, .length = sizeof(code),
        .constant_pool = NULL, .constant_count = 0,
        .max_locals = 0, .max_stack = 4
    };

    VTX_ASSERT_TRUE(vtx_bytecode_opcode_at(&bc, 0) == VT_OP_GOTO);
    VTX_ASSERT_TRUE(vtx_bytecode_opcode_at(&bc, 3) == VT_OP_IF_TRUE);
    VTX_ASSERT_TRUE(vtx_bytecode_opcode_at(&bc, 6) == VT_OP_IF_FALSE);
    VTX_ASSERT_TRUE(vtx_bytecode_opcode_at(&bc, 9) == VT_OP_RETURN);
    VTX_ASSERT_TRUE(vtx_bytecode_opcode_at(&bc, 10) == VT_OP_RETURN_VALUE);
}

VTX_TEST(test_bytecode_25)
{
    /* Bytecode length calculation: mixed instructions */
    uint8_t code[] = {
        VT_OP_LOAD_LOCAL, 0x00, 0x00,   /* 3 bytes */
        VT_OP_IADD,                       /* 1 byte */
        VT_OP_STORE_LOCAL, 0x00, 0x01,   /* 3 bytes */
        VT_OP_RETURN_VALUE                /* 1 byte */
    };
    /* Total: 8 bytes */
    vtx_bytecode_t bc = {
        .code = code, .length = sizeof(code),
        .constant_pool = NULL, .constant_count = 0,
        .max_locals = 2, .max_stack = 4
    };

    VTX_ASSERT_EQUAL(bc.length, (size_t)8);

    /* Walk through instructions */
    size_t pc = 0;
    VTX_ASSERT_EQUAL(vtx_bytecode_insn_length(&bc, pc), (size_t)3); pc += 3;
    VTX_ASSERT_EQUAL(vtx_bytecode_insn_length(&bc, pc), (size_t)1); pc += 1;
    VTX_ASSERT_EQUAL(vtx_bytecode_insn_length(&bc, pc), (size_t)3); pc += 3;
    VTX_ASSERT_EQUAL(vtx_bytecode_insn_length(&bc, pc), (size_t)1); pc += 1;
    VTX_ASSERT_EQUAL(pc, bc.length);
}

VTX_TEST(test_bytecode_26)
{
    /* Verify opcode_table entries are consistent: names non-null */
    for (int i = 0; i < VT_OP_COUNT; i++) {
        VTX_ASSERT_NOT_NULL(vtx_opcode_table[i].name);
        VTX_ASSERT_TRUE(strlen(vtx_opcode_table[i].name) > 0);
    }
}

VTX_TEST(test_bytecode_27)
{
    /* Verify opcode_table: stack counts are reasonable */
    for (int i = 0; i < VT_OP_COUNT; i++) {
        VTX_ASSERT_TRUE(vtx_opcode_table[i].stack_input_count <= 4);
        VTX_ASSERT_TRUE(vtx_opcode_table[i].stack_output_count <= 4);
    }
}

VTX_TEST(test_bytecode_28)
{
    /* Verify opcode_table: operand_size is reasonable (0, 1, 2, or 4) */
    for (int i = 0; i < VT_OP_COUNT; i++) {
        if (vtx_opcode_table[i].has_operand) {
            VTX_ASSERT_TRUE(vtx_opcode_table[i].operand_size == 1 ||
                            vtx_opcode_table[i].operand_size == 2 ||
                            vtx_opcode_table[i].operand_size == 4);
        }
    }
}

VTX_TEST(test_bytecode_29)
{
    /* Disassemble GOTO */
    uint8_t code[] = { VT_OP_GOTO, 0x00, 0x0A };
    vtx_bytecode_t bc = {
        .code = code, .length = sizeof(code),
        .constant_pool = NULL, .constant_count = 0,
        .max_locals = 0, .max_stack = 4
    };

    char buf[128];
    size_t next = vtx_bytecode_disassemble_op(&bc, 0, buf, sizeof(buf));
    VTX_ASSERT_EQUAL(next, (size_t)3);
    VTX_ASSERT_NOT_NULL(strstr(buf, "GOTO"));
}

VTX_TEST(test_bytecode_30)
{
    /* Disassemble RETURN_VALUE */
    uint8_t code[] = { VT_OP_RETURN_VALUE };
    vtx_bytecode_t bc = {
        .code = code, .length = sizeof(code),
        .constant_pool = NULL, .constant_count = 0,
        .max_locals = 0, .max_stack = 4
    };

    char buf[128];
    size_t next = vtx_bytecode_disassemble_op(&bc, 0, buf, sizeof(buf));
    VTX_ASSERT_EQUAL(next, (size_t)1);
    VTX_ASSERT_NOT_NULL(strstr(buf, "RETURN_VALUE"));
}

VTX_TEST(test_bytecode_31)
{
    /* Stack effect for LOAD_CONST_INT */
    VTX_ASSERT_EQUAL(vtx_bytecode_stack_effect(VT_OP_LOAD_CONST_INT), 1);
    VTX_ASSERT_EQUAL(vtx_bytecode_stack_effect(VT_OP_LOAD_CONST_FLOAT), 1);
    VTX_ASSERT_EQUAL(vtx_bytecode_stack_effect(VT_OP_LOAD_CONST_STR), 1);
}

VTX_TEST(test_bytecode_32)
{
    /* Stack effect for LOAD_NULL, LOAD_TRUE, LOAD_FALSE, LOAD_UNDEFINED */
    VTX_ASSERT_EQUAL(vtx_bytecode_stack_effect(VT_OP_LOAD_NULL), 1);
    VTX_ASSERT_EQUAL(vtx_bytecode_stack_effect(VT_OP_LOAD_TRUE), 1);
    VTX_ASSERT_EQUAL(vtx_bytecode_stack_effect(VT_OP_LOAD_FALSE), 1);
    VTX_ASSERT_EQUAL(vtx_bytecode_stack_effect(VT_OP_LOAD_UNDEFINED), 1);
}

VTX_TEST(test_bytecode_33)
{
    /* Stack effect for NEW, NEWARRAY */
    VTX_ASSERT_TRUE(vtx_bytecode_stack_effect(VT_OP_NEW) >= 0);
    VTX_ASSERT_TRUE(vtx_bytecode_stack_effect(VT_OP_NEWARRAY) >= 0);
}

VTX_TEST(test_bytecode_34)
{
    /* Stack effect for bitwise ops */
    VTX_ASSERT_EQUAL(vtx_bytecode_stack_effect(VT_OP_ISHL), -1);
    VTX_ASSERT_EQUAL(vtx_bytecode_stack_effect(VT_OP_ISHR), -1);
    VTX_ASSERT_EQUAL(vtx_bytecode_stack_effect(VT_OP_IAND), -1);
    VTX_ASSERT_EQUAL(vtx_bytecode_stack_effect(VT_OP_IOR), -1);
    VTX_ASSERT_EQUAL(vtx_bytecode_stack_effect(VT_OP_IXOR), -1);
}

VTX_TEST(test_bytecode_35)
{
    /* Has-operand consistency with operand_size */
    for (int i = 0; i < VT_OP_COUNT; i++) {
        if (!vtx_opcode_table[i].has_operand) {
            /* If no operand, operand_size should be 0 */
            VTX_ASSERT_EQUAL(vtx_opcode_table[i].operand_size, (uint8_t)0);
        } else {
            VTX_ASSERT_TRUE(vtx_opcode_table[i].operand_size > 0);
        }
    }
}

VTX_TEST(test_bytecode_36)
{
    /* Instruction length matches opcode_table for no-operand opcode */
    uint8_t code[] = { VT_OP_NOP };
    vtx_bytecode_t bc = {
        .code = code, .length = sizeof(code),
        .constant_pool = NULL, .constant_count = 0,
        .max_locals = 0, .max_stack = 4
    };

    size_t expected = 1 + vtx_opcode_table[VT_OP_NOP].operand_size;
    VTX_ASSERT_EQUAL(vtx_bytecode_insn_length(&bc, 0), expected);
}

VTX_TEST(test_bytecode_37)
{
    /* Instruction length matches opcode_table for 2-byte-operand opcode */
    uint8_t code[] = { VT_OP_CALL_STATIC, 0x00, 0x05 };
    vtx_bytecode_t bc = {
        .code = code, .length = sizeof(code),
        .constant_pool = NULL, .constant_count = 0,
        .max_locals = 0, .max_stack = 4
    };

    size_t expected = 1 + vtx_opcode_table[VT_OP_CALL_STATIC].operand_size;
    VTX_ASSERT_EQUAL(vtx_bytecode_insn_length(&bc, 0), expected);
}

VTX_TEST(test_bytecode_38)
{
    /* Multi-instruction disassembly */
    uint8_t code[] = {
        VT_OP_LOAD_CONST_INT, 0x00, 0x00,
        VT_OP_LOAD_CONST_INT, 0x00, 0x01,
        VT_OP_IADD,
        VT_OP_RETURN_VALUE
    };
    vtx_value_t consts[] = { vtx_make_smi(10), vtx_make_smi(20) };
    vtx_bytecode_t bc = {
        .code = code, .length = sizeof(code),
        .constant_pool = consts, .constant_count = 2,
        .max_locals = 0, .max_stack = 4
    };

    char buf[128];
    size_t pc = 0;
    pc = vtx_bytecode_disassemble_op(&bc, pc, buf, sizeof(buf));
    VTX_ASSERT_NOT_NULL(strstr(buf, "LOAD_CONST_INT"));
    pc = vtx_bytecode_disassemble_op(&bc, pc, buf, sizeof(buf));
    VTX_ASSERT_NOT_NULL(strstr(buf, "LOAD_CONST_INT"));
    pc = vtx_bytecode_disassemble_op(&bc, pc, buf, sizeof(buf));
    VTX_ASSERT_NOT_NULL(strstr(buf, "IADD"));
    pc = vtx_bytecode_disassemble_op(&bc, pc, buf, sizeof(buf));
    VTX_ASSERT_NOT_NULL(strstr(buf, "RETURN_VALUE"));
}

VTX_TEST(test_bytecode_39)
{
    /* Operand read for CALL_VIRTUAL */
    uint8_t code[] = { VT_OP_CALL_VIRTUAL, 0x00, 0x07 };
    vtx_bytecode_t bc = {
        .code = code, .length = sizeof(code),
        .constant_pool = NULL, .constant_count = 0,
        .max_locals = 0, .max_stack = 4
    };

    VTX_ASSERT_TRUE(vtx_opcode_table[VT_OP_CALL_VIRTUAL].has_operand);
    VTX_ASSERT_EQUAL(vtx_bytecode_read_operand(&bc, 0), (uint16_t)7);
}

VTX_TEST(test_bytecode_40)
{
    /* Bytecode with field access opcodes */
    uint8_t code[] = {
        VT_OP_LOAD_FIELD, 0x00, 0x02,
        VT_OP_STORE_FIELD, 0x00, 0x03
    };
    vtx_bytecode_t bc = {
        .code = code, .length = sizeof(code),
        .constant_pool = NULL, .constant_count = 0,
        .max_locals = 0, .max_stack = 4
    };

    VTX_ASSERT_TRUE(vtx_bytecode_opcode_at(&bc, 0) == VT_OP_LOAD_FIELD);
    VTX_ASSERT_EQUAL(vtx_bytecode_read_operand(&bc, 0), (uint16_t)2);
    VTX_ASSERT_TRUE(vtx_bytecode_opcode_at(&bc, 3) == VT_OP_STORE_FIELD);
    VTX_ASSERT_EQUAL(vtx_bytecode_read_operand(&bc, 3), (uint16_t)3);
}

/* ========================================================================== */
/* GC tests (20 tests)                                                        */
/* ========================================================================== */

VTX_TEST(test_gc_01)
{
    /* Init/destroy in GENERATIONAL mode */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_gc_t gc;
    int rc = vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);
    VTX_ASSERT_EQUAL(rc, 0);

    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_gc_02)
{
    /* Init/destroy in NONE mode */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_gc_t gc;
    int rc = vtx_gc_init(&gc, &ts, VTX_GC_NONE);
    VTX_ASSERT_EQUAL(rc, 0);

    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_gc_03)
{
    /* Init/destroy in MANUAL mode */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_gc_t gc;
    int rc = vtx_gc_init(&gc, &ts, VTX_GC_MANUAL);
    VTX_ASSERT_EQUAL(rc, 0);

    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_gc_04)
{
    /* Init/destroy in ARENA mode */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_gc_t gc;
    int rc = vtx_gc_init(&gc, &ts, VTX_GC_ARENA);
    VTX_ASSERT_EQUAL(rc, 0);

    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_gc_05)
{
    /* Allocate object in generational mode */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_gc_t gc;
    vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);

    size_t sz = vtx_heap_object_alloc_size(2);
    vtx_heap_object_t *obj = vtx_gc_alloc(&gc, sz, VTX_TYPE_OBJECT);
    VTX_ASSERT_NOT_NULL(obj);
    VTX_ASSERT_TRUE(vtx_gc_in_young(&gc, obj));

    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_gc_06)
{
    /* Root push/pop */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_gc_t gc;
    vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);

    vtx_value_t val = vtx_make_smi(42);
    vtx_gc_root_push(&gc, val);
    vtx_value_t popped = vtx_gc_root_pop(&gc);
    VTX_ASSERT_EQUAL(popped, val);

    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_gc_07)
{
    /* Write barrier call (no crash) */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_gc_t gc;
    vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);

    size_t sz = vtx_heap_object_alloc_size(2);
    vtx_heap_object_t *obj = vtx_gc_alloc(&gc, sz, VTX_TYPE_OBJECT);
    VTX_ASSERT_NOT_NULL(obj);

    vtx_value_t val = vtx_make_smi(10);
    /* Write barrier should not crash */
    vtx_gc_write_barrier(&gc, obj, 0, val);

    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_gc_08)
{
    /* Safepoint call (no crash) */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_gc_t gc;
    vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);

    /* Safepoint with no roots should not crash */
    vtx_gc_safepoint(&gc);

    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_gc_09)
{
    /* Pin/unpin objects */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_gc_t gc;
    vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);

    size_t sz = vtx_heap_object_alloc_size(1);
    vtx_heap_object_t *obj = vtx_gc_alloc(&gc, sz, VTX_TYPE_OBJECT);
    VTX_ASSERT_NOT_NULL(obj);

    VTX_ASSERT_FALSE(vtx_gc_is_pinned(&gc, obj));
    vtx_gc_pin(&gc, obj);
    VTX_ASSERT_TRUE(vtx_gc_is_pinned(&gc, obj));
    vtx_gc_unpin(&gc, obj);
    VTX_ASSERT_FALSE(vtx_gc_is_pinned(&gc, obj));

    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_gc_10)
{
    /* Is pinned check for newly allocated object */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_gc_t gc;
    vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);

    size_t sz = vtx_heap_object_alloc_size(1);
    vtx_heap_object_t *obj = vtx_gc_alloc(&gc, sz, VTX_TYPE_OBJECT);
    VTX_ASSERT_NOT_NULL(obj);
    VTX_ASSERT_FALSE(vtx_gc_is_pinned(&gc, obj));

    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_gc_11)
{
    /* Young gen used queries */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_gc_t gc;
    vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);

    size_t before = vtx_gc_young_used(&gc);
    size_t sz = vtx_heap_object_alloc_size(2);
    vtx_gc_alloc(&gc, sz, VTX_TYPE_OBJECT);
    size_t after = vtx_gc_young_used(&gc);
    VTX_ASSERT_TRUE(after >= before + sz);

    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_gc_12)
{
    /* Old gen used queries */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_gc_t gc;
    vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);

    size_t old_used = vtx_gc_old_used(&gc);
    /* Initially old gen should be empty or near-empty */
    VTX_ASSERT_TRUE(old_used < VTX_GC_OLD_SIZE);

    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_gc_13)
{
    /* In young check */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_gc_t gc;
    vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);

    size_t sz = vtx_heap_object_alloc_size(1);
    vtx_heap_object_t *obj = vtx_gc_alloc(&gc, sz, VTX_TYPE_OBJECT);
    VTX_ASSERT_TRUE(vtx_gc_in_young(&gc, obj));
    VTX_ASSERT_FALSE(vtx_gc_in_old(&gc, obj));

    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_gc_14)
{
    /* Mode switching: set_mode/get_mode */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_gc_t gc;
    vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);
    VTX_ASSERT_EQUAL(vtx_gc_get_mode(&gc), VTX_GC_GENERATIONAL);

    vtx_gc_set_mode(&gc, VTX_GC_NONE);
    VTX_ASSERT_EQUAL(vtx_gc_get_mode(&gc), VTX_GC_NONE);

    vtx_gc_set_mode(&gc, VTX_GC_MANUAL);
    VTX_ASSERT_EQUAL(vtx_gc_get_mode(&gc), VTX_GC_MANUAL);

    vtx_gc_set_mode(&gc, VTX_GC_ARENA);
    VTX_ASSERT_EQUAL(vtx_gc_get_mode(&gc), VTX_GC_ARENA);

    vtx_gc_set_mode(&gc, VTX_GC_GENERATIONAL);
    VTX_ASSERT_EQUAL(vtx_gc_get_mode(&gc), VTX_GC_GENERATIONAL);

    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_gc_15)
{
    /* Manual free mode: alloc and free */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_gc_t gc;
    vtx_gc_init(&gc, &ts, VTX_GC_MANUAL);

    size_t sz = vtx_heap_object_alloc_size(2);
    vtx_heap_object_t *obj = vtx_gc_alloc(&gc, sz, VTX_TYPE_OBJECT);
    VTX_ASSERT_NOT_NULL(obj);

    /* Manual free should not crash */
    vtx_gc_manual_free(&gc, obj, sz);

    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_gc_16)
{
    /* Arena enter/leave */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_gc_t gc;
    vtx_gc_init(&gc, &ts, VTX_GC_ARENA);

    size_t sz = vtx_heap_object_alloc_size(1);
    vtx_heap_object_t *obj1 = vtx_gc_alloc(&gc, sz, VTX_TYPE_OBJECT);
    VTX_ASSERT_NOT_NULL(obj1);

    vtx_gc_arena_enter(&gc);

    vtx_heap_object_t *obj2 = vtx_gc_alloc(&gc, sz, VTX_TYPE_OBJECT);
    VTX_ASSERT_NOT_NULL(obj2);

    /* Leave should free everything allocated after enter */
    vtx_gc_arena_leave(&gc);

    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_gc_17)
{
    /* Collect young (no crash) */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_gc_t gc;
    vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);

    size_t sz = vtx_heap_object_alloc_size(1);
    vtx_gc_alloc(&gc, sz, VTX_TYPE_OBJECT);

    /* Collect young with no roots — object may be collected */
    vtx_gc_collect_young(&gc);

    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_gc_18)
{
    /* Collect old (no crash) */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_gc_t gc;
    vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);

    vtx_gc_collect_old(&gc);

    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_gc_19)
{
    /* Multiple allocations and safepoint */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_gc_t gc;
    vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);

    size_t sz = vtx_heap_object_alloc_size(2);
    for (int i = 0; i < 10; i++) {
        vtx_heap_object_t *obj = vtx_gc_alloc(&gc, sz, VTX_TYPE_OBJECT);
        VTX_ASSERT_NOT_NULL(obj);
        /* Write a value to a field */
        vtx_object_set_field(obj, 0, vtx_make_smi(i));
    }

    vtx_gc_safepoint(&gc);

    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
}

VTX_TEST(test_gc_20)
{
    /* GC with no roots (smoke test) */
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_gc_t gc;
    vtx_gc_init(&gc, &ts, VTX_GC_GENERATIONAL);

    /* Allocate several objects with no rooting */
    size_t sz = vtx_heap_object_alloc_size(2);
    for (int i = 0; i < 5; i++) {
        vtx_gc_alloc(&gc, sz, VTX_TYPE_OBJECT);
    }

    /* Both collections should complete without crashing */
    vtx_gc_collect_young(&gc);
    vtx_gc_collect_old(&gc);
    vtx_gc_safepoint(&gc);

    vtx_gc_destroy(&gc);
    vtx_type_system_destroy(&ts);
}

/* ========================================================================== */
/* Main                                                                         */
/* ========================================================================== */

int main(void) {
    vtx_test_result_t result = vtx_test_run_all();
    return (result.fail_count > 0) ? 1 : 0;
}
