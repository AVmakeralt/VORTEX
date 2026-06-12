#include "deopt/deoptless.h"
#include "interp/type_feedback.h"
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

    /* Patch the guard's JCC in the original compiled code to point to
     * the continuation code instead of the deopt stub.
     *
     * On x86-64, a near JCC has format: 0F 8x [4-byte rel32]
     * The displacement is relative to the end of the JCC instruction,
     * i.e., relative to (displacement_position + 4).
     *
     * We need:
     *   - code_start:            base address of the original compiled code
     *   - guard_branch_offset:   offset from code_start to the 4-byte rel32
     *   - continuation_code:     target address to jump to on guard failure
     *
     * New displacement = target_addr - (disp_pos + 4)
     *                  = continuation_code - (code_start + guard_branch_offset + 4)
     */

    if (!version->code_start || version->guard_branch_offset == 0) {
        /* No patch site recorded — cannot patch. The runtime must fall
         * back to checking for a deoptless version before entering the
         * deopt stub. */
        return true;
    }

    /* Compute the address of the 4-byte displacement in the JCC */
    uint8_t *disp_pos = version->code_start + version->guard_branch_offset;

    /* D2 fix: Save the original JCC displacement BEFORE patching.
     * This is critical for eviction: when the version is evicted, we
     * must restore the JCC to point back to the deopt stub. Without
     * saving the original displacement, we'd leave a dangling pointer
     * to freed memory (use-after-free). */
    volatile int32_t *read_addr = (volatile int32_t *)disp_pos;
    version->original_jcc_disp = *read_addr;

    /* Compute the new relative displacement */
    intptr_t target = (intptr_t)version->continuation_code;
    intptr_t branch_end = (intptr_t)(disp_pos + 4);
    int32_t new_disp = (int32_t)(target - branch_end);

    /* Check that the displacement fits in 32 bits (near JCC range).
     * If the continuation code is too far away, we cannot patch with
     * a near JCC and must fall back to the runtime check approach. */
    intptr_t raw_disp = target - branch_end;
    if (raw_disp != (intptr_t)(int32_t)raw_disp) {
        /* Displacement out of range for near JCC — cannot patch */
        return true;
    }

    /* Patch the 4 bytes with the new displacement.
     * We use a volatile pointer to prevent the compiler from
     * reordering or optimizing away the write. */
    volatile int32_t *patch_addr = (volatile int32_t *)disp_pos;
    *patch_addr = new_disp;

    /* Mark as patched so eviction knows to restore */
    version->is_patched = true;

    /* Flush the instruction cache: on x86-64, coherent caches mean we
     * only need a compiler barrier to ensure the write is visible.
     * On other architectures, __builtin___clear_cache would be needed.
     * We use both for portability and correctness. */
    __asm__ __volatile__("" : : : "memory");
#if defined(__GNUC__) && !defined(__x86_64__)
    __builtin___clear_cache((char *)disp_pos, (char *)(disp_pos + 4));
#endif

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

    /* Free failed guard tracking array (Proposal #3) */
    free(table->failed_guards);

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

    /* Store graph and parent for incremental continuation (Proposal #3) */
    v->graph = NULL;  /* graph is set separately after compilation */
    v->parent_version = table->versions;  /* current newest is our parent */

    /* D2 eviction fix: initialize JCC patch tracking */
    v->original_jcc_disp = 0;
    v->is_patched = false;

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

    /* D2 fix: Unpatch the guard's JCC back to the original deopt stub.
     *
     * When a deoptless version was installed, we patched the guard's JCC
     * to point to the continuation code instead of the deopt stub. Now
     * that we're evicting the version, we must restore the original JCC
     * displacement so the guard branches back to the deopt stub. Without
     * this, the JCC would point to freed memory (use-after-free).
     *
     * The original displacement was saved in vtx_deoptless_install().
     * We restore it here, then flush the instruction cache.
     */
    if (oldest->is_patched && oldest->code_start != NULL &&
        oldest->guard_branch_offset != 0) {
        uint8_t *disp_pos = oldest->code_start + oldest->guard_branch_offset;
        volatile int32_t *patch_addr = (volatile int32_t *)disp_pos;
        *patch_addr = oldest->original_jcc_disp;

        /* Flush instruction cache after restoring */
        __asm__ __volatile__("" : : : "memory");
#if defined(__GNUC__) && !defined(__x86_64__)
        __builtin___clear_cache((char *)disp_pos, (char *)(disp_pos + 4));
#endif
        oldest->is_patched = false;
    }

    oldest->continuation_code = NULL;
    oldest->continuation_size = 0;

    /* Destroy the version's graph snapshot if present (Proposal #3) */
    if (oldest->graph != NULL) {
        vtx_graph_destroy(oldest->graph);
        oldest->graph = NULL;
    }

    free(oldest);
}

/* ========================================================================== */
/* Incremental deoptless continuations (Proposal #3)                            */
/* ========================================================================== */

vtx_deoptless_version_t *vtx_deoptless_find_latest_version(
    const vtx_deoptless_table_t *table)
{
    if (!table || !table->versions) return NULL;

    /* The versions list is prepend-only (newest first).
     * Walk to find the first version that has a graph. */
    for (vtx_deoptless_version_t *v = table->versions; v != NULL; v = v->next_version) {
        if (v->graph != NULL) return v;
    }
    return NULL;
}

bool vtx_deoptless_is_guard_removed(const vtx_deoptless_table_t *table,
                                      vtx_guard_id_t guard_id)
{
    if (!table) return false;

    for (uint32_t i = 0; i < table->failed_guard_count; i++) {
        if (table->failed_guards[i] == guard_id) return true;
    }
    return false;
}

int vtx_deoptless_record_failed_guard(vtx_deoptless_table_t *table,
                                        vtx_guard_id_t guard_id)
{
    if (!table) return -1;

    /* Check if already recorded */
    if (vtx_deoptless_is_guard_removed(table, guard_id)) return 0;

    /* Grow the array if needed */
    if (table->failed_guard_count >= table->failed_guard_capacity) {
        uint32_t new_cap = table->failed_guard_capacity == 0 ? 16 : table->failed_guard_capacity * 2;
        vtx_guard_id_t *new_arr = (vtx_guard_id_t *)realloc(
            table->failed_guards, new_cap * sizeof(vtx_guard_id_t));
        if (!new_arr) return -1;
        table->failed_guards = new_arr;
        table->failed_guard_capacity = new_cap;
    }

    table->failed_guards[table->failed_guard_count++] = guard_id;
    return 0;
}

vtx_graph_t *vtx_deoptless_create_incremental_continuation(
    vtx_deoptless_table_t *table,
    vtx_guard_id_t failed_guard_id,
    vtx_arena_t *arena)
{
    if (!table || failed_guard_id == VTX_GUARD_ID_INVALID || !arena) {
        return NULL;
    }

    /* If this guard has already been removed, find the existing version */
    if (vtx_deoptless_is_guard_removed(table, failed_guard_id)) {
        /* The guard is already removed in some version — find and return
         * that version's graph (or NULL if the graph is gone). */
        for (vtx_deoptless_version_t *v = table->versions; v != NULL; v = v->next_version) {
            if (v->failed_guard_id == failed_guard_id && v->graph != NULL) {
                /* This guard already removed — caller should use existing version */
                return NULL;
            }
        }
    }

    /* Find the base graph for incremental continuation.
     * Use the latest version's graph, falling back to the original. */
    vtx_graph_t *base_graph = NULL;
    vtx_deoptless_version_t *latest = vtx_deoptless_find_latest_version(table);
    if (latest != NULL && latest->graph != NULL) {
        base_graph = latest->graph;
    } else if (table->original_graph != NULL) {
        base_graph = table->original_graph;
    }

    if (base_graph == NULL) return NULL;

    /* Create the continuation by removing one guard from the base graph.
     * Reuse the existing vtx_deoptless_create_continuation() function
     * which already handles graph cloning and guard removal. */
    vtx_graph_t *new_graph = vtx_deoptless_create_continuation(
        base_graph, failed_guard_id, arena);

    /* Record the failed guard */
    vtx_deoptless_record_failed_guard(table, failed_guard_id);

    return new_graph;
}

/* ========================================================================== */
/* Profile-guarded specialization (Proposal #13)                                 */
/* ========================================================================== */

uint64_t vtx_deoptless_compute_decision_hash(const vtx_graph_t *graph)
{
    if (!graph) return 0;

    /* FNV-1a hash over all DeoptGuard nodes' assumptions.
     * Each DeoptGuard encodes a speculation decision:
     *   - The guard's bytecode_pc (which call site it protects)
     *   - The guard's type_id (which type was assumed)
     *   - The guard's cond (what comparison is made) */
    uint64_t h = 14695981039346656037ULL;

    for (uint32_t i = 0; i < graph->node_table.count; i++) {
        const vtx_node_t *node = &graph->node_table.nodes[i];
        if (node->dead) continue;
        if (node->opcode != VTX_OP_DeoptGuard) continue;

        /* Mix the guard's identity into the hash */
        uint32_t pc = node->bytecode_pc;
        uint32_t tid = node->type_id;
        uint32_t cond = (uint32_t)node->cond;

        h ^= (uint64_t)(pc & 0xFF);
        h *= 1099511628211ULL;
        h ^= (uint64_t)((pc >> 8) & 0xFF);
        h *= 1099511628211ULL;
        h ^= (uint64_t)(tid & 0xFF);
        h *= 1099511628211ULL;
        h ^= (uint64_t)(cond & 0xFF);
        h *= 1099511628211ULL;
    }

    return h;
}

bool vtx_deoptless_needs_recompilation(const vtx_deoptless_table_t *table,
                                         uint64_t current_hash)
{
    if (!table) return true;  /* no table → must compile */
    if (table->compiled_profile_hash == 0) return true;  /* never compiled → must compile */
    return table->compiled_profile_hash != current_hash;
}

uint64_t vtx_profile_compute_hash(const vtx_type_feedback_t *type_feedback,
                                    uint32_t method_id)
{
    if (!type_feedback) return 0;

    /* FNV-1a hash over the type feedback data for the given method.
     * We hash the dominant type at each call site and the stable shapes
     * at each field site. This captures the information that affects
     * speculation decisions. */
    uint64_t h = 14695981039346656037ULL;

    /* Hash call site type signatures */
    uint32_t site_count = type_feedback->call_site_count;
    if (method_id < site_count) {
        const vtx_tf_call_site_t *site = &type_feedback->call_sites[method_id];
        /* Hash the stable signature if available */
        if (site->stable_signature.slot_count > 0) {
            for (uint32_t i = 0; i < site->stable_signature.slot_count && i < VTX_TYPE_SIGNATURE_MAX_SLOTS; i++) {
                uint32_t tid = site->stable_signature.types[i];
                h ^= (uint64_t)(tid & 0xFF);
                h *= 1099511628211ULL;
                h ^= (uint64_t)((tid >> 8) & 0xFF);
                h *= 1099511628211ULL;
            }
        }
        /* Hash the type frequency entries */
        for (uint32_t i = 0; i < site->type_freq.entry_count && i < VTX_TYPE_FREQ_MAX_SLOTS; i++) {
            uint32_t tid = site->type_freq.entries[i].type_id;
            uint32_t cnt = site->type_freq.entries[i].count;
            h ^= (uint64_t)(tid & 0xFF);
            h *= 1099511628211ULL;
            h ^= (uint64_t)(cnt & 0xFF);
            h *= 1099511628211ULL;
        }
    }

    return h;
}
