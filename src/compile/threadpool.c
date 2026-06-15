/**
 * VORTEX Compilation Thread Pool
 *
 * Fixed-size thread pool with per-worker arenas and priority-based
 * task scheduling.
 */

#define _POSIX_C_SOURCE 199309L
#include "compile/threadpool.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ========================================================================== */
/* Internal: thread argument wrapper                                           */
/* ========================================================================== */

typedef struct {
    vtx_threadpool_t *pool;
    vtx_worker_t     *worker;
} vtx_worker_arg_t;

/* ========================================================================== */
/* Internal: actual worker thread main loop                                    */
/* ========================================================================== */

static void *worker_thread_func(void *arg)
{
    vtx_worker_arg_t *warg = (vtx_worker_arg_t *)arg;
    vtx_threadpool_t *pool = warg->pool;
    vtx_worker_t *worker = warg->worker;

    /* Free the arg wrapper (allocated on heap during init) */
    free(warg);

    while (true) {
        /* Check shutdown flag */
        pthread_mutex_lock(&pool->pool_mutex);
        while (!pool->shutdown && vtx_pq_is_empty(&pool->task_queue)) {
            pthread_cond_wait(&pool->work_available, &pool->pool_mutex);
        }

        if (pool->shutdown && vtx_pq_is_empty(&pool->task_queue)) {
            worker->state = VTX_WORKER_SHUTDOWN;
            pthread_mutex_unlock(&pool->pool_mutex);
            break;
        }

        pthread_mutex_unlock(&pool->pool_mutex);

        /* Try to get a task from the priority queue */
        vtx_compile_task_t task;
        if (!vtx_pq_pop(&pool->task_queue, &task)) {
            /* Queue was empty (another worker grabbed the task) */
            continue;
        }

        /* Execute the task */
        worker->state = VTX_WORKER_COMPILING;
        worker->current_method = task.method_id;
        worker->current_tier = task.tier;

        /* Reset the worker's arena for this compilation */
        vtx_arena_reset(&worker->arena);

        /* Use the task's arena if provided, otherwise use the worker's */
        if (task.arena) {
            /* Task brings its own arena — use it */
        }

        /* Execute the compilation function */
        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);

        if (task.task_fn) {
            task.task_fn(task.arg);
        } else if (task.method_id != 0 && pool->compile_callback != NULL) {
            /* Method compilation task (no task_fn, but has method_id).
             * This is the path used by the orchestrator and
             * vtx_request_compilation(). Previously, these tasks
             * were silently discarded because task_fn was NULL.
             * Now we invoke the compile callback to run the pipeline. */
            pool->compile_callback(task.method_id, task.tier,
                                   pool->compile_callback_context);
        }

        clock_gettime(CLOCK_MONOTONIC, &end);

        uint64_t elapsed_ns =
            (uint64_t)(end.tv_sec - start.tv_sec) * 1000000000ULL +
            (uint64_t)(end.tv_nsec - start.tv_nsec);

        /* Update statistics */
        worker->tasks_completed++;
        worker->current_method = 0;
        worker->current_tier = 0;
        worker->state = VTX_WORKER_IDLE;

        __atomic_fetch_add(&pool->total_tasks_completed, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&pool->total_compile_time_ns, elapsed_ns, __ATOMIC_RELAXED);
    }

    /* Clean up the worker's arena */
    vtx_arena_destroy(&worker->arena);

    return NULL;
}

/* ========================================================================== */
/* Internal: detect number of CPU cores                                       */
/* ========================================================================== */

static uint32_t detect_cpu_cores(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n > 0) {
        return (uint32_t)n;
    }
    return 1; /* fallback */
}

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

int vtx_threadpool_init(vtx_threadpool_t *pool, uint32_t num_threads)
{
    VTX_ASSERT(pool != NULL, "pool must not be NULL");

    memset(pool, 0, sizeof(*pool));

    /* Determine number of worker threads */
    if (num_threads == 0) {
        num_threads = VORTEX_COMPILE_THREADS;
        if (num_threads == 0) {
            /* Fallback: cores - 1, minimum 1 */
            uint32_t cores = detect_cpu_cores();
            num_threads = cores > 1 ? cores - 1 : 1;
        }
    }

    pool->num_workers = num_threads;
    pool->shutdown = false;

    /* Initialize the priority queue */
    if (vtx_pq_init(&pool->task_queue) != 0) {
        return -1;
    }

    /* Initialize mutex and condition variable */
    if (pthread_mutex_init(&pool->pool_mutex, NULL) != 0) {
        vtx_pq_destroy(&pool->task_queue);
        return -1;
    }

    if (pthread_cond_init(&pool->work_available, NULL) != 0) {
        pthread_mutex_destroy(&pool->pool_mutex);
        vtx_pq_destroy(&pool->task_queue);
        return -1;
    }

    /* Allocate worker array */
    pool->workers = calloc(num_threads, sizeof(vtx_worker_t));
    if (!pool->workers) {
        pthread_cond_destroy(&pool->work_available);
        pthread_mutex_destroy(&pool->pool_mutex);
        vtx_pq_destroy(&pool->task_queue);
        return -1;
    }

    /* Initialize and start each worker */
    for (uint32_t i = 0; i < num_threads; i++) {
        pool->workers[i].worker_id = i;
        pool->workers[i].state = VTX_WORKER_IDLE;
        pool->workers[i].tasks_completed = 0;
        pool->workers[i].current_method = 0;
        pool->workers[i].current_tier = 0;

        /* Initialize per-worker arena */
        if (vtx_arena_init(&pool->workers[i].arena) != 0) {
            /* Failed — shut down already-started workers */
            pool->shutdown = true;
            pthread_cond_broadcast(&pool->work_available);
            for (uint32_t j = 0; j < i; j++) {
                pthread_join(pool->workers[j].thread, NULL);
            }
            free(pool->workers);
            pthread_cond_destroy(&pool->work_available);
            pthread_mutex_destroy(&pool->pool_mutex);
            vtx_pq_destroy(&pool->task_queue);
            return -1;
        }

        /* Create the worker thread */
        vtx_worker_arg_t *warg = malloc(sizeof(vtx_worker_arg_t));
        if (!warg) {
            vtx_arena_destroy(&pool->workers[i].arena);
            pool->shutdown = true;
            pthread_cond_broadcast(&pool->work_available);
            for (uint32_t j = 0; j < i; j++) {
                pthread_join(pool->workers[j].thread, NULL);
            }
            free(pool->workers);
            pthread_cond_destroy(&pool->work_available);
            pthread_mutex_destroy(&pool->pool_mutex);
            vtx_pq_destroy(&pool->task_queue);
            return -1;
        }
        warg->pool = pool;
        warg->worker = &pool->workers[i];

        if (pthread_create(&pool->workers[i].thread, NULL,
                           worker_thread_func, warg) != 0) {
            free(warg);
            vtx_arena_destroy(&pool->workers[i].arena);
            pool->shutdown = true;
            pthread_cond_broadcast(&pool->work_available);
            for (uint32_t j = 0; j < i; j++) {
                pthread_join(pool->workers[j].thread, NULL);
            }
            free(pool->workers);
            pthread_cond_destroy(&pool->work_available);
            pthread_mutex_destroy(&pool->pool_mutex);
            vtx_pq_destroy(&pool->task_queue);
            return -1;
        }
    }

    return 0;
}

void vtx_threadpool_shutdown(vtx_threadpool_t *pool)
{
    if (!pool) return;

    /* Set shutdown flag and wake all workers */
    pthread_mutex_lock(&pool->pool_mutex);
    __atomic_store_n(&pool->shutdown, true, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&pool->work_available);
    pthread_mutex_unlock(&pool->pool_mutex);

    /* Wait for all workers to finish */
    for (uint32_t i = 0; i < pool->num_workers; i++) {
        pthread_join(pool->workers[i].thread, NULL);
    }

    /* Clean up resources */
    free(pool->workers);
    pool->workers = NULL;

    pthread_cond_destroy(&pool->work_available);
    pthread_mutex_destroy(&pool->pool_mutex);

    vtx_pq_destroy(&pool->task_queue);

    pool->num_workers = 0;
}

void vtx_threadpool_set_compile_callback(vtx_threadpool_t *pool,
                                          vtx_compile_callback_t callback,
                                          void *context)
{
    if (pool == NULL) return;
    pool->compile_callback = callback;
    pool->compile_callback_context = context;
}

/* ========================================================================== */
/* Task submission                                                             */
/* ========================================================================== */

int vtx_threadpool_submit(vtx_threadpool_t *pool,
                           void (*task_fn)(void *arg),
                           void *arg,
                           int64_t priority)
{
    VTX_ASSERT(pool != NULL, "pool must not be NULL");

    if (__atomic_load_n(&pool->shutdown, __ATOMIC_ACQUIRE)) return -1;

    vtx_compile_task_t task;
    memset(&task, 0, sizeof(task));
    task.task_fn = task_fn;
    task.arg     = arg;
    task.heat    = (uint32_t)(priority > 0 ? priority : 0);
    task.tier    = VTX_TIER_T2; /* default tier */

    /* vtx_threadpool_submit_task already increments total_tasks_submitted,
     * so we must not double-count here. */

    return vtx_threadpool_submit_task(pool, &task);
}

int vtx_threadpool_submit_task(vtx_threadpool_t *pool,
                                const vtx_compile_task_t *task)
{
    VTX_ASSERT(pool != NULL, "pool must not be NULL");

    if (__atomic_load_n(&pool->shutdown, __ATOMIC_ACQUIRE)) return -1;

    /* Push to the priority queue */
    if (vtx_pq_push(&pool->task_queue, task) != 0) {
        return -1;
    }

    /* Signal one waiting worker */
    pthread_mutex_lock(&pool->pool_mutex);
    pthread_cond_signal(&pool->work_available);
    pthread_mutex_unlock(&pool->pool_mutex);

    __atomic_fetch_add(&pool->total_tasks_submitted, 1, __ATOMIC_RELAXED);

    return 0;
}

/* ========================================================================== */
/* Status                                                                      */
/* ========================================================================== */

uint32_t vtx_threadpool_idle_workers(const vtx_threadpool_t *pool)
{
    VTX_ASSERT(pool != NULL, "pool must not be NULL");
    uint32_t idle = 0;
    for (uint32_t i = 0; i < pool->num_workers; i++) {
        if (pool->workers[i].state == VTX_WORKER_IDLE) {
            idle++;
        }
    }
    return idle;
}

uint32_t vtx_threadpool_queue_depth(const vtx_threadpool_t *pool)
{
    VTX_ASSERT(pool != NULL, "pool must not be NULL");
    return vtx_pq_count(&pool->task_queue);
}

bool vtx_threadpool_is_shutdown(const vtx_threadpool_t *pool)
{
    VTX_ASSERT(pool != NULL, "pool must not be NULL");
    return __atomic_load_n(&pool->shutdown, __ATOMIC_ACQUIRE);
}
