#include "profile/data.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ========================================================================== */
/* Internal helpers                                                           */
/* ========================================================================== */

/**
 * Saturating increment: increment *counter, but clamp at UINT64_MAX.
 */
static inline void saturating_inc64(uint64_t *counter)
{
    if (*counter < UINT64_MAX) {
        (*counter)++;
    }
}

/**
 * Grow a dynamic array if needed. Returns 0 on success, -1 on failure.
 * *arr is the array pointer, *count is the current element count,
 * *capacity is the current capacity, elem_size is the size of one element.
 * On growth, capacity doubles (minimum initial capacity = 8).
 */
static int grow_array(void **arr, uint32_t *count, uint32_t *capacity,
                       size_t elem_size)
{
    if (*count < *capacity) {
        return 0;
    }
    uint32_t new_cap = (*capacity == 0) ? 8 : (*capacity * 2);
    void *new_arr = realloc(*arr, (size_t)new_cap * elem_size);
    if (!new_arr) {
        return -1;
    }
    /* Zero-initialize the new portion */
    memset((uint8_t *)new_arr + (size_t)*capacity * elem_size,
           0, (size_t)(new_cap - *capacity) * elem_size);
    *arr = new_arr;
    *capacity = new_cap;
    return 0;
}

/* ========================================================================== */
/* Global profile lifecycle                                                   */
/* ========================================================================== */

int vtx_profile_global_init(vtx_profile_global_t *global)
{
    memset(global, 0, sizeof(*global));

    global->method_capacity = VTX_PROFILE_INITIAL_METHOD_CAPACITY;
    global->methods = calloc(global->method_capacity,
                              sizeof(vtx_profile_method_t));
    if (!global->methods) {
        return -1;
    }
    global->method_count = 0;

    global->call_edge_capacity = VTX_PROFILE_INITIAL_EDGE_CAPACITY;
    global->call_edges = calloc(global->call_edge_capacity,
                                 sizeof(vtx_call_edge_t));
    if (!global->call_edges) {
        free(global->methods);
        global->methods = NULL;
        return -1;
    }
    global->call_edge_count = 0;

    global->phase_transition_capacity = 64;
    global->phase_transitions = calloc(global->phase_transition_capacity,
                                        sizeof(vtx_phase_transition_t));
    if (!global->phase_transitions) {
        free(global->call_edges);
        free(global->methods);
        global->methods = NULL;
        global->call_edges = NULL;
        return -1;
    }
    global->phase_transition_count = 0;

    global->phase_graph = NULL;
    return 0;
}

void vtx_profile_global_destroy(vtx_profile_global_t *global)
{
    if (!global) return;

    /* Free per-method internals */
    for (uint32_t i = 0; i < global->method_count; i++) {
        vtx_profile_method_t *m = &global->methods[i];
        free(m->call_sites);
        free(m->branches);
        free(m->field_accesses);
        free(m->loops);
    }
    free(global->methods);

    free(global->call_edges);
    free(global->phase_transitions);

    /* phase_graph is owned and freed separately by phase module */
    global->phase_graph = NULL;

    memset(global, 0, sizeof(*global));
}

/* ========================================================================== */
/* Method profile management                                                  */
/* ========================================================================== */

vtx_profile_method_t *vtx_profile_add_method(vtx_profile_global_t *global,
                                              uint32_t method_id)
{
    /* Check if method already exists */
    for (uint32_t i = 0; i < global->method_count; i++) {
        if (global->methods[i].method_id == method_id) {
            return &global->methods[i];
        }
    }

    /* Grow array if needed */
    if (grow_array((void **)&global->methods, &global->method_count,
                    &global->method_capacity,
                    sizeof(vtx_profile_method_t)) != 0) {
        return NULL;
    }

    vtx_profile_method_t *m = &global->methods[global->method_count];
    memset(m, 0, sizeof(*m));
    m->method_id = method_id;
    global->method_count++;
    return m;
}

vtx_profile_method_t *vtx_profile_get_method(const vtx_profile_global_t *global,
                                              uint32_t method_id)
{
    for (uint32_t i = 0; i < global->method_count; i++) {
        if (global->methods[i].method_id == method_id) {
            return &global->methods[i];
        }
    }
    return NULL;
}

/* ========================================================================== */
/* Internal: find or create a call site entry                                 */
/* ========================================================================== */

static vtx_callsite_profile_t *find_or_create_callsite(
    vtx_profile_method_t *m, uint32_t callsite_index)
{
    /* Linear scan for existing entry */
    for (uint32_t i = 0; i < m->call_site_count; i++) {
        /* We use the array index as the callsite index directly */
    }
    /* For callsite profiles, we use the array indexed by callsite_index.
     * If the array is too small, grow it. Entries are sparse, so we
     * ensure the array is large enough to hold index callsite_index. */
    if (callsite_index >= m->call_site_capacity) {
        uint32_t new_cap = m->call_site_capacity;
        if (new_cap == 0) new_cap = 8;
        while (new_cap <= callsite_index) new_cap *= 2;

        vtx_callsite_profile_t *new_arr = realloc(
            m->call_sites, (size_t)new_cap * sizeof(vtx_callsite_profile_t));
        if (!new_arr) return NULL;

        /* Zero-initialize new entries */
        memset(new_arr + m->call_site_capacity, 0,
               (size_t)(new_cap - m->call_site_capacity) * sizeof(vtx_callsite_profile_t));
        m->call_sites = new_arr;
        m->call_site_capacity = new_cap;
    }

    /* Update count if we extended past it */
    if (callsite_index >= m->call_site_count) {
        m->call_site_count = callsite_index + 1;
    }

    return &m->call_sites[callsite_index];
}

/* ========================================================================== */
/* Internal: find or create a branch entry                                    */
/* ========================================================================== */

static vtx_branch_profile_t *find_or_create_branch(
    vtx_profile_method_t *m, uint32_t bytecode_pc)
{
    /* Linear scan for existing entry */
    for (uint32_t i = 0; i < m->branch_count; i++) {
        if (m->branches[i].bytecode_pc == bytecode_pc) {
            return &m->branches[i];
        }
    }

    /* Grow and add new entry */
    if (grow_array((void **)&m->branches, &m->branch_count,
                    &m->branch_capacity,
                    sizeof(vtx_branch_profile_t)) != 0) {
        return NULL;
    }

    vtx_branch_profile_t *b = &m->branches[m->branch_count];
    memset(b, 0, sizeof(*b));
    b->bytecode_pc = bytecode_pc;
    m->branch_count++;
    return b;
}

/* ========================================================================== */
/* Internal: find or create a field access entry                              */
/* ========================================================================== */

static vtx_field_profile_t *find_or_create_field_access(
    vtx_profile_method_t *m, uint32_t field_offset)
{
    for (uint32_t i = 0; i < m->field_access_count; i++) {
        if (m->field_accesses[i].field_offset == field_offset) {
            return &m->field_accesses[i];
        }
    }

    if (grow_array((void **)&m->field_accesses, &m->field_access_count,
                    &m->field_access_capacity,
                    sizeof(vtx_field_profile_t)) != 0) {
        return NULL;
    }

    vtx_field_profile_t *f = &m->field_accesses[m->field_access_count];
    memset(f, 0, sizeof(*f));
    f->field_offset = field_offset;
    m->field_access_count++;
    return f;
}

/* ========================================================================== */
/* Internal: find or create a loop entry                                      */
/* ========================================================================== */

static vtx_loop_profile_t *find_or_create_loop(
    vtx_profile_method_t *m, uint32_t loop_header_pc)
{
    for (uint32_t i = 0; i < m->loop_count; i++) {
        if (m->loops[i].loop_header_pc == loop_header_pc) {
            return &m->loops[i];
        }
    }

    if (grow_array((void **)&m->loops, &m->loop_count,
                    &m->loop_capacity,
                    sizeof(vtx_loop_profile_t)) != 0) {
        return NULL;
    }

    vtx_loop_profile_t *l = &m->loops[m->loop_count];
    memset(l, 0, sizeof(*l));
    l->loop_header_pc = loop_header_pc;
    m->loop_count++;
    return l;
}

/* ========================================================================== */
/* Recording functions                                                        */
/* ========================================================================== */

void vtx_profile_record_invocation(vtx_profile_global_t *global,
                                    uint32_t method_id)
{
    vtx_profile_method_t *m = vtx_profile_add_method(global, method_id);
    if (m) {
        saturating_inc64(&m->invocation_count);
    }
}

void vtx_profile_record_callsite_type(vtx_profile_global_t *global,
                                       uint32_t method_id,
                                       uint32_t callsite_index,
                                       vtx_typeid_t receiver_type)
{
    vtx_profile_method_t *m = vtx_profile_add_method(global, method_id);
    if (!m) return;

    vtx_callsite_profile_t *cs = find_or_create_callsite(m, callsite_index);
    if (!cs) return;

    if (cs->megamorphic) return;

    /* Check if already recorded */
    for (uint32_t i = 0; i < cs->count; i++) {
        if (cs->types[i] == receiver_type) {
            return; /* already seen */
        }
    }

    /* Add new type */
    if (cs->count < VTX_POLY_LIMIT) {
        cs->types[cs->count] = receiver_type;
        cs->count++;
    } else {
        /* Exceeded polymorphic limit → transition to megamorphic */
        cs->megamorphic = true;
    }
}

void vtx_profile_record_branch(vtx_profile_global_t *global,
                                uint32_t method_id,
                                uint32_t bytecode_pc,
                                bool taken)
{
    vtx_profile_method_t *m = vtx_profile_add_method(global, method_id);
    if (!m) return;

    vtx_branch_profile_t *b = find_or_create_branch(m, bytecode_pc);
    if (!b) return;

    if (taken) {
        saturating_inc64(&b->taken);
    } else {
        saturating_inc64(&b->not_taken);
    }
}

void vtx_profile_record_field_shape(vtx_profile_global_t *global,
                                     uint32_t method_id,
                                     uint32_t field_offset,
                                     vtx_shapeid_t shape_id)
{
    vtx_profile_method_t *m = vtx_profile_add_method(global, method_id);
    if (!m) return;

    vtx_field_profile_t *f = find_or_create_field_access(m, field_offset);
    if (!f) return;

    if (f->megamorphic) return;

    /* Check if already recorded */
    for (uint32_t i = 0; i < f->count; i++) {
        if (f->shapes[i] == shape_id) {
            return;
        }
    }

    /* Add new shape */
    if (f->count < VTX_POLY_LIMIT) {
        f->shapes[f->count] = shape_id;
        f->count++;
    } else {
        f->megamorphic = true;
    }
}

void vtx_profile_record_loop_backedge(vtx_profile_global_t *global,
                                       uint32_t method_id,
                                       uint32_t loop_header_pc)
{
    vtx_profile_method_t *m = vtx_profile_add_method(global, method_id);
    if (!m) return;

    vtx_loop_profile_t *l = find_or_create_loop(m, loop_header_pc);
    if (!l) return;

    saturating_inc64(&l->backedge_count);
}

void vtx_profile_record_call_edge(vtx_profile_global_t *global,
                                   uint32_t caller_method_id,
                                   uint32_t callee_method_id)
{
    /* Check for existing edge */
    for (uint32_t i = 0; i < global->call_edge_count; i++) {
        vtx_call_edge_t *e = &global->call_edges[i];
        if (e->caller_method_id == caller_method_id &&
            e->callee_method_id == callee_method_id) {
            saturating_inc64(&e->frequency);
            return;
        }
    }

    /* Add new edge */
    if (grow_array((void **)&global->call_edges,
                    &global->call_edge_count,
                    &global->call_edge_capacity,
                    sizeof(vtx_call_edge_t)) != 0) {
        return;
    }

    vtx_call_edge_t *e = &global->call_edges[global->call_edge_count];
    e->caller_method_id = caller_method_id;
    e->callee_method_id = callee_method_id;
    e->frequency = 1;
    global->call_edge_count++;
}

/* ========================================================================== */
/* Query helpers                                                              */
/* ========================================================================== */

const vtx_callsite_profile_t *vtx_profile_get_callsite(
    const vtx_profile_global_t *global,
    uint32_t method_id,
    uint32_t callsite_index)
{
    const vtx_profile_method_t *m = vtx_profile_get_method(global, method_id);
    if (!m) return NULL;
    if (callsite_index >= m->call_site_count) return NULL;
    return &m->call_sites[callsite_index];
}

const vtx_branch_profile_t *vtx_profile_get_branch(
    const vtx_profile_global_t *global,
    uint32_t method_id,
    uint32_t bytecode_pc)
{
    const vtx_profile_method_t *m = vtx_profile_get_method(global, method_id);
    if (!m) return NULL;

    for (uint32_t i = 0; i < m->branch_count; i++) {
        if (m->branches[i].bytecode_pc == bytecode_pc) {
            return &m->branches[i];
        }
    }
    return NULL;
}

double vtx_profile_branch_probability(const vtx_branch_profile_t *branch)
{
    if (!branch) return 0.0;
    uint64_t total = branch->taken + branch->not_taken;
    if (total == 0) return 0.0;
    return (double)branch->taken / (double)total;
}

bool vtx_profile_method_is_hot(const vtx_profile_method_t *method,
                                uint64_t threshold)
{
    return method && method->invocation_count >= threshold;
}

/* ========================================================================== */
/* Trip count stability (Proposal #7)                                           */
/* ========================================================================== */

void vtx_profile_record_trip_count(vtx_profile_global_t *global,
                                     uint32_t method_id,
                                     uint32_t loop_header_pc,
                                     uint64_t trip_count)
{
    if (!global) return;

    vtx_profile_method_t *method = vtx_profile_get_method(global, method_id);
    if (!method) {
        method = vtx_profile_add_method(global, method_id);
        if (!method) return;
    }

    /* Find or create the loop profile */
    vtx_loop_profile_t *loop = NULL;
    for (uint32_t i = 0; i < method->loop_count; i++) {
        if (method->loops[i].loop_header_pc == loop_header_pc) {
            loop = &method->loops[i];
            break;
        }
    }

    if (loop == NULL) {
        /* Create a new loop profile */
        if (method->loop_count >= method->loop_capacity) {
            uint32_t new_cap = method->loop_capacity == 0 ? 8 : method->loop_capacity * 2;
            vtx_loop_profile_t *new_loops = (vtx_loop_profile_t *)realloc(
                method->loops, new_cap * sizeof(vtx_loop_profile_t));
            if (!new_loops) return;
            memset(new_loops + method->loop_count, 0,
                   (new_cap - method->loop_count) * sizeof(vtx_loop_profile_t));
            method->loops = new_loops;
            method->loop_capacity = new_cap;
        }
        loop = &method->loops[method->loop_count++];
        loop->loop_header_pc = loop_header_pc;
        loop->backedge_count = 0;
        loop->last_trip_count = 0;
        loop->trip_stability_count = 0;
        loop->is_trip_stable = false;
    }

    /* Update trip count stability */
    if (loop->last_trip_count == trip_count && trip_count > 0) {
        if (loop->trip_stability_count < UINT32_MAX) {
            loop->trip_stability_count++;
        }
        if (loop->trip_stability_count >= VTX_TRIP_STABILITY_WINDOW) {
            loop->is_trip_stable = true;
        }
    } else {
        loop->last_trip_count = trip_count;
        loop->trip_stability_count = 1;
        loop->is_trip_stable = false;
    }

    /* Also increment backedge count (saturating) */
    if (loop->backedge_count < UINT64_MAX - trip_count) {
        loop->backedge_count += trip_count;
    } else {
        loop->backedge_count = UINT64_MAX;
    }
}

bool vtx_profile_is_trip_stable(const vtx_profile_global_t *global,
                                  uint32_t method_id,
                                  uint32_t loop_header_pc)
{
    if (!global) return false;

    const vtx_profile_method_t *method = vtx_profile_get_method(global, method_id);
    if (!method) return false;

    for (uint32_t i = 0; i < method->loop_count; i++) {
        if (method->loops[i].loop_header_pc == loop_header_pc) {
            return method->loops[i].is_trip_stable;
        }
    }
    return false;
}

uint64_t vtx_profile_get_stable_trip_count(const vtx_profile_global_t *global,
                                              uint32_t method_id,
                                              uint32_t loop_header_pc)
{
    if (!global) return 0;

    const vtx_profile_method_t *method = vtx_profile_get_method(global, method_id);
    if (!method) return 0;

    for (uint32_t i = 0; i < method->loop_count; i++) {
        if (method->loops[i].loop_header_pc == loop_header_pc) {
            if (method->loops[i].is_trip_stable) {
                return method->loops[i].last_trip_count;
            }
            return 0;
        }
    }
    return 0;
}
