/* ========================================================================== */
/* Coroutines — Implementation using ucontext                                  */
/* ========================================================================== */

#include "runtime/coroutine.h"
#include <stdlib.h>
#include <string.h>

/* Thread-local current coroutine pointer.
 * NULL when running on the main thread (not in a coroutine). */
static __thread vtx_coroutine_t *g_current_coroutine = NULL;

/* ---- Trampoline for makecontext ----
 *
 * makecontext requires a function with no arguments (or int args).
 * We use a global to pass the coroutine pointer, which is safe because
 * only one coroutine can start at a time (the caller holds the mutex). */
static vtx_coroutine_t *g_starting_co = NULL;

static void coroutine_entry(void)
{
    vtx_coroutine_t *co = g_starting_co;
    g_starting_co = NULL;

    /* Run the function */
    vtx_value_t result = co->fn(co->user_data, co->resume_arg);

    /* Function returned — coroutine is done */
    co->return_value = result;
    co->status = VTX_CO_DEAD;

    /* Return to caller. swapcontext saves our (dead) context and
     * switches to the caller's context. The caller sees status=DEAD
     * and the return_value. */
    g_current_coroutine = NULL;
    setcontext(co->caller_ctx);
    /* setcontext does not return */
}

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

vtx_coroutine_t *vtx_coroutine_create(vtx_coroutine_fn fn,
                                        void *user_data,
                                        size_t stack_size)
{
    if (!fn) return NULL;
    if (stack_size == 0) stack_size = VTX_COROUTINE_DEFAULT_STACK_SIZE;

    vtx_coroutine_t *co = (vtx_coroutine_t *)calloc(1, sizeof(*co));
    if (!co) return NULL;

    co->stack = malloc(stack_size);
    if (!co->stack) {
        free(co);
        return NULL;
    }
    co->stack_size = stack_size;

    co->fn = fn;
    co->user_data = user_data;
    co->status = VTX_CO_SUSPENDED;
    co->resume_arg = VTX_VALUE_UNDEFINED;
    co->yield_value = VTX_VALUE_UNDEFINED;
    co->return_value = VTX_VALUE_UNDEFINED;
    co->caller_ctx = NULL;

    /* Set up the ucontext */
    if (getcontext(&co->ctx) != 0) {
        free(co->stack);
        free(co);
        return NULL;
    }
    co->ctx.uc_stack.ss_sp = co->stack;
    co->ctx.uc_stack.ss_size = stack_size;
    co->ctx.uc_link = NULL;  /* we handle return manually in coroutine_entry */

    /* makecontext requires the entry function and arg count.
     * We pass the coroutine via the global g_starting_co. */
    g_starting_co = co;
    makecontext(&co->ctx, coroutine_entry, 0);

    return co;
}

void vtx_coroutine_destroy(vtx_coroutine_t *co)
{
    if (!co) return;
    free(co->stack);
    free(co);
}

/* ========================================================================== */
/* Resume and yield                                                            */
/* ========================================================================== */

vtx_value_t vtx_coroutine_resume(vtx_coroutine_t *co,
                                   vtx_value_t arg,
                                   bool *finished_out)
{
    if (!co) {
        if (finished_out) *finished_out = true;
        return VTX_VALUE_UNDEFINED;
    }
    if (co->status == VTX_CO_DEAD) {
        if (finished_out) *finished_out = true;
        return co->return_value;
    }

    /* Set up the resume */
    co->resume_arg = arg;
    co->status = VTX_CO_RUNNING;

    /* Save the caller's context (to return to on yield) */
    ucontext_t caller_ctx;
    co->caller_ctx = &caller_ctx;

    /* Save the previous current coroutine (for nesting) */
    vtx_coroutine_t *prev_co = g_current_coroutine;
    g_current_coroutine = co;

    /* Switch to the coroutine */
    swapcontext(&caller_ctx, &co->ctx);

    /* --- We're back in the caller's context ---
     * Either the coroutine yielded or finished. */

    g_current_coroutine = prev_co;

    if (co->status == VTX_CO_DEAD) {
        if (finished_out) *finished_out = true;
        return co->return_value;
    } else {
        /* Coroutine yielded — restore suspended status */
        co->status = VTX_CO_SUSPENDED;
        if (finished_out) *finished_out = false;
        return co->yield_value;
    }
}

vtx_value_t vtx_coroutine_yield(vtx_value_t value)
{
    vtx_coroutine_t *co = g_current_coroutine;
    if (!co) {
        /* yield called outside a coroutine — no-op */
        return VTX_VALUE_UNDEFINED;
    }

    /* Store the yield value for the resumer */
    co->yield_value = value;
    co->status = VTX_CO_SUSPENDED;

    /* Switch back to the caller's context */
    ucontext_t *caller_ctx = co->caller_ctx;
    g_current_coroutine = NULL;
    swapcontext(&co->ctx, caller_ctx);

    /* --- We're back in the coroutine's context ---
     * The resumer called resume() again, which set co->resume_arg. */

    /* Restore current coroutine pointer */
    g_current_coroutine = co;
    co->status = VTX_CO_RUNNING;

    return co->resume_arg;
}

vtx_coroutine_t *vtx_coroutine_current(void)
{
    return g_current_coroutine;
}

/* ========================================================================== */
/* Status and control                                                          */
/* ========================================================================== */

vtx_coroutine_status_t vtx_coroutine_status(const vtx_coroutine_t *co)
{
    return co ? co->status : VTX_CO_DEAD;
}

vtx_value_t vtx_coroutine_return_value(const vtx_coroutine_t *co)
{
    return co ? co->return_value : VTX_VALUE_UNDEFINED;
}

void vtx_coroutine_close(vtx_coroutine_t *co)
{
    if (!co) return;
    co->status = VTX_CO_DEAD;
}
