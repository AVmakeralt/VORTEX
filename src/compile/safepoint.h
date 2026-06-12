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
/* Global safepoint flag (polled by JIT-compiled code at loop back-edges)      */
/* ========================================================================== */

/**
 * Global safepoint flag. Set to non-zero by the GC/runtime when it needs
 * all threads to reach a safepoint. JIT-compiled code polls this flag at
 * every loop back-edge with:
 *   cmpq [vtx_safepoint_flag], 0
 *   jne  deopt_stub
 */
extern volatile int vtx_safepoint_flag;

/** Poll the safepoint flag. Called by JIT-compiled code at loop back-edges. */
static inline bool vtx_safepoint_should_stop(void) {
    return vtx_safepoint_flag != 0;
}

/* ========================================================================== */
/* Guard page safepoint polling (zero-cost deopt)                              */
/* ========================================================================== */

/**
 * Guard page safepoint mechanism — replaces CMP+JCC with a single MOV load.
 *
 * Instead of:
 *   cmpq [vtx_safepoint_flag], 0    ; 8 bytes, 1 uop + branch
 *   jne  deopt_stub                  ; 6 bytes, 1 uop (branch prediction)
 *
 * Emit:
 *   movq rax, [guard_page]           ; 6-8 bytes, 1 uop, no branch
 *
 * When no safepoint is requested, the guard_page is readable (PROT_READ)
 * and the MOV completes normally — the loaded value is ignored.
 * When a safepoint is needed, the runtime calls mprotect(guard_page, ...,
 * PROT_NONE) which makes the page inaccessible. The next MOV from the
 * guard page triggers SIGSEGV, which the signal handler catches and
 * translates to a safepoint/deopt.
 *
 * Hot-path cost: 1 load (no compare, no branch, no branch-prediction slot).
 * Cold-path cost: SIGSEGV signal delivery + handler (~1-2 microseconds).
 *
 * The guard page is a single 4KB page allocated at initialization.
 * All JIT-compiled code shares the same guard page.
 */

/** Maximum number of deopt info entries the SIGSEGV handler can look up */
#define VTX_GUARD_PAGE_DEOPT_TABLE_SIZE 4096

/** Null-page detection threshold — any fault address below this is a null deref */
#define VTX_NULL_PAGE_LIMIT  0x10000UL  /* 64KB — covers null + small offsets */

/**
 * Guard page deopt table entry — maps a code range to deopt metadata.
 * When a SIGSEGV occurs, the handler scans this table to find which
 * compiled method the fault occurred in, then performs deopt.
 */
typedef struct {
    const uint8_t *code_start;    /* start of compiled code */
    uint32_t       code_size;     /* size of compiled code */
    uint32_t       method_id;     /* method ID for deopt lookup */
    uint32_t       side_table_index; /* index into the method's side table */
} vtx_guard_page_deopt_entry_t;

/**
 * Initialize the guard page safepoint mechanism.
 * Allocates a single page with mmap and sets it PROT_READ.
 * Registers SIGSEGV and SIGTRAP signal handlers.
 *
 * @return 0 on success, -1 on failure
 */
int vtx_guard_page_init(void);

/**
 * Destroy the guard page and restore original signal handlers.
 */
void vtx_guard_page_destroy(void);

/**
 * Get the address of the guard page for RIP-relative MOV emission.
 * Returns NULL if the guard page has not been initialized.
 */
void *vtx_guard_page_address(void);

/**
 * Trigger a safepoint by making the guard page inaccessible.
 * All JIT-compiled code that polls the guard page will SIGSEGV
 * on their next poll, entering the signal handler which performs
 * the safepoint check.
 *
 * @return 0 on success, -1 on failure
 */
int vtx_guard_page_arm(void);

/**
 * Disarm the safepoint by making the guard page readable again.
 * Called after the safepoint operation completes.
 *
 * @return 0 on success, -1 on failure
 */
int vtx_guard_page_disarm(void);

/**
 * Register a compiled method's code range in the deopt table.
 * The SIGSEGV handler uses this to map fault addresses to deopt info.
 *
 * @param code_start   Start address of compiled code
 * @param code_size    Size of compiled code in bytes
 * @param method_id    Method ID for deopt lookup
 * @param side_table_index Side table entry index for the safepoint
 * @return             0 on success, -1 on table full
 */
int vtx_guard_page_register_code(const uint8_t *code_start, uint32_t code_size,
                                   uint32_t method_id, uint32_t side_table_index);

/**
 * Unregister a compiled method's code range from the deopt table.
 *
 * @param code_start   Start address of compiled code
 * @return             0 on success, -1 on not found
 */
int vtx_guard_page_unregister_code(const uint8_t *code_start);

/**
 * Check whether the guard page mechanism is initialized and available.
 */
bool vtx_guard_page_is_available(void);

/**
 * Global flag indicating guard page availability.
 * This is set by vtx_guard_page_init() and can be checked by
 * code in other libraries (e.g., isel) without creating a
 * circular dependency. Read with atomic load for thread safety.
 */
extern volatile int vtx_guard_page_available_flag;

/** Inline fast-path check for guard page availability.
 * This avoids a function call and can be used from any
 * translation unit that includes this header. */
static inline bool vtx_guard_page_is_available_inline(void) {
    return __atomic_load_n(&vtx_guard_page_available_flag, __ATOMIC_ACQUIRE) != 0;
}

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
