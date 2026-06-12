#include "inliner/transform.h"
#include "deopt/frame_state.h"
#include "interp/type_feedback.h"
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* Internal: node ID remapping table                                           */
/* ========================================================================== */

/**
 * During inlining, we clone the callee's nodes into the caller's node table.
 * We need a mapping from callee NodeID → caller NodeID for the cloned nodes.
 * This is a simple array indexed by callee NodeID.
 */
typedef struct {
    vtx_nodeid_t *map;        /* callee_nodeid → caller_nodeid */
    uint32_t      capacity;   /* size of the map array */
} vtx_nodeid_map_t;

static int nodeid_map_init(vtx_nodeid_map_t *m, uint32_t capacity)
{
    m->map = (vtx_nodeid_t *)malloc(capacity * sizeof(vtx_nodeid_t));
    if (m->map == NULL) return -1;
    m->capacity = capacity;

    /* Initialize all entries to VTX_NODEID_INVALID */
    for (uint32_t i = 0; i < capacity; i++) {
        m->map[i] = VTX_NODEID_INVALID;
    }
    return 0;
}

static void nodeid_map_destroy(vtx_nodeid_map_t *m)
{
    if (m->map != NULL) {
        free(m->map);
        m->map = NULL;
    }
    m->capacity = 0;
}

static void nodeid_map_set(vtx_nodeid_map_t *m, vtx_nodeid_t callee_id,
                            vtx_nodeid_t caller_id)
{
    VTX_ASSERT(callee_id < m->capacity, "callee_id out of map range");
    m->map[callee_id] = caller_id;
}

static vtx_nodeid_t nodeid_map_get(const vtx_nodeid_map_t *m, vtx_nodeid_t callee_id)
{
    if (callee_id >= m->capacity) return VTX_NODEID_INVALID;
    return m->map[callee_id];
}

/* ========================================================================== */
/* Internal: clone a single node from callee to caller                         */
/* ========================================================================== */

/**
 * Clone a node from the callee's node table into the caller's node table.
 * The cloned node gets a new NodeID in the caller's table.
 * Inputs are NOT remapped yet — they still point to callee NodeIDs.
 * The nodeid_map is updated to record the mapping.
 *
 * Returns the new NodeID in the caller's table, or VTX_NODEID_INVALID on failure.
 */
static vtx_nodeid_t clone_node(vtx_graph_t *caller_graph,
                                const vtx_graph_t *callee_graph,
                                vtx_nodeid_t callee_node_id,
                                vtx_nodeid_map_t *id_map)
{
    const vtx_node_t *src = vtx_node_get_const(&callee_graph->node_table, callee_node_id);
    if (src == NULL) return VTX_NODEID_INVALID;

    /* Skip dead nodes */
    if (src->dead) return VTX_NODEID_INVALID;

    /* Create a new node in the caller's table with the same opcode */
    vtx_nodeid_t new_id = vtx_node_create(&caller_graph->node_table, src->opcode);
    if (new_id == VTX_NODEID_INVALID) return VTX_NODEID_INVALID;

    vtx_node_t *dst = vtx_node_get(&caller_graph->node_table, new_id);
    VTX_ASSERT(dst != NULL, "newly created node must exist");

    /* Copy all fields except id, inputs, output_count, value_number, dead, mark */
    dst->type         = src->type;
    dst->flags        = src->flags;
    dst->constval     = src->constval;
    dst->cond         = src->cond;
    dst->local_index  = src->local_index;
    dst->field_offset = src->field_offset;
    dst->method_index = src->method_index;
    dst->type_id      = src->type_id;
    dst->bytecode_pc  = src->bytecode_pc;
    dst->frame_state  = VTX_NODEID_INVALID; /* will be set later for deopt nodes */

    /* Copy inputs (still pointing to callee NodeIDs — will be remapped later) */
    for (uint32_t i = 0; i < src->input_count; i++) {
        vtx_nodeid_t input_id = src->inputs[i];
        /* We add the input as-is; remapping happens in a second pass */
        if (input_id != VTX_NODEID_INVALID) {
            vtx_node_add_input(&caller_graph->node_table, new_id, input_id);
        }
    }

    /* Record the mapping */
    nodeid_map_set(id_map, callee_node_id, new_id);

    return new_id;
}

/* ========================================================================== */
/* Internal: remap inputs of cloned nodes from callee IDs to caller IDs        */
/* ========================================================================== */

/**
 * After all nodes are cloned, remap the inputs of each cloned node
 * from callee NodeIDs to the corresponding caller NodeIDs using
 * the id_map.
 *
 * Also handles special cases:
 *   - VTX_OP_Start inputs → caller's entry control/memory
 *   - VTX_OP_Parameter → will be replaced with call arguments later
 */
static void remap_cloned_inputs(vtx_graph_t *caller_graph,
                                 const vtx_graph_t *callee_graph,
                                 const vtx_nodeid_map_t *id_map)
{
    for (uint32_t callee_id = 0; callee_id < id_map->capacity; callee_id++) {
        vtx_nodeid_t caller_id = nodeid_map_get(id_map, (vtx_nodeid_t)callee_id);
        if (caller_id == VTX_NODEID_INVALID) continue;

        vtx_node_t *node = vtx_node_get(&caller_graph->node_table, caller_id);
        if (node == NULL) continue;

        /* Skip Parameter nodes — their inputs will be replaced with call args */
        if (node->opcode == VTX_OP_Parameter) continue;

        /* Remap each input */
        for (uint32_t i = 0; i < node->input_count; i++) {
            vtx_nodeid_t old_input = node->inputs[i];
            if (old_input == VTX_NODEID_INVALID) continue;

            /* Check if this input is a callee node that was cloned */
            vtx_nodeid_t new_input = nodeid_map_get(id_map, old_input);
            if (new_input != VTX_NODEID_INVALID) {
                /* Found a mapping — replace */
                vtx_node_replace_input(&caller_graph->node_table,
                                        caller_id, i, new_input);
            } else {
                /* Input is not in the callee graph — it might be a reference
                 * to the callee's Start node or other structural nodes.
                 * These need special handling:
                 *   - Start node → caller's entry control
                 *   - Province node → caller's entry memory */
                const vtx_node_t *src_input = vtx_node_get_const(
                    &callee_graph->node_table, old_input);
                if (src_input != NULL && src_input->opcode == VTX_OP_Start) {
                    /* Replace with caller's entry control */
                    vtx_node_replace_input(&caller_graph->node_table,
                                            caller_id, i, caller_graph->entry_control);
                } else if (src_input != NULL && src_input->opcode == VTX_OP_Province) {
                    /* Replace with caller's entry memory */
                    vtx_node_replace_input(&caller_graph->node_table,
                                            caller_id, i, caller_graph->entry_memory);
                }
                /* If none of the above, the input stays as-is (cross-graph ref).
                 * This shouldn't happen in a well-formed callee graph. */
            }
        }
    }
}

/* ========================================================================== */
/* Internal: replace Parameter nodes with call arguments                       */
/* ========================================================================== */

/**
 * Replace each Parameter node in the cloned subgraph with the
 * corresponding argument node from the call site.
 *
 * The call node's inputs are:
 *   input[0] = control predecessor
 *   input[1] = memory predecessor
 *   input[2..] = data arguments
 *
 * The callee's Parameter nodes have local_index = parameter index (0-based).
 * Parameter 0 corresponds to call input[2], etc.
 */
static void replace_parameters_with_args(vtx_graph_t *caller_graph,
                                          vtx_nodeid_t call_node_id,
                                          const vtx_nodeid_map_t *id_map,
                                          uint32_t callee_param_count)
{
    vtx_node_t *call_node = vtx_node_get(&caller_graph->node_table, call_node_id);
    if (call_node == NULL) return;

    /* Find all Parameter nodes in the cloned subgraph and replace their uses */
    for (uint32_t callee_id = 0; callee_id < id_map->capacity; callee_id++) {
        vtx_nodeid_t caller_id = nodeid_map_get(id_map, (vtx_nodeid_t)callee_id);
        if (caller_id == VTX_NODEID_INVALID) continue;

        vtx_node_t *node = vtx_node_get(&caller_graph->node_table, caller_id);
        if (node == NULL || node->opcode != VTX_OP_Parameter) continue;

        /* Determine which argument this parameter corresponds to */
        uint32_t param_idx = node->local_index;
        uint32_t arg_input_idx = 2 + param_idx; /* skip control + memory */

        if (arg_input_idx >= call_node->input_count) {
            /* Not enough arguments — this shouldn't happen in valid IR.
             * Use a constant 0 as a safe fallback. */
            vtx_nodeid_t zero_const = vtx_node_create(&caller_graph->node_table,
                                                        VTX_OP_Constant);
            if (zero_const != VTX_NODEID_INVALID) {
                vtx_node_t *zero = vtx_node_get(&caller_graph->node_table, zero_const);
                if (zero != NULL) {
                    zero->type = VTX_TYPE_Int;
                    zero->constval = vtx_constval_int(0);
                }
                /* Redirect all uses of this parameter to the zero constant */
                /* We mark the parameter as dead and let DCE clean it up */
                node->dead = true;
            }
            continue;
        }

        vtx_nodeid_t arg_node_id = call_node->inputs[arg_input_idx];

        /* Redirect all uses of this Parameter node to the argument node.
         * We do this by scanning all nodes in the caller graph that reference
         * this parameter and replacing their inputs. */
        /* For efficiency, we use a simpler approach: since we just cloned,
         * we scan the cloned nodes and replace any input that matches
         * the parameter's caller ID with the argument. */
        for (uint32_t other_callee_id = 0; other_callee_id < id_map->capacity; other_callee_id++) {
            vtx_nodeid_t other_caller_id = nodeid_map_get(id_map, (vtx_nodeid_t)other_callee_id);
            if (other_caller_id == VTX_NODEID_INVALID) continue;

            vtx_node_t *other = vtx_node_get(&caller_graph->node_table, other_caller_id);
            if (other == NULL || other->dead) continue;

            for (uint32_t inp = 0; inp < other->input_count; inp++) {
                if (other->inputs[inp] == caller_id) {
                    vtx_node_replace_input(&caller_graph->node_table,
                                            other_caller_id, inp, arg_node_id);
                }
            }
        }

        /* Mark the Parameter node as dead — its value is now provided
         * directly by the argument node. */
        node->dead = true;
    }
}

/* ========================================================================== */
/* Internal: replace Return nodes with value extraction                        */
/* ========================================================================== */

/**
 * Handle Return nodes in the cloned subgraph.
 *
 * For each Return node:
 *   - The return value (input after control + memory) becomes the
 *     replacement for the call node's output.
 *   - The control flow is redirected: the Return's control input
 *     is connected to the caller's control flow.
 *   - If there are multiple Returns, a Phi node is created to merge values.
 *
 * Memory chain: the Return's memory input is the last memory state
 * of the callee. This becomes the new memory state after the inlined call.
 */
static void handle_returns(vtx_graph_t *caller_graph,
                            vtx_nodeid_t call_node_id,
                            const vtx_nodeid_map_t *id_map,
                            uint32_t callee_graph_node_count,
                            vtx_inline_result_t *result)
{
    vtx_node_t *call_node = vtx_node_get(&caller_graph->node_table, call_node_id);
    if (call_node == NULL) return;

    /* Collect all Return nodes in the cloned subgraph */
    uint32_t return_count = 0;
    vtx_nodeid_t return_value_nodes[64];  /* reasonable max for Returns in a method */
    vtx_nodeid_t return_memory_nodes[64];
    vtx_nodeid_t return_control_nodes[64];

    for (uint32_t callee_id = 0; callee_id < callee_graph_node_count; callee_id++) {
        vtx_nodeid_t caller_id = nodeid_map_get(id_map, (vtx_nodeid_t)callee_id);
        if (caller_id == VTX_NODEID_INVALID) continue;

        vtx_node_t *node = vtx_node_get(&caller_graph->node_table, caller_id);
        if (node == NULL || node->dead || node->opcode != VTX_OP_Return) continue;

        /* Return node inputs:
         *   input[0] = control predecessor
         *   input[1] = memory predecessor
         *   input[2] = return value (if non-void) */
        vtx_nodeid_t ctrl = (node->input_count > 0) ? node->inputs[0] : VTX_NODEID_INVALID;
        vtx_nodeid_t mem  = (node->input_count > 1) ? node->inputs[1] : VTX_NODEID_INVALID;
        vtx_nodeid_t val  = (node->input_count > 2) ? node->inputs[2] : VTX_NODEID_INVALID;

        if (return_count < 64) {
            return_control_nodes[return_count] = ctrl;
            return_memory_nodes[return_count] = mem;
            return_value_nodes[return_count] = val;
        }
        return_count++;

        /* Mark the Return node as dead — it's been replaced by the merge logic */
        node->dead = true;
    }

    if (return_count == 0) {
        /* No returns found — the callee might be infinite loop or abnormal.
         * Set results to invalid and let caller handle. */
        result->return_value_node = VTX_NODEID_INVALID;
        result->new_memory_node = VTX_NODEID_INVALID;
        return;
    }

    /* Handle memory chain: if there's a single Return, its memory input
     * is the new memory state. If multiple, we need to merge with Phi.
     * For simplicity, if there's one Return, use its memory directly.
     * For multiple Returns, we create a Phi. */
    if (return_count == 1) {
        result->new_memory_node = return_memory_nodes[0];
        result->return_value_node = return_value_nodes[0];
    } else {
        /* Multiple returns: create Region + Phi for values and memory.
         *
         * This is the standard SoN inlining approach:
         * 1. Create a Region node with one input per Return's control
         * 2. Create Phi nodes for the return value and memory,
         *    each with one input per Return */
        vtx_nodeid_t region_id = vtx_node_create(&caller_graph->node_table,
                                                   VTX_OP_Region);
        if (region_id == VTX_NODEID_INVALID) {
            result->return_value_node = VTX_NODEID_INVALID;
            result->new_memory_node = VTX_NODEID_INVALID;
            return;
        }

        /* Add control inputs to Region from each Return's control predecessor */
        for (uint32_t i = 0; i < return_count && i < 64; i++) {
            if (return_control_nodes[i] != VTX_NODEID_INVALID) {
                vtx_node_add_input(&caller_graph->node_table, region_id,
                                    return_control_nodes[i]);
            }
        }

        /* Create Phi for return value (if callee returns non-void) */
        if (return_value_nodes[0] != VTX_NODEID_INVALID) {
            vtx_nodeid_t phi_id = vtx_node_create(&caller_graph->node_table,
                                                    VTX_OP_Phi);
            if (phi_id != VTX_NODEID_INVALID) {
                vtx_node_t *phi = vtx_node_get(&caller_graph->node_table, phi_id);
                if (phi != NULL) {
                    phi->flags = VTX_NF_PINNED;
                    /* Add Region as first input */
                    vtx_node_add_input(&caller_graph->node_table, phi_id, region_id);
                    /* Add return values as Phi inputs */
                    for (uint32_t i = 0; i < return_count && i < 64; i++) {
                        if (return_value_nodes[i] != VTX_NODEID_INVALID) {
                            vtx_node_add_input(&caller_graph->node_table, phi_id,
                                                return_value_nodes[i]);
                        }
                    }
                }
                result->return_value_node = phi_id;
                result->phis_created++;
            }
        } else {
            result->return_value_node = VTX_NODEID_INVALID;
        }

        /* Create Phi for memory */
        vtx_nodeid_t mem_phi_id = vtx_node_create(&caller_graph->node_table,
                                                     VTX_OP_Phi);
        if (mem_phi_id != VTX_NODEID_INVALID) {
            vtx_node_t *mem_phi = vtx_node_get(&caller_graph->node_table, mem_phi_id);
            if (mem_phi != NULL) {
                mem_phi->flags = VTX_NF_MEMORY | VTX_NF_PINNED;
                vtx_node_add_input(&caller_graph->node_table, mem_phi_id, region_id);
                for (uint32_t i = 0; i < return_count && i < 64; i++) {
                    if (return_memory_nodes[i] != VTX_NODEID_INVALID) {
                        vtx_node_add_input(&caller_graph->node_table, mem_phi_id,
                                            return_memory_nodes[i]);
                    }
                }
            }
            result->new_memory_node = mem_phi_id;
            result->phis_created++;
        } else {
            result->new_memory_node = VTX_NODEID_INVALID;
        }
    }
}

/* ========================================================================== */
/* Internal: add FrameState at inlined entry                                   */
/* ========================================================================== */

/**
 * Add a FrameState node at the entry point of the inlined method.
 * This FrameState captures the state for deoptimization if a guard
 * inside the inlined method fails.
 *
 * The FrameState records:
 *   - bytecode_pc: entry of the callee method
 *   - method_id: the callee's method_id
 *   - locals: mapped from the call arguments
 *   - stack: empty (entry point has nothing on stack)
 *   - caller: the FrameState of the call node (if any)
 */
static vtx_nodeid_t add_inline_frame_state(vtx_graph_t *caller_graph,
                                            vtx_nodeid_t call_node_id,
                                            uint32_t callee_method_id,
                                            uint32_t callee_param_count,
                                            vtx_arena_t *arena)
{
    vtx_node_t *call_node = vtx_node_get(&caller_graph->node_table, call_node_id);
    if (call_node == NULL) return VTX_NODEID_INVALID;

    /* Create a FrameState node in the SoN graph */
    vtx_nodeid_t fs_id = vtx_node_create(&caller_graph->node_table,
                                           VTX_OP_FrameState);
    if (fs_id == VTX_NODEID_INVALID) return VTX_NODEID_INVALID;

    vtx_node_t *fs_node = vtx_node_get(&caller_graph->node_table, fs_id);
    if (fs_node == NULL) return VTX_NODEID_INVALID;

    fs_node->flags = VTX_NF_PINNED;
    fs_node->bytecode_pc = 0; /* entry of callee */
    fs_node->method_index = callee_method_id;

    /* Add control input from the call site's control predecessor */
    if (call_node->input_count > 0) {
        vtx_node_add_input(&caller_graph->node_table, fs_id, call_node->inputs[0]);
    }

    /* Add memory input */
    if (call_node->input_count > 1) {
        vtx_node_add_input(&caller_graph->node_table, fs_id, call_node->inputs[1]);
    }

    /* Add argument values as FrameState inputs (for deopt materialization) */
    for (uint32_t i = 2; i < call_node->input_count; i++) {
        vtx_node_add_input(&caller_graph->node_table, fs_id, call_node->inputs[i]);
    }

    /* Link this FrameState to the call node's existing FrameState (caller chain) */
    fs_node->frame_state = call_node->frame_state;

    /* Set the call node's FrameState to this new one */
    call_node->frame_state = fs_id;

    return fs_id;
}

/* ========================================================================== */
/* Internal: thread callee memory chain into caller                            */
/* ========================================================================== */

/**
 * After inlining, the callee's memory operations must be threaded
 * into the caller's memory chain. The call node's memory input
 * (input[1]) was the memory state before the call. The callee's
 * memory chain starts from that state and produces a new memory
 * state (the return's memory output).
 *
 * The memory chain is already handled by the node cloning —
 * memory nodes in the callee reference the Start/Province node,
 * which was remapped to the call's memory input. The return's
 * memory output is the new memory state.
 *
 * This function just needs to update any nodes in the caller that
 * were using the call node's memory output to use the new memory
 * state instead.
 */
static void thread_memory_chain(vtx_graph_t *caller_graph,
                                 vtx_nodeid_t call_node_id,
                                 vtx_nodeid_t new_memory)
{
    if (new_memory == VTX_NODEID_INVALID) return;

    /* Find all nodes that reference the call node as a memory input
     * and replace with the new memory node */
    for (uint32_t i = 0; i < caller_graph->node_table.count; i++) {
        vtx_node_t *node = &caller_graph->node_table.nodes[i];
        if (node->dead) continue;

        for (uint32_t inp = 0; inp < node->input_count; inp++) {
            if (node->inputs[inp] == call_node_id &&
                vtx_nf_has(node->flags, VTX_NF_MEMORY)) {
                vtx_node_replace_input(&caller_graph->node_table,
                                        node->id, inp, new_memory);
            }
        }
    }
}

/* ========================================================================== */
/* Can-inline check                                                            */
/* ========================================================================== */

bool vtx_inline_can_inline(const vtx_graph_t *caller_graph,
                            vtx_nodeid_t call_node,
                            const vtx_graph_t *callee_graph,
                            uint32_t current_depth)
{
    if (caller_graph == NULL || callee_graph == NULL) return false;

    /* Check call node exists and is not dead */
    const vtx_node_t *call = vtx_node_get_const(&caller_graph->node_table, call_node);
    if (call == NULL || call->dead) return false;

    /* Must be a call opcode */
    if (call->opcode != VTX_OP_CallStatic &&
        call->opcode != VTX_OP_CallVirtual &&
        call->opcode != VTX_OP_CallInterface) {
        return false;
    }

    /* Callee must not be too large */
    if (callee_graph->node_table.count > VTX_INLINE_SIZE_LIMIT) {
        return false;
    }

    /* Inlining depth must not be exceeded */
    if (current_depth >= VTX_MAX_TREE_DEPTH) {
        return false;
    }

    /* Prevent recursive inlining (caller == callee) */
    if (caller_graph == callee_graph) {
        return false;
    }

    return true;
}

/* ========================================================================== */
/* Main inlining transform                                                     */
/* ========================================================================== */

vtx_inline_result_t vtx_inline_transform(vtx_graph_t *caller_graph,
                                          vtx_nodeid_t call_node,
                                          const vtx_graph_t *callee_graph,
                                          vtx_arena_t *arena)
{
    vtx_inline_result_t result;
    memset(&result, 0, sizeof(result));
    result.return_value_node = VTX_NODEID_INVALID;
    result.new_memory_node = VTX_NODEID_INVALID;

    /* Validate inputs */
    if (caller_graph == NULL || callee_graph == NULL || arena == NULL) {
        return result;
    }

    vtx_node_t *call = vtx_node_get(&caller_graph->node_table, call_node);
    if (call == NULL || call->dead) {
        return result;
    }

    /* Verify this is a call node */
    if (call->opcode != VTX_OP_CallStatic &&
        call->opcode != VTX_OP_CallVirtual &&
        call->opcode != VTX_OP_CallInterface) {
        return result;
    }

    uint32_t nodes_before = vtx_graph_node_count(caller_graph);

    /* Step 1: Build the node ID remapping table */
    uint32_t callee_node_count = callee_graph->node_table.count;
    vtx_nodeid_map_t id_map;
    if (nodeid_map_init(&id_map, callee_node_count) != 0) {
        return result;
    }

    /* Step 2: Clone all non-structural callee nodes into caller.
     * Skip Start node and Province node — these are structural
     * and will be replaced by caller's entry nodes. */
    for (uint32_t i = 0; i < callee_node_count; i++) {
        const vtx_node_t *src = vtx_node_get_const(&callee_graph->node_table, i);
        if (src == NULL || src->dead) continue;

        /* Skip structural nodes that are replaced by caller equivalents */
        if (src->opcode == VTX_OP_Start || src->opcode == VTX_OP_Province) {
            continue;
        }

        vtx_nodeid_t new_id = clone_node(caller_graph, callee_graph, i, &id_map);
        if (new_id == VTX_NODEID_INVALID) {
            /* Clone failed — clean up and return failure */
            nodeid_map_destroy(&id_map);
            return result;
        }
    }

    /* Step 3: Remap inputs of cloned nodes from callee IDs to caller IDs */
    remap_cloned_inputs(caller_graph, callee_graph, &id_map);

    /* Step 4: Replace Parameter nodes with call arguments */
    replace_parameters_with_args(caller_graph, call_node, &id_map,
                                  callee_graph->parameter_count);

    /* Step 5: Handle Return nodes — create merge Phis */
    handle_returns(caller_graph, call_node, &id_map, callee_node_count, &result);

    /* Step 6: Add FrameState at inlined entry for deoptimization */
    add_inline_frame_state(caller_graph, call_node,
                            call->method_index,
                            callee_graph->parameter_count,
                            arena);

    /* Step 7: Thread callee memory chain into caller */
    thread_memory_chain(caller_graph, call_node, result.new_memory_node);

    /* Step 8: Replace uses of the call node's data output with the
     * inlined return value. Any node that was using the call node
     * as a data input should now use the return value node instead. */
    if (result.return_value_node != VTX_NODEID_INVALID) {
        /* Scan all nodes in the caller for references to the call node
         * as a data input and replace with the return value */
        for (uint32_t i = 0; i < caller_graph->node_table.count; i++) {
            vtx_node_t *node = &caller_graph->node_table.nodes[i];
            if (node->dead) continue;

            for (uint32_t inp = 0; inp < node->input_count; inp++) {
                if (node->inputs[inp] == call_node &&
                    vtx_nf_has(node->flags, VTX_NF_DATA)) {
                    vtx_node_replace_input(&caller_graph->node_table,
                                            node->id, inp, result.return_value_node);
                }
            }
        }
    }

    /* Step 9: Mark the call node as dead (it's been replaced by the inlined body) */
    call->dead = true;
    result.nodes_removed++;

    /* Step 10: Run GVN on the inlined subgraph to eliminate redundancies */
    uint32_t gvn_eliminated = vtx_gvn_run(caller_graph);
    (void)gvn_eliminated; /* GVN results are applied in-place */

    /* Compute metrics */
    uint32_t nodes_after = vtx_graph_node_count(caller_graph);
    result.nodes_added = nodes_after - nodes_before + result.nodes_removed;
    result.success = true;

    /* Clean up */
    nodeid_map_destroy(&id_map);

    return result;
}

/* ========================================================================== */
/* Chain inlining for hyper-stable call sites (Proposal #6)                      */
/* ========================================================================== */

/**
 * Internal: check if a call site is hyper-stable based on type feedback.
 *
 * Uses the type feedback system to check if the call site at the given
 * bytecode_pc has a stable-type signature that has been consistent for
 * VTX_TYPE_STABILITY_WINDOW observations.
 */
static bool is_call_site_hyper_stable(const vtx_type_feedback_t *type_feedback,
                                        uint32_t bytecode_pc)
{
    if (type_feedback == NULL) return false;
    if (bytecode_pc >= type_feedback->call_site_count) return false;
    return vtx_tf_call_site_is_hyper_stable(&type_feedback->call_sites[bytecode_pc]);
}

/**
 * Internal: count the number of active (non-dead) nodes in a graph.
 */
static uint32_t count_active_nodes(const vtx_graph_t *graph)
{
    if (graph == NULL) return 0;
    uint32_t count = 0;
    for (uint32_t i = 0; i < graph->node_table.count; i++) {
        if (!graph->node_table.nodes[i].dead) count++;
    }
    return count;
}

/**
 * Internal: find all CallVirtual/CallStatic nodes in a subgraph that
 * could be candidates for further chain inlining.
 *
 * Returns an array of NodeIDs and the count. The array is allocated
 * from the arena.
 */
static vtx_nodeid_t *find_chain_candidates(vtx_graph_t *graph,
                                             const vtx_type_feedback_t *type_feedback,
                                             vtx_arena_t *arena,
                                             uint32_t *out_count)
{
    if (graph == NULL || type_feedback == NULL || arena == NULL || out_count == NULL) {
        if (out_count) *out_count = 0;
        return NULL;
    }

    /* Count candidates first */
    uint32_t candidate_count = 0;
    for (uint32_t i = 0; i < graph->node_table.count; i++) {
        vtx_node_t *node = &graph->node_table.nodes[i];
        if (node->dead) continue;
        if (node->opcode != VTX_OP_CallVirtual && node->opcode != VTX_OP_CallStatic) continue;
        if (!is_call_site_hyper_stable(type_feedback, node->bytecode_pc)) continue;
        candidate_count++;
    }

    if (candidate_count == 0) {
        *out_count = 0;
        return NULL;
    }

    /* Allocate and fill the array */
    vtx_nodeid_t *candidates = (vtx_nodeid_t *)vtx_arena_alloc(
        arena, candidate_count * sizeof(vtx_nodeid_t));
    if (candidates == NULL) {
        *out_count = 0;
        return NULL;
    }

    uint32_t idx = 0;
    for (uint32_t i = 0; i < graph->node_table.count && idx < candidate_count; i++) {
        vtx_node_t *node = &graph->node_table.nodes[i];
        if (node->dead) continue;
        if (node->opcode != VTX_OP_CallVirtual && node->opcode != VTX_OP_CallStatic) continue;
        if (!is_call_site_hyper_stable(type_feedback, node->bytecode_pc)) continue;
        candidates[idx++] = i;
    }

    *out_count = candidate_count;
    return candidates;
}

vtx_chain_inline_result_t vtx_inline_chain(
    vtx_graph_t *graph,
    vtx_nodeid_t call_node,
    const vtx_type_feedback_t *type_feedback,
    vtx_callee_lookup_fn callee_lookup,
    void *lookup_context,
    vtx_arena_t *arena)
{
    vtx_chain_inline_result_t result;
    memset(&result, 0, sizeof(result));

    if (graph == NULL || callee_lookup == NULL || arena == NULL) return result;

    vtx_node_t *node = vtx_node_get(&graph->node_table, call_node);
    if (node == NULL || node->dead) return result;
    if (node->opcode != VTX_OP_CallVirtual && node->opcode != VTX_OP_CallStatic) return result;

    /* Only chain-inline hyper-stable sites */
    if (!is_call_site_hyper_stable(type_feedback, node->bytecode_pc)) return result;

    /* Record the node count before inlining */
    uint32_t nodes_before = count_active_nodes(graph);

    /* Look up the callee graph */
    const vtx_graph_t *callee = callee_lookup(node->method_index, lookup_context);
    if (callee == NULL) return result;

    /* Check if inlining the callee would exceed the budget.
     * Estimate the inlined size as the callee's active node count. */
    uint32_t callee_nodes = count_active_nodes(callee);
    if (result.cumulative_node_count + callee_nodes > VTX_CHAIN_INLINE_BUDGET) {
        result.budget_exhausted = true;
        return result;
    }

    /* Perform the inlining at this level using the existing inline transform.
     * The vtx_inline_transform function handles node cloning, parameter
     * replacement, return merging, and FrameState threading. */
    vtx_inline_result_t inline_result = vtx_inline_transform(
        graph, call_node, callee, arena);

    if (!inline_result.success) return result;

    result.chain_depth = 1;
    result.cumulative_node_count = count_active_nodes(graph) - nodes_before + callee_nodes;
    result.sites_inlined = 1;

    /* Recurse: scan the inlined subgraph for further hyper-stable sites */
    if (result.chain_depth < VTX_MAX_CHAIN_DEPTH) {
        uint32_t next_count = 0;
        vtx_nodeid_t *next_candidates = find_chain_candidates(
            graph, type_feedback, arena, &next_count);

        for (uint32_t i = 0; i < next_count; i++) {
            if (result.chain_depth >= VTX_MAX_CHAIN_DEPTH) {
                result.depth_exhausted = true;
                break;
            }

            if (result.cumulative_node_count > VTX_CHAIN_INLINE_BUDGET) {
                result.budget_exhausted = true;
                break;
            }

            /* Recursively chain-inline this candidate */
            vtx_chain_inline_result_t sub_result = vtx_inline_chain(
                graph, next_candidates[i], type_feedback,
                callee_lookup, lookup_context, arena);

            result.chain_depth += sub_result.chain_depth;
            result.cumulative_node_count += sub_result.cumulative_node_count;
            result.sites_inlined += sub_result.sites_inlined;
            result.budget_exhausted = result.budget_exhausted || sub_result.budget_exhausted;
            result.depth_exhausted = result.depth_exhausted || sub_result.depth_exhausted;

            if (sub_result.chain_depth == 0) break;
        }
    } else {
        result.depth_exhausted = true;
    }

    return result;
}
