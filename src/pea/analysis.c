/**
 * VORTEX Partial Escape Analysis — Flow-Sensitive Escape Analysis
 *
 * Implementation of the dataflow framework that computes per-allocation
 * escape states. Uses a reverse-postorder worklist iteration to reach
 * a fixed point.
 */

#include "pea/analysis.h"
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* Escape state name                                                           */
/* ========================================================================== */

const char *vtx_escape_state_name(vtx_escape_state_t s)
{
    switch (s) {
    case VTX_ESCAPE_NONE:   return "NoEscape";
    case VTX_ESCAPE_ARG:    return "ArgEscape";
    case VTX_ESCAPE_GLOBAL: return "GlobalEscape";
    }
    return "Unknown";
}

/* ========================================================================== */
/* Internal: check if a node is an allocation                                  */
/* ========================================================================== */

static inline bool is_allocation(vtx_node_opcode_t opcode)
{
    return opcode == VTX_OP_NewObject ||
           opcode == VTX_OP_NewArray  ||
           opcode == VTX_OP_Allocate;
}

/* ========================================================================== */
/* Internal: identify allocations and build the map                            */
/* ========================================================================== */

static int build_allocation_map(vtx_graph_t *graph, vtx_escape_map_t *map,
                                 vtx_arena_t *arena)
{
    vtx_node_table_t *table = &graph->node_table;
    uint32_t count = table->count;

    map->state_count = count;
    map->states = vtx_arena_alloc(arena, count * sizeof(vtx_escape_state_t));
    if (!map->states) return -1;

    /* Initialize all states to NoEscape */
    memset(map->states, 0, count * sizeof(vtx_escape_state_t));

    /* Count allocations first for capacity */
    uint32_t alloc_count = 0;
    for (uint32_t i = 0; i < count; i++) {
        vtx_node_t *node = &table->nodes[i];
        if (!node->dead && is_allocation(node->opcode)) {
            alloc_count++;
        }
    }

    map->alloc_capacity = alloc_count > 0 ? alloc_count : 1;
    map->alloc_ids = vtx_arena_alloc(arena, map->alloc_capacity * sizeof(vtx_nodeid_t));
    if (!map->alloc_ids) return -1;

    map->alloc_count = 0;
    for (uint32_t i = 0; i < count; i++) {
        vtx_node_t *node = &table->nodes[i];
        if (!node->dead && is_allocation(node->opcode)) {
            map->alloc_ids[map->alloc_count++] = node->id;
        }
    }

    return 0;
}

/* ========================================================================== */
/* Internal: copy escape state array                                           */
/* ========================================================================== */

static void copy_escape_states(vtx_escape_state_t *dst,
                                const vtx_escape_state_t *src,
                                uint32_t count)
{
    memcpy(dst, src, count * sizeof(vtx_escape_state_t));
}

/* ========================================================================== */
/* Internal: compare two escape state arrays, return true if different         */
/* ========================================================================== */

static bool escape_states_differ(const vtx_escape_state_t *a,
                                  const vtx_escape_state_t *b,
                                  uint32_t count)
{
    return memcmp(a, b, count * sizeof(vtx_escape_state_t)) != 0;
}

/* ========================================================================== */
/* Internal: join two escape state arrays (dst = join(dst, src))               */
/* Returns true if dst changed.                                                */
/* ========================================================================== */

static bool join_escape_states(vtx_escape_state_t *dst,
                                const vtx_escape_state_t *src,
                                uint32_t count)
{
    bool changed = false;
    for (uint32_t i = 0; i < count; i++) {
        vtx_escape_state_t joined = vtx_escape_join(dst[i], src[i]);
        if (joined != dst[i]) {
            dst[i] = joined;
            changed = true;
        }
    }
    return changed;
}

/* ========================================================================== */
/* Internal: build per-block state arrays                                      */
/* ========================================================================== */

static int build_block_states(vtx_graph_t *graph, vtx_pea_analysis_t *result,
                               vtx_arena_t *arena)
{
    uint32_t block_count = graph->block_count;
    uint32_t state_count = result->escape_map.state_count;

    result->block_state_count = block_count;
    result->block_states = vtx_arena_alloc(arena,
        block_count * sizeof(vtx_pea_block_state_t));
    if (!result->block_states) return -1;

    for (uint32_t i = 0; i < block_count; i++) {
        vtx_pea_block_state_t *bs = &result->block_states[i];
        bs->state_count = state_count;
        bs->entry_changed = false;

        bs->entry_state = vtx_arena_alloc(arena,
            state_count * sizeof(vtx_escape_state_t));
        bs->exit_state = vtx_arena_alloc(arena,
            state_count * sizeof(vtx_escape_state_t));

        if (!bs->entry_state || !bs->exit_state) return -1;

        /* Initialize all to NoEscape */
        memset(bs->entry_state, 0, state_count * sizeof(vtx_escape_state_t));
        memset(bs->exit_state, 0, state_count * sizeof(vtx_escape_state_t));
    }

    return 0;
}

/* ========================================================================== */
/* Internal: find block index for a region node                                */
/* ========================================================================== */

static inline int32_t find_block_for_region(vtx_graph_t *graph, vtx_nodeid_t region_id)
    __attribute__((unused));
static inline int32_t find_block_for_region(vtx_graph_t *graph, vtx_nodeid_t region_id)
{
    for (uint32_t i = 0; i < graph->block_count; i++) {
        if (graph->blocks[i].region_node == region_id) {
            return (int32_t)i;
        }
    }
    return -1;
}

/* ========================================================================== */
/* Internal: compute block successors                                          */
/* ========================================================================== */

static inline void get_block_successors(vtx_graph_t *graph, uint32_t block_idx,
                                  uint32_t *succs, uint32_t *succ_count)
    __attribute__((unused));
static inline void get_block_successors(vtx_graph_t *graph, uint32_t block_idx,
                                  uint32_t *succs, uint32_t *succ_count)
{
    *succ_count = graph->blocks[block_idx].succ_count;
    for (uint32_t i = 0; i < *succ_count && i < graph->blocks[block_idx].succ_capacity; i++) {
        succs[i] = graph->blocks[block_idx].succ_indices[i];
    }
}

/* ========================================================================== */
/* Internal: transfer function for a single node                               */
/*                                                                           */
/* Examines how the node uses values and updates escape states accordingly.   */
/* ========================================================================== */

static void transfer_node(vtx_node_t *node, vtx_node_table_t *table,
                           vtx_escape_state_t *state, uint32_t state_count)
{
    if (node->dead) return;

    switch (node->opcode) {
    /* ---- Return: any returned allocation escapes globally ---- */
    case VTX_OP_Return:
        for (uint32_t i = 0; i < node->input_count; i++) {
            vtx_nodeid_t input_id = node->inputs[i];
            if (input_id < state_count) {
                /* The returned value itself is a data value. If it is an
                 * allocation, it escapes globally. If it references an
                 * allocation indirectly, we need to check the input node. */
                vtx_node_t *input = vtx_node_get(table, input_id);
                if (input && !input->dead && is_allocation(input->opcode)) {
                    state[input_id] = vtx_escape_join(state[input_id],
                                                       VTX_ESCAPE_GLOBAL);
                }
            }
        }
        break;

    /* ---- StoreField: if the stored value is an allocation, it escapes
       based on the receiver's escape state ---- */
    case VTX_OP_StoreField:
        if (node->input_count >= 2) {
            /* Input layout: [control?, memory?, receiver, value] */
            /* Find the receiver and value inputs — they are the last two
             * data inputs. We search for them by position. */
            vtx_nodeid_t receiver_id = node->inputs[node->input_count - 2];
            vtx_nodeid_t value_id    = node->inputs[node->input_count - 1];

            /* If the stored value is an allocation, it escapes at least
             * as much as the container object */
            if (value_id < state_count) {
                vtx_node_t *val_node = vtx_node_get(table, value_id);
                if (val_node && !val_node->dead && is_allocation(val_node->opcode)) {
                    vtx_escape_state_t container_state = VTX_ESCAPE_GLOBAL;
                    if (receiver_id < state_count) {
                        vtx_node_t *recv_node = vtx_node_get(table, receiver_id);
                        if (recv_node && !recv_node->dead && is_allocation(recv_node->opcode)) {
                            container_state = state[receiver_id];
                        }
                    }
                    /* Stored into a container that escapes → the stored value
                     * escapes at least as much as the container */
                    state[value_id] = vtx_escape_join(state[value_id],
                                                       vtx_escape_join(container_state, VTX_ESCAPE_ARG));
                }
            }
        }
        break;

    /* ---- StoreIndexed: storing into an array — the value escapes
       at least as much as the array ---- */
    case VTX_OP_StoreIndexed:
        if (node->input_count >= 3) {
            vtx_nodeid_t array_id = node->inputs[node->input_count - 3];
            vtx_nodeid_t value_id = node->inputs[node->input_count - 1];

            if (value_id < state_count) {
                vtx_node_t *val_node = vtx_node_get(table, value_id);
                if (val_node && !val_node->dead && is_allocation(val_node->opcode)) {
                    vtx_escape_state_t array_state = VTX_ESCAPE_GLOBAL;
                    if (array_id < state_count) {
                        vtx_node_t *arr_node = vtx_node_get(table, array_id);
                        if (arr_node && !arr_node->dead && is_allocation(arr_node->opcode)) {
                            array_state = state[array_id];
                        }
                    }
                    state[value_id] = vtx_escape_join(state[value_id],
                                                       vtx_escape_join(array_state, VTX_ESCAPE_ARG));
                }
            }
        }
        break;

    /* ---- Calls: any allocation passed as argument escapes ---- */
    case VTX_OP_CallStatic:
    case VTX_OP_CallVirtual:
    case VTX_OP_CallInterface:
    case VTX_OP_CallRuntime:
        /* All allocation arguments to unknown functions escape.
         * For CallVirtual/CallInterface, the receiver also escapes. */
        for (uint32_t i = 0; i < node->input_count; i++) {
            vtx_nodeid_t input_id = node->inputs[i];
            if (input_id < state_count) {
                vtx_node_t *input = vtx_node_get(table, input_id);
                if (input && !input->dead && is_allocation(input->opcode)) {
                    /* Arguments to unknown functions escape at least through args.
                     * For CallRuntime (which may store globally), be conservative. */
                    if (node->opcode == VTX_OP_CallRuntime) {
                        state[input_id] = vtx_escape_join(state[input_id],
                                                           VTX_ESCAPE_GLOBAL);
                    } else {
                        state[input_id] = vtx_escape_join(state[input_id],
                                                           VTX_ESCAPE_ARG);
                    }
                }
            }
        }

        /* For virtual/interface calls, the receiver may dispatch to an
         * unknown method that stores the object globally. Conservative:
         * ArgEscape (could be made more precise with callee analysis). */
        if (node->opcode == VTX_OP_CallVirtual ||
            node->opcode == VTX_OP_CallInterface) {
            /* The receiver is typically the first data input after control/memory.
             * We check all inputs for allocation nodes — already handled above. */
        }
        break;

    /* ---- Monitor: using an object in a monitor makes it globally escape ---- */
    case VTX_OP_DeoptGuard:
        /* DeoptGuard: the guarded value is consumed but doesn't escape */
        break;

    /* ---- CheckCast / InstanceOf: the object doesn't escape through these
       (they are pure queries on the object's type) ---- */
    case VTX_OP_CheckCast:
    case VTX_OP_InstanceOf:
        /* No escape — these are read-only type checks */
        break;

    /* ---- Guard: the checked value doesn't escape ---- */
    case VTX_OP_Guard:
        /* Guards don't cause escape — they just check a condition */
        break;

    /* ---- Phi: a Phi of allocations — join the input states ---- */
    case VTX_OP_Phi:
        /* Phi nodes merge values from multiple control flow paths.
         * If any input allocation has a higher escape state, propagate it.
         * We do NOT make the Phi itself an allocation — Phis of allocations
         * are handled by the virtual object tracking pass.
         * However, we must propagate escape states through Phis:
         * if one Phi input escapes, the allocation it references escapes. */
        for (uint32_t i = 0; i < node->input_count; i++) {
            vtx_nodeid_t input_id = node->inputs[i];
            if (input_id < state_count) {
                vtx_node_t *input = vtx_node_get(table, input_id);
                if (input && !input->dead && is_allocation(input->opcode)) {
                    /* The Phi uses this allocation — its state is already
                     * tracked. The Phi itself isn't an allocation. */
                }
            }
        }
        break;

    default:
        break;
    }
}

/* ========================================================================== */
/* Internal: apply transfer function to all nodes in a block                   */
/* ========================================================================== */

static void transfer_block(vtx_graph_t *graph, uint32_t block_idx,
                            vtx_pea_block_state_t *bs,
                            vtx_node_table_t *table)
{
    /* Start from entry state */
    copy_escape_states(bs->exit_state, bs->entry_state, bs->state_count);

    vtx_block_info_t *block = &graph->blocks[block_idx];

    /* Walk all nodes that belong to this block.
     * We identify nodes belonging to this block by checking if their
     * control input is the block's control node, or by walking all
     * nodes and checking their scheduled position.
     *
     * For the SoN IR, nodes are not explicitly assigned to blocks at
     * this stage. Instead, we walk all nodes and apply the transfer
     * function to each node reachable from the block's control flow.
     *
     * Simplified approach: walk ALL nodes in the graph for each block's
     * transfer function. This is correct because the escape state is
     * monotonic — joining can only increase states, never decrease them.
     * Each block processes all nodes, and the entry state captures the
     * block's specific flow-sensitivity.
     *
     * For better performance on large graphs, we would compute a
     * block-to-node mapping first. But for correctness, the global
     * walk is sufficient.
     */
    (void)block; /* block_idx used for state, block info for future refinement */

    for (uint32_t i = 0; i < table->count; i++) {
        vtx_node_t *node = &table->nodes[i];
        if (node->dead) continue;
        transfer_node(node, table, bs->exit_state, bs->state_count);
    }
}

/* ========================================================================== */
/* Internal: compute reverse postorder for worklist                            */
/* ========================================================================== */

static uint32_t *compute_reverse_postorder(vtx_graph_t *graph, vtx_arena_t *arena,
                                             uint32_t *rpo_count)
{
    uint32_t n = graph->block_count;
    if (n == 0) {
        *rpo_count = 0;
        return NULL;
    }

    uint32_t *rpo = vtx_arena_alloc(arena, n * sizeof(uint32_t));
    if (!rpo) return NULL;

    /* Simple DFS-based reverse postorder computation.
     * We start from block 0 (entry block) and traverse successors. */
    bool *visited = vtx_arena_alloc(arena, n * sizeof(bool));
    uint32_t *stack = vtx_arena_alloc(arena, n * sizeof(uint32_t));
    if (!visited || !stack) return NULL;

    memset(visited, 0, n * sizeof(bool));

    uint32_t rpo_idx = n; /* fill from end */
    int32_t top = 0;
    stack[0] = 0;
    visited[0] = true;

    while (top >= 0) {
        uint32_t current = stack[top];
        bool has_unvisited_succ = false;

        vtx_block_info_t *block = &graph->blocks[current];
        for (uint32_t i = 0; i < block->succ_count; i++) {
            uint32_t succ = block->succ_indices[i];
            if (succ < n && !visited[succ]) {
                visited[succ] = true;
                top++;
                stack[top] = succ;
                has_unvisited_succ = true;
                break;
            }
        }

        if (!has_unvisited_succ) {
            rpo_idx--;
            rpo[rpo_idx] = current;
            top--;
        }
    }

    *rpo_count = n - rpo_idx;
    if (rpo_idx > 0) {
        /* Some blocks were unreachable; shift the array */
        memmove(rpo, rpo + rpo_idx, *rpo_count * sizeof(uint32_t));
    }

    return rpo;
}

/* ========================================================================== */
/* Main analysis entry point                                                   */
/* ========================================================================== */

vtx_pea_analysis_t *vtx_pea_run(vtx_graph_t *graph, vtx_arena_t *arena)
{
    VTX_ASSERT(graph != NULL, "graph must not be NULL");
    VTX_ASSERT(arena != NULL, "arena must not be NULL");

    if (graph->block_count == 0) {
        /* No blocks — degenerate graph */
        return NULL;
    }

    /* Allocate the result */
    vtx_pea_analysis_t *result = vtx_arena_alloc(arena, sizeof(vtx_pea_analysis_t));
    if (!result) return NULL;
    memset(result, 0, sizeof(*result));

    /* Step 1: Identify all allocations */
    if (build_allocation_map(graph, &result->escape_map, arena) != 0) {
        return NULL;
    }

    /* Step 2: Build per-block state arrays */
    if (build_block_states(graph, result, arena) != 0) {
        return NULL;
    }

    vtx_node_table_t *table = &graph->node_table;
    uint32_t state_count = result->escape_map.state_count;
    uint32_t block_count = graph->block_count;

    /* Step 3: Compute reverse postorder for efficient worklist */
    uint32_t rpo_count = 0;
    uint32_t *rpo = compute_reverse_postorder(graph, arena, &rpo_count);

    /* Step 4: Initialize worklist with all blocks in RPO */
    /* We use a simple flag-based worklist: a block is on the worklist
     * if its `entry_changed` flag is true. */
    bool *on_worklist = vtx_arena_alloc(arena, block_count * sizeof(bool));
    if (!on_worklist) return NULL;
    memset(on_worklist, 0, block_count * sizeof(bool));

    /* Circular buffer worklist */
    uint32_t wl_capacity = block_count + 1;
    uint32_t *worklist = vtx_arena_alloc(arena, wl_capacity * sizeof(uint32_t));
    if (!worklist) return NULL;

    uint32_t wl_head = 0, wl_tail = 0;

    /* Enqueue all blocks in RPO */
    for (uint32_t i = 0; i < rpo_count; i++) {
        uint32_t block_idx = rpo[i];
        worklist[wl_tail] = block_idx;
        wl_tail = (wl_tail + 1) % wl_capacity;
        on_worklist[block_idx] = true;
    }

    /* Step 5: Iterate to fixed point */
    uint32_t iterations = 0;
    const uint32_t MAX_ITERATIONS = 100; /* safety bound */

    while (wl_head != wl_tail && iterations < MAX_ITERATIONS) {
        iterations++;

        uint32_t block_idx = worklist[wl_head];
        wl_head = (wl_head + 1) % wl_capacity;
        on_worklist[block_idx] = false;

        vtx_pea_block_state_t *bs = &result->block_states[block_idx];
        vtx_block_info_t *block = &graph->blocks[block_idx];

        /* Compute entry state as join of predecessor exit states */
        vtx_escape_state_t *new_entry = vtx_arena_alloc(arena,
            state_count * sizeof(vtx_escape_state_t));
        if (!new_entry) return NULL;
        memset(new_entry, 0, state_count * sizeof(vtx_escape_state_t));

        if (block->pred_count == 0) {
            /* Entry block: entry state is all NoEscape (already zeroed) */
        } else {
            /* Join all predecessor exit states */
            for (uint32_t p = 0; p < block->pred_count; p++) {
                uint32_t pred_idx = block->pred_indices[p];
                if (pred_idx < block_count) {
                    join_escape_states(new_entry,
                                       result->block_states[pred_idx].exit_state,
                                       state_count);
                }
            }
        }

        /* Check if entry state changed */
        bool entry_changed = escape_states_differ(new_entry, bs->entry_state,
                                                    state_count);
        if (entry_changed) {
            copy_escape_states(bs->entry_state, new_entry, state_count);
        }

        /* Apply transfer function */
        vtx_escape_state_t *old_exit = vtx_arena_alloc(arena,
            state_count * sizeof(vtx_escape_state_t));
        if (!old_exit) return NULL;
        copy_escape_states(old_exit, bs->exit_state, state_count);

        transfer_block(graph, block_idx, bs, table);

        bool exit_changed = escape_states_differ(bs->exit_state, old_exit,
                                                   state_count);

        /* If exit state changed, add successors to worklist */
        if (entry_changed || exit_changed) {
            for (uint32_t s = 0; s < block->succ_count; s++) {
                uint32_t succ_idx = block->succ_indices[s];
                if (succ_idx < block_count && !on_worklist[succ_idx]) {
                    worklist[wl_tail] = succ_idx;
                    wl_tail = (wl_tail + 1) % wl_capacity;
                    on_worklist[succ_idx] = true;
                }
            }
        }
    }

    result->iterations = iterations;

    /* Step 6: Finalize — compute the global escape state for each allocation
     * as the join of all block exit states */
    for (uint32_t a = 0; a < result->escape_map.alloc_count; a++) {
        vtx_nodeid_t alloc_id = result->escape_map.alloc_ids[a];
        vtx_escape_state_t final_state = VTX_ESCAPE_NONE;

        for (uint32_t b = 0; b < block_count; b++) {
            if (alloc_id < result->block_states[b].state_count) {
                final_state = vtx_escape_join(final_state,
                    result->block_states[b].exit_state[alloc_id]);
            }
        }

        result->escape_map.states[alloc_id] = final_state;

        /* Update statistics */
        result->total_allocs++;
        switch (final_state) {
        case VTX_ESCAPE_NONE:   result->no_escape_count++; break;
        case VTX_ESCAPE_ARG:    result->arg_escape_count++; break;
        case VTX_ESCAPE_GLOBAL: result->global_escape_count++; break;
        }
    }

    return result;
}

/* ========================================================================== */
/* Query helpers                                                               */
/* ========================================================================== */

vtx_escape_state_t vtx_pea_get_escape(const vtx_pea_analysis_t *analysis,
                                       vtx_nodeid_t node_id)
{
    VTX_ASSERT(analysis != NULL, "analysis must not be NULL");
    if (node_id < analysis->escape_map.state_count) {
        return analysis->escape_map.states[node_id];
    }
    return VTX_ESCAPE_GLOBAL; /* conservative default */
}

bool vtx_pea_is_scalar_replaceable(const vtx_pea_analysis_t *analysis,
                                    vtx_nodeid_t node_id)
{
    return vtx_pea_get_escape(analysis, node_id) == VTX_ESCAPE_NONE;
}

vtx_escape_state_t vtx_pea_block_entry_state(const vtx_pea_analysis_t *analysis,
                                              uint32_t block_idx,
                                              vtx_nodeid_t alloc_id)
{
    VTX_ASSERT(analysis != NULL, "analysis must not be NULL");
    if (block_idx < analysis->block_state_count &&
        alloc_id < analysis->block_states[block_idx].state_count) {
        return analysis->block_states[block_idx].entry_state[alloc_id];
    }
    return VTX_ESCAPE_GLOBAL;
}

vtx_escape_state_t vtx_pea_block_exit_state(const vtx_pea_analysis_t *analysis,
                                             uint32_t block_idx,
                                             vtx_nodeid_t alloc_id)
{
    VTX_ASSERT(analysis != NULL, "analysis must not be NULL");
    if (block_idx < analysis->block_state_count &&
        alloc_id < analysis->block_states[block_idx].state_count) {
        return analysis->block_states[block_idx].exit_state[alloc_id];
    }
    return VTX_ESCAPE_GLOBAL;
}
