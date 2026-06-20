#ifndef VORTEX_RUNTIME_SAFEPOINT_MANAGER_H
#define VORTEX_RUNTIME_SAFEPOINT_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>
#include "runtime/gc.h"

/**
 * VORTEX Safepoint Manager
 * ========================
 *
 * Audit priority #4 (Hardening): "Thread-safe GC — currently uses the_gc
 * global. For concurrent compilation + execution: make GC state thread-local
 * or use read-copy-update. Stop-the-world safepoints must actually pause all
 * mutator threads. Write barrier must be atomic for concurrent marking."
 *
 * This module provides a safepoint manager that coordinates multiple mutator
 * threads for stop-the-world garbage collection. It ensures:
 *
 *   1. When the GC requests a safepoint, ALL mutator threads reach a safe
 *      point before collection begins.
 *   2. Mutator threads check for pending safepoints at regular intervals
 *      (at method entry, loop back-edges, and allocation sites).
 *   3. A thread in a safepoint blocks until the GC releases it.
 *
 * Usage:
 *   - Main thread: vtx_safepoint_manager_init(&mgr, &gc)
 *   - Each mutator thread: vtx_safepoint_thread_register(&mgr)
 *   - At safepoint check sites: vtx_safepoint_check(&mgr)
 *   - GC thread: vtx_safepoint_request_all(&mgr) → blocks until all threads
 *     are safepointed → do GC → vtx_safepoint_release_all(&mgr)
 *   - Thread exit: vtx_safepoint_thread_unregister(&mgr)
 *
 * Thread safety: all operations are thread-safe. The manager uses a
 * combination of atomics and a condition variable to coordinate.
 */

#define VTX_SAFEOINT_MAX_THREADS 256

typedef enum {
    VTX_THREAD_STATE_RUNNING = 0,   /* thread is executing mutator code */
    VTX_THREAD_STATE_SAFEPOINT = 1, /* thread is blocked at a safepoint */
    VTX_THREAD_STATE_GC = 2,        /* thread is the GC thread, doing collection */
    VTX_THREAD_STATE_EXITED = 3,    /* thread has unregistered */
} vtx_thread_state_t;

typedef struct {
    pthread_t           tid;
    vtx_thread_state_t  state;
    uint32_t            safepoint_count;  /* number of safepoints this thread has entered */
    uint64_t            total_wait_ns;    /* total time spent waiting at safepoints */
} vtx_safepoint_thread_t;

typedef struct {
    vtx_gc_t                *gc;             /* the GC being coordinated (shared) */
    pthread_mutex_t          mutex;          /* protects all fields below */
    pthread_cond_t           all_safepointed; /* signaled when all threads are safepointed */
    pthread_cond_t           released;       /* signaled when GC releases all threads */

    vtx_safepoint_thread_t   threads[VTX_SAFEOINT_MAX_THREADS];
    uint32_t                 thread_count;
    uint32_t                 active_count;   /* threads in RUNNING or SAFEPOINT state */

    bool                     safepoint_requested;  /* true when GC wants all threads to stop */
    uint32_t                 safepoint_id;          /* monotonic ID of current safepoint request */
    uint32_t                 safepointed_count;     /* threads that have reached the safepoint */

    /* Statistics */
    uint64_t                 total_safepoints;      /* lifetime count of safepoint events */
    uint64_t                 total_wait_ns;         /* total time all threads spent waiting */
} vtx_safepoint_manager_t;

/* Initialize the safepoint manager. The GC pointer is stored but not owned. */
int vtx_safepoint_manager_init(vtx_safepoint_manager_t *mgr, vtx_gc_t *gc);

/* Destroy the safepoint manager. */
void vtx_safepoint_manager_destroy(vtx_safepoint_manager_t *mgr);

/* Register the calling thread with the safepoint manager.
 * Returns 0 on success, -1 if the max thread count is exceeded.
 * Each mutator thread must call this before executing any code. */
int vtx_safepoint_thread_register(vtx_safepoint_manager_t *mgr);

/* Unregister the calling thread. Call before thread exit. */
void vtx_safepoint_thread_unregister(vtx_safepoint_manager_t *mgr);

/* Check if a safepoint has been requested. If so, block until released.
 * This should be called at regular intervals by mutator threads:
 *   - At method entry
 *   - At loop back-edges (every N iterations)
 *   - At allocation sites
 *
 * This function is FAST when no safepoint is requested: it just checks
 * the atomic flag. When a safepoint IS requested, it blocks until the
 * GC releases all threads. */
void vtx_safepoint_check(vtx_safepoint_manager_t *mgr);

/* Request all mutator threads to reach a safepoint. Blocks until all
 * active threads have reached the safepoint. Called by the GC thread
 * before starting collection. */
int vtx_safepoint_request_all(vtx_safepoint_manager_t *mgr);

/* Release all threads waiting at the safepoint. Called by the GC thread
 * after collection is complete. */
int vtx_safepoint_release_all(vtx_safepoint_manager_t *mgr);

/* Get statistics as a printable string. */
void vtx_safepoint_stats_str(const vtx_safepoint_manager_t *mgr,
                              char *buf, size_t bufsize);

/* Get the number of currently registered threads. */
uint32_t vtx_safepoint_thread_count(const vtx_safepoint_manager_t *mgr);

#endif /* VORTEX_RUNTIME_SAFEPOINT_MANAGER_H */
