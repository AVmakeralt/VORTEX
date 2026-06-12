#ifndef VORTEX_COMPILE_PHASE_REACT_H
#define VORTEX_COMPILE_PHASE_REACT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>
#include "vortex_config.h"
#include "compile/version.h"
#include "interp/type_feedback.h"

/**
 * VORTEX Phase-Reactive Version Reactivation
 *
 * Programs often exhibit phasic behavior: they alternate between distinct
 * execution phases (e.g., "processing Dog objects" → "processing Cat objects"
 * → back to "processing Dog objects"). When a type guard fails during the
 * Dog phase, VORTEX creates a deoptless continuation for the Cat phase.
 * When the program returns to the Dog phase, the original version works —
 * but the continuation for Cat was evicted to make room. When the Cat phase
 * returns, we must recompile from scratch.
 *
 * Phase-reactive reactivation solves this by parking deoptless versions
 * instead of evicting them. A parked version keeps its compiled code in
 * memory but is marked as inactive. When the same execution phase recurs
 * (detected by the phase detection module), the parked version is
 * reactivated in O(1) — no recompilation needed.
 *
 * Version state extension:
 *
 *   Compiling → Active → Deprecated → Freed
 *                 ↓          ↑
 *               Parked ──────┘
 *                 ↓
 *               Freed (only when memory pressure is high)
 *
 * A Parked version:
 *   - Has valid compiled code (not freed)
 *   - Is not the active version (not callable)
 *   - Is associated with a phase hash (the execution phase it was compiled for)
 *   - Can be reactivated in O(1) when the phase recurs
 *
 * Reactivation flow:
 *   1. Phase detection module detects a phase transition (method changes
 *      from phase A to phase B).
 *   2. Call vtx_phase_react_park(mgr, method_id, current_version, phase_A_hash)
 *      to park the current version for its phase.
 *   3. Call vtx_phase_react_try_reactivate(mgr, method_id, phase_B_hash) to
 *      look for a parked version compiled for phase B.
 *   4. If found: transition the parked version from Parked → Active,
 *      install it as the method's code. Zero recompilation cost.
 *   5. If not found: trigger background compilation for phase B.
 *
 * Thread safety:
 *   - The global manager mutex protects all registry operations
 *   - Individual version state transitions are protected by the version
 *     manager's per-method mutex
 *   - Reactivation is atomic: a version is either Parked or Active, never
 *     in between
 */

/* ========================================================================== */
/* Phase hash                                                                  */
/* ========================================================================== */

/**
 * Maximum number of parked versions per method.
 * Parked versions consume memory but avoid recompilation.
 * This is higher than VTX_DEOPTLESS_MAX_VERSIONS (64, max aggro) because
 * parked versions only retain compiled code (no graph snapshots needed),
 * so they are cheaper to keep around.
 */
#define VTX_PHASE_REACT_MAX_PARKED 16

/**
 * A phase hash: identifies a specific execution phase.
 * Computed from the call stack's type signature at the time
 * of compilation. Same phase hash = same type behavior expected.
 *
 * The hash is computed using FNV-1a over the type feedback for a method,
 * capturing the dominant type at each call site and the shape at each
 * field access site. This is similar to vtx_profile_compute_hash() in
 * deoptless.c but focuses on the current phase's type distribution rather
 * than the full profile history.
 */
typedef uint64_t vtx_phase_hash_t;

/** Sentinel value indicating no phase hash (uninitialized or cleared). */
#define VTX_PHASE_HASH_NONE 0

/* ========================================================================== */
/* Phase-version entry                                                         */
/* ========================================================================== */

/**
 * Phase-version entry: maps a phase hash to a parked version.
 *
 * Each entry records the phase hash that the version was compiled for,
 * a pointer to the parked code version, and metadata for LRU eviction
 * (park time) and reactivation statistics.
 *
 * An entry becomes invalid when its parked version is freed due to
 * memory pressure. The entry slot is then available for reuse.
 */
typedef struct {
    vtx_phase_hash_t      phase_hash;       /* the phase this version was compiled for */
    vtx_code_version_t   *parked_version;   /* the parked version (or NULL if freed) */
    uint32_t              method_id;        /* method this is a version of */
    uint64_t              park_time_ns;     /* when this version was parked (monotonic clock) */
    uint64_t              reactivate_count; /* how many times this version has been reactivated */
    bool                  is_valid;         /* false if the version was freed due to memory pressure */
} vtx_phase_version_entry_t;

/* ========================================================================== */
/* Per-method phase-version registry                                           */
/* ========================================================================== */

/**
 * Per-method phase-version registry.
 * Tracks parked versions for each execution phase of a single method.
 *
 * The entries array is a fixed-size open-addressing table with linear
 * probing. Phase hash collisions are handled by chaining to the next
 * free slot. The table is deliberately small (16 entries) because:
 *   1. Most methods have 2-4 distinct phases
 *   2. Parked versions consume memory, so we cap the total
 *   3. Linear scan over 16 entries is faster than a hash table for
 *      these sizes
 */
typedef struct {
    uint32_t                      method_id;
    vtx_phase_version_entry_t     entries[VTX_PHASE_REACT_MAX_PARKED];
    uint32_t                      entry_count;    /* number of valid entries */

    /* The phase hash of the currently active version.
     * VTX_PHASE_HASH_NONE if no version is currently active or if
     * the active version was not installed via phase-reactive reactivation. */
    vtx_phase_hash_t              current_phase_hash;

    /* Statistics for this method's phase-reactive behavior */
    uint32_t                      total_reactivations;   /* successful reactivations */
    uint32_t                      total_recompilations;  /* recompilations that could have been
                                                            avoided if a version had been parked */
    uint32_t                      total_evictions;       /* parked versions evicted due to memory */
} vtx_phase_version_registry_t;

/* ========================================================================== */
/* Global phase-reactive version manager                                       */
/* ========================================================================== */

/**
 * Initial capacity for the registries array.
 * Grows via realloc as new methods are registered.
 */
#define VTX_PHASE_REACT_INITIAL_CAPACITY 256

/**
 * Default code budget for parked versions (8 MB).
 * When the total size of parked code exceeds this budget, the oldest
 * parked versions are evicted (LRU based on park_time_ns).
 */
#define VTX_PHASE_REACT_DEFAULT_CODE_BUDGET (8 * 1024 * 1024)

/**
 * Global phase-reactive version manager.
 *
 * Manages per-method registries and enforces a global code budget for
 * parked versions. When the budget is exceeded, the least recently parked
 * versions are evicted first (LRU policy based on park_time_ns).
 *
 * Thread safety: all operations on the manager are protected by the
 * manager_mutex. Individual version state transitions are additionally
 * protected by the version manager's per-method mutex.
 */
typedef struct {
    /* Per-method registries: array indexed by method_id.
     * NULL entries indicate methods that have not yet been registered. */
    vtx_phase_version_registry_t *registries;
    uint32_t                      registry_count;    /* highest method_id seen + 1 */
    uint32_t                      registry_capacity; /* allocated capacity */

    /* Total code size budget for parked versions (bytes).
     * Parked versions that exceed this budget trigger LRU eviction. */
    size_t                        parked_code_budget;
    size_t                        parked_code_used;  /* current total bytes of parked code */

    /* Global mutex: protects all fields of this structure and all
     * registries. Must be held for any read or write operation. */
    pthread_mutex_t               manager_mutex;

    /* Global statistics */
    uint32_t                      total_parked_versions;
    uint32_t                      total_reactivated_versions;
    uint32_t                      total_evicted_versions;
} vtx_phase_react_manager_t;

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

/**
 * Initialize the phase-reactive version manager.
 *
 * @param mgr         Manager to initialize
 * @param code_budget Total bytes of code that may be parked before
 *                    eviction kicks in. Use VTX_PHASE_REACT_DEFAULT_CODE_BUDGET
 *                    for the default (8 MB).
 * @return            0 on success, -1 on failure
 */
int vtx_phase_react_manager_init(vtx_phase_react_manager_t *mgr, size_t code_budget);

/**
 * Destroy the phase-reactive version manager and release all resources.
 * Does NOT free the parked code versions themselves — those are owned
 * by the version manager (compile/version.h). This only frees the
 * registry structures.
 *
 * @param mgr  Manager to destroy
 */
void vtx_phase_react_manager_destroy(vtx_phase_react_manager_t *mgr);

/* ========================================================================== */
/* Registry management                                                         */
/* ========================================================================== */

/**
 * Get or create a phase-version registry for a method.
 *
 * The registry is created on first access for a method_id.
 * Subsequent calls with the same method_id return the existing registry.
 *
 * Thread safety: caller must NOT hold manager_mutex (this function
 * acquires it internally).
 *
 * @param mgr        Phase-reactive manager
 * @param method_id  Method ID
 * @return           Pointer to the registry, or NULL on failure
 */
vtx_phase_version_registry_t *vtx_phase_react_get_registry(
    vtx_phase_react_manager_t *mgr, uint32_t method_id);

/* ========================================================================== */
/* Parking and reactivation                                                    */
/* ========================================================================== */

/**
 * Park the current active version for a method's current phase.
 *
 * Called when:
 *   1. A phase transition is detected (the method is about to switch
 *      to a different execution phase)
 *   2. A newer version is installed (the current version becomes deprecated)
 *
 * The version's state is transitioned from Active to Parked. Its compiled
 * code remains in memory and can be reactivated when the same phase recurs.
 *
 * If the registry is full (VTX_PHASE_REACT_MAX_PARKED entries), the
 * oldest parked version (by park_time_ns) is evicted to make room.
 *
 * If memory pressure is high (parked_code_used > parked_code_budget),
 * the oldest parked versions across all methods are evicted before
 * parking the new one.
 *
 * @param mgr         Phase-reactive manager
 * @param method_id   Method ID
 * @param version     The version to park (must be Active or Deprecated)
 * @param phase_hash  The phase hash this version was compiled for
 * @return            0 on success, -1 on failure
 */
int vtx_phase_react_park(vtx_phase_react_manager_t *mgr,
                          uint32_t method_id,
                          vtx_code_version_t *version,
                          vtx_phase_hash_t phase_hash);

/**
 * Try to reactivate a parked version for a given phase.
 *
 * Looks up the phase hash in the method's registry. If a valid parked
 * version exists, it is transitioned from Parked → Active and returned.
 * The reactivation is O(1): no recompilation, no code generation.
 *
 * If no parked version exists for the given phase, returns NULL.
 * The caller should then trigger background compilation for the new phase.
 *
 * Thread safety: acquires manager_mutex internally. The caller is
 * responsible for installing the reactivated version as the method's
 * active code (via the version manager).
 *
 * @param mgr         Phase-reactive manager
 * @param method_id   Method ID
 * @param phase_hash  The phase hash to look up
 * @return            The reactivated version, or NULL if no parked version exists
 */
vtx_code_version_t *vtx_phase_react_try_reactivate(
    vtx_phase_react_manager_t *mgr,
    uint32_t method_id,
    vtx_phase_hash_t phase_hash);

/* ========================================================================== */
/* Eviction                                                                    */
/* ========================================================================== */

/**
 * Evict the oldest parked version for a method (LRU eviction).
 *
 * Finds the entry with the smallest park_time_ns and frees the
 * associated parked version. The entry is marked as invalid and
 * the slot becomes available for reuse.
 *
 * Called when the per-method registry is full, or when global
 * memory pressure requires eviction.
 *
 * @param mgr        Phase-reactive manager
 * @param method_id  Method ID
 * @return           0 on success, -1 if no valid parked version to evict
 */
int vtx_phase_react_evict_oldest(vtx_phase_react_manager_t *mgr,
                                  uint32_t method_id);

/**
 * Evict parked versions globally until the code budget is satisfied.
 *
 * Iterates over all registries and evicts the oldest parked versions
 * (by park_time_ns) until parked_code_used <= parked_code_budget.
 *
 * This is called after parking a new version to ensure the budget
 * constraint is maintained.
 *
 * @param mgr  Phase-reactive manager
 * @return     Number of versions evicted
 */
uint32_t vtx_phase_react_evict_for_budget(vtx_phase_react_manager_t *mgr);

/**
 * Check if memory pressure requires eviction.
 *
 * Returns true if the total size of parked code exceeds the budget.
 *
 * @param mgr  Phase-reactive manager
 * @return     true if eviction is needed
 */
bool vtx_phase_react_needs_eviction(const vtx_phase_react_manager_t *mgr);

/* ========================================================================== */
/* Phase hash computation                                                      */
/* ========================================================================== */

/**
 * Compute a phase hash from the current execution context.
 *
 * The hash captures the type signature of the current call stack:
 *   1. The dominant type at each call site (top type from type_freq)
 *   2. The shape at each field access site
 *
 * This is similar to vtx_profile_compute_hash() in deoptless.c but
 * focuses on the current phase's type distribution rather than the
 * full profile history. The hash uses FNV-1a for robust distribution.
 *
 * Two execution contexts with the same phase hash are expected to
 * exhibit the same type behavior, so a version compiled for one
 * should work for the other without recompilation.
 *
 * @param type_feedback  Type feedback data for the method
 * @param method_id      Method ID
 * @return               Phase hash, or VTX_PHASE_HASH_NONE on failure
 */
vtx_phase_hash_t vtx_phase_react_compute_hash(
    const vtx_type_feedback_t *type_feedback,
    uint32_t method_id);

/* ========================================================================== */
/* Statistics                                                                  */
/* ========================================================================== */

/**
 * Get global phase-reactive statistics.
 *
 * @param mgr               Phase-reactive manager
 * @param total_parked      Output: total parked versions currently held
 * @param total_reactivated Output: total successful reactivations
 * @param total_evicted     Output: total evicted versions
 */
void vtx_phase_react_get_stats(const vtx_phase_react_manager_t *mgr,
                                uint32_t *total_parked,
                                uint32_t *total_reactivated,
                                uint32_t *total_evicted);

/**
 * Get per-method phase-reactive statistics.
 *
 * @param registry           Per-method registry
 * @param reactivations      Output: successful reactivations for this method
 * @param recompilations     Output: recompilations that could have been avoided
 * @param evictions          Output: evicted parked versions for this method
 */
void vtx_phase_react_get_method_stats(const vtx_phase_version_registry_t *registry,
                                       uint32_t *reactivations,
                                       uint32_t *recompilations,
                                       uint32_t *evictions);

#endif /* VORTEX_COMPILE_PHASE_REACT_H */
