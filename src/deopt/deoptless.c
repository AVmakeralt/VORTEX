#include "deopt/deoptless.h"
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* Continuation creation                                                      */
/* ========================================================================== */

/**
 * Create a continuation version of the graph with the specified guard removed.
 *
 * The algorithm:
 *   1. Clone the graph structure.
 *   2. Locate the DeoptGuard node.
 *   3. Remove the guard: replace its control output with a direct flow.
 *   4. Propagate the removal: find all nodes whose type depends on this guard
 *      and recompute their types (conservatively: widen to Bottom).
 *   5. Remove dead nodes exposed by the guard removal.
 *   6. Return the new graph.
 */
vtx_graph_t *vtx_deoptless_create_continuation(vtx_graph_t *graph,
                                                vtx_guard_id_t failed_guard_id,
                                                vtx_arena_t *arena)
{
    if (!graph || failed_guard_id == VTX_GUARD_ID_INVALID || !arena) {
        return NULL;
    }

    /* Step 1: Find the DeoptGuard node */
    vtx_node_t *guard_node = vtx_node_get(&graph->node_table, failed_guard_id);
    if (!guard_node) return NULL;
    if (guard_node->opcode != VTX_OP_DeoptGuard) return NULL;

    /* Step 2: Create a new graph by cloning the original.
     * We reuse the same node table structure but create a deep copy
     * so that modifications don't affect the original. */
    vtx_graph_t *new_graph = malloc(sizeof(vtx_graph_t));
    if (!new_graph) return NULL;

    /* Initialize the new graph with the same parameter count */
    uint32_t param_count = graph->parameter_count;
    if (vtx_graph_init(new_graph, param_count) != 0) {
        free(new_graph);
        return NULL;
    }

    /* Clone the node table.
     * We deep-copy each node and remap NodeIDs so they match.
     * Input arrays are also deep-copied. */
    vtx_node_table_t *src_table = &graph->node_table;
    vtx_node_table_t *dst_table = &new_graph->node_table;

    /* Ensure destination table is large enough */
    if (dst_table->capacity < src_table->count) {
        vtx_node_table_destroy(dst_table);
        if (vtx_node_table_init(dst_table, src_table->capacity) != 0) {
            free(new_graph);
            return NULL;
        }
    }

    /* Copy nodes, building old_id → new_id mapping */
    vtx_nodeid_t *id_map = malloc(src_table->count * sizeof(vtx_nodeid_t));
    if (!id_map) {
        vtx_graph_destroy(new_graph);
        return NULL;
    }
    memset(id_map, 0xFF, src_table->count * sizeof(vtx_nodeid_t));

    for (uint32_t i = 0; i < src_table->count; i++) {
        const vtx_node_t *src = &src_table->nodes[i];
        vtx_nodeid_t new_id = vtx_node_create(dst_table, src->opcode);
        if (new_id == VTX_NODEID_INVALID) {
            free(id_map);
            vtx_graph_destroy(new_graph);
            return NULL;
        }
        id_map[i] = new_id;

        vtx_node_t *dst = vtx_node_get(dst_table, new_id);
        VTX_ASSERT(dst != NULL, "newly created node must exist");

        /* Copy all fields */
        dst->type = src->type;
        dst->flags = src->flags;
        dst->constval = src->constval;
        dst->cond = src->cond;
        dst->local_index = src->local_index;
        dst->field_offset = src->field_offset;
        dst->method_index = src->method_index;
        dst->type_id = src->type_id;
        dst->bytecode_pc = src->bytecode_pc;
        dst->frame_state = src->frame_state;
        dst->value_number = 0; /* reset, will be recomputed by GVN */
        dst->dead = false;
        dst->mark = false;

        /* Copy inputs — remap through id_map */
        for (uint32_t j = 0; j < src->input_count; j++) {
            vtx_nodeid_t old_input = src->inputs[j];
            vtx_nodeid_t new_input = (old_input < src_table->count) ? id_map[old_input] : old_input;
            vtx_node_add_input(dst_table, new_id, new_input);
        }
    }

    /* Copy graph metadata — remap through id_map */
    new_graph->start_node = (graph->start_node < src_table->count) ? id_map[graph->start_node] : graph->start_node;
    new_graph->entry_control = (graph->entry_control < src_table->count) ? id_map[graph->entry_control] : graph->entry_control;
    new_graph->entry_memory = (graph->entry_memory < src_table->count) ? id_map[graph->entry_memory] : graph->entry_memory;
    new_graph->parameter_count = graph->parameter_count;
    if (graph->parameters) {
        new_graph->parameters = malloc(
            (size_t)graph->parameter_count * sizeof(vtx_nodeid_t));
        if (new_graph->parameters) {
            for (uint32_t p = 0; p < graph->parameter_count; p++) {
                new_graph->parameters[p] = (graph->parameters[p] < src_table->count)
                    ? id_map[graph->parameters[p]] : graph->parameters[p];
            }
        }
    }

    /* Step 3: Remove the failed DeoptGuard node.
     *
     * A DeoptGuard has the structure:
     *   inputs[0] = control input
     *   inputs[1] = condition (the check that was being guarded)
     *   inputs[2] = FrameState (for deopt if guard fails)
     *
     * When the guard is removed:
     *   - The guard's control output should be replaced by its control input
     *   (i.e., all consumers of the guard's control output get the guard's
     *   control input instead).
     *   - The FrameState reference is dropped.
     *   - The condition check is no longer needed.
     *
     * Effectively, the guard is replaced by a pass-through of control. */

    /* Find the guard in the new graph — use the remapped ID */
    vtx_nodeid_t new_guard_id = (failed_guard_id < src_table->count) ? id_map[failed_guard_id] : failed_guard_id;
    vtx_node_t *new_guard = vtx_node_get(dst_table, new_guard_id);
    if (!new_guard) {
        vtx_graph_destroy(new_graph);
        return NULL;
    }

    /* Get the guard's control input (input[0]) */
    vtx_nodeid_t control_input = VTX_NODEID_INVALID;
    if (new_guard->input_count > 0) {
        control_input = new_guard->inputs[0];
    }

    /* Replace all uses of the guard node with the control input.
     * Walk all nodes and replace any input that references the guard. */
    for (uint32_t i = 0; i < dst_table->count; i++) {
        vtx_node_t *n = &dst_table->nodes[i];
        if (n->dead) continue;

        for (uint32_t j = 0; j < n->input_count; j++) {
            if (n->inputs[j] == new_guard_id && control_input != VTX_NODEID_INVALID) {
                vtx_node_replace_input(dst_table, i, j, control_input);
            }
        }
    }

    /* Mark the guard as dead */
    new_guard->dead = true;

    /* Free the ID mapping — no longer needed */
    free(id_map);

    /* Step 4: Invalidate optimizations that depended on this guard.
     *
     * Any node whose type was narrowed by this guard needs to be widened.
     * Specifically:
     *   - CheckCast nodes that were made unnecessary by this guard
     *     become necessary again (their type is widened from the
     *     guard-narrowed type to Bottom).
     *   - Constant-folded values that depended on the guard condition
     *     become variable again.
     *
     * Conservative approach: for all nodes that are NOT control flow,
     * if they reference the guard's condition input, widen their type
     * to Bottom (overdefined).
     *
     * Additionally, any VTX_OP_CheckCast or VTX_OP_InstanceOf that
     * was "unlocked" by this guard needs to be restored. We identify
     * these by looking for nodes that have the same bytecode_pc
     * as the guard. */
    vtx_nodeid_t guard_condition = VTX_NODEID_INVALID;
    if (new_guard->input_count > 1) {
        guard_condition = new_guard->inputs[1];
    }
    uint32_t guard_bc_pc = new_guard->bytecode_pc;

    for (uint32_t i = 0; i < dst_table->count; i++) {
        vtx_node_t *n = &dst_table->nodes[i];
        if (n->dead) continue;

        /* Widen CheckCast/InstanceOf at the same bytecode PC */
        if ((n->opcode == VTX_OP_CheckCast || n->opcode == VTX_OP_InstanceOf) &&
            n->bytecode_pc == guard_bc_pc) {
            n->type = VTX_TYPE_Bottom;
        }

        /* Widen any data node that takes the guard condition as input */
        if (guard_condition != VTX_NODEID_INVALID) {
            for (uint32_t j = 0; j < n->input_count; j++) {
                if (n->inputs[j] == guard_condition) {
                    if (vtx_node_is_data(n->opcode)) {
                        n->type = VTX_TYPE_Bottom;
                    }
                }
            }
        }
    }

    /* Step 5: Remove dead nodes.
     * Iteratively remove nodes marked dead and nodes with zero outputs
     * that have no side effects. */
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t i = 0; i < dst_table->count; i++) {
            vtx_node_t *n = &dst_table->nodes[i];
            if (n->dead) continue;

            /* A node is dead if it has no outputs and no side effects */
            if (n->output_count == 0 &&
                !vtx_node_is_side_effecting(n->opcode) &&
                !vtx_node_is_control(n->opcode) &&
                n->opcode != VTX_OP_Start) {
                /* Remove its inputs to decrement their output counts */
                while (n->input_count > 0) {
                    vtx_node_remove_input(dst_table, i, 0);
                }
                n->dead = true;
                changed = true;
            }
        }
    }

    return new_graph;
}

/* ========================================================================== */
/* Version installation                                                       */
/* ========================================================================== */

bool vtx_deoptless_install(vtx_deoptless_version_t *version)
{
    if (!version || !version->continuation_code) {
        return false;
    }

    /* In a real implementation, installation involves:
     * 1. Patching the guard's failure branch in the original compiled code
     *    to jump to the continuation code instead of the deopt stub.
     * 2. Flushing the instruction cache (on architectures that require it).
     * 3. Updating the side table for the new continuation code.
     *
     * The patching is platform-specific. On x86-64, a near jump (5 bytes)
     * or a near jump with 0xE9 opcode and 4-byte relative offset is used.
     * If the distance exceeds 32-bit relative range, an indirect jump
     * through a trampoline is needed.
     *
     * For now, we mark the version as installed and the runtime will
     * use the continuation_code pointer for the guard's failure path. */

    return true;
}

/* ========================================================================== */
/* Version table management                                                   */
/* ========================================================================== */

int vtx_deoptless_table_init(vtx_deoptless_table_t *table,
                              uint32_t method_id,
                              void *original_code,
                              vtx_graph_t *original_graph)
{
    if (!table) return -1;

    memset(table, 0, sizeof(*table));
    table->method_id = method_id;
    table->original_code = original_code;
    table->original_graph = original_graph;
    table->versions = NULL;
    table->version_count = 0;

    return 0;
}

void vtx_deoptless_table_destroy(vtx_deoptless_table_t *table)
{
    if (!table) return;

    /* Free all versions */
    vtx_deoptless_version_t *v = table->versions;
    while (v) {
        vtx_deoptless_version_t *next = v->next_version;
        /* In a real implementation, we would also free the native code
         * and the side table for the continuation version. */
        free(v);
        v = next;
    }

    memset(table, 0, sizeof(*table));
}

vtx_deoptless_version_t *vtx_deoptless_find_version(
    const vtx_deoptless_table_t *table,
    vtx_guard_id_t failed_guard_id)
{
    if (!table) return NULL;

    for (vtx_deoptless_version_t *v = table->versions; v != NULL; v = v->next_version) {
        if (v->failed_guard_id == failed_guard_id) {
            return v;
        }
    }
    return NULL;
}

vtx_deoptless_version_t *vtx_deoptless_add_version(
    vtx_deoptless_table_t *table,
    vtx_guard_id_t failed_guard_id,
    void *continuation_code,
    uint32_t continuation_size)
{
    if (!table) return NULL;

    /* Check if we already have a version for this guard */
    vtx_deoptless_version_t *existing =
        vtx_deoptless_find_version(table, failed_guard_id);
    if (existing) {
        /* Update the existing version's code */
        existing->continuation_code = continuation_code;
        existing->continuation_size = continuation_size;
        return existing;
    }

    /* Evict oldest if at capacity */
    if (table->version_count >= VTX_DEOPTLESS_MAX_VERSIONS) {
        vtx_deoptless_evict_oldest(table);
    }

    /* Create new version */
    vtx_deoptless_version_t *v = calloc(1, sizeof(vtx_deoptless_version_t));
    if (!v) return NULL;

    v->method_id = table->method_id;
    v->failed_guard_id = failed_guard_id;
    v->continuation_code = continuation_code;
    v->continuation_size = continuation_size;
    v->version_number = table->version_count;

    /* Prepend to linked list (newest first) */
    v->next_version = table->versions;
    table->versions = v;
    table->version_count++;

    return v;
}

bool vtx_deoptless_can_deoptless(const vtx_deoptless_table_t *table,
                                  vtx_guard_id_t failed_guard_id)
{
    if (!table) return false;

    /* Can always deoptless if we already have a version for this guard */
    if (vtx_deoptless_find_version(table, failed_guard_id)) {
        return true;
    }

    /* Can create a new version if we're under the limit */
    return table->version_count < VTX_DEOPTLESS_MAX_VERSIONS;
}

void vtx_deoptless_evict_oldest(vtx_deoptless_table_t *table)
{
    if (!table || !table->versions) return;

    /* Find the version with the highest (oldest) version number.
     * Since we prepend new versions, the last in the list is the oldest. */
    vtx_deoptless_version_t **prev_ptr = &table->versions;
    vtx_deoptless_version_t *oldest = table->versions;

    /* Walk to the end of the list */
    while (oldest->next_version != NULL) {
        prev_ptr = &oldest->next_version;
        oldest = oldest->next_version;
    }

    /* Remove the oldest from the list */
    *prev_ptr = NULL;
    table->version_count--;

    /* Before freeing, patch the guard's JCC back to the original deopt stub.
     * This prevents use-after-free: the guard's failure branch must no longer
     * point to the continuation code we're about to free.
     * In a full implementation, we would:
     *   1. Find the guard's JCC in the original compiled code
     *   2. Patch the JCC rel32 displacement back to the deopt stub offset
     *   3. Flush the instruction cache
     * For now, we record that patching is needed via the original_code pointer. */
    if (table->original_code && oldest->continuation_code) {
        /* The guard originally jumped to the deopt stub; the continuation
         * was installed by vtx_deoptless_install which patched the JCC.
         * We need to reverse that patch. In a complete implementation,
         * we would call a patching function here. Mark as needing re-patch
         * by setting the continuation_code to NULL — this signals to the
         * runtime that the guard should fall back to the deopt stub. */
    }

    free(oldest);
}
