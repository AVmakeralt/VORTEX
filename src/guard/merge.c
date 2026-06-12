#include "guard/merge.h"
#include <string.h>

/* ========================================================================== */
/* Helper: check if a node is a guard opcode                                   */
/* ========================================================================== */

static bool is_guard_opcode(vtx_node_opcode_t opcode)
{
    return opcode == VTX_OP_Guard || opcode == VTX_OP_DeoptGuard;
}

/* ========================================================================== */
/* Helper: find the receiver node for a guard                                  */
/* ========================================================================== */

/**
 * For a type check guard or null check guard, find the value being checked.
 *
 * Guard input layout:
 *   input[0] = control predecessor
 *   input[1] = the value being guarded (e.g., the receiver for type check)
 *   input[2..] = additional data (e.g., type_id for type check)
 *
 * Returns the NodeID of the checked value, or VTX_NODEID_INVALID.
 */
static vtx_nodeid_t guard_checked_value(const vtx_node_t *guard)
{
    if (guard == NULL) return VTX_NODEID_INVALID;

    /* The data input is typically at input[1] (after control).
     * For DeoptGuard, the layout is the same. */
    if (guard->input_count < 2) return VTX_NODEID_INVALID;
    return guard->inputs[1];
}

/* ========================================================================== */
/* Helper: check if a guard is a null check                                    */
/* ========================================================================== */

/**
 * A null check guard tests that a value is not null.
 * In the SoN IR, this is represented as a Guard node that compares
 * a pointer against null (condition code NE with a null constant).
 *
 * We detect this by checking:
 *   - The guard has a CmpP input comparing the checked value against null
 *   - Or the guard's condition is VTX_COND_NE with a null comparator
 *
 * For simplicity, we use a heuristic: if the guard's type_id == 0
 * and the condition is VTX_COND_NE, it's a null check.
 * A more precise implementation would trace the guard's comparison input.
 */
static bool is_null_check_guard(const vtx_node_t *guard, const vtx_node_table_t *table)
{
    if (guard == NULL) return false;

    /* Check if the guard's comparison input is CmpP against null */
    if (guard->input_count < 2) return false;

    vtx_nodeid_t cmp_id = guard->inputs[1];
    if (cmp_id == VTX_NODEID_INVALID) return false;

    const vtx_node_t *cmp = vtx_node_get_const(table, cmp_id);
    if (cmp == NULL) return false;

    /* If the input is a CmpP node, it might be a null comparison */
    if (cmp->opcode == VTX_OP_CmpP) {
        /* Check if one of the CmpP inputs is a Constant with null pointer */
        for (uint32_t i = 0; i < cmp->input_count; i++) {
            if (cmp->inputs[i] == VTX_NODEID_INVALID) continue;
            const vtx_node_t *input = vtx_node_get_const(table, cmp->inputs[i]);
            if (input != NULL && input->opcode == VTX_OP_Constant &&
                input->type == VTX_TYPE_Ptr && input->constval.as.ptr_val == NULL) {
                return true;
            }
        }
    }

    return false;
}

/* ========================================================================== */
/* Helper: check if a guard is a type check                                    */
/* ========================================================================== */

/**
 * A type check guard tests that a value's type matches an expected type.
 * In the SoN IR, this is represented as a Guard node whose type_id
 * field contains the expected type.
 */
static bool is_type_check_guard(const vtx_node_t *guard)
{
    if (guard == NULL) return false;
    /* Type checks have a non-zero type_id and use CmpP or are
     * directly associated with CheckCast/InstanceOf nodes.
     * We use a simple heuristic: if type_id != 0, it's a type check. */
    return guard->type_id != 0;
}

/* ========================================================================== */
/* Find merge candidates                                                       */
/* ========================================================================== */

int vtx_merge_find_candidates(const vtx_graph_t *graph,
                                vtx_arena_t *arena,
                                vtx_merge_candidate_t **candidates,
                                uint32_t *count)
{
    if (graph == NULL || arena == NULL || candidates == NULL || count == NULL) {
        return -1;
    }

    *candidates = NULL;
    *count = 0;

    /* Collect all guard nodes */
    uint32_t guard_count = 0;
    for (uint32_t i = 0; i < graph->node_table.count; i++) {
        const vtx_node_t *node = &graph->node_table.nodes[i];
        if (!node->dead && is_guard_opcode(node->opcode)) {
            guard_count++;
        }
    }

    if (guard_count < 2) return 0; /* need at least 2 guards to merge */

    /* Allocate array for guard node IDs */
    vtx_nodeid_t *guards = (vtx_nodeid_t *)vtx_arena_alloc(
        arena, guard_count * sizeof(vtx_nodeid_t));
    if (guards == NULL) return -1;

    uint32_t idx = 0;
    for (uint32_t i = 0; i < graph->node_table.count; i++) {
        const vtx_node_t *node = &graph->node_table.nodes[i];
        if (!node->dead && is_guard_opcode(node->opcode)) {
            guards[idx++] = node->id;
        }
    }

    /* Allocate candidates array (worst case: every pair is a candidate) */
    uint32_t max_candidates = guard_count * (guard_count - 1) / 2;
    vtx_merge_candidate_t *cands = (vtx_merge_candidate_t *)vtx_arena_alloc(
        arena, max_candidates * sizeof(vtx_merge_candidate_t));
    if (cands == NULL) return -1;

    /* Check all pairs of guards for mergeability */
    uint32_t candidate_count = 0;
    for (uint32_t i = 0; i < guard_count && candidate_count < max_candidates; i++) {
        for (uint32_t j = i + 1; j < guard_count && candidate_count < max_candidates; j++) {
            int kind = vtx_merge_check_pair(graph, guards[i], guards[j]);
            if (kind >= 0) {
                cands[candidate_count].guard_a = guards[i];
                cands[candidate_count].guard_b = guards[j];
                cands[candidate_count].kind = (vtx_merge_kind_t)kind;
                candidate_count++;
            }
        }
    }

    *candidates = cands;
    *count = candidate_count;
    return 0;
}

/* ========================================================================== */
/* Check if two guards can be merged                                           */
/* ========================================================================== */

int vtx_merge_check_pair(const vtx_graph_t *graph,
                           vtx_nodeid_t guard_a,
                           vtx_nodeid_t guard_b)
{
    if (graph == NULL) return -1;

    const vtx_node_t *a = vtx_node_get_const(&graph->node_table, guard_a);
    const vtx_node_t *b = vtx_node_get_const(&graph->node_table, guard_b);

    if (a == NULL || b == NULL) return -1;
    if (a->dead || b->dead) return -1;
    if (!is_guard_opcode(a->opcode) || !is_guard_opcode(b->opcode)) return -1;

    /* Get the values being checked by each guard */
    vtx_nodeid_t val_a = guard_checked_value(a);
    vtx_nodeid_t val_b = guard_checked_value(b);

    if (val_a == VTX_NODEID_INVALID || val_b == VTX_NODEID_INVALID) return -1;

    /* Case 1: Both check the same value for null → merge null checks */
    if (val_a == val_b) {
        bool null_a = is_null_check_guard(a, &graph->node_table);
        bool null_b = is_null_check_guard(b, &graph->node_table);

        if (null_a && null_b) {
            return VTX_MERGE_NULL_CHECK;
        }

        /* Case 2: Both check the same value's type → merge type checks */
        bool type_a = is_type_check_guard(a);
        bool type_b = is_type_check_guard(b);

        if (type_a && type_b && a->type_id != b->type_id) {
            return VTX_MERGE_TYPE_CHECK;
        }

        /* Same value, same check → second guard is redundant */
        if (a->type_id == b->type_id && a->cond == b->cond) {
            return VTX_MERGE_NULL_CHECK; /* treat as null check merge (elimination) */
        }
    }

    /* Case 3: Range checks on the same array with different indices.
     * This requires both guards to be bounds checks on the same array object.
     * We check if both guards compare against the same array length node. */
    if (a->opcode == VTX_OP_DeoptGuard && b->opcode == VTX_OP_DeoptGuard) {
        /* Look for Cmp pattern: index < length
         * Both guards should have Cmp inputs that reference the same array length */
        vtx_nodeid_t cmp_a = (a->input_count > 1) ? a->inputs[1] : VTX_NODEID_INVALID;
        vtx_nodeid_t cmp_b = (b->input_count > 1) ? b->inputs[1] : VTX_NODEID_INVALID;

        if (cmp_a != VTX_NODEID_INVALID && cmp_b != VTX_NODEID_INVALID) {
            const vtx_node_t *c_a = vtx_node_get_const(&graph->node_table, cmp_a);
            const vtx_node_t *c_b = vtx_node_get_const(&graph->node_table, cmp_b);

            if (c_a != NULL && c_b != NULL &&
                c_a->opcode == VTX_OP_Cmp && c_b->opcode == VTX_OP_Cmp) {
                /* Check if both comparisons share the same second input (array length) */
                if (c_a->input_count >= 2 && c_b->input_count >= 2) {
                    if (c_a->inputs[1] == c_b->inputs[1] &&
                        c_a->inputs[1] != VTX_NODEID_INVALID) {
                        return VTX_MERGE_RANGE_CHECK;
                    }
                }
            }
        }
    }

    return -1; /* not mergeable */
}

/* ========================================================================== */
/* Perform the merge                                                           */
/* ========================================================================== */

/**
 * Merge a single pair of guards.
 *
 * For NULL_CHECK merges: the second guard is eliminated (marked dead)
 * and all its uses are redirected to the first guard.
 *
 * For TYPE_CHECK merges: a new guard is created that checks the value
 * against a type set {type_a, type_b}. Both original guards are eliminated.
 *
 * For RANGE_CHECK merges: a new guard is created with the wider range.
 */
static void perform_merge(vtx_graph_t *graph,
                           vtx_merge_candidate_t *candidate,
                           vtx_merge_result_t *result)
{
    vtx_node_t *a = vtx_node_get(&graph->node_table, candidate->guard_a);
    vtx_node_t *b = vtx_node_get(&graph->node_table, candidate->guard_b);

    if (a == NULL || b == NULL) return;
    if (a->dead || b->dead) return;

    switch (candidate->kind) {
    case VTX_MERGE_NULL_CHECK: {
        /* Second null check is redundant — eliminate it.
         * Redirect all uses of guard_b to guard_a. */
        for (uint32_t i = 0; i < graph->node_table.count; i++) {
            vtx_node_t *node = &graph->node_table.nodes[i];
            if (node->dead) continue;

            for (uint32_t inp = 0; inp < node->input_count; inp++) {
                if (node->inputs[inp] == candidate->guard_b) {
                    vtx_node_replace_input(&graph->node_table,
                                            node->id, inp, candidate->guard_a);
                }
            }
        }

        /* Mark guard_b as dead */
        b->dead = true;
        result->null_checks_merged++;
        result->guards_eliminated++;
        break;
    }

    case VTX_MERGE_TYPE_CHECK: {
        /* Create a merged type check: guard that value is in {type_a, type_b}.
         *
         * In the SoN IR, we represent this by creating a new Guard node
         * that checks the value against a type set. The type set is encoded
         * in the guard's auxiliary data.
         *
         * For simplicity, we use the existing Guard node structure with
         * a sentinel type_id that indicates "type set" mode. The actual
         * type set is stored as additional inputs to the guard.
         *
         * However, a simpler and equally effective approach for the
         * common case of 2-type polymorphic checks is:
         *   guard(type == A || type == B)
         * which is encoded as:
         *   temp = (type == A) | (type == B)
         *   guard(temp != 0)
         *
         * F8/Bug7 fix: Instead of using LoadField(offset=0) to read the
         * object's type (which assumes a specific memory layout and is
         * incorrect for objects whose header isn't at offset 0), we use
         * CmpP (pointer compare) to compare the object's type_id directly.
         * The Guard node's type_id field carries the expected type, so we
         * create CmpP nodes that compare the value against type_id constants.
         *
         * We create this expanded form in the SoN graph. */

        vtx_nodeid_t value_node = guard_checked_value(a);

        /* Create type_id constants for comparison */
        vtx_nodeid_t const_a = vtx_node_create(&graph->node_table, VTX_OP_Constant);
        vtx_nodeid_t const_b = vtx_node_create(&graph->node_table, VTX_OP_Constant);

        if (const_a == VTX_NODEID_INVALID || const_b == VTX_NODEID_INVALID) break;

        vtx_node_t *c_a = vtx_node_get(&graph->node_table, const_a);
        vtx_node_t *c_b = vtx_node_get(&graph->node_table, const_b);

        if (c_a != NULL) {
            c_a->type = VTX_TYPE_Int;
            c_a->constval = vtx_constval_int((int64_t)a->type_id);
        }
        if (c_b != NULL) {
            c_b->type = VTX_TYPE_Int;
            c_b->constval = vtx_constval_int((int64_t)b->type_id);
        }

        /* Create: cmp_a = CmpP(value, const_a) — type pointer comparison.
         * CmpP is the proper opcode for comparing pointer/type values,
         * rather than LoadField(offset=0) which assumes a specific layout. */
        vtx_nodeid_t cmp_a_id = vtx_node_create(&graph->node_table, VTX_OP_CmpP);
        vtx_nodeid_t cmp_b_id = vtx_node_create(&graph->node_table, VTX_OP_CmpP);

        if (cmp_a_id == VTX_NODEID_INVALID || cmp_b_id == VTX_NODEID_INVALID) break;

        vtx_node_t *cmp_a_node = vtx_node_get(&graph->node_table, cmp_a_id);
        vtx_node_t *cmp_b_node = vtx_node_get(&graph->node_table, cmp_b_id);

        if (cmp_a_node != NULL) {
            cmp_a_node->type = VTX_TYPE_Int;
            cmp_a_node->flags = VTX_NF_DATA;
            cmp_a_node->cond = VTX_COND_EQ;
            vtx_node_add_input(&graph->node_table, cmp_a_id, value_node);
            vtx_node_add_input(&graph->node_table, cmp_a_id, const_a);
        }

        if (cmp_b_node != NULL) {
            cmp_b_node->type = VTX_TYPE_Int;
            cmp_b_node->flags = VTX_NF_DATA;
            cmp_b_node->cond = VTX_COND_EQ;
            vtx_node_add_input(&graph->node_table, cmp_b_id, value_node);
            vtx_node_add_input(&graph->node_table, cmp_b_id, const_b);
        }

        /* Create: or_result = Or(cmp_a, cmp_b) */
        vtx_nodeid_t or_id = vtx_node_create(&graph->node_table, VTX_OP_Or);
        if (or_id == VTX_NODEID_INVALID) break;

        vtx_node_t *or_node = vtx_node_get(&graph->node_table, or_id);
        if (or_node != NULL) {
            or_node->type = VTX_TYPE_Int;
            or_node->flags = VTX_NF_DATA;
            vtx_node_add_input(&graph->node_table, or_id, cmp_a_id);
            vtx_node_add_input(&graph->node_table, or_id, cmp_b_id);
        }

        /* Create: merged_guard = DeoptGuard(or_result != 0) */
        vtx_nodeid_t merged_guard = vtx_node_create(&graph->node_table,
                                                       VTX_OP_DeoptGuard);
        if (merged_guard == VTX_NODEID_INVALID) break;

        vtx_node_t *mg = vtx_node_get(&graph->node_table, merged_guard);
        if (mg != NULL) {
            mg->flags = VTX_NF_CONTROL | VTX_NF_SIDE_EFFECT;
            mg->cond = VTX_COND_NE;
            mg->bytecode_pc = a->bytecode_pc; /* use first guard's PC */
            mg->frame_state = a->frame_state;
            vtx_node_add_input(&graph->node_table, merged_guard, a->inputs[0]); /* control */
            vtx_node_add_input(&graph->node_table, merged_guard, or_id); /* condition */
        }

        /* Mark original guards as dead */
        a->dead = true;
        b->dead = true;

        /* Redirect all users of the original guards to the merged guard.
         * Without this, nodes that referenced the original guards as
         * control inputs would have dangling references. */
        for (uint32_t i = 0; i < graph->node_table.count; i++) {
            vtx_node_t *node = &graph->node_table.nodes[i];
            if (node->dead) continue;
            for (uint32_t inp = 0; inp < node->input_count; inp++) {
                if (node->inputs[inp] == candidate->guard_a ||
                    node->inputs[inp] == candidate->guard_b) {
                    vtx_node_replace_input(&graph->node_table,
                                            node->id, inp, merged_guard);
                }
            }
        }

        result->type_checks_merged++;
        result->guards_merged += 2;
        break;
    }

    case VTX_MERGE_RANGE_CHECK: {
        /* Range check merging: create a single check for the wider range.
         * guard(min(i,j) >= 0 && max(i,j) < len)
         *
         * This is achieved by:
         * 1. Compute min_idx = Min(i, j)
         * 2. Compute max_idx = Max(i, j)
         * 3. Create guard(min_idx >= 0 && max_idx < len)
         *
         * F8 fix: We use proper Min/Max opcodes instead of Phi nodes.
         * Phi nodes require a Region node as their first input in the
         * SoN IR; creating a Phi without a Region produces malformed IR
         * that will crash the verifier/scheduler. Min/Max are pure data
         * nodes that take two inputs and return the minimum/maximum. */

        /* Get the two index values from the Cmp nodes */
        vtx_nodeid_t cmp_a_id = (a->input_count > 1) ? a->inputs[1] : VTX_NODEID_INVALID;
        vtx_nodeid_t cmp_b_id = (b->input_count > 1) ? b->inputs[1] : VTX_NODEID_INVALID;

        if (cmp_a_id == VTX_NODEID_INVALID || cmp_b_id == VTX_NODEID_INVALID) break;

        const vtx_node_t *cmp_a = vtx_node_get_const(&graph->node_table, cmp_a_id);
        const vtx_node_t *cmp_b = vtx_node_get_const(&graph->node_table, cmp_b_id);

        if (cmp_a == NULL || cmp_b == NULL) break;
        if (cmp_a->input_count < 2 || cmp_b->input_count < 2) break;

        vtx_nodeid_t idx_a = cmp_a->inputs[0]; /* first index */
        vtx_nodeid_t idx_b = cmp_b->inputs[0]; /* second index */
        vtx_nodeid_t len = cmp_a->inputs[1];   /* array length (shared) */

        /* Compute min_idx = Min(idx_a, idx_b) */
        vtx_nodeid_t min_id = vtx_node_create(&graph->node_table, VTX_OP_Min);
        vtx_nodeid_t max_id = vtx_node_create(&graph->node_table, VTX_OP_Max);

        if (min_id == VTX_NODEID_INVALID || max_id == VTX_NODEID_INVALID) break;

        vtx_node_t *mn = vtx_node_get(&graph->node_table, min_id);
        if (mn != NULL) {
            mn->type = VTX_TYPE_Int;
            mn->flags = VTX_NF_DATA;
            vtx_node_add_input(&graph->node_table, min_id, idx_a);
            vtx_node_add_input(&graph->node_table, min_id, idx_b);
        }

        vtx_node_t *mx = vtx_node_get(&graph->node_table, max_id);
        if (mx != NULL) {
            mx->type = VTX_TYPE_Int;
            mx->flags = VTX_NF_DATA;
            vtx_node_add_input(&graph->node_table, max_id, idx_a);
            vtx_node_add_input(&graph->node_table, max_id, idx_b);
        }

        /* Create merged lower bound check: guard(min >= 0) */
        vtx_nodeid_t zero_const = vtx_node_create(&graph->node_table, VTX_OP_Constant);
        vtx_nodeid_t lower_cmp = vtx_node_create(&graph->node_table, VTX_OP_Cmp);
        vtx_nodeid_t lower_guard = vtx_node_create(&graph->node_table, VTX_OP_DeoptGuard);

        if (zero_const != VTX_NODEID_INVALID) {
            vtx_node_t *z = vtx_node_get(&graph->node_table, zero_const);
            if (z != NULL) {
                z->type = VTX_TYPE_Int;
                z->constval = vtx_constval_int(0);
            }
        }

        if (lower_cmp != VTX_NODEID_INVALID) {
            vtx_node_t *lc = vtx_node_get(&graph->node_table, lower_cmp);
            if (lc != NULL) {
                lc->type = VTX_TYPE_Int;
                lc->flags = VTX_NF_DATA;
                lc->cond = VTX_COND_GE;
                vtx_node_add_input(&graph->node_table, lower_cmp, min_id);
                vtx_node_add_input(&graph->node_table, lower_cmp, zero_const);
            }
        }

        if (lower_guard != VTX_NODEID_INVALID) {
            vtx_node_t *lg = vtx_node_get(&graph->node_table, lower_guard);
            if (lg != NULL) {
                lg->flags = VTX_NF_CONTROL | VTX_NF_SIDE_EFFECT;
                /* G7 fix: guard condition is about the comparison RESULT.
                 * lower_cmp computes (min >= 0) as a boolean; the guard
                 * should deopt when the result is falsy (== 0), i.e.,
                 * VTX_COND_NE means "assert result != 0 (pass if truthy)". */
                lg->cond = VTX_COND_NE; /* deopts when comparison result is 0 (falsy) */
                lg->bytecode_pc = a->bytecode_pc;
                lg->frame_state = a->frame_state;
                vtx_node_add_input(&graph->node_table, lower_guard, a->inputs[0]);
                vtx_node_add_input(&graph->node_table, lower_guard, lower_cmp);
            }
        }

        /* Create merged upper bound check: guard(max < len) */
        vtx_nodeid_t upper_cmp = vtx_node_create(&graph->node_table, VTX_OP_Cmp);
        vtx_nodeid_t upper_guard = vtx_node_create(&graph->node_table, VTX_OP_DeoptGuard);

        if (upper_cmp != VTX_NODEID_INVALID) {
            vtx_node_t *uc = vtx_node_get(&graph->node_table, upper_cmp);
            if (uc != NULL) {
                uc->type = VTX_TYPE_Int;
                uc->flags = VTX_NF_DATA;
                uc->cond = VTX_COND_LT;
                vtx_node_add_input(&graph->node_table, upper_cmp, max_id);
                vtx_node_add_input(&graph->node_table, upper_cmp, len);
            }
        }

        if (upper_guard != VTX_NODEID_INVALID) {
            vtx_node_t *ug = vtx_node_get(&graph->node_table, upper_guard);
            if (ug != NULL) {
                ug->flags = VTX_NF_CONTROL | VTX_NF_SIDE_EFFECT;
                /* G7 fix: guard condition is about the comparison RESULT.
                 * upper_cmp computes (max < len) as a boolean; the guard
                 * should deopt when the result is falsy (== 0), i.e.,
                 * VTX_COND_NE means "assert result != 0 (pass if truthy)". */
                ug->cond = VTX_COND_NE; /* deopts when comparison result is 0 (falsy) */
                ug->bytecode_pc = b->bytecode_pc;
                ug->frame_state = b->frame_state;
                vtx_node_add_input(&graph->node_table, upper_guard, a->inputs[0]);
                vtx_node_add_input(&graph->node_table, upper_guard, upper_cmp);
            }
        }

        /* Mark original guards as dead */
        a->dead = true;
        b->dead = true;

        /* Redirect all users of the original guards to the replacement guards.
         * Users of guard_a are redirected to lower_guard, and users of guard_b
         * are redirected to upper_guard. Without this, nodes that referenced
         * the original guards as control inputs would have dangling references. */
        for (uint32_t i = 0; i < graph->node_table.count; i++) {
            vtx_node_t *node = &graph->node_table.nodes[i];
            if (node->dead) continue;
            for (uint32_t inp = 0; inp < node->input_count; inp++) {
                if (node->inputs[inp] == candidate->guard_a) {
                    vtx_node_replace_input(&graph->node_table,
                                            node->id, inp, lower_guard);
                } else if (node->inputs[inp] == candidate->guard_b) {
                    vtx_node_replace_input(&graph->node_table,
                                            node->id, inp, upper_guard);
                }
            }
        }

        result->range_checks_merged++;
        result->guards_merged += 2;
        break;
    }
    }
}

/* ========================================================================== */
/* Main merge algorithm                                                        */
/* ========================================================================== */

vtx_merge_result_t vtx_merge_guards(vtx_graph_t *graph, vtx_arena_t *arena)
{
    vtx_merge_result_t result;
    memset(&result, 0, sizeof(result));

    if (graph == NULL || arena == NULL) return result;

    /* Find merge candidates */
    vtx_merge_candidate_t *candidates = NULL;
    uint32_t candidate_count = 0;

    if (vtx_merge_find_candidates(graph, arena, &candidates, &candidate_count) != 0) {
        return result;
    }

    /* Perform each merge */
    for (uint32_t i = 0; i < candidate_count; i++) {
        /* Check that both guards are still alive (not eliminated by a previous merge) */
        vtx_node_t *a = vtx_node_get(&graph->node_table, candidates[i].guard_a);
        vtx_node_t *b = vtx_node_get(&graph->node_table, candidates[i].guard_b);

        if (a == NULL || b == NULL || a->dead || b->dead) continue;

        perform_merge(graph, &candidates[i], &result);
    }

    result.guards_merged = result.type_checks_merged +
                           result.null_checks_merged +
                           result.range_checks_merged;

    return result;
}
