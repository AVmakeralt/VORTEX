/**
 * VORTEX Compilation Priority Queue
 *
 * Binary max-heap with mutex protection and lock-free fast path.
 */

#define _POSIX_C_SOURCE 199309L
#include "compile/priority.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Priority weights: tier dominates, heat is secondary, starvation is safety net */
#define VTX_TIER_WEIGHT    100000000LL  /* 10^8 — tier is the dominant factor */
#define VTX_HEAT_WEIGHT         1000LL  /* heat scales linearly */
#define VTX_STARVATION_DIVISOR  1000LL  /* nanoseconds → microsecond-scale boost */

/* Default starvation threshold: 100ms = 100,000,000 ns */
#define VTX_DEFAULT_STARVATION_THRESHOLD_NS 100000000ULL

/* ========================================================================== */
/* Internal: get current time in nanoseconds                                  */
/* ========================================================================== */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ========================================================================== */
/* Internal: heap operations                                                   */
/* ========================================================================== */

static int64_t task_priority_cached(const vtx_compile_task_t *task,
                                      uint64_t starvation_threshold_ns,
                                      uint64_t now)
{
    VTX_ASSERT(task != NULL, "task must not be NULL");

    int64_t priority = 0;

    /* Tier weight: dominant factor */
    priority += (int64_t)task->tier * VTX_TIER_WEIGHT;

    /* Heat weight: secondary factor */
    priority += (int64_t)task->heat * VTX_HEAT_WEIGHT;

    /* Starvation boost: tasks waiting too long get a priority increase */
    if (task->submit_time > 0 && now > task->submit_time) {
        uint64_t wait_time = now - task->submit_time;
        if (wait_time > starvation_threshold_ns) {
            uint64_t excess = wait_time - starvation_threshold_ns;
            int64_t boost = (int64_t)(excess / VTX_STARVATION_DIVISOR);
            priority += boost;
        }
    }

    return priority;
}

static void swap_tasks(vtx_compile_task_t *a, vtx_compile_task_t *b)
{
    vtx_compile_task_t tmp = *a;
    *a = *b;
    *b = tmp;
}

/* Sift up: restore heap property after insert */
static void sift_up(vtx_priority_queue_t *pq, uint32_t index)
{
    /* Cache current time once for all comparisons in this operation */
    uint64_t now = now_ns();

    while (index > 0) {
        uint32_t parent = (index - 1) / 2;
        int64_t idx_pri = task_priority_cached(&pq->tasks[index],
                                                pq->starvation_threshold_ns, now);
        int64_t par_pri = task_priority_cached(&pq->tasks[parent],
                                                pq->starvation_threshold_ns, now);

        if (idx_pri > par_pri) {
            swap_tasks(&pq->tasks[index], &pq->tasks[parent]);
            index = parent;
        } else {
            break;
        }
    }
}

/* Sift down: restore heap property after pop */
static void sift_down(vtx_priority_queue_t *pq, uint32_t index)
{
    /* Cache current time once for all comparisons in this operation */
    uint64_t now = now_ns();

    while (true) {
        uint32_t largest = index;
        uint32_t left    = 2 * index + 1;
        uint32_t right   = 2 * index + 2;

        int64_t largest_pri = task_priority_cached(&pq->tasks[largest],
                                                    pq->starvation_threshold_ns, now);

        if (left < pq->count) {
            int64_t left_pri = task_priority_cached(&pq->tasks[left],
                                                     pq->starvation_threshold_ns, now);
            if (left_pri > largest_pri) {
                largest = left;
                largest_pri = left_pri;
            }
        }

        if (right < pq->count) {
            int64_t right_pri = task_priority_cached(&pq->tasks[right],
                                                      pq->starvation_threshold_ns, now);
            if (right_pri > largest_pri) {
                largest = right;
            }
        }

        if (largest != index) {
            swap_tasks(&pq->tasks[index], &pq->tasks[largest]);
            index = largest;
        } else {
            break;
        }
    }
}

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

int vtx_pq_init(vtx_priority_queue_t *pq)
{
    VTX_ASSERT(pq != NULL, "pq must not be NULL");

    memset(pq, 0, sizeof(*pq));

    pq->capacity = VTX_PQ_INITIAL_CAPACITY;
    pq->tasks = malloc(pq->capacity * sizeof(vtx_compile_task_t));
    if (!pq->tasks) return -1;

    pq->count = 0;
    pq->lock_free = false;
    pq->starvation_threshold_ns = VTX_DEFAULT_STARVATION_THRESHOLD_NS;

    if (pthread_mutex_init(&pq->mutex, NULL) != 0) {
        free(pq->tasks);
        pq->tasks = NULL;
        return -1;
    }

    return 0;
}

void vtx_pq_destroy(vtx_priority_queue_t *pq)
{
    if (!pq) return;

    if (pq->tasks) {
        free(pq->tasks);
        pq->tasks = NULL;
    }

    pthread_mutex_destroy(&pq->mutex);

    pq->count = 0;
    pq->capacity = 0;
}

/* ========================================================================== */
/* Operations                                                                  */
/* ========================================================================== */

int vtx_pq_push(vtx_priority_queue_t *pq, const vtx_compile_task_t *task)
{
    VTX_ASSERT(pq != NULL, "pq must not be NULL");
    VTX_ASSERT(task != NULL, "task must not be NULL");

    pthread_mutex_lock(&pq->mutex);

    /* Grow if needed */
    if (pq->count >= pq->capacity) {
        uint32_t new_cap = pq->capacity * 2;
        vtx_compile_task_t *new_tasks = realloc(pq->tasks,
            new_cap * sizeof(vtx_compile_task_t));
        if (!new_tasks) {
            pthread_mutex_unlock(&pq->mutex);
            return -1;
        }
        pq->tasks = new_tasks;
        pq->capacity = new_cap;
    }

    /* Insert at end and sift up */
    pq->tasks[pq->count] = *task;
    if (pq->tasks[pq->count].submit_time == 0) {
        pq->tasks[pq->count].submit_time = now_ns();
    }
    sift_up(pq, pq->count);
    pq->count++;

    /* Update statistics */
    pq->total_pushed++;
    if (pq->count > pq->max_queue_depth) {
        pq->max_queue_depth = pq->count;
    }

    pthread_mutex_unlock(&pq->mutex);
    return 0;
}

bool vtx_pq_pop(vtx_priority_queue_t *pq, vtx_compile_task_t *out_task)
{
    VTX_ASSERT(pq != NULL, "pq must not be NULL");

    pthread_mutex_lock(&pq->mutex);

    if (pq->count == 0) {
        pthread_mutex_unlock(&pq->mutex);
        return false;
    }

    /* Return the root (highest priority) */
    if (out_task) {
        *out_task = pq->tasks[0];
    }

    /* Move last element to root and sift down */
    pq->count--;
    if (pq->count > 0) {
        pq->tasks[0] = pq->tasks[pq->count];
        sift_down(pq, 0);
    }

    pq->total_popped++;

    pthread_mutex_unlock(&pq->mutex);
    return true;
}

bool vtx_pq_peek(const vtx_priority_queue_t *pq, vtx_compile_task_t *out_task)
{
    VTX_ASSERT(pq != NULL, "pq must not be NULL");

    /* Note: const correctness — we lock a non-const mutex.
     * This is acceptable because peeking doesn't modify the heap. */
    pthread_mutex_lock((pthread_mutex_t *)&pq->mutex);

    if (pq->count == 0) {
        pthread_mutex_unlock((pthread_mutex_t *)&pq->mutex);
        return false;
    }

    if (out_task) {
        *out_task = pq->tasks[0];
    }

    pthread_mutex_unlock((pthread_mutex_t *)&pq->mutex);
    return true;
}

uint32_t vtx_pq_count(const vtx_priority_queue_t *pq)
{
    VTX_ASSERT(pq != NULL, "pq must not be NULL");
    return pq->count;
}

bool vtx_pq_is_empty(const vtx_priority_queue_t *pq)
{
    VTX_ASSERT(pq != NULL, "pq must not be NULL");
    return pq->count == 0;
}

/* ========================================================================== */
/* Priority computation                                                        */
/* ========================================================================== */

int64_t vtx_pq_task_priority(const vtx_compile_task_t *task,
                               uint64_t starvation_threshold_ns)
{
    VTX_ASSERT(task != NULL, "task must not be NULL");
    /* Use cached time version with current time — this is the public API
     * used outside of heap operations. For internal heap operations,
     * task_priority_cached() is used instead to avoid repeated clock_gettime. */
    return task_priority_cached(task, starvation_threshold_ns, now_ns());
}
