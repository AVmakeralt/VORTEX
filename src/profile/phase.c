#include "profile/phase.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ========================================================================== */
/* Internal: adjacency list for the call graph                                */
/* ========================================================================== */

typedef struct {
    uint32_t callee_id;
    uint64_t frequency;
} vtx_adj_entry_t;

typedef struct {
    uint32_t           method_id;
    vtx_adj_entry_t   *out_edges;   /* outgoing edges: this method → callees */
    uint32_t           out_count;
    uint32_t           out_capacity;
} vtx_adj_node_t;

typedef struct {
    vtx_adj_node_t *nodes;      /* indexed by dense node index */
    uint32_t        node_count;
    uint32_t        node_capacity;

    /* Mapping: method_id → dense node index. Simple linear scan for now;
     * methods are typically < 10K so this is fast enough. */
    uint32_t       *method_id_map;     /* dense index → method_id */
    uint32_t       *dense_index_map;   /* method_id → dense index (sparse) */
    uint32_t        dense_index_map_cap;
} vtx_call_graph_t;

/* ========================================================================== */
/* Call graph construction                                                    */
/* ========================================================================== */

static void cg_init(vtx_call_graph_t *cg)
{
    memset(cg, 0, sizeof(*cg));
    cg->node_capacity = 64;
    cg->nodes = calloc(cg->node_capacity, sizeof(vtx_adj_node_t));
    cg->method_id_map = calloc(cg->node_capacity, sizeof(uint32_t));
    cg->dense_index_map_cap = 256;
    cg->dense_index_map = calloc(cg->dense_index_map_cap, sizeof(uint32_t));
    /* Initialize dense_index_map to 0xFFFFFFFF (invalid) */
    for (uint32_t i = 0; i < cg->dense_index_map_cap; i++) {
        cg->dense_index_map[i] = VTX_PHASE_NONE;
    }
    cg->node_count = 0;
}

static void cg_destroy(vtx_call_graph_t *cg)
{
    for (uint32_t i = 0; i < cg->node_count; i++) {
        free(cg->nodes[i].out_edges);
    }
    free(cg->nodes);
    free(cg->method_id_map);
    free(cg->dense_index_map);
    memset(cg, 0, sizeof(*cg));
}

static uint32_t cg_find_or_create_node(vtx_call_graph_t *cg, uint32_t method_id)
{
    /* Check dense_index_map if method_id fits */
    if (method_id < cg->dense_index_map_cap) {
        uint32_t idx = cg->dense_index_map[method_id];
        if (idx != VTX_PHASE_NONE) {
            return idx;
        }
    } else {
        /* Grow dense_index_map */
        uint32_t new_cap = cg->dense_index_map_cap;
        while (new_cap <= method_id) new_cap *= 2;
        uint32_t *new_map = realloc(cg->dense_index_map,
                                     (size_t)new_cap * sizeof(uint32_t));
        if (!new_map) return VTX_PHASE_NONE;
        /* Initialize new entries to invalid */
        for (uint32_t i = cg->dense_index_map_cap; i < new_cap; i++) {
            new_map[i] = VTX_PHASE_NONE;
        }
        cg->dense_index_map = new_map;
        cg->dense_index_map_cap = new_cap;
    }

    /* Create new node */
    if (cg->node_count >= cg->node_capacity) {
        uint32_t new_cap = cg->node_capacity * 2;
        vtx_adj_node_t *new_nodes = realloc(cg->nodes,
                                             (size_t)new_cap * sizeof(vtx_adj_node_t));
        if (!new_nodes) return VTX_PHASE_NONE;
        cg->nodes = new_nodes;
        cg->node_capacity = new_cap;

        uint32_t *new_mmap = realloc(cg->method_id_map,
                                      (size_t)new_cap * sizeof(uint32_t));
        if (!new_mmap) return VTX_PHASE_NONE;
        cg->method_id_map = new_mmap;
    }

    uint32_t idx = cg->node_count++;
    memset(&cg->nodes[idx], 0, sizeof(vtx_adj_node_t));
    cg->nodes[idx].method_id = method_id;
    cg->nodes[idx].out_capacity = 4;
    cg->nodes[idx].out_edges = calloc(4, sizeof(vtx_adj_entry_t));
    cg->nodes[idx].out_count = 0;
    cg->method_id_map[idx] = method_id;
    cg->dense_index_map[method_id] = idx;
    return idx;
}

static void cg_add_edge(vtx_call_graph_t *cg, uint32_t caller_id,
                         uint32_t callee_id, uint64_t frequency)
{
    uint32_t caller_idx = cg_find_or_create_node(cg, caller_id);
    if (caller_idx == VTX_PHASE_NONE) return;

    cg_find_or_create_node(cg, callee_id); /* ensure callee exists */

    vtx_adj_node_t *node = &cg->nodes[caller_idx];

    /* Check if edge already exists */
    for (uint32_t i = 0; i < node->out_count; i++) {
        if (node->out_edges[i].callee_id == callee_id) {
            uint64_t sum = node->out_edges[i].frequency + frequency;
            node->out_edges[i].frequency =
                (sum < node->out_edges[i].frequency) ? UINT64_MAX : sum;
            return;
        }
    }

    /* Add new edge */
    if (node->out_count >= node->out_capacity) {
        uint32_t new_cap = node->out_capacity * 2;
        vtx_adj_entry_t *new_edges = realloc(node->out_edges,
                                              (size_t)new_cap * sizeof(vtx_adj_entry_t));
        if (!new_edges) return;
        node->out_edges = new_edges;
        node->out_capacity = new_cap;
    }

    node->out_edges[node->out_count].callee_id = callee_id;
    node->out_edges[node->out_count].frequency = frequency;
    node->out_count++;
}

static vtx_call_graph_t *build_call_graph(const vtx_profile_global_t *global)
{
    vtx_call_graph_t *cg = malloc(sizeof(vtx_call_graph_t));
    if (!cg) return NULL;
    cg_init(cg);

    /* Add all methods as nodes (even those with no edges) */
    for (uint32_t i = 0; i < global->method_count; i++) {
        cg_find_or_create_node(cg, global->methods[i].method_id);
    }

    /* Add call edges */
    for (uint32_t i = 0; i < global->call_edge_count; i++) {
        const vtx_call_edge_t *e = &global->call_edges[i];
        cg_add_edge(cg, e->caller_method_id, e->callee_method_id, e->frequency);
    }

    return cg;
}

/* ========================================================================== */
/* Tarjan's SCC algorithm                                                     */
/* ========================================================================== */

/**
 * Tarjan's algorithm state.
 * indices: discovery index for each node (0xFFFFFFFF = unvisited)
 * lowlinks: lowest index reachable from this node
 * on_stack: whether a node is currently on the DFS stack
 * stack: the DFS stack
 */
typedef struct {
    uint32_t  *indices;
    uint32_t  *lowlinks;
    bool      *on_stack;
    uint32_t  *stack;
    uint32_t   stack_top;
    uint32_t   current_index;

    /* Output: SCC assignments */
    uint32_t  *scc_ids;       /* dense node index → SCC ID */
    uint32_t   scc_count;

    uint32_t   node_count;
} vtx_tarjan_t;

/**
 * SCC result: for each SCC, list of node indices.
 */
typedef struct {
    uint32_t  *node_indices;  /* all nodes in this SCC (contiguous) */
    uint32_t   start;         /* start index into node_indices */
    uint32_t   count;         /* number of nodes in this SCC */
} vtx_scc_t;

static void tarjan_strongconnect(vtx_tarjan_t *t, uint32_t v,
                                  const vtx_call_graph_t *cg,
                                  vtx_scc_t *sccs,
                                  uint32_t *scc_node_buf,
                                  uint32_t *scc_node_buf_pos)
{
    t->indices[v] = t->current_index;
    t->lowlinks[v] = t->current_index;
    t->current_index++;

    /* Push v onto stack */
    t->stack[t->stack_top++] = v;
    t->on_stack[v] = true;

    /* Consider successors of v */
    const vtx_adj_node_t *node = &cg->nodes[v];
    for (uint32_t i = 0; i < node->out_count; i++) {
        uint32_t w_method = node->out_edges[i].callee_id;
        uint32_t w;
        if (w_method < cg->dense_index_map_cap) {
            w = cg->dense_index_map[w_method];
        } else {
            w = VTX_PHASE_NONE;
        }
        if (w == VTX_PHASE_NONE) continue;

        if (t->indices[w] == VTX_PHASE_NONE) {
            /* Successor w has not been visited; recurse on it */
            tarjan_strongconnect(t, w, cg, sccs, scc_node_buf, scc_node_buf_pos);
            if (t->lowlinks[w] < t->lowlinks[v]) {
                t->lowlinks[v] = t->lowlinks[w];
            }
        } else if (t->on_stack[w]) {
            /* Successor w is on the stack and hence in the current SCC */
            if (t->indices[w] < t->lowlinks[v]) {
                t->lowlinks[v] = t->indices[w];
            }
        }
    }

    /* If v is a root node, pop the stack and generate an SCC */
    if (t->lowlinks[v] == t->indices[v]) {
        uint32_t scc_id = t->scc_count++;
        uint32_t start = *scc_node_buf_pos;
        uint32_t w;
        do {
            w = t->stack[--t->stack_top];
            t->on_stack[w] = false;
            t->scc_ids[w] = scc_id;
            scc_node_buf[*scc_node_buf_pos] = w;
            (*scc_node_buf_pos)++;
        } while (w != v);

        sccs[scc_id].start = start;
        sccs[scc_id].count = *scc_node_buf_pos - start;
        sccs[scc_id].node_indices = scc_node_buf; /* shared buffer */
    }
}

/* ========================================================================== */
/* Phase graph construction                                                   */
/* ========================================================================== */

static vtx_phase_graph_t *phase_graph_create(uint32_t initial_capacity)
{
    vtx_phase_graph_t *g = calloc(1, sizeof(vtx_phase_graph_t));
    if (!g) return NULL;

    g->phase_capacity = initial_capacity;
    g->phases = calloc(g->phase_capacity, sizeof(vtx_phase_t));
    if (!g->phases) { free(g); return NULL; }

    g->transition_capacity = 64;
    g->transitions = calloc(g->transition_capacity, sizeof(vtx_phase_transition_t));
    if (!g->transitions) { free(g->phases); free(g); return NULL; }

    g->method_to_phase_capacity = 256;
    g->method_to_phase = malloc((size_t)g->method_to_phase_capacity * sizeof(uint32_t));
    if (!g->method_to_phase) {
        free(g->transitions); free(g->phases); free(g);
        return NULL;
    }
    for (uint32_t i = 0; i < g->method_to_phase_capacity; i++) {
        g->method_to_phase[i] = VTX_PHASE_NONE;
    }

    return g;
}

void vtx_phase_graph_destroy(vtx_phase_graph_t *graph)
{
    if (!graph) return;
    for (uint32_t i = 0; i < graph->phase_count; i++) {
        free(graph->phases[i].method_ids);
    }
    free(graph->phases);
    free(graph->transitions);
    free(graph->method_to_phase);
    free(graph);
}

/* ========================================================================== */
/* Main detection function                                                    */
/* ========================================================================== */

vtx_phase_graph_t *vtx_phase_detect(const vtx_profile_global_t *global)
{
    if (!global || global->method_count == 0) return NULL;

    /* Step 1: Build call graph */
    vtx_call_graph_t *cg = build_call_graph(global);
    if (!cg) return NULL;

    uint32_t n = cg->node_count;
    if (n == 0) { cg_destroy(cg); return NULL; }

    /* Step 2: Run Tarjan's SCC algorithm */
    vtx_tarjan_t t;
    t.indices = malloc((size_t)n * sizeof(uint32_t));
    t.lowlinks = malloc((size_t)n * sizeof(uint32_t));
    t.on_stack = calloc(n, sizeof(bool));
    t.stack = malloc((size_t)n * sizeof(uint32_t));
    t.scc_ids = malloc((size_t)n * sizeof(uint32_t));
    t.stack_top = 0;
    t.current_index = 0;
    t.scc_count = 0;
    t.node_count = n;

    /* SCC result storage */
    vtx_scc_t *sccs = calloc(n, sizeof(vtx_scc_t)); /* max n SCCs */
    uint32_t *scc_node_buf = malloc((size_t)n * sizeof(uint32_t));
    uint32_t scc_node_buf_pos = 0;

    if (!t.indices || !t.lowlinks || !t.on_stack || !t.stack ||
        !t.scc_ids || !sccs || !scc_node_buf) {
        free(t.indices); free(t.lowlinks); free(t.on_stack);
        free(t.stack); free(t.scc_ids); free(sccs);
        free(scc_node_buf); cg_destroy(cg);
        return NULL;
    }

    /* Initialize all indices to "unvisited" */
    for (uint32_t i = 0; i < n; i++) {
        t.indices[i] = VTX_PHASE_NONE;
    }

    /* Run Tarjan's for each unvisited node */
    for (uint32_t v = 0; v < n; v++) {
        if (t.indices[v] == VTX_PHASE_NONE) {
            tarjan_strongconnect(&t, v, cg, sccs, scc_node_buf,
                                  &scc_node_buf_pos);
        }
    }

    /* Step 3: Build phase graph from SCCs */
    vtx_phase_graph_t *pg = phase_graph_create(t.scc_count);
    if (!pg) {
        free(t.indices); free(t.lowlinks); free(t.on_stack);
        free(t.stack); free(t.scc_ids); free(sccs);
        free(scc_node_buf); cg_destroy(cg);
        return NULL;
    }

    /* Grow method_to_phase map if needed */
    if (pg->method_to_phase_capacity < n) {
        uint32_t new_cap = pg->method_to_phase_capacity;
        while (new_cap < n) new_cap *= 2;
        uint32_t *new_map = realloc(pg->method_to_phase,
                                     (size_t)new_cap * sizeof(uint32_t));
        if (new_map) {
            for (uint32_t i = pg->method_to_phase_capacity; i < new_cap; i++) {
                new_map[i] = VTX_PHASE_NONE;
            }
            pg->method_to_phase = new_map;
            pg->method_to_phase_capacity = new_cap;
        }
    }

    /* For each SCC, create a phase */
    for (uint32_t scc_id = 0; scc_id < t.scc_count; scc_id++) {
        vtx_scc_t *scc = &sccs[scc_id];

        /* Compute total invocation frequency for this SCC */
        uint64_t total_freq = 0;
        for (uint32_t j = 0; j < scc->count; j++) {
            uint32_t node_idx = scc->node_indices[scc->start + j];
            uint32_t method_id = cg->nodes[node_idx].method_id;

            /* Look up invocation count from profile */
            const vtx_profile_method_t *pm =
                vtx_profile_get_method(global, method_id);
            if (pm) {
                uint64_t sum = total_freq + pm->invocation_count;
                total_freq = (sum < total_freq) ? UINT64_MAX : sum;
            }
        }

        /* Create the phase */
        if (pg->phase_count >= pg->phase_capacity) {
            uint32_t new_cap = pg->phase_capacity * 2;
            vtx_phase_t *new_phases = realloc(pg->phases,
                                               (size_t)new_cap * sizeof(vtx_phase_t));
            if (!new_phases) break;
            pg->phases = new_phases;
            pg->phase_capacity = new_cap;
        }

        vtx_phase_t *phase = &pg->phases[pg->phase_count];
        phase->phase_id = pg->phase_count;
        phase->method_count = scc->count;
        phase->method_capacity = scc->count;
        phase->method_ids = calloc(scc->count, sizeof(uint32_t));
        phase->total_frequency = total_freq;
        phase->is_significant =
            (scc->count >= VTX_PHASE_MIN_METHODS) &&
            (total_freq >= VTX_PHASE_MIN_FREQUENCY);

        /* Fill method IDs */
        for (uint32_t j = 0; j < scc->count; j++) {
            uint32_t node_idx = scc->node_indices[scc->start + j];
            uint32_t method_id = cg->nodes[node_idx].method_id;
            phase->method_ids[j] = method_id;

            /* Update method_to_phase mapping */
            if (method_id < pg->method_to_phase_capacity) {
                pg->method_to_phase[method_id] = phase->phase_id;
            }
        }

        pg->phase_count++;
    }

    /* Step 4: Extract phase transitions (inter-SCC edges between significant phases) */
    for (uint32_t i = 0; i < global->call_edge_count; i++) {
        const vtx_call_edge_t *edge = &global->call_edges[i];

        uint32_t caller_phase = vtx_phase_for_method(pg, edge->caller_method_id);
        uint32_t callee_phase = vtx_phase_for_method(pg, edge->callee_method_id);

        /* Skip intra-phase edges */
        if (caller_phase == VTX_PHASE_NONE || callee_phase == VTX_PHASE_NONE) continue;
        if (caller_phase == callee_phase) continue;

        /* Only record transitions between significant phases */
        if (caller_phase < pg->phase_count && callee_phase < pg->phase_count) {
            if (!pg->phases[caller_phase].is_significant ||
                !pg->phases[callee_phase].is_significant) continue;
        }

        /* Check if transition already exists */
        bool found = false;
        for (uint32_t j = 0; j < pg->transition_count; j++) {
            if (pg->transitions[j].from_phase_id == caller_phase &&
                pg->transitions[j].to_phase_id == callee_phase) {
                uint64_t sum = pg->transitions[j].frequency + edge->frequency;
                pg->transitions[j].frequency =
                    (sum < pg->transitions[j].frequency) ? UINT64_MAX : sum;
                found = true;
                break;
            }
        }

        if (!found) {
            if (pg->transition_count >= pg->transition_capacity) {
                uint32_t new_cap = pg->transition_capacity * 2;
                vtx_phase_transition_t *new_trans = realloc(
                    pg->transitions,
                    (size_t)new_cap * sizeof(vtx_phase_transition_t));
                if (!new_trans) continue;
                pg->transitions = new_trans;
                pg->transition_capacity = new_cap;
            }
            vtx_phase_transition_t *trans =
                &pg->transitions[pg->transition_count++];
            trans->from_phase_id = caller_phase;
            trans->to_phase_id = callee_phase;
            trans->frequency = edge->frequency;
        }
    }

    /* Also store transitions in the global profile */
    if (global->phase_transitions) {
        /* We write into the global's phase_transition array */
        /* This is a const pointer but the global was given to us non-const...
         * Actually the parameter is const. We update our own pg instead.
         * The caller can copy from pg->transitions to global if needed. */
    }

    /* Cleanup */
    free(t.indices); free(t.lowlinks); free(t.on_stack);
    free(t.stack); free(t.scc_ids); free(sccs);
    free(scc_node_buf); cg_destroy(cg);

    return pg;
}

/* ========================================================================== */
/* Query functions                                                            */
/* ========================================================================== */

uint32_t vtx_phase_for_method(const vtx_phase_graph_t *graph, uint32_t method_id)
{
    if (!graph || !graph->method_to_phase) return VTX_PHASE_NONE;
    if (method_id >= graph->method_to_phase_capacity) return VTX_PHASE_NONE;
    return graph->method_to_phase[method_id];
}

bool vtx_phase_is_entering(const vtx_phase_graph_t *graph,
                            uint32_t prev_top_method,
                            uint32_t curr_top_method)
{
    if (!graph) return false;

    uint32_t prev_phase = vtx_phase_for_method(graph, prev_top_method);
    uint32_t curr_phase = vtx_phase_for_method(graph, curr_top_method);

    /* If either is not in a phase, no transition */
    if (prev_phase == VTX_PHASE_NONE || curr_phase == VTX_PHASE_NONE) {
        return false;
    }

    /* Transition = different phases */
    return prev_phase != curr_phase;
}

const vtx_phase_t *vtx_phase_get_by_id(const vtx_phase_graph_t *graph,
                                         uint32_t phase_id)
{
    if (!graph || phase_id >= graph->phase_count) return NULL;
    return &graph->phases[phase_id];
}
