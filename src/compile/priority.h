#ifndef VORTEX_COMPILE_PRIORITY_H
#define VORTEX_COMPILE_PRIORITY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>
#include "vortex_config.h"
#include "runtime/arena.h"
#include "interp/profiler.h"

/**
 * VORTEX Compilation Priority Queue
 *
 * A priority queue for compilation tasks, ordered by:
 *   1. Tier (T3 > T2 > T1 — higher tier = higher priority)
 *   2. Method heat (hotter methods compiled first)
 *   3. Wait time (starvation prevention — tasks waiting too long
 *      get a priority boost)
 *
 * Implementation: binary max-heap with mutex protection for thread safety.
 * Includes a lock-free fast path for single-producer/single-consumer
 * scenarios: the application thread submits tasks and one worker thread
 * consumes them. The fast path avoids the mutex when there is no contention.
 */

/* ========================================================================== */
/* Compilation task                                                            */
/* ========================================================================== */

/**
 * A single compilation task in the priority queue.
 */
typedef struct {
    uint32_t          method_id;     /* method to compile */
    vtx_compile_tier_t tier;         /* compilation tier */
    uint32_t          heat;          /* method invocation count (profiling) */
    uint64_t          submit_time;   /* timestamp when submitted (nanoseconds) */
    void            (*task_fn)(void *arg); /* compilation function */
    void             *arg;           /* argument to task_fn */
    vtx_arena_t      *arena;         /* per-task arena for compilation */
} vtx_compile_task_t;

/* ========================================================================== */
/* Priority queue                                                              */
/* ========================================================================== */

#define VTX_PQ_INITIAL_CAPACITY 64

typedef struct {
    vtx_compile_task_t *tasks;         /* heap array */
    uint32_t            count;         /* number of tasks in the queue */
    uint32_t            capacity;      /* allocated capacity */

    pthread_mutex_t     mutex;         /* protects the heap */
    bool                lock_free;     /* true if single-producer/single-consumer detected */

    /* Starvation prevention: after this many nanoseconds, a task's
     * effective priority is boosted. Derived from compile-time target:
     * if a task waits more than 100ms, it gets a boost. */
    uint64_t            starvation_threshold_ns;

    /* Statistics */
    uint64_t            total_pushed;
    uint64_t            total_popped;
    uint64_t            max_queue_depth;
} vtx_priority_queue_t;

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

/**
 * Initialize the priority queue.
 * Returns 0 on success, -1 on failure.
 */
int vtx_pq_init(vtx_priority_queue_t *pq);

/**
 * Destroy the priority queue and free memory.
 */
void vtx_pq_destroy(vtx_priority_queue_t *pq);

/* ========================================================================== */
/* Operations                                                                  */
/* ========================================================================== */

/**
 * Push a task onto the priority queue.
 * The task is copied into the queue (the caller can free their copy).
 * Returns 0 on success, -1 on failure.
 */
int vtx_pq_push(vtx_priority_queue_t *pq, const vtx_compile_task_t *task);

/**
 * Pop the highest-priority task from the queue.
 * Returns true if a task was popped, false if the queue is empty.
 */
bool vtx_pq_pop(vtx_priority_queue_t *pq, vtx_compile_task_t *out_task);

/**
 * Peek at the highest-priority task without removing it.
 * Returns true if a task was found, false if the queue is empty.
 */
bool vtx_pq_peek(const vtx_priority_queue_t *pq, vtx_compile_task_t *out_task);

/**
 * Get the number of tasks in the queue.
 */
uint32_t vtx_pq_count(const vtx_priority_queue_t *pq);

/**
 * Check if the queue is empty.
 */
bool vtx_pq_is_empty(const vtx_priority_queue_t *pq);

/* ========================================================================== */
/* Priority computation                                                        */
/* ========================================================================== */

/**
 * Compute the effective priority of a task.
 * Higher value = higher priority.
 *
 * Priority formula:
 *   effective_priority = (tier * TIER_WEIGHT) + (heat * HEAT_WEIGHT) + starvation_boost
 *
 *   starvation_boost = max(0, (wait_time - threshold) / DIVISOR)
 *
 * This ensures:
 *   - Tier dominates (T3 is always prioritized over T2)
 *   - Within same tier, hotter methods are compiled first
 *   - Tasks waiting too long get a boost to prevent starvation
 */
int64_t vtx_pq_task_priority(const vtx_compile_task_t *task,
                               uint64_t starvation_threshold_ns);

#endif /* VORTEX_COMPILE_PRIORITY_H */
