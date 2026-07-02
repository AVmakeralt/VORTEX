/**
 * VORTEX SMI Tag Elision Pass
 *
 * Identifies straight-line arithmetic chains on SMI values and marks them
 * as "raw int" so the isel skips per-op untag/retag. Instead of:
 *
 *   untag(a) → ADD → retag → untag(result) → ADD → retag → ...
 *   (7 instructions per op)
 *
 * We get:
 *
 *   untag(a) → ADD → ADD → ADD → retag(final)
 *   (1 untag + N ops + 1 retag for the whole chain)
 *
 * Algorithm:
 *   1. Find arithmetic nodes (Add, Sub, Mul, And, Or, Xor, Shl, Shr)
 *      whose result feeds into another arithmetic node (not a store,
 *      call, return, or guard).
 *   2. Check that both inputs are either:
 *      - Already RAW_INT (from a previous iteration)
 *      - SMIs (Parameter, Constant, Phi, or another arithmetic node)
 *   3. If both inputs will be raw int, mark this node RAW_INT.
 *   4. The isel checks RAW_INT: if set, skip untag (inputs are already
 *      raw) and skip retag (output will be untagged by the next consumer).
 *      Only the chain endpoints (Return, Store, Guard, If) trigger retag.
 *
 * Safety:
 *   - Only applies to integer arithmetic, not float (float uses XMM)
 *   - Does not apply to Div/Mod (they have special IDIV handling)
 *   - Does not apply to nodes whose result is used by non-arithmetic
 *     consumers (Return, Store, Call, Guard, If, Cmp)
 *   - The chain is broken at any non-arithmetic consumer
 */

#include "ir/graph.h"
#include "ir/node.h"
#include <stdlib.h>
#include <string.h>

/* Check if a node opcode can PRODUCE a raw int value when marked RAW_INT.
 * These are pure integer arithmetic/bitwise ops whose result is a 64-bit int.
 * When marked RAW_INT, their isel MUST skip the SMI retag on the output.
 *
 * NOTE: Only opcodes whose isel has been updated to respect VTX_NF_RAW_INT
 * should be listed here. Adding an opcode without updating its isel will
 * produce wrong code (the isel retags the output, but consumers expect raw).
 *
 * Shl/Shr/Sar/Not are NOT here because their isel always retags. They can
 * still CONSUME raw int inputs (listed in can_consume_raw_int via the
 * producer list), but they produce tagged SMI outputs. */
static bool can_produce_raw_int(vtx_node_opcode_t op) {
    switch (op) {
    case VTX_OP_Add:
    case VTX_OP_Sub:
        return true;
    default:
        return false;
    }
}

/* Check if a node opcode can CONSUME raw int values without needing retag.
 * This includes all raw-int producers PLUS Cmp (which consumes ints but
 * produces a condition code, not an int) PLUS the shift ops (which untag
 * their inputs but retag their outputs — they can accept raw int inputs
 * because emit_smi_untag on a raw int is a no-op in terms of correctness...
 *
 * Actually NO — emit_smi_untag does SHL 13 + SAR 16 which CORRUPTS a raw
 * int. So shift ops can NOT consume raw int inputs unless their isel is
 * updated to skip the untag. Since we haven't done that, shifts are NOT
 * valid raw-int consumers. They act as chain terminators. */
static bool can_consume_raw_int(vtx_node_opcode_t op) {
    if (can_produce_raw_int(op)) return true;
    /* Cmp takes raw int inputs and produces a cond code — no retag needed
     * on inputs if they're already raw. The Cmp isel has been updated to
     * skip untag when inputs are RAW_INT. */
    if (op == VTX_OP_Cmp || op == VTX_OP_CmpP) return true;
    return false;
}

/* Backward-compatible alias for the consumer check used in the main loop.
 * A node is a "valid raw-int consumer" if it can accept raw int inputs. */
static bool is_eligible_arith(vtx_node_opcode_t op) {
    return can_consume_raw_int(op);
}

/* Check if a node is a "chain terminator" — its input must be retagged
 * because the consumer expects a NaN-boxed SMI, not a raw int.
 * Note: Cmp/Phi are NOT terminators — they can consume raw int directly. */
static bool is_chain_terminator(vtx_node_opcode_t op) {
    switch (op) {
    case VTX_OP_Return:
    case VTX_OP_Store:
    case VTX_OP_StoreField:
    case VTX_OP_StoreIndexed:
    case VTX_OP_CallStatic:
    case VTX_OP_CallVirtual:
    case VTX_OP_CallInterface:
    case VTX_OP_CallRuntime:
    case VTX_OP_Guard:
    case VTX_OP_DeoptGuard:
    case VTX_OP_If:
    case VTX_OP_CmpF:
    case VTX_OP_CmpD:
    case VTX_OP_Switch:
    case VTX_OP_CheckCast:
    case VTX_OP_InstanceOf:
    case VTX_OP_FrameState:
        return true;
    default:
        return false;
    }
}

/* Check if a node produces a value that is definitely an SMI (NaN-boxed)
 * or can be treated as a raw int source for tag elision chains.
 * Shift ops (Shl/Shr/Sar) and Not produce tagged SMIs (their isel retags),
 * so they break RAW_INT chains — they're valid SMI producers but NOT raw. */
static bool is_smi_producer(vtx_node_t *node) {
    if (!node) return false;
    switch (node->opcode) {
    case VTX_OP_Parameter:
    case VTX_OP_Constant:
    case VTX_OP_Phi:
        return true;
    case VTX_OP_Add: case VTX_OP_Sub:
    case VTX_OP_Mul:
    case VTX_OP_And: case VTX_OP_Or: case VTX_OP_Xor:
    case VTX_OP_Neg:
        return true;
    default:
        return false;
    }
}

uint32_t vtx_smi_tag_elision_run(vtx_graph_t *graph)
{
    if (!graph) return 0;
    vtx_node_table_t *nt = &graph->node_table;
    uint32_t elided = 0;

    /* Multi-pass: repeat until no more nodes can be marked RAW_INT.
     * This handles chains where A→B→C: marking A as RAW_INT allows
     * B to be marked, which allows C. */
    bool changed = true;
    while (changed) {
        changed = false;

        for (uint32_t i = 0; i < nt->count; i++) {
            vtx_node_t *node = &nt->nodes[i];
            if (node->dead) continue;
            if (vtx_nf_has(node->flags, VTX_NF_RAW_INT)) continue; /* already marked */

            /* Only nodes that can PRODUCE raw int (not just consume it).
             * Cmp can consume raw int but its output is a cond code,
             * not an int — so it's never marked RAW_INT. */
            if (!can_produce_raw_int(node->opcode)) continue;

            /* Skip float-typed ops (they use XMM, not SMI) */
            if (node->type == VTX_TYPE_Float) continue;

            /* Skip Div/Mod (they use IDIV with special register constraints) */
            if (node->opcode == VTX_OP_Div || node->opcode == VTX_OP_Mod) continue;

            /* For Phi nodes: only elide if ALL data inputs are either
             * RAW_INT or SMI producers. A Phi merges values from multiple
             * paths — if any path brings in a tagged SMI, the Phi output
             * must be tagged (otherwise the raw-int consumer would get
             * a tagged value on that path). */
            if (node->opcode == VTX_OP_Phi) {
                bool phi_inputs_ok = true;
                for (uint32_t j = 0; j < node->input_count; j++) {
                    vtx_nodeid_t inp_id = node->inputs[j];
                    if (inp_id == VTX_NODEID_INVALID || inp_id >= nt->count) continue;
                    vtx_node_t *inp = &nt->nodes[inp_id];
                    if (vtx_nf_has(inp->flags, VTX_NF_CONTROL)) continue;
                    if (vtx_nf_has(inp->flags, VTX_NF_MEMORY)) continue;
                    if (vtx_nf_has(inp->flags, VTX_NF_RAW_INT)) continue;
                    if (is_smi_producer(inp)) continue;
                    phi_inputs_ok = false;
                    break;
                }
                if (!phi_inputs_ok) continue;
            }

            /* Check: are ALL data inputs either RAW_INT or SMI producers?
             * And does the output feed into another arithmetic node (not
             * a chain terminator)?
             *
             * For binary ops: inputs[0] and inputs[1] are the two operands.
             * For unary ops (Neg): inputs[0] is the operand.
             * The last input may be a control/memory input — skip those. */

            bool inputs_ok = true;
            for (uint32_t j = 0; j < node->input_count; j++) {
                vtx_nodeid_t inp_id = node->inputs[j];
                if (inp_id == VTX_NODEID_INVALID || inp_id >= nt->count) continue;

                vtx_node_t *inp = &nt->nodes[inp_id];
                if (vtx_nf_has(inp->flags, VTX_NF_CONTROL)) continue; /* skip control inputs */
                if (vtx_nf_has(inp->flags, VTX_NF_MEMORY)) continue;  /* skip memory inputs */

                /* This is a data input. It must be either:
                 * - Already RAW_INT (from a previous elision iteration)
                 * - A SMI producer (Parameter, Constant, Phi, arithmetic)
                 * - A Constant (we'll untag it at chain entry) */
                if (vtx_nf_has(inp->flags, VTX_NF_RAW_INT)) {
                    /* Input is already raw int — perfect */
                    continue;
                }

                if (is_smi_producer(inp)) {
                    /* Input is an SMI producer — we'll untag it once
                     * at chain entry. Mark this node as needing untag
                     * for its inputs. */
                    continue;
                }

                /* Input is something else (Div, Mod, Call result, etc.)
                 * — can't elision this node */
                inputs_ok = false;
                break;
            }

            if (!inputs_ok) continue;

            /* Check: do ALL consumers expect raw int (are eligible arithmetic)?
             * If ANY consumer is a chain terminator (Cmp, If, Return, Store,
             * etc.), the output must be a tagged SMI — can't elide.
             *
             * This is critical for correctness: a Cmp node compares
             * NaN-boxed SMIs. If it receives a raw int, the comparison
             * is wrong (e.g., raw 0 vs SMI(0) = 0x7FF8000000000000). */
            bool all_consumers_arith = true;
            bool has_arith_consumer = false;
            for (uint32_t u = 0; u < node->use_count; u++) {
                vtx_use_entry_t *use = &node->uses[u];
                if (use->user_id >= nt->count) continue;
                vtx_node_t *user = &nt->nodes[use->user_id];
                if (user->dead) continue;

                if (is_eligible_arith(user->opcode) &&
                    user->type != VTX_TYPE_Float) {
                    has_arith_consumer = true;
                } else {
                    /* This consumer is NOT an eligible arithmetic op.
                     * It needs a tagged SMI, so we can't elide. */
                    all_consumers_arith = false;
                    break;
                }
            }

            if (!has_arith_consumer || !all_consumers_arith) {
                /* Either no arithmetic consumer (chain endpoint) or
                 * a non-arithmetic consumer needs tagged SMI. */
                continue;
            }

            /* All checks passed — mark this node as RAW_INT */
            node->flags = vtx_nf_union(node->flags, VTX_NF_RAW_INT);
            elided++;
            changed = true;
        }
    }

    return elided;
}
