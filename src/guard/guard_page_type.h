#ifndef VORTEX_GUARD_GUARD_PAGE_TYPE_H
#define VORTEX_GUARD_GUARD_PAGE_TYPE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>
#include "vortex_config.h"

/**
 * VORTEX Guard-Page Type Checking — Zero-Cost Deopt for Type Guards
 *
 * Eliminates type-check CMP+JCC branches on the hot path by using
 * mprotected memory pages indexed by type_id. When the type matches
 * the expected value, a load from a readable page succeeds silently.
 * When the type doesn't match, the load hits a PROT_NONE page,
 * triggering SIGSEGV which the handler translates to a deopt.
 *
 * ====================================================================
 * Design
 * ====================================================================
 *
 * For each type guard, we allocate a contiguous virtual memory region
 * where each type_id T has its own 4KB sub-page at offset T * 4096.
 * The "stride" of 4096 bytes per type_id is the key insight: it lets
 * us selectively mprotect individual type_id slots independently,
 * since mprotect operates on whole pages.
 *
 * Layout for a guard checking type_id = 3, max_type_id = 7:
 *
 *   Offset 0x0000  Page 0 (type_id=0):  PROT_NONE  ← trap
 *   Offset 0x1000  Page 1 (type_id=1):  PROT_NONE  ← trap
 *   Offset 0x2000  Page 2 (type_id=2):  PROT_NONE  ← trap
 *   Offset 0x3000  Page 3 (type_id=3):  PROT_READ  ← pass (expected)
 *   Offset 0x4000  Page 4 (type_id=4):  PROT_NONE  ← trap
 *   Offset 0x5000  Page 5 (type_id=5):  PROT_NONE  ← trap
 *   Offset 0x6000  Page 6 (type_id=6):  PROT_NONE  ← trap
 *   Offset 0x7000  Page 7 (type_id=7):  PROT_NONE  ← trap
 *
 * Emitted code (replaces CMP+JCC):
 *   mov rax, [obj + type_offset]              ; load type_id
 *   mov rax, [guard_base + rax * 4096]        ; trap if wrong type
 *   ; execution continues here only if type_id == expected
 *
 * Hot-path cost: 2 instructions (load + indexed load). No compare,
 * no branch, no branch-prediction slot consumed.
 *
 * Cold-path cost: SIGSEGV signal delivery + handler lookup (~1-2 μs).
 *
 * Memory overhead: (max_type_id + 1) * 4096 bytes of virtual address
 * space. Only the page for the expected type_id is mapped readable;
 * all PROT_NONE pages are lazy — the kernel allocates no physical
 * memory or swap space for them. Typical overhead: ~256KB-2MB of
 * virtual address space (for 64-512 type_ids), with only one 4KB
 * physical page used.
 *
 * ====================================================================
 * SIGSEGV Handler Integration
 * ====================================================================
 *
 * When a SIGSEGV fires from a type guard page, the handler must:
 *   1. Check if the fault address falls in any registered type guard
 *      page region (via vtx_type_guard_page_registry_lookup)
 *   2. If so, extract the guard metadata (method_id, guard_id) and
 *      perform deoptimization
 *   3. If not, fall through to the existing handler chain
 *
 * The registry provides a callback mechanism that the safepoint
 * module's SIGSEGV handler calls before its default handling.
 *
 * ====================================================================
 * Thread Safety
 * ====================================================================
 *
 * The registry is protected by a pthread_mutex. Guard page creation
 * and destruction happen during compilation (rare), while lookups
 * happen in the SIGSEGV handler (signal context). The mutex uses
 * PTHREAD_MUTEX_DEFAULT which is NOT safe in signal context, so
 * the lookup function uses a lock-free scan of a snapshot of the
 * registry that is updated atomically.
 *
 * Specifically:
 *   - Create/destroy: acquire mutex, update the registry array
 *     and the atomic snapshot pointer
 *   - Lookup (from signal handler): read the snapshot pointer
 *     atomically, then scan without locks
 *
 * This ensures that the signal handler never blocks on a mutex
 * held by a compilation thread.
 */

/* ========================================================================== */
/* Constants                                                                    */
/* ========================================================================== */

/**
 * Stride per type_id in the guard page region.
 * Each type_id gets its own 4KB page, enabling independent mprotect.
 */
#define VTX_TYPE_GUARD_STRIDE 4096

/**
 * Maximum number of concurrent type guard pages in the registry.
 * This bounds the scan time in the SIGSEGV handler.
 */
#define VTX_TYPE_GUARD_REGISTRY_MAX_CAPACITY 256

/**
 * Magic value stored in the readable page of a type guard.
 * The SIGSEGV handler can verify this to distinguish type guard
 * faults from other faults in the same address range.
 */
#define VTX_TYPE_GUARD_PAGE_MAGIC 0x4E49414D54594550ULL  /* "TYPEMIND" */

/* ========================================================================== */
/* Type guard page                                                              */
/* ========================================================================== */

/**
 * A single type guard page — one per type check guard in compiled code.
 *
 * The region is allocated with mmap and laid out so that type_id T
 * maps to the page at offset T * VTX_TYPE_GUARD_STRIDE. Only the
 * page for expected_type_id is PROT_READ; all others are PROT_NONE.
 */
typedef struct {
    void           *region_base;      /* mmap'd region start (page-aligned) */
    size_t          region_size;      /* total region size in bytes */
    uint32_t        expected_type_id; /* the type this guard checks for */
    uint32_t        max_type_id;      /* maximum type_id in this region */
    uint32_t        method_id;        /* method this guard belongs to */
    uint32_t        guard_id;         /* guard metadata ID for deopt lookup */
    uint32_t        side_table_index; /* side table entry for deopt */
    bool            is_active;        /* true if the guard page is usable */
} vtx_type_guard_page_t;

/* ========================================================================== */
/* Type guard page registry                                                     */
/* ========================================================================== */

/**
 * Registry of all active type guard pages.
 *
 * The registry is used by the SIGSEGV handler to map fault addresses
 * back to guard metadata for deoptimization. It supports lock-free
 * lookup from signal handler context via an atomic snapshot pointer.
 */
typedef struct {
    /* Mutable state — protected by mutex */
    vtx_type_guard_page_t   pages[VTX_TYPE_GUARD_REGISTRY_MAX_CAPACITY];
    uint32_t                page_count;
    pthread_mutex_t         mutex;

    /* Lock-free snapshot for signal handler lookups.
     * Updated atomically after any mutation (create/destroy).
     * The snapshot is a separately-allocated copy of the pages array
     * that is guaranteed not to be mutated during a signal handler scan.
     * The old snapshot is freed only after a grace period. */
    vtx_type_guard_page_t  *snapshot;
    uint32_t                snapshot_count;

    /* Statistics */
    uint64_t                total_created;
    uint64_t                total_destroyed;
    uint64_t                total_faults_handled;
} vtx_type_guard_page_registry_t;

/* ========================================================================== */
/* Registry lifecycle                                                           */
/* ========================================================================== */

/**
 * Initialize the type guard page registry.
 *
 * Allocates the initial snapshot and initializes the mutex.
 *
 * @param registry  Registry to initialize
 * @return          0 on success, -1 on failure
 */
int vtx_type_guard_page_registry_init(vtx_type_guard_page_registry_t *registry);

/**
 * Destroy the type guard page registry.
 *
 * Destroys all active type guard pages (unmaps their regions),
 * frees the snapshot, and destroys the mutex.
 *
 * @param registry  Registry to destroy
 */
void vtx_type_guard_page_registry_destroy(vtx_type_guard_page_registry_t *registry);

/* ========================================================================== */
/* Type guard page creation and destruction                                     */
/* ========================================================================== */

/**
 * Create a type guard page for a type check.
 *
 * Allocates a virtual memory region of (max_type_id + 1) * 4096 bytes,
 * sets all pages to PROT_NONE, then sets the page for expected_type_id
 * to PROT_READ. The readable page is filled with a magic value for
 * disambiguation.
 *
 * The created guard page is registered in the registry for SIGSEGV
 * handler lookup.
 *
 * @param registry          Type guard page registry
 * @param expected_type_id  The type_id that should pass this guard
 * @param max_type_id       Maximum type_id (determines region size)
 * @param method_id         Method ID this guard belongs to
 * @param guard_id          Guard metadata ID for deopt lookup
 * @param side_table_index  Side table entry for deopt
 * @return                  Pointer to the created guard page, or NULL on failure
 */
vtx_type_guard_page_t *vtx_type_guard_page_create(
    vtx_type_guard_page_registry_t *registry,
    uint32_t expected_type_id,
    uint32_t max_type_id,
    uint32_t method_id,
    uint32_t guard_id,
    uint32_t side_table_index);

/**
 * Destroy a type guard page and unregister it from the registry.
 *
 * Unmaps the virtual memory region and removes the guard page
 * from the registry. The guard page pointer is invalidated.
 *
 * @param registry  Type guard page registry
 * @param page      Type guard page to destroy
 * @return          0 on success, -1 on failure
 */
int vtx_type_guard_page_destroy(vtx_type_guard_page_registry_t *registry,
                                 vtx_type_guard_page_t *page);

/* ========================================================================== */
/* SIGSEGV handler lookup (lock-free, signal-safe)                              */
/* ========================================================================== */

/**
 * Result of looking up a fault address in the type guard page registry.
 *
 * Filled by vtx_type_guard_page_registry_lookup when a SIGSEGV is
 * determined to originate from a type guard page.
 */
typedef struct {
    uint32_t        method_id;        /* method ID for deopt */
    uint32_t        guard_id;         /* guard metadata ID */
    uint32_t        side_table_index; /* side table entry for deopt */
    uint32_t        actual_type_id;   /* type_id that caused the fault */
    uint32_t        expected_type_id; /* type_id that the guard expects */
    bool            found;            /* true if the fault was from a type guard page */
} vtx_type_guard_fault_info_t;

/**
 * Look up a fault address in the type guard page registry.
 *
 * This function is designed to be called from a SIGSEGV handler.
 * It performs a lock-free scan of the registry snapshot to find
 * which type guard page region contains the fault address.
 *
 * If found, the fault_info structure is filled with the guard
 * metadata needed for deoptimization.
 *
 * @param registry    Type guard page registry
 * @param fault_addr  The fault address from siginfo_t::si_addr
 * @param fault_info  Output: filled with guard metadata if found
 * @return            true if the fault was from a type guard page
 */
bool vtx_type_guard_page_registry_lookup(
    const vtx_type_guard_page_registry_t *registry,
    uintptr_t fault_addr,
    vtx_type_guard_fault_info_t *fault_info);

/* ========================================================================== */
/* Guard page access                                                           */
/* ========================================================================== */

/**
 * Get the base address of a type guard page for code emission.
 *
 * The emitted code uses this as the base for indexed loads:
 *   mov rax, [base + type_id * VTX_TYPE_GUARD_STRIDE]
 *
 * @param page  Type guard page
 * @return      Base address, or NULL if the page is not active
 */
void *vtx_type_guard_page_base(const vtx_type_guard_page_t *page);

/**
 * Check if a type guard page is active and usable.
 *
 * @param page  Type guard page
 * @return      true if the page is active
 */
bool vtx_type_guard_page_is_active(const vtx_type_guard_page_t *page);

/* ========================================================================== */
/* Reconfiguration (for adaptive type guards)                                   */
/* ========================================================================== */

/**
 * Reconfigure a type guard page to check a different expected type.
 *
 * This is used when a guard is "re-educated" — e.g., when a type
 * guard that previously checked for type A needs to now check for
 * type B because the call site's type profile has changed.
 *
 * The reconfiguration:
 *   1. Mprotect the old expected_type_id page to PROT_NONE
 *   2. Mprotect the new expected_type_id page to PROT_READ
 *
 * This is safe to call while JIT code is executing: there is a brief
 * window where both pages are PROT_NONE, which would cause a spurious
 * SIGSEGV for the old type. The handler can detect this and retry
 * after a short delay. Alternatively, both pages can be made readable
 * temporarily during the transition (trading strictness for atomicity).
 *
 * @param page              Type guard page to reconfigure
 * @param new_expected_type The new expected type_id
 * @return                  0 on success, -1 on failure
 */
int vtx_type_guard_page_reconfigure(vtx_type_guard_page_t *page,
                                     uint32_t new_expected_type);

/* ========================================================================== */
/* SIGSEGV handler callback registration                                       */
/* ========================================================================== */

/**
 * Callback type for type guard page fault handling.
 *
 * Called from the SIGSEGV handler when a fault is identified as
 * originating from a type guard page. The callback should perform
 * deoptimization using the provided fault info.
 *
 * The callback runs in signal handler context and must be async-signal-safe.
 */
typedef void (*vtx_type_guard_fault_callback_t)(
    const vtx_type_guard_fault_info_t *fault_info,
    void *ucontext);

/**
 * Register a callback for type guard page faults.
 *
 * The callback is invoked from the SIGSEGV handler when a fault
 * originates from a type guard page. Typically, this callback
 * initiates deoptimization for the affected method.
 *
 * @param callback  Function to call on type guard page fault
 * @return          0 on success, -1 if callback was already set
 */
int vtx_type_guard_page_register_fault_callback(
    vtx_type_guard_fault_callback_t callback);

/**
 * Get the currently registered fault callback.
 *
 * Used by the SIGSEGV handler in safepoint.c to check whether
 * type guard page faults should be handled.
 *
 * @return  The registered callback, or NULL if none is set
 */
vtx_type_guard_fault_callback_t vtx_type_guard_page_get_fault_callback(void);

/* ========================================================================== */
/* Global registry access                                                       */
/* ========================================================================== */

/**
 * Get the global type guard page registry.
 *
 * There is a single global registry that is shared across all
 * compilation threads and the SIGSEGV handler. The registry
 * must be initialized before any type guard pages are created.
 *
 * @return  Pointer to the global registry, or NULL if not initialized
 */
vtx_type_guard_page_registry_t *vtx_type_guard_page_get_registry(void);

/**
 * Set the global type guard page registry.
 *
 * Called once during initialization (before any guard pages are created).
 *
 * @param registry  Registry to set as global
 */
void vtx_type_guard_page_set_registry(vtx_type_guard_page_registry_t *registry);

/**
 * Check whether the global type guard page registry is initialized
 * and available for use.
 *
 * This can be checked from isel/guard_emit to determine whether
 * guard-page type checking can be used for a given type guard.
 *
 * @return  true if the registry is initialized and available
 */
bool vtx_type_guard_page_is_available(void);

/**
 * Global flag indicating type guard page availability.
 * Set by vtx_type_guard_page_registry_init(), cleared on destroy.
 * Read with atomic load for thread safety.
 */
extern volatile int vtx_type_guard_page_available_flag;

/**
 * Inline fast-path check for type guard page availability.
 * Avoids a function call; can be used from any translation unit.
 */
static inline bool vtx_type_guard_page_is_available_inline(void) {
    return __atomic_load_n(&vtx_type_guard_page_available_flag, __ATOMIC_ACQUIRE) != 0;
}

#endif /* VORTEX_GUARD_GUARD_PAGE_TYPE_H */
