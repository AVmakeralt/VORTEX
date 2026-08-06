/* ========================================================================== */
/* Algebraic Simplification Pass                                               */
/* ========================================================================== */
/*
 * ir/algebraic.c — Fold trivial algebraic identities in the IR.
 *
 * Runs after SCCP (constant propagation) and before DCE. Replaces:
 *   Add(x, 0)     → x
 *   Add(0, x)     → x
 *   Sub(x, 0)     → x
 *   Sub(x, x)     → 0
 *   Mul(x, 0)     → 0
 *   Mul(x, 1)     → x
 *   Mul(1, x)     → x
 *   Mul(x, -1)    → Neg(x)
 *   Div(x, 1)     → x
 *   And(x, 0)     → 0
 *   And(x, -1)    → x
 *   Or(x, 0)      → x
 *   Or(x, -1)     → -1
 *   Xor(x, 0)     → x
 *   Xor(x, x)     → 0
 *   Shl(x, 0)     → x
 *   Shr(x, 0)     → x
 *   Neg(Neg(x))   → x
 *   Not(Not(x))   → x
 *
 * Returns the number of nodes simplified.
 */

#include "ir/graph.h"
#include "ir/node.h"
#include "vortex_config.h"
#include <stdint.h>

/* Try to get a constant integer value from a node.
 * Returns true and sets *val if the node is a Constant with Int type. */
static bool try_get_const(vtx_node_table_t *nt, vtx_nodeid_t id, int64_t *val)
{
    if (id == VTX_NODEID_INVALID || id >= nt->count) return false;
    vtx_node_t *n = &nt->nodes[id];
    if (n->dead || n->opcode != VTX_OP_Constant) return false;
    if (n->constval.kind != VTX_TYPE_Int) return false;
    *val = n->constval.as.int_val;
    return true;
}

/* Replace all uses of `old_id` with `new_id` and mark `old_id` as dead. */
static void replace_node(vtx_node_table_t *nt, vtx_nodeid_t old_id, vtx_nodeid_t new_id)
{
    vtx_node_replace_all_uses(nt, old_id, new_id);
    nt->nodes[old_id].dead = true;
}

uint32_t vtx_algebraic_simplify_run(vtx_graph_t *graph)
{
    if (!graph) return 0;
    vtx_node_table_t *nt = &graph->node_table;
    uint32_t simplified = 0;

    for (uint32_t i = 0; i < nt->count; i++) {
        vtx_node_t *node = &nt->nodes[i];
        if (node->dead) continue;
        if (node->input_count < 1) continue;

        /* Skip float-typed nodes (different semantics) */
        if (node->type == VTX_TYPE_Float) continue;

        vtx_nodeid_t lhs = (node->input_count >= 1) ? node->inputs[0] : VTX_NODEID_INVALID;
        vtx_nodeid_t rhs = (node->input_count >= 2) ? node->inputs[1] : VTX_NODEID_INVALID;

        int64_t lhs_const, rhs_const;
        bool lhs_is_const = try_get_const(nt, lhs, &lhs_const);
        bool rhs_is_const = try_get_const(nt, rhs, &rhs_const);

        switch (node->opcode) {
        /* Add(x, 0) → x, Add(0, x) → x */
        case VTX_OP_Add:
            if (rhs_is_const && rhs_const == 0) {
                replace_node(nt, i, lhs);
                simplified++;
            } else if (lhs_is_const && lhs_const == 0) {
                replace_node(nt, i, rhs);
                simplified++;
            }
            break;

        /* Sub(x, 0) → x, Sub(x, x) → 0 */
        case VTX_OP_Sub:
            if (rhs_is_const && rhs_const == 0) {
                replace_node(nt, i, lhs);
                simplified++;
            } else if (lhs == rhs) {
                /* Sub(x, x) → 0 — create a constant 0 */
                vtx_nodeid_t zero = vtx_node_create(nt, VTX_OP_Constant);
                vtx_node_t *z = vtx_node_get(nt, zero);
                z->constval.kind = VTX_TYPE_Int;
                z->constval.as.int_val = 0;
                z->type = VTX_TYPE_Int;
                replace_node(nt, i, zero);
                simplified++;
            }
            break;

        /* Mul(x, 0) → 0, Mul(x, 1) → x, Mul(1, x) → x, Mul(x, -1) → Neg(x) */
        case VTX_OP_Mul:
            if (rhs_is_const && rhs_const == 0) {
                replace_node(nt, i, rhs);  /* replace with the 0 constant */
                simplified++;
            } else if (rhs_is_const && rhs_const == 1) {
                replace_node(nt, i, lhs);
                simplified++;
            } else if (lhs_is_const && lhs_const == 1) {
                replace_node(nt, i, rhs);
                simplified++;
            } else if (rhs_is_const && rhs_const == -1) {
                /* Mul(x, -1) → Neg(x) */
                node->opcode = VTX_OP_Neg;
                node->input_count = 1;
                /* Remove the second input (the -1 constant) */
                node->inputs[1] = VTX_NODEID_INVALID;
                simplified++;
            }
            break;

        /* Div(x, 1) → x */
        case VTX_OP_Div:
            if (rhs_is_const && rhs_const == 1) {
                replace_node(nt, i, lhs);
                simplified++;
            }
            break;

        /* And(x, 0) → 0, And(x, -1) → x */
        case VTX_OP_And:
            if (rhs_is_const && rhs_const == 0) {
                replace_node(nt, i, rhs);
                simplified++;
            } else if (rhs_is_const && rhs_const == -1) {
                replace_node(nt, i, lhs);
                simplified++;
            } else if (lhs_is_const && lhs_const == -1) {
                replace_node(nt, i, rhs);
                simplified++;
            }
            break;

        /* Or(x, 0) → x, Or(x, -1) → -1 */
        case VTX_OP_Or:
            if (rhs_is_const && rhs_const == 0) {
                replace_node(nt, i, lhs);
                simplified++;
            } else if (rhs_is_const && rhs_const == -1) {
                replace_node(nt, i, rhs);
                simplified++;
            } else if (lhs_is_const && lhs_const == 0) {
                replace_node(nt, i, rhs);
                simplified++;
            }
            break;

        /* Xor(x, 0) → x, Xor(x, x) → 0 */
        case VTX_OP_Xor:
            if (rhs_is_const && rhs_const == 0) {
                replace_node(nt, i, lhs);
                simplified++;
            } else if (lhs == rhs) {
                vtx_nodeid_t zero = vtx_node_create(nt, VTX_OP_Constant);
                vtx_node_t *z = vtx_node_get(nt, zero);
                z->constval.kind = VTX_TYPE_Int;
                z->constval.as.int_val = 0;
                z->type = VTX_TYPE_Int;
                replace_node(nt, i, zero);
                simplified++;
            }
            break;

        /* Shl(x, 0) → x, Shr(x, 0) → x, Sar(x, 0) → x */
        case VTX_OP_Shl:
        case VTX_OP_Shr:
        case VTX_OP_Sar:
            if (rhs_is_const && rhs_const == 0) {
                replace_node(nt, i, lhs);
                simplified++;
            }
            break;

        /* Neg(Neg(x)) → x */
        case VTX_OP_Neg:
            if (lhs != VTX_NODEID_INVALID && lhs < nt->count) {
                vtx_node_t *lhs_node = &nt->nodes[lhs];
                if (!lhs_node->dead && lhs_node->opcode == VTX_OP_Neg) {
                    /* Neg(Neg(x)) → x */
                    vtx_nodeid_t inner = lhs_node->inputs[0];
                    replace_node(nt, i, inner);
                    simplified++;
                }
            }
            break;

        /* Not(Not(x)) → x */
        case VTX_OP_Not:
            if (lhs != VTX_NODEID_INVALID && lhs < nt->count) {
                vtx_node_t *lhs_node = &nt->nodes[lhs];
                if (!lhs_node->dead && lhs_node->opcode == VTX_OP_Not) {
                    vtx_nodeid_t inner = lhs_node->inputs[0];
                    replace_node(nt, i, inner);
                    simplified++;
                }
            }
            break;

        default:
            break;
        }
    }

    if (simplified > 0) {
        vtx_node_table_clear_dead(nt);
    }

    return simplified;
}
