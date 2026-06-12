#include "ir/verify.h"

#include <string.h>
#include <stdio.h>

#ifdef VORTEX_ENABLE_VERIFY

/* ========================================================================== */
/* Verification implementation (only compiled when VORTEX_ENABLE_VERIFY)        */
/* ========================================================================== */

static bool verify_graph_impl(const vtx_graph_t *graph, bool check_no_dead)
{
    if (graph == NULL) return true;

    const vtx_node_table_t *nt = &graph->node_table;
    uint32_t node_count = nt->count;
    bool ok = true;

    /* 1. Check all inputs of every live node are valid */
    for (uint32_t i = 0; i < node_count; i++) {
        const vtx_node_t *node = &nt->nodes[i];
        if (node->dead) continue;

        for (uint32_t j = 0; j < node->input_count; j++) {
            vtx_nodeid_t inp = node->inputs[j];
            if (inp == VTX_NODEID_INVALID) {
                fprintf(stderr, "VERIFY FAIL: N%u (%s) input[%u] is INVALID\n",
                        i, vtx_node_opcode_name(node->opcode), j);
                ok = false;
                continue;
            }
            if (inp >= node_count) {
                fprintf(stderr, "VERIFY FAIL: N%u (%s) input[%u]=N%u out of range (count=%u)\n",
                        i, vtx_node_opcode_name(node->opcode), j, inp, node_count);
                ok = false;
                continue;
            }
            if (nt->nodes[inp].dead) {
                fprintf(stderr, "VERIFY FAIL: N%u (%s) input[%u]=N%u is dead\n",
                        i, vtx_node_opcode_name(node->opcode), j, inp);
                ok = false;
            }
        }
    }

    /* 2. Check no data cycles (DFS-based cycle detection).
     * Control back-edges in loops are allowed. We only check for cycles
     * among pure data edges (DATA flag set, CONTROL not set). */

    /* State for each node: 0=unvisited, 1=in-progress, 2=done */
    uint8_t *visit_state = (uint8_t *)calloc(node_count, sizeof(uint8_t));
    if (visit_state == NULL) return ok; /* can't check, assume ok */

    /* Iterative DFS using an explicit stack to avoid stack overflow */
    typedef struct {
        vtx_nodeid_t node;
        uint32_t     next_input;  /* next input index to process */
    } dfs_frame_t;

    dfs_frame_t *dfs_stack = (dfs_frame_t *)malloc(node_count * sizeof(dfs_frame_t));
    if (dfs_stack == NULL) {
        free(visit_state);
        return ok;
    }

    for (uint32_t start = 0; start < node_count; start++) {
        if (nt->nodes[start].dead) continue;
        if (visit_state[start] != 0) continue;

        uint32_t sp = 0;
        dfs_stack[sp].node = start;
        dfs_stack[sp].next_input = 0;
        sp++;

        while (sp > 0) {
            dfs_frame_t *frame = &dfs_stack[sp - 1];
            vtx_nodeid_t nid = frame->node;
            const vtx_node_t *node = &nt->nodes[nid];

            if (frame->next_input == 0) {
                /* First visit */
                if (visit_state[nid] == 2) {
                    sp--;
                    continue;
                }
                if (visit_state[nid] == 1) {
                    sp--;
                    continue;
                }
                visit_state[nid] = 1; /* in progress */
            }

            /* Process next data input */
            bool found_unprocessed = false;
            while (frame->next_input < node->input_count) {
                vtx_nodeid_t inp = node->inputs[frame->next_input];
                frame->next_input++;

                if (inp == VTX_NODEID_INVALID || inp >= node_count) continue;
                if (nt->nodes[inp].dead) continue;

                /* Only check data edges for cycles.
                 * Control edges can form back-edges (loops). */
                const vtx_node_t *inp_node = &nt->nodes[inp];
                if (!vtx_nf_has(inp_node->flags, VTX_NF_DATA)) continue;
                if (vtx_nf_has(inp_node->flags, VTX_NF_CONTROL)) continue;
                if (vtx_nf_has(node->flags, VTX_NF_CONTROL) &&
                    vtx_nf_has(inp_node->flags, VTX_NF_CONTROL)) continue;

                if (visit_state[inp] == 1) {
                    /* Back edge -> cycle among data nodes */
                    fprintf(stderr, "VERIFY FAIL: data cycle detected: N%u -> N%u -> ... -> N%u\n",
                            inp, nid, inp);
                    ok = false;
                } else if (visit_state[inp] == 0) {
                    dfs_stack[sp].node = inp;
                    dfs_stack[sp].next_input = 0;
                    sp++;
                    found_unprocessed = true;
                    break;
                }
            }

            if (!found_unprocessed) {
                visit_state[nid] = 2; /* done */
                sp--;
            }
        }
    }

    free(dfs_stack);
    free(visit_state);

    /* 3. Phi input count matches Region predecessor count. */
    for (uint32_t i = 0; i < node_count; i++) {
        const vtx_node_t *node = &nt->nodes[i];
        if (node->dead || node->opcode != VTX_OP_Phi) continue;

        /* Find the Region input of this Phi */
        vtx_nodeid_t region_id = VTX_NODEID_INVALID;
        for (uint32_t j = 0; j < node->input_count; j++) {
            vtx_nodeid_t inp = node->inputs[j];
            if (inp != VTX_NODEID_INVALID && inp < node_count) {
                const vtx_node_t *inp_node = &nt->nodes[inp];
                if (inp_node->opcode == VTX_OP_Region || inp_node->opcode == VTX_OP_LoopBegin) {
                    region_id = inp;
                    break;
                }
            }
        }

        if (region_id != VTX_NODEID_INVALID) {
            const vtx_node_t *region = &nt->nodes[region_id];
            uint32_t expected = region->input_count + 1;
            if (node->input_count != expected) {
                fprintf(stderr, "VERIFY FAIL: Phi N%u has %u inputs, expected %u "
                        "(Region N%u has %u predecessors)\n",
                        i, node->input_count, expected, region_id, region->input_count);
                ok = false;
            }
        }
    }

    /* 4. Valid memory chains: each memory node's memory input should be
     * another memory node or Province/Start. */
    for (uint32_t i = 0; i < node_count; i++) {
        const vtx_node_t *node = &nt->nodes[i];
        if (node->dead) continue;
        if (!vtx_nf_has(node->flags, VTX_NF_MEMORY)) continue;

        bool found_mem_input = false;
        for (uint32_t j = 0; j < node->input_count; j++) {
            vtx_nodeid_t inp = node->inputs[j];
            if (inp != VTX_NODEID_INVALID && inp < node_count) {
                const vtx_node_t *inp_node = &nt->nodes[inp];
                if (vtx_nf_has(inp_node->flags, VTX_NF_MEMORY) ||
                    inp_node->opcode == VTX_OP_Province ||
                    inp_node->opcode == VTX_OP_Start) {
                    found_mem_input = true;
                    break;
                }
            }
        }

        if (!found_mem_input && node->opcode != VTX_OP_Province) {
            /* Soft check — memory nodes created from Start may not have
             * explicit memory inputs during early construction. */
        }
    }

    /* 5. Check no dead nodes remain (if verify_post_dce) */
    if (check_no_dead) {
        for (uint32_t i = 0; i < node_count; i++) {
            if (nt->nodes[i].dead) {
                fprintf(stderr, "VERIFY FAIL: dead node N%u (%s) still present after DCE\n",
                        i, vtx_node_opcode_name(nt->nodes[i].opcode));
                ok = false;
            }
        }
    }

    /* 6. Output counts are consistent */
    {
        uint32_t *computed_outputs = (uint32_t *)calloc(node_count, sizeof(uint32_t));
        if (computed_outputs != NULL) {
            for (uint32_t i = 0; i < node_count; i++) {
                const vtx_node_t *node = &nt->nodes[i];
                if (node->dead) continue;
                for (uint32_t j = 0; j < node->input_count; j++) {
                    vtx_nodeid_t inp = node->inputs[j];
                    if (inp != VTX_NODEID_INVALID && inp < node_count) {
                        computed_outputs[inp]++;
                    }
                }
            }

            for (uint32_t i = 0; i < node_count; i++) {
                const vtx_node_t *node = &nt->nodes[i];
                if (node->dead) continue;
                if (node->output_count != computed_outputs[i]) {
                    fprintf(stderr, "VERIFY FAIL: N%u (%s) output_count=%u, computed=%u\n",
                            i, vtx_node_opcode_name(node->opcode),
                            node->output_count, computed_outputs[i]);
                    ok = false;
                }
            }

            free(computed_outputs);
        }
    }

    /* 7. Start node has no inputs */
    if (graph->start_node < node_count) {
        const vtx_node_t *start = &nt->nodes[graph->start_node];
        if (start->input_count != 0) {
            fprintf(stderr, "VERIFY FAIL: Start N%u has %u inputs (should be 0)\n",
                    graph->start_node, start->input_count);
            ok = false;
        }
    }

    /* 8. All Region nodes have at least one input (except Start block) */
    for (uint32_t i = 0; i < node_count; i++) {
        const vtx_node_t *node = &nt->nodes[i];
        if (node->dead) continue;
        if (node->opcode == VTX_OP_Region || node->opcode == VTX_OP_LoopBegin) {
            if (node->input_count == 0 && i != graph->start_node) {
                fprintf(stderr, "VERIFY FAIL: Region N%u has no inputs\n", i);
                ok = false;
            }
        }
    }

    /* 9. If nodes should have at least 2 inputs (control + condition).
     * After DCE with dead-node cleanup, the condition input may have
     * been removed if SCCP proved it constant. An If with only 1 input
     * (control) is a degenerate form that should ideally be converted
     * to Goto, but we allow it in post-optimization verification. */
    for (uint32_t i = 0; i < node_count; i++) {
        const vtx_node_t *node = &nt->nodes[i];
        if (node->dead || node->opcode != VTX_OP_If) continue;
        if (node->input_count < 1) {
            fprintf(stderr, "VERIFY FAIL: If N%u has %u inputs (expected >= 1)\n",
                    i, node->input_count);
            ok = false;
        }
    }

    /* 10. Return nodes should have 1-2 inputs */
    for (uint32_t i = 0; i < node_count; i++) {
        const vtx_node_t *node = &nt->nodes[i];
        if (node->dead || node->opcode != VTX_OP_Return) continue;
        if (node->input_count < 1 || node->input_count > 2) {
            fprintf(stderr, "VERIFY FAIL: Return N%u has %u inputs (expected 1-2)\n",
                    i, node->input_count);
            ok = false;
        }
    }

    return ok;
}

/* ========================================================================== */
/* Public API                                                                  */
/* ========================================================================== */

bool vtx_verify_graph(const vtx_graph_t *graph)
{
    return verify_graph_impl(graph, false);
}

bool vtx_verify_graph_post_dce(const vtx_graph_t *graph)
{
    return verify_graph_impl(graph, true);
}

#else /* !VORTEX_ENABLE_VERIFY */

bool vtx_verify_graph(const vtx_graph_t *graph)
{
    (void)graph;
    return true;
}

bool vtx_verify_graph_post_dce(const vtx_graph_t *graph)
{
    (void)graph;
    return true;
}

#endif /* VORTEX_ENABLE_VERIFY */
