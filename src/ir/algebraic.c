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

    /* IR-008 fix: iterate to a fixed point. Cascades like
     * Sub(Add(x,0),0) where the Add is at a HIGHER index than the
     * Sub miss simplification in a single forward pass. Loop until
     * no further simplifications occur (cap at 8 iterations to bound
     * compile time on pathological graphs). */
    uint32_t pass = 0;
    while (pass < 8) {
        uint32_t pass_simplified = 0;
        /* Reverse-ID order: consumers before producers, so cascades
         * that flow backward in ID order still get caught in one pass. */
        for (uint32_t ii = nt->count; ii-- > 0; ) {
            vtx_node_t *node = &nt->nodes[ii];
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
                replace_node(nt, ii, lhs);
                pass_simplified++;
            } else if (lhs_is_const && lhs_const == 0) {
                replace_node(nt, ii, rhs);
                pass_simplified++;
            }
            break;

        /* Sub(x, 0) → x, Sub(x, x) → 0 */
        case VTX_OP_Sub:
            if (rhs_is_const && rhs_const == 0) {
                replace_node(nt, ii, lhs);
                pass_simplified++;
            } else if (lhs == rhs) {
                /* Sub(x, x) → 0 — create a constant 0 */
                /* IR-006 fix: check allocation failure before deref. */
                vtx_nodeid_t zero = vtx_node_create(nt, VTX_OP_Constant);
                if (zero == VTX_NODEID_INVALID) break;
                vtx_node_t *z = vtx_node_get(nt, zero);
                if (z == NULL) break;
                z->constval.kind = VTX_TYPE_Int;
                z->constval.as.int_val = 0;
                z->type = VTX_TYPE_Int;
                replace_node(nt, ii, zero);
                pass_simplified++;
            }
            break;

        /* Mul(x, 0) → 0, Mul(x, 1) → x, Mul(1, x) → x, Mul(x, -1) → Neg(x) */
        case VTX_OP_Mul:
            if (rhs_is_const && rhs_const == 0) {
                replace_node(nt, ii, rhs);  /* replace with the 0 constant */
                pass_simplified++;
            } else if (rhs_is_const && rhs_const == 1) {
                replace_node(nt, ii, lhs);
                pass_simplified++;
            } else if (lhs_is_const && lhs_const == 1) {
                replace_node(nt, ii, rhs);
                pass_simplified++;
            } else if (rhs_is_const && rhs_const == -1) {
                /* Mul(x, -1) → Neg(x).
                 * IR-007 fix: properly disconnect the rhs Constant via
                 * vtx_node_remove_input so its use-def list and
                 * output_count are updated. The old raw mutation left
                 * the Constant appearing to have a user (this node),
                 * so DCE never collected it. */
                vtx_node_remove_input(nt, (vtx_nodeid_t)ii, 1);
                node->opcode = VTX_OP_Neg;
                pass_simplified++;
            }
            break;

        /* Div(x, 1) → x */
        case VTX_OP_Div:
            if (rhs_is_const && rhs_const == 1) {
                replace_node(nt, ii, lhs);
                pass_simplified++;
            }
            break;

        /* And(x, 0) → 0, And(x, -1) → x */
        case VTX_OP_And:
            if (rhs_is_const && rhs_const == 0) {
                replace_node(nt, ii, rhs);
                pass_simplified++;
            } else if (rhs_is_const && rhs_const == -1) {
                replace_node(nt, ii, lhs);
                pass_simplified++;
            } else if (lhs_is_const && lhs_const == -1) {
                replace_node(nt, ii, rhs);
                pass_simplified++;
            }
            break;

        /* Or(x, 0) → x, Or(x, -1) → -1 */
        case VTX_OP_Or:
            if (rhs_is_const && rhs_const == 0) {
                replace_node(nt, ii, lhs);
                pass_simplified++;
            } else if (rhs_is_const && rhs_const == -1) {
                replace_node(nt, ii, rhs);
                pass_simplified++;
            } else if (lhs_is_const && lhs_const == 0) {
                replace_node(nt, ii, rhs);
                pass_simplified++;
            }
            break;

        /* Xor(x, 0) → x, Xor(x, x) → 0 */
        case VTX_OP_Xor:
            if (rhs_is_const && rhs_const == 0) {
                replace_node(nt, ii, lhs);
                pass_simplified++;
            } else if (lhs == rhs) {
                /* IR-006 fix: check allocation failure before deref. */
                vtx_nodeid_t zero = vtx_node_create(nt, VTX_OP_Constant);
                if (zero == VTX_NODEID_INVALID) break;
                vtx_node_t *z = vtx_node_get(nt, zero);
                if (z == NULL) break;
                z->constval.kind = VTX_TYPE_Int;
                z->constval.as.int_val = 0;
                z->type = VTX_TYPE_Int;
                replace_node(nt, ii, zero);
                pass_simplified++;
            }
            break;

        /* Shl(x, 0) → x, Shr(x, 0) → x, Sar(x, 0) → x */
        case VTX_OP_Shl:
        case VTX_OP_Shr:
        case VTX_OP_Sar:
            if (rhs_is_const && rhs_const == 0) {
                replace_node(nt, ii, lhs);
                pass_simplified++;
            }
            break;

        /* Neg(Neg(x)) → x */
        case VTX_OP_Neg:
            if (lhs != VTX_NODEID_INVALID && lhs < nt->count) {
                vtx_node_t *lhs_node = &nt->nodes[lhs];
                if (!lhs_node->dead && lhs_node->opcode == VTX_OP_Neg) {
                    /* Neg(Neg(x)) → x */
                    vtx_nodeid_t inner = lhs_node->inputs[0];
                    replace_node(nt, ii, inner);
                    pass_simplified++;
                }
            }
            break;

        /* Not(Not(x)) → x */
        case VTX_OP_Not:
            if (lhs != VTX_NODEID_INVALID && lhs < nt->count) {
                vtx_node_t *lhs_node = &nt->nodes[lhs];
                if (!lhs_node->dead && lhs_node->opcode == VTX_OP_Not) {
                    vtx_nodeid_t inner = lhs_node->inputs[0];
                    replace_node(nt, ii, inner);
                    pass_simplified++;
                }
            }
            break;

        /* Cmp(Constant, Constant) → Constant (0 or 1)
         *
         * A4 — Common operator reducer. V8's common-operator-reducer.cc
         * does this. When both operands of a comparison are compile-time
         * constants, fold the comparison to its result. This exposes
         * downstream opportunities (e.g., constant-If folding in
         * cfg_simplify.c). */
        case VTX_OP_Cmp:
        case VTX_OP_CmpP: {
            if (lhs_is_const && rhs_is_const) {
                bool result = false;
                int64_t a = lhs_const, b = rhs_const;
                switch (node->cond) {
                case VTX_COND_EQ:  result = (a == b); break;
                case VTX_COND_NE:  result = (a != b); break;
                case VTX_COND_LT:  result = (a < b);  break;
                case VTX_COND_LE:  result = (a <= b); break;
                case VTX_COND_GT:  result = (a > b);  break;
                case VTX_COND_GE:  result = (a >= b); break;
                default: break;
                }
                /* Replace this Cmp with a Constant(result) */
                vtx_nodeid_t c_id = vtx_node_create(nt, VTX_OP_Constant);
                if (c_id != VTX_NODEID_INVALID) {
                    vtx_node_t *c = vtx_node_get(nt, c_id);
                    if (c) {
                        c->constval.kind = VTX_TYPE_Int;
                        c->constval.as.int_val = result ? 1 : 0;
                        c->type = VTX_TYPE_Int;
                        replace_node(nt, ii, c_id);
                        pass_simplified++;
                    }
                }
            }
            /* Cmp(x, x) → Constant(1) for EQ/GE/LE/UGE/ULE,
             * Constant(0) for NE/GT/LT/UGT/ULT.
             * Only valid when x is not a float (NaN != NaN).
             *
             * BUGFIX (audit High #16): The old code fell through `default`
             * for unsigned ULE/UGE, folding them to 0 (should be 1). */
            if (node->type != VTX_TYPE_Float && lhs == rhs &&
                lhs != VTX_NODEID_INVALID) {
                bool result = false;
                switch (node->cond) {
                case VTX_COND_EQ:  result = true;  break;
                case VTX_COND_NE:  result = false; break;
                case VTX_COND_LE:  result = true;  break;
                case VTX_COND_GE:  result = true;  break;
                case VTX_COND_LT:  result = false; break;
                case VTX_COND_GT:  result = false; break;
                case VTX_COND_ULE: result = true;  break;
                case VTX_COND_UGE: result = true;  break;
                case VTX_COND_ULT: result = false; break;
                case VTX_COND_UGT: result = false; break;
                default: break;
                }
                vtx_nodeid_t c_id = vtx_node_create(nt, VTX_OP_Constant);
                if (c_id != VTX_NODEID_INVALID) {
                    vtx_node_t *c = vtx_node_get(nt, c_id);
                    if (c) {
                        c->constval.kind = VTX_TYPE_Int;
                        c->constval.as.int_val = result ? 1 : 0;
                        c->type = VTX_TYPE_Int;
                        replace_node(nt, ii, c_id);
                        pass_simplified++;
                    }
                }
            }
            break;
        }

        default:
            break;
        }
        }

        if (pass_simplified == 0) break;
        simplified += pass_simplified;
        pass++;
    }

    if (simplified > 0) {
        vtx_node_table_clear_dead(nt);
    }

    return simplified;
}
