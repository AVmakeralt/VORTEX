/* ========================================================================== */
/* Coroutines                                                                  */
/* ========================================================================== */
/*
 * runtime/coroutine.h — Coroutine system using ucontext stack switching.
 *
 * Coroutines are cooperative threads. A coroutine has its own stack
 * and execution context. When it calls yield, control returns to the
 * coroutine that resumed it.
 *
 * Lifecycle:
 *   1. coroutine_create(fn, stack_size) → creates a suspended coroutine
 *   2. coroutine_resume(co, args) → starts/resumes the coroutine
 *   3. coroutine_yield(value) → suspends, returns to resumer
 *   4. coroutine_status(co) → "suspended", "running", "dead", "normal"
 *   5. coroutine_close(co) → destroys the coroutine
 *
 * Implementation: uses makecontext/swapcontext (POSIX.1-2001).
 * Each coroutine has its own ucontext_t and stack.
 *
 * Thread safety: coroutines are NOT thread-safe. Use one set per thread.
 * A coroutine must resume and yield on the same OS thread.
 */

#ifndef VORTEX_COROUTINE_H
#define VORTEX_COROUTINE_H

#include "vortex_config.h"
#include "runtime/object.h"
#include <ucontext.h>

/* ========================================================================== */
/* Coroutine states                                                            */
/* ========================================================================== */

typedef enum {
    VTX_CO_SUSPENDED = 0,  /* not running, can be resumed */
    VTX_CO_RUNNING,        /* currently executing */
    VTX_CO_NORMAL,         /* resumed another coroutine (nested resume) */
    VTX_CO_DEAD,           /* finished or errored */
} vtx_coroutine_status_t;

/* ========================================================================== */
/* Coroutine                                                                   */
/* ========================================================================== */

/* Forward declaration — the entry function type. */
typedef vtx_value_t (*vtx_coroutine_fn)(void *user_data, vtx_value_t arg);

typedef struct vtx_coroutine {
    ucontext_t             ctx;           /* this coroutine's context */
    ucontext_t            *caller_ctx;   /* resumer's context (for yield) */
    void                  *stack;         /* allocated stack memory */
    size_t                 stack_size;
    vtx_coroutine_status_t status;
    vtx_coroutine_fn       fn;            /* the function to run */
    void                  *user_data;     /* passed to fn */
    vtx_value_t            resume_arg;   /* value passed to coroutine via resume */
    vtx_value_t            yield_value;   /* value returned from coroutine via yield */
    vtx_value_t            return_value;  /* final return value when coroutine finishes */
} vtx_coroutine_t;

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

/* Create a new coroutine with the given function and stack size.
 * The coroutine starts in SUSPENDED state.
 *
 * Parameters:
 *   fn         - the function to execute in the coroutine
 *   user_data  - opaque pointer passed to fn
 *   stack_size - stack size in bytes (0 = default 64KB)
 *
 * Returns the coroutine, or NULL on failure. */
vtx_coroutine_t *vtx_coroutine_create(vtx_coroutine_fn fn,
                                        void *user_data,
                                        size_t stack_size);

/* Destroy a coroutine and free its stack.
 * Safe to call on NULL or dead coroutines. */
void vtx_coroutine_destroy(vtx_coroutine_t *co);

/* ========================================================================== */
/* Resume and yield                                                            */
/* ========================================================================== */

/* Resume a suspended coroutine, passing `arg` to it.
 * The coroutine runs until it yields or returns.
 * Returns the yielded value (or the final return value if the coroutine finished).
 * Sets *finished_out to true if the coroutine completed (status → DEAD). */
vtx_value_t vtx_coroutine_resume(vtx_coroutine_t *co,
                                   vtx_value_t arg,
                                   bool *finished_out);

/* Yield from the current coroutine, returning `value` to the resumer.
 * Can only be called from within a coroutine (not from the main thread).
 * When the coroutine is resumed again, this returns the new resume arg. */
vtx_value_t vtx_coroutine_yield(vtx_value_t value);

/* Get the current coroutine (NULL if not in a coroutine).
 * Uses thread-local storage. */
vtx_coroutine_t *vtx_coroutine_current(void);

/* ========================================================================== */
/* Status and control                                                          */
/* ========================================================================== */

/* Get the coroutine's status. */
vtx_coroutine_status_t vtx_coroutine_status(const vtx_coroutine_t *co);

/* Get the coroutine's final return value (valid when status == DEAD). */
vtx_value_t vtx_coroutine_return_value(const vtx_coroutine_t *co);

/* Force-close a coroutine (even if suspended). Marks it DEAD.
 * The stack is freed on destroy. */
void vtx_coroutine_close(vtx_coroutine_t *co);

/* Default stack size for coroutines (64KB). */
#define VTX_COROUTINE_DEFAULT_STACK_SIZE (64 * 1024)

#endif /* VORTEX_COROUTINE_H */
