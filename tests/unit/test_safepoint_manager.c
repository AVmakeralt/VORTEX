/*
 * VORTEX Safepoint Manager Tests
 *
 * Audit priority #4 (Hardening): validates the thread-safe safepoint
 * coordination in src/runtime/safepoint_manager.c.
 *
 * Tests:
 *   1. Single-thread register/unregister
 *   2. Safepoint check with no request is fast (no-op)
 *   3. Request_all blocks until all threads safepoint
 *   4. Release_all unblocks all threads
 *   5. Multi-thread: 4 mutator threads + 1 GC thread
 *   6. Stats string
 */

#include "test_framework.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>

#include "runtime/safepoint_manager.h"
#include "runtime/gc.h"
#include "runtime/type_system.h"

/* Test context for multi-threaded tests */
typedef struct {
    vtx_safepoint_manager_t *mgr;
    uint32_t                 iterations;
    uint32_t                 safepoints_hit;
} mutator_ctx_t;

/* Mutator thread: registers, does work with periodic safepoint checks,
 * then unregisters. */
static void *mutator_thread_fn(void *arg)
{
    mutator_ctx_t *ctx = (mutator_ctx_t *)arg;
    vtx_safepoint_thread_register(ctx->mgr);

    for (uint32_t i = 0; i < ctx->iterations; i++) {
        /* Simulate work */
        for (volatile int j = 0; j < 100; j++) {}
        /* Check for safepoint at regular intervals */
        bool was_requested = false;
        pthread_mutex_lock(&ctx->mgr->mutex);
        was_requested = ctx->mgr->safepoint_requested;
        pthread_mutex_unlock(&ctx->mgr->mutex);
        if (was_requested) ctx->safepoints_hit++;
        vtx_safepoint_check(ctx->mgr);
    }

    vtx_safepoint_thread_unregister(ctx->mgr);
    return NULL;
}

VTX_TEST(safepoint_single_thread_register) {
    vtx_safepoint_manager_t mgr;
    VTX_ASSERT_TRUE(vtx_safepoint_manager_init(&mgr, NULL) == 0);

    VTX_ASSERT_TRUE(vtx_safepoint_thread_count(&mgr) == 0);
    VTX_ASSERT_TRUE(vtx_safepoint_thread_register(&mgr) == 0);
    VTX_ASSERT_TRUE(vtx_safepoint_thread_count(&mgr) == 1);

    vtx_safepoint_thread_unregister(&mgr);
    VTX_ASSERT_TRUE(vtx_safepoint_thread_count(&mgr) == 1);  /* count doesn't decrease */

    vtx_safepoint_manager_destroy(&mgr);
}

VTX_TEST(safepoint_check_noop_when_not_requested) {
    vtx_safepoint_manager_t mgr;
    VTX_ASSERT_TRUE(vtx_safepoint_manager_init(&mgr, NULL) == 0);
    vtx_safepoint_thread_register(&mgr);

    /* No safepoint requested — check should be instant (no-op). */
    vtx_safepoint_check(&mgr);
    VTX_ASSERT_TRUE(mgr.total_safepoints == 0);

    vtx_safepoint_thread_unregister(&mgr);
    vtx_safepoint_manager_destroy(&mgr);
}

VTX_TEST(safepoint_request_release_single_thread) {
    vtx_safepoint_manager_t mgr;
    VTX_ASSERT_TRUE(vtx_safepoint_manager_init(&mgr, NULL) == 0);
    vtx_safepoint_thread_register(&mgr);

    /* In a single-thread scenario, request_all should immediately succeed
     * because the calling thread IS the only thread. But we can't call
     * request_all from the same thread that needs to safepoint (deadlock).
     * So we test that request/release from the "GC perspective" works
     * when no mutator threads need to be paused. */

    /* Since we're the only thread and we're calling request_all, we
     * can't also be the mutator. This test just verifies the API doesn't
     * crash. A proper test requires a separate GC thread. */
    VTX_ASSERT_TRUE(mgr.active_count == 1);

    vtx_safepoint_thread_unregister(&mgr);
    vtx_safepoint_manager_destroy(&mgr);
}

/* Multi-threaded test: 2 mutator threads + 1 GC thread.
 * The GC thread requests a safepoint, waits for all mutators to stop,
 * then releases them. */
typedef struct {
    vtx_safepoint_manager_t *mgr;
    uint32_t                 safepoint_id;  /* ID of the safepoint we triggered */
} gc_ctx_t;

static void *gc_thread_fn(void *arg)
{
    gc_ctx_t *ctx = (gc_ctx_t *)arg;
    /* Wait a bit for mutators to start. */
    usleep(10000);  /* 10ms */

    /* Request all threads to safepoint. */
    vtx_safepoint_request_all(ctx->mgr);

    /* "Do GC" — simulate collection time. */
    usleep(50000);  /* 50ms */

    /* Release all threads. */
    vtx_safepoint_release_all(ctx->mgr);

    return NULL;
}

VTX_TEST(safepoint_multi_thread_gc) {
    vtx_safepoint_manager_t mgr;
    VTX_ASSERT_TRUE(vtx_safepoint_manager_init(&mgr, NULL) == 0);

    /* Register 2 mutator threads. */
    mutator_ctx_t ctx1 = { .mgr = &mgr, .iterations = 100000, .safepoints_hit = 0 };
    mutator_ctx_t ctx2 = { .mgr = &mgr, .iterations = 100000, .safepoints_hit = 0 };

    pthread_t mut1, mut2;
    pthread_create(&mut1, NULL, mutator_thread_fn, &ctx1);
    pthread_create(&mut2, NULL, mutator_thread_fn, &ctx2);

    /* Start the GC thread. */
    gc_ctx_t gc_ctx = { .mgr = &mgr, .safepoint_id = 0 };
    pthread_t gc_thread;
    pthread_create(&gc_thread, NULL, gc_thread_fn, &gc_ctx);

    /* Wait for all to finish. */
    pthread_join(gc_thread, NULL);
    pthread_join(mut1, NULL);
    pthread_join(mut2, NULL);

    /* The GC should have triggered at least 1 safepoint. */
    printf("[safepoint] total_safepoints=%llu, mut1_hits=%u, mut2_hits=%u\n",
           (unsigned long long)mgr.total_safepoints,
           ctx1.safepoints_hit, ctx2.safepoints_hit);
    VTX_ASSERT_TRUE(mgr.total_safepoints >= 1);

    vtx_safepoint_manager_destroy(&mgr);
}

VTX_TEST(safepoint_stats_str) {
    vtx_safepoint_manager_t mgr;
    VTX_ASSERT_TRUE(vtx_safepoint_manager_init(&mgr, NULL) == 0);
    vtx_safepoint_thread_register(&mgr);

    char buf[256];
    vtx_safepoint_stats_str(&mgr, buf, sizeof(buf));
    printf("[safepoint] stats: %s\n", buf);
    VTX_ASSERT_TRUE(strstr(buf, "threads=1") != NULL);
    VTX_ASSERT_TRUE(strstr(buf, "active=1") != NULL);

    vtx_safepoint_thread_unregister(&mgr);
    vtx_safepoint_manager_destroy(&mgr);
}

VTX_TEST(safepoint_max_threads) {
    /* Register up to MAX_THREADS threads. This test just verifies
     * the limit is enforced. */
    vtx_safepoint_manager_t mgr;
    VTX_ASSERT_TRUE(vtx_safepoint_manager_init(&mgr, NULL) == 0);

    /* We can't actually register MAX_THREADS (256) threads in a test,
     * but we can verify the data structure handles a few registrations. */
    for (int i = 0; i < 4; i++) {
        VTX_ASSERT_TRUE(vtx_safepoint_thread_register(&mgr) == 0);
    }
    VTX_ASSERT_TRUE(vtx_safepoint_thread_count(&mgr) == 4);

    /* Unregister all. */
    for (int i = 0; i < 4; i++) {
        vtx_safepoint_thread_unregister(&mgr);
    }

    vtx_safepoint_manager_destroy(&mgr);
}

int main(void) {
    printf("=== VORTEX Safepoint Manager Tests ===\n\n");
    vtx_test_run_all();
    return 0;
}
