#ifndef VORTEX_BUDGET_H
#define VORTEX_BUDGET_H

#include <stdint.h>
#include <stdbool.h>
#include "vortex_config.h"
#include "ir/graph.h"

/* Forward declaration — defined in region/stitch.h */
struct vtx_hyperblock_t;

/**
 * VORTEX Hyperblock Size Budget
 *
 * Enforces size limits on hyperblocks to prevent compile-time explosion
 * and code cache pressure.
 *
 * Two limits:
 *   1. VTX_MAX_HYPERBLOCK_NODES (4096) — maximum number of SoN nodes
 *      in a hyperblock. Derived from compile-time budget: each node
 *      requires ~100ns of compilation time on average, so 4096 nodes
 *      ≈ 400μs compile time, which is within the acceptable budget
 *      for a T2 compilation.
 *
 *   2. VTX_MAX_NATIVE_SIZE (32KB) — maximum estimated native code size.
 *      Derived from code cache pressure: 32KB is small enough that
 *      multiple hyperblocks can coexist in the L1 instruction cache
 *      (typically 32KB), ensuring hot code stays cached.
 *
 * The budget checker supports a greedy algorithm:
 *   1. Sort branch candidates by priority (exit_count * estimated_speedup).
 *   2. Try to include each candidate in order.
 *   3. If including a candidate exceeds either budget, skip it.
 *   4. If no candidates fit, return the root-only hyperblock.
 *
 * Backtracking: if including a candidate causes the final native code
 * size to exceed the budget (which can happen because the estimated
 * native size is approximate), the candidate is removed and the next
 * one is tried.
 */

/* ========================================================================== */
/* Budget structure                                                            */
/* ========================================================================== */

/**
 * Tracks the current budget state for hyperblock construction.
 */
typedef struct {
    uint32_t current_node_count;     /* current number of nodes */
    uint32_t current_native_size;    /* current estimated native code size */
    uint32_t max_nodes;              /* node limit (default VTX_MAX_HYPERBLOCK_NODES) */
    uint32_t max_native_size;        /* native size limit (default VTX_MAX_NATIVE_SIZE) */
} vtx_budget_t;

/* ========================================================================== */
/* Budget lifecycle                                                            */
/* ========================================================================== */

/**
 * Initialize a budget with default limits.
 */
void vtx_budget_init(vtx_budget_t *budget);

/**
 * Initialize a budget with custom limits.
 */
void vtx_budget_init_custom(vtx_budget_t *budget,
                             uint32_t max_nodes,
                             uint32_t max_native_size);

/* ========================================================================== */
/* Budget checking                                                             */
/* ========================================================================== */

/**
 * Check if the current budget state allows the hyperblock.
 * Returns true if both the node count and native size are within limits.
 */
bool vtx_budget_check(const vtx_budget_t *budget,
                       const struct vtx_hyperblock_t *hyperblock);

/**
 * Check if adding `additional_nodes` and `additional_native_size` would
 * stay within budget. Does NOT modify the budget state.
 * Returns true if the addition would fit.
 */
bool vtx_budget_can_add(const vtx_budget_t *budget,
                         uint32_t additional_nodes,
                         uint32_t additional_native_size);

/**
 * Check specific node count and native size values against limits.
 * Returns true if both are within limits.
 */
bool vtx_budget_check_counts(const vtx_budget_t *budget,
                              uint32_t node_count,
                              uint32_t native_size);

/* ========================================================================== */
/* Budget mutation                                                             */
/* ========================================================================== */

/**
 * Record that nodes have been added to the hyperblock.
 * Updates the budget's current counts.
 */
void vtx_budget_add(vtx_budget_t *budget,
                     uint32_t additional_nodes,
                     uint32_t additional_native_size);

/**
 * Record that nodes have been removed from the hyperblock (backtracking).
 * Updates the budget's current counts.
 */
void vtx_budget_remove(vtx_budget_t *budget,
                        uint32_t removed_nodes,
                        uint32_t removed_native_size);

/**
 * Reset the budget to zero usage (keep limits).
 */
void vtx_budget_reset(vtx_budget_t *budget);

/* ========================================================================== */
/* Budget queries                                                              */
/* ========================================================================== */

/**
 * Get the remaining node budget.
 */
uint32_t vtx_budget_remaining_nodes(const vtx_budget_t *budget);

/**
 * Get the remaining native size budget.
 */
uint32_t vtx_budget_remaining_native(const vtx_budget_t *budget);

/**
 * Get the utilization ratio for nodes (0.0 to 1.0).
 */
double vtx_budget_node_utilization(const vtx_budget_t *budget);

/**
 * Get the utilization ratio for native size (0.0 to 1.0).
 */
double vtx_budget_native_utilization(const vtx_budget_t *budget);

#endif /* VORTEX_BUDGET_H */
