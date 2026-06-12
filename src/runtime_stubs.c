/*
 * VORTEX JIT Runtime Stubs — Full Implementation
 *
 * These functions are called from JIT-compiled code and the interpreter.
 * They implement type queries, method dispatch (static/virtual/interface),
 * monitor synchronization, exception throwing, and deoptimization.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <pthread.h>
#include "vortex_config.h"
#include "runtime/object.h"
#include "runtime/type_system.h"
#include "runtime/gc.h"
#include "runtime/helpers.h"
#include "baseline/deopt_stubs.h"
#include "interp/dispatch.h"
#include "interp/frame.h"
#include "deopt/side_table.h"
#include "deopt/frame_state.h"

/* ========================================================================== */
/* Global state                                                                */
/* ========================================================================== */

/* Current GC instance — now defined in runtime/gc.c.
 * vtx_get_current_gc() and vtx_set_current_gc() are declared in gc.h. */

/* Current type system instance — access via vtx_get_current_type_system()
 * which is defined in runtime/type_system.c. */

/* Current interpreter instance (for fallback dispatch and exception handling) */
static vtx_interp_t *the_interp = NULL;

vtx_interp_t *vtx_get_current_interp(void) { return the_interp; }
void vtx_set_current_interp(vtx_interp_t *interp) { the_interp = interp; }

/* Global side table reference for deoptimization.
 * The JIT sets this before entering compiled code. */
static vtx_side_table_t *the_side_table = NULL;

vtx_side_table_t *vtx_get_current_side_table(void) { return the_side_table; }
void vtx_set_current_side_table(vtx_side_table_t *st) { the_side_table = st; }

/* Thread-local return value slot for void-returning call helpers.
 * The JIT codegen should be updated to read RAX directly; this slot
 * exists as a fallback for cases where the return value in RAX is
 * clobbered by the register-restore sequence after the call. */
static __thread vtx_value_t vtx_runtime_call_result = VTX_VALUE_UNDEFINED;

vtx_value_t vtx_get_runtime_call_result(void) { return vtx_runtime_call_result; }

/* ========================================================================== */
/* Inline cache pool                                                           */
/* ========================================================================== */

/*
 * The JIT compiler assigns each call site an IC index (the `x` parameter
 * in call_virtual / call_interface).  We maintain a global pool of
 * pre-initialized inline caches indexed by that site ID.
 */

#define VTX_IC_POOL_INITIAL_SIZE 4096

static vtx_inline_cache_t *ic_pool = NULL;
static uint32_t            ic_pool_size = 0;
static pthread_mutex_t     ic_pool_lock = PTHREAD_MUTEX_INITIALIZER;

/* Ensure the pool is large enough to accommodate index `idx`. */
static void ic_pool_ensure(uint32_t idx)
{
    if (idx < ic_pool_size) return;

    pthread_mutex_lock(&ic_pool_lock);
    /* Double-check under lock */
    if (idx < ic_pool_size) {
        pthread_mutex_unlock(&ic_pool_lock);
        return;
    }

    uint32_t new_size = ic_pool_size ? ic_pool_size : VTX_IC_POOL_INITIAL_SIZE;
    while (new_size <= idx) new_size *= 2;

    vtx_inline_cache_t *new_pool = (vtx_inline_cache_t *)realloc(
        ic_pool, new_size * sizeof(vtx_inline_cache_t));
    if (!new_pool) {
        VTX_ASSERT(false, "IC pool allocation failed");
        pthread_mutex_unlock(&ic_pool_lock);
        return;
    }

    /* Initialize newly allocated entries */
    for (uint32_t i = ic_pool_size; i < new_size; i++) {
        vtx_ic_init(&new_pool[i]);
    }

    ic_pool = new_pool;
    ic_pool_size = new_size;
    pthread_mutex_unlock(&ic_pool_lock);
}

static vtx_inline_cache_t *get_ic(uint32_t index)
{
    ic_pool_ensure(index);
    VTX_ASSERT(index < ic_pool_size, "IC index out of bounds after resize");
    return &ic_pool[index];
}

/* ========================================================================== */
/* Synthetic type IDs for non-heap types                                       */
/* ========================================================================== */

/*
 * These use the high-bit range (0xFFFFFFF0+) so they never collide
 * with user-defined type IDs which start at VTX_TYPE_OBJECT (1).
 */
#define VTX_TYPEID_SMI       ((vtx_typeid_t)(0xFFFFFFF0u | VTX_TAG_SMI))       /* 0xFFFFFFF0 */
#define VTX_TYPEID_DOUBLE    ((vtx_typeid_t)(0xFFFFFFF0u | VTX_TAG_DOUBLE))    /* 0xFFFFFFF2 */
#define VTX_TYPEID_BOOL      ((vtx_typeid_t)(0xFFFFFFF0u | VTX_TAG_BOOL))      /* 0xFFFFFFF3 */
#define VTX_TYPEID_NULL      ((vtx_typeid_t)(0xFFFFFFF0u | VTX_TAG_NULL))      /* 0xFFFFFFF4 */
#define VTX_TYPEID_UNDEFINED ((vtx_typeid_t)(0xFFFFFFF0u | VTX_TAG_UNDEFINED)) /* 0xFFFFFFF5 */

/* ========================================================================== */
/* Method signature parsing                                                    */
/* ========================================================================== */

/*
 * Count the number of parameters in a JVM-style method signature.
 * E.g. "(II)I" → 2, "(Ljava/lang/String;I)V" → 2, "()V" → 0.
 */
static uint32_t parse_sig_arg_count(const char *sig)
{
    if (sig == NULL || sig[0] != '(') return 0;

    uint32_t count = 0;
    const char *p = sig + 1; /* skip '(' */

    while (*p && *p != ')') {
        switch (*p) {
        case 'B': case 'C': case 'D': case 'F':
        case 'I': case 'J': case 'S': case 'Z':
            /* Primitive — one slot */
            p++;
            count++;
            break;
        case 'L':
            /* Object type: Lclassname; */
            while (*p && *p != ';') p++;
            if (*p == ';') p++;
            count++;
            break;
        case '[':
            /* Array type: skip all '[' then the element type */
            while (*p == '[') p++;
            if (*p == 'L') {
                while (*p && *p != ';') p++;
                if (*p == ';') p++;
            } else {
                p++; /* primitive element type */
            }
            count++;
            break;
        default:
            /* Unknown character — skip */
            p++;
            break;
        }
    }
    return count;
}

/* Maximum number of argument words we handle per call site.
 * If a method has more parameters, only the first N are forwarded. */
#define VTX_MAX_CALL_ARGS 8

/* ========================================================================== */
/* vtx_runtime_typeof                                                          */
/* ========================================================================== */

vtx_typeid_t vtx_runtime_typeof(vtx_value_t v)
{
    /* Non-NaN-boxed values are raw doubles (exponent != 0x7FF) */
    if (!vtx_is_nan_boxed(v)) {
        return VTX_TYPEID_DOUBLE;
    }

    /* NaN-boxed: extract the 3-bit tag */
    uint64_t tag = v & VTX_TAG_MASK;
    switch (tag) {
    case VTX_TAG_SMI:
        return VTX_TYPEID_SMI;

    case VTX_TAG_HEAP_PTR: {
        /* Extract the heap object and return its type_id */
        vtx_heap_object_t *obj = (vtx_heap_object_t *)vtx_heap_ptr(v);
        VTX_ASSERT(obj != NULL, "heap pointer must not be NULL");
        return obj->type_id;
    }

    case VTX_TAG_DOUBLE:
        return VTX_TYPEID_DOUBLE;

    case VTX_TAG_BOOL:
        return VTX_TYPEID_BOOL;

    case VTX_TAG_NULL:
        return VTX_TYPEID_NULL;

    case VTX_TAG_UNDEFINED:
        return VTX_TYPEID_UNDEFINED;

    default:
        VTX_ASSERT(false, "unknown NaN-boxing tag in vtx_runtime_typeof");
        return VTX_TYPE_INVALID;
    }
}

/* ========================================================================== */
/* vtx_runtime_call_static                                                     */
/* ========================================================================== */

/*
 * Static method dispatch.
 *
 * The JIT passes the method descriptor and the method's actual arguments
 * as variadic parameters.  We extract them, then:
 *   1. If compiled_code is available, call it directly.
 *   2. Otherwise, fall back to the interpreter entry point.
 *
 * The compiled_code entry point signature is:
 *   vtx_value_t entry(const vtx_method_desc_t *method,
 *                     vtx_value_t *args, uint32_t argc)
 *
 * The interpreter fallback uses vtx_interp_run().
 */
void vtx_runtime_call_static(const void *m, ...)
{
    const vtx_method_desc_t *method = (const vtx_method_desc_t *)m;
    VTX_ASSERT(method != NULL, "method descriptor must not be NULL");

    va_list ap;
    va_start(ap, m);

    /* Determine argument count from the precomputed field or signature */
    uint32_t argc = method->arg_count;
    if (argc == 0 && method->signature != NULL) {
        argc = parse_sig_arg_count(method->signature);
    }
    if (argc > VTX_MAX_CALL_ARGS) argc = VTX_MAX_CALL_ARGS;

    vtx_value_t args[VTX_MAX_CALL_ARGS];
    for (uint32_t i = 0; i < argc; i++) {
        args[i] = va_arg(ap, vtx_value_t);
    }
    va_end(ap);

    if (method->compiled_code != NULL) {
        /* Call JIT-compiled code directly.
         * The entry point receives (method, args_array, argc). */
        typedef vtx_value_t (*compiled_entry_t)(const vtx_method_desc_t *,
                                                 vtx_value_t *, uint32_t);
        compiled_entry_t entry = (compiled_entry_t)method->compiled_code;
        vtx_runtime_call_result = entry(method, args, argc);
        return;
    }

    /* Fallback: invoke the interpreter */
    if (the_interp != NULL) {
        vtx_runtime_call_result = vtx_interp_run(the_interp, method, args, argc);
        return;
    }

    /* No way to execute the method — fatal error */
    VTX_ASSERT(false, "no compiled code or interpreter available for static call");
    abort();
}

/* ========================================================================== */
/* vtx_runtime_call_virtual                                                    */
/* ========================================================================== */

/*
 * Virtual dispatch with inline cache.
 *
 * Parameters:
 *   x = inline cache index (assigned by the JIT at compile time)
 *   m = method descriptor for the call site (provides name, signature)
 *   ... = variadic arguments; the first is the receiver object
 *
 * Algorithm:
 *   1. Extract the receiver and its type_id
 *   2. Probe the inline cache for a hit
 *   3. On miss, walk the vtable (vtx_type_resolve_method) and update the IC
 *   4. Dispatch to compiled_code or the interpreter
 */
void vtx_runtime_call_virtual(uint32_t x, const void *m, ...)
{
    const vtx_method_desc_t *method = (const vtx_method_desc_t *)m;
    VTX_ASSERT(method != NULL, "method descriptor must not be NULL");
    VTX_ASSERT(vtx_get_current_type_system() != NULL, "type system must be initialized");

    va_list ap;
    va_start(ap, m);

    /* The receiver is always the first variadic argument */
    vtx_value_t receiver = va_arg(ap, vtx_value_t);

    /* Collect remaining method arguments */
    uint32_t sig_argc = method->arg_count;
    if (sig_argc == 0 && method->signature != NULL) {
        sig_argc = parse_sig_arg_count(method->signature);
    }
    /* The signature includes the receiver for virtual calls,
     * so method params = sig_argc - 1 (if sig_argc > 0).
     * We already extracted the receiver, so collect the rest. */
    uint32_t param_count = sig_argc > 0 ? sig_argc - 1 : 0;
    if (param_count > VTX_MAX_CALL_ARGS - 1) {
        param_count = VTX_MAX_CALL_ARGS - 1;
    }

    /* Build the full arg array: [receiver, param0, param1, ...] */
    vtx_value_t args[VTX_MAX_CALL_ARGS];
    args[0] = receiver;
    uint32_t call_argc = 1;

    for (uint32_t i = 0; i < param_count; i++) {
        args[call_argc] = va_arg(ap, vtx_value_t);
        call_argc++;
    }
    va_end(ap);

    /* Resolve the virtual method using inline cache */
    vtx_inline_cache_t *ic = get_ic(x);

    const vtx_method_desc_t *resolved = vtx_helpers_resolve_virtual(
        vtx_get_current_type_system(), ic, receiver, method->name);

    if (resolved == NULL) {
        VTX_ASSERT(false, "virtual method not found during dispatch");
        vtx_runtime_call_result = vtx_make_undefined();
        return;
    }

    /* Dispatch to compiled code or interpreter */
    if (resolved->compiled_code != NULL) {
        typedef vtx_value_t (*compiled_entry_t)(const vtx_method_desc_t *,
                                                 vtx_value_t *, uint32_t);
        compiled_entry_t entry = (compiled_entry_t)resolved->compiled_code;
        vtx_runtime_call_result = entry(resolved, args, call_argc);
        return;
    }

    if (the_interp != NULL) {
        vtx_runtime_call_result = vtx_interp_run(the_interp, resolved, args, call_argc);
        return;
    }

    VTX_ASSERT(false, "no compiled code or interpreter for virtual dispatch");
    abort();
}

/* ========================================================================== */
/* vtx_runtime_call_interface                                                  */
/* ========================================================================== */

/*
 * Interface dispatch with inline cache.
 *
 * Similar to virtual dispatch but also verifies that the receiver's type
 * implements the target interface.  On IC miss, we walk the itable
 * (interface table) of the receiver's type hierarchy.
 *
 * Since the current API does not pass the interface typeid directly,
 * we use vtx_type_resolve_method which walks the full type hierarchy.
 * The IC is separate from the virtual dispatch ICs (different index space),
 * so polymorphic interface calls are cached independently.
 */
void vtx_runtime_call_interface(uint32_t x, const void *m, ...)
{
    const vtx_method_desc_t *method = (const vtx_method_desc_t *)m;
    VTX_ASSERT(method != NULL, "method descriptor must not be NULL");
    VTX_ASSERT(vtx_get_current_type_system() != NULL, "type system must be initialized");

    va_list ap;
    va_start(ap, m);

    vtx_value_t receiver = va_arg(ap, vtx_value_t);

    uint32_t sig_argc = method->arg_count;
    if (sig_argc == 0 && method->signature != NULL) {
        sig_argc = parse_sig_arg_count(method->signature);
    }
    uint32_t param_count = sig_argc > 0 ? sig_argc - 1 : 0;
    if (param_count > VTX_MAX_CALL_ARGS - 1) {
        param_count = VTX_MAX_CALL_ARGS - 1;
    }

    vtx_value_t args[VTX_MAX_CALL_ARGS];
    args[0] = receiver;
    uint32_t call_argc = 1;

    for (uint32_t i = 0; i < param_count; i++) {
        args[call_argc] = va_arg(ap, vtx_value_t);
        call_argc++;
    }
    va_end(ap);

    /* Get inline cache for this interface call site */
    vtx_inline_cache_t *ic = get_ic(x);

    const vtx_method_desc_t *resolved = NULL;

    if (vtx_is_heap_ptr(receiver)) {
        vtx_heap_object_t *obj = (vtx_heap_object_t *)vtx_heap_ptr(receiver);
        vtx_typeid_t recv_typeid = obj->type_id;

        /* Try IC lookup first */
        resolved = vtx_ic_lookup(ic, recv_typeid);

        if (resolved == NULL) {
            /* IC miss — walk the itable.
             * Check the receiver's type for the method, walking up
             * the hierarchy and checking interface implementations. */
            resolved = vtx_type_resolve_method(vtx_get_current_type_system(),
                                                recv_typeid, method->name);

            if (resolved != NULL) {
                /* Verify that the receiver's type actually implements
                 * an interface that declares this method.  Walk the
                 * type's interface list to confirm. */
                const vtx_type_desc_t *recv_td =
                    vtx_type_get(vtx_get_current_type_system(), recv_typeid);
                bool implements_interface = false;

                if (recv_td != NULL) {
                    /* Check if any of the implemented interfaces has
                     * a method with this name. */
                    for (uint32_t i = 0; i < recv_td->interface_count; i++) {
                        const vtx_type_desc_t *iface_td =
                            vtx_type_get(vtx_get_current_type_system(),
                                         recv_td->interfaces[i]);
                        if (iface_td != NULL) {
                            for (uint32_t j = 0; j < iface_td->method_count; j++) {
                                if (iface_td->methods[j].method_symbol_id == method->method_symbol_id) {
                                    implements_interface = true;
                                    break;
                                }
                            }
                        }
                        if (implements_interface) break;
                    }

                    /* Also check parent types for interface implementations */
                    if (!implements_interface) {
                        vtx_typeid_t parent = recv_td->parent_type;
                        while (parent != VTX_TYPE_INVALID && !implements_interface) {
                            const vtx_type_desc_t *parent_td =
                                vtx_type_get(vtx_get_current_type_system(), parent);
                            if (parent_td == NULL) break;
                            for (uint32_t i = 0; i < parent_td->interface_count; i++) {
                                const vtx_type_desc_t *iface_td =
                                    vtx_type_get(vtx_get_current_type_system(),
                                                 parent_td->interfaces[i]);
                                if (iface_td != NULL) {
                                    for (uint32_t j = 0; j < iface_td->method_count; j++) {
                                        if (iface_td->methods[j].method_symbol_id == method->method_symbol_id) {
                                            implements_interface = true;
                                            break;
                                        }
                                    }
                                }
                                if (implements_interface) break;
                            }
                            parent = parent_td->parent_type;
                        }
                    }
                }

                if (implements_interface) {
                    /* Update the IC with the new mapping */
                    vtx_ic_update(ic, recv_typeid, resolved);
                } else {
                    /* The receiver doesn't implement the interface.
                     * This is a runtime error (ClassCastException equivalent). */
                    resolved = NULL;
                }
            }
        }
    }

    if (resolved == NULL) {
        VTX_ASSERT(false, "interface method not found during dispatch");
        vtx_runtime_call_result = vtx_make_undefined();
        return;
    }

    /* Dispatch */
    if (resolved->compiled_code != NULL) {
        typedef vtx_value_t (*compiled_entry_t)(const vtx_method_desc_t *,
                                                 vtx_value_t *, uint32_t);
        compiled_entry_t entry = (compiled_entry_t)resolved->compiled_code;
        vtx_runtime_call_result = entry(resolved, args, call_argc);
        return;
    }

    if (the_interp != NULL) {
        vtx_runtime_call_result = vtx_interp_run(the_interp, resolved, args, call_argc);
        return;
    }

    VTX_ASSERT(false, "no compiled code or interpreter for interface dispatch");
    abort();
}

/* ========================================================================== */
/* Monitor (synchronization) implementation                                    */
/* ========================================================================== */

/*
 * Simple mutex-based monitor using a chained hash table that maps
 * object addresses to pthread_mutex_t + recursion state.
 *
 * This is correct but not high-performance.  A production VM would use
 * thin locks / inflated locks / futex-based monitors.
 */

#define VTX_MONITOR_TABLE_BUCKETS 1024

typedef struct vtx_monitor_entry {
    uintptr_t           key;             /* object identity (heap pointer address) */
    pthread_mutex_t     mutex;           /* the monitor lock */
    pthread_t           owner;           /* thread that currently holds the lock */
    int                 recursion_count; /* reentrant lock count */
    struct vtx_monitor_entry *next;      /* hash chain */
} vtx_monitor_entry_t;

/* The hash table */
static vtx_monitor_entry_t *monitor_table[VTX_MONITOR_TABLE_BUCKETS];
static pthread_mutex_t      monitor_global_lock = PTHREAD_MUTEX_INITIALIZER;

/* Hash function for object addresses */
static inline uint32_t monitor_hash(uintptr_t key)
{
    /* Fibonacci hashing for pointer-like keys */
    return (uint32_t)((key * 11400714819323198485ULL) >> 53);
}

/*
 * Find or create the monitor entry for the given object address.
 * Returns the entry with the mutex initialized (but NOT locked).
 */
static vtx_monitor_entry_t *monitor_get_or_create(uintptr_t key)
{
    uint32_t bucket = monitor_hash(key) % VTX_MONITOR_TABLE_BUCKETS;

    /* Search existing chain */
    vtx_monitor_entry_t *entry = monitor_table[bucket];
    while (entry != NULL) {
        if (entry->key == key) return entry;
        entry = entry->next;
    }

    /* Not found — create a new entry */
    entry = (vtx_monitor_entry_t *)malloc(sizeof(vtx_monitor_entry_t));
    if (!entry) {
        VTX_ASSERT(false, "monitor entry allocation failed");
        return NULL;
    }

    entry->key = key;
    entry->owner = 0;
    entry->recursion_count = 0;
    pthread_mutex_init(&entry->mutex, NULL);

    /* Insert at head of chain (under global lock for thread safety) */
    pthread_mutex_lock(&monitor_global_lock);
    entry->next = monitor_table[bucket];
    monitor_table[bucket] = entry;
    pthread_mutex_unlock(&monitor_global_lock);

    return entry;
}

void vtx_runtime_monitor_enter(vtx_value_t obj)
{
    VTX_ASSERT(vtx_is_heap_ptr(obj), "monitor enter requires a heap object");

    uintptr_t key = (uintptr_t)vtx_heap_ptr(obj);
    vtx_monitor_entry_t *entry = monitor_get_or_create(key);

    if (entry == NULL) {
        VTX_ASSERT(false, "failed to create monitor entry");
        return;
    }

    /* Check if this thread already owns the monitor (reentrant) */
    if (pthread_equal(entry->owner, pthread_self())) {
        entry->recursion_count++;
        return;
    }

    /* Block until we acquire the monitor */
    pthread_mutex_lock(&entry->mutex);
    entry->owner = pthread_self();
    entry->recursion_count = 1;
}

void vtx_runtime_monitor_exit(vtx_value_t obj)
{
    VTX_ASSERT(vtx_is_heap_ptr(obj), "monitor exit requires a heap object");

    uintptr_t key = (uintptr_t)vtx_heap_ptr(obj);
    uint32_t bucket = monitor_hash(key) % VTX_MONITOR_TABLE_BUCKETS;

    /* Find the monitor entry */
    vtx_monitor_entry_t *entry = monitor_table[bucket];
    while (entry != NULL) {
        if (entry->key == key) break;
        entry = entry->next;
    }

    if (entry == NULL) {
        VTX_ASSERT(false, "monitor exit: no monitor found for object");
        return;
    }

    VTX_ASSERT(pthread_equal(entry->owner, pthread_self()),
               "monitor exit: current thread does not own this monitor");

    entry->recursion_count--;
    if (entry->recursion_count == 0) {
        /* Fully released */
        entry->owner = 0;
        pthread_mutex_unlock(&entry->mutex);
    }
}

/* ========================================================================== */
/* Exception throwing                                                          */
/* ========================================================================== */

/*
 * vtx_runtime_throw — Walk the stack looking for a catch handler.
 *
 * If found, unwind to that frame and resume at the handler PC.
 * If not found, call abort().
 *
 * The implementation walks the interpreter frame chain (vtx_frame_t)
 * via the_interp->current_frame.  Each frame may have a catch_handler_pc
 * that indicates an active exception handler.  If the handler's catch
 * type matches the exception's type (or is catch-all), we unwind to it.
 *
 * For JIT frames, this function relies on the interpreter frame chain
 * being maintained alongside JIT frames.  A full production VM would
 * need to walk mixed JIT/interpreter frames using stack maps.
 */
void vtx_runtime_throw(vtx_value_t exc)
{
    VTX_ASSERT(!vtx_is_undefined(exc), "cannot throw undefined");

    /* Get the exception's type_id for matching catch handlers */
    vtx_typeid_t exc_typeid = VTX_TYPE_INVALID;
    if (vtx_is_heap_ptr(exc)) {
        vtx_heap_object_t *obj = (vtx_heap_object_t *)vtx_heap_ptr(exc);
        exc_typeid = obj->type_id;
    }

    /* Walk the interpreter frame chain looking for a handler */
    if (the_interp != NULL && the_interp->current_frame != NULL) {
        vtx_frame_t *frame = the_interp->current_frame;

        while (frame != NULL) {
            /* Check if this frame has an active catch handler */
            if (frame->catch_handler_pc != VTX_CATCH_NONE) {
                /* Check if the handler catches this exception type.
                 * catch_type == 0 means catch-all (handles everything).
                 * catch_type != 0 means only exceptions whose type is
                 * a subtype of catch_type are caught. */
                vtx_typeid_t catch_type = frame->catch_type;
                bool handler_matches = false;

                if (catch_type == 0) {
                    /* catch-all: accepts any exception */
                    handler_matches = true;
                } else if (exc_typeid != VTX_TYPE_INVALID && vtx_get_current_type_system() != NULL) {
                    /* Check if the exception type is a subtype of the
                     * catch type using the type system's hierarchy check.
                     * vtx_type_is_subtype walks the parent chain. */
                    handler_matches = vtx_type_is_subtype(vtx_get_current_type_system(),
                                                          exc_typeid, catch_type);
                }
                /* else: non-heap exception with typed handler, or no
                 * type system — can't match, skip this handler. */

                if (!handler_matches) {
                    /* Exception type doesn't match this handler —
                     * continue walking the frame chain. */
                    frame = frame->caller;
                    continue;
                }

                /* Found a matching handler — unwind to this frame */
                the_interp->exception = exc;

                /* Unwind all frames above the handler frame */
                while (the_interp->current_frame != frame) {
                    vtx_frame_t *top = the_interp->current_frame;
                    the_interp->current_frame = top->caller;
                    vtx_frame_destroy(top, &the_interp->frame_stack);
                }

                /* Set the handler PC in the frame */
                frame->exception = exc;
                /* The interpreter dispatch loop will check for pending
                 * exceptions and jump to catch_handler_pc. */
                return;
            }

            frame = frame->caller;
        }
    }

    /* No handler found — uncaught exception */
    fprintf(stderr, "VORTEX: uncaught exception (type_id=%u)\n", exc_typeid);

    /* If we have an interpreter, let it handle the uncaught exception */
    if (the_interp != NULL) {
        vtx_interp_handle_uncaught(the_interp, exc);
        return;
    }

    /* Last resort: abort */
    abort();
}

/* ========================================================================== */
/* Deoptimization handler                                                      */
/* ========================================================================== */

/*
 * vtx_deopt_handler_stub — Real deoptimization.
 *
 * Called from JIT-compiled code when a guard fails and no custom deopt
 * stub has been registered.  The calling convention (System V AMD64 ABI):
 *   RDI = frame_state_index  (which FrameState to use for reconstitution)
 *   RSI = native_pc          (offset where the failure occurred)
 *
 * Algorithm:
 *   1. Obtain the JIT frame pointer (RBP from the calling JIT frame)
 *   2. Call vtx_deopt_runtime_transition to reconstruct the interpreter
 *      frame from the JIT frame
 *   3. Transfer control to the interpreter dispatch loop
 *
 * If no side table or interpreter is available, we fall back to the
 * diagnostic abort (the previous stub behavior).
 */
void vtx_deopt_handler_stub(uint32_t frame_state_index, uint32_t native_pc)
{
    /*
     * Attempt to use the full deoptimization path.
     *
     * The JIT frame's RBP is available as the caller's frame pointer.
     * On x86-64 with GCC/Clang, __builtin_frame_address(0) gives us
     * the C function's frame pointer, and the JIT frame's RBP is one
     * level up.  However, the most reliable approach is to use the
     * side table + deopt_runtime_transition mechanism.
     */

    /* Step 1: Look up the FrameState from the side table */
    if (the_side_table != NULL) {
        vtx_frame_state_t *fs =
            vtx_side_table_get_frame_state(the_side_table, frame_state_index);

        if (fs != NULL) {
            /*
             * We have the frame state.  Now we need to reconstruct
             * the interpreter frame and transfer control.
             *
             * Get the JIT frame pointer.  The JIT code that called us
             * set up RBP to point to its frame.  We can recover it by
             * walking up one frame from our own.
             *
             * Bug #14 fix: __builtin_frame_address(1) (caller's frame
             * address) is undefined behavior per the C standard when
             * inlined or optimized. Replace with a portable stack walk
             * that reads from the saved RBP chain.
             *
             * On x86-64 with frame pointers, each frame's RBP points to
             * the saved RBP of the caller. So:
             *   *(void **)frame_pointer = caller's RBP
             * We use __builtin_frame_address(0) for our own frame, then
             * walk the RBP chain to find the JIT frame (our caller). */
            void *our_frame = __builtin_frame_address(0);
            void *caller_frame = *(void **)our_frame;  /* walk the RBP chain */
            void *jit_rbp = caller_frame;
            if (jit_rbp != NULL) {
                /* Step 2: Call the runtime transition function to
                 * reconstruct the interpreter frame. */
                void *interp_frame =
                    vtx_deopt_runtime_transition(jit_rbp, native_pc);

                if (interp_frame != NULL) {
                    /* Step 3: Transfer control to the interpreter.
                     *
                     * If the interpreter has set up a re-entry point
                     * (via vtx_deopt_set_interp_entry), we jump to it.
                     * Otherwise, we set up the interpreter's current
                     * frame and return (the interpreter loop will
                     * resume on the next iteration). */
                    vtx_interp_entry_t interp_entry =
                        vtx_deopt_get_interp_entry();

                    if (interp_entry != NULL && the_interp != NULL) {
                        /* Set the interpreter's current frame to the
                         * reconstructed frame */
                        the_interp->current_frame =
                            (vtx_frame_t *)interp_frame;

                        /* Jump to the interpreter dispatch loop.
                         * Since we can't do a direct jump from C,
                         * we call the entry point as a function.
                         * The interpreter entry is expected to
                         * longjmp or setjmp/longjmp back into the
                         * dispatch loop.  For now, we use a simple
                         * approach: set the interpreter state and
                         * return — the calling code will re-enter
                         * the interpreter. */
                        interp_entry();
                        /* If interp_entry returns, something went wrong */
                        VTX_ASSERT(false,
                                   "interpreter entry point returned "
                                   "unexpectedly during deopt");
                        abort();
                    }

                    /* No interpreter entry — set up state and return.
                     * The caller (JIT trampoline) should check for
                     * deopt and re-enter the interpreter. */
                    if (the_interp != NULL) {
                        the_interp->current_frame =
                            (vtx_frame_t *)interp_frame;
                        return;
                    }
                }
            }
        }
    }

    /*
     * Fallback: no side table or interpreter available.
     * Provide diagnostic output and abort (the original stub behavior).
     */
    fprintf(stderr,
            "VORTEX: deoptimization triggered at native_pc=%u, "
            "frame_state_index=%u\n",
            native_pc, frame_state_index);

    if (the_side_table == NULL) {
        fprintf(stderr,
                "VORTEX: No side table available — cannot look up "
                "frame state for deoptimization.\n");
    } else {
        vtx_frame_state_t *fs =
            vtx_side_table_get_frame_state(the_side_table, frame_state_index);
        if (fs != NULL) {
            fprintf(stderr,
                    "VORTEX: FrameState found: method_id=%u, "
                    "bytecode_pc=%u, locals=%u, stack_depth=%u\n",
                    fs->method_id, fs->bytecode_pc,
                    fs->local_count, fs->stack_count);
        } else {
            fprintf(stderr,
                    "VORTEX: FrameState index %u not found in side table.\n",
                    frame_state_index);
        }
    }

    fprintf(stderr,
            "VORTEX: Cannot resume interpretation — aborting.\n");
    abort();
}
