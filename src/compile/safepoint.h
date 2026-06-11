#ifndef VORTEX_COMPILE_SAFEPOINT_H
#define VORTEX_COMPILE_SAFEPOINT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>
#include "vortex_config.h"
#include "codecache/install.h"

/**
 * VORTEX Safe Point Checks
 *
 * Application threads periodically check whether a compilation has
 * completed and needs to be installed, or whether an invalidation
 * has occurred that affects the current method.
 *
 * Safe point checks are inserted at:
 *   - Backward branches (loop back-edges)
 *   - Method calls
 *   - Allocation points
 *
 * The check itself is cheap: read a global flag, branch if set.
 * If the flag is set, the thread enters a safe point handler that:
 *   1. Installs any completed compilations waiting for this thread
 *   2. Checks for invalidations affecting the current method
 *   3. Returns to normal execution
 *
 * Thread safety:
 *   - The global flag is an atomic variable with release/acquire semantics
 *   - Installation requests are protected by a mutex
 *   - The safe point handler is designed to be quick (< 1μs typical)
 */

/* ========================================================================== */
/* Safe point state                                                            */
/* ========================================================================== */

typedef enum {
    VTX_SP_CLEAR          = 0,  /* no pending work */
    VTX_SP_INSTALL_PENDING = 1, /* a compilation is ready to install */
    VTX_SP_INVALIDATE_PENDING = 2, /* an invalidation is pending */
    VTX_SP_ALL_PENDING    = 3   /* both install and invalidate pending */
} vtx_safepoint_state_t;

/* ========================================================================== */
/* Installation request                                                        */
/* ========================================================================== */

/**
 * A pending installation request: a method whose compilation has
 * completed and needs its code pointer updated.
 */
typedef struct vtx_sp_install_request vtx_sp_install_request_t;

struct vtx_sp_install_request {
    uint32_t                 method_id;        /* method to update */
    vtx_compiled_method_t   *compiled_method;  /* new compiled version */
    vtx_sp_install_request_t *next;            /* linked list */
};

/* ========================================================================== */
/* Invalidation request                                                        */
/* ========================================================================== */

/**
 * A pending invalidation request: a method whose compiled code is
 * no longer valid and must be deoptimized.
 */
typedef struct vtx_sp_invalidate_request vtx_sp_invalidate_request_t;

struct vtx_sp_invalidate_request {
    uint32_t                    method_id;     /* method to invalidate */
    vtx_sp_invalidate_request_t *next;         /* linked list */
};

/* ========================================================================== */
/* Safe point manager                                                          */
/* ========================================================================== */

typedef struct {
    /* Global safe point state flag (atomic) */
    volatile vtx_safepoint_state_t state;

    /* Pending installation requests */
    vtx_sp_install_request_t    *install_head;
    vtx_sp_install_request_t    *install_tail;
    pthread_mutex_t              install_mutex;

    /* Pending invalidation requests */
    vtx_sp_invalidate_request_t *invalidate_head;
    vtx_sp_invalidate_request_t *invalidate_tail;
    pthread_mutex_t              invalidate_mutex;

    /* Method registry for installation */
    vtx_method_registry_t       *registry;

    /* Code cache for installation */
    vtx_code_cache_t            *code_cache;

    /* Statistics */
    uint64_t                     total_checks;       /* number of safe point checks */
    uint64_t                     total_installations; /* number of installations performed */
    uint64_t                     total_invalidations; /* number of invalidations performed */
    uint64_t                     total_time_ns;       /* cumulative time in safe point handlers */
} vtx_safepoint_manager_t;

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

/**
 * Initialize the safe point manager.
 *
 * @param manager    Safe point manager to initialize
 * @param registry   Method registry (used for installation lookups)
 * @param code_cache Code cache (used for installation)
 * @return           0 on success, -1 on failure
 */
int vtx_safepoint_init(vtx_safepoint_manager_t *manager,
                        vtx_method_registry_t *registry,
                        vtx_code_cache_t *code_cache);

/**
 * Destroy the safe point manager and free resources.
 */
void vtx_safepoint_destroy(vtx_safepoint_manager_t *manager);

/* ========================================================================== */
/* Safe point check                                                            */
/* ========================================================================== */

/**
 * Perform a safe point check. This is the fast-path function called
 * by application threads at backward branches, calls, and allocations.
 *
 * If the global flag is clear, this is a single load + branch (~1ns).
 * If the flag is set, this enters the slow path to process pending
 * installations and invalidations.
 *
 * @param manager  Safe point manager
 * @param interp   Interpreter state (for invalidation handling)
 * @return         0 if no action needed, 1 if installations were processed,
 *                 -1 if invalidation requires deopt
 */
int vtx_safepoint_check(vtx_safepoint_manager_t *manager, void *interp);

/* ========================================================================== */
/* Installation requests                                                       */
/* ========================================================================== */

/**
 * Request that a compiled method be installed at the next safe point.
 *
 * This is called by the compilation thread when compilation completes.
 * The method's code pointer will be updated atomically at the next
 * safe point check on an application thread.
 *
 * @param manager          Safe point manager
 * @param method_id        Method ID to install
 * @param compiled_method  Compiled method metadata
 * @return                 0 on success, -1 on failure
 */
int vtx_safepoint_request_install(vtx_safepoint_manager_t *manager,
                                   uint32_t method_id,
                                   vtx_compiled_method_t *compiled_method);

/* ========================================================================== */
/* Invalidation requests                                                       */
/* ========================================================================== */

/**
 * Request that a method be invalidated at the next safe point.
 *
 * This is called when a class load or redefinition invalidates
 * compiled code that depends on the old class definition.
 *
 * @param manager   Safe point manager
 * @param method_id Method ID to invalidate
 * @return          0 on success, -1 on failure
 */
int vtx_safepoint_request_invalidate(vtx_safepoint_manager_t *manager,
                                      uint32_t method_id);

/* ========================================================================== */
/* Fast-path inline check                                                      */
/* ========================================================================== */

/**
 * Fast-path safe point check: just reads the global flag.
 * Returns true if a safe point is pending (slow path needed).
 * This is designed to be inlined at every safe point in compiled code.
 */
static inline bool vtx_safepoint_is_pending(const vtx_safepoint_manager_t *manager)
{
    return __atomic_load_n(&manager->state, __ATOMIC_ACQUIRE) != VTX_SP_CLEAR;
}

#endif /* VORTEX_COMPILE_SAFEPOINT_H */
