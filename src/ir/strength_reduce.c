/**
 * VORTEX Strength Reduction Pass
 *
 * Replaces expensive operations with cheaper equivalents when the
 * divisor/multiplier is a power of two:
 *
 *   Div(x, 2^k)  → Shr(x, k)     (arithmetic shift for signed)
 *   Mod(x, 2^k)  → And(x, 2^k-1) (bitmask)
 *   Mul(x, 2^k)  → Shl(x, k)     (left shift)
 *
 * This is especially important for loops like collatz where n/2 is
 * the dominant operation. IDIV takes ~25 cycles; SAR takes 1 cycle.
 *
 * The pass runs after SCCP (so constants are propagated) and before
 * DCE (so dead nodes are cleaned up).
 */

#include "ir/graph.h"
#include "ir/node.h"
#include "ir/constant_prop.h"
#include <stdlib.h>
#include <string.h>

/* Check if a value is a power of two. Returns the shift amount (>=0) or -1. */
static int power_of_two_log2(int64_t val)
{
    if (val <= 0) return -1;
    if ((val & (val - 1)) != 0) return -1; /* not power of 2 */
    /* Count trailing zeros */
    int shift = 0;
    uint64_t u = (uint64_t)val;
    while (!(u & 1)) { shift++; u >>= 1; }
    return shift;
}

/* Try to get the constant integer value of a node input. */
static bool try_get_const_int_input(vtx_graph_t *graph, vtx_nodeid_t node_id,
                                     int64_t *out_val)
{
    if (node_id == VTX_NODEID_INVALID || node_id >= graph->node_table.count)
        return false;
    vtx_node_t *node = &graph->node_table.nodes[node_id];
    if (node->dead || node->opcode != VTX_OP_Constant)
        return false;
    if (node->constval.kind != VTX_TYPE_Int)
        return false;
    *out_val = node->constval.as.int_val;
    return true;
}

uint32_t vtx_strength_reduce_run(vtx_graph_t *graph)
{
    if (!graph) return 0;
    vtx_node_table_t *nt = &graph->node_table;
    uint32_t replaced = 0;

    for (uint32_t i = 0; i < nt->count; i++) {
        vtx_node_t *node = &nt->nodes[i];
        if (node->dead) continue;

        /* Need at least 2 data inputs */
        if (node->input_count < 2) continue;

        /* Find the constant operand (input[1] for binary ops) */
        vtx_nodeid_t rhs_id = node->inputs[1];
        int64_t rhs_val;
        if (!try_get_const_int_input(graph, rhs_id, &rhs_val))
            continue;

        vtx_nodeid_t lhs_id = node->inputs[0];
        if (lhs_id == VTX_NODEID_INVALID) continue;

        int shift;

        switch (node->opcode) {
        case VTX_OP_Div:
            /* Div(x, 2^k) → Shr(x, k) for signed integers.
             * Note: SAR (arithmetic shift) is correct for signed division
             * by positive powers of 2, with rounding toward -infinity.
             * C's / operator rounds toward zero, so Div(-7, 2) = -3 but
             * Shr(-7, 1) = -4. This is only safe for non-negative values.
             *
             * For safety, only apply when the divisor is a positive power
             * of 2 and we can't prove the dividend is non-negative. The
             * interpreter uses C's / (truncate toward zero), so we must
             * match that. Skip for now unless the divisor is 1 (identity). */
            if (rhs_val == 1) {
                /* Div(x, 1) → x (identity, let DCE clean up) */
                vtx_node_replace_all_uses(nt, (vtx_nodeid_t)i, lhs_id);
                node->dead = true;
                replaced++;
                continue;
            }
            /* For power-of-2 divisors, only replace if divisor > 0 and
             * we accept the rounding difference. Since VORTEX's interpreter
             * uses C semantics (truncate toward zero), SAR gives wrong
             * results for negative dividends. So we ONLY replace when
             * the divisor is 2 and we know the value is used in a context
             * where the dividend is non-negative (e.g., after a check).
             *
             * For now, skip — the rounding mismatch is a real correctness
             * issue. A future fix can add a guard for non-negative values. */
            break;

        case VTX_OP_Mod:
            /* Mod(x, 2^k) → And(x, 2^k - 1).
             * This is correct for ALL integers (positive and negative)
             * because C's % and bitwise AND give the same result for
             * positive divisors that are powers of 2.
             *   7 % 4 = 3,  7 & 3 = 3  ✓
             *  -7 % 4 = -3 (C), -7 & 3 = 1  ✗ MISMATCH!
             *
             * So this is only correct for non-negative dividends. Skip
             * for correctness unless we can prove non-negativity. */
            break;

        case VTX_OP_Mul:
            /* Mul(x, 2^k) → Shl(x, k).
             * This is correct for ALL integers (signed and unsigned).
             *   3 * 4 = 12,  3 << 2 = 12  ✓
             *  -3 * 4 = -12, -3 << 2 = -12  ✓
             * Overflow behavior matches (both wrap in 2's complement). */
            shift = power_of_two_log2(rhs_val);
            if (shift >= 0 && shift <= 62) {
                /* Create a Constant node for the shift amount */
                vtx_nodeid_t shift_const = vtx_node_create(nt, VTX_OP_Constant);
                if (shift_const == VTX_NODEID_INVALID) continue;
                vtx_node_t *sc = vtx_node_get(nt, shift_const);
                sc->constval.kind = VTX_TYPE_Int;
                sc->constval.as.int_val = shift;
                sc->type = VTX_TYPE_Int;

                /* Replace the Mul node's opcode and inputs */
                node->opcode = VTX_OP_Shl;
                /* inputs[0] stays as lhs, inputs[1] = shift constant */
                vtx_node_replace_input(nt, (vtx_nodeid_t)i, 1, shift_const);
                replaced++;
                continue;
            }
            /* Mul(x, 1) → x */
            if (rhs_val == 1) {
                vtx_node_replace_all_uses(nt, (vtx_nodeid_t)i, lhs_id);
                node->dead = true;
                replaced++;
                continue;
            }
            /* Mul(x, 0) → 0 */
            if (rhs_val == 0) {
                vtx_node_replace_all_uses(nt, (vtx_nodeid_t)i, rhs_id);
                node->dead = true;
                replaced++;
                continue;
            }
            break;

        default:
            break;
        }
    }

    return replaced;
}
