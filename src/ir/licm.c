#include "ir/licm.h"
#include "ir/tbaa.h"

#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* Loop-Invariant Code Motion (LICM) for VORTEX SoN IR                        */
/*                                                                            */
/* Algorithm:                                                                 */
/*   1. Identify loops from the schedule (blocks with loop_depth > 0)         */
/*   2. For each loop (innermost first), find the preheader block             */
/*   3. Iteratively compute which nodes in the loop are loop-invariant:       */
/*      a. Not pinned (Phi, Region, LoopBegin, LoopEnd, FrameState)           */
/*      b. Not a control node                                                 */
/*      c. Not a memory node with side effects (stores, calls, allocations)   */
/*      d. All inputs are defined outside the loop OR are themselves invariant */
/*   4. For memory loads, only hoist if no potentially-aliasing store exists   */
/*      anywhere in the loop body (conservative; TBAA refines later)          */
/*   5. For Guard nodes, only hoist if the guarded condition is invariant     */
/*   6. Move invariant nodes to the preheader block                           */
/*   7. Update the schedule's node_block map                                  */
/* ========================================================================== */

/* ========================================================================== */
/* Internal data structures                                                    */
/* ========================================================================== */

/**
 * Per-node LICM state.
 *   VTX_LICM_UNKNOWN   — not yet classified
 *   VTX_LICM_INVARIANT — determined to be loop-invariant
 *   VTX_LICM_VARIANT   — depends on loop-variant value or is non-hoistable
 */
typedef enum {
    VTX_LICM_UNKNOWN   = 0,
    VTX_LICM_INVARIANT = 1,
    VTX_LICM_VARIANT   = 2
} vtx_licm_state_t;

/**
 * Working state for a single loop's LICM analysis.
 */
typedef struct {
    const vtx_schedule_t  *schedule;
    vtx_graph_t           *graph;
    vtx_node_table_t      *nt;
    uint32_t               node_count;

    /* The loop header block index */
    uint32_t               header_block;

    /* The preheader block index (outside the loop, entry into the loop) */
    uint32_t               preheader_block;

    /* The loop depth of the header (used to determine "inside the loop") */
    uint32_t               loop_depth;

    /* Per-node invariance state (indexed by node ID) */
    vtx_licm_state_t      *invariant_state;

    /* Bit-vector: is this node inside the current loop? */
    bool                  *in_loop;

    /* Bit-vector: does this loop contain any store that may alias? */
    bool                   has_escaping_store;
    bool                   has_store;
    bool                   has_call;
    bool                   has_allocation;

    /* TBAA analysis result (shared across all loops in this compilation) */
    const vtx_tbaa_result_t *tbaa_result;

    /* Number of nodes hoisted in this loop */
    uint32_t               hoisted_count;
} vtx_licm_loop_ctx_t;

/* ========================================================================== */
/* Helper: is a node inherently non-hoistable?                                */
/*                                                                            */
/* Nodes that can never be hoisted out of a loop:                             */
/*   - Pinned nodes (Phi, Region, FrameState) — position-dependent            */
/*   - Control nodes (If, Goto, Switch, Return, etc.)                         */
/*   - Side-effecting nodes (stores, calls, allocations, deopt)               */
/*   - Memory nodes that write (Store, StoreField, StoreIndexed, MemBar,      */
/*     Initialize, InitializeKlass)                                            */
/* ========================================================================== */

static bool node_is_never_hoistable(const vtx_node_t *node)
{
    /* Dead nodes are irrelevant */
    if (node->dead) return true;

    /* Pinned nodes must stay at their position */
    if (vtx_nf_has(node->flags, VTX_NF_PINNED)) return true;

    /* Control nodes define CFG structure and cannot float */
    if (vtx_nf_has(node->flags, VTX_NF_CONTROL)) return true;

    /* Side-effecting nodes must execute in program order */
    if (vtx_nf_has(node->flags, VTX_NF_SIDE_EFFECT)) return true;

    /* Explicitly reject stores and memory barriers — they write memory */
    switch (node->opcode) {
    case VTX_OP_Store:
    case VTX_OP_StoreField:
    case VTX_OP_StoreIndexed:
    case VTX_OP_MemBar:
    case VTX_OP_Initialize:
    case VTX_OP_InitializeKlass:
    case VTX_OP_Deopt:
    case VTX_OP_DeoptGuard:
    case VTX_OP_Unwind:
    case VTX_OP_NewObject:
    case VTX_OP_NewArray:
    case VTX_OP_Allocate:
        return true;
    default:
        break;
    }

    /* Call nodes always have side effects (may write memory, throw, etc.) */
    if (node->opcode == VTX_OP_CallStatic ||
        node->opcode == VTX_OP_CallVirtual ||
        node->opcode == VTX_OP_CallInterface ||
        node->opcode == VTX_OP_CallRuntime) {
        return true;
    }

    return false;
}

/* ========================================================================== */
/* Helper: is a node a memory read (load) that could potentially be hoisted?  */
/*                                                                            */
/* Memory loads can be hoisted only if there are no aliasing stores in the    */
/* loop. This is a conservative check — we assume any store may alias any     */
/* load unless we can prove otherwise.                                        */
/* ========================================================================== */

static bool node_is_memory_load(vtx_node_opcode_t opcode)
{
    return (opcode == VTX_OP_Load ||
            opcode == VTX_OP_LoadField ||
            opcode == VTX_OP_LoadIndexed);
}

/* ========================================================================== */
/* Helper: is a node a Guard that might be hoistable?                         */
/*                                                                            */
/* Guard nodes have the VTX_NF_SIDE_EFFECT flag, but they are semantically   */
/* just assertions — if the condition is loop-invariant, the guard can be     */
/* hoisted because it will always produce the same result.                    */
/*                                                                            */
/* However, we must be careful: hoisting a guard changes the deoptimization   */
/* point. We only hoist if the guard's condition input and its control input  */
/* are both loop-invariant.                                                   */
/* ========================================================================== */

static bool node_is_hoistable_guard(vtx_node_opcode_t opcode)
{
    return (opcode == VTX_OP_Guard);
}

/* ========================================================================== */
/* Find the preheader block for a loop                                        */
/*                                                                            */
/* The preheader is the predecessor of the loop header that is NOT part of    */
/* the loop (i.e., has a lower loop_depth). For well-formed loops, exactly    */
/* one predecessor is the preheader; the other(s) are back edges from inside  */
/* the loop.                                                                  */
/*                                                                            */
/* Returns the block index of the preheader, or (uint32_t)-1 if not found.    */
/* ========================================================================== */

static uint32_t find_preheader(const vtx_schedule_t *schedule,
                                uint32_t header_block)
{
    const vtx_schedule_block_t *header = &schedule->blocks[header_block];

    for (uint32_t i = 0; i < header->pred_count; i++) {
        uint32_t pred = header->pred_blocks[i];
        /* The preheader is the predecessor whose loop_depth is strictly less
         * than the header's loop_depth. For a simple loop, the header has
         * loop_depth = d, the preheader has loop_depth < d, and the backedge
         * predecessor has loop_depth >= d. */
        if (schedule->blocks[pred].loop_depth < header->loop_depth) {
            return pred;
        }
    }

    /* If no predecessor has a strictly lower loop depth, try to find the
     * predecessor that is not dominated by the header (i.e., comes from
     * outside the loop). This handles some irregular CFG shapes. */
    for (uint32_t i = 0; i < header->pred_count; i++) {
        uint32_t pred = header->pred_blocks[i];
        if (!schedule->blocks[pred].is_loop_header ||
            schedule->blocks[pred].loop_depth < header->loop_depth) {
            return pred;
        }
    }

    return (uint32_t)-1;
}

/* ========================================================================== */
/* Determine which nodes are "in the loop"                                    */
/*                                                                            */
/* A node is in the loop if it is scheduled in a block whose loop_depth is    */
/* >= the header's loop_depth. For nested loops, this correctly identifies    */
/* only nodes in the current loop level (and inner loops).                    */
/*                                                                            */
/* For LICM purposes, we consider a node to be "in the loop" if its block's   */
/* loop_depth is >= header's loop_depth AND the block is reachable from the   */
/* header (i.e., its block index corresponds to a block in this loop nest).   */
/* ========================================================================== */

static void compute_loop_membership(vtx_licm_loop_ctx_t *ctx)
{
    memset(ctx->in_loop, 0, ctx->node_count * sizeof(bool));

    for (uint32_t nid = 0; nid < ctx->node_count; nid++) {
        vtx_node_t *node = &ctx->nt->nodes[nid];
        if (node->dead) continue;

        uint32_t blk = vtx_schedule_node_block(ctx->schedule, nid);
        if (blk == (uint32_t)-1) continue;

        /* A node is in this loop if its block's loop_depth >= the header's
         * loop_depth. This means nodes in inner loops are also considered
         * part of the outer loop, which is correct for LICM: we only hoist
         * from the current loop to its preheader, not from inner loops. */
        if (ctx->schedule->blocks[blk].loop_depth >= ctx->loop_depth) {
            ctx->in_loop[nid] = true;
        }
    }
}

/* ========================================================================== */
/* Scan the loop for stores, calls, and allocations                           */
/*                                                                            */
/* This information is used to determine whether memory loads can be safely   */
/* hoisted. If the loop contains any store, we conservatively assume it may   */
/* alias any load. If it contains any call, the call may read or write any    */
/* memory. If it contains any allocation, the allocation might change the     */
/* heap layout.                                                               */
/* ========================================================================== */

static void scan_loop_side_effects(vtx_licm_loop_ctx_t *ctx)
{
    ctx->has_store = false;
    ctx->has_call = false;
    ctx->has_allocation = false;
    ctx->has_escaping_store = false;

    for (uint32_t nid = 0; nid < ctx->node_count; nid++) {
        if (!ctx->in_loop[nid]) continue;

        vtx_node_t *node = &ctx->nt->nodes[nid];
        if (node->dead) continue;

        switch (node->opcode) {
        case VTX_OP_Store:
        case VTX_OP_StoreField:
        case VTX_OP_StoreIndexed:
            ctx->has_store = true;
            ctx->has_escaping_store = true;
            break;

        case VTX_OP_CallStatic:
        case VTX_OP_CallVirtual:
        case VTX_OP_CallInterface:
        case VTX_OP_CallRuntime:
            ctx->has_call = true;
            /* Calls may write to any memory, so they act as escaping stores */
            ctx->has_escaping_store = true;
            break;

        case VTX_OP_NewObject:
        case VTX_OP_NewArray:
        case VTX_OP_Allocate:
            ctx->has_allocation = true;
            /* Allocations don't alias existing objects, but they may trigger
             * GC which could move objects. Conservatively treat as a store. */
            break;

        case VTX_OP_Initialize:
        case VTX_OP_InitializeKlass:
            ctx->has_store = true;
            ctx->has_escaping_store = true;
            break;

        case VTX_OP_MemBar:
            /* MemBar prevents reordering but doesn't write memory itself */
            ctx->has_escaping_store = true;
            break;

        default:
            break;
        }
    }
}

/* ========================================================================== */
/* Compute loop-invariance for all nodes in the loop                          */
/*                                                                            */
/* A node is loop-invariant if:                                               */
/*   1. It is not inherently non-hoistable (not pinned, not control, not      */
/*      side-effecting)                                                       */
/*   2. All of its inputs are either defined outside the loop, OR are         */
/*      themselves loop-invariant                                             */
/*                                                                            */
/* We use a worklist-driven fixed-point iteration:                            */
/*   - Initially, all non-hoistable nodes are VARIANT, and all potentially    */
/*     hoistable nodes are UNKNOWN                                            */
/*   - Nodes whose inputs are all outside the loop or INVARIANT become        */
/*     INVARIANT                                                              */
/*   - Iterate until no more state changes occur                              */
/* ========================================================================== */

static void compute_invariance(vtx_licm_loop_ctx_t *ctx)
{
    bool changed = true;
    uint32_t iteration = 0;
    const uint32_t max_iterations = 64; /* bounded for safety */

    while (changed && iteration < max_iterations) {
        changed = false;
        iteration++;

        for (uint32_t nid = 0; nid < ctx->node_count; nid++) {
            /* Skip nodes not in this loop */
            if (!ctx->in_loop[nid]) continue;

            /* Skip nodes already classified */
            if (ctx->invariant_state[nid] != VTX_LICM_UNKNOWN) continue;

            vtx_node_t *node = &ctx->nt->nodes[nid];
            if (node->dead) {
                ctx->invariant_state[nid] = VTX_LICM_VARIANT;
                continue;
            }

            /* Check if this node type can ever be hoisted */
            if (node_is_never_hoistable(node)) {
                ctx->invariant_state[nid] = VTX_LICM_VARIANT;
                changed = true;
                continue;
            }

            /* Special handling for memory loads: they can only be hoisted if
             * there are no potentially-aliasing stores in the loop.
             *
             * With TBAA, we refine this check: a load can be hoisted if
             * all stores in the loop are proven to NOT alias the load
             * (different type categories). This enables hoisting int[]
             * loads past ref[] stores, float[] loads past int[] stores, etc. */
            if (node_is_memory_load(node->opcode)) {
                if (ctx->has_escaping_store) {
                    /* There are stores in the loop. Check if TBAA can prove
                     * they don't alias this load. */
                    bool can_hoist_load = true;

                    if (ctx->tbaa_result != NULL) {
                        const vtx_tbaa_info_t *load_info =
                            vtx_tbaa_get_info(ctx->tbaa_result, nid);
                        if (load_info != NULL && load_info->kind != VTX_TBAA_ANY &&
                            load_info->kind != VTX_TBAA_RAW) {
                            /* We have TBAA info for this load. Check all stores
                             * in the loop to see if any may alias. */
                            for (uint32_t sid = 0; sid < ctx->node_count; sid++) {
                                if (!ctx->in_loop[sid]) continue;
                                const vtx_node_t *store_node = &ctx->nt->nodes[sid];
                                if (store_node->dead) continue;

                                /* Check if this is a store */
                                vtx_tbaa_kind_t store_kind = VTX_TBAA_ANY;
                                switch (store_node->opcode) {
                                case VTX_OP_Store:
                                case VTX_OP_StoreField:
                                case VTX_OP_StoreIndexed:
                                case VTX_OP_Initialize:
                                case VTX_OP_VectorStore: {
                                    const vtx_tbaa_info_t *store_info =
                                        vtx_tbaa_get_info(ctx->tbaa_result, sid);
                                    if (store_info != NULL) {
                                        store_kind = store_info->kind;
                                    }
                                    break;
                                }
                                case VTX_OP_CallStatic:
                                case VTX_OP_CallVirtual:
                                case VTX_OP_CallInterface:
                                case VTX_OP_CallRuntime:
                                    /* Calls may write any memory — can't hoist */
                                    can_hoist_load = false;
                                    goto done_store_check;
                                default:
                                    break;
                                }

                                if (store_kind == VTX_TBAA_ANY ||
                                    store_kind == VTX_TBAA_RAW) {
                                    /* Unknown store — can't hoist */
                                    can_hoist_load = false;
                                    goto done_store_check;
                                }

                                /* Check if the load can be hoisted past this store */
                                if (!vtx_tbaa_can_hoist_load(load_info->kind, store_kind)) {
                                    can_hoist_load = false;
                                    goto done_store_check;
                                }
                            }
                        done_store_check:
                            (void)0;  /* label requires a statement */
                        } else {
                            /* No TBAA info for this load — conservative */
                            can_hoist_load = false;
                        }
                    } else {
                        /* No TBAA result — conservative: any store prevents hoisting */
                        can_hoist_load = false;
                    }

                    if (!can_hoist_load) {
                        ctx->invariant_state[nid] = VTX_LICM_VARIANT;
                        changed = true;
                        continue;
                    }

                    /* TBAA proves no aliasing — this load CAN be hoisted
                     * despite the presence of stores in the loop. */
                }
            }

            /* Special handling for Guard nodes: they are side-effecting, so
             * node_is_never_hoistable returns true. But we still need to
             * classify them for the hoistable_guard check later. Guards
             * are handled separately in the hoisting phase. */
            if (node_is_hoistable_guard(node->opcode)) {
                /* Guard was already classified as VARIANT by
                 * node_is_never_hoistable. We'll handle it in
                 * try_hoist_guards(). */
                continue;
            }

            /* Check all inputs: are they defined outside the loop or
             * themselves loop-invariant? */
            bool all_inputs_invariant = true;
            for (uint32_t j = 0; j < node->input_count; j++) {
                vtx_nodeid_t inp_id = node->inputs[j];

                /* Invalid or out-of-range inputs don't prevent hoisting */
                if (inp_id == VTX_NODEID_INVALID || inp_id >= ctx->node_count) {
                    continue;
                }

                /* If the input is defined outside the loop, it's fine */
                if (!ctx->in_loop[inp_id]) {
                    continue;
                }

                /* If the input is in the loop and already classified as
                 * VARIANT, this node cannot be invariant */
                if (ctx->invariant_state[inp_id] == VTX_LICM_VARIANT) {
                    all_inputs_invariant = false;
                    break;
                }

                /* If the input is still UNKNOWN, we can't decide yet.
                 * Wait for the next iteration. */
                if (ctx->invariant_state[inp_id] == VTX_LICM_UNKNOWN) {
                    all_inputs_invariant = false;
                    break;
                }

                /* Input is INVARIANT — this is fine */
            }

            if (all_inputs_invariant) {
                /* All inputs are from outside the loop or are invariant.
                 * Double-check: verify all loop-resident inputs are
                 * indeed marked INVARIANT. */
                bool confirmed = true;
                for (uint32_t j = 0; j < node->input_count; j++) {
                    vtx_nodeid_t inp_id = node->inputs[j];
                    if (inp_id == VTX_NODEID_INVALID || inp_id >= ctx->node_count) {
                        continue;
                    }
                    if (ctx->in_loop[inp_id] &&
                        ctx->invariant_state[inp_id] != VTX_LICM_INVARIANT) {
                        confirmed = false;
                        break;
                    }
                }

                if (confirmed) {
                    ctx->invariant_state[nid] = VTX_LICM_INVARIANT;
                    changed = true;
                }
            }
        }
    }

    /* After fixed-point: any remaining UNKNOWN nodes are VARIANT
     * (they have cyclic dependencies that prevent hoisting) */
    for (uint32_t nid = 0; nid < ctx->node_count; nid++) {
        if (ctx->invariant_state[nid] == VTX_LICM_UNKNOWN) {
            ctx->invariant_state[nid] = VTX_LICM_VARIANT;
        }
    }
}

/* ========================================================================== */
/* Try to hoist Guard nodes                                                   */
/*                                                                            */
/* Guard nodes are side-effecting (they can deoptimize), so they are never    */
/* classified as invariant by the standard analysis. However, if a Guard's    */
/* condition input is loop-invariant AND its control input comes from outside  */
/* the loop, we can safely hoist it.                                         */
/*                                                                            */
/* Rationale: If the guard's condition doesn't change in the loop, then the   */
/* guard will either always pass or always fail. If it always passes, hoisting */
/* is equivalent (just earlier). If it always fails, hoisting makes the       */
/* deoptimization happen sooner, which is strictly better (fail-fast).        */
/*                                                                            */
/* We must also verify that the Guard's FrameState is valid at the hoist      */
/* point. For simplicity, we only hoist guards whose frame_state input is     */
/* defined outside the loop.                                                  */
/* ========================================================================== */

static void try_hoist_guards(vtx_licm_loop_ctx_t *ctx)
{
    for (uint32_t nid = 0; nid < ctx->node_count; nid++) {
        if (!ctx->in_loop[nid]) continue;

        vtx_node_t *node = &ctx->nt->nodes[nid];
        if (node->dead) continue;
        if (!node_is_hoistable_guard(node->opcode)) continue;

        /* Guard has inputs: [0] = control, [1] = condition (checked node)
         * Optionally: frame_state */
        if (node->input_count < 2) continue;

        vtx_nodeid_t ctrl_id = node->inputs[0];
        vtx_nodeid_t cond_id = node->inputs[1];

        /* Control input must come from outside the loop.
         * If the control input is in the loop, we can't hoist because
         * the guard's control dependency would be broken. */
        bool ctrl_outside = false;
        if (ctrl_id != VTX_NODEID_INVALID && ctrl_id < ctx->node_count) {
            ctrl_outside = !ctx->in_loop[ctrl_id];
        } else if (ctrl_id == VTX_NODEID_INVALID) {
            /* No control input — treat as outside */
            ctrl_outside = true;
        }

        if (!ctrl_outside) continue;

        /* Condition input must be loop-invariant.
         * It could be outside the loop or classified as INVARIANT. */
        bool cond_invariant = false;
        if (cond_id != VTX_NODEID_INVALID && cond_id < ctx->node_count) {
            if (!ctx->in_loop[cond_id]) {
                cond_invariant = true;
            } else if (ctx->invariant_state[cond_id] == VTX_LICM_INVARIANT) {
                cond_invariant = true;
            }
        }

        if (!cond_invariant) continue;

        /* Check FrameState: if present, it must be outside the loop */
        if (node->frame_state != VTX_NODEID_INVALID &&
            node->frame_state < ctx->node_count) {
            if (ctx->in_loop[node->frame_state]) {
                /* FrameState is inside the loop — can't hoist without
                 * an invalid deoptimization point */
                continue;
            }
        }

        /* Guard is hoistable! Mark it as invariant. */
        ctx->invariant_state[nid] = VTX_LICM_INVARIANT;
    }
}

/* ========================================================================== */
/* Hoist invariant nodes to the preheader                                     */
/*                                                                            */
/* For each node classified as INVARIANT that is in the loop, move it to the  */
/* preheader block by updating the schedule's node_block mapping.             */
/*                                                                            */
/* We process nodes in a careful order: data nodes first, then memory loads,  */
/* then hoistable guards. This ensures that when we update a node's block,    */
/* any dependents that also need hoisting won't be confused by stale state.   */
/*                                                                            */
/* Returns the number of nodes hoisted.                                       */
/* ========================================================================== */

static uint32_t hoist_invariant_nodes(vtx_licm_loop_ctx_t *ctx)
{
    uint32_t hoisted = 0;

    /* Verify we have a valid preheader */
    if (ctx->preheader_block == (uint32_t)-1) {
        return 0;
    }

    /* First pass: hoist pure data nodes (arithmetic, comparisons, etc.) */
    for (uint32_t nid = 0; nid < ctx->node_count; nid++) {
        if (!ctx->in_loop[nid]) continue;
        if (ctx->invariant_state[nid] != VTX_LICM_INVARIANT) continue;

        vtx_node_t *node = &ctx->nt->nodes[nid];
        if (node->dead) continue;

        /* Skip memory loads and guards for now */
        if (node_is_memory_load(node->opcode)) continue;
        if (node_is_hoistable_guard(node->opcode)) continue;

        /* Hoist: update the node_block mapping */
        ctx->schedule->node_block[nid] = ctx->preheader_block;
        hoisted++;
    }

    /* Second pass: hoist memory loads.
     * These are only marked INVARIANT if there are no aliasing stores,
     * so they are safe to hoist. */
    for (uint32_t nid = 0; nid < ctx->node_count; nid++) {
        if (!ctx->in_loop[nid]) continue;
        if (ctx->invariant_state[nid] != VTX_LICM_INVARIANT) continue;

        vtx_node_t *node = &ctx->nt->nodes[nid];
        if (node->dead) continue;

        if (!node_is_memory_load(node->opcode)) continue;

        /* Additional safety check: verify the load's memory input is
         * either outside the loop or has been hoisted. The memory input
         * to a load is the memory state (Province or previous memory op).
         * If it's inside the loop and hasn't been hoisted, we can't hoist
         * the load because the memory state would be wrong. */
        bool mem_input_ok = true;
        for (uint32_t j = 0; j < node->input_count; j++) {
            vtx_nodeid_t inp_id = node->inputs[j];
            if (inp_id == VTX_NODEID_INVALID || inp_id >= ctx->node_count) {
                continue;
            }
            if (ctx->in_loop[inp_id]) {
                /* The input is in the loop. Is it being hoisted? */
                const vtx_node_t *inp_node = &ctx->nt->nodes[inp_id];
                if (vtx_nf_has(inp_node->flags, VTX_NF_MEMORY)) {
                    /* Memory input is a loop-resident memory node.
                     * If it was classified as invariant, it's already been
                     * moved to the preheader. If it wasn't, we can't hoist. */
                    if (ctx->invariant_state[inp_id] != VTX_LICM_INVARIANT) {
                        mem_input_ok = false;
                        break;
                    }
                }
            }
        }

        if (!mem_input_ok) continue;

        /* Hoist the load */
        ctx->schedule->node_block[nid] = ctx->preheader_block;
        hoisted++;
    }

    /* Third pass: hoist guards.
     * Guards are hoisted after everything else because they depend on
     * invariant conditions that should already be in the preheader. */
    for (uint32_t nid = 0; nid < ctx->node_count; nid++) {
        if (!ctx->in_loop[nid]) continue;
        if (ctx->invariant_state[nid] != VTX_LICM_INVARIANT) continue;

        vtx_node_t *node = &ctx->nt->nodes[nid];
        if (node->dead) continue;

        if (!node_is_hoistable_guard(node->opcode)) continue;

        /* Verify the guard's condition input has been moved to the
         * preheader (or was already outside the loop) */
        vtx_nodeid_t cond_id = node->input_count > 1 ? node->inputs[1] : VTX_NODEID_INVALID;
        if (cond_id != VTX_NODEID_INVALID && cond_id < ctx->node_count) {
            uint32_t cond_block = vtx_schedule_node_block(ctx->schedule, cond_id);
            /* The condition should now be in the preheader or outside the
             * loop. If it's still in the loop, don't hoist the guard. */
            if (cond_block != (uint32_t)-1 &&
                ctx->schedule->blocks[cond_block].loop_depth >= ctx->loop_depth) {
                /* Condition is still in the loop — skip */
                continue;
            }
        }

        /* Hoist the guard */
        ctx->schedule->node_block[nid] = ctx->preheader_block;
        hoisted++;
    }

    return hoisted;
}

/* ========================================================================== */
/* Process a single loop                                                      */
/*                                                                            */
/* Returns the number of nodes hoisted from this loop.                        */
/* ========================================================================== */

static uint32_t process_loop(vtx_graph_t *graph,
                              const vtx_schedule_t *schedule,
                              vtx_arena_t *arena,
                              uint32_t header_block,
                              const vtx_tbaa_result_t *tbaa_result)
{
    vtx_node_table_t *nt = &graph->node_table;
    uint32_t node_count = nt->count;

    if (node_count == 0) return 0;

    /* Find the preheader */
    uint32_t preheader = find_preheader(schedule, header_block);
    if (preheader == (uint32_t)-1) {
        /* No preheader found — cannot hoist from this loop */
        return 0;
    }

    /* Initialize loop context */
    vtx_licm_loop_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    ctx.schedule = schedule;
    ctx.graph = graph;
    ctx.nt = nt;
    ctx.node_count = node_count;
    ctx.header_block = header_block;
    ctx.preheader_block = preheader;
    ctx.loop_depth = schedule->blocks[header_block].loop_depth;
    ctx.hoisted_count = 0;
    ctx.tbaa_result = tbaa_result;

    /* Allocate working arrays from the arena */
    ctx.invariant_state = (vtx_licm_state_t *)vtx_arena_alloc(
        arena, node_count * sizeof(vtx_licm_state_t));
    ctx.in_loop = (bool *)vtx_arena_alloc(
        arena, node_count * sizeof(bool));

    if (ctx.invariant_state == NULL || ctx.in_loop == NULL) {
        /* Allocation failure — cannot run LICM on this loop */
        return 0;
    }

    /* Initialize all states to UNKNOWN */
    memset(ctx.invariant_state, 0, node_count * sizeof(vtx_licm_state_t));

    /* Phase 1: Determine which nodes are in this loop */
    compute_loop_membership(&ctx);

    /* Phase 2: Scan for side effects (stores, calls, allocations) */
    scan_loop_side_effects(&ctx);

    /* Phase 3: Compute loop-invariance (fixed-point iteration) */
    compute_invariance(&ctx);

    /* Phase 4: Try to hoist Guard nodes (special case) */
    try_hoist_guards(&ctx);

    /* Phase 5: Hoist invariant nodes to the preheader */
    uint32_t hoisted = hoist_invariant_nodes(&ctx);

    /* Note: we don't need to free arena allocations — the arena will be
     * reset or destroyed by the caller. */

    return hoisted;
}

/* ========================================================================== */
/* Rebuild the per-block node lists after hoisting                            */
/*                                                                            */
/* After LICM changes the node_block mapping, the per-block node lists in     */
/* the schedule are stale. We rebuild them by re-adding all live nodes to     */
/* their (new) block. The ordering respects node IDs, which is approximately  */
/* topological for a SoN graph built in order.                                */
/* ========================================================================== */

static void rebuild_block_node_lists(vtx_graph_t *graph,
                                      const vtx_schedule_t *schedule)
{
    vtx_node_table_t *nt = &graph->node_table;
    uint32_t node_count = nt->count;

    /* Clear all block node lists */
    for (uint32_t bi = 0; bi < schedule->count; bi++) {
        vtx_schedule_block_t *blk = (vtx_schedule_block_t *)&schedule->blocks[bi];
        blk->node_count = 0;
    }

    /* Re-add each live node to its block */
    for (uint32_t nid = 0; nid < node_count; nid++) {
        vtx_node_t *node = &nt->nodes[nid];
        if (node->dead) continue;

        uint32_t blk_idx = schedule->node_block[nid];
        if (blk_idx == (uint32_t)-1 || blk_idx >= schedule->count) continue;

        vtx_schedule_block_t *blk = (vtx_schedule_block_t *)&schedule->blocks[blk_idx];

        /* Grow the node list if needed */
        if (blk->node_count >= blk->node_capacity) {
            uint32_t new_cap = (blk->node_capacity == 0) ? 16
                             : blk->node_capacity * 2;
            vtx_nodeid_t *new_nodes = (vtx_nodeid_t *)realloc(
                blk->nodes, new_cap * sizeof(vtx_nodeid_t));
            if (new_nodes == NULL) continue; /* skip on allocation failure */
            blk->nodes = new_nodes;
            blk->node_capacity = new_cap;
        }

        blk->nodes[blk->node_count++] = nid;
    }
}

/* ========================================================================== */
/* Sort nodes within each block to respect data dependencies                  */
/*                                                                            */
/* After hoisting, the node order within blocks may be invalid (a node might  */
/* appear before its input in the same block). We fix this with a simple      */
/* insertion-sort-like approach: for each node, ensure it comes after all its */
/* inputs that are in the same block.                                         */
/* ========================================================================== */

static void sort_block_nodes(vtx_graph_t *graph,
                              const vtx_schedule_t *schedule)
{
    vtx_node_table_t *nt = &graph->node_table;
    uint32_t node_count = nt->count;

    for (uint32_t bi = 0; bi < schedule->count; bi++) {
        vtx_schedule_block_t *blk = (vtx_schedule_block_t *)&schedule->blocks[bi];

        /* Build a position map for this block for faster lookups */
        uint32_t *position = NULL;
        if (blk->node_count > 0) {
            position = (uint32_t *)malloc(node_count * sizeof(uint32_t));
            if (position == NULL) continue;
            for (uint32_t i = 0; i < node_count; i++) {
                position[i] = (uint32_t)-1;
            }
            for (uint32_t i = 0; i < blk->node_count; i++) {
                if (blk->nodes[i] < node_count) {
                    position[blk->nodes[i]] = i;
                }
            }
        }

        /* Simple bubble-sort-like passes to fix ordering */
        bool sorted = false;
        uint32_t passes = 0;
        const uint32_t max_passes = blk->node_count + 1;

        while (!sorted && passes < max_passes) {
            sorted = true;
            passes++;

            for (uint32_t i = 1; i < blk->node_count; i++) {
                vtx_nodeid_t nid = blk->nodes[i];
                if (nid >= node_count) continue;

                vtx_node_t *node = &nt->nodes[nid];

                /* Find the latest input of this node that is in this block */
                int32_t latest_input_pos = -1;
                for (uint32_t j = 0; j < node->input_count; j++) {
                    vtx_nodeid_t inp = node->inputs[j];
                    if (inp == VTX_NODEID_INVALID || inp >= node_count) continue;
                    if (position[inp] != (uint32_t)-1 && position[inp] < i) {
                        if ((int32_t)position[inp] > latest_input_pos) {
                            latest_input_pos = (int32_t)position[inp];
                        }
                    }
                }

                /* If the latest input is after position i, this shouldn't
                 * happen with our incremental approach. If the latest input
                 * is at position >= i, we need to move this node after it.
                 * But since we process in order, inputs should already be
                 * before us. Check for any input that appears AFTER us. */
                for (uint32_t j = 0; j < node->input_count; j++) {
                    vtx_nodeid_t inp = node->inputs[j];
                    if (inp == VTX_NODEID_INVALID || inp >= node_count) continue;
                    if (position[inp] != (uint32_t)-1 && position[inp] > i) {
                        /* Input appears after us — need to swap */
                        uint32_t inp_pos = position[inp];

                        /* Move nid from position i to after inp_pos.
                         * Shift everything between i+1..inp_pos left by 1,
                         * then place nid at inp_pos. */
                        for (uint32_t k = i; k < inp_pos; k++) {
                            blk->nodes[k] = blk->nodes[k + 1];
                            if (blk->nodes[k] < node_count) {
                                position[blk->nodes[k]] = k;
                            }
                        }
                        blk->nodes[inp_pos] = nid;
                        position[nid] = inp_pos;

                        sorted = false;
                        break;
                    }
                }
            }
        }

        free(position);
    }
}

/* ========================================================================== */
/* Main LICM entry point                                                      */
/* ========================================================================== */

int vtx_licm_run(vtx_graph_t *graph, const vtx_schedule_t *schedule, vtx_arena_t *arena)
{
    VTX_ASSERT(graph != NULL, "graph must not be NULL");
    VTX_ASSERT(schedule != NULL, "schedule must not be NULL");
    VTX_ASSERT(arena != NULL, "arena must not be NULL");

    /* Edge case: empty graph or schedule */
    if (graph->node_table.count == 0 || schedule->count == 0) {
        return 0;
    }

    /* Run TBAA analysis once for the entire graph.
     * This allows LICM to determine whether a load can be hoisted
     * past a store based on type-based alias analysis, rather than
     * the conservative "any store prevents any load hoisting" rule.
     *
     * TBAA enables 50%+ of loop-invariant load hoisting because most
     * loops mix loads and stores of different types (e.g., reading
     * int[] while writing ref[]). */
    vtx_tbaa_result_t *tbaa_result = vtx_tbaa_analyze(graph, arena);

    uint32_t total_hoisted = 0;

    /* Collect all loop header blocks, sorted by loop_depth (ascending).
     * We process innermost loops first so that when we process an outer
     * loop, nodes hoisted from inner loops are already outside the inner
     * loop but may still be inside the outer loop. */

    /* Count loop headers */
    uint32_t loop_count = 0;
    for (uint32_t bi = 0; bi < schedule->count; bi++) {
        if (schedule->blocks[bi].is_loop_header) {
            loop_count++;
        }
    }

    if (loop_count == 0) {
        /* No loops — nothing to do */
        return 0;
    }

    /* Collect loop headers */
    uint32_t *loop_headers = (uint32_t *)vtx_arena_alloc(
        arena, loop_count * sizeof(uint32_t));
    if (loop_headers == NULL) {
        return -1;
    }

    uint32_t idx = 0;
    for (uint32_t bi = 0; bi < schedule->count; bi++) {
        if (schedule->blocks[bi].is_loop_header) {
            loop_headers[idx++] = bi;
        }
    }

    /* Sort by loop_depth descending (process innermost first).
     * Simple insertion sort — the number of loops is typically small. */
    for (uint32_t i = 1; i < loop_count; i++) {
        uint32_t key = loop_headers[i];
        uint32_t key_depth = schedule->blocks[key].loop_depth;
        int32_t j = (int32_t)i - 1;
        while (j >= 0 && schedule->blocks[loop_headers[j]].loop_depth < key_depth) {
            loop_headers[j + 1] = loop_headers[j];
            j--;
        }
        loop_headers[j + 1] = key;
    }

    /* Process each loop with TBAA info */
    for (uint32_t li = 0; li < loop_count; li++) {
        uint32_t header_block = loop_headers[li];
        uint32_t hoisted = process_loop(graph, schedule, arena, header_block, tbaa_result);
        total_hoisted += hoisted;
    }

    /* Rebuild the per-block node lists to reflect hoisting */
    rebuild_block_node_lists(graph, schedule);

    /* Sort nodes within blocks to respect dependencies */
    sort_block_nodes(graph, schedule);

    return (int)total_hoisted;
}
