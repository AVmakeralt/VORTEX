#include "ir/schedule.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ========================================================================== */
/* Dominance computation (simple iterative algorithm)                          */
/* ========================================================================== */

/**
 * Compute dominators using the iterative algorithm.
 * dom[i] = intersection of dom[p] for all predecessors p of i.
 * dom[start] = start.
 *
 * IR-3 fix: The Cooper-Harvey-Kennedy intersection algorithm requires
 * reverse postorder (RPO) numbers for correct comparisons. The original
 * code compared block indices, which have no guaranteed relationship
 * to dominator-tree depth. We now compute RPO numbers first and use
 * them in the intersection walk.
 *
 * Returns a malloc'd array of uint32_t (dominator block indices).
 * The caller must free it.
 */
static uint32_t *compute_dominators(uint32_t block_count,
                                    uint32_t start_block,
                                    const vtx_schedule_block_t *blocks)
{
    uint32_t *dom = (uint32_t *)malloc(block_count * sizeof(uint32_t));
    if (dom == NULL) return NULL;

    /* Compute reverse postorder numbers via iterative DFS.
     * RPO numbers ensure that a > b in RPO means a is closer to the
     * root of the dominator tree, which is required by the intersection
     * algorithm's comparison-based walk. */
    uint32_t *rpo = (uint32_t *)malloc(block_count * sizeof(uint32_t));
    uint32_t *dfs_stack = (uint32_t *)malloc(block_count * sizeof(uint32_t));
    uint8_t *visited = (uint8_t *)calloc(block_count, 1);
    if (!rpo || !dfs_stack || !visited) {
        free(dom); free(rpo); free(dfs_stack); free(visited);
        return NULL;
    }

    /* Iterative DFS to compute postorder, then reverse for RPO */
    uint32_t postorder_count = 0;
    uint32_t *postorder = (uint32_t *)malloc(block_count * sizeof(uint32_t));
    if (!postorder) {
        free(dom); free(rpo); free(dfs_stack); free(visited);
        return NULL;
    }

    /* DFS from start_block */
    uint32_t sp = 0;
    dfs_stack[sp++] = start_block;
    visited[start_block] = 1;

    while (sp > 0) {
        uint32_t current = dfs_stack[sp - 1];
        bool all_children_visited = true;

        vtx_schedule_block_t *blk = (vtx_schedule_block_t *)&blocks[current];
        for (uint32_t s = 0; s < blk->succ_count; s++) {
            uint32_t succ = blk->succ_blocks[s];
            if (succ < block_count && !visited[succ]) {
                visited[succ] = 1;
                dfs_stack[sp++] = succ;
                all_children_visited = false;
                break;
            }
        }

        if (all_children_visited) {
            sp--;
            postorder[postorder_count++] = current;
        }
    }

    /* RPO: reverse the postorder */
    for (uint32_t i = 0; i < block_count; i++) rpo[i] = block_count; /* unreachable */
    for (uint32_t i = 0; i < postorder_count; i++) {
        uint32_t rpo_num = postorder_count - 1 - i;
        rpo[postorder[i]] = rpo_num;
    }
    /* Start block gets highest RPO number */
    rpo[start_block] = postorder_count > 0 ? postorder_count - 1 : 0;

    free(postorder);
    free(dfs_stack);
    free(visited);

    /* Initialize: all blocks dominated by start (sentinel), start by itself */
    for (uint32_t i = 0; i < block_count; i++) {
        dom[i] = (i == start_block) ? i : start_block;
    }

    /* Iterate using RPO order for faster convergence */
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t i = 0; i < block_count; i++) {
            if (i == start_block) continue;

            /* Intersect dominators of all predecessors */
            uint32_t new_dom = (uint32_t)-1;
            vtx_schedule_block_t *blk = (vtx_schedule_block_t *)&blocks[i];

            for (uint32_t p = 0; p < blk->pred_count; p++) {
                uint32_t pred = blk->pred_blocks[p];
                if (new_dom == (uint32_t)-1) {
                    new_dom = dom[pred];
                } else {
                    /* Walk up both chains to find common ancestor.
                     * IR-3 fix: Use RPO numbers for comparison instead of
                     * block indices. A higher RPO number means closer to
                     * the root of the dominator tree. */
                    uint32_t a = new_dom;
                    uint32_t b = dom[pred];
                    while (a != b) {
                        while (rpo[a] > rpo[b]) a = dom[a];
                        while (rpo[b] > rpo[a]) b = dom[b];
                    }
                    new_dom = a;
                }
            }

            if (new_dom == (uint32_t)-1) new_dom = start_block;

            if (dom[i] != new_dom) {
                dom[i] = new_dom;
                changed = true;
            }
        }
    }

    free(rpo);
    return dom;
}

/* ========================================================================== */
/* Dominance frontier computation (Cytron et al. 1991)                        */
/* ========================================================================== */

/**
 * Add a block index to a dominance frontier set, avoiding duplicates.
 * Returns 0 on success, -1 on allocation failure.
 */
static int df_add(vtx_schedule_block_t *blk, uint32_t block_idx)
{
    /* Check for duplicate */
    for (uint32_t i = 0; i < blk->df_count; i++) {
        if (blk->df_blocks[i] == block_idx) return 0;
    }

    /* Grow array if needed */
    if (blk->df_count >= blk->df_capacity) {
        uint32_t new_cap = (blk->df_capacity == 0) ? 8 : blk->df_capacity * 2;
        uint32_t *new_arr = (uint32_t *)realloc(blk->df_blocks,
                                                 new_cap * sizeof(uint32_t));
        if (new_arr == NULL) return -1;
        blk->df_blocks = new_arr;
        blk->df_capacity = new_cap;
    }

    blk->df_blocks[blk->df_count++] = block_idx;
    return 0;
}

/**
 * Compute dominance frontiers for each block.
 *
 * Algorithm (Cytron et al. 1991):
 *   For each join block b (pred_count >= 2):
 *     For each predecessor p of b:
 *       runner = p
 *       while runner != dom[b]:
 *         add b to DF(runner)
 *         runner = dom[runner]
 *
 * DF(b) is the set of blocks where b's dominance ends — blocks that are
 * successors of blocks dominated by b, but are not themselves strictly
 * dominated by b. This is the standard criterion for SSA Phi node
 * placement.
 */
void vtx_compute_dominance_frontiers(vtx_schedule_block_t *blocks,
                                       uint32_t count,
                                       const uint32_t *dom,
                                       uint32_t start_block)
{
    if (blocks == NULL || dom == NULL || count == 0) return;

    /* Initialize all frontier sets to empty */
    for (uint32_t i = 0; i < count; i++) {
        blocks[i].df_count = 0;
    }

    /* For each block b, walk up the dominator tree from each predecessor */
    for (uint32_t b = 0; b < count; b++) {
        /* Only join blocks (>= 2 predecessors) contribute to frontiers */
        if (blocks[b].pred_count < 2) continue;

        for (uint32_t pi = 0; pi < blocks[b].pred_count; pi++) {
            uint32_t runner = blocks[b].pred_blocks[pi];

            /* Walk up the dominator tree until we reach b's immediate
             * dominator. Every block along the way has b in its frontier. */
            while (runner != dom[b] && runner != start_block) {
                /* Guard: if runner is out of range, stop */
                if (runner >= count) break;

                df_add(&blocks[runner], b);

                runner = dom[runner];
            }
        }
    }
}

/* ========================================================================== */
/* Loop depth computation                                                      */
/* ========================================================================== */

/**
 * Compute loop depth for each block. A block is in a loop if it is
 * dominated by a loop header and the loop header has a back edge.
 *
 * We use the simple approach: for each back edge (latch → header),
 * all blocks on the path from header to latch are in the loop.
 */
static void compute_loop_depth(uint32_t block_count,
                               uint32_t start_block,
                               vtx_schedule_block_t *blocks,
                               const uint32_t *dom)
{
    /* Initialize all to 0 */
    for (uint32_t i = 0; i < block_count; i++) {
        blocks[i].loop_depth = 0;
    }

    /* For each loop header, increment depth of all blocks in the loop */
    for (uint32_t i = 0; i < block_count; i++) {
        if (!blocks[i].is_loop_header) continue;

        /* Find the loop latch: a predecessor of i that i dominates */
        for (uint32_t p = 0; p < blocks[i].pred_count; p++) {
            uint32_t pred = blocks[i].pred_blocks[p];
            /* Check if i dominates pred → this is a back edge */
            uint32_t d = dom[pred];
            bool is_back_edge = false;
            while (d != start_block && d != (uint32_t)-1) {
                if (d == i) { is_back_edge = true; break; }
                d = dom[d];
            }
            if (d == i) is_back_edge = true;

            if (is_back_edge) {
                /* All blocks from pred up to i (walking dom tree) are in the loop */
                uint32_t b = pred;
                while (b != i && b != (uint32_t)-1) {
                    blocks[b].loop_depth++;
                    b = dom[b];
                }
                blocks[i].loop_depth++;
            }
        }
    }
}

/* ========================================================================== */
/* Schedule block construction                                                 */
/* ========================================================================== */

/**
 * Add a node to a scheduled block.
 */
static int schedule_block_add_node(vtx_schedule_block_t *blk, vtx_nodeid_t nid)
{
    if (blk->node_count >= blk->node_capacity) {
        uint32_t new_cap = (blk->node_capacity == 0) ? 16 : blk->node_capacity * 2;
        vtx_nodeid_t *new_nodes = (vtx_nodeid_t *)realloc(blk->nodes, new_cap * sizeof(vtx_nodeid_t));
        if (new_nodes == NULL) return -1;
        blk->nodes = new_nodes;
        blk->node_capacity = new_cap;
    }
    blk->nodes[blk->node_count++] = nid;
    return 0;
}

/**
 * Add a successor edge to a block, growing the array if needed.
 *
 * BUGFIX: The original code allocated a fixed-size array of 4 entries for
 * successor/predecessor edges and silently dropped any edges beyond that
 * limit. This silently truncated CFG edges for blocks with more than 4
 * successors (e.g., switch statements with many cases) or more than 4
 * predecessors (e.g., merge points from many paths), producing an
 * incomplete CFG that led to incorrect scheduling and code generation.
 * The fix uses realloc to dynamically grow the array when it fills up.
 *
 * Returns 0 on success, -1 on allocation failure.
 */
static int schedule_block_add_succ(vtx_schedule_block_t *blk, uint32_t succ_idx)
{
    /* Check for duplicate */
    for (uint32_t i = 0; i < blk->succ_count; i++) {
        if (blk->succ_blocks[i] == succ_idx) return 0;
    }

    /* Grow if needed */
    if (blk->succ_count >= blk->succ_capacity) {
        uint32_t new_cap = (blk->succ_capacity == 0) ? 4 : blk->succ_capacity * 2;
        uint32_t *new_arr = (uint32_t *)realloc(blk->succ_blocks, new_cap * sizeof(uint32_t));
        if (new_arr == NULL) return -1;
        blk->succ_blocks = new_arr;
        blk->succ_capacity = new_cap;
    }
    blk->succ_blocks[blk->succ_count++] = succ_idx;
    return 0;
}

/**
 * Add a predecessor edge to a block, growing the array if needed.
 *
 * BUGFIX: Same as schedule_block_add_succ — the original silently truncated
 * edges beyond 4 entries.
 *
 * Returns 0 on success, -1 on allocation failure.
 */
static int schedule_block_add_pred(vtx_schedule_block_t *blk, uint32_t pred_idx)
{
    /* Check for duplicate */
    for (uint32_t i = 0; i < blk->pred_count; i++) {
        if (blk->pred_blocks[i] == pred_idx) return 0;
    }

    /* Grow if needed */
    if (blk->pred_count >= blk->pred_capacity) {
        uint32_t new_cap = (blk->pred_capacity == 0) ? 4 : blk->pred_capacity * 2;
        uint32_t *new_arr = (uint32_t *)realloc(blk->pred_blocks, new_cap * sizeof(uint32_t));
        if (new_arr == NULL) return -1;
        blk->pred_blocks = new_arr;
        blk->pred_capacity = new_cap;
    }
    blk->pred_blocks[blk->pred_count++] = pred_idx;
    return 0;
}

/* ========================================================================== */
/* Main scheduling algorithm                                                   */
/* ========================================================================== */

int vtx_schedule_run(vtx_graph_t *graph, vtx_arena_t *arena, vtx_schedule_t *schedule)
{
    VTX_ASSERT(graph != NULL, "graph must not be NULL");
    VTX_ASSERT(arena != NULL, "arena must not be NULL");
    VTX_ASSERT(schedule != NULL, "schedule must not be NULL");

    memset(schedule, 0, sizeof(vtx_schedule_t));

    vtx_node_table_t *nt = &graph->node_table;
    uint32_t node_count = nt->count;

    if (node_count == 0) return 0;

    /* Phase 1: Identify basic blocks from control nodes.
     *
     * Each Region/LoopBegin/Catch node starts a new block.
     * Each If/Switch/Return/Unwind/Deopt/Goto ends a block.
     * We walk all nodes and partition them into blocks. */

    /* First pass: count blocks (one per Region/LoopBegin node, plus Start) */
    uint32_t block_count = 0;
    for (uint32_t i = 0; i < node_count; i++) {
        vtx_node_t *node = &nt->nodes[i];
        if (node->dead) continue;
        if (node->opcode == VTX_OP_Region || node->opcode == VTX_OP_LoopBegin ||
            node->opcode == VTX_OP_Start || node->opcode == VTX_OP_Catch) {
            block_count++;
        }
    }

    if (block_count == 0) {
        /* No control structure — shouldn't happen, but handle gracefully */
        block_count = 1;
    }

    /* Allocate blocks */
    schedule->blocks = (vtx_schedule_block_t *)calloc(block_count, sizeof(vtx_schedule_block_t));
    if (schedule->blocks == NULL) return -1;
    schedule->count = block_count;
    schedule->capacity = block_count;

    /* Allocate node-block mapping */
    schedule->node_block = (uint32_t *)malloc(node_count * sizeof(uint32_t));
    if (schedule->node_block == NULL) {
        free(schedule->blocks);
        return -1;
    }
    schedule->node_block_count = node_count;
    for (uint32_t i = 0; i < node_count; i++) {
        schedule->node_block[i] = (uint32_t)-1;
    }

    /* Phase 2: Create blocks and assign control nodes */
    uint32_t bi = 0;

    /* The Start node always starts block 0 */
    if (graph->start_node < node_count) {
        schedule->blocks[bi].region_node = graph->start_node;
        schedule->blocks[bi].block_id = bi;
        schedule->node_block[graph->start_node] = bi;
        bi++;
    }

    /* Create blocks for other Region/LoopBegin/Catch nodes */
    for (uint32_t i = 0; i < node_count; i++) {
        vtx_node_t *node = &nt->nodes[i];
        if (node->dead) continue;
        if (i == graph->start_node) continue; /* already handled */
        if (node->opcode == VTX_OP_Region || node->opcode == VTX_OP_LoopBegin ||
            node->opcode == VTX_OP_Catch) {
            VTX_ASSERT(bi < block_count, "block index overflow");
            schedule->blocks[bi].region_node = i;
            schedule->blocks[bi].block_id = bi;
            schedule->blocks[bi].is_loop_header = (node->opcode == VTX_OP_LoopBegin);
            schedule->blocks[bi].is_catch = (node->opcode == VTX_OP_Catch);
            schedule->node_block[i] = bi;
            bi++;
        }
    }

    /* Fill in actual block count if we miscounted */
    schedule->count = bi;

    /* Phase 2.5: Assign control and pinned nodes to their blocks.
     *
     * BUGFIX: The original code skipped control nodes (If, LoopEnd, Goto,
     * Return) and pinned nodes (Phi) in Phase 5's data-node placement,
     * causing them to fall through to the "assign to block 0" fallback.
     * This put all If/LoopEnd/Return/Phi nodes in block 0 regardless of
     * where they actually belong, producing completely wrong instruction
     * ordering and branch targets.
     *
     * The correct rule: a control/pinned node goes in the same block as
     * its control input. We walk each node's first input (the control
     * dependency) until we find a node already assigned to a block
     * (i.e., a Region/LoopBegin/Start). This must iterate until fixed-
     * point because some control chains are multi-hop:
     *   If → Proj → If → Region  (nested if in same block)
     *   LoopEnd → If → Region
     *   Phi → Region
     */
    {
        bool changed = true;
        while (changed) {
            changed = false;
            for (uint32_t i = 0; i < node_count; i++) {
                vtx_node_t *node = &nt->nodes[i];
                if (node->dead) continue;
                if (schedule->node_block[i] != (uint32_t)-1) continue;

                /* Only process control and pinned nodes here */
                bool is_ctrl = vtx_nf_has(node->flags, VTX_NF_CONTROL);
                bool is_pinned = vtx_nf_has(node->flags, VTX_NF_PINNED);
                if (!is_ctrl && !is_pinned) continue;

                /* Walk the control chain to find an assigned block.
                 * For control nodes, input[0] is the control dependency.
                 * For Phi, we search all inputs for a Region/LoopBegin. */
                vtx_nodeid_t ctrl = VTX_NODEID_INVALID;
                if (node->opcode == VTX_OP_Phi) {
                    /* Phi: find the Region/LoopBegin among inputs.
                     * The Phi goes in the same block as its Region/LoopBegin.
                     * We look for the control input (Region/LoopBegin) specifically,
                     * not just any assigned input, because a Phi in a loop header
                     * should be in the loop header block, not in the preheader
                     * where param_n might be. */
                    for (uint32_t pi = 0; pi < node->input_count; pi++) {
                        vtx_nodeid_t inp = node->inputs[pi];
                        if (inp != VTX_NODEID_INVALID && inp < node_count) {
                            vtx_node_t *inp_node = &nt->nodes[inp];
                            if (inp_node->opcode == VTX_OP_Region ||
                                inp_node->opcode == VTX_OP_LoopBegin) {
                                uint32_t blk = schedule->node_block[inp];
                                if (blk != (uint32_t)-1) {
                                    ctrl = inp;
                                    break;
                                }
                            }
                        }
                    }
                    /* Fallback: if no Region/LoopBegin found, use any assigned input */
                    if (ctrl == VTX_NODEID_INVALID) {
                        for (uint32_t pi = 0; pi < node->input_count; pi++) {
                            vtx_nodeid_t inp = node->inputs[pi];
                            if (inp != VTX_NODEID_INVALID && inp < node_count) {
                                uint32_t blk = schedule->node_block[inp];
                                if (blk != (uint32_t)-1) {
                                    ctrl = inp;
                                    break;
                                }
                            }
                        }
                    }
                } else {
                    /* Control nodes: first input is the control dependency */
                    if (node->input_count > 0)
                        ctrl = node->inputs[0];
                }

                while (ctrl != VTX_NODEID_INVALID && ctrl < node_count) {
                    uint32_t blk = schedule->node_block[ctrl];
                    if (blk != (uint32_t)-1) {
                        schedule->node_block[i] = blk;
                        changed = true;
                        break;
                    }
                    /* Walk further up the control chain */
                    vtx_node_t *cn = &nt->nodes[ctrl];
                    if (cn->input_count > 0)
                        ctrl = cn->inputs[0];
                    else
                        break;
                }
            }
        }
    }

    /* Phase 2.75: Create entry edges from Start block to its successors.
     * The Start node may directly feed into Region/LoopBegin nodes
     * (the entry points of the function). Without this, there's no
     * edge from block 0 to the first control block, breaking the
     * dominator computation. */
    {
        uint32_t start_blk = (uint32_t)-1;
        if (graph->start_node < node_count) {
            start_blk = schedule->node_block[graph->start_node];
        }
        if (start_blk != (uint32_t)-1) {
            /* Find all Region/LoopBegin nodes that have Start as input */
            for (uint32_t i = 0; i < node_count; i++) {
                vtx_node_t *node = &nt->nodes[i];
                if (node->dead) continue;
                if (node->opcode != VTX_OP_Region && node->opcode != VTX_OP_LoopBegin)
                    continue;
                for (uint32_t k = 0; k < node->input_count; k++) {
                    if (node->inputs[k] == graph->start_node) {
                        uint32_t target_blk = schedule->node_block[i];
                        if (target_blk != (uint32_t)-1 && target_blk != start_blk) {
                            vtx_schedule_block_t *fb = &schedule->blocks[start_blk];
                            vtx_schedule_block_t *tb = &schedule->blocks[target_blk];
                            if (schedule_block_add_succ(fb, target_blk) != 0) return -1;
                            if (schedule_block_add_pred(tb, start_blk) != 0) return -1;
                        }
                        break;
                    }
                }
            }
        }
    }

    /* Phase 3: Build block successor/predecessor edges.
     * We do this by examining control flow: for each If node, its Proj
     * nodes determine successors. For Goto, it goes to the next Region.
     * For Return, no successor. */
    for (uint32_t i = 0; i < node_count; i++) {
        vtx_node_t *node = &nt->nodes[i];
        if (node->dead) continue;

        if (node->opcode == VTX_OP_Proj) {
            /* Proj nodes connect to Region nodes.
             * Find which Region this Proj feeds into. */
            for (uint32_t j = 0; j < node_count; j++) {
                vtx_node_t *user = &nt->nodes[j];
                if (user->dead) continue;
                if (user->opcode == VTX_OP_Region || user->opcode == VTX_OP_LoopBegin) {
                    for (uint32_t k = 0; k < user->input_count; k++) {
                        if (user->inputs[k] == i) {
                            /* This Proj feeds into this Region.
                             * The Proj's input is an If or Goto.
                             * The If/Goto's block is the predecessor.
                             * The Region's block is the successor. */
                            uint32_t succ_block = schedule->node_block[j];

                            /* We need to find the block containing the If node.
                             * The If node is the Proj's input. But the If's block
                             * is the block containing it. Walk back to find the
                             * If's Region. */
                            if (node->inputs[0] < node_count) {
                                /* Walk back through control chain to find Region */
                                vtx_nodeid_t ctrl = node->inputs[0];
                                uint32_t if_block = (uint32_t)-1;
                                /* The If is in the same block as its Region */
                                while (ctrl != VTX_NODEID_INVALID && ctrl < node_count) {
                                    if_block = schedule->node_block[ctrl];
                                    if (if_block != (uint32_t)-1) break;
                                    vtx_node_t *ctrl_node = &nt->nodes[ctrl];
                                    if (ctrl_node->input_count > 0) {
                                        ctrl = ctrl_node->inputs[0];
                                    } else {
                                        break;
                                    }
                                }

                                if (if_block != (uint32_t)-1 && succ_block != (uint32_t)-1) {
                                    /* Add edge if_block → succ_block */
                                    vtx_schedule_block_t *pred_blk = &schedule->blocks[if_block];
                                    vtx_schedule_block_t *succ_blk = &schedule->blocks[succ_block];

                                    /* Add successor (BUGFIX: uses dynamic growth) */
                                    if (schedule_block_add_succ(pred_blk, succ_block) != 0) return -1;

                                    /* Add predecessor (BUGFIX: uses dynamic growth) */
                                    if (schedule_block_add_pred(succ_blk, if_block) != 0) return -1;
                                }
                            }
                        }
                    }
                }
            }
        }

        /* Handle Goto nodes: they connect to a Region */
        if (node->opcode == VTX_OP_Goto) {
            /* Find which Region this Goto feeds into */
            for (uint32_t j = 0; j < node_count; j++) {
                vtx_node_t *region = &nt->nodes[j];
                if (region->dead) continue;
                if (region->opcode == VTX_OP_Region || region->opcode == VTX_OP_LoopBegin) {
                    for (uint32_t k = 0; k < region->input_count; k++) {
                        if (region->inputs[k] == i) {
                            uint32_t region_block = schedule->node_block[j];

                            /* Goto is in the same block as its input control node */
                            vtx_nodeid_t ctrl = node->input_count > 0 ? node->inputs[0] : VTX_NODEID_INVALID;
                            uint32_t from_block = (uint32_t)-1;
                            while (ctrl != VTX_NODEID_INVALID && ctrl < node_count) {
                                from_block = schedule->node_block[ctrl];
                                if (from_block != (uint32_t)-1) break;
                                vtx_node_t *cn = &nt->nodes[ctrl];
                                ctrl = cn->input_count > 0 ? cn->inputs[0] : VTX_NODEID_INVALID;
                            }

                            if (from_block != (uint32_t)-1 && region_block != (uint32_t)-1) {
                                vtx_schedule_block_t *fb = &schedule->blocks[from_block];
                                vtx_schedule_block_t *tb = &schedule->blocks[region_block];

                                /* BUGFIX: uses dynamic growth instead of fixed 4-entry limit */
                                if (schedule_block_add_succ(fb, region_block) != 0) return -1;
                                if (schedule_block_add_pred(tb, from_block) != 0) return -1;
                            }
                        }
                    }
                }
            }
        }

        /* Handle If nodes that connect directly to Region/LoopBegin
         * without intermediate Proj nodes.
         *
         * BUGFIX: The original code only built CFG edges for Proj and Goto
         * nodes. When a graph builder connected If → Region/LoopBegin
         * directly (without Proj intermediaries), no successor/predecessor
         * edges were created, breaking dominator computation, loop depth,
         * branch target resolution, and Phi copy emission.
         *
         * This fallback handles the common pattern:
         *   If → LoopEnd (continue branch)
         *   If → Region  (exit branch)
         * We scan all Region/LoopBegin nodes for If inputs and create
         * the corresponding CFG edges. Each Region/LoopBegin that has
         * an If as input is a successor of the If's block. */
        if (node->opcode == VTX_OP_If) {
            /* Find the If's block via its control input */
            vtx_nodeid_t ctrl = node->input_count > 0 ? node->inputs[0] : VTX_NODEID_INVALID;
            uint32_t if_block = (uint32_t)-1;
            while (ctrl != VTX_NODEID_INVALID && ctrl < node_count) {
                if_block = schedule->node_block[ctrl];
                if (if_block != (uint32_t)-1) break;
                vtx_node_t *cn = &nt->nodes[ctrl];
                ctrl = cn->input_count > 0 ? cn->inputs[0] : VTX_NODEID_INVALID;
            }
            if (if_block == (uint32_t)-1) continue;

            /* Find all Region/LoopBegin nodes that have this If as input */
            for (uint32_t j = 0; j < node_count; j++) {
                vtx_node_t *region = &nt->nodes[j];
                if (region->dead) continue;
                if (region->opcode != VTX_OP_Region && region->opcode != VTX_OP_LoopBegin)
                    continue;
                for (uint32_t k = 0; k < region->input_count; k++) {
                    if (region->inputs[k] == i) {
                        /* This If feeds directly into this Region/LoopBegin */
                        uint32_t region_block = schedule->node_block[j];
                        if (region_block != (uint32_t)-1 && region_block != if_block) {
                            vtx_schedule_block_t *fb = &schedule->blocks[if_block];
                            vtx_schedule_block_t *tb = &schedule->blocks[region_block];
                            if (schedule_block_add_succ(fb, region_block) != 0) return -1;
                            if (schedule_block_add_pred(tb, if_block) != 0) return -1;
                        }
                        break; /* Each If input appears once per Region */
                    }
                }
            }

            /* Also handle LoopEnd nodes that have this If as input.
             * LoopEnd creates a back-edge from the If's block to the
             * LoopBegin's block (which is the same block). We add this
             * self-edge so that the dominator/loop-depth computation
             * can detect the back-edge. */
            for (uint32_t j = 0; j < node_count; j++) {
                vtx_node_t *le = &nt->nodes[j];
                if (le->dead) continue;
                if (le->opcode != VTX_OP_LoopEnd) continue;
                if (le->input_count < 1 || le->inputs[0] != i) continue;

                /* LoopEnd is in the If's block (if_block). The back-edge
                 * goes to the LoopBegin that is the region_node of if_block. */
                vtx_schedule_block_t *fb = &schedule->blocks[if_block];
                /* Add self-edge (back-edge) for the loop */
                if (schedule_block_add_succ(fb, if_block) != 0) return -1;
                if (schedule_block_add_pred(fb, if_block) != 0) return -1;
                break; /* At most one LoopEnd per If */
            }

            /* Also check for LoopEnd nodes that have a Proj of this If
             * as input (e.g., If → Proj(true) → LoopEnd). */
            for (uint32_t j = 0; j < node_count; j++) {
                vtx_node_t *le = &nt->nodes[j];
                if (le->dead) continue;
                if (le->opcode != VTX_OP_LoopEnd) continue;
                if (le->input_count < 1) continue;

                /* Check if LoopEnd's input is a Proj of this If */
                vtx_nodeid_t le_input = le->inputs[0];
                if (le_input < node_count) {
                    vtx_node_t *le_input_node = &nt->nodes[le_input];
                    if (le_input_node->opcode == VTX_OP_Proj &&
                        le_input_node->input_count > 0 &&
                        le_input_node->inputs[0] == i) {
                        /* This LoopEnd is reached through a Proj of this If */
                        vtx_schedule_block_t *fb = &schedule->blocks[if_block];
                        if (schedule_block_add_succ(fb, if_block) != 0) return -1;
                        if (schedule_block_add_pred(fb, if_block) != 0) return -1;
                        break;
                    }
                }
            }
        }

        /* Handle LoopEnd nodes that are not connected through If directly.
         * LoopEnd creates a back-edge from its block to the LoopBegin block
         * it belongs to. We find the LoopBegin by checking which LoopBegin
         * has Phi nodes whose back-edge inputs come from the LoopEnd's block.
         *
         * This handles cases like: Proj → Region → LoopEnd where the
         * LoopEnd doesn't have an If as its direct control input. */
        if (node->opcode == VTX_OP_LoopEnd) {
            vtx_nodeid_t ctrl = node->input_count > 0 ? node->inputs[0] : VTX_NODEID_INVALID;
            uint32_t le_block = (uint32_t)-1;
            while (ctrl != VTX_NODEID_INVALID && ctrl < node_count) {
                le_block = schedule->node_block[ctrl];
                if (le_block != (uint32_t)-1) break;
                vtx_node_t *cn = &nt->nodes[ctrl];
                ctrl = cn->input_count > 0 ? cn->inputs[0] : VTX_NODEID_INVALID;
            }
            if (le_block == (uint32_t)-1) continue;

            /* Find the LoopBegin that this LoopEnd goes back to.
             * Scan all blocks for LoopBegin headers, then check if
             * any Phi in that block has a back-edge input in le_block. */
            for (uint32_t j = 0; j < schedule->count; j++) {
                if (!schedule->blocks[j].is_loop_header) continue;
                if (j == le_block) continue; /* self-edge handled elsewhere */

                vtx_nodeid_t lb_node = schedule->blocks[j].region_node;
                /* Check Phi nodes in this LoopBegin's block for
                 * back-edge inputs from le_block */
                for (uint32_t p = 0; p < node_count; p++) {
                    vtx_node_t *phi = &nt->nodes[p];
                    if (phi->dead || phi->opcode != VTX_OP_Phi) continue;
                    if (schedule->node_block[p] != j) continue;

                    /* Check if any Phi input is in le_block */
                    for (uint32_t pi = 0; pi < phi->input_count; pi++) {
                        vtx_nodeid_t phi_inp = phi->inputs[pi];
                        if (phi_inp != VTX_NODEID_INVALID && phi_inp < node_count) {
                            if (schedule->node_block[phi_inp] == le_block) {
                                /* Found a back-edge from le_block to loop header j */
                                vtx_schedule_block_t *fb = &schedule->blocks[le_block];
                                vtx_schedule_block_t *tb = &schedule->blocks[j];
                                if (schedule_block_add_succ(fb, j) != 0) return -1;
                                if (schedule_block_add_pred(tb, le_block) != 0) return -1;
                                goto next_loopend; /* Found the back-edge, move on */
                            }
                        }
                    }
                }
            }
            next_loopend:;
        }

        /* Handle ExceptProj nodes: they represent exception edges.
         * An ExceptProj connects a throwing node's block to the catch
         * handler block (the block containing the Catch node that the
         * ExceptProj feeds into). We treat this like a control edge —
         * the catch handler block is a successor of the throwing block. */
        if (node->opcode == VTX_OP_ExceptProj) {
            /* Find which Catch node this ExceptProj feeds into.
             * The ExceptProj's input is the throwing node; we need
             * to find the Catch node that consumes this ExceptProj. */
            for (uint32_t j = 0; j < node_count; j++) {
                vtx_node_t *catch_node = &nt->nodes[j];
                if (catch_node->dead) continue;
                if (catch_node->opcode == VTX_OP_Catch) {
                    for (uint32_t k = 0; k < catch_node->input_count; k++) {
                        if (catch_node->inputs[k] == i) {
                            /* This ExceptProj feeds into this Catch.
                             * The Catch's block is the successor.
                             * The throwing node's block is the predecessor. */
                            uint32_t catch_block = schedule->node_block[j];
                            if (catch_block == (uint32_t)-1) continue;

                            /* Find the block of the throwing node */
                            if (node->input_count > 0 && node->inputs[0] < node_count) {
                                vtx_nodeid_t thrower = node->inputs[0];
                                uint32_t from_block = schedule->node_block[thrower];
                                /* If the thrower isn't directly in a block,
                                 * walk back through its inputs to find one */
                                while (from_block == (uint32_t)-1 &&
                                       thrower != VTX_NODEID_INVALID &&
                                       thrower < node_count) {
                                    vtx_node_t *tn = &nt->nodes[thrower];
                                    if (tn->input_count > 0) {
                                        thrower = tn->inputs[0];
                                        from_block = schedule->node_block[thrower];
                                    } else {
                                        break;
                                    }
                                }

                                if (from_block != (uint32_t)-1 &&
                                    from_block != catch_block) {
                                    vtx_schedule_block_t *fb = &schedule->blocks[from_block];
                                    vtx_schedule_block_t *tb = &schedule->blocks[catch_block];

                                    /* BUGFIX: uses dynamic growth with built-in dedup */
                                    if (schedule_block_add_succ(fb, catch_block) != 0) return -1;
                                    if (schedule_block_add_pred(tb, from_block) != 0) return -1;
                                }
                            }
                            break; /* Each ExceptProj feeds into at most one Catch */
                        }
                    }
                }
            }
        }
    }

    /* BUG-16 fix: Guard debug CFG printing with VORTEX_ENABLE_VERIFY
     * so it doesn't spam stderr in production builds. */
#ifdef VORTEX_ENABLE_VERIFY
    fprintf(stderr, "[schedule] CFG edges after Phase 3:\n");
    for (uint32_t dbi = 0; dbi < schedule->count; dbi++) {
        vtx_schedule_block_t *dbl = &schedule->blocks[dbi];
        fprintf(stderr, "  Block %u: succ=[", dbi);
        for (uint32_t ds = 0; ds < dbl->succ_count; ds++) {
            if (ds > 0) fprintf(stderr, ",");
            fprintf(stderr, "%u", dbl->succ_blocks[ds]);
        }
        fprintf(stderr, "] pred=[");
        for (uint32_t dp = 0; dp < dbl->pred_count; dp++) {
            if (dp > 0) fprintf(stderr, ",");
            fprintf(stderr, "%u", dbl->pred_blocks[dp]);
        }
        fprintf(stderr, "]\n");
    }
#endif

    /* Phase 4: Compute dominators and loop depth.
     * Keep the dominator array alive through Phase 5 so we can use it
     * for LCA computation when placing data nodes. */
    uint32_t start_block = 0;
    uint32_t *dom = compute_dominators(schedule->count, start_block, schedule->blocks);
    if (dom != NULL) {
        compute_loop_depth(schedule->count, start_block, schedule->blocks, dom);
    }

    /* Phase 5: Assign data and memory nodes to blocks.
     *
     * For each non-control, non-dead node, find the block where it should
     * be scheduled. The rule is:
     *   - A node must be in a block that dominates all its uses.
     *   - A node should be as early as possible (hoist for CSE benefit).
     *   - Loop-invariant nodes should be hoisted out of loops.
     *
     * We assign each node to the LCA (lowest common ancestor) of all blocks
     * that contain its inputs in the dominator tree. This is the lowest
     * block that dominates all inputs.
     *
     * BUGFIX: The original code used `ib > best_block` (block index
     * comparison) to pick the "latest" input block. Block indices have no
     * guaranteed relationship to dominance — block 5 does not necessarily
     * dominate block 3 just because 5 > 3. The correct approach is to
     * compute the LCA in the dominator tree, which finds the lowest block
     * that dominates all input blocks. */

    /* LCA helper using the dominator tree.
     * Walks two blocks up the dominator tree until they meet. */
    uint32_t lca_block = start_block; /* default for nodes with no inputs */

    /* First, assign memory chain nodes. Memory nodes must follow the
     * memory chain order, so they go in the same block as their control
     * context. */
    for (uint32_t i = 0; i < node_count; i++) {
        vtx_node_t *node = &nt->nodes[i];
        if (node->dead) continue;
        if (schedule->node_block[i] != (uint32_t)-1) continue; /* already assigned */

        if (vtx_nf_has(node->flags, VTX_NF_MEMORY)) {
            /* Memory nodes go in the same block as their control input
             * or the LCA of their input blocks using dominator tree. */
            uint32_t best_block = (uint32_t)-1;
            for (uint32_t j = 0; j < node->input_count; j++) {
                vtx_nodeid_t inp = node->inputs[j];
                if (inp != VTX_NODEID_INVALID && inp < node_count) {
                    uint32_t ib = schedule->node_block[inp];
                    if (ib != (uint32_t)-1) {
                        if (best_block == (uint32_t)-1) {
                            best_block = ib;
                        } else if (dom != NULL) {
                            /* Compute LCA of best_block and ib using dominator tree */
                            uint32_t a = best_block;
                            uint32_t b = ib;
                            while (a != b) {
                                if (a == (uint32_t)-1 || b == (uint32_t)-1) break;
                                if (a >= schedule->count || b >= schedule->count) break;
                                /* Walk the one with deeper dominator chain upward.
                                 * We use a simple approach: walk both up until they
                                 * meet. The block with higher RPO depth walks first. */
                                if (schedule->blocks[a].loop_depth > schedule->blocks[b].loop_depth) {
                                    a = dom[a];
                                } else if (schedule->blocks[b].loop_depth > schedule->blocks[a].loop_depth) {
                                    b = dom[b];
                                } else {
                                    a = dom[a];
                                    b = dom[b];
                                }
                            }
                            if (a == b && a != (uint32_t)-1 && a < schedule->count) {
                                best_block = a;
                            }
                            /* else: couldn't compute LCA, keep current best_block */
                        } else {
                            /* Fallback without dominator info: prefer higher loop depth */
                            if (schedule->blocks[ib].loop_depth > schedule->blocks[best_block].loop_depth) {
                                best_block = ib;
                            }
                        }
                    }
                }
            }
            if (best_block != (uint32_t)-1) {
                schedule->node_block[i] = best_block;
            }
        }
    }

    /* Assign data nodes. Each data node goes in the LCA of all its
     * input blocks in the dominator tree. */
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t i = 0; i < node_count; i++) {
            vtx_node_t *node = &nt->nodes[i];
            if (node->dead) continue;
            if (schedule->node_block[i] != (uint32_t)-1) continue;
            if (vtx_nf_has(node->flags, VTX_NF_CONTROL)) continue;
            if (vtx_nf_has(node->flags, VTX_NF_PINNED)) continue;

            /* Check if all inputs are assigned */
            bool all_inputs_assigned = true;
            for (uint32_t j = 0; j < node->input_count; j++) {
                vtx_nodeid_t inp = node->inputs[j];
                if (inp != VTX_NODEID_INVALID && inp < node_count && !nt->nodes[inp].dead) {
                    if (schedule->node_block[inp] == (uint32_t)-1) {
                        all_inputs_assigned = false;
                        break;
                    }
                }
            }

            if (!all_inputs_assigned) continue;

            /* Compute LCA of all input blocks using dominator tree.
             * BUGFIX: The original code used `ib > best_block` which
             * compares block indices — these have no dominance semantics.
             * The correct approach is LCA in the dominator tree.
             *
             * IR-4 fix: Skip globally-available inputs (Constants,
             * Parameters, Start) when computing the LCA. These values
             * can be materialized in any block, so they should not
             * constrain the placement of their consumers. Without this,
             * a Sub(phi_n, constant_1) would be placed in block 0 (the
             * LCA of the loop header and the constant's block), which is
             * wrong — the Sub should be in the loop header block. */
            lca_block = start_block; /* default: start block */
            bool has_inputs = false;
            for (uint32_t j = 0; j < node->input_count; j++) {
                vtx_nodeid_t inp = node->inputs[j];
                if (inp != VTX_NODEID_INVALID && inp < node_count) {
                    /* Skip globally-available inputs */
                    const vtx_node_t *inp_node = &nt->nodes[inp];
                    if (inp_node->opcode == VTX_OP_Constant ||
                        inp_node->opcode == VTX_OP_Parameter ||
                        inp_node->opcode == VTX_OP_Start) {
                        continue;
                    }
                    uint32_t ib = schedule->node_block[inp];
                    if (ib != (uint32_t)-1) {
                        if (!has_inputs) {
                            lca_block = ib;
                            has_inputs = true;
                        } else if (dom != NULL) {
                            /* Compute LCA of lca_block and ib */
                            uint32_t a = lca_block;
                            uint32_t b = ib;
                            while (a != b) {
                                if (a >= schedule->count || b >= schedule->count) break;
                                /* Walk both upward until they converge.
                                 * Use loop_depth as a proxy for dominator depth
                                 * to avoid unnecessary upward walking. */
                                if (schedule->blocks[a].loop_depth > schedule->blocks[b].loop_depth) {
                                    a = dom[a];
                                } else if (schedule->blocks[b].loop_depth > schedule->blocks[a].loop_depth) {
                                    b = dom[b];
                                } else {
                                    a = dom[a];
                                    b = dom[b];
                                }
                            }
                            if (a == b && a < schedule->count) {
                                lca_block = a;
                            }
                            /* else: fallback, keep current lca_block */
                        }
                    }
                }
            }

            schedule->node_block[i] = lca_block;
            changed = true;
        }
    }

    /* Free dominator array now that Phase 5 is done */
    if (dom != NULL) {
        free(dom);
        dom = NULL;
    }

    /* Assign remaining unassigned nodes to block 0 */
    for (uint32_t i = 0; i < node_count; i++) {
        if (!nt->nodes[i].dead && schedule->node_block[i] == (uint32_t)-1) {
            schedule->node_block[i] = 0;
        }
    }

    /* Phase 6: Loop-invariant code motion.
     * For each data node in a loop block, check if all its inputs are
     * defined outside the loop or are themselves loop-invariant.
     * If so, hoist the node to the loop preheader (the block that
     * dominates the loop header). */
    for (uint32_t bi = 0; bi < schedule->count; bi++) {
        if (schedule->blocks[bi].loop_depth == 0) continue;

        /* Iterate nodes assigned to this block */
        for (uint32_t i = 0; i < node_count; i++) {
            if (schedule->node_block[i] != bi) continue;
            vtx_node_t *node = &nt->nodes[i];
            if (node->dead) continue;
            if (vtx_nf_has(node->flags, VTX_NF_CONTROL)) continue;
            if (vtx_nf_has(node->flags, VTX_NF_MEMORY)) continue;
            if (vtx_nf_has(node->flags, VTX_NF_SIDE_EFFECT)) continue;
            if (vtx_nf_has(node->flags, VTX_NF_PINNED)) continue;

            /* Check if all inputs are defined outside the loop */
            bool all_inputs_outside = true;
            for (uint32_t j = 0; j < node->input_count; j++) {
                vtx_nodeid_t inp = node->inputs[j];
                if (inp == VTX_NODEID_INVALID || inp >= node_count) continue;
                uint32_t ib = schedule->node_block[inp];
                if (schedule->blocks[ib].loop_depth >= schedule->blocks[bi].loop_depth) {
                    all_inputs_outside = false;
                    break;
                }
            }

            if (all_inputs_outside && schedule->blocks[bi].pred_count > 0) {
                /* BUG-13 fix: Find the preheader, not just pred_blocks[0].
                 * The first predecessor might be the back-edge (latch),
                 * not the preheader. The preheader is the predecessor
                 * whose loop_depth is strictly less than this block's.
                 * If all predecessors are at the same depth (e.g. a
                 * non-loop merge), fall back to pred_blocks[0]. */
                uint32_t preheader = schedule->blocks[bi].pred_blocks[0];
                uint32_t my_depth = schedule->blocks[bi].loop_depth;
                for (uint32_t pi = 0; pi < schedule->blocks[bi].pred_count; pi++) {
                    uint32_t pred_idx = schedule->blocks[bi].pred_blocks[pi];
                    if (schedule->blocks[pred_idx].loop_depth < my_depth) {
                        preheader = pred_idx;
                        break;
                    }
                }
                schedule->node_block[i] = preheader;
            }
        }
    }

    /* Phase 7: Build the ordered node lists for each block.
     * We do a topological sort within each block: a node must come
     * after all its inputs that are in the same block. */
    for (uint32_t i = 0; i < node_count; i++) {
        vtx_node_t *node = &nt->nodes[i];
        if (node->dead) continue;

        uint32_t blk_idx = schedule->node_block[i];
        if (blk_idx >= schedule->count) continue;

        /* We add nodes in ID order, which is roughly topological
         * since the graph was built in order. A proper topological
         * sort would be needed for more complex cases. */
        schedule_block_add_node(&schedule->blocks[blk_idx], i);
    }

    /* Phase 8: Within each block, sort nodes to respect dependencies.
     * We use a simple insertion-sort-like approach: for each node,
     * ensure it comes after its inputs in the same block.
     *
     * BUGFIX: Phi nodes are pinned at the top of their block and should
     * NOT be moved, because their back-edge inputs create circular
     * dependencies (Phi uses loop body result, loop body uses Phi).
     * Without this exclusion, the sort would try to move the Phi after
     * its back-edge input, breaking the evaluation order.
     *
     * BUGFIX: Also skip back-edge inputs when checking dependencies.
     * A node in a loop body may have an input from a Phi that has a
     * back-edge input from a later node in the same block. We should
     * only reorder based on forward-edge inputs (inputs from earlier
     * blocks or from Constants/Parameters). */
    for (uint32_t bi = 0; bi < schedule->count; bi++) {
        vtx_schedule_block_t *blk = &schedule->blocks[bi];
        bool sorted = false;
        uint32_t passes = 0;

        while (!sorted && passes < blk->node_count * 2) {
            sorted = true;
            passes++;
            for (uint32_t i = 1; i < blk->node_count; i++) {
                vtx_nodeid_t nid = blk->nodes[i];
                if (nid >= node_count) continue;
                vtx_node_t *node = &nt->nodes[nid];

                /* Phi nodes are pinned — never move them */
                if (node->opcode == VTX_OP_Phi) continue;

                /* Check if any input of this node appears later in the block.
                 * Skip back-edge inputs: if the input is a Phi that has this
                 * block's region as an input, it's a loop-carried dependency
                 * that creates a cycle. The Phi provides the value at the
                 * start of the iteration, not after the back-edge. */
                for (uint32_t j = 0; j < node->input_count; j++) {
                    vtx_nodeid_t inp = node->inputs[j];
                    /* Skip inputs not in this block */
                    if (inp == VTX_NODEID_INVALID || inp >= node_count) continue;
                    if (schedule->node_block[inp] != bi) continue;

                    /* Skip back-edge inputs (Phi with loop-carried value) */
                    vtx_node_t *inp_node = &nt->nodes[inp];
                    if (inp_node->opcode == VTX_OP_Phi) continue;

                    /* Find position of inp in this block */
                    for (uint32_t k = i + 1; k < blk->node_count; k++) {
                        if (blk->nodes[k] == inp) {
                            /* Swap: move inp before nid */
                            /* Actually, we need to move nid after inp.
                             * Remove nid from position i and re-insert after k. */
                            for (uint32_t m = i; m < k; m++) {
                                blk->nodes[m] = blk->nodes[m + 1];
                            }
                            blk->nodes[k] = nid;
                            sorted = false;
                            break;
                        }
                    }
                }
            }
        }
    }

    return 0;
}

/* ========================================================================== */
/* Schedule destruction                                                        */
/* ========================================================================== */

void vtx_schedule_destroy(vtx_schedule_t *schedule)
{
    if (schedule == NULL) return;

    if (schedule->blocks != NULL) {
        for (uint32_t i = 0; i < schedule->count; i++) {
            free(schedule->blocks[i].nodes);
            free(schedule->blocks[i].df_blocks);
            /* BUGFIX: succ_blocks and pred_blocks are now realloc'd
             * dynamically, so they must be freed. The original code
             * assumed they were arena-allocated and skipped freeing,
             * causing a memory leak. */
            free(schedule->blocks[i].succ_blocks);
            free(schedule->blocks[i].pred_blocks);
        }
        free(schedule->blocks);
    }

    free(schedule->node_block);
    schedule->blocks = NULL;
    schedule->node_block = NULL;
    schedule->count = 0;
    schedule->capacity = 0;
    schedule->node_block_count = 0;
}

uint32_t vtx_schedule_node_block(const vtx_schedule_t *schedule, vtx_nodeid_t node_id)
{
    if (schedule == NULL || schedule->node_block == NULL) return (uint32_t)-1;
    if (node_id >= schedule->node_block_count) return (uint32_t)-1;
    return schedule->node_block[node_id];
}

/* ========================================================================== */
/* Schedule printing                                                           */
/* ========================================================================== */

void vtx_schedule_print(const vtx_schedule_t *schedule, const vtx_graph_t *graph)
{
    VTX_ASSERT(schedule != NULL, "schedule must not be NULL");
    VTX_ASSERT(graph != NULL, "graph must not be NULL");

    fprintf(stderr, "=== VORTEX Schedule (%u blocks) ===\n", schedule->count);

    for (uint32_t bi = 0; bi < schedule->count; bi++) {
        const vtx_schedule_block_t *blk = &schedule->blocks[bi];
        fprintf(stderr, "Block %u (region=N%u, depth=%u)%s%s:\n",
                bi, blk->region_node, blk->loop_depth,
                blk->is_loop_header ? " [LOOP_HEADER]" : "",
                blk->is_catch ? " [CATCH]" : "");

        for (uint32_t i = 0; i < blk->node_count; i++) {
            vtx_nodeid_t nid = blk->nodes[i];
            const vtx_node_t *n = vtx_node_get_const(&graph->node_table, nid);
            if (n == NULL) {
                fprintf(stderr, "  N%u: <invalid>\n", nid);
            } else {
                fprintf(stderr, "  N%u: %s\n", nid, vtx_node_opcode_name(n->opcode));
            }
        }
    }
}
