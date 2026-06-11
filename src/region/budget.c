/**
 * VORTEX Hyperblock Size Budget — Implementation
 *
 * Enforces size limits on hyperblocks to prevent compile-time explosion
 * and code cache pressure. Provides greedy budget checking with
 * backtracking support for the stitching algorithm.
 */

#include "region/budget.h"
#include "region/stitch.h"
#include <string.h>

/* ========================================================================== */
/* Budget lifecycle                                                            */
/* ========================================================================== */

void vtx_budget_init(vtx_budget_t *budget)
{
    VTX_ASSERT(budget != NULL, "budget must not be NULL");
    budget->current_node_count = 0;
    budget->current_native_size = 0;
    budget->max_nodes = VTX_MAX_HYPERBLOCK_NODES;
    budget->max_native_size = VTX_MAX_NATIVE_SIZE;
}

void vtx_budget_init_custom(vtx_budget_t *budget,
                             uint32_t max_nodes,
                             uint32_t max_native_size)
{
    VTX_ASSERT(budget != NULL, "budget must not be NULL");
    budget->current_node_count = 0;
    budget->current_native_size = 0;
    budget->max_nodes = max_nodes;
    budget->max_native_size = max_native_size;
}

/* ========================================================================== */
/* Budget checking                                                             */
/* ========================================================================== */

bool vtx_budget_check(const vtx_budget_t *budget,
                       const vtx_hyperblock_t *hyperblock)
{
    if (budget == NULL || hyperblock == NULL) return false;

    return hyperblock->node_count <= budget->max_nodes &&
           hyperblock->estimated_native_size <= budget->max_native_size;
}

bool vtx_budget_can_add(const vtx_budget_t *budget,
                         uint32_t additional_nodes,
                         uint32_t additional_native_size)
{
    if (budget == NULL) return false;

    uint32_t projected_nodes = budget->current_node_count + additional_nodes;
    uint32_t projected_native = budget->current_native_size + additional_native_size;

    /* Check for overflow */
    if (projected_nodes < budget->current_node_count) return false;
    if (projected_native < budget->current_native_size) return false;

    return projected_nodes <= budget->max_nodes &&
           projected_native <= budget->max_native_size;
}

bool vtx_budget_check_counts(const vtx_budget_t *budget,
                              uint32_t node_count,
                              uint32_t native_size)
{
    if (budget == NULL) return false;
    return node_count <= budget->max_nodes &&
           native_size <= budget->max_native_size;
}

/* ========================================================================== */
/* Budget mutation                                                             */
/* ========================================================================== */

void vtx_budget_add(vtx_budget_t *budget,
                     uint32_t additional_nodes,
                     uint32_t additional_native_size)
{
    if (budget == NULL) return;

    budget->current_node_count += additional_nodes;
    budget->current_native_size += additional_native_size;
}

void vtx_budget_remove(vtx_budget_t *budget,
                        uint32_t removed_nodes,
                        uint32_t removed_native_size)
{
    if (budget == NULL) return;

    /* Clamp to zero — never go negative */
    if (budget->current_node_count >= removed_nodes) {
        budget->current_node_count -= removed_nodes;
    } else {
        budget->current_node_count = 0;
    }

    if (budget->current_native_size >= removed_native_size) {
        budget->current_native_size -= removed_native_size;
    } else {
        budget->current_native_size = 0;
    }
}

void vtx_budget_reset(vtx_budget_t *budget)
{
    if (budget == NULL) return;
    budget->current_node_count = 0;
    budget->current_native_size = 0;
}

/* ========================================================================== */
/* Budget queries                                                              */
/* ========================================================================== */

uint32_t vtx_budget_remaining_nodes(const vtx_budget_t *budget)
{
    if (budget == NULL) return 0;
    if (budget->current_node_count >= budget->max_nodes) return 0;
    return budget->max_nodes - budget->current_node_count;
}

uint32_t vtx_budget_remaining_native(const vtx_budget_t *budget)
{
    if (budget == NULL) return 0;
    if (budget->current_native_size >= budget->max_native_size) return 0;
    return budget->max_native_size - budget->current_native_size;
}

double vtx_budget_node_utilization(const vtx_budget_t *budget)
{
    if (budget == NULL || budget->max_nodes == 0) return 1.0;
    return (double)budget->current_node_count / (double)budget->max_nodes;
}

double vtx_budget_native_utilization(const vtx_budget_t *budget)
{
    if (budget == NULL || budget->max_native_size == 0) return 1.0;
    return (double)budget->current_native_size / (double)budget->max_native_size;
}
