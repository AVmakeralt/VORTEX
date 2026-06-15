#ifndef VORTEX_COMPILE_THREADPOOL_H
#define VORTEX_COMPILE_THREADPOOL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>
#include "vortex_config.h"
#include "compile/priority.h"
#include "runtime/arena.h"

/**
 * VORTEX Compilation Thread Pool
 *
 * Fixed-size thread pool for concurrent method compilation. Worker threads
 * pull compilation tasks from a shared priority queue.
 *
 * Design:
 *   - VTX_COMPILE_THREADS workers (default = CPU cores - 1, minimum 1)
 *   - Each worker thread has its own arena allocator for compilation
 *   - Workers block on a condition variable when the queue is empty
 *   - Tasks are prioritized by tier, heat, and wait time
 *   - Graceful shutdown: set a shutdown flag, signal all workers,
 *     wait for them to finish their current task
 *
 * Thread safety:
 *   - The priority queue is protected by its own mutex
 *   - The thread pool has a separate mutex for the condition variable
 *   - No locks are held during compilation (each worker has its own arena)
 */

/* ========================================================================== */
/* Worker thread state                                                         */
/* ========================================================================== */

typedef enum {
    VTX_WORKER_IDLE       = 0,  /* waiting for a task */
    VTX_WORKER_COMPILING  = 1,  /* executing a compilation task */
    VTX_WORKER_SHUTDOWN   = 2   /* thread is shutting down */
} vtx_worker_state_t;

typedef struct {
    pthread_t          thread;        /* pthread handle */
    uint32_t           worker_id;     /* sequential worker index */
    vtx_worker_state_t state;         /* current state */
    vtx_arena_t        arena;         /* per-worker arena for compilation */
    uint64_t           tasks_completed; /* number of tasks this worker completed */
    uint32_t           current_method;  /* method currently being compiled (0 if idle) */
    vtx_compile_tier_t current_tier;    /* tier of current compilation */
} vtx_worker_t;

/* ========================================================================== */
/* Thread pool                                                                 */
/* ========================================================================== */

/**
 * Compilation callback: invoked by worker threads when a task has
 * method_id set but task_fn is NULL. The callback looks up the method,
 * runs the pipeline, and installs the compiled code.
 *
 * @param method_id  Method to compile
 * @param tier       Compilation tier
 * @param context    User-provided context (e.g. compile context)
 * @return           0 on success, -1 on failure
 */
typedef int (*vtx_compile_callback_t)(uint32_t method_id,
                                       vtx_compile_tier_t tier,
                                       void *context);

typedef struct {
    vtx_worker_t         *workers;        /* array of worker threads */
    uint32_t              num_workers;    /* number of worker threads */
    bool                  shutdown;       /* shutdown flag */

    vtx_priority_queue_t  task_queue;     /* shared priority queue */

    pthread_mutex_t       pool_mutex;     /* protects shutdown flag and condition */
    pthread_cond_t        work_available; /* signaled when a task is pushed */

    /* Compilation callback: when a task has method_id but no task_fn,
     * the worker invokes this callback to perform the actual compilation.
     * This is the key fix that makes the JIT pipeline functional —
     * previously, tasks submitted with method_id only had task_fn=NULL
     * and were silently discarded by workers. */
    vtx_compile_callback_t compile_callback;
    void                  *compile_callback_context;

    /* Statistics */
    uint64_t              total_tasks_completed;
    uint64_t              total_tasks_submitted;
    uint64_t              total_compile_time_ns; /* cumulative compilation time */
} vtx_threadpool_t;

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

/**
 * Initialize the thread pool with the given number of worker threads.
 * Creates worker threads and starts them waiting for tasks.
 *
 * @param pool        Thread pool to initialize
 * @param num_threads Number of worker threads (0 = use VORTEX_COMPILE_THREADS)
 * @return            0 on success, -1 on failure
 */
int vtx_threadpool_init(vtx_threadpool_t *pool, uint32_t num_threads);

/**
 * Set the compilation callback for the thread pool.
 *
 * When a worker receives a task with method_id set but task_fn == NULL,
 * it calls this callback to perform the compilation. This is the fix
 * for the critical bug where orchestrator-submitted tasks (which have
 * method_id but no task_fn) were silently discarded.
 *
 * Must be called BEFORE starting any compilation tasks.
 *
 * @param pool     Thread pool
 * @param callback Compilation callback function
 * @param context  Context pointer passed to the callback
 */
void vtx_threadpool_set_compile_callback(vtx_threadpool_t *pool,
                                          vtx_compile_callback_t callback,
                                          void *context);

/**
 * Shutdown the thread pool. Signals all workers to stop, waits for
 * them to finish their current task, then destroys resources.
 *
 * @param pool Thread pool to shut down
 */
void vtx_threadpool_shutdown(vtx_threadpool_t *pool);

/* ========================================================================== */
/* Task submission                                                             */
/* ========================================================================== */

/**
 * Submit a compilation task to the thread pool.
 *
 * @param pool     Thread pool
 * @param task_fn  Function to execute for compilation
 * @param arg      Argument to pass to task_fn
 * @param priority Task priority (higher = compiled sooner)
 * @return         0 on success, -1 on failure
 */
int vtx_threadpool_submit(vtx_threadpool_t *pool,
                           void (*task_fn)(void *arg),
                           void *arg,
                           int64_t priority);

/**
 * Submit a full compilation task (with all metadata) to the thread pool.
 *
 * @param pool Thread pool
 * @param task Task to submit
 * @return     0 on success, -1 on failure
 */
int vtx_threadpool_submit_task(vtx_threadpool_t *pool,
                                const vtx_compile_task_t *task);

/* ========================================================================== */
/* Status                                                                      */
/* ========================================================================== */

/**
 * Get the number of idle workers.
 */
uint32_t vtx_threadpool_idle_workers(const vtx_threadpool_t *pool);

/**
 * Get the number of tasks in the queue.
 */
uint32_t vtx_threadpool_queue_depth(const vtx_threadpool_t *pool);

/**
 * Check if the thread pool is shutting down.
 */
bool vtx_threadpool_is_shutdown(const vtx_threadpool_t *pool);

#endif /* VORTEX_COMPILE_THREADPOOL_H */
