/**
 * VORTEX Hyperblock Stitching — Implementation
 *
 * Stitches a trace tree into a hyperblock by inlining hot branch traces
 * at guard points in the root trace. The result is a single-entry,
 * multiple-exit SoN subgraph suitable for cross-trace optimization.
 */

#include "region/stitch.h"
#include "region/budget.h"
#include "trace/tree.h"
#include "trace/side_exit.h"
#include "runtime/arena.h"
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* Hyperblock node list helpers                                                */
/* ========================================================================== */

static int vtx_hyperblock_ensure_node_capacity(vtx_hyperblock_t *hb,
                                                vtx_arena_t *arena,
                                                uint32_t needed)
{
    if (hb->node_count + needed <= hb->node_capacity) return 0;

    uint32_t new_cap = hb->node_capacity;
    while (new_cap < hb->node_count + needed) {
        new_cap *= 2;
    }

    vtx_nodeid_t *new_nodes = vtx_arena_alloc(arena,
                                                sizeof(vtx_nodeid_t) * new_cap);
    if (new_nodes == NULL) return -1;
    memcpy(new_nodes, hb->nodes, sizeof(vtx_nodeid_t) * hb->node_count);
    hb->nodes = new_nodes;
    hb->node_capacity = new_cap;
    return 0;
}

static int vtx_hyperblock_add_node(vtx_hyperblock_t *hb,
                                    vtx_arena_t *arena,
                                    vtx_nodeid_t node)
{
    if (vtx_hyperblock_ensure_node_capacity(hb, arena, 1) != 0) return -1;
    hb->nodes[hb->node_count++] = node;
    return 0;
}

/* ========================================================================== */
/* Hyperblock exit list helpers                                                */
/* ========================================================================== */

static int vtx_hyperblock_add_exit(vtx_hyperblock_t *hb,
                                    vtx_arena_t *arena,
                                    const vtx_hyperblock_exit_t *exit)
{
    if (hb->exit_count >= hb->exit_capacity) {
        uint32_t new_cap = hb->exit_capacity * 2;
        vtx_hyperblock_exit_t *new_exits = vtx_arena_alloc(arena,
                sizeof(vtx_hyperblock_exit_t) * new_cap);
        if (new_exits == NULL) return -1;
        memcpy(new_exits, hb->exits,
               sizeof(vtx_hyperblock_exit_t) * hb->exit_count);
        hb->exits = new_exits;
        hb->exit_capacity = new_cap;
    }
    hb->exits[hb->exit_count++] = *exit;
    return 0;
}

/* ========================================================================== */
/* Branch prioritization for stitching                                         */
/* ========================================================================== */

/**
 * A candidate branch for stitching, prioritized by a score that
 * combines exit frequency and estimated speedup.
 */
typedef struct {
    vtx_trace_branch_t *branch;
    vtx_side_exit_t    *side_exit;    /* the exit that triggers this branch */
    double              score;        /* priority score (higher = more important) */
} vtx_stitch_candidate_t;

/**
 * Compute the stitching priority score for a branch.
 * score = exit_count * estimated_speedup
 *
 * estimated_speedup is a heuristic: branches that are closed (loop back)
 * get a higher speedup because they eliminate interpreter re-entry on
 * hot alternate paths. Open branches get a lower score.
 */
static double vtx_stitch_candidate_score(const vtx_side_exit_t *exit,
                                          const vtx_trace_t *trace)
{
    double exit_weight = (double)exit->exit_counter;
    double speedup = vtx_trace_is_closed(trace) ? 2.0 : 1.0;
    return exit_weight * speedup;
}

static int vtx_stitch_candidate_compare_desc(const void *a, const void *b)
{
    const vtx_stitch_candidate_t *ca = (const vtx_stitch_candidate_t *)a;
    const vtx_stitch_candidate_t *cb = (const vtx_stitch_candidate_t *)b;
    if (ca->score > cb->score) return -1;
    if (ca->score < cb->score) return  1;
    return 0;
}

/* ========================================================================== */
/* Inline a branch trace at a guard point                                      */
/* ========================================================================== */

/**
 * Inline a branch trace's nodes into the hyperblock at the position
 * where the guard node for the side exit appears.
 *
 * Algorithm:
 *   1. Find the guard node in the hyperblock's node list.
 *   2. The guard is retained (it still protects the main path).
 *   3. After the guard, insert the branch trace's nodes.
 *   4. If the branch trace is closed (loops back), connect its end
 *      back to the loop header (LoopBegin node).
 *   5. If the branch trace is open or truncated, add a rejoinder
 *      guard that exits to the interpreter.
 *
 * Returns 0 on success, -1 on failure.
 */
static int vtx_stitch_inline_branch(vtx_hyperblock_t *hb,
                                     vtx_graph_t *graph,
                                     const vtx_side_exit_t *exit,
                                     const vtx_trace_t *branch_trace,
                                     vtx_arena_t *arena)
{
    VTX_ASSERT(hb != NULL, "hyperblock must not be NULL");
    VTX_ASSERT(graph != NULL, "graph must not be NULL");
    VTX_ASSERT(exit != NULL, "side exit must not be NULL");
    VTX_ASSERT(branch_trace != NULL, "branch trace must not be NULL");
    VTX_ASSERT(arena != NULL, "arena must not be NULL");

    /* Find the guard node in the hyperblock */
    int32_t guard_index = -1;
    for (uint32_t i = 0; i < hb->node_count; i++) {
        if (hb->nodes[i] == exit->guard_node) {
            guard_index = (int32_t)i;
            break;
        }
    }

    if (guard_index < 0) {
        /* Guard node not found in hyperblock — this shouldn't happen
         * for a well-formed trace tree, but we handle it gracefully. */
        return -1;
    }

    /* Ensure we have capacity for the branch's nodes */
    if (vtx_hyperblock_ensure_node_capacity(hb, arena,
            branch_trace->node_count + 4) != 0) {
        return -1;
    }

    /* Insert branch trace nodes after the guard.
     * We skip the branch trace's own LoopBegin/Region start node since
     * the guard already serves as the control flow entry point for the branch.
     * We start from index 1 if the first node is a LoopBegin or Region. */
    uint32_t start_idx = 0;
    if (branch_trace->node_count > 0) {
        vtx_node_t *first = vtx_node_get(&graph->node_table, branch_trace->nodes[0]);
        if (first != NULL &&
            (first->opcode == VTX_OP_LoopBegin || first->opcode == VTX_OP_Region)) {
            start_idx = 1;
        }
    }

    /* Make room for the branch nodes after the guard by shifting
     * existing nodes. The insertion point is guard_index + 1. */
    uint32_t insert_pos = (uint32_t)(guard_index + 1);
    uint32_t nodes_to_insert = branch_trace->node_count - start_idx;

    if (nodes_to_insert > 0) {
        /* Shift nodes after the guard to make room */
        uint32_t move_count = hb->node_count - insert_pos;
        memmove(&hb->nodes[insert_pos + nodes_to_insert],
                &hb->nodes[insert_pos],
                sizeof(vtx_nodeid_t) * move_count);

        /* Copy branch nodes into the gap */
        for (uint32_t i = start_idx; i < branch_trace->node_count; i++) {
            hb->nodes[insert_pos + (i - start_idx)] = branch_trace->nodes[i];
        }

        hb->node_count += nodes_to_insert;
    }

    /* If the branch trace is open or truncated, add a rejoinder guard
     * at the end. The rejoinder guard exits to the interpreter at the
     * bytecode PC after the original guard's instruction. */
    if (!vtx_trace_is_closed(branch_trace)) {
        vtx_nodeid_t rejoinder_nid = vtx_node_create(&graph->node_table, VTX_OP_Guard);
        if (rejoinder_nid == VTX_NODEID_INVALID) return -1;

        vtx_node_t *rejoinder = vtx_node_get(&graph->node_table, rejoinder_nid);
        if (rejoinder != NULL) {
            rejoinder->cond = VTX_COND_ALWAYS;
            rejoinder->bytecode_pc = exit->target_pc;
        }

        if (vtx_hyperblock_add_node(hb, arena, rejoinder_nid) != 0) return -1;

        /* Add an exit descriptor for the rejoinder */
        vtx_hyperblock_exit_t rejoinder_exit;
        memset(&rejoinder_exit, 0, sizeof(rejoinder_exit));
        rejoinder_exit.exit_node = rejoinder_nid;
        rejoinder_exit.target_pc = exit->target_pc;
        rejoinder_exit.reason = VTX_EXIT_BRANCH_NOT_TAKEN;

        /* Copy stack state from the side exit */
        rejoinder_exit.stack_depth = exit->stack_state.stack_depth;
        if (exit->stack_state.stack_depth > 0 && exit->stack_state.stack != NULL) {
            rejoinder_exit.stack_state = vtx_arena_alloc(arena,
                sizeof(vtx_nodeid_t) * exit->stack_state.stack_depth);
            if (rejoinder_exit.stack_state == NULL) return -1;
            memcpy(rejoinder_exit.stack_state, exit->stack_state.stack,
                   sizeof(vtx_nodeid_t) * exit->stack_state.stack_depth);
        }

        if (vtx_hyperblock_add_exit(hb, arena, &rejoinder_exit) != 0) return -1;
    }

    return 0;
}

/* ========================================================================== */
/* Estimate native code size for a set of nodes                                */
/* ========================================================================== */

/**
 * Rough estimate of native code size for a set of SoN nodes.
 * This is a conservative estimate used for budget checking.
 * Each node maps to roughly 4-16 bytes of x86-64 machine code.
 */
static uint32_t vtx_estimate_native_size(const vtx_node_table_t *table,
                                          const vtx_nodeid_t *nodes,
                                          uint32_t count)
{
    uint32_t total = 0;
    for (uint32_t i = 0; i < count; i++) {
        vtx_node_t *node = vtx_node_get_const(table, nodes[i]);
        if (node == NULL) continue;

        switch (node->opcode) {
        /* Simple arithmetic: typically 3-5 bytes */
        case VTX_OP_Add: case VTX_OP_Sub: case VTX_OP_Mul:
        case VTX_OP_And: case VTX_OP_Or:  case VTX_OP_Xor:
        case VTX_OP_Shl: case VTX_OP_Shr:
        case VTX_OP_Neg: case VTX_OP_Not:
            total += 6;
            break;

        /* Division: more complex, needs call or sequence */
        case VTX_OP_Div: case VTX_OP_Mod:
            total += 20;
            break;

        /* Memory operations: 3-8 bytes for addressing */
        case VTX_OP_Load: case VTX_OP_LoadField: case VTX_OP_LoadIndexed:
            total += 8;
            break;
        case VTX_OP_Store: case VTX_OP_StoreField: case VTX_OP_StoreIndexed:
            total += 8;
            break;

        /* Comparisons: 4-6 bytes */
        case VTX_OP_Cmp: case VTX_OP_CmpP: case VTX_OP_CmpF: case VTX_OP_CmpD:
            total += 6;
            break;

        /* Guards: 6-12 bytes (compare + conditional jump) */
        case VTX_OP_Guard: case VTX_OP_DeoptGuard:
            total += 12;
            break;

        /* Calls: 5 bytes for call + argument setup */
        case VTX_OP_CallStatic: case VTX_OP_CallVirtual:
        case VTX_OP_CallInterface: case VTX_OP_CallRuntime:
            total += 32;
            break;

        /* Allocations: call to runtime + initialization */
        case VTX_OP_NewObject: case VTX_OP_NewArray: case VTX_OP_Allocate:
            total += 24;
            break;

        /* Control: 2-6 bytes */
        case VTX_OP_If: case VTX_OP_Goto: case VTX_OP_Switch:
            total += 6;
            break;

        /* Constants: often folded, but account for load if not */
        case VTX_OP_Constant:
            total += 4;
            break;

        /* Type operations: 6-12 bytes */
        case VTX_OP_CheckCast: case VTX_OP_InstanceOf:
            total += 12;
            break;

        /* Frame state / deopt: metadata only, small code impact */
        case VTX_OP_FrameState: case VTX_OP_Deopt:
            total += 2;
            break;

        /* Structural nodes: no code */
        case VTX_OP_Start: case VTX_OP_End:
        case VTX_OP_LoopBegin: case VTX_OP_LoopEnd:
        case VTX_OP_Province: case VTX_OP_Region:
        case VTX_OP_Phi: case VTX_OP_Proj:
        case VTX_OP_MemBar: case VTX_OP_Initialize:
        case VTX_OP_InitializeKlass:
            total += 0;
            break;

        /* Return */
        case VTX_OP_Return:
            total += 4;
            break;

        default:
            total += 8;
            break;
        }
    }
    return total;
}

/* ========================================================================== */
/* Build hyperblock from root trace only                                       */
/* ========================================================================== */

/**
 * Create a hyperblock from just the root trace, without any branches.
 * This is the first step before inlining hot branches.
 */
static vtx_hyperblock_t *vtx_stitch_build_root_hyperblock(
    const vtx_trace_tree_t *tree,
    vtx_graph_t *graph,
    vtx_arena_t *arena)
{
    vtx_trace_t *root = tree->root;
    if (root == NULL) return NULL;

    vtx_hyperblock_t *hb = vtx_arena_alloc(arena, sizeof(vtx_hyperblock_t));
    if (hb == NULL) return NULL;
    memset(hb, 0, sizeof(vtx_hyperblock_t));

    /* Initialize node list */
    hb->node_capacity = root->node_count + 16;
    hb->nodes = vtx_arena_alloc(arena, sizeof(vtx_nodeid_t) * hb->node_capacity);
    if (hb->nodes == NULL) return NULL;
    memcpy(hb->nodes, root->nodes, sizeof(vtx_nodeid_t) * root->node_count);
    hb->node_count = root->node_count;

    /* Initialize exit list */
    hb->exit_capacity = root->side_exit_count + 4;
    hb->exits = vtx_arena_alloc(arena,
                                 sizeof(vtx_hyperblock_exit_t) * hb->exit_capacity);
    if (hb->exits == NULL) return NULL;
    hb->exit_count = 0;

    /* Set entry point */
    hb->entry_node = root->start_node;
    hb->source_tree = tree;
    hb->terminal_control = root->control_node;
    hb->terminal_memory = root->memory_node;

    /* Add side exits as hyperblock exits */
    for (uint32_t i = 0; i < root->side_exit_count; i++) {
        vtx_side_exit_t *se = root->side_exits[i];
        if (se == NULL) continue;

        vtx_hyperblock_exit_t exit;
        memset(&exit, 0, sizeof(exit));
        exit.exit_node = se->guard_node;
        exit.target_pc = se->target_pc;
        exit.stack_depth = se->stack_state.stack_depth;
        exit.reason = se->reason;

        if (se->stack_state.stack_depth > 0 && se->stack_state.stack != NULL) {
            exit.stack_state = vtx_arena_alloc(arena,
                sizeof(vtx_nodeid_t) * se->stack_state.stack_depth);
            if (exit.stack_state == NULL) return NULL;
            memcpy(exit.stack_state, se->stack_state.stack,
                   sizeof(vtx_nodeid_t) * se->stack_state.stack_depth);
        }

        if (vtx_hyperblock_add_exit(hb, arena, &exit) != 0) return NULL;
    }

    /* Estimate native size */
    hb->estimated_native_size = vtx_estimate_native_size(
        &graph->node_table, hb->nodes, hb->node_count);

    return hb;
}

/* ========================================================================== */
/* Stitcher lifecycle                                                          */
/* ========================================================================== */

int vtx_stitcher_init(vtx_stitcher_t *stitcher)
{
    VTX_ASSERT(stitcher != NULL, "stitcher must not be NULL");
    stitcher->branches_stitched = 0;
    stitcher->branches_skipped = 0;
    return 0;
}

void vtx_stitcher_destroy(vtx_stitcher_t *stitcher)
{
    (void)stitcher;
}

/* ========================================================================== */
/* Main stitching entry point                                                  */
/* ========================================================================== */

vtx_hyperblock_t *vtx_stitcher_stitch(vtx_stitcher_t *stitcher,
                                       const vtx_trace_tree_t *tree,
                                       vtx_graph_t *graph,
                                       vtx_arena_t *arena)
{
    VTX_ASSERT(stitcher != NULL, "stitcher must not be NULL");
    VTX_ASSERT(tree != NULL, "tree must not be NULL");
    VTX_ASSERT(graph != NULL, "graph must not be NULL");
    VTX_ASSERT(arena != NULL, "arena must not be NULL");

    stitcher->branches_stitched = 0;
    stitcher->branches_skipped = 0;

    /* Step 1: Build the root hyperblock from the root trace */
    vtx_hyperblock_t *hb = vtx_stitch_build_root_hyperblock(tree, graph, arena);
    if (hb == NULL) return NULL;

    /* Step 2: Identify candidate branches for stitching.
     * A branch is a candidate if its side exit counter exceeds
     * VTX_STITCH_THRESHOLD. */
    uint32_t candidate_count = 0;
    vtx_stitch_candidate_t *candidates = NULL;

    if (tree->branch_count > 0) {
        candidates = vtx_arena_alloc(arena,
            sizeof(vtx_stitch_candidate_t) * tree->branch_count);
        if (candidates == NULL) return hb; /* return root-only hyperblock */

        for (uint32_t i = 0; i < tree->branch_count; i++) {
            vtx_trace_branch_t *branch = tree->all_branches[i];
            if (branch == NULL || branch->trace == NULL) continue;

            /* Find the side exit that triggered this branch */
            vtx_side_exit_t *exit = vtx_trace_tree_find_exit(
                tree, branch->parent_exit_id);
            if (exit == NULL) continue;

            /* Check if the exit is hot enough for stitching */
            if (exit->exit_counter <= VTX_STITCH_THRESHOLD) continue;

            vtx_stitch_candidate_t *cand = &candidates[candidate_count++];
            cand->branch = branch;
            cand->side_exit = exit;
            cand->score = vtx_stitch_candidate_score(exit, branch->trace);
        }
    }

    if (candidate_count == 0) {
        return hb; /* no branches to stitch */
    }

    /* Step 3: Sort candidates by score (highest priority first) */
    qsort(candidates, candidate_count, sizeof(vtx_stitch_candidate_t),
          vtx_stitch_candidate_compare_desc);

    /* Step 4: Initialize the budget checker */
    vtx_budget_t budget;
    vtx_budget_init(&budget);

    /* Step 5: Greedily inline branches in priority order.
     * For each candidate, check if inlining it would exceed the budget.
     * If not, inline it. If so, skip it (backtracking: try the next one). */
    for (uint32_t i = 0; i < candidate_count; i++) {
        vtx_stitch_candidate_t *cand = &candidates[i];
        vtx_trace_branch_t *branch = cand->branch;
        vtx_side_exit_t *exit = cand->side_exit;

        /* Compute the additional node count from this branch */
        uint32_t branch_nodes = branch->trace->node_count;
        /* Skip LoopBegin/Region at start */
        if (branch_nodes > 0) {
            vtx_node_t *first = vtx_node_get(&graph->node_table,
                                               branch->trace->nodes[0]);
            if (first != NULL &&
                (first->opcode == VTX_OP_LoopBegin ||
                 first->opcode == VTX_OP_Region)) {
                branch_nodes--;
            }
        }
        /* Add 1 for potential rejoinder guard */
        branch_nodes++;

        /* Check node budget */
        if (hb->node_count + branch_nodes > VTX_MAX_HYPERBLOCK_NODES) {
            stitcher->branches_skipped++;
            continue;
        }

        /* Estimate additional native code size */
        uint32_t branch_start = (branch->trace->node_count > 0 &&
            vtx_node_get(&graph->node_table, branch->trace->nodes[0]) != NULL &&
            (vtx_node_get(&graph->node_table, branch->trace->nodes[0])->opcode ==
                VTX_OP_LoopBegin ||
             vtx_node_get(&graph->node_table, branch->trace->nodes[0])->opcode ==
                VTX_OP_Region)) ? 1 : 0;

        uint32_t added_native = vtx_estimate_native_size(
            &graph->node_table,
            &branch->trace->nodes[branch_start],
            branch->trace->node_count - branch_start);
        added_native += 12; /* rejoinder guard estimate */

        /* Check native size budget */
        if (hb->estimated_native_size + added_native > VTX_MAX_NATIVE_SIZE) {
            stitcher->branches_skipped++;
            continue;
        }

        /* Check budget formally */
        uint32_t projected_node_count = hb->node_count + branch_nodes;
        uint32_t projected_native_size = hb->estimated_native_size + added_native;
        if (!vtx_budget_check_counts(&budget, projected_node_count,
                                      projected_native_size)) {
            stitcher->branches_skipped++;
            continue;
        }

        /* Inline the branch */
        if (vtx_stitch_inline_branch(hb, graph, exit, branch->trace, arena) != 0) {
            /* Inline failed — skip this branch */
            stitcher->branches_skipped++;
            continue;
        }

        /* Update estimates */
        hb->estimated_native_size += added_native;
        stitcher->branches_stitched++;
    }

    return hb;
}

/* ========================================================================== */
/* Hyperblock access                                                           */
/* ========================================================================== */

vtx_nodeid_t vtx_hyperblock_entry(const vtx_hyperblock_t *hb)
{
    return hb != NULL ? hb->entry_node : VTX_NODEID_INVALID;
}

uint32_t vtx_hyperblock_node_count(const vtx_hyperblock_t *hb)
{
    return hb != NULL ? hb->node_count : 0;
}

uint32_t vtx_hyperblock_exit_count(const vtx_hyperblock_t *hb)
{
    return hb != NULL ? hb->exit_count : 0;
}

vtx_nodeid_t vtx_hyperblock_get_node(const vtx_hyperblock_t *hb, uint32_t index)
{
    if (hb == NULL || index >= hb->node_count) return VTX_NODEID_INVALID;
    return hb->nodes[index];
}

const vtx_hyperblock_exit_t *vtx_hyperblock_get_exit(const vtx_hyperblock_t *hb,
                                                       uint32_t index)
{
    if (hb == NULL || index >= hb->exit_count) return NULL;
    return &hb->exits[index];
}
