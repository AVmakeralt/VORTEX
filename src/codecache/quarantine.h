#ifndef VORTEX_CODECACHE_QUARANTINE_H
#define VORTEX_CODECACHE_QUARANTINE_H

/* ============================================================================ *
 * AI-MODIFIED CODE
 *
 * This file was originally written by a human developer. It has been
 * substantially modified by an AI assistant (GLM/Z.ai) for bug fixes,
 * performance improvements, and feature additions.
 *
 * Original human-written structure is preserved; AI changes are marked
 * with bug fix IDs (B1-B28) or perf notes (Perf 1-10) in comments.
 *
 * If reviewing, please verify AI changes against the original logic.
 * ============================================================================ */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>

/**
 * VORTEX Code-Cache Quarantine
 * ============================
 *
 * OSR-20 / OSR-21 fix: deferred reclamation of retired compiled-method
 * metadata.
 *
 * When a method is recompiled (vtx_install_method) or invalidated
 * (vtx_invalidate_dependencies), the previous version's code and
 * metadata (side_table, deopt_info, bc_pc_map, poly_ics, dep_*)
 * CANNOT be freed immediately — another thread may be in the middle of
 * `vtx_osr_up` or `vtx_deopt_runtime_transition` and already hold a
 * cached pointer to the old side_table / deopt_info.
 *
 * The quarantine holds retired metadata until a safepoint confirms no
 * thread is in JIT code, at which point `vtx_codecache_quarantine_drain`
 * frees everything safely.
 *
 * The drain is wired into `vtx_gc_safepoint` (after
 * `vtx_safepoint_request_all` stops all mutator threads, before
 * `vtx_gc_collect_young` runs).
 *
 * Design: a singly-linked list of (ptr, free_fn) entries, protected by
 * a mutex. The list is append-only on the retire path (fast) and
 * bulk-freed on the drain path (called rarely — at GC safepoints).
 *
 * The quarantine holds opaque pointers + a destructor function
 * pointer, so it can quarantine arbitrary heap objects without
 * pulling in their type headers.
 */

struct vtx_compiled_method;
struct vtx_side_table;
struct vtx_deopt_info;
struct vtx_bc_pc_map_entry;

/* Generic destructor: takes the opaque pointer and frees it (and any
 * owned sub-objects). Each retire site supplies the appropriate
 * destructor for its object type. */
typedef void (*vtx_quarantine_dtor_fn)(void *ptr);

typedef struct vtx_codecache_quarantine_entry {
    void                              *ptr;        /* object to free */
    vtx_quarantine_dtor_fn            dtor;       /* destructor */
    const char                        *tag;        /* human-readable source */
    struct vtx_codecache_quarantine_entry *next;
} vtx_codecache_quarantine_entry_t;

typedef struct {
    vtx_codecache_quarantine_entry_t *head;
    uint32_t                          entry_count;
    pthread_mutex_t                  mutex;
    bool                              mutex_initialized;
} vtx_codecache_quarantine_t;

/**
 * Initialize a quarantine. Returns 0 on success, -1 on failure.
 */
int vtx_codecache_quarantine_init(vtx_codecache_quarantine_t *q);

/**
 * Destroy the quarantine, freeing all entries immediately (used at
 * shutdown — assumes no other thread is in JIT code).
 */
void vtx_codecache_quarantine_destroy(vtx_codecache_quarantine_t *q);

/**
 * Retire an object to the quarantine. The quarantine takes ownership
 * of `ptr` and will call `dtor(ptr)` when drained.
 *
 * @param q     Quarantine
 * @param ptr   Object to retire (must not be NULL)
 * @param dtor  Destructor that frees ptr (and any owned sub-objects)
 * @param tag   Human-readable label for debugging (e.g. "old_cm install.c")
 */
void vtx_codecache_quarantine_retire(vtx_codecache_quarantine_t *q,
                                       void *ptr,
                                       vtx_quarantine_dtor_fn dtor,
                                       const char *tag);

/**
 * Drain the quarantine: free all retired entries. Caller MUST
 * guarantee no thread is currently in JIT code (typically called
 * from vtx_gc_safepoint after vtx_safepoint_request_all).
 *
 * Returns the number of entries freed.
 */
uint32_t vtx_codecache_quarantine_drain(vtx_codecache_quarantine_t *q);

/**
 * OSR-20/21: adapter that calls vtx_codecache_quarantine_drain on
 * the global quarantine (the one set via vtx_codecache_set_quarantine).
 *
 * Suitable for registering with vtx_gc_set_quarantine_drain_callback
 * so the GC's safepoint can drain the code-cache quarantine without
 * a hard library dependency from vortex_runtime → vortex_codecache.
 *
 * If no global quarantine is registered, this is a no-op.
 */
void vtx_codecache_quarantine_drain_global(void);

/**
 * Get the current number of quarantined entries.
 */
uint32_t vtx_codecache_quarantine_count(const vtx_codecache_quarantine_t *q);

/* ========================================================================== */
/* Global quarantine (singleton)                                              */
/* ========================================================================== */

/**
 * The code cache has a single global quarantine (analogous to the_gc
 * in runtime/gc.c). vtx_install_method and vtx_invalidate_dependencies
 * retire to this global; vtx_gc_safepoint drains it after all mutator
 * threads have reached a safepoint.
 */
vtx_codecache_quarantine_t *vtx_codecache_get_quarantine(void);
void vtx_codecache_set_quarantine(vtx_codecache_quarantine_t *q);

/* ========================================================================== */
/* Common destructors                                                         */
/* ========================================================================== */

/**
 * Destroy a compiled_method_t: frees side_table, deopt_info, bc_pc_map,
 * poly_ics, dep arrays, and the cm struct itself. This is the canonical
 * destructor for OSR-20 (retiring old_cm in vtx_install_method).
 *
 * Forward-declared as `struct vtx_compiled_method` so quarantine.c
 * doesn't need install.h (avoids a header cycle).
 *
 * The cache pointer is stashed in a TLS-like global set via
 * vtx_codecache_set_destroy_cache() before any cm destroy runs, so
 * the destructor can call vtx_code_cache_free for the cm's code.
 */
void vtx_codecache_destroy_compiled_method(void *cm_ptr);

/**
 * Destroy a side_table_t (vtx_side_table_destroy wrapper). Used by
 * OSR-21 in vtx_invalidate_dependencies.
 */
void vtx_codecache_destroy_side_table(void *st_ptr);

/**
 * Destroy a deopt_info_t (free). Used by OSR-21.
 */
void vtx_codecache_destroy_deopt_info(void *di_ptr);

/**
 * Destroy a bc_pc_map array (free). Used by OSR-21.
 */
void vtx_codecache_destroy_bc_pc_map(void *map_ptr);

/**
 * Destroy a poly_ics array: frees each IC then the array. Used by
 * OSR-21 / OSR-20.
 *
 * The `count_ptr` parameter points to the count of ICs (the cm owns
 * poly_ic_count). To avoid leaking the count, the retire site
 * captures (poly_ics, poly_ic_count) into a heap-allocated pair,
 * which this destructor frees.
 */
void vtx_codecache_destroy_poly_ics(void *pair_ptr);

#endif /* VORTEX_CODECACHE_QUARANTINE_H */
