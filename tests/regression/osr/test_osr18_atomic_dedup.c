/*
 * OSR-18 regression: atomic deduplication in vtx_request_compilation
 *                    and atomic profiler counters.
 *
 * Bug: vtx_request_compilation used a non-atomic check-then-set
 *      (RELAXED loads + stores) on the `compilation_requested` flag.
 *      Two interpreter threads hitting the same hot loop could both
 *      pass the check and both submit compile tasks, then both set
 *      the flag — violating the dedup invariant and corrupting the
 *      threadpool.
 *
 * Fix: use __atomic_compare_exchange_n to atomically transition the
 *      flag from false → true. Only one thread wins the CAS; the
 *      loser sees the failure and returns without submitting.
 *
 * Reproducer: spawn N threads, all calling vtx_request_compilation
 *             for the same method behind a barrier. After all threads
 *             finish, the dedup flag must be set (exactly one winner
 *             is unobservable from outside, but the flag-set invariant
 *             and the lack of a crash is the contract).
 *
 * Also tests: atomic saturating increments on profiler counters
 *             (no lost updates under concurrent increments).
 */

#include "test_framework.h"
#include "compile/request.h"
#include "interp/profiler.h"
#include "runtime/type_system.h"
#include "runtime/bytecode.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* vtx_request_compilation dedup test                                          */
/* ========================================================================== */

/* Test method stub. */
static vtx_bytecode_t g_bytecode;
static vtx_method_desc_t g_method = {
    .name = "hot_loop",
    .signature = "()I",
    .bytecode = &g_bytecode,
    .compiled_code = NULL,
    .vtable_index = 42,    /* method_id */
    .arg_count = 0,
    .is_virtual = false
};

#define OSR18_NTHREADS 8

static vtx_compile_context_t *g_ctx;
static pthread_barrier_t g_barrier;

static void *osr18_thread_fn(void *arg) {
    (void)arg;
    /* Wait until all threads are ready, then all call
     * vtx_request_compilation simultaneously. This maximizes the
     * chance of a race — without the barrier, threads would serialize
     * naturally and the bug might not manifest. */
    pthread_barrier_wait(&g_barrier);

    /* All N threads call this concurrently. Pre-fix: multiple could
     * pass the relaxed check and submit duplicate tasks. Post-fix:
     * the CAS guarantees only one wins. */
    vtx_request_compilation(g_ctx, &g_method, 50000);
    return NULL;
}

VTX_TEST(osr18_concurrent_request_compilation_sets_flag_once) {
    /* Initialize a compile context WITHOUT a threadpool. With no
     * threadpool, vtx_request_compilation just claims the flag
     * and returns (no submission). The dedup invariant we test:
     *   - flag is set after all threads finish (at least one winner)
     *   - flag is clearable + re-settable (no spurious state)
     *   - repeated runs are deterministic (no crashes / hangs) */
    vtx_compile_context_t ctx;
    VTX_ASSERT_TRUE(vtx_compile_context_init(&ctx) == 0);

    g_ctx = &ctx;
    pthread_barrier_init(&g_barrier, NULL, OSR18_NTHREADS);

    pthread_t threads[OSR18_NTHREADS];
    for (int i = 0; i < OSR18_NTHREADS; i++) {
        VTX_ASSERT_TRUE(pthread_create(&threads[i], NULL,
                                         osr18_thread_fn, NULL) == 0);
    }
    for (int i = 0; i < OSR18_NTHREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    pthread_barrier_destroy(&g_barrier);

    /* Invariant: the flag is set (at least one CAS winner). */
    VTX_ASSERT_TRUE(vtx_is_compilation_requested(&ctx, g_method.vtable_index));

    /* Clear the flag, then re-request. The flag should still be
     * clearable and re-settable. */
    vtx_clear_compilation_requested(&ctx, g_method.vtable_index);
    VTX_ASSERT_FALSE(vtx_is_compilation_requested(&ctx, g_method.vtable_index));

    /* Single-threaded re-request must succeed. */
    vtx_request_compilation(&ctx, &g_method, 50000);
    VTX_ASSERT_TRUE(vtx_is_compilation_requested(&ctx, g_method.vtable_index));

    vtx_compile_context_destroy(&ctx);
    g_ctx = NULL;
}

VTX_TEST(osr18_cas_primitive_works_correctly) {
    /* Direct test of the atomic primitive we rely on. */
    bool flag = false;
    bool expected = false;

    /* First CAS: should succeed (transition false → true). */
    bool won = __atomic_compare_exchange_n(&flag, &expected, true,
                                             false,
                                             __ATOMIC_ACQUIRE,
                                             __ATOMIC_RELAXED);
    VTX_ASSERT_TRUE(won);
    VTX_ASSERT_TRUE(flag);

    /* Second CAS: should fail (flag is already true). */
    expected = false;
    bool won2 = __atomic_compare_exchange_n(&flag, &expected, true,
                                              false,
                                              __ATOMIC_ACQUIRE,
                                              __ATOMIC_RELAXED);
    VTX_ASSERT_FALSE(won2);
    VTX_ASSERT_TRUE(flag);  /* unchanged */
}

/* ========================================================================== */
/* Atomic profiler counter test                                                */
/* ========================================================================== */

typedef struct {
    vtx_profiler_t *profiler;
    const vtx_method_desc_t *method;
    int n_increments;
    pthread_barrier_t *bar;
} osr18_profiler_arg_t;

static void *osr18_profiler_thread_fn(void *p) {
    osr18_profiler_arg_t *a = (osr18_profiler_arg_t *)p;
    pthread_barrier_wait(a->bar);
    for (int i = 0; i < a->n_increments; i++) {
        vtx_profiler_record_backward_branch(a->profiler, a->method);
    }
    return NULL;
}

VTX_TEST(osr18_profiler_atomic_counters_no_lost_updates) {
    /* With the OSR-18 fix, the counter increments use atomics, so
     * no updates are lost. Pre-fix, plain `pd->backward_branch_count++`
     * could lose updates under concurrent access. */
    vtx_profiler_t profiler;
    VTX_ASSERT_TRUE(vtx_profiler_init(&profiler) == 0);

    /* Set up a fake method. */
    vtx_bytecode_t bc = { .length = 16 };
    vtx_method_desc_t method = {
        .name = "test",
        .signature = "()V",
        .bytecode = &bc,
        .compiled_code = NULL,
        .vtable_index = 7,
        .arg_count = 0,
        .is_virtual = false
    };

    const int N_THREADS = 4;
    const int N_INCS_PER_THREAD = 1000;
    pthread_barrier_t bar;
    pthread_barrier_init(&bar, NULL, N_THREADS);

    osr18_profiler_arg_t arg = {
        .profiler = &profiler,
        .method = &method,
        .n_increments = N_INCS_PER_THREAD,
        .bar = &bar
    };

    pthread_t threads[4];
    for (int i = 0; i < N_THREADS; i++) {
        pthread_create(&threads[i], NULL, osr18_profiler_thread_fn, &arg);
    }
    for (int i = 0; i < N_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    pthread_barrier_destroy(&bar);

    /* Invariant: total counter value = N_THREADS * N_INCS_PER_THREAD.
     * Pre-fix: lost updates would cause the count to be < expected. */
    uint64_t heat = vtx_profiler_method_heat(&profiler, &method);
    /* heat = invocation_count + backward_branch_count * 2. We only
     * recorded backward branches (no invocations), so heat = bb_count * 2. */
    uint64_t expected_heat = (uint64_t)N_THREADS * N_INCS_PER_THREAD * 2;
    VTX_ASSERT_TRUE(heat == expected_heat);

    vtx_profiler_destroy(&profiler);
}

int main(void) {
    printf("=== OSR-18 regression: atomic dedup + profiler counters ===\n\n");
    vtx_test_run_all();
    return 0;
}
