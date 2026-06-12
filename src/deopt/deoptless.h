#ifndef VORTEX_DEOPT_DEOPTLESS_H
#define VORTEX_DEOPT_DEOPTLESS_H

#include <stdint.h>
#include <stdbool.h>
#include "vortex_config.h"
#include "ir/node.h"
#include "ir/graph.h"
#include "runtime/arena.h"
#include "interp/type_feedback.h"

/**
 * VORTEX Deoptless Continuation Versions
 *
 * When a guard fails in compiled code, instead of deoptimizing all the way
 * back to the interpreter (which is expensive), we can generate a "continuation
 * version" of the compiled code with the failed speculation removed.
 *
 * The continuation version is a new compilation of the same method, but with:
 *   - The failed guard removed (replaced with the slow path)
 *   - All optimizations that depended on the failed guard invalidated
 *   - All other guards remain intact
 *
 * When a guard fails at runtime:
 *   1. Instead of jumping to the deopt stub, jump to the continuation version.
 *   2. The continuation version picks up execution at the point after the guard.
 *   3. No interpreter involvement → no re-warming → no re-compilation latency.
 *
 * Benefits:
 *   - Avoids the high cost of full deoptimization (interpreter re-execution)
 *   - Reduces re-compilation pressure
 *   - Maintains compiled-code performance for the remaining speculations
 *
 * Limitations:
 *   - Continuation versions accumulate: each guard failure creates a new version
 *   - Must cap the number of continuation versions to avoid code bloat
 *   - When too many guards have failed, fall back to full deopt
 */

/* ========================================================================== */
/* Guard identification                                                       */
/* ========================================================================== */

/**
 * A guard identifier: the NodeID of the DeoptGuard node in the SoN graph.
 */
typedef vtx_nodeid_t vtx_guard_id_t;

#define VTX_GUARD_ID_INVALID VTX_NODEID_INVALID

/* ========================================================================== */
/* Continuation version                                                       */
/* ========================================================================== */

/**
 * A deoptless continuation version: compiled code with a specific guard removed.
 */
typedef struct vtx_deoptless_version vtx_deoptless_version_t;

struct vtx_deoptless_version {
    uint32_t                method_id;         /* method this is a version of */
    vtx_guard_id_t          failed_guard_id;   /* the guard that was removed */
    void                   *continuation_code; /* native code entry point */
    uint32_t                continuation_size; /* native code size in bytes */
    vtx_deoptless_version_t *next_version;     /* linked list of versions */
    uint32_t                version_number;    /* monotonically increasing version */
    uint32_t                guard_branch_offset; /* offset from code_start to JCC rel32 displacement */
    uint8_t                *code_start;        /* base address of the original compiled code */
    vtx_graph_t         *graph;            /* SoN graph snapshot for this version (for incremental continuation) */
    vtx_deoptless_version_t *parent_version; /* parent version that this continues from */
};

/* ========================================================================== */
/* Deoptless version table                                                    */
/* ========================================================================== */

#define VTX_DEOPTLESS_MAX_VERSIONS 8 /* max continuation versions per method */

/**
 * Per-method version table: tracks all continuation versions for a method.
 */
typedef struct {
    uint32_t                 method_id;
    vtx_deoptless_version_t *versions;        /* linked list of versions */
    uint32_t                 version_count;   /* number of active versions */
    void                    *original_code;   /* original compiled code entry */
    vtx_graph_t             *original_graph;  /* original SoN graph (kept for recompilation) */
    /* Incremental deoptless: track all failed guard IDs across the chain.
     * This enables O(1) lookup of whether a guard has already been removed
     * in any active continuation. */
    vtx_guard_id_t      *failed_guards;      /* array of all failed guard IDs */
    uint32_t             failed_guard_count;  /* number of failed guards tracked */
    uint32_t             failed_guard_capacity; /* allocated capacity */

    /* Profile-guarded specialization (Proposal #13) */
    uint64_t            decision_vector_hash;  /* hash of (guard_id, assumption) pairs */
    uint64_t            compiled_profile_hash; /* hash of the profile at compilation time */
} vtx_deoptless_table_t;

/* ========================================================================== */
/* Continuation creation                                                      */
/* ========================================================================== */

/**
 * Create a continuation version of the graph with the specified guard removed.
 *
 * Steps:
 *   1. Clone the original graph (or start from the last version's graph).
 *   2. Find the DeoptGuard node identified by failed_guard_id.
 *   3. Replace the guard with an unconditional branch (removing the speculation).
 *   4. Invalidate all optimizations that depended on this guard:
 *      - CheckCast nodes that were guarded by this guard become unconditional
 *      - Inlined methods that were guarded by this guard are un-inlined
 *      - Constant-folded values that depended on this guard are un-folded
 *   5. Re-run GVN and DCE on the modified graph.
 *   6. Return the new graph.
 *
 * @param graph          The original SoN graph (NOT modified)
 * @param failed_guard_id The NodeID of the DeoptGuard that failed
 * @param arena          Arena for allocations during graph manipulation
 * @return A new graph with the guard removed, or NULL on failure
 */
vtx_graph_t *vtx_deoptless_create_continuation(vtx_graph_t *graph,
                                                vtx_guard_id_t failed_guard_id,
                                                vtx_arena_t *arena);

/* ========================================================================== */
/* Version installation                                                       */
/* ========================================================================== */

/**
 * Install a deoptless continuation version. This compiles the continuation
 * graph into native code and patches the guard's failure path to jump
 * directly to the continuation code instead of the deopt stub.
 *
 * @param version  The continuation version to install (must have continuation_code set)
 * @return true on success, false on failure
 */
bool vtx_deoptless_install(vtx_deoptless_version_t *version);

/* ========================================================================== */
/* Version table management                                                   */
/* ========================================================================== */

/**
 * Initialize a deoptless version table for a method.
 */
int vtx_deoptless_table_init(vtx_deoptless_table_t *table,
                              uint32_t method_id,
                              void *original_code,
                              vtx_graph_t *original_graph);

/**
 * Destroy a deoptless version table and free all versions.
 */
void vtx_deoptless_table_destroy(vtx_deoptless_table_t *table);

/**
 * Look up a continuation version by the failed guard ID.
 * Returns the version, or NULL if not found.
 */
vtx_deoptless_version_t *vtx_deoptless_find_version(
    const vtx_deoptless_table_t *table,
    vtx_guard_id_t failed_guard_id);

/**
 * Add a continuation version to the table.
 * If the table is at capacity (VTX_DEOPTLESS_MAX_VERSIONS),
 * the oldest version is evicted.
 * Returns the added version, or NULL on failure.
 */
vtx_deoptless_version_t *vtx_deoptless_add_version(
    vtx_deoptless_table_t *table,
    vtx_guard_id_t failed_guard_id,
    void *continuation_code,
    uint32_t continuation_size);

/**
 * Check if deoptless deoptimization is possible for a guard.
 * Returns false if the version table is full and the guard has no version yet.
 */
bool vtx_deoptless_can_deoptless(const vtx_deoptless_table_t *table,
                                  vtx_guard_id_t failed_guard_id);

/**
 * Evict the oldest continuation version from the table.
 * Called when the table is full and a new version is needed.
 */
void vtx_deoptless_evict_oldest(vtx_deoptless_table_t *table);

/* ========================================================================== */
/* Incremental deoptless continuations (Proposal #3)                            */
/* ========================================================================== */

/**
 * Create an incremental continuation: remove a single guard from the
 * latest version's graph (or the original graph if no versions exist).
 *
 * This is the key improvement: instead of generating each continuation
 * from the original graph (which creates combinatorial explosion), we
 * chain them: V2 is generated from V1's graph with one more guard removed.
 *
 * @param table        The deoptless version table
 * @param failed_guard_id  The guard that just failed
 * @param arena        Arena for graph allocations
 * @return             New graph with the guard removed, or NULL on failure
 */
vtx_graph_t *vtx_deoptless_create_incremental_continuation(
    vtx_deoptless_table_t *table,
    vtx_guard_id_t failed_guard_id,
    vtx_arena_t *arena);

/**
 * Find the latest version in the chain that can be used as a base
 * for the next incremental continuation.
 *
 * @param table  The deoptless version table
 * @return       The latest version with a graph, or NULL
 */
vtx_deoptless_version_t *vtx_deoptless_find_latest_version(
    const vtx_deoptless_table_t *table);

/**
 * Check if a guard has already been removed in any active continuation.
 *
 * @param table       The deoptless version table
 * @param guard_id    The guard to check
 * @return            true if this guard has already been removed
 */
bool vtx_deoptless_is_guard_removed(const vtx_deoptless_table_t *table,
                                      vtx_guard_id_t guard_id);

/**
 * Record a failed guard ID in the table's tracking array.
 *
 * @param table       The deoptless version table
 * @param guard_id    The guard that failed
 * @return            0 on success, -1 on failure
 */
int vtx_deoptless_record_failed_guard(vtx_deoptless_table_t *table,
                                        vtx_guard_id_t guard_id);

/**
 * Compute a decision vector hash from the current graph's guard assumptions.
 * The hash encodes all speculation decisions as a compact fingerprint.
 * Two graphs with the same decision vector hash make the same speculative
 * assumptions, so recompilation would produce identical code.
 *
 * @param graph  The SoN graph
 * @return       64-bit hash of the speculation decisions
 */
uint64_t vtx_deoptless_compute_decision_hash(const vtx_graph_t *graph);

/**
 * Check if recompilation is needed by comparing the current profile
 * hash against the compiled profile hash.
 *
 * @param table           Deoptless version table
 * @param current_hash    Hash of the current profile state
 * @return                true if recompilation is needed (hashes differ)
 */
bool vtx_deoptless_needs_recompilation(const vtx_deoptless_table_t *table,
                                         uint64_t current_hash);

/**
 * Compute a 64-bit hash of the current profile state for a method.
 * Used to detect profile changes that might affect speculation decisions.
 *
 * @param type_feedback   Type feedback data
 * @param method_id       Method ID
 * @return                64-bit hash
 */
uint64_t vtx_profile_compute_hash(const vtx_type_feedback_t *type_feedback,
                                    uint32_t method_id);

#endif /* VORTEX_DEOPT_DEOPTLESS_H */
