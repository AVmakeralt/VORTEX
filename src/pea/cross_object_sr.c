/**
 * VORTEX Cross-Object Scalar Replacement
 *
 * Builds an allocation graph, computes effective escape states, and
 * rewrites field accesses to scalar locals for non-escaping inner objects.
 */

#include "pea/cross_object_sr.h"
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* Internal: check if opcode is an allocation                                  */
/* ========================================================================== */

static inline bool is_allocation(vtx_node_opcode_t opcode)
{
    return opcode == VTX_OP_NewObject ||
           opcode == VTX_OP_NewArray  ||
           opcode == VTX_OP_Allocate;
}

/* ========================================================================== */
/* Internal: allocation graph construction                                     */
/* ========================================================================== */

/**
 * Add an edge to the allocation graph.
 */
static int alloc_graph_add_edge(vtx_alloc_graph_t *g, vtx_arena_t *arena,
                                 vtx_nodeid_t container_id,
                                 vtx_nodeid_t value_id,
                                 uint32_t field_offset,
                                 vtx_nodeid_t store_node_id)
{
    /* Grow the all_edges array if needed */
    if (g->edge_count >= g->edge_capacity) {
        uint32_t new_cap = g->edge_capacity == 0 ? 32 : g->edge_capacity * 2;
        vtx_alloc_edge_t *new_edges = vtx_arena_alloc(arena,
            new_cap * sizeof(vtx_alloc_edge_t));
        if (!new_edges) return -1;
        if (g->all_edges && g->edge_count > 0) {
            memcpy(new_edges, g->all_edges,
                   g->edge_count * sizeof(vtx_alloc_edge_t));
        }
        g->all_edges = new_edges;
        g->edge_capacity = new_cap;
    }

    vtx_alloc_edge_t *edge = &g->all_edges[g->edge_count];
    edge->container_id  = container_id;
    edge->value_id      = value_id;
    edge->field_offset  = field_offset;
    edge->store_node_id = store_node_id;
    edge->next          = NULL;
    edge->rev_next      = NULL;

    /* Add to forward adjacency list */
    if (container_id < g->edge_array_size) {
        edge->next = g->alloc_edges[container_id];
        g->alloc_edges[container_id] = edge;
    }

    /* Add to reverse adjacency list */
    if (value_id < g->reverse_array_size) {
        edge->rev_next = g->reverse_edges[value_id];
        g->reverse_edges[value_id] = edge;
    }

    g->edge_count++;
    return 0;
}

/**
 * Build the allocation graph from the SoN graph.
 * Scans all StoreField nodes for edges between allocations.
 */
static int build_alloc_graph(vtx_graph_t *graph, vtx_alloc_graph_t *g,
                              vtx_arena_t *arena)
{
    vtx_node_table_t *table = &graph->node_table;
    uint32_t node_count = table->count;

    memset(g, 0, sizeof(*g));

    /* Allocate adjacency arrays */
    g->edge_array_size = node_count;
    g->alloc_edges = vtx_arena_alloc(arena,
        node_count * sizeof(vtx_alloc_edge_t *));
    if (!g->alloc_edges) return -1;
    memset(g->alloc_edges, 0, node_count * sizeof(vtx_alloc_edge_t *));

    g->reverse_array_size = node_count;
    g->reverse_edges = vtx_arena_alloc(arena,
        node_count * sizeof(vtx_alloc_edge_t *));
    if (!g->reverse_edges) return -1;
    memset(g->reverse_edges, 0, node_count * sizeof(vtx_alloc_edge_t *));

    /* Scan for StoreField nodes that create edges between allocations */
    for (uint32_t i = 0; i < node_count; i++) {
        vtx_node_t *node = &table->nodes[i];
        if (node->dead || node->opcode != VTX_OP_StoreField) continue;

        /* StoreField inputs: [control?, memory?, receiver, value]
         * We need at least 2 inputs for receiver and value */
        if (node->input_count < 2) continue;

        /* Find the receiver and value inputs.
         * The last two inputs are typically the receiver and value. */
        vtx_nodeid_t receiver_id = node->inputs[node->input_count - 2];
        vtx_nodeid_t value_id    = node->inputs[node->input_count - 1];

        /* Check if both receiver and value are allocations */
        vtx_node_t *recv_node = vtx_node_get(table, receiver_id);
        vtx_node_t *val_node  = vtx_node_get(table, value_id);

        if (!recv_node || recv_node->dead || !is_allocation(recv_node->opcode))
            continue;
        if (!val_node || val_node->dead || !is_allocation(val_node->opcode))
            continue;

        /* Add edge: receiver → value via field_offset */
        if (alloc_graph_add_edge(g, arena, receiver_id, value_id,
                                  node->field_offset, node->id) != 0) {
            return -1;
        }
    }

    /* Reverse adjacency lists are now populated in alloc_graph_add_edge()
     * as edges are added, so no separate build pass is needed. */

    return 0;
}

/* ========================================================================== */
/* Internal: check if a field is read at an escape point                       */
/* ========================================================================== */

/**
 * Bug 9 fix: Check if the field storing B into A is ever read at an
 * actual escape point. If not, B does not effectively escape through A.
 *
 * Previously, the code assumed that if A escapes and A.field is read
 * anywhere, then B (stored in A.field) also escapes. This is over-
 * conservative because the read of A.field might not be at the escape
 * point — the read value might only be used locally.
 *
 * This function checks whether the LoadField result actually reaches
 * an escape point (return, call argument, etc.). Only if the loaded
 * value is consumed at an escape point does B effectively escape.
 */
static bool is_field_read_at_escape(vtx_graph_t *graph,
                                      vtx_nodeid_t container_id,
                                      uint32_t field_offset,
                                      const vtx_pea_analysis_t *analysis)
{
    vtx_node_table_t *table = &graph->node_table;
    (void)analysis; /* may be used for more precise checks in the future */

    for (uint32_t i = 0; i < table->count; i++) {
        vtx_node_t *node = &table->nodes[i];
        if (node->dead) continue;
        if (node->opcode != VTX_OP_LoadField) continue;
        if (node->field_offset != field_offset) continue;
        if (node->input_count < 1) continue;

        /* Check if the receiver is the container */
        vtx_nodeid_t receiver_id = node->inputs[node->input_count - 1];
        if (receiver_id != container_id) continue;

        /* Found a LoadField that reads this field from the container.
         * Now check if the loaded value reaches an escape point.
         * We do a simple reachability check: walk all users of this
         * LoadField node and check if any of them are escape points. */
        for (uint32_t u = 0; u < table->count; u++) {
            vtx_node_t *user = &table->nodes[u];
            if (user->dead) continue;

            /* Check if this user takes the LoadField as input */
            bool uses_load = false;
            for (uint32_t inp = 0; inp < user->input_count; inp++) {
                if (user->inputs[inp] == node->id) {
                    uses_load = true;
                    break;
                }
            }
            if (!uses_load) continue;

            /* Check if this user is an escape point */
            switch (user->opcode) {
            case VTX_OP_Return:
            case VTX_OP_CallStatic:
            case VTX_OP_CallVirtual:
            case VTX_OP_CallInterface:
            case VTX_OP_CallRuntime:
                /* The loaded value is consumed at an escape point.
                 * This means the field IS read at escape, and B
                 * effectively escapes through A.field. */
                return true;
            default:
                break;
            }
        }
    }

    return false;
}

/* ========================================================================== */
/* Internal: compute effective escape states                                   */
/* ========================================================================== */

/**
 * For each allocation, trace all paths in the allocation graph to
 * escape points. An allocation's effective escape is NoEscape if:
 *   - Its raw escape state is NoEscape, OR
 *   - Its only escape paths go through field stores into containers
 *     that DO escape, but the specific field that holds this allocation
 *     is never read at the actual escape point.
 *
 * D3 enhancement: The analysis is now TRANSITIVE. If A→B→C in the
 * allocation graph (A.f=B, B.g=C), and A escapes but the field f
 * is never read at escape, then B effectively does NOT escape, and
 * transitively C does NOT escape either. Even if B.g IS read, C
 * doesn't escape because B itself doesn't effectively escape.
 *
 * The transitive analysis uses a fixed-point iteration:
 *   1. Initialize effective states = raw escape states
 *   2. For each allocation with effective > NoEscape:
 *      a. Check non-field escape paths
 *      b. Check outgoing edges: is the allocation stored in a
 *         container whose field is read at escape?
 *      c. Use EFFECTIVE (not raw) escape of containers — if a
 *         container has effective NoEscape, values stored in it
 *         don't escape through it
 *   3. Repeat until no more states can be lowered
 */
static vtx_effective_escape_t *compute_effective_escape(
    vtx_graph_t *graph,
    const vtx_pea_analysis_t *analysis,
    const vtx_alloc_graph_t *alloc_graph,
    vtx_arena_t *arena)
{
    vtx_node_table_t *table = &graph->node_table;
    uint32_t state_count = analysis->escape_map.state_count;

    vtx_effective_escape_t *eff = vtx_arena_alloc(arena,
        sizeof(vtx_effective_escape_t));
    if (!eff) return NULL;
    memset(eff, 0, sizeof(*eff));

    eff->state_count = state_count;
    eff->effective_states = vtx_arena_alloc(arena,
        state_count * sizeof(vtx_escape_state_t));
    if (!eff->effective_states) return NULL;

    /* Start with the raw escape states */
    memcpy(eff->effective_states, analysis->escape_map.states,
           state_count * sizeof(vtx_escape_state_t));

    eff->scalar_field_count = state_count;
    eff->scalar_fields = vtx_arena_alloc(arena,
        state_count * sizeof(uint32_t));
    if (!eff->scalar_fields) return NULL;
    memset(eff->scalar_fields, 0, state_count * sizeof(uint32_t));

    /* D3: Fixed-point iteration for transitive effective escape.
     * We iterate until no more effective states can be lowered.
     * Each iteration may lower an allocation's effective escape,
     * which can then allow other allocations stored into it to
     * also be lowered in the next iteration. */
    const uint32_t MAX_ITERATIONS = 10; /* safety bound */
    for (uint32_t iter = 0; iter < MAX_ITERATIONS; iter++) {
        bool any_changed = false;

        for (uint32_t a = 0; a < analysis->escape_map.alloc_count; a++) {
            vtx_nodeid_t alloc_id = analysis->escape_map.alloc_ids[a];
            vtx_escape_state_t raw_state = analysis->escape_map.states[alloc_id];

            if (raw_state == VTX_ESCAPE_NONE) continue; /* already NoEscape */
            if (eff->effective_states[alloc_id] == VTX_ESCAPE_NONE) continue; /* already lowered */

            /* Check if this allocation escapes through non-field paths:
             * return, call argument, global store, monitor */
            bool has_non_field_escape = false;
            for (uint32_t i = 0; i < table->count; i++) {
                vtx_node_t *node = &table->nodes[i];
                if (node->dead) continue;

                switch (node->opcode) {
                case VTX_OP_Return:
                case VTX_OP_CallStatic:
                case VTX_OP_CallVirtual:
                case VTX_OP_CallInterface:
                case VTX_OP_CallRuntime:
                    for (uint32_t j = 0; j < node->input_count; j++) {
                        if (node->inputs[j] == alloc_id) {
                            has_non_field_escape = true;
                            break;
                        }
                    }
                    break;
                default:
                    break;
                }
                if (has_non_field_escape) break;
            }

            /* If the allocation escapes through a non-field path, effective
             * escape cannot be lowered */
            if (has_non_field_escape) continue;

            /* Check outgoing field-store edges (where this allocation is
             * the value stored into a container's field).
             *
             * Bug 9 fix + D3: We check if the container's field is actually
             * read at an escape point using is_field_read_at_escape(), and
             * we consider the EFFECTIVE escape state of the container (not
             * the raw state). If the container has effective NoEscape, the
             * value stored in it doesn't escape through that edge, even if
             * the field is read somewhere (because the container itself is
             * not reachable at any escape point). */
            uint32_t incoming_count = 0;
            bool escapes_through_any_edge = false;

            for (uint32_t e = 0; e < alloc_graph->edge_count; e++) {
                vtx_alloc_edge_t *edge = &alloc_graph->all_edges[e];
                if (edge->value_id == alloc_id) {
                    incoming_count++;

                    /* D3: Check the EFFECTIVE escape of the container.
                     * If the container has effective NoEscape, the value
                     * stored in it doesn't escape through this edge. */
                    if (edge->container_id < state_count &&
                        eff->effective_states[edge->container_id] == VTX_ESCAPE_NONE) {
                        /* Container effectively doesn't escape, so the
                         * value stored in it doesn't escape through this
                         * edge either. Skip this edge. */
                        continue;
                    }

                    /* Container has effective escape > NoEscape.
                     * Check if the field is actually read at an escape point. */
                    bool field_read_at_escape = is_field_read_at_escape(
                        graph, edge->container_id, edge->field_offset, analysis);

                    if (field_read_at_escape) {
                        escapes_through_any_edge = true;
                        break; /* no need to check more edges */
                    }
                }
            }

            /* If the allocation doesn't escape through any edge (all
             * edges either point to effectively non-escaping containers
             * or have fields that are not read at escape), lower its
             * effective escape to NoEscape. */
            if (!escapes_through_any_edge && incoming_count > 0) {
                eff->effective_states[alloc_id] = VTX_ESCAPE_NONE;
                any_changed = true;
            }
        }

        if (!any_changed) break; /* fixed point reached */
    }

    /* Count scalar fields for each allocation that can be replaced */
    for (uint32_t a = 0; a < analysis->escape_map.alloc_count; a++) {
        vtx_nodeid_t alloc_id = analysis->escape_map.alloc_ids[a];
        if (eff->effective_states[alloc_id] != VTX_ESCAPE_NONE) continue;

        vtx_node_t *alloc_node = vtx_node_get(table, alloc_id);
        if (!alloc_node) continue;

        /* Count fields: for NewObject, the field count comes from the
         * type descriptor. For our purposes, we count the number of
         * distinct field_offsets accessed via LoadField/StoreField. */
        uint32_t field_count = 0;
        uint32_t *offsets = NULL;
        uint32_t offset_count = 0;
        uint32_t offset_capacity = 0;

        for (uint32_t n = 0; n < table->count; n++) {
            vtx_node_t *node = &table->nodes[n];
            if (node->dead) continue;
            if (node->opcode != VTX_OP_LoadField &&
                node->opcode != VTX_OP_StoreField) continue;

            /* Check if this field access is on the allocation */
            vtx_nodeid_t receiver = VTX_NODEID_INVALID;
            if (node->input_count >= 1) {
                if (node->opcode == VTX_OP_LoadField) {
                    receiver = node->inputs[node->input_count - 1];
                } else {
                    /* StoreField: last two are receiver and value */
                    receiver = node->inputs[node->input_count - 2];
                }
            }

            if (receiver == alloc_id) {
                /* Check if this offset is already counted */
                bool found = false;
                for (uint32_t k = 0; k < offset_count; k++) {
                    if (offsets[k] == node->field_offset) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    /* Grow offsets array if needed */
                    if (offset_count >= offset_capacity) {
                        uint32_t new_cap = offset_capacity == 0 ? 8 : offset_capacity * 2;
                        uint32_t *new_offsets = vtx_arena_alloc(arena,
                            new_cap * sizeof(uint32_t));
                        if (!new_offsets) break;
                        if (offsets && offset_count > 0) {
                            memcpy(new_offsets, offsets,
                                   offset_count * sizeof(uint32_t));
                        }
                        offsets = new_offsets;
                        offset_capacity = new_cap;
                    }
                    offsets[offset_count++] = node->field_offset;
                }
            }
        }

        if (alloc_id < eff->scalar_field_count) {
            eff->scalar_fields[alloc_id] = offset_count;
        }
    }

    return eff;
}

/* ========================================================================== */
/* Internal: create scalar local nodes and rewrite field accesses              */
/* ========================================================================== */

/**
 * For each allocation with effective NoEscape:
 *   - Create a scalar local variable for each accessed field
 *   - Rewrite LoadField accesses to read from the local
 *   - Rewrite StoreField accesses to write to the local
 *   - Mark the allocation as dead (to be removed by DCE)
 */
static int rewrite_scalar_replacements(vtx_graph_t *graph,
                                        const vtx_pea_analysis_t *analysis,
                                        const vtx_effective_escape_t *eff,
                                        vtx_cross_sr_result_t *result,
                                        vtx_arena_t *arena)
{
    vtx_node_table_t *table = &graph->node_table;

    /* Initialize result */
    result->mapping_capacity = 64;
    result->mappings = vtx_arena_alloc(arena,
        result->mapping_capacity * sizeof(vtx_sr_mapping_t));
    if (!result->mappings) return -1;
    result->mapping_count = 0;

    /* For each allocation with effective NoEscape */
    for (uint32_t a = 0; a < analysis->escape_map.alloc_count; a++) {
        vtx_nodeid_t alloc_id = analysis->escape_map.alloc_ids[a];
        if (eff->effective_states[alloc_id] != VTX_ESCAPE_NONE) continue;

        vtx_node_t *alloc_node = vtx_node_get(table, alloc_id);
        if (!alloc_node || alloc_node->dead) continue;

        /* Create scalar locals for each field.
         * We use Parameter-like nodes to represent locals, but since
         * we can't add real Parameter nodes, we use Constant nodes
         * as placeholders and then replace them with actual value
         * nodes when we encounter StoreField operations.
         *
         * A better approach: for each StoreField to this allocation,
         * record the stored value as the local. For LoadField, replace
         * with the local value. */
        bool alloc_fully_replaced = true;

        /* First pass: create mappings for all stored field values */
        for (uint32_t n = 0; n < table->count; n++) {
            vtx_node_t *node = &table->nodes[n];
            if (node->dead) continue;
            if (node->opcode != VTX_OP_StoreField) continue;

            /* Check if this store is to our allocation */
            if (node->input_count < 2) continue;
            vtx_nodeid_t receiver_id = node->inputs[node->input_count - 2];
            vtx_nodeid_t value_id    = node->inputs[node->input_count - 1];

            if (receiver_id != alloc_id) continue;

            /* The stored value becomes the scalar local for this field.
             * We don't create a new node — we just record the mapping
             * from (alloc_id, field_offset) → value_id. */
            if (result->mapping_count >= result->mapping_capacity) {
                uint32_t new_cap = result->mapping_capacity * 2;
                vtx_sr_mapping_t *new_mappings = vtx_arena_alloc(arena,
                    new_cap * sizeof(vtx_sr_mapping_t));
                if (!new_mappings) return -1;
                memcpy(new_mappings, result->mappings,
                       result->mapping_count * sizeof(vtx_sr_mapping_t));
                result->mappings = new_mappings;
                result->mapping_capacity = new_cap;
            }

            result->mappings[result->mapping_count].alloc_id     = alloc_id;
            result->mappings[result->mapping_count].field_offset = node->field_offset;
            result->mappings[result->mapping_count].local_id     = value_id;
            result->mapping_count++;

            /* Mark the StoreField as dead — the store now goes to the local */
            node->dead = true;
            result->field_accesses_rewritten++;
        }

        /* Second pass: rewrite LoadField accesses to use the scalar local */
        for (uint32_t n = 0; n < table->count; n++) {
            vtx_node_t *node = &table->nodes[n];
            if (node->dead) continue;
            if (node->opcode != VTX_OP_LoadField) continue;

            if (node->input_count < 1) continue;
            vtx_nodeid_t receiver_id = node->inputs[node->input_count - 1];

            if (receiver_id != alloc_id) continue;

            /* Find the scalar local for this field */
            vtx_nodeid_t local_id = VTX_NODEID_INVALID;
            for (uint32_t m = 0; m < result->mapping_count; m++) {
                if (result->mappings[m].alloc_id == alloc_id &&
                    result->mappings[m].field_offset == node->field_offset) {
                    local_id = result->mappings[m].local_id;
                    break;
                }
            }

            if (local_id != VTX_NODEID_INVALID) {
                /* Replace all uses of this LoadField with the local */
                /* We can't easily replace all uses in the SoN graph,
                 * so we mark the LoadField as having a replacement.
                 * A proper implementation would redirect all output
                 * edges. Instead, we'll do a graph-wide replacement. */
                for (uint32_t u = 0; u < table->count; u++) {
                    vtx_node_t *user = &table->nodes[u];
                    if (user->dead) continue;
                    for (uint32_t inp = 0; inp < user->input_count; inp++) {
                        if (user->inputs[inp] == node->id) {
                            vtx_node_replace_input(table, user->id, inp, local_id);
                        }
                    }
                }
                node->dead = true;
                result->field_accesses_rewritten++;
            } else {
                /* Field was never stored — this means the field has the
                 * default value (null/zero). Create a null constant. */
                vtx_nodeid_t null_id = vtx_node_create(table, VTX_OP_Constant);
                if (null_id != VTX_NODEID_INVALID) {
                    vtx_node_t *null_node = vtx_node_get(table, null_id);
                    null_node->type = VTX_TYPE_Ptr;
                    null_node->constval = vtx_constval_ptr(NULL);

                    /* Record the mapping */
                    if (result->mapping_count >= result->mapping_capacity) {
                        uint32_t new_cap = result->mapping_capacity * 2;
                        vtx_sr_mapping_t *new_mappings = vtx_arena_alloc(arena,
                            new_cap * sizeof(vtx_sr_mapping_t));
                        if (!new_mappings) return -1;
                        memcpy(new_mappings, result->mappings,
                               result->mapping_count * sizeof(vtx_sr_mapping_t));
                        result->mappings = new_mappings;
                        result->mapping_capacity = new_cap;
                    }
                    result->mappings[result->mapping_count].alloc_id     = alloc_id;
                    result->mappings[result->mapping_count].field_offset = node->field_offset;
                    result->mappings[result->mapping_count].local_id     = null_id;
                    result->mapping_count++;

                    /* Replace uses */
                    for (uint32_t u = 0; u < table->count; u++) {
                        vtx_node_t *user = &table->nodes[u];
                        if (user->dead) continue;
                        for (uint32_t inp = 0; inp < user->input_count; inp++) {
                            if (user->inputs[inp] == node->id) {
                                vtx_node_replace_input(table, user->id, inp, null_id);
                            }
                        }
                    }
                    node->dead = true;
                    result->field_accesses_rewritten++;
                }
            }
        }

        /* Mark the allocation as dead if all its uses are rewritten */
        if (alloc_fully_replaced) {
            /* Check if there are any remaining uses of the allocation */
            bool has_remaining_uses = false;
            for (uint32_t n = 0; n < table->count; n++) {
                vtx_node_t *node = &table->nodes[n];
                if (node->dead) continue;
                if (node->id == alloc_id) continue; /* skip self */

                for (uint32_t inp = 0; inp < node->input_count; inp++) {
                    if (node->inputs[inp] == alloc_id) {
                        /* Check if this use is a StoreField receiver or
                         * LoadField receiver (already handled) or something else */
                        if (node->opcode == VTX_OP_StoreField ||
                            node->opcode == VTX_OP_LoadField) {
                            continue; /* already handled */
                        }
                        has_remaining_uses = true;
                        break;
                    }
                }
                if (has_remaining_uses) break;
            }

            if (!has_remaining_uses) {
                alloc_node->dead = true;
                result->allocs_replaced++;
            }
        }
    }

    return 0;
}

/* ========================================================================== */
/* Main entry point                                                            */
/* ========================================================================== */

vtx_cross_sr_result_t *vtx_cross_object_sr_run(vtx_graph_t *graph,
                                                 const vtx_pea_analysis_t *analysis,
                                                 vtx_arena_t *arena)
{
    VTX_ASSERT(graph != NULL, "graph must not be NULL");
    VTX_ASSERT(analysis != NULL, "analysis must not be NULL");
    VTX_ASSERT(arena != NULL, "arena must not be NULL");

    /* Allocate result */
    vtx_cross_sr_result_t *result = vtx_arena_alloc(arena,
        sizeof(vtx_cross_sr_result_t));
    if (!result) return NULL;
    memset(result, 0, sizeof(*result));

    /* Step 1: Build the allocation graph */
    vtx_alloc_graph_t alloc_graph;
    if (build_alloc_graph(graph, &alloc_graph, arena) != 0) {
        return NULL;
    }

    result->edges_analyzed = alloc_graph.edge_count;

    /* Step 2: Compute effective escape states */
    vtx_effective_escape_t *eff = compute_effective_escape(
        graph, analysis, &alloc_graph, arena);
    if (!eff) return NULL;

    /* Step 3: Create scalar locals and rewrite field accesses */
    if (rewrite_scalar_replacements(graph, analysis, eff, result, arena) != 0) {
        return NULL;
    }

    return result;
}

/* ========================================================================== */
/* Query helpers                                                               */
/* ========================================================================== */

vtx_nodeid_t vtx_cross_sr_get_local(const vtx_cross_sr_result_t *result,
                                      vtx_nodeid_t alloc_id,
                                      uint32_t field_offset)
{
    VTX_ASSERT(result != NULL, "result must not be NULL");
    for (uint32_t i = 0; i < result->mapping_count; i++) {
        if (result->mappings[i].alloc_id == alloc_id &&
            result->mappings[i].field_offset == field_offset) {
            return result->mappings[i].local_id;
        }
    }
    return VTX_NODEID_INVALID;
}

bool vtx_cross_sr_is_replaced(const vtx_cross_sr_result_t *result,
                               vtx_nodeid_t alloc_id)
{
    VTX_ASSERT(result != NULL, "result must not be NULL");
    for (uint32_t i = 0; i < result->mapping_count; i++) {
        if (result->mappings[i].alloc_id == alloc_id) {
            return true;
        }
    }
    return false;
}
