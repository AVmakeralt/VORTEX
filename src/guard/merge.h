#ifndef VORTEX_GUARD_MERGE_H
#define VORTEX_GUARD_MERGE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vortex_config.h"
#include "ir/graph.h"
#include "runtime/arena.h"

/**
 * VORTEX Adaptive Guards — Guard Merging
 *
 * Merges adjacent guards that check the same value, reducing the
 * number of runtime checks and deopt points.
 *
 * Merge patterns:
 *
 * 1. Type check merging: Two type checks on the same receiver
 *    → single check against a type set (disjunction of types)
 *    Example:
 *      guard(obj.type == A)  \
 *      guard(obj.type == B)   → guard(obj.type in {A, B})
 *
 * 2. Null check merging: Two null checks on the same value
 *    → one null check (redundant check eliminated)
 *    Example:
 *      guard(x != null)  \
 *      guard(x != null)   → guard(x != null)  // second eliminated
 *
 * 3. Range check merging: Two bounds checks on the same array
 *    with different indices → single check against the wider range
 *    Example:
 *      guard(i >= 0 && i < len)   \
 *      guard(j >= 0 && j < len)    → guard(min(i,j) >= 0 && max(i,j) < len)
 *
 * The merge algorithm works on the SoN graph (not the schedule),
 * scanning all Guard/DeoptGuard nodes and identifying merge candidates.
 *
 * Safety: Merged guards must be at least as strong as the original
 * guards — a merged guard must fail whenever any original guard would
 * fail. This ensures no incorrect deoptimization.
 */

/* ========================================================================== */
/* Merge result                                                                */
/* ========================================================================== */

typedef struct {
    uint32_t guards_merged;       /* number of guards that were merged */
    uint32_t type_checks_merged;  /* type checks merged */
    uint32_t null_checks_merged;  /* null checks merged */
    uint32_t range_checks_merged; /* range checks merged */
    uint32_t guards_eliminated;   /* guards completely eliminated (redundant) */
} vtx_merge_result_t;

/* ========================================================================== */
/* Merge candidate                                                             */
/* ========================================================================== */

typedef enum {
    VTX_MERGE_TYPE_CHECK  = 0,  /* same receiver, different types */
    VTX_MERGE_NULL_CHECK  = 1,  /* same value, both null checks */
    VTX_MERGE_RANGE_CHECK = 2   /* same array, different indices */
} vtx_merge_kind_t;

typedef struct {
    vtx_nodeid_t guard_a;       /* first guard */
    vtx_nodeid_t guard_b;       /* second guard */
    vtx_merge_kind_t kind;      /* type of merge */
} vtx_merge_candidate_t;

/* ========================================================================== */
/* Merge API                                                                   */
/* ========================================================================== */

/**
 * Merge adjacent guards in the graph.
 *
 * @param graph  The SoN graph (modified in place)
 * @param arena  Arena for temporary allocations
 * @return       Result structure with merge statistics
 */
vtx_merge_result_t vtx_merge_guards(vtx_graph_t *graph, vtx_arena_t *arena);

/**
 * Find merge candidates in the graph.
 *
 * Scans all Guard/DeoptGuard nodes and identifies pairs that can
 * be merged. The candidates array is allocated from the arena.
 *
 * @param graph      The SoN graph
 * @param arena      Arena for allocations
 * @param candidates Output: array of merge candidates
 * @param count      Output: number of candidates found
 * @return           0 on success, -1 on failure
 */
int vtx_merge_find_candidates(const vtx_graph_t *graph,
                                vtx_arena_t *arena,
                                vtx_merge_candidate_t **candidates,
                                uint32_t *count);

/**
 * Check if two guard nodes can be merged.
 *
 * @param graph   The SoN graph
 * @param guard_a First guard node
 * @param guard_b Second guard node
 * @return        The kind of merge possible, or -1 if not mergeable
 */
int vtx_merge_check_pair(const vtx_graph_t *graph,
                           vtx_nodeid_t guard_a,
                           vtx_nodeid_t guard_b);

#endif /* VORTEX_GUARD_MERGE_H */
