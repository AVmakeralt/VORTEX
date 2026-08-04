/* ============================================================================ *
 * AI-GENERATED CODE
 *
 * This file was written from scratch by an AI assistant (GLM/Z.ai).
 * It is part of the VORTEX JIT compiler project.
 *
 * Human-written code lives in: src/interp/ (dispatch loop), src/baseline/
 * (codegen), src/runtime/ (GC, type system, arena), src/main_new.c.
 *
 * If reviewing, please verify correctness independently.
 * ============================================================================ */

/**
 * VORTEX Input-Shape-Keyed Profiles (Sprint 4) — Implementation
 *
 * See input_shape.h for design rationale.
 *
 * This is the novel differentiator: no production JIT does input-shape-
 * keyed profiling. Database query planners do (parameter-sensitive plan
 * caching). Bringing that to a JIT is the paper-worthy contribution.
 */

#include "profile/input_shape.h"
#include "profile/merge.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ========================================================================== */
/* Internal helpers                                                            */
/* ========================================================================== */

static uint64_t shape_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ========================================================================== */
/* Size binning                                                                */
/* ========================================================================== */

vtx_size_bin_t vtx_size_bin(uint64_t count)
{
    if (count == 0) return VTX_SIZE_BIN_EMPTY;
    if (count == 1) return VTX_SIZE_BIN_ONE;
    if (count <= 4) return VTX_SIZE_BIN_TINY;
    if (count <= 16) return VTX_SIZE_BIN_SMALL;
    if (count <= 256) return VTX_SIZE_BIN_MEDIUM;
    return VTX_SIZE_BIN_LARGE;
}

const char *vtx_size_bin_name(vtx_size_bin_t bin)
{
    switch (bin) {
        case VTX_SIZE_BIN_EMPTY:  return "empty(0)";
        case VTX_SIZE_BIN_ONE:    return "one(1)";
        case VTX_SIZE_BIN_TINY:   return "tiny(2-4)";
        case VTX_SIZE_BIN_SMALL:  return "small(5-16)";
        case VTX_SIZE_BIN_MEDIUM: return "medium(17-256)";
        case VTX_SIZE_BIN_LARGE:  return "large(257+)";
        default:                  return "unknown";
    }
}

/* ========================================================================== */
/* Shape signature computation                                                 */
/* ========================================================================== */

vtx_input_shape_t vtx_input_shape_make(
    uint8_t arg_type_fingerprint,
    uint8_t receiver_type_fingerprint,
    vtx_size_bin_t dominant_size_bin,
    vtx_size_bin_t dominant_trip_bin,
    uint8_t call_site_count,
    uint8_t field_shape_fingerprint)
{
    /* Layout:
     *   bits[63:56] = arg type fingerprint
     *   bits[55:48] = receiver type fingerprint
     *   bits[47:44] = dominant size bin (4 bits, 0-5)
     *   bits[43:40] = dominant trip bin (4 bits, 0-5)
     *   bits[39:32] = call site count
     *   bits[31:24] = field shape fingerprint
     *   bits[23:16] = reserved (0)
     *   bits[15:0]  = version
     */
    vtx_input_shape_t shape = 0;
    shape |= ((vtx_input_shape_t)arg_type_fingerprint) << 56;
    shape |= ((vtx_input_shape_t)receiver_type_fingerprint) << 48;
    shape |= ((vtx_input_shape_t)(dominant_size_bin & 0xF)) << 44;
    shape |= ((vtx_input_shape_t)(dominant_trip_bin & 0xF)) << 40;
    shape |= ((vtx_input_shape_t)call_site_count) << 32;
    shape |= ((vtx_input_shape_t)field_shape_fingerprint) << 24;
    shape |= ((vtx_input_shape_t)VTX_INPUT_SHAPE_VERSION);
    return shape;
}

bool vtx_input_shape_equals(vtx_input_shape_t a, vtx_input_shape_t b)
{
    return a == b;
}

vtx_size_bin_t vtx_input_shape_size_bin(vtx_input_shape_t shape)
{
    return (vtx_size_bin_t)((shape >> 44) & 0xF);
}

vtx_size_bin_t vtx_input_shape_trip_bin(vtx_input_shape_t shape)
{
    return (vtx_size_bin_t)((shape >> 40) & 0xF);
}

/* ========================================================================== */
/* Per-method shape table                                                      */
/* ========================================================================== */

int vtx_input_shape_table_init(vtx_input_shape_table_t *table,
                                 uint32_t method_id)
{
    if (table == NULL) return -1;
    memset(table, 0, sizeof(*table));
    table->method_id = method_id;
    table->entry_count = 0;
    table->total_observations = 0;
    table->shape_transitions = 0;
    table->shape_evictions = 0;

    /* Create the default shape entry so observations can be recorded
     * even before shape-keying kicks in. */
    vtx_shape_profile_entry_t *def = &table->entries[0];
    memset(def, 0, sizeof(*def));
    def->shape = VTX_INPUT_SHAPE_DEFAULT;
    def->valid = true;
    def->first_seen_ns = shape_now_ns();
    def->last_seen_ns = def->first_seen_ns;
    /* Initialize the embedded profile method. */
    /* Note: vtx_profile_method_t is zero-initialized by memset above.
     * The arrays (call_sites, branches, etc.) start NULL and are
     * allocated on first use by the profile recording API. */
    table->entry_count = 1;
    return 0;
}

void vtx_input_shape_table_destroy(vtx_input_shape_table_t *table)
{
    if (table == NULL) return;
    for (uint32_t i = 0; i < VTX_INPUT_SHAPE_MAX_PER_METHOD; i++) {
        if (table->entries[i].valid) {
            /* Free the embedded profile method's arrays. */
            vtx_profile_method_t *m = &table->entries[i].method;
            if (m->call_sites)    free(m->call_sites);
            if (m->branches)      free(m->branches);
            if (m->field_accesses) free(m->field_accesses);
            if (m->loops)         free(m->loops);
            table->entries[i].valid = false;
        }
    }
    table->entry_count = 0;
}

/* Find the index of an entry for the given shape, or UINT32_MAX if not found. */
static uint32_t find_entry(const vtx_input_shape_table_t *table,
                             vtx_input_shape_t shape)
{
    for (uint32_t i = 0; i < VTX_INPUT_SHAPE_MAX_PER_METHOD; i++) {
        if (table->entries[i].valid && table->entries[i].shape == shape) {
            return i;
        }
    }
    return UINT32_MAX;
}

/* Find the LRU entry (smallest last_seen_ns) for eviction.
 * Never evicts the default shape (index 0). */
static uint32_t find_lru_entry(const vtx_input_shape_table_t *table)
{
    uint32_t lru_idx = UINT32_MAX;
    uint64_t lru_time = UINT64_MAX;
    for (uint32_t i = 1; i < VTX_INPUT_SHAPE_MAX_PER_METHOD; i++) {
        if (!table->entries[i].valid) continue;
        if (table->entries[i].last_seen_ns < lru_time) {
            lru_time = table->entries[i].last_seen_ns;
            lru_idx = i;
        }
    }
    return lru_idx;
}

vtx_profile_method_t *vtx_input_shape_table_get_or_create(
    vtx_input_shape_table_t *table,
    vtx_input_shape_t shape,
    uint64_t now_ns_)
{
    if (table == NULL) return NULL;
    uint64_t now = (now_ns_ == 0) ? shape_now_ns() : now_ns_;

    /* Look for an existing entry. */
    uint32_t idx = find_entry(table, shape);
    if (idx != UINT32_MAX) {
        table->entries[idx].last_seen_ns = now;
        return &table->entries[idx].method;
    }

    /* Find a free slot. */
    uint32_t free_idx = UINT32_MAX;
    for (uint32_t i = 0; i < VTX_INPUT_SHAPE_MAX_PER_METHOD; i++) {
        if (!table->entries[i].valid) {
            free_idx = i;
            break;
        }
    }

    /* If no free slot, evict the LRU (never the default at index 0). */
    if (free_idx == UINT32_MAX) {
        free_idx = find_lru_entry(table);
        if (free_idx == UINT32_MAX) {
            /* Only the default exists — return it. */
            return &table->entries[0].method;
        }

        /* Merge the evicted shape's data into the default.
         * BUGFIX P8: The old code called vtx_profile_merge_method(&def->method,
         * &evicted->method), but that function takes (vtx_profile_global_t*,
         * vtx_profile_method_t*) — passing a method struct as the global
         * argument causes it to interpret method fields as global struct
         * fields, reading garbage pointers and causing heap corruption.
         *
         * Fix: merge method-to-method directly. We inline the merge logic
         * because there's no existing method-to-method merge API. The logic
         * is: sum invocation counts, merge branches/callsites/loops/fields
         * by finding or creating matching entries. */
        vtx_shape_profile_entry_t *evicted = &table->entries[free_idx];
        vtx_shape_profile_entry_t *def = &table->entries[0];
        vtx_profile_method_t *dst = &def->method;
        const vtx_profile_method_t *src = &evicted->method;

        /* Sum invocation counts (saturating). */
        uint64_t inv_sum = dst->invocation_count + src->invocation_count;
        dst->invocation_count = (inv_sum < dst->invocation_count) ? UINT64_MAX : inv_sum;

        /* Merge branches by bytecode_pc. */
        for (uint32_t b = 0; b < src->branch_count; b++) {
            const vtx_branch_profile_t *sb = &src->branches[b];
            vtx_branch_profile_t *db = NULL;
            for (uint32_t j = 0; j < dst->branch_count; j++) {
                if (dst->branches[j].bytecode_pc == sb->bytecode_pc) {
                    db = &dst->branches[j];
                    break;
                }
            }
            if (db == NULL) {
                /* Grow dst->branches if needed. */
                if (dst->branch_count >= dst->branch_capacity) {
                    uint32_t new_cap = dst->branch_capacity == 0 ? 8 : dst->branch_capacity * 2;
                    vtx_branch_profile_t *new_arr = realloc(dst->branches,
                        (size_t)new_cap * sizeof(vtx_branch_profile_t));
                    if (new_arr == NULL) continue;
                    memset(new_arr + dst->branch_capacity, 0,
                           (size_t)(new_cap - dst->branch_capacity) * sizeof(vtx_branch_profile_t));
                    dst->branches = new_arr;
                    dst->branch_capacity = new_cap;
                }
                db = &dst->branches[dst->branch_count++];
                memset(db, 0, sizeof(*db));
                db->bytecode_pc = sb->bytecode_pc;
            }
            uint64_t t_sum = db->taken + sb->taken;
            db->taken = (t_sum < db->taken) ? UINT64_MAX : t_sum;
            uint64_t n_sum = db->not_taken + sb->not_taken;
            db->not_taken = (n_sum < db->not_taken) ? UINT64_MAX : n_sum;
        }

        /* Merge callsites by index (union types). */
        for (uint32_t c = 0; c < src->call_site_count; c++) {
            const vtx_callsite_profile_t *sc = &src->call_sites[c];
            if (c >= dst->call_site_capacity) {
                uint32_t new_cap = dst->call_site_capacity == 0 ? 8 : dst->call_site_capacity * 2;
                while (new_cap <= c) new_cap *= 2;
                vtx_callsite_profile_t *new_arr = realloc(dst->call_sites,
                    (size_t)new_cap * sizeof(vtx_callsite_profile_t));
                if (new_arr == NULL) continue;
                memset(new_arr + dst->call_site_capacity, 0,
                       (size_t)(new_cap - dst->call_site_capacity) * sizeof(vtx_callsite_profile_t));
                dst->call_sites = new_arr;
                dst->call_site_capacity = new_cap;
            }
            if (c >= dst->call_site_count) {
                dst->call_site_count = c + 1;
            }
            vtx_callsite_profile_t *dc = &dst->call_sites[c];
            if (sc->megamorphic) dc->megamorphic = true;
            if (dc->megamorphic) continue;
            for (uint32_t t = 0; t < sc->count; t++) {
                vtx_typeid_t ty = sc->types[t];
                bool found = false;
                for (uint32_t j = 0; j < dc->count; j++) {
                    if (dc->types[j] == ty) { found = true; break; }
                }
                if (!found && dc->count < VTX_POLY_LIMIT) {
                    dc->types[dc->count++] = ty;
                } else if (!found) {
                    dc->megamorphic = true;
                    break;
                }
            }
        }

        /* Merge loops by loop_header_pc. */
        for (uint32_t l = 0; l < src->loop_count; l++) {
            const vtx_loop_profile_t *sl = &src->loops[l];
            vtx_loop_profile_t *dl = NULL;
            for (uint32_t j = 0; j < dst->loop_count; j++) {
                if (dst->loops[j].loop_header_pc == sl->loop_header_pc) {
                    dl = &dst->loops[j];
                    break;
                }
            }
            if (dl == NULL) {
                if (dst->loop_count >= dst->loop_capacity) {
                    uint32_t new_cap = dst->loop_capacity == 0 ? 8 : dst->loop_capacity * 2;
                    vtx_loop_profile_t *new_arr = realloc(dst->loops,
                        (size_t)new_cap * sizeof(vtx_loop_profile_t));
                    if (new_arr == NULL) continue;
                    memset(new_arr + dst->loop_capacity, 0,
                           (size_t)(new_cap - dst->loop_capacity) * sizeof(vtx_loop_profile_t));
                    dst->loops = new_arr;
                    dst->loop_capacity = new_cap;
                }
                dl = &dst->loops[dst->loop_count++];
                memset(dl, 0, sizeof(*dl));
                dl->loop_header_pc = sl->loop_header_pc;
            }
            uint64_t be_sum = dl->backedge_count + sl->backedge_count;
            dl->backedge_count = (be_sum < dl->backedge_count) ? UINT64_MAX : be_sum;
        }

        /* Merge field accesses by field_offset (union shapes). */
        for (uint32_t f = 0; f < src->field_access_count; f++) {
            const vtx_field_profile_t *sf = &src->field_accesses[f];
            vtx_field_profile_t *df = NULL;
            for (uint32_t j = 0; j < dst->field_access_count; j++) {
                if (dst->field_accesses[j].field_offset == sf->field_offset) {
                    df = &dst->field_accesses[j];
                    break;
                }
            }
            if (df == NULL) {
                if (dst->field_access_count >= dst->field_access_capacity) {
                    uint32_t new_cap = dst->field_access_capacity == 0 ? 8 : dst->field_access_capacity * 2;
                    vtx_field_profile_t *new_arr = realloc(dst->field_accesses,
                        (size_t)new_cap * sizeof(vtx_field_profile_t));
                    if (new_arr == NULL) continue;
                    memset(new_arr + dst->field_access_capacity, 0,
                           (size_t)(new_cap - dst->field_access_capacity) * sizeof(vtx_field_profile_t));
                    dst->field_accesses = new_arr;
                    dst->field_access_capacity = new_cap;
                }
                df = &dst->field_accesses[dst->field_access_count++];
                memset(df, 0, sizeof(*df));
                df->field_offset = sf->field_offset;
            }
            if (sf->megamorphic) df->megamorphic = true;
            if (df->megamorphic) continue;
            for (uint32_t s = 0; s < sf->count; s++) {
                vtx_shapeid_t sh = sf->shapes[s];
                bool found = false;
                for (uint32_t j = 0; j < df->count; j++) {
                    if (df->shapes[j] == sh) { found = true; break; }
                }
                if (!found && df->count < VTX_POLY_LIMIT) {
                    df->shapes[df->count++] = sh;
                } else if (!found) {
                    df->megamorphic = true;
                    break;
                }
            }
        }

        /* Free the evicted entry's arrays. */
        if (evicted->method.call_sites)    { free(evicted->method.call_sites);    evicted->method.call_sites = NULL; }
        if (evicted->method.branches)      { free(evicted->method.branches);      evicted->method.branches = NULL; }
        if (evicted->method.field_accesses) { free(evicted->method.field_accesses); evicted->method.field_accesses = NULL; }
        if (evicted->method.loops)         { free(evicted->method.loops);         evicted->method.loops = NULL; }

        evicted->valid = false;
        table->entry_count--;
        table->shape_evictions++;
    }

    /* Initialize the new entry. */
    vtx_shape_profile_entry_t *entry = &table->entries[free_idx];
    memset(entry, 0, sizeof(*entry));
    entry->shape = shape;
    entry->valid = true;
    entry->first_seen_ns = now;
    entry->last_seen_ns = now;
    entry->sample_count = 0;
    /* The embedded vtx_profile_method_t is zero-initialized by memset.
     * Arrays will be allocated on first use. */
    table->entry_count++;
    table->shape_transitions++;

    return &entry->method;
}

vtx_profile_method_t *vtx_input_shape_table_get_default(
    vtx_input_shape_table_t *table)
{
    if (table == NULL) return NULL;
    if (!table->entries[0].valid) return NULL;
    return &table->entries[0].method;
}

vtx_profile_method_t *vtx_input_shape_table_get(
    vtx_input_shape_table_t *table,
    vtx_input_shape_t shape)
{
    if (table == NULL) return NULL;
    uint32_t idx = find_entry(table, shape);
    if (idx == UINT32_MAX) return NULL;
    return &table->entries[idx].method;
}

void vtx_input_shape_table_record_observation(
    vtx_input_shape_table_t *table,
    vtx_input_shape_t shape,
    uint64_t now_ns_)
{
    if (table == NULL) return;
    uint64_t now = (now_ns_ == 0) ? shape_now_ns() : now_ns_;

    uint32_t idx = find_entry(table, shape);
    if (idx == UINT32_MAX) {
        /* Shape doesn't exist — create it (get_or_create handles eviction). */
        vtx_input_shape_table_get_or_create(table, shape, now);
        idx = find_entry(table, shape);
        if (idx == UINT32_MAX) return;
    }

    table->entries[idx].sample_count++;
    table->entries[idx].last_seen_ns = now;
    table->total_observations++;
}

/* ========================================================================== */
/* Queries                                                                     */
/* ========================================================================== */

uint32_t vtx_input_shape_table_shape_count(const vtx_input_shape_table_t *table)
{
    if (table == NULL) return 0;
    return table->entry_count;
}

vtx_input_shape_t vtx_input_shape_table_dominant_shape(
    const vtx_input_shape_table_t *table)
{
    if (table == NULL) return VTX_INPUT_SHAPE_DEFAULT;

    vtx_input_shape_t dominant = VTX_INPUT_SHAPE_DEFAULT;
    uint64_t max_samples = 0;

    for (uint32_t i = 0; i < VTX_INPUT_SHAPE_MAX_PER_METHOD; i++) {
        if (!table->entries[i].valid) continue;
        if (table->entries[i].sample_count > max_samples) {
            max_samples = table->entries[i].sample_count;
            dominant = table->entries[i].shape;
        }
    }
    return dominant;
}

bool vtx_input_shape_table_needs_multi_version(
    const vtx_input_shape_table_t *table)
{
    if (table == NULL) return false;

    /* Count shapes with enough samples for multi-version compilation. */
    uint32_t hot_shapes = 0;
    for (uint32_t i = 0; i < VTX_INPUT_SHAPE_MAX_PER_METHOD; i++) {
        if (!table->entries[i].valid) continue;
        if (table->entries[i].sample_count >=
            VTX_INPUT_SHAPE_MIN_SAMPLES_FOR_MULTI_VERSION) {
            hot_shapes++;
        }
    }
    return hot_shapes >= 2;
}

/* ========================================================================== */
/* Statistics                                                                  */
/* ========================================================================== */

void vtx_input_shape_table_stats(const vtx_input_shape_table_t *table,
                                   uint32_t *shape_count,
                                   uint64_t *total_observations,
                                   uint64_t *transitions,
                                   uint64_t *evictions,
                                   vtx_input_shape_t *dominant_shape)
{
    if (table == NULL) {
        if (shape_count) *shape_count = 0;
        if (total_observations) *total_observations = 0;
        if (transitions) *transitions = 0;
        if (evictions) *evictions = 0;
        if (dominant_shape) *dominant_shape = VTX_INPUT_SHAPE_DEFAULT;
        return;
    }
    if (shape_count) *shape_count = table->entry_count;
    if (total_observations) *total_observations = table->total_observations;
    if (transitions) *transitions = table->shape_transitions;
    if (evictions) *evictions = table->shape_evictions;
    if (dominant_shape) *dominant_shape = vtx_input_shape_table_dominant_shape(table);
}

/* ========================================================================== */
/* Global shape manager                                                        */
/* ========================================================================== */

int vtx_input_shape_manager_init(vtx_input_shape_manager_t *mgr)
{
    if (mgr == NULL) return -1;
    memset(mgr, 0, sizeof(*mgr));
    mgr->table_capacity = 64;
    mgr->tables = (vtx_input_shape_table_t **)calloc(
        mgr->table_capacity, sizeof(vtx_input_shape_table_t *));
    if (mgr->tables == NULL) {
        mgr->table_capacity = 0;
        return -1;
    }
    mgr->table_count = 0;
    mgr->total_shape_observations = 0;
    mgr->total_multi_version_methods = 0;
    return 0;
}

void vtx_input_shape_manager_destroy(vtx_input_shape_manager_t *mgr)
{
    if (mgr == NULL) return;
    if (mgr->tables != NULL) {
        for (uint32_t i = 0; i < mgr->table_count; i++) {
            if (mgr->tables[i] != NULL) {
                vtx_input_shape_table_destroy(mgr->tables[i]);
                free(mgr->tables[i]);
                mgr->tables[i] = NULL;
            }
        }
        free(mgr->tables);
        mgr->tables = NULL;
    }
    mgr->table_count = 0;
    mgr->table_capacity = 0;
}

/* Ensure the tables array can accommodate the given method_id. */
static bool ensure_capacity(vtx_input_shape_manager_t *mgr, uint32_t method_id)
{
    if (method_id < mgr->table_capacity) return true;

    /* BUGFIX P21: The old loop `while (new_cap <= method_id) new_cap *= 2`
     * overflows to 0 when method_id is near UINT32_MAX, causing an
     * infinite loop. Fix: check for overflow and cap at a sane maximum. */
    uint32_t new_cap = mgr->table_capacity;
    while (new_cap <= method_id) {
        if (new_cap >= (UINT32_MAX / 2)) {
            /* Would overflow — cap at UINT32_MAX. */
            new_cap = UINT32_MAX;
            break;
        }
        new_cap *= 2;
    }
    if (new_cap <= method_id) return false;  /* still too small — give up */

    vtx_input_shape_table_t **new_tables = (vtx_input_shape_table_t **)realloc(
        mgr->tables, (size_t)new_cap * sizeof(vtx_input_shape_table_t *));
    if (new_tables == NULL) return false;
    memset(new_tables + mgr->table_capacity, 0,
           (new_cap - mgr->table_capacity) * sizeof(vtx_input_shape_table_t *));
    mgr->tables = new_tables;
    mgr->table_capacity = new_cap;
    return true;
}

vtx_input_shape_table_t *vtx_input_shape_manager_get_or_create(
    vtx_input_shape_manager_t *mgr,
    uint32_t method_id)
{
    if (mgr == NULL) return NULL;
    if (!ensure_capacity(mgr, method_id)) return NULL;

    if (method_id >= mgr->table_count) {
        mgr->table_count = method_id + 1;
    }

    if (mgr->tables[method_id] == NULL) {
        mgr->tables[method_id] = (vtx_input_shape_table_t *)calloc(
            1, sizeof(vtx_input_shape_table_t));
        if (mgr->tables[method_id] == NULL) return NULL;
        if (vtx_input_shape_table_init(mgr->tables[method_id], method_id) != 0) {
            free(mgr->tables[method_id]);
            mgr->tables[method_id] = NULL;
            return NULL;
        }
    }
    return mgr->tables[method_id];
}

vtx_input_shape_table_t *vtx_input_shape_manager_get(
    vtx_input_shape_manager_t *mgr,
    uint32_t method_id)
{
    if (mgr == NULL) return NULL;
    if (method_id >= mgr->table_count) return NULL;
    return mgr->tables[method_id];
}

vtx_profile_method_t *vtx_input_shape_manager_get_profile(
    vtx_input_shape_manager_t *mgr,
    uint32_t method_id,
    vtx_input_shape_t shape)
{
    vtx_input_shape_table_t *table = vtx_input_shape_manager_get(mgr, method_id);
    if (table == NULL) return NULL;

    /* Try the specific shape first. */
    vtx_profile_method_t *m = vtx_input_shape_table_get(table, shape);
    if (m != NULL) return m;

    /* Fall back to the default shape. */
    return vtx_input_shape_table_get_default(table);
}

void vtx_input_shape_manager_stats(const vtx_input_shape_manager_t *mgr,
                                     uint64_t *total_observations,
                                     uint64_t *multi_version_methods,
                                     uint32_t *table_count)
{
    if (mgr == NULL) {
        if (total_observations) *total_observations = 0;
        if (multi_version_methods) *multi_version_methods = 0;
        if (table_count) *table_count = 0;
        return;
    }
    if (total_observations) *total_observations = mgr->total_shape_observations;
    if (multi_version_methods) *multi_version_methods = mgr->total_multi_version_methods;
    if (table_count) *table_count = mgr->table_count;
}
