/* ============================================================================ *
 * AI-GENERATED CODE
 *
 * Representation inference — explicit UnboxInt/BoxInt insertion.
 * See rep_infer.h for algorithm description.
 * ============================================================================ */

#include "ir/rep_infer.h"
#include "ir/node.h"
#include <stdlib.h>
#include <string.h>

/* Check if an opcode is integer arithmetic that benefits from RAW_INT.
 * Note: Div and Mod are excluded because IDIV uses fixed RAX/RDX
 * registers which conflict with the RAW_INT path's register handling.
 * The isel's Div/Mod paths already handle untagging internally. */
static bool is_arith_opcode(vtx_node_opcode_t op) {
    return op == VTX_OP_Add || op == VTX_OP_Sub ||
           op == VTX_OP_Mul || op == VTX_OP_Neg;
}

/* Cmp is NOT marked as RAW_INT because its output is a boolean
 * (flag condition), not a raw integer. The Cmp's INPUTS benefit
 * from being raw (so the comparison is on raw values), but the
 * Cmp itself produces a tagged result for the If node. We handle
 * Cmp inputs specially: if both inputs are raw, the Cmp isel
 * already skips untagging (see the Cmp+If fusion path). */

/* Check if a node is a "tagged" value producer (not already raw).
 * Constants and Parameters are tagged SMIs. Phis are tagged unless
 * already marked RAW_INT. */
static bool is_tagged_producer(const vtx_node_t *node) {
    if (!node || node->dead) return false;
    if (vtx_nf_has(node->flags, VTX_NF_RAW_INT)) return false;
    if (node->opcode == VTX_OP_Constant ||
        node->opcode == VTX_OP_Parameter ||
        node->opcode == VTX_OP_Phi ||
        node->opcode == VTX_OP_Load ||
        node->opcode == VTX_OP_LoadField ||
        node->opcode == VTX_OP_LoadIndexed ||
        node->opcode == VTX_OP_BoxInt) {
        return true;
    }
    /* Arithmetic nodes that are NOT yet RAW_INT produce tagged output */
    if (is_arith_opcode(node->opcode)) {
        return !vtx_nf_has(node->flags, VTX_NF_RAW_INT);
    }
    return false;
}

/* Check if a consumer needs a tagged (boxed) value.
 * Non-arithmetic consumers (Return, Store, Call, If, Proj) need tagged. */
static bool consumer_needs_tagged(vtx_node_opcode_t op) {
    return op == VTX_OP_Return ||
           op == VTX_OP_Store ||
           op == VTX_OP_StoreField ||
           op == VTX_OP_StoreIndexed ||
           op == VTX_OP_CallStatic ||
           op == VTX_OP_CallVirtual ||
           op == VTX_OP_CallInterface ||
           op == VTX_OP_CallRuntime ||
           op == VTX_OP_If ||
           op == VTX_OP_Phi ||
           op == VTX_OP_CheckCast ||
           op == VTX_OP_InstanceOf ||
           op == VTX_OP_Guard ||
           op == VTX_OP_DeoptGuard ||
           op == VTX_OP_LoadIndexed ||
           op == VTX_OP_NewArray ||
           op == VTX_OP_Div ||   /* Div/Mod not in is_arith_opcode, need tagged */
           op == VTX_OP_Mod;
}

/* Check if a consumer can accept RAW_INT input (arithmetic or comparison). */
static bool consumer_accepts_raw(vtx_node_opcode_t op) {
    return is_arith_opcode(op) || op == VTX_OP_Cmp ||
           op == VTX_OP_BoxInt;
}

uint32_t vtx_rep_infer_run(vtx_graph_t *graph, vtx_arena_t *arena)
{
    (void)arena;
    if (!graph) return 0;

    vtx_node_table_t *nt = &graph->node_table;
    uint32_t inserted = 0;

    /* Phase 1: For each arithmetic node with tagged inputs, insert UnboxInt
     * before each tagged input. This converts:
     *   Add(tagged_a, tagged_b)
     * into:
     *   raw_a = UnboxInt(tagged_a)
     *   raw_b = UnboxInt(tagged_b)
     *   Add(raw_a, raw_b)  [marked RAW_INT]
     *
     * We only do this if ALL data inputs are tagged (to avoid mixing
     * representations within a single operation). */

    for (uint32_t i = 0; i < nt->count; i++) {
        vtx_node_t *node = &nt->nodes[i];
        if (node->dead) continue;
        if (vtx_nf_has(node->flags, VTX_NF_RAW_INT)) continue; /* already raw */
        if (!is_arith_opcode(node->opcode)) continue;

        /* Check if all data inputs are tagged SMIs */
        bool all_tagged = true;
        bool has_data_input = false;
        for (uint32_t j = 0; j < node->input_count; j++) {
            vtx_nodeid_t inp = node->inputs[j];
            if (inp == VTX_NODEID_INVALID || inp >= nt->count) continue;
            vtx_node_t *inp_node = &nt->nodes[inp];
            if (!vtx_nf_has(inp_node->flags, VTX_NF_DATA)) continue; /* skip control/memory */
            has_data_input = true;
            if (!is_tagged_producer(inp_node)) {
                all_tagged = false;
                break;
            }
        }
        if (!has_data_input || !all_tagged) continue;

        /* All data inputs are tagged — insert UnboxInt for each and
         * redirect the arithmetic node's inputs to the UnboxInt outputs.
         * Don't mark the arithmetic node as RAW_INT — the isel already
         * detects RAW_INT inputs (via vtx_nf_has on the input node) and
         * skips untagging. The output stays tagged unless explicitly
         * boxed. We mark the node as RAW_INT ONLY if ALL inputs are
         * now raw (either from UnboxInt or from previous raw arith). */
        bool all_inputs_raw = true;
        for (uint32_t j = 0; j < node->input_count; j++) {
            vtx_nodeid_t inp = node->inputs[j];
            if (inp == VTX_NODEID_INVALID || inp >= nt->count) continue;
            vtx_node_t *inp_node = &nt->nodes[inp];
            if (!vtx_nf_has(inp_node->flags, VTX_NF_DATA)) continue;

            /* Skip if input is already raw (from a previous arith node) */
            if (vtx_nf_has(inp_node->flags, VTX_NF_RAW_INT)) continue;

            /* Skip if input is a Constant — isel handles raw constants */
            if (inp_node->opcode == VTX_OP_Constant) {
                /* Constants aren't raw, but isel can use them directly */
                continue;
            }

            /* Create UnboxInt node */
            vtx_nodeid_t unbox_id = vtx_node_create(nt, VTX_OP_UnboxInt);
            if (unbox_id == VTX_NODEID_INVALID) { all_inputs_raw = false; continue; }
            vtx_node_t *unbox = vtx_node_get(nt, unbox_id);
            if (!unbox) { all_inputs_raw = false; continue; }

            unbox->type = VTX_TYPE_Int;
            unbox->bytecode_pc = node->bytecode_pc;
            vtx_node_add_input(nt, unbox_id, inp);

            /* Redirect the arithmetic node's input to the UnboxInt output */
            vtx_node_replace_input(nt, i, j, unbox_id);
            inserted++;
        }

        /* Mark the arithmetic node as RAW_INT so the isel knows its
         * output is raw and skips retagging. The isel checks this flag
         * to decide whether to emit the retag sequence. */
        if (all_inputs_raw) {
            node->flags = vtx_nf_union(node->flags, VTX_NF_RAW_INT);
        }
    }

    /* Phase 2: For each RAW_INT node, find consumers that need tagged
     * values and insert BoxInt between the raw producer and the tagged
     * consumer. This ensures that Return, Store, If, etc. receive
     * properly tagged SMIs. */

    for (uint32_t i = 0; i < nt->count; i++) {
        vtx_node_t *node = &nt->nodes[i];
        if (node->dead) continue;
        if (!vtx_nf_has(node->flags, VTX_NF_RAW_INT)) continue;
        if (node->opcode == VTX_OP_BoxInt || node->opcode == VTX_OP_UnboxInt) continue;

        /* This node produces raw int64. Check all its users. */
        /* We iterate the use list. Since we may modify use entries during
         * iteration (by inserting BoxInt), we collect the list first. */
        uint32_t use_count = node->use_count;
        if (use_count == 0) continue;

        /* Collect (user_id, input_index) pairs that need BoxInt */
        typedef struct { vtx_nodeid_t user; uint32_t input_idx; } needs_box_t;
        needs_box_t *needs = (needs_box_t *)malloc(use_count * sizeof(needs_box_t));
        if (!needs) continue;
        uint32_t needs_count = 0;

        for (uint32_t u = 0; u < use_count; u++) {
            vtx_use_entry_t *use = &node->uses[u];
            if (use->user_id >= nt->count) continue;
            vtx_node_t *user = &nt->nodes[use->user_id];
            if (user->dead) continue;

            /* If the consumer needs tagged and doesn't accept raw */
            if (consumer_needs_tagged(user->opcode) &&
                !consumer_accepts_raw(user->opcode)) {
                needs[needs_count].user = use->user_id;
                needs[needs_count].input_idx = use->input_index;
                needs_count++;
            }
        }

        /* Insert BoxInt for each consumer that needs tagged */
        for (uint32_t n = 0; n < needs_count; n++) {
            vtx_nodeid_t box_id = vtx_node_create(nt, VTX_OP_BoxInt);
            if (box_id == VTX_NODEID_INVALID) continue;
            vtx_node_t *box = vtx_node_get(nt, box_id);
            if (!box) continue;

            box->type = VTX_TYPE_Int;
            box->bytecode_pc = node->bytecode_pc;
            vtx_node_add_input(nt, box_id, (vtx_nodeid_t)i);

            /* Redirect the consumer's input to the BoxInt output */
            vtx_node_replace_input(nt, needs[n].user, needs[n].input_idx, box_id);
            inserted++;
        }

        free(needs);
    }

    /* Phase 3: GVN-like simplification — eliminate UnboxInt(BoxInt(x)) → x
     * and BoxInt(UnboxInt(x)) → x when the roundtrip is identity.
     * This handles cases where the inference pass inserted redundant
     * conversions. A full GVN pass would also catch these, but doing it
     * here reduces IR bloat before scheduling. */
    for (uint32_t i = 0; i < nt->count; i++) {
        vtx_node_t *node = &nt->nodes[i];
        if (node->dead) continue;

        /* UnboxInt(BoxInt(x)) → x */
        if (node->opcode == VTX_OP_UnboxInt && node->input_count >= 1) {
            vtx_nodeid_t inp = node->inputs[0];
            if (inp < nt->count) {
                vtx_node_t *inp_node = &nt->nodes[inp];
                if (inp_node->opcode == VTX_OP_BoxInt && inp_node->input_count >= 1) {
                    vtx_nodeid_t inner = inp_node->inputs[0];
                    vtx_node_replace_all_uses(nt, i, inner);
                    node->dead = true;
                }
            }
        }

        /* BoxInt(UnboxInt(x)) → x */
        if (node->opcode == VTX_OP_BoxInt && node->input_count >= 1) {
            vtx_nodeid_t inp = node->inputs[0];
            if (inp < nt->count) {
                vtx_node_t *inp_node = &nt->nodes[inp];
                if (inp_node->opcode == VTX_OP_UnboxInt && inp_node->input_count >= 1) {
                    vtx_nodeid_t inner = inp_node->inputs[0];
                    vtx_node_replace_all_uses(nt, i, inner);
                    node->dead = true;
                }
            }
        }
    }

    return inserted;
}
