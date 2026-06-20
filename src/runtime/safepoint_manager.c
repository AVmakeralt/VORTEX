/*
 * VORTEX Safepoint Manager implementation.
 *
 * See safepoint_manager.h for design and audit rationale.
 *
 * Uses a mutex + condition variables to coordinate threads. When a
 * safepoint is requested, mutator threads check the flag and block on
 * the "released" condition variable. The GC thread waits on the
 * "all_safepointed" condition variable until all threads are blocked.
 *
 * The fast path (no safepoint requested) is just an atomic flag check
 * protected by the mutex. In production, this would use a lock-free
 * atomic for the flag, but pthread_mutex is sufficient for correctness.
 */

#include "safepoint_manager.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

int vtx_safepoint_manager_init(vtx_safepoint_manager_t *mgr, vtx_gc_t *gc)
{
    if (mgr == NULL) return -1;
    memset(mgr, 0, sizeof(*mgr));
    mgr->gc = gc;
    pthread_mutex_init(&mgr->mutex, NULL);
    pthread_cond_init(&mgr->all_safepointed, NULL);
    pthread_cond_init(&mgr->released, NULL);
    return 0;
}

void vtx_safepoint_manager_destroy(vtx_safepoint_manager_t *mgr)
{
    if (mgr == NULL) return;
    pthread_mutex_destroy(&mgr->mutex);
    pthread_cond_destroy(&mgr->all_safepointed);
    pthread_cond_destroy(&mgr->released);
}

int vtx_safepoint_thread_register(vtx_safepoint_manager_t *mgr)
{
    if (mgr == NULL) return -1;
    pthread_mutex_lock(&mgr->mutex);
    if (mgr->thread_count >= VTX_SAFEOINT_MAX_THREADS) {
        pthread_mutex_unlock(&mgr->mutex);
        return -1;
    }
    uint32_t idx = mgr->thread_count++;
    mgr->threads[idx].tid = pthread_self();
    mgr->threads[idx].state = VTX_THREAD_STATE_RUNNING;
    mgr->threads[idx].safepoint_count = 0;
    mgr->threads[idx].total_wait_ns = 0;
    mgr->active_count++;
    pthread_mutex_unlock(&mgr->mutex);
    return 0;
}

void vtx_safepoint_thread_unregister(vtx_safepoint_manager_t *mgr)
{
    if (mgr == NULL) return;
    pthread_mutex_lock(&mgr->mutex);
    pthread_t self = pthread_self();
    for (uint32_t i = 0; i < mgr->thread_count; i++) {
        if (pthread_equal(mgr->threads[i].tid, self)) {
            mgr->threads[i].state = VTX_THREAD_STATE_EXITED;
            mgr->active_count--;
            break;
        }
    }
    /* If a safepoint was requested, check if we're the last to leave */
    if (mgr->safepoint_requested) {
        uint32_t safepointed = 0;
        for (uint32_t i = 0; i < mgr->thread_count; i++) {
            if (mgr->threads[i].state == VTX_THREAD_STATE_SAFEPOINT) {
                safepointed++;
            }
        }
        if (safepointed == mgr->active_count) {
            pthread_cond_signal(&mgr->all_safepointed);
        }
    }
    pthread_mutex_unlock(&mgr->mutex);
}

void vtx_safepoint_check(vtx_safepoint_manager_t *mgr)
{
    if (mgr == NULL) return;
    pthread_mutex_lock(&mgr->mutex);

    /* Fast path: no safepoint requested. */
    if (!mgr->safepoint_requested) {
        pthread_mutex_unlock(&mgr->mutex);
        return;
    }

    /* Slow path: safepoint requested. Block until released. */
    pthread_t self = pthread_self();
    uint32_t my_idx = (uint32_t)-1;
    for (uint32_t i = 0; i < mgr->thread_count; i++) {
        if (pthread_equal(mgr->threads[i].tid, self)) {
            my_idx = i;
            break;
        }
    }
    if (my_idx == (uint32_t)-1) {
        /* Thread not registered — just return. */
        pthread_mutex_unlock(&mgr->mutex);
        return;
    }

    /* Transition to SAFEPOINT state. */
    mgr->threads[my_idx].state = VTX_THREAD_STATE_SAFEPOINT;
    mgr->threads[my_idx].safepoint_count++;
    mgr->safepointed_count++;

    struct timespec wait_start;
    clock_gettime(CLOCK_MONOTONIC, &wait_start);

    /* Signal the GC thread if all active threads are now safepointed. */
    if (mgr->safepointed_count == mgr->active_count) {
        pthread_cond_signal(&mgr->all_safepointed);
    }

    /* Wait for the GC to release us. */
    uint32_t my_safepoint_id = mgr->safepoint_id;
    while (mgr->safepoint_requested && mgr->safepoint_id == my_safepoint_id) {
        pthread_cond_wait(&mgr->released, &mgr->mutex);
    }

    /* Back to RUNNING state. */
    mgr->threads[my_idx].state = VTX_THREAD_STATE_RUNNING;
    mgr->safepointed_count--;

    struct timespec wait_end;
    clock_gettime(CLOCK_MONOTONIC, &wait_end);
    uint64_t wait_ns = (wait_end.tv_sec - wait_start.tv_sec) * 1000000000ULL +
                        (wait_end.tv_nsec - wait_start.tv_nsec);
    mgr->threads[my_idx].total_wait_ns += wait_ns;
    mgr->total_wait_ns += wait_ns;

    pthread_mutex_unlock(&mgr->mutex);
}

int vtx_safepoint_request_all(vtx_safepoint_manager_t *mgr)
{
    if (mgr == NULL) return -1;
    pthread_mutex_lock(&mgr->mutex);

    mgr->safepoint_requested = true;
    mgr->safepoint_id++;
    mgr->safepointed_count = 0;
    mgr->total_safepoints++;

    /* Wait until all active threads have reached the safepoint. */
    while (mgr->safepointed_count < mgr->active_count) {
        pthread_cond_wait(&mgr->all_safepointed, &mgr->mutex);
    }

    pthread_mutex_unlock(&mgr->mutex);
    return 0;
}

int vtx_safepoint_release_all(vtx_safepoint_manager_t *mgr)
{
    if (mgr == NULL) return -1;
    pthread_mutex_lock(&mgr->mutex);

    mgr->safepoint_requested = false;

    /* Wake up all waiting threads. */
    pthread_cond_broadcast(&mgr->released);

    pthread_mutex_unlock(&mgr->mutex);
    return 0;
}

void vtx_safepoint_stats_str(const vtx_safepoint_manager_t *mgr,
                              char *buf, size_t bufsize)
{
    if (mgr == NULL || buf == NULL || bufsize == 0) return;
    snprintf(buf, bufsize,
        "safepoint_mgr: threads=%u active=%u total_safepoints=%llu total_wait_ns=%llu",
        mgr->thread_count, mgr->active_count,
        (unsigned long long)mgr->total_safepoints,
        (unsigned long long)mgr->total_wait_ns);
}

uint32_t vtx_safepoint_thread_count(const vtx_safepoint_manager_t *mgr)
{
    if (mgr == NULL) return 0;
    return mgr->thread_count;
}
