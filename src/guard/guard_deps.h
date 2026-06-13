#ifndef VORTEX_GUARD_GUARD_DEPS_H
#define VORTEX_GUARD_GUARD_DEPS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vortex_config.h"
#include "ir/node.h"
#include "ir/graph.h"
#include "pea/analysis.h"
#include "guard/value_profile.h"

/**
 * VORTEX Guard Dependency Graph — O(dependents) Invalidation on Deopt
 *
 * When a guard fails at runtime and deoptimization occurs, the runtime needs
 * to identify and invalidate exactly the code that depended on the failed
 * guard — not the entire compiled version. This module pre-computes which
 * optimizations depend on each guard, enabling O(dependents) invalidation
 * instead of O(all_nodes) graph traversal.
 *
 * The four categories of guard-dependent optimizations are:
 *
 *   1. Inline sites — method calls that were inlined because this guard
 *      guaranteed the receiver type. If the guard fails, these inlined
 *      methods need to be "un-inlined" (deopt to interpreter at the call site).
 *
 *   2. Scalar replacement objects — allocations that were scalar-replaced
 *      because this guard proved they don't escape. If the guard fails,
 *      these objects need to be materialized back on the heap.
 *
 *   3. Branch PCs — branches that were eliminated (replaced with unconditional
 *      jumps) because this guard proved the branch direction. If the guard
 *      fails, these branches need to be restored.
 *
 *   4. Value sites — loads that were constant-folded because this guard
 *      guaranteed the loaded value. If the guard fails, these loads need
 *      to be re-materialized from memory.
 *
 * Building the graph:
 *   The graph is built during the compilation pipeline, after all optimizations
 *   have been applied. The builder walks the SoN graph and, for each DeoptGuard
 *   node, identifies:
 *     - CheckCast/InstanceOf nodes at the same bytecode_pc
 *     - Data nodes that take the guard's condition as input
 *     - Inlined MethodCall nodes devirtualized by this guard
 *     - Allocate nodes scalar-replaced because this guard proved NoEscape
 *     - If nodes whose branch was eliminated because this guard proved condition
 *     - LoadField nodes constant-folded based on a value guard
 *
 * Thread safety: NOT thread-safe. The caller must synchronize, same as
 * existing guard metadata (vtx_guard_meta_table_t).
 */

/* ========================================================================== */
/* Guard ID                                                                    */
/* ========================================================================== */

/**
 * Invalid guard ID sentinel value.
 * A guard_id is the NodeID of the DeoptGuard/Guard node in the SoN graph.
 * This matches the convention in deopt/deoptless.h where
 * vtx_guard_id_t = vtx_nodeid_t and VTX_GUARD_ID_INVALID = VTX_NODEID_INVALID.
 */
#define VTX_GUARD_DEPS_ID_INVALID VTX_NODEID_INVALID

/* ========================================================================== */
/* Invalidated dependency categories (bitmask)                                 */
/* ========================================================================== */

typedef enum {
    VTX_DEP_INVALIDATE_NONE   = 0,
    VTX_DEP_INVALIDATE_INLINE = 1,    /* dependent inline sites invalidated */
    VTX_DEP_INVALIDATE_SR     = 2,    /* dependent scalar-replaced objects invalidated */
    VTX_DEP_INVALIDATE_BRANCH = 4,    /* dependent branch eliminations invalidated */
    VTX_DEP_INVALIDATE_VALUE  = 8,    /* dependent value speculations invalidated */
    VTX_DEP_INVALIDATE_ALL    = 0xF   /* all categories invalidated */
} vtx_dep_invalidate_mask_t;

/* ========================================================================== */
/* Per-guard dependency record                                                 */
/* ========================================================================== */

/**
 * Records all optimizations that depend on a single guard.
 *
 * Each category uses a dynamically-growing array. The arrays are typically
 * small (0–4 entries per guard) because most guards enable only one or two
 * dependent optimizations. The total_dep_count field enables fast "does this
 * guard have any dependents?" checks without summing all four counts.
 */
typedef struct vtx_guard_deps {
    uint32_t guard_id;                /* which guard this is */

    /* Dependent inline sites: method calls that were inlined because
     * this guard guaranteed the receiver type. If the guard fails,
     * these inlined methods need to be "un-inlined" (deopt to interpreter
     * at the call site). */
    uint32_t *dependent_inline_sites;  /* array of bytecode PCs */
    uint32_t  inline_site_count;
    uint32_t  inline_site_capacity;

    /* Dependent scalar replacement objects: allocations that were
     * scalar-replaced because this guard proved they don't escape.
     * If the guard fails, these objects need to be materialized. */
    uint32_t *dependent_sr_objects;    /* array of node IDs (alloc nodes) */
    uint32_t  sr_object_count;
    uint32_t  sr_object_capacity;

    /* Dependent branch PCs: branches that were eliminated (replaced with
     * unconditional jumps) because this guard proved the branch direction.
     * If the guard fails, these branches need to be restored. */
    uint32_t *dependent_branch_pcs;    /* array of bytecode PCs */
    uint32_t  branch_pc_count;
    uint32_t  branch_pc_capacity;

    /* Dependent value sites: loads that were constant-folded because
     * this guard guaranteed the loaded value. If the guard fails,
     * these loads need to be re-materialized from memory. */
    uint32_t *dependent_value_sites;   /* array of bytecode PCs */
    uint32_t  value_site_count;
    uint32_t  value_site_capacity;

    /* Total number of dependencies (sum of all counts above).
     * Enables fast "any dependents?" check. */
    uint32_t total_dep_count;

    /* Whether this guard's dependencies have been fully computed
     * by the graph builder. Before is_computed = true, the record
     * may contain manually-added dependencies from optimization passes. */
    bool is_computed;
} vtx_guard_deps_t;

/* ========================================================================== */
/* Guard dependency graph (registry)                                           */
/* ========================================================================== */

#define VTX_GUARD_DEPS_INITIAL_CAPACITY  32
#define VTX_GUARD_DEPS_ARRAY_INITIAL_CAP  4

/**
 * The guard dependency graph: maps each guard to its dependent optimizations.
 *
 * The primary mapping is guard_id → vtx_guard_deps_t, stored in a flat array
 * indexed by guard_id. The reverse mapping (node_id → guard_id) enables
 * queries like "which guard does this allocation depend on?" without
 * scanning all guard entries.
 *
 * Statistics track the total number of each dependency category across all
 * guards, useful for profiling and debugging the dependency tracker.
 */
typedef struct vtx_guard_deps_graph {
    vtx_guard_deps_t *deps;          /* array, indexed by guard_id */
    uint32_t          deps_count;
    uint32_t          deps_capacity;

    /* Reverse mapping: node_id → guard_id that the node depends on.
     * Computed by scanning the SoN graph and tracking which guards
     * each data node's type depends on.
     * Value = VTX_GUARD_DEPS_ID_INVALID if the node has no guard dependency. */
    uint32_t *node_to_guard;         /* indexed by node_id */
    uint32_t  node_to_guard_capacity;

    /* Statistics across all guards */
    uint32_t total_inline_deps;
    uint32_t total_sr_deps;
    uint32_t total_branch_deps;
    uint32_t total_value_deps;
} vtx_guard_deps_graph_t;

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

/**
 * Initialize a guard dependency graph.
 * Returns 0 on success, -1 on failure.
 */
int vtx_guard_deps_graph_init(vtx_guard_deps_graph_t *graph);

/**
 * Destroy a guard dependency graph and free all memory.
 * Frees all per-guard dependency arrays and the main arrays.
 */
void vtx_guard_deps_graph_destroy(vtx_guard_deps_graph_t *graph);

/* ========================================================================== */
/* Build the graph from a compiled SoN graph                                   */
/* ========================================================================== */

/**
 * Build the guard dependency graph by walking the SoN graph.
 *
 * This is called after all optimizations have been applied. The builder
 * walks every node in the graph and, for each DeoptGuard node, identifies
 * all optimizations that depend on it:
 *
 *   1. For each DeoptGuard, find CheckCast/InstanceOf nodes at the same
 *      bytecode_pc — these are type narrows guarded by this check.
 *
 *   2. For each inlined CallVirtual/CallInterface that was devirtualized
 *      by this guard (guard's output feeds the call's receiver type check),
 *      record the call's bytecode_pc as a dependent inline site.
 *
 *   3. For each Allocate/NewObject/NewArray that was scalar-replaced
 *      (NoEscape in PEA) because this guard proved the object doesn't
 *      escape, record the alloc node's ID as a dependent SR object.
 *
 *   4. For each If node whose condition input comes (directly or via
 *      a CmpP that takes this guard's result), and whose branch was
 *      eliminated (one successor is unreachable), record the If node's
 *      bytecode_pc as a dependent branch PC.
 *
 *   5. For each LoadField that was constant-folded based on a value
 *      speculation guard, record the load's bytecode_pc as a dependent
 *      value site.
 *
 * Also builds the reverse mapping (node_to_guard) by scanning all nodes
 * and recording which guard each data node depends on.
 *
 * @param graph           The guard dependency graph to populate
 * @param son_graph       The SoN graph (not modified)
 * @param pea             Partial escape analysis results (may be NULL if PEA not run)
 * @param value_profiles  Value profile table (may be NULL if no value speculation)
 * @return                0 on success, -1 on failure
 */
int vtx_guard_deps_build(vtx_guard_deps_graph_t *graph,
                          const vtx_graph_t *son_graph,
                          const vtx_pea_analysis_t *pea,
                          const vtx_value_profile_table_t *value_profiles);

/* ========================================================================== */
/* Query                                                                       */
/* ========================================================================== */

/**
 * Look up the dependency record for a specific guard.
 *
 * Returns a pointer to the guard's dependency record, or NULL if the
 * guard_id is invalid or has no entry in the graph.
 *
 * The returned pointer is valid until the graph is destroyed or the
 * next call to vtx_guard_deps_build().
 */
const vtx_guard_deps_t *vtx_guard_deps_lookup(const vtx_guard_deps_graph_t *graph,
                                                 uint32_t guard_id);

/**
 * Look up which guard a node depends on.
 *
 * Uses the reverse mapping (node_to_guard) for O(1) lookup.
 * Returns VTX_GUARD_DEPS_ID_INVALID if the node has no guard dependency
 * or if the node_id is out of range.
 *
 * A node may depend on at most one guard. If an optimization depends
 * on multiple guards (e.g., inlining that required both a null check
 * and a type check), the "primary" guard (the one that would cause
 * deoptimization) is recorded.
 */
uint32_t vtx_guard_deps_for_node(const vtx_guard_deps_graph_t *graph,
                                   uint32_t node_id);

/* ========================================================================== */
/* Invalidation                                                                */
/* ========================================================================== */

/**
 * Invalidate all optimizations that depended on a failed guard.
 *
 * Given a guard_id (the guard that just failed at runtime), this function
 * returns all nodes/PCs that need invalidation, grouped by category.
 *
 * Output pointers are set to point into the guard's dependency arrays
 * (not copies — caller must not free them). If a category has no
 * dependents, the corresponding output pointer is set to NULL and
 * the count is set to 0.
 *
 * Returns a bitmask of vtx_dep_invalidate_mask_t indicating which
 * categories had dependents that were invalidated. This enables the
 * caller to quickly determine what kind of invalidation work is needed:
 *
 *   uint32_t mask = vtx_guard_deps_invalidate(graph, guard_id, ...);
 *   if (mask & VTX_DEP_INVALIDATE_INLINE) {
 *       // un-inline the call sites
 *   }
 *   if (mask & VTX_DEP_INVALIDATE_SR) {
 *       // materialize the scalar-replaced objects
 *   }
 *
 * @param graph              The guard dependency graph
 * @param guard_id           The guard that failed
 * @param out_inline_sites   Output: array of dependent inline site PCs (or NULL)
 * @param out_inline_count   Output: number of inline sites
 * @param out_sr_objects     Output: array of dependent SR alloc node IDs (or NULL)
 * @param out_sr_count       Output: number of SR objects
 * @param out_branch_pcs     Output: array of dependent branch PCs (or NULL)
 * @param out_branch_count   Output: number of branch PCs
 * @param out_value_sites    Output: array of dependent value site PCs (or NULL)
 * @param out_value_count    Output: number of value sites
 * @return                   Bitmask of invalidated categories
 */
uint32_t vtx_guard_deps_invalidate(vtx_guard_deps_graph_t *graph,
                                     uint32_t guard_id,
                                     uint32_t **out_inline_sites,
                                     uint32_t *out_inline_count,
                                     uint32_t **out_sr_objects,
                                     uint32_t *out_sr_count,
                                     uint32_t **out_branch_pcs,
                                     uint32_t *out_branch_count,
                                     uint32_t **out_value_sites,
                                     uint32_t *out_value_count);

/* ========================================================================== */
/* Manual dependency addition (called during optimization passes)               */
/* ========================================================================== */

/**
 * Add an inline site dependency for a guard.
 *
 * Called by the inliner when a virtual call is devirtualized because
 * this guard proved the receiver type. The bytecode_pc identifies the
 * call site that was inlined.
 *
 * Returns 0 on success, -1 on failure.
 */
int vtx_guard_deps_add_inline(vtx_guard_deps_graph_t *graph,
                                uint32_t guard_id, uint32_t bytecode_pc);

/**
 * Add a scalar-replacement object dependency for a guard.
 *
 * Called by PEA when an allocation is scalar-replaced because this
 * guard proved the object doesn't escape. The alloc_node_id is the
 * NodeID of the allocation node (VTX_OP_NewObject/NewArray/Allocate).
 *
 * Returns 0 on success, -1 on failure.
 */
int vtx_guard_deps_add_sr_object(vtx_guard_deps_graph_t *graph,
                                   uint32_t guard_id, uint32_t alloc_node_id);

/**
 * Add a branch elimination dependency for a guard.
 *
 * Called when a branch is eliminated because this guard proved the
 * condition value. The bytecode_pc identifies the branch that was
 * eliminated.
 *
 * Returns 0 on success, -1 on failure.
 */
int vtx_guard_deps_add_branch(vtx_guard_deps_graph_t *graph,
                                uint32_t guard_id, uint32_t bytecode_pc);

/**
 * Add a value speculation dependency for a guard.
 *
 * Called when a LoadField is constant-folded because this guard
 * guaranteed the loaded value (via value profiling). The bytecode_pc
 * identifies the load that was constant-folded.
 *
 * Returns 0 on success, -1 on failure.
 */
int vtx_guard_deps_add_value_site(vtx_guard_deps_graph_t *graph,
                                    uint32_t guard_id, uint32_t bytecode_pc);

/* ========================================================================== */
/* Statistics                                                                  */
/* ========================================================================== */

/**
 * Get the total number of guards with at least one dependency.
 */
uint32_t vtx_guard_deps_active_guard_count(const vtx_guard_deps_graph_t *graph);

/**
 * Get the total number of dependencies across all guards and categories.
 */
uint32_t vtx_guard_deps_total_dependency_count(const vtx_guard_deps_graph_t *graph);

#endif /* VORTEX_GUARD_GUARD_DEPS_H */
