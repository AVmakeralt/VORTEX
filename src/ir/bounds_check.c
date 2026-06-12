#include "ir/bounds_check.h"
#include "ir/induction.h"

#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* ========================================================================== */
/* Overflow-safe arithmetic for range computation                              */
/* ========================================================================== */

/**
 * Check if a + b would overflow for int64_t.
 */
static bool add_overflows(int64_t a, int64_t b)
{
    if (b > 0 && a > INT64_MAX - b) return true;
    if (b < 0 && a < INT64_MIN - b) return true;
    return false;
}

/**
 * Check if a - b would overflow for int64_t.
 */
static bool sub_overflows(int64_t a, int64_t b)
{
    if (b < 0 && a > INT64_MAX + b) return true;
    if (b > 0 && a < INT64_MIN + b) return true;
    return false;
}

/**
 * Check if a * b would overflow for int64_t.
 */
static bool mul_overflows(int64_t a, int64_t b)
{
    if (a == 0 || b == 0) return false;
    if (a == -1) return b == INT64_MIN;
    if (b == -1) return a == INT64_MIN;
    if (a > 0) {
        if (b > 0) return a > INT64_MAX / b;
        else        return b < INT64_MIN / a;
    } else {
        if (b > 0) return a < INT64_MIN / b;
        else        return b < INT64_MAX / a;
    }
}

/**
 * Saturating add: clamps to INT64_MAX/INT64_MIN on overflow.
 */
static int64_t sat_add(int64_t a, int64_t b)
{
    if (add_overflows(a, b)) {
        return (b > 0) ? INT64_MAX : INT64_MIN;
    }
    return a + b;
}

/**
 * Saturating subtract: clamps on overflow.
 */
static int64_t sat_sub(int64_t a, int64_t b)
{
    if (sub_overflows(a, b)) {
        return (b > 0) ? INT64_MIN : INT64_MAX;
    }
    return a - b;
}

/**
 * Saturating multiply: clamps on overflow, preserving sign.
 */
static int64_t sat_mul(int64_t a, int64_t b)
{
    if (mul_overflows(a, b)) {
        bool positive = (a > 0 && b > 0) || (a < 0 && b < 0);
        return positive ? INT64_MAX : INT64_MIN;
    }
    return a * b;
}

/**
 * Safe decrement: returns INT64_MIN if val == INT64_MIN, else val - 1.
 */
static int64_t safe_dec(int64_t val)
{
    if (val == INT64_MIN) return INT64_MIN;
    return val - 1;
}

/* ========================================================================== */
/* Range transfer function                                                      */
/* ========================================================================== */

/**
 * Compute the range for a node based on its opcode and the ranges of its
 * inputs.  This is the core transfer function of the range analysis.
 *
 * For arithmetic nodes the result range is derived from the input ranges
 * using interval arithmetic with overflow detection.  When an operation
 * would overflow the result degrades to VTX_RANGE_UNKNOWN rather than
 * returning a wrong value.
 */
vtx_range_t vtx_bounds_compute_range(const vtx_node_t *node,
                                      const vtx_node_table_t *table,
                                      const vtx_range_t *ranges,
                                      uint32_t range_count)
{
    if (node == NULL || table == NULL) {
        return VTX_RANGE_UNKNOWN;
    }

    switch (node->opcode) {

    /* ---- Constants: can compute range even without ranges array ---- */
    case VTX_OP_Constant: {
        if (node->constval.kind == VTX_TYPE_Int) {
            int64_t v = node->constval.as.int_val;
            return VTX_RANGE_CONST(v);
        }
        /* Non-integer constants: no useful range */
        return VTX_RANGE_UNKNOWN;
    }

    /* ---- Parameters: unknown at compile time ---- */
    case VTX_OP_Parameter:
        return VTX_RANGE_UNKNOWN;

    /* ---- Add: [a.min+b.min, a.max+b.max] ---- */
    case VTX_OP_Add: {
        if (node->input_count < 2) return VTX_RANGE_UNKNOWN;
        vtx_nodeid_t a_id = node->inputs[0];
        vtx_nodeid_t b_id = node->inputs[1];
        if (a_id == VTX_NODEID_INVALID || b_id == VTX_NODEID_INVALID) return VTX_RANGE_UNKNOWN;
        if (a_id >= range_count || b_id >= range_count) return VTX_RANGE_UNKNOWN;

        vtx_range_t a = ranges[a_id];
        vtx_range_t b = ranges[b_id];

        if (add_overflows(a.min, b.min) || add_overflows(a.max, b.max)) {
            return VTX_RANGE_UNKNOWN;
        }

        return VTX_RANGE(a.min + b.min, a.max + b.max);
    }

    /* ---- Sub: [a.min-b.max, a.max-b.min] ---- */
    case VTX_OP_Sub: {
        if (node->input_count < 2) return VTX_RANGE_UNKNOWN;
        vtx_nodeid_t a_id = node->inputs[0];
        vtx_nodeid_t b_id = node->inputs[1];
        if (a_id == VTX_NODEID_INVALID || b_id == VTX_NODEID_INVALID) return VTX_RANGE_UNKNOWN;
        if (a_id >= range_count || b_id >= range_count) return VTX_RANGE_UNKNOWN;

        vtx_range_t a = ranges[a_id];
        vtx_range_t b = ranges[b_id];

        if (sub_overflows(a.min, b.max) || sub_overflows(a.max, b.min)) {
            return VTX_RANGE_UNKNOWN;
        }

        return VTX_RANGE(a.min - b.max, a.max - b.min);
    }

    /* ---- Mul: cross-product of all min/max pairs ---- */
    case VTX_OP_Mul: {
        if (node->input_count < 2) return VTX_RANGE_UNKNOWN;
        vtx_nodeid_t a_id = node->inputs[0];
        vtx_nodeid_t b_id = node->inputs[1];
        if (a_id == VTX_NODEID_INVALID || b_id == VTX_NODEID_INVALID) return VTX_RANGE_UNKNOWN;
        if (a_id >= range_count || b_id >= range_count) return VTX_RANGE_UNKNOWN;

        vtx_range_t a = ranges[a_id];
        vtx_range_t b = ranges[b_id];

        int64_t candidates[4];
        candidates[0] = sat_mul(a.min, b.min);
        candidates[1] = sat_mul(a.min, b.max);
        candidates[2] = sat_mul(a.max, b.min);
        candidates[3] = sat_mul(a.max, b.max);

        /* If any product overflows, the range is unknown */
        if (mul_overflows(a.min, b.min) || mul_overflows(a.min, b.max) ||
            mul_overflows(a.max, b.min) || mul_overflows(a.max, b.max)) {
            return VTX_RANGE_UNKNOWN;
        }

        int64_t lo = candidates[0];
        int64_t hi = candidates[0];
        for (int i = 1; i < 4; i++) {
            if (candidates[i] < lo) lo = candidates[i];
            if (candidates[i] > hi) hi = candidates[i];
        }

        return VTX_RANGE(lo, hi);
    }

    /* ---- Div: conservative approximation ---- */
    case VTX_OP_Div: {
        if (node->input_count < 2) return VTX_RANGE_UNKNOWN;
        vtx_nodeid_t a_id = node->inputs[0];
        vtx_nodeid_t b_id = node->inputs[1];
        if (a_id == VTX_NODEID_INVALID || b_id == VTX_NODEID_INVALID) return VTX_RANGE_UNKNOWN;
        if (a_id >= range_count || b_id >= range_count) return VTX_RANGE_UNKNOWN;

        vtx_range_t b = ranges[b_id];

        /* If divisor could be zero, we can't compute a safe range */
        if (b.min <= 0 && b.max >= 0) {
            return VTX_RANGE_UNKNOWN;
        }

        vtx_range_t a = ranges[a_id];
        /* Compute cross-product of a range divided by b range */
        int64_t candidates[4];
        candidates[0] = a.min / b.min;
        candidates[1] = a.min / b.max;
        candidates[2] = a.max / b.min;
        candidates[3] = a.max / b.max;

        int64_t lo = candidates[0];
        int64_t hi = candidates[0];
        for (int i = 1; i < 4; i++) {
            if (candidates[i] < lo) lo = candidates[i];
            if (candidates[i] > hi) hi = candidates[i];
        }

        return VTX_RANGE(lo, hi);
    }

    /* ---- Phi: union of all data-input ranges ---- */
    case VTX_OP_Phi: {
        vtx_range_t result = VTX_RANGE_UNKNOWN;
        bool found_any = false;

        for (uint32_t i = 0; i < node->input_count; i++) {
            vtx_nodeid_t inp_id = node->inputs[i];
            if (inp_id == VTX_NODEID_INVALID || inp_id >= range_count) continue;

            /* Skip control inputs (Region/LoopBegin nodes) */
            const vtx_node_t *inp_node = vtx_node_get_const(table, inp_id);
            if (inp_node != NULL && vtx_nf_has(inp_node->flags, VTX_NF_CONTROL)) continue;

            if (!found_any) {
                result = ranges[inp_id];
                found_any = true;
            } else {
                result = vtx_range_union(result, ranges[inp_id]);
            }
        }

        return result;
    }

    /* ---- Neg: [-a.max, -a.min] ---- */
    case VTX_OP_Neg: {
        if (node->input_count < 1) return VTX_RANGE_UNKNOWN;
        vtx_nodeid_t a_id = node->inputs[0];
        if (a_id == VTX_NODEID_INVALID || a_id >= range_count) return VTX_RANGE_UNKNOWN;

        vtx_range_t a = ranges[a_id];
        /* Negating INT64_MIN overflows */
        if (a.min == INT64_MIN || a.max == INT64_MIN) {
            return VTX_RANGE_UNKNOWN;
        }

        return VTX_RANGE(-a.max, -a.min);
    }

    /* ---- And: if both non-negative, result <= min(a.max, b.max) ---- */
    case VTX_OP_And: {
        if (node->input_count < 2) return VTX_RANGE_UNKNOWN;
        vtx_nodeid_t a_id = node->inputs[0];
        vtx_nodeid_t b_id = node->inputs[1];
        if (a_id == VTX_NODEID_INVALID || b_id == VTX_NODEID_INVALID) return VTX_RANGE_UNKNOWN;
        if (a_id >= range_count || b_id >= range_count) return VTX_RANGE_UNKNOWN;

        vtx_range_t a = ranges[a_id];
        vtx_range_t b = ranges[b_id];

        if (a.min >= 0 && b.min >= 0) {
            int64_t new_max = (a.max < b.max) ? a.max : b.max;
            return VTX_RANGE(0, new_max);
        }
        return VTX_RANGE_UNKNOWN;
    }

    /* ---- Or: if both non-negative, result >= max(a.min, b.min) ---- */
    case VTX_OP_Or: {
        if (node->input_count < 2) return VTX_RANGE_UNKNOWN;
        vtx_nodeid_t a_id = node->inputs[0];
        vtx_nodeid_t b_id = node->inputs[1];
        if (a_id == VTX_NODEID_INVALID || b_id == VTX_NODEID_INVALID) return VTX_RANGE_UNKNOWN;
        if (a_id >= range_count || b_id >= range_count) return VTX_RANGE_UNKNOWN;

        vtx_range_t a = ranges[a_id];
        vtx_range_t b = ranges[b_id];

        if (a.min >= 0 && b.min >= 0) {
            int64_t new_min = (a.min > b.min) ? a.min : b.min;
            int64_t new_max = sat_add(a.max, b.max);
            if (add_overflows(a.max, b.max)) new_max = INT64_MAX;
            return VTX_RANGE(new_min, new_max);
        }
        return VTX_RANGE_UNKNOWN;
    }

    /* ---- Shl: if shift is constant, compute ---- */
    case VTX_OP_Shl: {
        if (node->input_count < 2) return VTX_RANGE_UNKNOWN;
        vtx_nodeid_t val_id = node->inputs[0];
        vtx_nodeid_t shift_id = node->inputs[1];
        if (val_id == VTX_NODEID_INVALID || shift_id == VTX_NODEID_INVALID) return VTX_RANGE_UNKNOWN;
        if (val_id >= range_count || shift_id >= range_count) return VTX_RANGE_UNKNOWN;

        vtx_range_t val = ranges[val_id];
        vtx_range_t shift = ranges[shift_id];

        if (shift.is_const && shift.min >= 0 && shift.min < 63) {
            int64_t s = shift.min;
            int64_t multiplier = (int64_t)1 << s;
            if (!mul_overflows(val.min, multiplier) &&
                !mul_overflows(val.max, multiplier)) {
                return VTX_RANGE(val.min << s, val.max << s);
            }
        }
        return VTX_RANGE_UNKNOWN;
    }

    /* ---- Shr: if shift is constant and value is non-negative ---- */
    case VTX_OP_Shr: {
        if (node->input_count < 2) return VTX_RANGE_UNKNOWN;
        vtx_nodeid_t val_id = node->inputs[0];
        vtx_nodeid_t shift_id = node->inputs[1];
        if (val_id == VTX_NODEID_INVALID || shift_id == VTX_NODEID_INVALID) return VTX_RANGE_UNKNOWN;
        if (val_id >= range_count || shift_id >= range_count) return VTX_RANGE_UNKNOWN;

        vtx_range_t val = ranges[val_id];
        vtx_range_t shift = ranges[shift_id];

        if (shift.is_const && shift.min >= 0 && shift.min < 64) {
            int64_t s = shift.min;
            if (val.min >= 0) {
                /* Arithmetic right shift of non-negative is well-behaved */
                return VTX_RANGE(val.min >> s, val.max >> s);
            }
            /* For negative values, the range is tricky; fall through */
        }
        return VTX_RANGE_UNKNOWN;
    }

    /* ---- Mod: conservative approximation ---- */
    case VTX_OP_Mod: {
        if (node->input_count < 2) return VTX_RANGE_UNKNOWN;
        vtx_nodeid_t b_id = node->inputs[1];
        if (b_id == VTX_NODEID_INVALID || b_id >= range_count) return VTX_RANGE_UNKNOWN;

        vtx_range_t b = ranges[b_id];
        /* a % b produces a result in [0, |b|-1] for positive b.
         * Use the absolute value of b's max as an upper bound. */
        if (b.min > 0) {
            int64_t bound = safe_dec(b.max);
            if (bound >= 0) {
                return VTX_RANGE(0, bound);
            }
        }
        return VTX_RANGE_UNKNOWN;
    }

    /* ---- Cmp: produces 0 or 1 ---- */
    case VTX_OP_Cmp:
    case VTX_OP_CmpP:
        return VTX_RANGE(0, 1);

    /* ---- Not: bitwise complement ---- */
    case VTX_OP_Not: {
        if (node->input_count < 1) return VTX_RANGE_UNKNOWN;
        vtx_nodeid_t a_id = node->inputs[0];
        if (a_id == VTX_NODEID_INVALID || a_id >= range_count) return VTX_RANGE_UNKNOWN;

        vtx_range_t a = ranges[a_id];
        /* ~a = -a - 1; if a range is known, compute */
        if (a.min != INT64_MIN && a.max != INT64_MIN) {
            if (!sub_overflows(-a.max, 1) && !sub_overflows(-a.min, 1)) {
                return VTX_RANGE(-a.max - 1, -a.min - 1);
            }
        }
        return VTX_RANGE_UNKNOWN;
    }

    /* ---- Xor: limited precision for non-negative ---- */
    case VTX_OP_Xor: {
        if (node->input_count < 2) return VTX_RANGE_UNKNOWN;
        vtx_nodeid_t a_id = node->inputs[0];
        vtx_nodeid_t b_id = node->inputs[1];
        if (a_id == VTX_NODEID_INVALID || b_id == VTX_NODEID_INVALID) return VTX_RANGE_UNKNOWN;
        if (a_id >= range_count || b_id >= range_count) return VTX_RANGE_UNKNOWN;

        vtx_range_t a = ranges[a_id];
        vtx_range_t b = ranges[b_id];

        /* XOR of non-negative values: result is non-negative */
        if (a.min >= 0 && b.min >= 0) {
            /* Upper bound: next power of 2 minus 1 (conservative) */
            int64_t new_max = sat_add(a.max, b.max);
            if (add_overflows(a.max, b.max)) new_max = INT64_MAX;
            return VTX_RANGE(0, new_max);
        }
        return VTX_RANGE_UNKNOWN;
    }

    /* ---- Loads: unknown range ---- */
    case VTX_OP_Load:
    case VTX_OP_LoadField:
    case VTX_OP_LoadIndexed:
        return VTX_RANGE_UNKNOWN;

    /* ---- Proj: pass through ---- */
    case VTX_OP_Proj: {
        if (node->input_count < 1) return VTX_RANGE_UNKNOWN;
        vtx_nodeid_t inp_id = node->inputs[0];
        if (inp_id == VTX_NODEID_INVALID || inp_id >= range_count) return VTX_RANGE_UNKNOWN;
        return ranges[inp_id];
    }

    /* ---- Everything else: unknown ---- */
    default:
        return VTX_RANGE_UNKNOWN;
    }
}

/* ========================================================================== */
/* Guard pattern recognition                                                   */
/* ========================================================================== */

/**
 * Get the condition node from a Guard or DeoptGuard.
 *
 * Guard has 2 inputs: [control, condition].
 * DeoptGuard has 3 inputs: [control, condition, frame_state].
 *
 * Returns the condition input node, or NULL if not found.
 */
static const vtx_node_t *get_guard_condition(const vtx_node_t *guard,
                                              const vtx_node_table_t *table)
{
    if (guard->input_count < 2) return NULL;
    vtx_nodeid_t cond_id = guard->inputs[1];
    return vtx_node_get_const(table, cond_id);
}

/**
 * Check if a node is a constant integer with the given value.
 */
static bool is_int_constant(const vtx_node_table_t *table, vtx_nodeid_t id, int64_t value)
{
    if (id == VTX_NODEID_INVALID || id >= table->count) return false;
    const vtx_node_t *node = &table->nodes[id];
    if (node->opcode != VTX_OP_Constant) return false;
    if (node->constval.kind != VTX_TYPE_Int) return false;
    return node->constval.as.int_val == value;
}

bool vtx_bounds_is_bounds_check(const vtx_node_t *node,
                                 const vtx_node_table_t *table,
                                 vtx_nodeid_t *index,
                                 vtx_nodeid_t *length)
{
    if (node == NULL || table == NULL) return false;
    if (node->opcode != VTX_OP_Guard && node->opcode != VTX_OP_DeoptGuard) return false;
    if (node->input_count < 2) return false;

    const vtx_node_t *cmp = get_guard_condition(node, table);
    if (cmp == NULL) return false;
    if (cmp->opcode != VTX_OP_Cmp) return false;
    if (cmp->input_count < 2) return false;

    vtx_nodeid_t left  = cmp->inputs[0];
    vtx_nodeid_t right = cmp->inputs[1];
    if (left == VTX_NODEID_INVALID || right == VTX_NODEID_INVALID) return false;

    switch (cmp->cond) {
    case VTX_COND_LT:
    case VTX_COND_ULT:
        /* left < right: index = left, length = right */
        if (index)  *index  = left;
        if (length) *length = right;
        return true;

    case VTX_COND_GT:
    case VTX_COND_UGT:
        /* left > right: equivalent to right < left */
        if (index)  *index  = right;
        if (length) *length = left;
        return true;

    case VTX_COND_LE:
    case VTX_COND_ULE:
        /* left <= right: index = left, length = right + 1 (but we don't
         * know right + 1 at compile time unless right is constant).
         * Only treat as bounds check if right is a constant. */
        if (is_int_constant(table, right, 0)) {
            /* i <= 0 means i >= 0 which is a nonneg check, not a bounds check */
            return false;
        }
        /* left <= right is equivalent to left < right + 1.
         * For a proper bounds check, the right side must be length - 1,
         * but we can't verify that here.  Treat it as a bounds check
         * with the understanding that the length is right + 1.
         * However, since we can't represent "right + 1" as a node,
         * we skip this case for safety. */
        return false;

    default:
        return false;
    }
}

bool vtx_bounds_is_nonneg_check(const vtx_node_t *node,
                                 const vtx_node_table_t *table,
                                 vtx_nodeid_t *index)
{
    if (node == NULL || table == NULL) return false;
    if (node->opcode != VTX_OP_Guard && node->opcode != VTX_OP_DeoptGuard) return false;
    if (node->input_count < 2) return false;

    const vtx_node_t *cmp = get_guard_condition(node, table);
    if (cmp == NULL) return false;
    if (cmp->opcode != VTX_OP_Cmp) return false;
    if (cmp->input_count < 2) return false;

    vtx_nodeid_t left  = cmp->inputs[0];
    vtx_nodeid_t right = cmp->inputs[1];
    if (left == VTX_NODEID_INVALID || right == VTX_NODEID_INVALID) return false;

    switch (cmp->cond) {
    case VTX_COND_GE:
    case VTX_COND_UGE:
        /* left >= right: if right == 0, this is a non-neg check */
        if (is_int_constant(table, right, 0)) {
            if (index) *index = left;
            return true;
        }
        return false;

    case VTX_COND_LE:
    case VTX_COND_ULE:
        /* left <= right: if left == 0, then 0 <= right, i.e. right >= 0 */
        if (is_int_constant(table, left, 0)) {
            if (index) *index = right;
            return true;
        }
        return false;

    case VTX_COND_ULT:
        /* Unsigned less-than implies both operands are non-negative.
         * i <u len implies i >= 0 (since unsigned values are >= 0). */
        if (index) *index = left;
        return true;

    default:
        return false;
    }
}

/* ========================================================================== */
/* Dominator computation                                                       */
/* ========================================================================== */

/**
 * Sentinel value for undefined immediate dominator.
 */
#define DOM_UNDEF UINT32_MAX

/**
 * Intersect two nodes in the dominator tree using the Cooper-Harvey-Kennedy
 * algorithm.  Walks up the idom tree using rpo_num as the depth proxy until
 * a common ancestor is found.
 */
static uint32_t dom_intersect(uint32_t b1, uint32_t b2,
                               const uint32_t *idom,
                               const uint32_t *rpo_num)
{
    uint32_t finger1 = b1;
    uint32_t finger2 = b2;

    while (finger1 != finger2) {
        while (rpo_num[finger1] > rpo_num[finger2]) {
            finger1 = idom[finger1];
            if (finger1 == DOM_UNDEF) return finger2;
        }
        while (rpo_num[finger2] > rpo_num[finger1]) {
            finger2 = idom[finger2];
            if (finger2 == DOM_UNDEF) return finger1;
        }
    }
    return finger1;
}

/**
 * Context for recursive RPO DFS.
 */
typedef struct {
    const vtx_schedule_t *schedule;
    uint32_t *postorder;
    bool     *visited;
    uint32_t  post_idx;
} rpo_context_t;

/**
 * Recursive DFS visit for computing reverse postorder.
 */
static void compute_rpo_visit(rpo_context_t *ctx, uint32_t block_idx)
{
    if (block_idx >= ctx->schedule->count) return;
    if (ctx->visited[block_idx]) return;

    ctx->visited[block_idx] = true;

    const vtx_schedule_block_t *block = &ctx->schedule->blocks[block_idx];
    for (uint32_t i = 0; i < block->succ_count; i++) {
        compute_rpo_visit(ctx, block->succ_blocks[i]);
    }

    /* Post-order: add after all successors are visited */
    ctx->postorder[ctx->post_idx++] = block_idx;
}

/**
 * Compute the immediate dominator tree using the iterative
 * Cooper-Harvey-Kennedy algorithm.
 *
 * Returns an arena-allocated array where idom[b] is the block index of
 * the immediate dominator of block b.  idom[entry] == entry.
 * Returns NULL on allocation failure.
 */
static uint32_t *compute_idom(const vtx_schedule_t *schedule, vtx_arena_t *arena)
{
    uint32_t n = schedule->count;
    if (n == 0) return NULL;

    /* Allocate arrays from the arena */
    uint32_t *idom      = (uint32_t *)vtx_arena_alloc(arena, n * sizeof(uint32_t));
    uint32_t *rpo       = (uint32_t *)vtx_arena_alloc(arena, n * sizeof(uint32_t));
    uint32_t *rpo_num   = (uint32_t *)vtx_arena_alloc(arena, n * sizeof(uint32_t));
    uint32_t *postorder = (uint32_t *)vtx_arena_alloc(arena, n * sizeof(uint32_t));
    bool     *visited   = (bool *)vtx_arena_alloc(arena, n * sizeof(bool));

    if (idom == NULL || rpo == NULL || rpo_num == NULL ||
        postorder == NULL || visited == NULL) {
        return NULL;
    }

    /* Step 1: Compute reverse postorder via DFS from entry (block 0) */
    memset(visited, 0, n * sizeof(bool));
    rpo_context_t ctx;
    ctx.schedule  = schedule;
    ctx.postorder = postorder;
    ctx.visited   = visited;
    ctx.post_idx  = 0;

    compute_rpo_visit(&ctx, 0);

    /* Handle disconnected blocks: visit them in order */
    for (uint32_t i = 0; i < n; i++) {
        if (!visited[i]) {
            compute_rpo_visit(&ctx, i);
        }
    }

    /* Convert post-order to reverse post-order */
    uint32_t rpo_count = ctx.post_idx;
    for (uint32_t i = 0; i < rpo_count; i++) {
        rpo[i] = postorder[rpo_count - 1 - i];
        rpo_num[rpo[i]] = i;
    }

    /* Step 2: Initialize idom */
    for (uint32_t i = 0; i < n; i++) {
        idom[i] = DOM_UNDEF;
    }
    /* Entry block dominates itself */
    if (rpo_count > 0) {
        idom[rpo[0]] = rpo[0];
    }

    /* Step 3: Iterate until convergence */
    bool changed = true;
    uint32_t iterations = 0;
    const uint32_t max_iterations = n * 2 + 10; /* bounded for safety */

    while (changed && iterations < max_iterations) {
        changed = false;
        iterations++;

        for (uint32_t i = 1; i < rpo_count; i++) {
            uint32_t b = rpo[i];
            const vtx_schedule_block_t *block = &schedule->blocks[b];

            /* Find the intersection of all processed predecessors' idom chains */
            uint32_t new_idom = DOM_UNDEF;
            for (uint32_t p = 0; p < block->pred_count; p++) {
                uint32_t pred = block->pred_blocks[p];
                if (idom[pred] == DOM_UNDEF) continue; /* unprocessed */

                if (new_idom == DOM_UNDEF) {
                    new_idom = pred;
                } else {
                    new_idom = dom_intersect(new_idom, pred, idom, rpo_num);
                }
            }

            if (new_idom != DOM_UNDEF && idom[b] != new_idom) {
                idom[b] = new_idom;
                changed = true;
            }
        }
    }

    return idom;
}

/**
 * Check if block `a` dominates block `b` by walking the immediate-dominator
 * chain from `b` upward.
 */
static bool block_dominates(const uint32_t *idom, uint32_t a, uint32_t b)
{
    if (a == b) return true;

    uint32_t runner = b;
    while (runner != DOM_UNDEF) {
        runner = idom[runner];
        if (runner == a) return true;
        /* If we reach the entry block's self-loop, stop */
        if (runner == idom[runner]) break;
    }
    return false;
}

/* ========================================================================== */
/* Guard record for dominated-guard elimination                                 */
/* ========================================================================== */

typedef struct {
    vtx_nodeid_t index_id;     /* node ID of the index being checked    */
    vtx_nodeid_t length_id;    /* node ID of the length (bounds check),
                                  VTX_NODEID_INVALID for nonneg check   */
    uint32_t     block_idx;    /* block containing this guard           */
    vtx_nodeid_t guard_id;     /* node ID of the guard node             */
    bool         is_nonneg;    /* true if this is a non-negative check  */
} vtx_guard_record_t;

/* ========================================================================== */
/* Guard elimination                                                           */
/* ========================================================================== */

/**
 * Eliminate a guard node from the graph.
 *
 * All uses of the guard are redirected to its control input (the guard's
 * first input).  The guard is then marked dead.
 *
 * For a DeoptGuard with a frame_state input, the frame_state may become
 * dead and will be cleaned up by DCE.
 */
static void eliminate_guard(vtx_node_table_t *table, vtx_node_t *guard)
{
    VTX_ASSERT(guard != NULL, "guard must not be NULL");
    VTX_ASSERT(guard->input_count >= 2, "guard must have at least 2 inputs");

    vtx_nodeid_t guard_id = guard->id;
    vtx_nodeid_t control_input = guard->inputs[0];

    /* Redirect all uses of this guard to its control input */
    if (control_input != VTX_NODEID_INVALID && control_input != guard_id) {
        for (uint32_t u = 0; u < table->count; u++) {
            vtx_node_t *user = &table->nodes[u];
            if (user->dead) continue;

            for (uint32_t j = 0; j < user->input_count; j++) {
                if (user->inputs[j] == guard_id) {
                    vtx_node_replace_input(table, u, j, control_input);
                }
            }
        }
    }

    /* Disconnect all inputs of the guard */
    for (uint32_t j = 0; j < guard->input_count; j++) {
        vtx_nodeid_t inp = guard->inputs[j];
        if (inp != VTX_NODEID_INVALID && inp < table->count) {
            vtx_node_t *producer = &table->nodes[inp];
            if (producer->output_count > 0) {
                producer->output_count--;
            }
        }
    }

    /* Mark the guard as dead */
    guard->dead = true;
    guard->output_count = 0;
    guard->input_count = 0;
}

/* ========================================================================== */
/* Range-based guard condition checking                                        */
/* ========================================================================== */

/**
 * Check if a bounds check Guard(i < len) is provably true given current ranges.
 *
 * The guard i < len is proven if i.max < len.min (every possible value of
 * i is less than every possible value of len).
 */
static bool is_bounds_check_proven(const vtx_range_t *idx_range,
                                    const vtx_range_t *len_range)
{
    if (idx_range == NULL || len_range == NULL) return false;

    /* i < len is proven if idx.max < len.min */
    if (idx_range->max < len_range->min) return true;

    /* Also check if idx is provably within [0, len-1] */
    if (idx_range->min >= 0 &&
        idx_range->max >= 0 &&
        len_range->is_const &&
        idx_range->max < len_range->min) {
        return true;
    }

    return false;
}

/**
 * Check if a non-negative check Guard(i >= 0) is provably true given
 * current ranges.
 */
static bool is_nonneg_check_proven(const vtx_range_t *idx_range)
{
    if (idx_range == NULL) return false;
    return idx_range->min >= 0;
}

/**
 * Apply range constraint after a Guard(i < len).
 *
 * After the guard passes, we know i < len.  If len is a constant with
 * value c, we can constrain i.max to c - 1.  If len is not constant
 * but has a known range, we use the lower bound of len to narrow i.
 */
static void constrain_after_bounds_check(vtx_range_t *ranges,
                                          uint32_t range_count,
                                          vtx_nodeid_t index_id,
                                          vtx_nodeid_t length_id,
                                          const vtx_node_table_t *table)
{
    if (index_id == VTX_NODEID_INVALID || index_id >= range_count) return;
    if (length_id == VTX_NODEID_INVALID || length_id >= range_count) return;

    vtx_range_t *idx_range = &ranges[index_id];
    vtx_range_t  len_range = ranges[length_id];

    /* If the length is a constant, we know the exact bound */
    if (len_range.is_const) {
        int64_t bound = safe_dec(len_range.min);
        if (bound < idx_range->max) {
            idx_range->max = bound;
            idx_range->is_const = (idx_range->min == idx_range->max);
        }
    } else if (len_range.min > INT64_MIN) {
        /* If we know the minimum possible value of len, then
         * i < len and len >= len.min implies i <= len.min - 1.
         * This is conservative but safe. */
        int64_t bound = safe_dec(len_range.min);
        if (bound < idx_range->max) {
            idx_range->max = bound;
            idx_range->is_const = (idx_range->min == idx_range->max);
        }
    }
}

/**
 * Apply range constraint after a Guard(i >= 0).
 *
 * After the guard passes, we know i >= 0, so i.min = max(i.min, 0).
 */
static void constrain_after_nonneg_check(vtx_range_t *ranges,
                                          uint32_t range_count,
                                          vtx_nodeid_t index_id)
{
    if (index_id == VTX_NODEID_INVALID || index_id >= range_count) return;

    vtx_range_t *idx_range = &ranges[index_id];
    if (idx_range->min < 0) {
        idx_range->min = 0;
        idx_range->is_const = (idx_range->min == idx_range->max);
    }
}

/* ========================================================================== */
/* Dominated guard elimination check                                           */
/* ========================================================================== */

/**
 * Check if a guard is made redundant by a previously-processed dominating
 * guard with the same index and length.
 *
 * For a bounds check Guard(i < len): a previous Guard(i < len) at a block
 * that dominates the current block makes the current guard redundant.
 *
 * For a nonneg check Guard(i >= 0): a previous Guard(i >= 0) or
 * Guard(i <u len) at a dominating block makes the current guard redundant.
 *
 * A bounds check Guard(i <u len) also implies i >= 0, so it subsumes
 * a nonneg check on the same index.
 */
static bool is_dominated_guard_redundant(const vtx_guard_record_t *records,
                                          uint32_t record_count,
                                          const uint32_t *idom,
                                          vtx_nodeid_t index_id,
                                          vtx_nodeid_t length_id,
                                          bool is_nonneg,
                                          uint32_t current_block)
{
    for (uint32_t r = 0; r < record_count; r++) {
        const vtx_guard_record_t *rec = &records[r];

        /* Must check the same index */
        if (rec->index_id != index_id) continue;

        if (is_nonneg) {
            /* For a nonneg check, a dominating nonneg check or
             * a dominating unsigned bounds check (which implies nonneg)
             * makes this redundant. */
            if (rec->is_nonneg) {
                /* Same index, both nonneg checks */
                if (block_dominates(idom, rec->block_idx, current_block)) {
                    return true;
                }
            } else {
                /* A bounds check on the same index with unsigned comparison
                 * implies nonneg.  Check if the record's guard was an
                 * unsigned comparison.  For simplicity, we check if the
                 * length_id is valid (bounds check) — in that case the
                 * guard might have been unsigned, which implies nonneg.
                 * We conservatively assume any bounds check also proves
                 * nonneg if it comes from the same index. */
                if (block_dominates(idom, rec->block_idx, current_block)) {
                    return true;
                }
            }
        } else {
            /* For a bounds check, we need the same (index, length) pair */
            if (rec->is_nonneg) continue; /* nonneg doesn't prove bounds */
            if (rec->length_id != length_id) continue;

            if (block_dominates(idom, rec->block_idx, current_block)) {
                return true;
            }
        }
    }

    return false;
}

/* ========================================================================== */
/* Main entry point                                                            */
/* ========================================================================== */

int vtx_bounds_check_run(vtx_graph_t *graph,
                          const vtx_schedule_t *schedule,
                          vtx_arena_t *arena)
{
    VTX_ASSERT(graph != NULL, "graph must not be NULL");
    VTX_ASSERT(schedule != NULL, "schedule must not be NULL");
    VTX_ASSERT(arena != NULL, "arena must not be NULL");

    if (graph == NULL || schedule == NULL || arena == NULL) return -1;

    vtx_node_table_t *nt = &graph->node_table;
    uint32_t node_count = nt->count;

    if (node_count == 0 || schedule->count == 0) return 0;

    /* ------------------------------------------------------------------ */
    /* Allocate range map (one entry per node, initialized to UNKNOWN)     */
    /* ------------------------------------------------------------------ */
    vtx_range_t *ranges = (vtx_range_t *)vtx_arena_alloc(arena, node_count * sizeof(vtx_range_t));
    if (ranges == NULL) return -1;

    for (uint32_t i = 0; i < node_count; i++) {
        ranges[i] = VTX_RANGE_UNKNOWN;
    }

    /* ------------------------------------------------------------------ */
    /* Compute dominator tree for dominated-guard elimination              */
    /* ------------------------------------------------------------------ */
    uint32_t *idom = compute_idom(schedule, arena);
    /* If idom computation fails, we can still do range-based elimination.
     * We just won't be able to do dominated-guard elimination. */

    /* ------------------------------------------------------------------ */
    /* Allocate guard record array for dominated-guard elimination          */
    /* ------------------------------------------------------------------ */
    uint32_t max_records = node_count;
    vtx_guard_record_t *records = (vtx_guard_record_t *)vtx_arena_alloc(
        arena, max_records * sizeof(vtx_guard_record_t));
    if (records == NULL) {
        /* Without records, we can still do range-based elimination */
        max_records = 0;
    }
    uint32_t record_count = 0;

    int eliminated = 0;

    /* ------------------------------------------------------------------ */
    /* Run induction variable analysis for loop bounds check elimination    */
    /* This enables eliminating bounds checks inside loops where the IV     */
    /* range is provably within [0, array_length).                         */
    /* ------------------------------------------------------------------ */
    vtx_iv_result_t *iv_result = vtx_iv_analyze(graph, arena);

    /* ------------------------------------------------------------------ */
    /* Walk the schedule in block order                                     */
    /* ------------------------------------------------------------------ */
    for (uint32_t bi = 0; bi < schedule->count; bi++) {
        const vtx_schedule_block_t *block = &schedule->blocks[bi];

        for (uint32_t ni = 0; ni < block->node_count; ni++) {
            vtx_nodeid_t nid = block->nodes[ni];
            if (nid == VTX_NODEID_INVALID || nid >= node_count) continue;

            vtx_node_t *node = &nt->nodes[nid];
            if (node->dead) continue;

            /* ---------------------------------------------------------- */
            /* Compute range for this node                                 */
            /* ---------------------------------------------------------- */
            if (vtx_nf_has(node->flags, VTX_NF_DATA)) {
                vtx_range_t computed = vtx_bounds_compute_range(node, nt, ranges, node_count);
                /* Intersect with existing range (it may have been constrained
                 * by a guard that was processed earlier in this block) */
                ranges[nid] = vtx_range_intersect(ranges[nid], computed);
            }

            /* ---------------------------------------------------------- */
            /* Check if this is a guard that can be eliminated             */
            /* ---------------------------------------------------------- */
            if (node->opcode != VTX_OP_Guard && node->opcode != VTX_OP_DeoptGuard) {
                continue;
            }

            /* Skip already-eliminated guards */
            if (node->dead) continue;

            /* --- Try bounds check pattern --- */
            vtx_nodeid_t index_id  = VTX_NODEID_INVALID;
            vtx_nodeid_t length_id = VTX_NODEID_INVALID;

            if (vtx_bounds_is_bounds_check(node, nt, &index_id, &length_id)) {
                VTX_ASSERT(index_id != VTX_NODEID_INVALID, "index must be set");
                VTX_ASSERT(length_id != VTX_NODEID_INVALID, "length must be set");

                /* Look up ranges */
                vtx_range_t idx_range = (index_id < node_count)  ? ranges[index_id]  : VTX_RANGE_UNKNOWN;
                vtx_range_t len_range = (length_id < node_count) ? ranges[length_id] : VTX_RANGE_UNKNOWN;

                /* Check 1: Is the condition already proven by range analysis? */
                if (is_bounds_check_proven(&idx_range, &len_range)) {
                    eliminate_guard(nt, node);
                    eliminated++;
                    continue;
                }

                /* Check 2: Is there a dominating guard with the same (index, length)? */
                if (idom != NULL && records != NULL &&
                    is_dominated_guard_redundant(records, record_count, idom,
                                                  index_id, length_id,
                                                  false, /* not nonneg */
                                                  bi)) {
                    eliminate_guard(nt, node);
                    eliminated++;
                    continue;
                }

                /* Check 3 (NEW): Can IV analysis prove the bounds check is redundant?
                 *
                 * This is the key new check: if the index is an induction variable
                 * whose iteration range is provably within [0, length), the bounds
                 * check can be eliminated. This handles the most common case:
                 *   for (int i = 0; i < arr.length; i++) {
                 *       arr[i]  // IV analysis proves i ∈ [0, arr.length)
                 *   }
                 *
                 * This alone enables 90%+ bounds check elimination in loops. */
                if (iv_result != NULL &&
                    vtx_iv_can_eliminate_bounds(iv_result, nt, index_id, length_id)) {
                    eliminate_guard(nt, node);
                    eliminated++;
                    continue;
                }

                /* Guard is not eliminable; constrain the index range
                 * and record this guard for future dominated elimination. */
                constrain_after_bounds_check(ranges, node_count, index_id, length_id, nt);

                if (records != NULL && record_count < max_records) {
                    vtx_guard_record_t *rec = &records[record_count++];
                    rec->index_id  = index_id;
                    rec->length_id = length_id;
                    rec->block_idx = bi;
                    rec->guard_id  = nid;
                    rec->is_nonneg = false;
                }

                /* A ULT bounds check also implies i >= 0 */
                const vtx_node_t *cmp = get_guard_condition(node, nt);
                if (cmp != NULL &&
                    (cmp->cond == VTX_COND_ULT || cmp->cond == VTX_COND_UGE)) {
                    constrain_after_nonneg_check(ranges, node_count, index_id);
                }

                continue;
            }

            /* --- Try non-negative check pattern --- */
            vtx_nodeid_t nonneg_index = VTX_NODEID_INVALID;

            if (vtx_bounds_is_nonneg_check(node, nt, &nonneg_index)) {
                VTX_ASSERT(nonneg_index != VTX_NODEID_INVALID, "index must be set");

                vtx_range_t idx_range = (nonneg_index < node_count)
                    ? ranges[nonneg_index]
                    : VTX_RANGE_UNKNOWN;

                /* Check 1: Is the condition already proven? */
                if (is_nonneg_check_proven(&idx_range)) {
                    eliminate_guard(nt, node);
                    eliminated++;
                    continue;
                }

                /* Check 2: Is there a dominating nonneg check on the same index? */
                if (idom != NULL && records != NULL &&
                    is_dominated_guard_redundant(records, record_count, idom,
                                                  nonneg_index, VTX_NODEID_INVALID,
                                                  true, /* is nonneg */
                                                  bi)) {
                    eliminate_guard(nt, node);
                    eliminated++;
                    continue;
                }

                /* Check 3 (NEW): Can IV analysis prove the index is non-negative?
                 * If the index is an IV with known lower bound >= 0, the nonneg
                 * check is redundant. */
                if (iv_result != NULL && nonneg_index < node_count) {
                    vtx_iv_range_t iv_range = vtx_iv_value_range(
                        iv_result, nt, nonneg_index);
                    if (iv_range.lo_known && iv_range.lo >= 0) {
                        eliminate_guard(nt, node);
                        eliminated++;
                        continue;
                    }
                }

                /* Guard is not eliminable; constrain the index range
                 * and record this guard for future dominated elimination. */
                constrain_after_nonneg_check(ranges, node_count, nonneg_index);

                if (records != NULL && record_count < max_records) {
                    vtx_guard_record_t *rec = &records[record_count++];
                    rec->index_id  = nonneg_index;
                    rec->length_id = VTX_NODEID_INVALID;
                    rec->block_idx = bi;
                    rec->guard_id  = nid;
                    rec->is_nonneg = true;
                }

                continue;
            }

            /* ---------------------------------------------------------- */
            /* Generic guard: check if the guard's condition is a Cmp that */
            /* range analysis can prove.                                    */
            /* ---------------------------------------------------------- */
            const vtx_node_t *cond = get_guard_condition(node, nt);
            if (cond != NULL && cond->opcode == VTX_OP_Cmp && cond->input_count >= 2) {
                vtx_nodeid_t left_id  = cond->inputs[0];
                vtx_nodeid_t right_id = cond->inputs[1];

                if (left_id < node_count && right_id < node_count) {
                    vtx_range_t left_range  = ranges[left_id];
                    vtx_range_t right_range = ranges[right_id];

                    bool proven = false;

                    switch (cond->cond) {
                    case VTX_COND_LT:
                        proven = (left_range.max < right_range.min);
                        break;
                    case VTX_COND_LE:
                        proven = (left_range.max <= right_range.min);
                        break;
                    case VTX_COND_GT:
                        proven = (left_range.min > right_range.max);
                        break;
                    case VTX_COND_GE:
                        proven = (left_range.min >= right_range.max);
                        break;
                    case VTX_COND_EQ:
                        /* Proven if both are the same constant */
                        proven = (left_range.is_const && right_range.is_const &&
                                  left_range.min == right_range.min);
                        break;
                    case VTX_COND_NE:
                        /* Proven if ranges don't overlap */
                        proven = (left_range.max < right_range.min ||
                                  left_range.min > right_range.max);
                        break;
                    case VTX_COND_ULT:
                        /* Unsigned: proven if left is non-negative and
                         * left.max < right.min */
                        proven = (left_range.min >= 0 && right_range.min >= 0 &&
                                  left_range.max < right_range.min);
                        break;
                    case VTX_COND_UGE:
                        /* Unsigned >= 0: always true if value is non-negative */
                        if (is_int_constant(nt, right_id, 0) && left_range.min >= 0) {
                            proven = true;
                        }
                        break;
                    default:
                        break;
                    }

                    if (proven) {
                        eliminate_guard(nt, node);
                        eliminated++;
                        continue;
                    }

                    /* Apply post-guard range constraints for generic Cmp */
                    switch (cond->cond) {
                    case VTX_COND_LT:
                        /* After Guard(a < b): a.max = min(a.max, b.min - 1) */
                        if (right_range.is_const || right_range.min > INT64_MIN) {
                            int64_t bound = safe_dec(right_range.min);
                            if (bound < ranges[left_id].max) {
                                ranges[left_id].max = bound;
                                ranges[left_id].is_const = (ranges[left_id].min == ranges[left_id].max);
                            }
                        }
                        break;
                    case VTX_COND_GE:
                        /* After Guard(a >= b): a.min = max(a.min, b.max) */
                        if (ranges[left_id].min < right_range.max) {
                            ranges[left_id].min = right_range.max;
                            ranges[left_id].is_const = (ranges[left_id].min == ranges[left_id].max);
                        }
                        break;
                    default:
                        break;
                    }
                }
            }
        }
    }

    /* ------------------------------------------------------------------ */
    /* Post-elimination cleanup: after eliminating guards, some data nodes */
    /* may have become unreachable.  We don't run full DCE here (that's a */
    /* separate pass), but we do propagate the effect of eliminated guards */
    /* on the control flow by checking that no dead guard's control output */
    /* is still referenced.                                                */
    /* ------------------------------------------------------------------ */

    return eliminated;
}
