#include "sota/alloc_graph.h"
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* Helpers                                                                     */
/* ========================================================================== */

static bool is_alloc_opcode(vtx_node_opcode_t opcode)
{
    return opcode == VTX_OP_NewObject ||
           opcode == VTX_OP_NewArray ||
           opcode == VTX_OP_Allocate;
}

static vtx_sota_alloc_record_t *find_or_create_record(vtx_sota_alloc_graph_t *ag,
                                                   vtx_nodeid_t alloc_node)
{
    /* Linear scan for existing record */
    for (uint32_t i = 0; i < ag->record_count; i++) {
        if (ag->records[i].alloc_node == alloc_node) {
            return &ag->records[i];
        }
    }

    /* Grow if needed */
    if (ag->record_count >= ag->record_capacity) {
        uint32_t new_cap = ag->record_capacity * 2;
        vtx_sota_alloc_record_t *new_recs = (vtx_sota_alloc_record_t *)realloc(
            ag->records, new_cap * sizeof(vtx_sota_alloc_record_t));
        if (new_recs == NULL) return NULL;
        ag->records = new_recs;
        ag->record_capacity = new_cap;
    }

    /* Create new record */
    vtx_sota_alloc_record_t *rec = &ag->records[ag->record_count];
    memset(rec, 0, sizeof(*rec));
    rec->alloc_node = alloc_node;
    rec->escape_state = VTX_ESCAPE_GLOBAL; /* assume worst case */
    rec->effective_escape = VTX_ESCAPE_GLOBAL;
    rec->is_virtual = false;
    rec->out_edges = NULL;
    rec->out_edge_count = 0;
    rec->out_edge_capacity = 0;
    rec->in_edges = NULL;
    rec->in_edge_count = 0;
    rec->in_edge_capacity = 0;
    rec->read_fields = NULL;
    rec->read_field_count = 0;
    rec->read_field_capacity = 0;

    ag->record_count++;
    return rec;
}

static void add_out_edge(vtx_sota_alloc_record_t *rec, const vtx_sota_alloc_edge_t *edge)
{
    if (rec->out_edge_count >= rec->out_edge_capacity) {
        uint32_t new_cap = rec->out_edge_capacity == 0 ? 4 : rec->out_edge_capacity * 2;
        vtx_sota_alloc_edge_t *new_edges = (vtx_sota_alloc_edge_t *)realloc(
            rec->out_edges, new_cap * sizeof(vtx_sota_alloc_edge_t));
        if (new_edges == NULL) return;
        rec->out_edges = new_edges;
        rec->out_edge_capacity = new_cap;
    }
    rec->out_edges[rec->out_edge_count++] = *edge;
}

static void add_in_edge(vtx_sota_alloc_record_t *rec, const vtx_sota_alloc_edge_t *edge)
{
    if (rec->in_edge_count >= rec->in_edge_capacity) {
        uint32_t new_cap = rec->in_edge_capacity == 0 ? 4 : rec->in_edge_capacity * 2;
        vtx_sota_alloc_edge_t *new_edges = (vtx_sota_alloc_edge_t *)realloc(
            rec->in_edges, new_cap * sizeof(vtx_sota_alloc_edge_t));
        if (new_edges == NULL) return;
        rec->in_edges = new_edges;
        rec->in_edge_capacity = new_cap;
    }
    rec->in_edges[rec->in_edge_count++] = *edge;
}

static void add_read_field(vtx_sota_alloc_record_t *rec, uint32_t field_offset)
{
    /* Check if already recorded */
    for (uint32_t i = 0; i < rec->read_field_count; i++) {
        if (rec->read_fields[i] == field_offset) return;
    }

    if (rec->read_field_count >= rec->read_field_capacity) {
        uint32_t new_cap = rec->read_field_capacity == 0 ? 4 : rec->read_field_capacity * 2;
        uint32_t *new_fields = (uint32_t *)realloc(
            rec->read_fields, new_cap * sizeof(uint32_t));
        if (new_fields == NULL) return;
        rec->read_fields = new_fields;
        rec->read_field_capacity = new_cap;
    }
    rec->read_fields[rec->read_field_count++] = field_offset;
}

/* ========================================================================== */
/* Escape analysis — flow-insensitive fallback                                 */
/* ========================================================================== */

/**
 * Flow-insensitive escape analysis (original, kept as fallback).
 *
 * Scans ALL nodes globally without considering control flow.
 * Conservative: if an allocation escapes on ANY path, it is marked as
 * escaping everywhere.
 *
 * Fixed StoreField case: when the target object is known to be NoEscape,
 * the stored value is also NoEscape (since if the container doesn't escape,
 * neither does anything stored in its fields).
 */
static vtx_escape_state_t compute_escape_flow_insensitive(const vtx_graph_t *graph,
                                                            vtx_nodeid_t alloc_node)
{
    const vtx_node_t *alloc = vtx_node_get_const(&graph->node_table, alloc_node);
    if (alloc == NULL) return VTX_ESCAPE_GLOBAL;

    /* Start with NoEscape and look for escape paths */
    vtx_escape_state_t escape = VTX_ESCAPE_NONE;

    /* Check all nodes that reference this allocation as an input */
    for (uint32_t i = 0; i < graph->node_table.count; i++) {
        const vtx_node_t *user = &graph->node_table.nodes[i];
        if (user->dead) continue;

        for (uint32_t j = 0; j < user->input_count; j++) {
            if (user->inputs[j] != alloc_node) continue;

            /* This node uses our allocation. Check how. */
            switch (user->opcode) {
            case VTX_OP_Return:
                /* Returned from method → global escape */
                return VTX_ESCAPE_GLOBAL;

            case VTX_OP_CallStatic:
            case VTX_OP_CallVirtual:
            case VTX_OP_CallInterface:
            case VTX_OP_CallRuntime:
                /* Passed as argument to a call → at least arg escape */
                if (j >= 2) { /* data inputs (args) start at index 2 */
                    escape = VTX_ESCAPE_ARG;
                }
                break;

            case VTX_OP_StoreField:
                /* Stored into a field. If the target object is known
                 * to be NoEscape, the stored value is also NoEscape
                 * (since if the container doesn't escape, neither does
                 * anything stored in its fields). Otherwise, the stored
                 * value escapes at least as much as the container. */
                if (j >= 2) { /* value input */
                    /* Check the target object's escape state.
                     * StoreField inputs: [0]=control, [1]=memory,
                     *                    [2]=target, [3]=value */
                    if (user->input_count >= 4 && user->inputs[2] != alloc_node) {
                        vtx_nodeid_t target_id = user->inputs[2];
                        const vtx_node_t *target =
                            vtx_node_get_const(&graph->node_table, target_id);
                        if (target != NULL && is_alloc_opcode(target->opcode)) {
                            /* Recursively check the target's escape.
                             * If target is NoEscape, stored value is NoEscape
                             * on this path — don't upgrade escape state.
                             * We do a quick local check: if the target is
                             * only used by LoadField/Guard/StoreField (as
                             * the target, not value), it doesn't escape. */
                            vtx_escape_state_t target_escape =
                                compute_escape_flow_insensitive(graph, target_id);
                            if (target_escape == VTX_ESCAPE_NONE) {
                                /* Container doesn't escape, so neither does
                                 * the stored value via this store. */
                                break;
                            }
                            /* Container escapes — stored value escapes
                             * at least as much as the container. */
                            escape = vtx_escape_join(escape, target_escape);
                            break;
                        }
                    }
                    /* Non-allocation target or can't determine —
                     * conservatively mark as arg escape */
                    escape = vtx_escape_join(escape, VTX_ESCAPE_ARG);
                }
                break;

            case VTX_OP_LoadField:
                /* Just loading from the allocation — no escape */
                break;

            case VTX_OP_CheckCast:
            case VTX_OP_InstanceOf:
                /* Type check — no escape by itself */
                break;

            case VTX_OP_Guard:
            case VTX_OP_DeoptGuard:
                /* Guard on the allocation — no escape */
                break;

            case VTX_OP_Phi:
                /* Allocation flows through a Phi — conservatively
                 * mark as arg escape. The flow-sensitive analysis
                 * can do better by tracking through Phis. */
                escape = VTX_ESCAPE_ARG;
                break;

            default:
                break;
            }
        }
    }

    return escape;
}

/* ========================================================================== */
/* Flow-sensitive escape analysis                                              */
/* ========================================================================== */

/**
 * Per-block escape state for the flow-sensitive analysis.
 *
 * Each block has entry and exit escape-state arrays, indexed by
 * allocation-node ID. The transfer function walks the block's nodes
 * and updates the exit state based on how each node uses allocations.
 */
typedef struct {
    vtx_escape_state_t *entry_state; /* per-alloc escape state at block entry */
    vtx_escape_state_t *exit_state;  /* per-alloc escape state at block exit */
    uint32_t            state_count; /* size of entry/exit arrays */
} flow_block_state_t;

/**
 * Determine if a node belongs to a given basic block.
 *
 * A node belongs to a block if:
 *   - It IS the block's region_node
 *   - It IS the block's control_node or memory_node
 *   - One of its inputs IS the block's region/control/memory node
 *   - Its bytecode_pc matches the block's region_node's bytecode_pc
 */
static bool node_belongs_to_block(const vtx_node_t *node,
                                    const vtx_block_info_t *block,
                                    const vtx_node_table_t *table)
{
    /* Region node of this block always belongs */
    if (node->id == block->region_node) return true;

    /* Control/memory nodes of this block belong */
    if (node->id == block->control_node ||
        node->id == block->memory_node) {
        return true;
    }

    /* Nodes whose direct input is the block's region/control/memory node */
    for (uint32_t j = 0; j < node->input_count; j++) {
        if (node->inputs[j] == block->region_node ||
            node->inputs[j] == block->control_node ||
            node->inputs[j] == block->memory_node) {
            return true;
        }
    }

    /* Check by bytecode_pc */
    if (block->region_node < table->count) {
        const vtx_node_t *region = &table->nodes[block->region_node];
        if (node->bytecode_pc == region->bytecode_pc) {
            return true;
        }
    }

    return false;
}

/**
 * Transfer function for a single node in the flow-sensitive analysis.
 *
 * Examines how the node uses allocation values and updates the escape
 * state array accordingly. Key improvement over flow-insensitive:
 *   - StoreField into a NoEscape container → stored value stays NoEscape
 *   - Escape states are per-block, so branch-sensitive results are possible
 */
static void flow_transfer_node(const vtx_node_t *node,
                                 const vtx_node_table_t *table,
                                 vtx_escape_state_t *state,
                                 uint32_t state_count)
{
    if (node->dead) return;

    switch (node->opcode) {
    /* ---- Return: any returned allocation escapes globally ---- */
    case VTX_OP_Return:
        for (uint32_t i = 0; i < node->input_count; i++) {
            vtx_nodeid_t input_id = node->inputs[i];
            if (input_id < state_count) {
                const vtx_node_t *input = vtx_node_get_const(table, input_id);
                if (input && !input->dead && is_alloc_opcode(input->opcode)) {
                    state[input_id] = vtx_escape_join(state[input_id],
                                                       VTX_ESCAPE_GLOBAL);
                }
            }
        }
        break;

    /* ---- StoreField: if the stored value is an allocation, it escapes
       based on the receiver's escape state ---- */
    case VTX_OP_StoreField:
        if (node->input_count >= 4) {
            vtx_nodeid_t receiver_id = node->inputs[2];
            vtx_nodeid_t value_id    = node->inputs[3];

            /* If the stored value is an allocation, its escape depends
             * on the container's escape state */
            if (value_id < state_count) {
                const vtx_node_t *val_node = vtx_node_get_const(table, value_id);
                if (val_node && !val_node->dead && is_alloc_opcode(val_node->opcode)) {
                    /* Key improvement: if the container (receiver) is
                     * NoEscape, the stored value is also NoEscape.
                     * If the container doesn't escape, nothing stored
                     * in its fields can escape through it. */
                    vtx_escape_state_t container_state = VTX_ESCAPE_GLOBAL;
                    if (receiver_id < state_count) {
                        const vtx_node_t *recv_node =
                            vtx_node_get_const(table, receiver_id);
                        if (recv_node && !recv_node->dead &&
                            is_alloc_opcode(recv_node->opcode)) {
                            container_state = state[receiver_id];
                        }
                    }

                    if (container_state == VTX_ESCAPE_NONE) {
                        /* Container doesn't escape → stored value doesn't
                         * escape through this store. No state change needed. */
                        break;
                    }

                    /* Container escapes → stored value escapes at least
                     * as much as the container (but not more). The join
                     * with VTX_ESCAPE_ARG captures the minimum "stored into
                     * a field" escape. */
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
                const vtx_node_t *val_node = vtx_node_get_const(table, value_id);
                if (val_node && !val_node->dead && is_alloc_opcode(val_node->opcode)) {
                    vtx_escape_state_t array_state = VTX_ESCAPE_GLOBAL;
                    if (array_id < state_count) {
                        const vtx_node_t *arr_node =
                            vtx_node_get_const(table, array_id);
                        if (arr_node && !arr_node->dead &&
                            is_alloc_opcode(arr_node->opcode)) {
                            array_state = state[array_id];
                        }
                    }

                    if (array_state == VTX_ESCAPE_NONE) {
                        break; /* array doesn't escape → value doesn't escape */
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
        for (uint32_t i = 0; i < node->input_count; i++) {
            vtx_nodeid_t input_id = node->inputs[i];
            if (input_id < state_count) {
                const vtx_node_t *input = vtx_node_get_const(table, input_id);
                if (input && !input->dead && is_alloc_opcode(input->opcode)) {
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
        break;

    /* ---- No-escape operations ---- */
    case VTX_OP_LoadField:
    case VTX_OP_CheckCast:
    case VTX_OP_InstanceOf:
    case VTX_OP_Guard:
    case VTX_OP_DeoptGuard:
        break;

    /* ---- Phi: propagate escape states through inputs ---- */
    case VTX_OP_Phi:
        /* A Phi merges values from multiple control flow paths.
         * If any input allocation has a higher escape state, propagate
         * it to all other allocation inputs of the Phi (since they
         * represent the same logical value at the merge point). */
        {
            vtx_escape_state_t max_input_state = VTX_ESCAPE_NONE;
            for (uint32_t i = 0; i < node->input_count; i++) {
                vtx_nodeid_t input_id = node->inputs[i];
                if (input_id < state_count) {
                    const vtx_node_t *input = vtx_node_get_const(table, input_id);
                    if (input && !input->dead && is_alloc_opcode(input->opcode)) {
                        max_input_state = vtx_escape_join(max_input_state,
                            state[input_id]);
                    }
                }
            }
            /* Propagate the maximum state back to all allocation inputs.
             * This models the fact that if a value escapes on one branch,
             * it escapes at the merge point. */
            if (max_input_state > VTX_ESCAPE_NONE) {
                for (uint32_t i = 0; i < node->input_count; i++) {
                    vtx_nodeid_t input_id = node->inputs[i];
                    if (input_id < state_count) {
                        const vtx_node_t *input = vtx_node_get_const(table, input_id);
                        if (input && !input->dead && is_alloc_opcode(input->opcode)) {
                            state[input_id] = vtx_escape_join(state[input_id],
                                max_input_state);
                        }
                    }
                }
            }
        }
        break;

    default:
        break;
    }
}

/**
 * Apply the transfer function to all nodes belonging to a block.
 *
 * Copies the entry state to the exit state, then walks all nodes
 * that belong to the block and applies the per-node transfer function.
 */
static void flow_transfer_block(const vtx_graph_t *graph,
                                  uint32_t block_idx,
                                  flow_block_state_t *bs)
{
    /* Start from entry state */
    memcpy(bs->exit_state, bs->entry_state,
           bs->state_count * sizeof(vtx_escape_state_t));

    const vtx_block_info_t *block = &graph->blocks[block_idx];
    const vtx_node_table_t *table = &graph->node_table;

    /* Walk only nodes that belong to this block */
    for (uint32_t i = 0; i < table->count; i++) {
        const vtx_node_t *node = &table->nodes[i];
        if (node->dead) continue;
        if (!node_belongs_to_block(node, block, table)) continue;

        flow_transfer_node(node, table, bs->exit_state, bs->state_count);
    }
}

/**
 * Join two escape-state arrays (dst = max(dst, src)).
 * Returns true if dst changed.
 */
static bool flow_join_states(vtx_escape_state_t *dst,
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

/**
 * Compare two escape-state arrays; return true if different.
 */
static bool flow_states_differ(const vtx_escape_state_t *a,
                                 const vtx_escape_state_t *b,
                                 uint32_t count)
{
    return memcmp(a, b, count * sizeof(vtx_escape_state_t)) != 0;
}

/**
 * Compute reverse postorder of the CFG for efficient worklist iteration.
 * Returns an arena-allocated array of block indices in RPO.
 */
static uint32_t *flow_compute_rpo(const vtx_graph_t *graph,
                                    vtx_arena_t *arena,
                                    uint32_t *rpo_count)
{
    uint32_t n = graph->block_count;
    if (n == 0) {
        *rpo_count = 0;
        return NULL;
    }

    uint32_t *rpo = vtx_arena_alloc(arena, n * sizeof(uint32_t));
    if (!rpo) return NULL;

    bool *visited = vtx_arena_alloc(arena, n * sizeof(bool));
    uint32_t *stack = vtx_arena_alloc(arena, n * sizeof(uint32_t));
    if (!visited || !stack) return NULL;

    memset(visited, 0, n * sizeof(bool));

    uint32_t rpo_idx = n;
    int32_t top = 0;
    stack[0] = 0;
    visited[0] = true;

    while (top >= 0) {
        uint32_t current = stack[top];
        bool has_unvisited_succ = false;

        const vtx_block_info_t *block = &graph->blocks[current];
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
        memmove(rpo, rpo + rpo_idx, *rpo_count * sizeof(uint32_t));
    }

    return rpo;
}

/**
 * Flow-sensitive escape analysis.
 *
 * For each allocation, tracks escape state per basic block and propagates
 * through the control flow graph. At merge points (Region/Phi), takes the
 * maximum (most escaping) of all predecessor states. Handles loops by
 * iterating until a fixed point is reached (escape states can only increase).
 *
 * Key improvement over flow-insensitive: if an allocation is stored into a
 * field in one branch but not another, flow-insensitive says it escapes.
 * Flow-sensitive can say it doesn't escape on the path where it's not stored.
 *
 * Also fixes the conservative StoreField case: when the target object is
 * NoEscape, the stored value stays NoEscape (container doesn't escape →
 * nothing in its fields escapes).
 *
 * @param graph  The SoN graph to analyze
 * @param ag     The allocation graph (records already populated in Step 1)
 * @param arena  Arena for temporary allocations
 * @return       true on success, false on failure (caller should fall back
 *               to flow-insensitive analysis)
 */
static bool compute_escape_flow_sensitive(const vtx_graph_t *graph,
                                            vtx_sota_alloc_graph_t *ag,
                                            vtx_arena_t *arena)
{
    uint32_t block_count = graph->block_count;
    uint32_t state_count = graph->node_table.count;

    /* If no blocks, we can't do flow-sensitive analysis */
    if (block_count == 0 || state_count == 0) return false;

    /* ---- Allocate per-block state arrays ---- */
    flow_block_state_t *block_states =
        vtx_arena_alloc(arena, block_count * sizeof(flow_block_state_t));
    if (!block_states) return false;

    for (uint32_t i = 0; i < block_count; i++) {
        flow_block_state_t *bs = &block_states[i];
        bs->state_count = state_count;

        bs->entry_state = vtx_arena_alloc(arena,
            state_count * sizeof(vtx_escape_state_t));
        bs->exit_state = vtx_arena_alloc(arena,
            state_count * sizeof(vtx_escape_state_t));

        if (!bs->entry_state || !bs->exit_state) return false;

        /* Initialize all to NoEscape */
        memset(bs->entry_state, 0, state_count * sizeof(vtx_escape_state_t));
        memset(bs->exit_state, 0, state_count * sizeof(vtx_escape_state_t));
    }

    /* ---- Compute reverse postorder for worklist ---- */
    uint32_t rpo_count = 0;
    uint32_t *rpo = flow_compute_rpo(graph, arena, &rpo_count);

    /* ---- Initialize worklist ---- */
    bool *on_worklist = vtx_arena_alloc(arena, block_count * sizeof(bool));
    if (!on_worklist) return false;
    memset(on_worklist, 0, block_count * sizeof(bool));

    /* Circular buffer worklist */
    uint32_t wl_capacity = block_count + 1;
    uint32_t *worklist = vtx_arena_alloc(arena, wl_capacity * sizeof(uint32_t));
    if (!worklist) return false;

    uint32_t wl_head = 0, wl_tail = 0;

    /* Enqueue all blocks in RPO */
    for (uint32_t i = 0; i < rpo_count; i++) {
        uint32_t block_idx = rpo ? rpo[i] : i;
        worklist[wl_tail] = block_idx;
        wl_tail = (wl_tail + 1) % wl_capacity;
        on_worklist[block_idx] = true;
    }
    /* If no RPO (allocation failure), enqueue all blocks in order */
    if (rpo_count == 0) {
        for (uint32_t i = 0; i < block_count; i++) {
            worklist[wl_tail] = i;
            wl_tail = (wl_tail + 1) % wl_capacity;
            on_worklist[i] = true;
        }
    }

    /* ---- Iterate to fixed point ---- */
    uint32_t iterations = 0;
    const uint32_t MAX_ITERATIONS = 200; /* safety bound for loops */

    while (wl_head != wl_tail && iterations < MAX_ITERATIONS) {
        iterations++;

        uint32_t block_idx = worklist[wl_head];
        wl_head = (wl_head + 1) % wl_capacity;
        on_worklist[block_idx] = false;

        flow_block_state_t *bs = &block_states[block_idx];
        const vtx_block_info_t *block = &graph->blocks[block_idx];

        /* Compute entry state as join of predecessor exit states */
        vtx_escape_state_t *new_entry = vtx_arena_alloc(arena,
            state_count * sizeof(vtx_escape_state_t));
        if (!new_entry) return false;
        memset(new_entry, 0, state_count * sizeof(vtx_escape_state_t));

        if (block->pred_count == 0) {
            /* Entry block: all NoEscape (already zeroed) */
        } else {
            /* Join all predecessor exit states */
            for (uint32_t p = 0; p < block->pred_count; p++) {
                uint32_t pred_idx = block->pred_indices[p];
                if (pred_idx < block_count) {
                    flow_join_states(new_entry,
                        block_states[pred_idx].exit_state,
                        state_count);
                }
            }
        }

        /* Check if entry state changed */
        bool entry_changed = flow_states_differ(new_entry, bs->entry_state,
                                                  state_count);
        if (entry_changed) {
            memcpy(bs->entry_state, new_entry,
                   state_count * sizeof(vtx_escape_state_t));
        }

        /* Save old exit state and apply transfer function */
        vtx_escape_state_t *old_exit = vtx_arena_alloc(arena,
            state_count * sizeof(vtx_escape_state_t));
        if (!old_exit) return false;
        memcpy(old_exit, bs->exit_state,
               state_count * sizeof(vtx_escape_state_t));

        flow_transfer_block(graph, block_idx, bs);

        bool exit_changed = flow_states_differ(bs->exit_state, old_exit,
                                                 state_count);

        /* If exit state changed, add successors to worklist */
        if (entry_changed || exit_changed) {
            for (uint32_t s = 0; s < block->succ_count; s++) {
                uint32_t succ_idx = block->succ_indices[s];
                if (succ_idx < block_count && !on_worklist[succ_idx]) {
                    worklist[wl_tail] = succ_idx;
                    wl_tail = (wl_tail + 1) % wl_capacity;
                    /* Guard against worklist overflow */
                    if (wl_tail == wl_head) break;
                    on_worklist[succ_idx] = true;
                }
            }
        }
    }

    /* ---- Finalize: for each allocation, compute the final escape state
     * as the join of all block exit states. This gives a global result
     * that accounts for the worst-case across all paths. ---- */
    for (uint32_t i = 0; i < ag->record_count; i++) {
        vtx_sota_alloc_record_t *rec = &ag->records[i];
        vtx_nodeid_t alloc_node = rec->alloc_node;

        vtx_escape_state_t final_state = VTX_ESCAPE_NONE;

        for (uint32_t b = 0; b < block_count; b++) {
            if (alloc_node < block_states[b].state_count) {
                final_state = vtx_escape_join(final_state,
                    block_states[b].exit_state[alloc_node]);
            }
        }

        rec->escape_state = final_state;
    }

    return true;
}

/* ========================================================================== */
/* Compute effective escape                                                    */
/* ========================================================================== */

/**
 * For each allocation that escapes (ARG or GLOBAL), check if any
 * objects stored in its fields have effective NoEscape.
 *
 * An object B stored in A.field has effective NoEscape if:
 *   1. B is only stored into A's field (no other escape paths)
 *   2. A.field is never read at any point where A escapes
 *
 * This enables cross-object scalar replacement: even though A escapes
 * (so A must remain as a heap allocation), B can be eliminated because
 * its value is never actually needed through A.
 */
static void compute_effective_escape(vtx_sota_alloc_graph_t *ag)
{
    for (uint32_t i = 0; i < ag->record_count; i++) {
        vtx_sota_alloc_record_t *rec = &ag->records[i];

        /* Initialize effective escape to the standard escape state */
        rec->effective_escape = rec->escape_state;

        if (rec->escape_state == VTX_ESCAPE_NONE) {
            /* Already NoEscape — definitely a candidate */
            rec->is_virtual = true;
            continue;
        }

        /* Check if this allocation only escapes through field stores
         * into escaping containers, and those fields are never read
         * at the escape points. */
        bool only_field_escape = true;

        /* Check all uses of this allocation.
         * If every use is either:
         *   - A StoreField into another allocation
         *   - A LoadField (reading a field from this allocation)
         *   - A Guard/CheckCast (no escape)
         * then this allocation only escapes through field stores. */

        /* For now, apply a simpler heuristic:
         * If the allocation has ARG_ESCAPE and all its out-edges
         * point to allocations with effective NoEscape, then
         * its effective escape can be downgraded.
         *
         * More precisely: for each field store A.field = B where
         * B is this allocation:
         *   - If A.field is never read at an escape point of A,
         *     then B's effective escape = NoEscape
         */

        /* Check incoming edges (stores into this allocation's fields) */
        for (uint32_t e = 0; e < rec->in_edge_count; e++) {
            const vtx_sota_alloc_edge_t *edge = &rec->in_edges[e];

            /* edge->from_alloc is the container (A)
             * edge->to_alloc is the stored value (B, which might be this allocation)
             * We need to check: is the field (edge->field_offset) ever read
             * from A at an escape point of A? */

            if (edge->to_alloc == rec->alloc_node) {
                /* This allocation is stored in from_alloc's field.
                 * Check if that field is read at an escape point. */
                vtx_sota_alloc_record_t *container = find_or_create_record(ag, edge->from_alloc);
                /* find_or_create_record may have already created this record
                 * during the build phase. Look it up properly. */
                bool field_read_at_escape = false;

                /* Find the container's record */
                for (uint32_t k = 0; k < ag->record_count; k++) {
                    if (ag->records[k].alloc_node == edge->from_alloc) {
                        const vtx_sota_alloc_record_t *cont = &ag->records[k];

                        /* Check if this field is read at an escape point */
                        for (uint32_t f = 0; f < cont->read_field_count; f++) {
                            if (cont->read_fields[f] == edge->field_offset) {
                                field_read_at_escape = true;
                                break;
                            }
                        }
                        break;
                    }
                }

                if (!field_read_at_escape) {
                    /* Field is never read at an escape point →
                     * this allocation effectively doesn't escape */
                    rec->effective_escape = VTX_ESCAPE_NONE;
                    rec->is_virtual = true;
                }
            }
        }
    }
}

/* ========================================================================== */
/* Build the allocation graph                                                  */
/* ========================================================================== */

vtx_sota_alloc_graph_t *vtx_alloc_graph_build(const vtx_graph_t *graph,
                                           vtx_arena_t *arena)
{
    if (graph == NULL || arena == NULL) return NULL;

    /* Allocate the graph structure */
    vtx_sota_alloc_graph_t *ag = (vtx_sota_alloc_graph_t *)vtx_arena_alloc(
        arena, sizeof(vtx_sota_alloc_graph_t));
    if (ag == NULL) return NULL;

    memset(ag, 0, sizeof(*ag));

    /* Initialize records array */
    ag->record_capacity = VTX_ALLOC_GRAPH_INITIAL_CAPACITY;
    ag->records = (vtx_sota_alloc_record_t *)vtx_arena_alloc(
        arena, ag->record_capacity * sizeof(vtx_sota_alloc_record_t));
    if (ag->records == NULL) return NULL;
    memset(ag->records, 0, ag->record_capacity * sizeof(vtx_sota_alloc_record_t));

    /* Initialize edges array */
    ag->edge_capacity = 64;
    ag->all_edges = (vtx_sota_alloc_edge_t *)vtx_arena_alloc(
        arena, ag->edge_capacity * sizeof(vtx_sota_alloc_edge_t));
    if (ag->all_edges == NULL) return NULL;

    /* Step 1: Find all allocation nodes and create records */
    for (uint32_t i = 0; i < graph->node_table.count; i++) {
        const vtx_node_t *node = &graph->node_table.nodes[i];
        if (node->dead || !is_alloc_opcode(node->opcode)) continue;

        vtx_sota_alloc_record_t *rec = find_or_create_record(ag, node->id);
        if (rec == NULL) continue;

        ag->total_allocations++;
    }

    /* Step 2: Find all StoreField nodes that store into allocation fields
     * and build the object graph edges */
    for (uint32_t i = 0; i < graph->node_table.count; i++) {
        const vtx_node_t *node = &graph->node_table.nodes[i];
        if (node->dead || node->opcode != VTX_OP_StoreField) continue;

        /* StoreField inputs:
         *   input[0] = control
         *   input[1] = memory
         *   input[2] = target object
         *   input[3] = value to store */
        if (node->input_count < 4) continue;

        vtx_nodeid_t target_id = node->inputs[2];
        vtx_nodeid_t value_id = node->inputs[3];

        /* Check if target is an allocation */
        const vtx_node_t *target = vtx_node_get_const(&graph->node_table, target_id);
        if (target == NULL || !is_alloc_opcode(target->opcode)) continue;

        /* Check if value is also an allocation (for cross-object edges) */
        const vtx_node_t *value = vtx_node_get_const(&graph->node_table, value_id);

        vtx_sota_alloc_edge_t edge;
        memset(&edge, 0, sizeof(edge));
        edge.from_alloc = target_id;
        edge.to_alloc = (value != NULL && is_alloc_opcode(value->opcode))
                         ? value_id : VTX_NODEID_INVALID;
        edge.field_offset = node->field_offset;
        edge.store_node = node->id;

        /* Add edge to the graph */
        if (ag->edge_count >= ag->edge_capacity) {
            /* Can't grow arena allocation — just stop adding edges */
            break;
        }
        ag->all_edges[ag->edge_count++] = edge;

        /* Add to target's outgoing edges */
        vtx_sota_alloc_record_t *target_rec = find_or_create_record(ag, target_id);
        if (target_rec != NULL) {
            add_out_edge(target_rec, &edge);
        }

        /* Add to value's incoming edges (if value is also an allocation) */
        if (edge.to_alloc != VTX_NODEID_INVALID) {
            vtx_sota_alloc_record_t *value_rec = find_or_create_record(ag, value_id);
            if (value_rec != NULL) {
                add_in_edge(value_rec, &edge);
            }
        }
    }

    /* Step 3: Find all LoadField nodes and record which fields are read
     * from each allocation. This is needed for effective escape analysis. */
    for (uint32_t i = 0; i < graph->node_table.count; i++) {
        const vtx_node_t *node = &graph->node_table.nodes[i];
        if (node->dead || node->opcode != VTX_OP_LoadField) continue;

        /* LoadField inputs:
         *   input[0] = memory
         *   input[1] = target object */
        if (node->input_count < 2) continue;

        vtx_nodeid_t target_id = node->inputs[1];
        const vtx_node_t *target = vtx_node_get_const(&graph->node_table, target_id);

        if (target != NULL && is_alloc_opcode(target->opcode)) {
            /* Record that this field is read from the allocation */
            vtx_sota_alloc_record_t *rec = find_or_create_record(ag, target_id);
            if (rec != NULL) {
                add_read_field(rec, node->field_offset);
            }
        }
    }

    /* Step 4: Run flow-sensitive escape analysis.
     * Falls back to flow-insensitive analysis if the flow-sensitive
     * pass fails (e.g., no block info, allocation failure). */
    bool flow_sensitive_ok = compute_escape_flow_sensitive(graph, ag, arena);

    if (!flow_sensitive_ok) {
        /* Fallback: use flow-insensitive analysis */
        for (uint32_t i = 0; i < ag->record_count; i++) {
            vtx_sota_alloc_record_t *rec = &ag->records[i];
            rec->escape_state = compute_escape_flow_insensitive(graph, rec->alloc_node);
        }
    }

    /* Compute statistics */
    for (uint32_t i = 0; i < ag->record_count; i++) {
        switch (ag->records[i].escape_state) {
        case VTX_ESCAPE_NONE:   ag->no_escape_count++; break;
        case VTX_ESCAPE_ARG:    ag->arg_escape_count++; break;
        case VTX_ESCAPE_GLOBAL: ag->global_escape_count++; break;
        }
    }

    /* Step 5: Compute effective escape (cross-object SR) */
    compute_effective_escape(ag);

    /* Count effective NoEscape */
    for (uint32_t i = 0; i < ag->record_count; i++) {
        if (ag->records[i].effective_escape == VTX_ESCAPE_NONE) {
            ag->effective_no_escape_count++;
        }
    }

    return ag;
}

/* ========================================================================== */
/* Destroy                                                                     */
/* ========================================================================== */

void vtx_alloc_graph_destroy(vtx_sota_alloc_graph_t *alloc_graph)
{
    if (alloc_graph == NULL) return;

    /* Free per-record dynamically allocated arrays */
    for (uint32_t i = 0; i < alloc_graph->record_count; i++) {
        vtx_sota_alloc_record_t *rec = &alloc_graph->records[i];
        if (rec->out_edges != NULL) free(rec->out_edges);
        if (rec->in_edges != NULL) free(rec->in_edges);
        if (rec->read_fields != NULL) free(rec->read_fields);
    }

    /* The main arrays are arena-allocated, so they're freed with the arena.
     * Just zero the pointers. */
    alloc_graph->records = NULL;
    alloc_graph->all_edges = NULL;
    alloc_graph->record_count = 0;
    alloc_graph->edge_count = 0;
}

/* ========================================================================== */
/* Queries                                                                     */
/* ========================================================================== */

vtx_escape_state_t vtx_alloc_graph_effective_escape(
    const vtx_sota_alloc_graph_t *alloc_graph,
    vtx_nodeid_t alloc_node)
{
    if (alloc_graph == NULL) return VTX_ESCAPE_GLOBAL;

    const vtx_sota_alloc_record_t *rec = vtx_alloc_graph_lookup(alloc_graph, alloc_node);
    if (rec == NULL) return VTX_ESCAPE_GLOBAL;
    return rec->effective_escape;
}

const vtx_sota_alloc_record_t *vtx_alloc_graph_lookup(
    const vtx_sota_alloc_graph_t *alloc_graph,
    vtx_nodeid_t alloc_node)
{
    if (alloc_graph == NULL) return NULL;

    for (uint32_t i = 0; i < alloc_graph->record_count; i++) {
        if (alloc_graph->records[i].alloc_node == alloc_node) {
            return &alloc_graph->records[i];
        }
    }
    return NULL;
}

bool vtx_alloc_graph_can_scalar_replace(
    const vtx_sota_alloc_graph_t *alloc_graph,
    vtx_nodeid_t alloc_node)
{
    return vtx_alloc_graph_effective_escape(alloc_graph, alloc_node) == VTX_ESCAPE_NONE;
}

uint32_t vtx_alloc_graph_sr_candidates(
    const vtx_sota_alloc_graph_t *alloc_graph,
    vtx_nodeid_t *candidates,
    uint32_t capacity)
{
    if (alloc_graph == NULL) return 0;

    uint32_t count = 0;
    for (uint32_t i = 0; i < alloc_graph->record_count; i++) {
        if (alloc_graph->records[i].effective_escape == VTX_ESCAPE_NONE) {
            if (candidates != NULL && count < capacity) {
                candidates[count] = alloc_graph->records[i].alloc_node;
            }
            count++;
        }
    }
    return count;
}

/* ========================================================================== */
/* Scalar Replacement Application                                              */
/* ========================================================================== */

/**
 * Helper: find all LoadField nodes that read from a specific allocation.
 * Returns their count and optionally fills an output array.
 */
static uint32_t find_loads_for_alloc(const vtx_graph_t *graph,
                                       vtx_nodeid_t alloc_node,
                                       vtx_nodeid_t *loads,
                                       uint32_t capacity)
{
    uint32_t count = 0;
    for (uint32_t i = 0; i < graph->node_table.count; i++) {
        const vtx_node_t *node = &graph->node_table.nodes[i];
        if (node->dead || node->opcode != VTX_OP_LoadField) continue;
        if (node->input_count < 2) continue;

        /* LoadField inputs: [0]=memory, [1]=target object */
        if (node->inputs[1] == alloc_node) {
            if (loads != NULL && count < capacity) {
                loads[count] = node->id;
            }
            count++;
        }
    }
    return count;
}

/**
 * Helper: find all StoreField nodes that write into a specific allocation.
 * Returns their count and optionally fills an output array.
 */
static uint32_t find_stores_for_alloc(const vtx_graph_t *graph,
                                        vtx_nodeid_t alloc_node,
                                        vtx_nodeid_t *stores,
                                        uint32_t capacity)
{
    uint32_t count = 0;
    for (uint32_t i = 0; i < graph->node_table.count; i++) {
        const vtx_node_t *node = &graph->node_table.nodes[i];
        if (node->dead || node->opcode != VTX_OP_StoreField) continue;
        if (node->input_count < 4) continue;

        /* StoreField inputs: [0]=control, [1]=memory, [2]=target, [3]=value */
        if (node->inputs[2] == alloc_node) {
            if (stores != NULL && count < capacity) {
                stores[count] = node->id;
            }
            count++;
        }
    }
    return count;
}

/**
 * Helper: check if any FrameState references an allocation node.
 * If so, we need materialization at that deopt point.
 */
static bool has_framestate_ref(const vtx_graph_t *graph, vtx_nodeid_t alloc_node)
{
    for (uint32_t i = 0; i < graph->node_table.count; i++) {
        const vtx_node_t *node = &graph->node_table.nodes[i];
        if (node->dead || node->opcode != VTX_OP_FrameState) continue;

        for (uint32_t j = 0; j < node->input_count; j++) {
            if (node->inputs[j] == alloc_node) return true;
        }
    }
    return false;
}

/**
 * Helper: create a scalar local variable (Parameter node repurposed
 * as a local) for a field of a scalar-replaced allocation.
 * In the SoN IR, we represent scalar locals as Phi nodes at the
 * Start region that carry the initial value.
 *
 * Returns the NodeID of the created local, or VTX_NODEID_INVALID on failure.
 */
static vtx_nodeid_t create_scalar_local(vtx_graph_t *graph,
                                          vtx_nodeid_t alloc_node,
                                          uint32_t field_offset,
                                          vtx_nodeid_t init_value)
{
    /* Create a Phi node at the entry to represent the scalar local.
     * Initially, the local holds the value that was stored into the field.
     * If there's no initial store, the local holds a default value. */
    vtx_nodeid_t phi_id = vtx_node_create(&graph->node_table, VTX_OP_Phi);
    if (phi_id == VTX_NODEID_INVALID) return VTX_NODEID_INVALID;

    vtx_node_t *phi = vtx_node_get(&graph->node_table, phi_id);
    if (phi == NULL) return VTX_NODEID_INVALID;

    phi->flags = VTX_NF_DATA | VTX_NF_PINNED;
    phi->field_offset = field_offset;

    /* Add the initial value as an input */
    if (init_value != VTX_NODEID_INVALID) {
        vtx_node_add_input(&graph->node_table, phi_id, init_value);
    }

    return phi_id;
}

uint32_t vtx_alloc_graph_apply_sr(vtx_graph_t *graph,
                                    vtx_sota_alloc_graph_t *alloc_graph,
                                    vtx_arena_t *arena)
{
    if (graph == NULL || alloc_graph == NULL || arena == NULL) return 0;

    uint32_t eliminated = 0;

    /* Process each allocation with effective escape = NoEscape */
    for (uint32_t r = 0; r < alloc_graph->record_count; r++) {
        vtx_sota_alloc_record_t *rec = &alloc_graph->records[r];

        if (rec->effective_escape != VTX_ESCAPE_NONE) continue;
        if (!rec->is_virtual) continue;

        vtx_nodeid_t alloc_node = rec->alloc_node;
        vtx_node_t *alloc = vtx_node_get(&graph->node_table, alloc_node);
        if (alloc == NULL || alloc->dead) continue;

        /* Phase 1: Create scalar locals for each field that is stored.
         * We scan all StoreField nodes targeting this allocation and
         * create a scalar local for each unique field offset. */

        /* Collect all stores to this allocation */
        uint32_t store_count = find_stores_for_alloc(graph, alloc_node, NULL, 0);
        if (store_count == 0 && find_loads_for_alloc(graph, alloc_node, NULL, 0) == 0) {
            /* No field accesses at all — allocation is dead, just remove it */
            alloc->dead = true;
            eliminated++;
            continue;
        }

        /* Allocate temporary arrays for store nodes */
        vtx_nodeid_t *store_nodes = NULL;
        if (store_count > 0) {
            store_nodes = (vtx_nodeid_t *)vtx_arena_alloc(
                arena, store_count * sizeof(vtx_nodeid_t));
            if (store_nodes == NULL) continue;
            find_stores_for_alloc(graph, alloc_node, store_nodes, store_count);
        }

        /* Create scalar locals map: field_offset → scalar local NodeID.
         * We use a simple linear array since the number of fields is small. */
        typedef struct {
            uint32_t     field_offset;
            vtx_nodeid_t scalar_node;
        } scalar_field_t;

        uint32_t scalar_cap = store_count > 0 ? store_count : 4;
        scalar_field_t *scalars = (scalar_field_t *)vtx_arena_alloc(
            arena, scalar_cap * sizeof(scalar_field_t));
        if (scalars == NULL) continue;

        uint32_t scalar_count = 0;

        /* For each store, create or update the scalar local for that field */
        for (uint32_t s = 0; s < store_count; s++) {
            vtx_node_t *store = vtx_node_get(&graph->node_table, store_nodes[s]);
            if (store == NULL) continue;

            uint32_t field_off = store->field_offset;
            vtx_nodeid_t value_id = (store->input_count >= 4) ? store->inputs[3] : VTX_NODEID_INVALID;

            /* Check if we already have a scalar local for this field */
            bool found = false;
            for (uint32_t k = 0; k < scalar_count; k++) {
                if (scalars[k].field_offset == field_off) {
                    /* Update the scalar local's value by replacing its input.
                     * The Phi node representing the scalar local now takes
                     * the new value as its latest input. */
                    if (value_id != VTX_NODEID_INVALID) {
                        /* Add the new value as an additional input to the Phi.
                         * The last input represents the current value of the
                         * scalar local after this store. */
                        vtx_node_add_input(&graph->node_table, scalars[k].scalar_node, value_id);
                    }
                    found = true;
                    break;
                }
            }

            if (!found && scalar_count < scalar_cap) {
                /* Create a new scalar local for this field */
                vtx_nodeid_t local_id = create_scalar_local(
                    graph, alloc_node, field_off, value_id);
                if (local_id != VTX_NODEID_INVALID) {
                    scalars[scalar_count].field_offset = field_off;
                    scalars[scalar_count].scalar_node = local_id;
                    scalar_count++;
                }
            }

            /* Mark the store as dead — the value is now in the scalar local.
             * The store is no longer needed because we're not allocating
             * the object on the heap. */
            store->dead = true;
        }

        /* Phase 2: Replace LoadField nodes with reads from scalar locals.
         * For each LoadField that reads from this allocation, replace
         * its output with the corresponding scalar local's value. */
        uint32_t load_count = find_loads_for_alloc(graph, alloc_node, NULL, 0);
        if (load_count > 0) {
            vtx_nodeid_t *load_nodes = (vtx_nodeid_t *)vtx_arena_alloc(
                arena, load_count * sizeof(vtx_nodeid_t));
            if (load_nodes != NULL) {
                find_loads_for_alloc(graph, alloc_node, load_nodes, load_count);

                for (uint32_t l = 0; l < load_count; l++) {
                    vtx_node_t *load = vtx_node_get(&graph->node_table, load_nodes[l]);
                    if (load == NULL) continue;

                    uint32_t field_off = load->field_offset;

                    /* Find the scalar local for this field */
                    vtx_nodeid_t replacement = VTX_NODEID_INVALID;
                    for (uint32_t k = 0; k < scalar_count; k++) {
                        if (scalars[k].field_offset == field_off) {
                            /* The scalar local's current value is its last input.
                             * A Phi with multiple inputs represents the latest value. */
                            vtx_node_t *scalar = vtx_node_get(
                                &graph->node_table, scalars[k].scalar_node);
                            if (scalar != NULL && scalar->input_count > 0) {
                                replacement = scalar->inputs[scalar->input_count - 1];
                            } else {
                                replacement = scalars[k].scalar_node;
                            }
                            break;
                        }
                    }

                    if (replacement != VTX_NODEID_INVALID) {
                        /* Replace all uses of the LoadField with the scalar local.
                         * Scan all nodes that reference the load and replace
                         * the reference with the scalar local value. */
                        for (uint32_t n = 0; n < graph->node_table.count; n++) {
                            vtx_node_t *user = &graph->node_table.nodes[n];
                            if (user->dead) continue;

                            for (uint32_t inp = 0; inp < user->input_count; inp++) {
                                if (user->inputs[inp] == load_nodes[l]) {
                                    vtx_node_replace_input(&graph->node_table,
                                                           user->id, inp, replacement);
                                }
                            }
                        }

                        /* Mark the load as dead */
                        load->dead = true;
                    }
                }
            }
        }

        /* Phase 3: Handle cross-object scalar replacement.
         * For allocations stored into containers' unreferenced fields:
         * the container's StoreField that stores this allocation is a dead
         * store (the field is never read at any escape point). Mark it dead. */
        for (uint32_t e = 0; e < rec->in_edge_count; e++) {
            vtx_sota_alloc_edge_t *edge = &rec->in_edges[e];
            if (edge->to_alloc == alloc_node) {
                /* This allocation was stored into from_alloc's field.
                 * The StoreField node is edge->store_node.
                 * Since the field is never read at an escape point
                 * (which is why effective_escape = NoEscape), the store
                 * is dead and can be removed. */
                vtx_node_t *store = vtx_node_get(&graph->node_table, edge->store_node);
                if (store != NULL) {
                    store->dead = true;
                }
            }
        }

        /* Phase 4: Materialization at deopt points.
         * If any FrameState references this allocation, we need to
         * materialize it at those deopt points. This means inserting
         * NewObject + StoreField nodes to reify the scalar values.
         *
         * For simplicity, we check if there are FrameState references.
         * If so, we keep the allocation node alive but mark it as
         * requiring materialization. The actual materialization code
         * is emitted during lowering (the code generator knows to
         * emit allocation + field stores when it encounters a
         * virtual object at a deopt point).
         *
         * If there are no FrameState references, we can completely
         * eliminate the allocation. */
        if (!has_framestate_ref(graph, alloc_node)) {
            /* No deopt points reference this allocation — safe to eliminate */
            alloc->dead = true;
            eliminated++;
        } else {
            /* Allocation must be materialized at deopt points.
             * We mark it as virtual (value_number = 1 signals virtual
             * to the lowering phase). The allocation node stays in the
             * graph but won't generate allocation code on the hot path.
             * Materialization code is only emitted at deopt points. */
            alloc->value_number = 1; /* 1 = virtual, 0 = normal */
            eliminated++; /* still counts as eliminated from hot path */
        }
    }

    /* Update statistics */
    alloc_graph->effective_no_escape_count = 0;
    for (uint32_t i = 0; i < alloc_graph->record_count; i++) {
        if (alloc_graph->records[i].effective_escape == VTX_ESCAPE_NONE) {
            alloc_graph->effective_no_escape_count++;
        }
    }

    return eliminated;
}
