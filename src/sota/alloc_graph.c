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
/* Escape analysis (simplified flow-insensitive)                               */
/* ========================================================================== */

/**
 * Determine if an allocation escapes by checking how its value is used.
 *
 * An allocation escapes if:
 *   - It is returned from the method (Return node input)
 *   - It is stored into a field of another escaping object
 *   - It is passed as an argument to a call (unknown callee)
 *   - It is stored into a global/static field
 *   - It is used in a monitor operation
 *
 * This is a conservative flow-insensitive analysis. A more precise
 * flow-sensitive analysis would track per-block escape states.
 */
static vtx_escape_state_t compute_escape(const vtx_graph_t *graph,
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
                /* Stored into a field. If the target object escapes,
                 * this allocation also escapes.
                 * For now, conservatively mark as arg escape.
                 * The effective_escape computation below will refine this. */
                if (j >= 2) { /* value input */
                    escape = VTX_ESCAPE_ARG;
                }
                break;

            case VTX_OP_LoadField:
                /* Just loading from the allocation — no escape */
                break;

            case VTX_OP_CheckCast:
            case VTX_OP_InstanceOf:
                /* Type check — no escape by itself, but may indicate
                 * the value is about to be used in an escaping way */
                break;

            case VTX_OP_Guard:
            case VTX_OP_DeoptGuard:
                /* Guard on the allocation — no escape */
                break;

            case VTX_OP_Phi:
                /* Allocation flows through a Phi — need to check
                 * all uses of the Phi. For simplicity, mark as arg escape.
                 * A more precise analysis would track through Phis. */
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

    /* Step 4: Run escape analysis for each allocation */
    for (uint32_t i = 0; i < ag->record_count; i++) {
        vtx_sota_alloc_record_t *rec = &ag->records[i];
        rec->escape_state = compute_escape(graph, rec->alloc_node);

        switch (rec->escape_state) {
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
