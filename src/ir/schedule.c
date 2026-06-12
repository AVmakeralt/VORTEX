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

                                    /* Add successor */
                                    if (pred_blk->succ_count == 0) {
                                        pred_blk->succ_blocks = (uint32_t *)vtx_arena_alloc(arena, 4 * sizeof(uint32_t));
                                        if (pred_blk->succ_blocks == NULL) return -1;
                                        pred_blk->succ_count = 0;
                                        /* Note: arena memory doesn't need realloc, but
                                         * we limit to 4 successors per block which is sufficient */
                                    }
                                    if (pred_blk->succ_count < 4) {
                                        pred_blk->succ_blocks[pred_blk->succ_count++] = succ_block;
                                    }

                                    /* Add predecessor */
                                    if (succ_blk->pred_count == 0) {
                                        succ_blk->pred_blocks = (uint32_t *)vtx_arena_alloc(arena, 4 * sizeof(uint32_t));
                                        if (succ_blk->pred_blocks == NULL) return -1;
                                        succ_blk->pred_count = 0;
                                    }
                                    if (succ_blk->pred_count < 4) {
                                        succ_blk->pred_blocks[succ_blk->pred_count++] = if_block;
                                    }
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

                                if (fb->succ_count < 4) {
                                    if (fb->succ_count == 0) {
                                        fb->succ_blocks = (uint32_t *)vtx_arena_alloc(arena, 4 * sizeof(uint32_t));
                                    }
                                    if (fb->succ_blocks) fb->succ_blocks[fb->succ_count++] = region_block;
                                }
                                if (tb->pred_count < 4) {
                                    if (tb->pred_count == 0) {
                                        tb->pred_blocks = (uint32_t *)vtx_arena_alloc(arena, 4 * sizeof(uint32_t));
                                    }
                                    if (tb->pred_blocks) tb->pred_blocks[tb->pred_count++] = from_block;
                                }
                            }
                        }
                    }
                }
            }
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

                                    /* Add exception successor edge */
                                    if (fb->succ_count < 4) {
                                        if (fb->succ_count == 0) {
                                            fb->succ_blocks = (uint32_t *)vtx_arena_alloc(arena, 4 * sizeof(uint32_t));
                                        }
                                        if (fb->succ_blocks &&
                                            /* Avoid duplicate edges */
                                            fb->succ_blocks[fb->succ_count - 1] != catch_block) {
                                            fb->succ_blocks[fb->succ_count++] = catch_block;
                                        }
                                    }
                                    /* Add exception predecessor edge */
                                    if (tb->pred_count < 4) {
                                        if (tb->pred_count == 0) {
                                            tb->pred_blocks = (uint32_t *)vtx_arena_alloc(arena, 4 * sizeof(uint32_t));
                                        }
                                        if (tb->pred_blocks &&
                                            /* Avoid duplicate edges */
                                            tb->pred_blocks[tb->pred_count - 1] != from_block) {
                                            tb->pred_blocks[tb->pred_count++] = from_block;
                                        }
                                    }
                                }
                            }
                            break; /* Each ExceptProj feeds into at most one Catch */
                        }
                    }
                }
            }
        }
    }

    /* Phase 4: Compute dominators and loop depth */
    uint32_t start_block = 0;
    uint32_t *dom = compute_dominators(schedule->count, start_block, schedule->blocks);
    if (dom != NULL) {
        compute_loop_depth(schedule->count, start_block, schedule->blocks, dom);
        free(dom);
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
     * that contain its users. For simplicity, we use a two-pass approach:
     *   1. First assign nodes with no data inputs (constants, parameters)
     *      to the earliest possible block.
     *   2. Then assign nodes with inputs to the block of their latest input.
     */

    /* First, assign memory chain nodes. Memory nodes must follow the
     * memory chain order, so they go in the same block as their control
     * context. */
    for (uint32_t i = 0; i < node_count; i++) {
        vtx_node_t *node = &nt->nodes[i];
        if (node->dead) continue;
        if (schedule->node_block[i] != (uint32_t)-1) continue; /* already assigned */

        if (vtx_nf_has(node->flags, VTX_NF_MEMORY)) {
            /* Memory nodes go in the same block as their control input
             * or the block of their memory input. */
            uint32_t best_block = (uint32_t)-1;
            for (uint32_t j = 0; j < node->input_count; j++) {
                vtx_nodeid_t inp = node->inputs[j];
                if (inp != VTX_NODEID_INVALID && inp < node_count) {
                    uint32_t ib = schedule->node_block[inp];
                    if (ib != (uint32_t)-1) {
                        if (best_block == (uint32_t)-1) {
                            best_block = ib;
                        } else {
                            /* Use the later block (dominated by the earlier) */
                            /* Simple heuristic: prefer the block with higher loop depth,
                             * or the one that dominates the other */
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

    /* Assign data nodes. Each data node goes in the block of its latest
     * input (or the LCA of all input blocks). */
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

            /* Find the latest input block */
            uint32_t best_block = 0; /* default: start block */
            for (uint32_t j = 0; j < node->input_count; j++) {
                vtx_nodeid_t inp = node->inputs[j];
                if (inp != VTX_NODEID_INVALID && inp < node_count) {
                    uint32_t ib = schedule->node_block[inp];
                    if (ib != (uint32_t)-1 && ib > best_block) {
                        best_block = ib;
                    }
                }
            }

            schedule->node_block[i] = best_block;
            changed = true;
        }
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
                /* Hoist to the first predecessor (preheader for a loop header) */
                uint32_t preheader = schedule->blocks[bi].pred_blocks[0];
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
     * ensure it comes after its inputs in the same block. */
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

                /* Check if any input of this node appears later in the block */
                for (uint32_t j = 0; j < node->input_count; j++) {
                    vtx_nodeid_t inp = node->inputs[j];
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
            /* pred_blocks and succ_blocks are arena-allocated, no free needed */
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
