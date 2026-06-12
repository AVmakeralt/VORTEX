#ifndef VORTEX_IR_BOUNDS_CHECK_H
#define VORTEX_IR_BOUNDS_CHECK_H

#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
#include "ir/graph.h"
#include "ir/schedule.h"
#include "runtime/arena.h"

/**
 * VORTEX Bounds Check Elimination
 *
 * Eliminates redundant array bounds check guards using range analysis
 * and induction variable analysis.
 * A bounds check is redundant if:
 *   - The index's range is provably within the array length
 *   - A dominating guard already proves the same condition
 *   - (NEW) Induction variable analysis proves the index is within
 *     bounds across all loop iterations
 *
 * Algorithm:
 *   1. Run induction variable analysis to identify IVs and their ranges
 *   2. Walk the schedule and find all Guard nodes that are bounds checks
 *      (Guard(i < len) or Guard(i >= 0 && i < len))
 *   3. Build a range analysis: for each node, compute [min, max] range
 *   4. After Guard(i < len), constrain i's max to len-1
 *   5. After Guard(i >= 0), constrain i's min to 0
 *   6. If a subsequent Guard(i < len) sees i in range [0, len-1], eliminate it
 *   7. Also eliminate dominated guards: if Guard(i < len) at block B1
 *      dominates Guard(i < len) at block B2 with the same len, the dominated
 *      guard is redundant
 *   8. (NEW) If the index is an IV whose range is provably within [0, len),
 *      eliminate the guard
 */

/* ========================================================================== */
/* Range type                                                                  */
/* ========================================================================== */

typedef struct {
    int64_t min;       /* minimum value (INT64_MIN = unknown low) */
    int64_t max;       /* maximum value (INT64_MAX = unknown high) */
    bool    is_const;  /* true if min == max */
} vtx_range_t;

#define VTX_RANGE_UNKNOWN ((vtx_range_t){INT64_MIN, INT64_MAX, false})
#define VTX_RANGE_CONST(v) ((vtx_range_t){(v), (v), true})
#define VTX_RANGE(lo, hi) ((vtx_range_t){(lo), (hi), ((lo) == (hi))})

/* Range operations */
static inline vtx_range_t vtx_range_union(vtx_range_t a, vtx_range_t b)
{
    int64_t lo = (a.min < b.min) ? a.min : b.min;
    int64_t hi = (a.max > b.max) ? a.max : b.max;
    return (vtx_range_t){lo, hi, (lo == hi)};
}

static inline vtx_range_t vtx_range_intersect(vtx_range_t a, vtx_range_t b)
{
    int64_t lo = (a.min > b.min) ? a.min : b.min;
    int64_t hi = (a.max < b.max) ? a.max : b.max;
    if (lo > hi) return VTX_RANGE_UNKNOWN; /* empty intersection */
    return (vtx_range_t){lo, hi, (lo == hi)};
}

static inline bool vtx_range_contains(vtx_range_t range, int64_t val)
{
    return range.min <= val && val <= range.max;
}

static inline bool vtx_range_is_within(vtx_range_t idx, vtx_range_t bounds)
{
    /* Check if idx is provably within bounds [bounds.min, bounds.max] */
    return idx.min >= bounds.min && idx.max <= bounds.max;
}

/* ========================================================================== */
/* Range analysis table                                                        */
/* ========================================================================== */

typedef struct {
    vtx_range_t *ranges;     /* array indexed by node ID */
    uint32_t     count;      /* number of entries */
} vtx_range_table_t;

/* ========================================================================== */
/* Bounds check elimination entry point                                        */
/* ========================================================================== */

/**
 * Eliminate redundant bounds check guards using range analysis.
 *
 * @param graph    The Sea-of-Nodes graph
 * @param schedule The schedule (provides block order and dominance)
 * @param arena    Arena allocator for temporary data
 *
 * @return Number of guards eliminated, or -1 on error
 */
int vtx_bounds_check_run(vtx_graph_t *graph, const vtx_schedule_t *schedule, vtx_arena_t *arena);

/**
 * Compute the range for a node based on its opcode and inputs.
 * This is the transfer function for range propagation.
 *
 * @param node   The node to compute range for
 * @param table  The node table
 * @param ranges The current range map (indexed by node ID)
 */
vtx_range_t vtx_bounds_compute_range(const vtx_node_t *node,
                                      const vtx_node_table_t *table,
                                      const vtx_range_t *ranges,
                                      uint32_t range_count);

/**
 * Check if a Guard node represents a bounds check (i < len).
 *
 * A Guard is a bounds check if:
 *   - Its condition input is a Cmp node
 *   - The Cmp compares an index against a length
 *   - The condition is LT (i < len)
 *
 * @param node    The Guard node
 * @param table   The node table
 * @param index   [out] Set to the node ID of the index being checked
 * @param length  [out] Set to the node ID of the array length
 *
 * @return true if this is a bounds check guard
 */
bool vtx_bounds_is_bounds_check(const vtx_node_t *node,
                                 const vtx_node_table_t *table,
                                 vtx_nodeid_t *index,
                                 vtx_nodeid_t *length);

/**
 * Check if a Guard node represents a non-negative check (i >= 0).
 *
 * @param node    The Guard node
 * @param table   The node table
 * @param index   [out] Set to the node ID of the index being checked
 *
 * @return true if this is a non-negative check guard
 */
bool vtx_bounds_is_nonneg_check(const vtx_node_t *node,
                                 const vtx_node_table_t *table,
                                 vtx_nodeid_t *index);

#endif /* VORTEX_IR_BOUNDS_CHECK_H */
