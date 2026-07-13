/* ============================================================================ *
 * AI-MODIFIED CODE
 *
 * This file was originally written by a human developer. It has been
 * substantially modified by an AI assistant (GLM/Z.ai) for bug fixes,
 * performance improvements, and feature additions.
 *
 * Original human-written structure is preserved; AI changes are marked
 * with bug fix IDs (B1-B28) or perf notes (Perf 1-10) in comments.
 *
 * If reviewing, please verify AI changes against the original logic.
 * ============================================================================ */

#include "sota/recomp.h"
#include "interp/type_feedback.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* ========================================================================== */
/* Profile snapshot                                                            */
/* ========================================================================== */

/**
 * Snapshot of a method's profile at compilation time.
 * Stores the type distribution at each call site.
 */
typedef struct vtx_recomp_snapshot {
    uint32_t method_id;        /* method this snapshot is for */
    bool     valid;            /* true if snapshot data is populated */

    /* Per-call-site type distribution at compilation time.
     * Each entry stores the type IDs and their frequencies. */
    vtx_callsite_profile_t *call_sites;  /* array of call site snapshots */
    uint32_t                call_site_count;
} vtx_recomp_snapshot_t;

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

int vtx_sota_recomp_init(vtx_sota_recomp_t *recomp)
{
    if (recomp == NULL) return -1;

    recomp->snapshot_capacity = 64;
    recomp->snapshots = (vtx_recomp_snapshot_t *)calloc(
        recomp->snapshot_capacity, sizeof(vtx_recomp_snapshot_t));
    if (recomp->snapshots == NULL) {
        recomp->snapshot_capacity = 0;
        return -1;
    }

    recomp->snapshot_count = 0;

    /* Initialize recompilation queue */
    recomp->recomp_queue_capacity = 32;
    recomp->recomp_queue = (vtx_recomp_queue_entry_t *)calloc(
        recomp->recomp_queue_capacity, sizeof(vtx_recomp_queue_entry_t));
    if (recomp->recomp_queue == NULL) {
        free(recomp->snapshots);
        recomp->snapshots = NULL;
        recomp->snapshot_capacity = 0;
        recomp->recomp_queue_capacity = 0;
        return -1;
    }
    recomp->recomp_queue_count = 0;

    /* Sprint 1.2: Initialize hysteresis hash table.
     * Capacity is a power of 2 for fast modulo. Start with 128 slots —
     * the table grows if load factor exceeds 0.75. */
    recomp->hysteresis_capacity = 128;
    recomp->hysteresis = (vtx_recomp_hysteresis_t *)calloc(
        recomp->hysteresis_capacity, sizeof(vtx_recomp_hysteresis_t));
    if (recomp->hysteresis == NULL) {
        free(recomp->recomp_queue);
        free(recomp->snapshots);
        recomp->snapshots = NULL;
        recomp->snapshot_capacity = 0;
        recomp->recomp_queue = NULL;
        recomp->recomp_queue_capacity = 0;
        recomp->hysteresis_capacity = 0;
        return -1;
    }
    recomp->hysteresis_count = 0;

    recomp->total_checks = 0;
    recomp->total_recompilations_triggered = 0;
    recomp->total_false_positives = 0;
    recomp->total_hysteresis_blocks = 0;
    recomp->total_dropped_soft_cap = 0;
    recomp->total_dropped_hard_cap = 0;
    recomp->total_coalesced = 0;

    return 0;
}

void vtx_sota_recomp_destroy(vtx_sota_recomp_t *recomp)
{
    if (recomp == NULL) return;

    if (recomp->snapshots != NULL) {
        for (uint32_t i = 0; i < recomp->snapshot_count; i++) {
            vtx_recomp_snapshot_t *snap = &recomp->snapshots[i];
            if (snap->call_sites != NULL) {
                free(snap->call_sites);
            }
        }
        free(recomp->snapshots);
        recomp->snapshots = NULL;
    }

    if (recomp->recomp_queue != NULL) {
        free(recomp->recomp_queue);
        recomp->recomp_queue = NULL;
    }

    if (recomp->hysteresis != NULL) {
        free(recomp->hysteresis);
        recomp->hysteresis = NULL;
    }

    recomp->snapshot_count = 0;
    recomp->snapshot_capacity = 0;
    recomp->recomp_queue_count = 0;
    recomp->recomp_queue_capacity = 0;
    recomp->hysteresis_count = 0;
    recomp->hysteresis_capacity = 0;
}

/* ========================================================================== */
/* Internal: find or create snapshot slot                                      */
/* ========================================================================== */

static vtx_recomp_snapshot_t *find_snapshot(vtx_sota_recomp_t *recomp,
                                              uint32_t method_id)
{
    for (uint32_t i = 0; i < recomp->snapshot_count; i++) {
        if (recomp->snapshots[i].method_id == method_id && recomp->snapshots[i].valid) {
            return &recomp->snapshots[i];
        }
    }
    return NULL;
}

static vtx_recomp_snapshot_t *create_snapshot_slot(vtx_sota_recomp_t *recomp,
                                                     uint32_t method_id)
{
    /* Check if there's an invalid slot we can reuse */
    for (uint32_t i = 0; i < recomp->snapshot_count; i++) {
        if (!recomp->snapshots[i].valid) {
            recomp->snapshots[i].method_id = method_id;
            recomp->snapshots[i].valid = true;
            return &recomp->snapshots[i];
        }
    }

    /* Grow if needed */
    if (recomp->snapshot_count >= recomp->snapshot_capacity) {
        uint32_t new_cap = recomp->snapshot_capacity * 2;
        vtx_recomp_snapshot_t *new_snaps = (vtx_recomp_snapshot_t *)realloc(
            recomp->snapshots, new_cap * sizeof(vtx_recomp_snapshot_t));
        if (new_snaps == NULL) return NULL;
        memset(new_snaps + recomp->snapshot_capacity, 0,
               (new_cap - recomp->snapshot_capacity) * sizeof(vtx_recomp_snapshot_t));
        recomp->snapshots = new_snaps;
        recomp->snapshot_capacity = new_cap;
    }

    vtx_recomp_snapshot_t *snap = &recomp->snapshots[recomp->snapshot_count];
    memset(snap, 0, sizeof(*snap));
    snap->method_id = method_id;
    snap->valid = true;
    recomp->snapshot_count++;
    return snap;
}

/* ========================================================================== */
/* Snapshot management                                                         */
/* ========================================================================== */

int vtx_sota_recomp_save_snapshot(vtx_sota_recomp_t *recomp,
                                    uint32_t method_id,
                                    const vtx_profile_global_t *profile)
{
    if (recomp == NULL || profile == NULL) return -1;

    const vtx_profile_method_t *method = vtx_profile_get_method(profile, method_id);
    if (method == NULL) {
        /* No profile data for this method — save an empty snapshot */
        vtx_recomp_snapshot_t *snap = create_snapshot_slot(recomp, method_id);
        if (snap == NULL) return -1;
        snap->call_sites = NULL;
        snap->call_site_count = 0;
        return 0;
    }

    vtx_recomp_snapshot_t *snap = create_snapshot_slot(recomp, method_id);
    if (snap == NULL) return -1;

    /* Free previous call sites if any */
    if (snap->call_sites != NULL) {
        free(snap->call_sites);
        snap->call_sites = NULL;
    }

    /* Copy call site profiles */
    if (method->call_site_count > 0) {
        snap->call_sites = (vtx_callsite_profile_t *)malloc(
            method->call_site_count * sizeof(vtx_callsite_profile_t));
        if (snap->call_sites == NULL) {
            snap->call_site_count = 0;
            return -1;
        }
        memcpy(snap->call_sites, method->call_sites,
               method->call_site_count * sizeof(vtx_callsite_profile_t));
        snap->call_site_count = method->call_site_count;
    } else {
        snap->call_sites = NULL;
        snap->call_site_count = 0;
    }

    return 0;
}

void vtx_sota_recomp_remove_snapshot(vtx_sota_recomp_t *recomp,
                                       uint32_t method_id)
{
    if (recomp == NULL) return;

    vtx_recomp_snapshot_t *snap = find_snapshot(recomp, method_id);
    if (snap == NULL) return;

    if (snap->call_sites != NULL) {
        free(snap->call_sites);
        snap->call_sites = NULL;
    }
    snap->call_site_count = 0;
    snap->valid = false;
}

/* ========================================================================== */
/* KL divergence                                                               */
/* ========================================================================== */

double vtx_kl_divergence(const vtx_typeid_t *types_a, const uint64_t *freqs_a, uint32_t count_a,
                           const vtx_typeid_t *types_b, const uint64_t *freqs_b, uint32_t count_b)
{
    /* Compute total frequencies for normalization */
    double total_a = 0.0;
    double total_b = 0.0;

    for (uint32_t i = 0; i < count_a; i++) total_a += (double)freqs_a[i];
    for (uint32_t i = 0; i < count_b; i++) total_b += (double)freqs_b[i];

    if (total_a == 0.0 || total_b == 0.0) return 0.0;

    /* Build a merged type set and compute probabilities.
     * We use a simple O(n*m) merge since the arrays are small (<= VTX_POLY_LIMIT). */
    double kl = 0.0;
    const double epsilon = 1e-6; /* smoothing to avoid ln(0) */

    /* For each type in distribution A (current), compute KL contribution */
    for (uint32_t i = 0; i < count_a; i++) {
        double p_i = ((double)freqs_a[i] + epsilon) / (total_a + epsilon * count_a);

        /* Find matching type in distribution B (compiled) */
        double q_i = epsilon / (total_b + epsilon * count_b); /* default: very small */
        for (uint32_t j = 0; j < count_b; j++) {
            if (types_b[j] == types_a[i]) {
                q_i = ((double)freqs_b[j] + epsilon) / (total_b + epsilon * count_b);
                break;
            }
        }

        /* KL contribution: P(i) * ln(P(i) / Q(i)) */
        if (p_i > 0.0 && q_i > 0.0) {
            kl += p_i * log(p_i / q_i);
        }
    }

    /* KL divergence is non-negative by definition */
    return kl > 0.0 ? kl : 0.0;
}

double vtx_kl_divergence_callsite(const vtx_callsite_profile_t *current,
                                    const vtx_callsite_profile_t *compiled)
{
    if (current == NULL || compiled == NULL) return 0.0;

    /* If either is megamorphic, we can't compare type distributions
     * meaningfully — return a high divergence to trigger recompilation */
    if (current->megamorphic || compiled->megamorphic) {
        return current->megamorphic != compiled->megamorphic ? 10.0 : 0.0;
    }

    /* Build frequency arrays.
     * D5: Previously, the callsite_profile only stored distinct types
     * without individual frequencies, forcing uniform weighting (each
     * observed type got equal frequency = 1). This made the KL divergence
     * meaningless: a type seen 9999 times and a type seen 1 time got
     * the same weight.
     *
     * Now, the per-type frequency data from vtx_type_freq_t provides
     * actual invocation counts. If the type_freq data is available
     * (total_count > 0), we use the real frequencies. Otherwise, we
     * fall back to uniform weighting as a safe default for profiles
     * that were collected before D5 was implemented. */
    vtx_typeid_t types_a[VTX_POLY_LIMIT + 1];
    uint64_t freqs_a[VTX_POLY_LIMIT + 1];
    vtx_typeid_t types_b[VTX_POLY_LIMIT + 1];
    uint64_t freqs_b[VTX_POLY_LIMIT + 1];

    for (uint32_t i = 0; i < current->count && i <= VTX_POLY_LIMIT; i++) {
        types_a[i] = current->types[i];
        freqs_a[i] = 1; /* uniform weight — fallback */
    }

    for (uint32_t i = 0; i < compiled->count && i <= VTX_POLY_LIMIT; i++) {
        types_b[i] = compiled->types[i];
        freqs_b[i] = 1; /* uniform weight — fallback */
    }

    return vtx_kl_divergence(types_a, freqs_a, current->count,
                               types_b, freqs_b, compiled->count);
}

/* ========================================================================== */
/* D5: Per-type frequency KL divergence                                        */
/* ========================================================================== */

double vtx_recomp_kl_divergence_freq(const vtx_type_freq_t *current,
                                       const vtx_type_freq_t *compiled)
{
    /* Delegate to the implementation in type_feedback.c */
    return vtx_type_freq_kl_divergence(current, compiled);
}

/* ========================================================================== */
/* Recompilation check                                                         */
/* ========================================================================== */

vtx_recomp_check_t vtx_sota_recomp_check(const vtx_sota_recomp_t *recomp,
                                            const vtx_profile_global_t *profile,
                                            uint32_t method_id)
{
    vtx_recomp_check_t result;
    memset(&result, 0, sizeof(result));
    result.method_id = method_id;

    if (recomp == NULL || profile == NULL) return result;

    /* Increment check counter */
    ((vtx_sota_recomp_t *)recomp)->total_checks++;

    /* Find the compilation-time snapshot */
    const vtx_recomp_snapshot_t *snap = NULL;
    for (uint32_t i = 0; i < recomp->snapshot_count; i++) {
        if (recomp->snapshots[i].method_id == method_id && recomp->snapshots[i].valid) {
            snap = &recomp->snapshots[i];
            break;
        }
    }

    if (snap == NULL) {
        /* No snapshot — can't check divergence */
        return result;
    }

    /* Get current profile for this method */
    const vtx_profile_method_t *current_method = vtx_profile_get_method(profile, method_id);
    if (current_method == NULL) {
        return result;
    }

    /* Compare each call site */
    double max_kl = 0.0;
    uint32_t divergent_sites = 0;

    uint32_t sites_to_check = current_method->call_site_count;
    if (sites_to_check > snap->call_site_count) {
        sites_to_check = snap->call_site_count;
    }

    for (uint32_t cs = 0; cs < sites_to_check; cs++) {
        double kl = vtx_kl_divergence_callsite(
            &current_method->call_sites[cs],
            &snap->call_sites[cs]);

        if (kl > max_kl) {
            max_kl = kl;
        }

        if (kl > VTX_PROFILE_DIVERGENCE_THRESHOLD) {
            divergent_sites++;
        }
    }

    /* Also check for new call sites that didn't exist at compilation time */
    if (current_method->call_site_count > snap->call_site_count) {
        divergent_sites += (current_method->call_site_count - snap->call_site_count);
        max_kl = 10.0; /* new sites = maximum divergence */
    }

    result.kl_divergence = max_kl;
    result.divergent_call_sites = divergent_sites;
    result.should_recompile = (divergent_sites > 0);

    if (result.should_recompile) {
        ((vtx_sota_recomp_t *)recomp)->total_recompilations_triggered++;
    }

    return result;
}

/* ========================================================================== */
/* Profile divergence computation                                              */
/* ========================================================================== */

double vtx_sota_recomp_compute_divergence(const vtx_profile_method_t *old_profile,
                                            const vtx_profile_method_t *new_profile)
{
    if (old_profile == NULL || new_profile == NULL) return 0.0;

    double max_kl = 0.0;

    /* Compare call site type distributions */
    uint32_t sites_to_check = old_profile->call_site_count;
    if (new_profile->call_site_count < sites_to_check) {
        sites_to_check = new_profile->call_site_count;
    }

    for (uint32_t cs = 0; cs < sites_to_check; cs++) {
        double kl = vtx_kl_divergence_callsite(
            &new_profile->call_sites[cs],
            &old_profile->call_sites[cs]);

        if (kl > max_kl) {
            max_kl = kl;
        }
    }

    /* New call sites that didn't exist in old profile = maximum divergence */
    if (new_profile->call_site_count > old_profile->call_site_count) {
        max_kl = (max_kl < 10.0) ? 10.0 : max_kl;
    }

    /* Compare branch profile divergence.
     * For branches, we compute the KL divergence of the taken/not-taken
     * probability distributions. This detects shifts in branch behavior. */
    uint32_t branches_to_check = old_profile->branch_count;
    if (new_profile->branch_count < branches_to_check) {
        branches_to_check = new_profile->branch_count;
    }

    for (uint32_t b = 0; b < branches_to_check; b++) {
        /* Find matching branch in new profile by bytecode_pc */
        uint32_t pc = old_profile->branches[b].bytecode_pc;
        for (uint32_t nb = 0; nb < new_profile->branch_count; nb++) {
            if (new_profile->branches[nb].bytecode_pc != pc) continue;

            /* Compute branch probability KL divergence.
             * Old distribution: [p_taken, p_not_taken]
             * New distribution: [q_taken, q_not_taken] */
            double old_taken = (double)old_profile->branches[b].taken;
            double old_total = old_taken + (double)old_profile->branches[b].not_taken;
            double new_taken = (double)new_profile->branches[nb].taken;
            double new_total = new_taken + (double)new_profile->branches[nb].not_taken;

            if (old_total > 0.0 && new_total > 0.0) {
                vtx_typeid_t types_a[2] = {0, 1};
                uint64_t freqs_a[2] = {
                    new_profile->branches[nb].taken,
                    new_profile->branches[nb].not_taken
                };
                vtx_typeid_t types_b[2] = {0, 1};
                uint64_t freqs_b[2] = {
                    old_profile->branches[b].taken,
                    old_profile->branches[b].not_taken
                };

                double branch_kl = vtx_kl_divergence(types_a, freqs_a, 2,
                                                       types_b, freqs_b, 2);
                if (branch_kl > max_kl) {
                    max_kl = branch_kl;
                }
            }
            break;
        }
    }

    return max_kl;
}

/* ========================================================================== */
/* Recompilation queue                                                         */
/* ========================================================================== */

void vtx_sota_recomp_queue(vtx_sota_recomp_t *recomp,
                             uint32_t method_id,
                             const vtx_profile_global_t *new_profile)
{
    if (recomp == NULL) return;

    /* Check for duplicate: if the method is already in the queue
     * and not yet processed, don't add it again */
    for (uint32_t i = 0; i < recomp->recomp_queue_count; i++) {
        if (recomp->recomp_queue[i].method_id == method_id &&
            !recomp->recomp_queue[i].processed) {
            return; /* already queued */
        }
    }

    /* Grow queue if needed */
    if (recomp->recomp_queue_count >= recomp->recomp_queue_capacity) {
        uint32_t new_cap = recomp->recomp_queue_capacity * 2;
        vtx_recomp_queue_entry_t *new_queue = (vtx_recomp_queue_entry_t *)realloc(
            recomp->recomp_queue, new_cap * sizeof(vtx_recomp_queue_entry_t));
        if (new_queue == NULL) return; /* allocation failure — drop the entry */
        memset(new_queue + recomp->recomp_queue_capacity, 0,
               (new_cap - recomp->recomp_queue_capacity) * sizeof(vtx_recomp_queue_entry_t));
        recomp->recomp_queue = new_queue;
        recomp->recomp_queue_capacity = new_cap;
    }

    /* Compute current divergence for the queue entry */
    double kl = 0.0;

    /* Find the snapshot for this method to compute divergence */
    for (uint32_t i = 0; i < recomp->snapshot_count; i++) {
        if (recomp->snapshots[i].method_id == method_id && recomp->snapshots[i].valid) {
            if (new_profile != NULL) {
                const vtx_profile_method_t *current_method =
                    vtx_profile_get_method(new_profile, method_id);
                if (current_method != NULL) {
                    /* Build a temporary old profile from the snapshot */
                    vtx_profile_method_t old_method;
                    memset(&old_method, 0, sizeof(old_method));
                    old_method.method_id = method_id;
                    old_method.call_sites = recomp->snapshots[i].call_sites;
                    old_method.call_site_count = recomp->snapshots[i].call_site_count;

                    kl = vtx_sota_recomp_compute_divergence(&old_method, current_method);
                }
            }
            break;
        }
    }

    /* Add entry to the queue */
    vtx_recomp_queue_entry_t *entry = &recomp->recomp_queue[recomp->recomp_queue_count];
    entry->method_id = method_id;
    entry->kl_divergence = kl;
    entry->enqueue_time_ns = 0; /* caller should set this if needed */
    entry->processed = false;

    recomp->recomp_queue_count++;
    recomp->total_recompilations_triggered++;

    /* Also save a new snapshot with the current profile, so that
     * when the recompilation happens, it uses the latest profile data */
    if (new_profile != NULL) {
        vtx_sota_recomp_save_snapshot(recomp, method_id, new_profile);
    }
}

uint32_t vtx_sota_recomp_dequeue(vtx_sota_recomp_t *recomp)
{
    if (recomp == NULL) return VTX_PHASE_NONE;

    /* Find the first unprocessed entry */
    for (uint32_t i = 0; i < recomp->recomp_queue_count; i++) {
        if (!recomp->recomp_queue[i].processed) {
            recomp->recomp_queue[i].processed = true;
            return recomp->recomp_queue[i].method_id;
        }
    }

    /* No pending entries — compact the queue by removing processed entries */
    uint32_t write = 0;
    for (uint32_t read = 0; read < recomp->recomp_queue_count; read++) {
        if (!recomp->recomp_queue[read].processed) {
            recomp->recomp_queue[write++] = recomp->recomp_queue[read];
        }
    }
    recomp->recomp_queue_count = write;

    return VTX_PHASE_NONE;
}

bool vtx_sota_recomp_has_pending(const vtx_sota_recomp_t *recomp)
{
    if (recomp == NULL) return false;

    for (uint32_t i = 0; i < recomp->recomp_queue_count; i++) {
        if (!recomp->recomp_queue[i].processed) {
            return true;
        }
    }
    return false;
}

/* ========================================================================== */
/* Sprint 1.2: Hysteresis hash table                                           */
/* ========================================================================== */

/* Open-addressing hash table with linear probing.
 * Hash: method_id mod capacity (capacity is a power of 2).
 * Tombstone-free: deleted entries are reset to valid=false and reused. */

static uint32_t hysteresis_hash(uint32_t method_id, uint32_t capacity)
{
    /* Fibonacci hashing — good distribution for small power-of-2 tables. */
    return (uint32_t)((method_id * 2654435761u) & (capacity - 1));
}

static vtx_recomp_hysteresis_t *hysteresis_find_slot(
    vtx_sota_recomp_t *recomp, uint32_t method_id, bool create)
{
    if (recomp->hysteresis == NULL || recomp->hysteresis_capacity == 0) {
        return NULL;
    }

    uint32_t mask = recomp->hysteresis_capacity - 1;
    uint32_t start = hysteresis_hash(method_id, recomp->hysteresis_capacity);
    uint32_t first_invalid = UINT32_MAX;

    for (uint32_t probe = 0; probe <= mask; probe++) {
        uint32_t idx = (start + probe) & mask;
        vtx_recomp_hysteresis_t *slot = &recomp->hysteresis[idx];

        if (!slot->valid) {
            if (first_invalid == UINT32_MAX) first_invalid = idx;
            /* Keep probing in case the method_id is later in the chain. */
            continue;
        }

        if (slot->method_id == method_id) {
            return slot;
        }
    }

    if (!create) return NULL;

    /* Grow if load factor > 0.75 */
    if ((recomp->hysteresis_count * 4) > (recomp->hysteresis_capacity * 3)) {
        uint32_t new_cap = recomp->hysteresis_capacity * 2;
        vtx_recomp_hysteresis_t *new_table = (vtx_recomp_hysteresis_t *)calloc(
            new_cap, sizeof(vtx_recomp_hysteresis_t));
        if (new_table == NULL) {
            /* Allocation failed — fall back to reusing an invalid slot
             * if we found one. */
            if (first_invalid != UINT32_MAX) {
                vtx_recomp_hysteresis_t *slot = &recomp->hysteresis[first_invalid];
                memset(slot, 0, sizeof(*slot));
                slot->method_id = method_id;
                slot->valid = true;
                recomp->hysteresis_count++;
                return slot;
            }
            return NULL;
        }

        /* Rehash */
        for (uint32_t i = 0; i < recomp->hysteresis_capacity; i++) {
            vtx_recomp_hysteresis_t *old = &recomp->hysteresis[i];
            if (!old->valid) continue;
            uint32_t new_idx = hysteresis_hash(old->method_id, new_cap);
            while (new_table[new_idx].valid) {
                new_idx = (new_idx + 1) & (new_cap - 1);
            }
            new_table[new_idx] = *old;
        }

        free(recomp->hysteresis);
        recomp->hysteresis = new_table;
        recomp->hysteresis_capacity = new_cap;

        /* Find a free slot in the new table */
        uint32_t idx = hysteresis_hash(method_id, new_cap);
        while (recomp->hysteresis[idx].valid) {
            idx = (idx + 1) & (new_cap - 1);
        }
        vtx_recomp_hysteresis_t *slot = &recomp->hysteresis[idx];
        memset(slot, 0, sizeof(*slot));
        slot->method_id = method_id;
        slot->valid = true;
        recomp->hysteresis_count++;
        return slot;
    }

    if (first_invalid != UINT32_MAX) {
        vtx_recomp_hysteresis_t *slot = &recomp->hysteresis[first_invalid];
        memset(slot, 0, sizeof(*slot));
        slot->method_id = method_id;
        slot->valid = true;
        recomp->hysteresis_count++;
        return slot;
    }

    return NULL;  /* table full (shouldn't happen with growth) */
}

vtx_recomp_check_t vtx_sota_recomp_check_hysteresis(vtx_sota_recomp_t *recomp,
                                                       const vtx_profile_global_t *profile,
                                                       uint32_t method_id)
{
    vtx_recomp_check_t result;
    memset(&result, 0, sizeof(result));
    result.method_id = method_id;

    if (recomp == NULL || profile == NULL) return result;

    /* First, run the raw check to get the divergence value. */
    vtx_recomp_check_t raw = vtx_sota_recomp_check(recomp, profile, method_id);
    result.kl_divergence = raw.kl_divergence;
    result.divergent_call_sites = raw.divergent_call_sites;
    result.method_id = raw.method_id;

    /* Update the hysteresis counter for this method. */
    vtx_recomp_hysteresis_t *slot = hysteresis_find_slot(recomp, method_id, true);
    if (slot == NULL) {
        /* Allocation failure — fall back to raw result so we don't
         * silently drop divergences. */
        result.should_recompile = raw.should_recompile;
        return result;
    }

    if (raw.divergent_call_sites > 0) {
        slot->consecutive_divergent++;
    } else {
        slot->consecutive_divergent = 0;
    }

    /* Only fire if the hysteresis threshold is met. */
    if (slot->consecutive_divergent >= VTX_RECOMP_HYSTERESIS_CONSECUTIVE) {
        result.should_recompile = true;
    } else {
        result.should_recompile = false;
        if (raw.should_recompile) {
            recomp->total_hysteresis_blocks++;
        }
    }

    return result;
}

void vtx_sota_recomp_hysteresis_reset(vtx_sota_recomp_t *recomp,
                                        uint32_t method_id)
{
    if (recomp == NULL) return;
    vtx_recomp_hysteresis_t *slot = hysteresis_find_slot(recomp, method_id, false);
    if (slot != NULL) {
        slot->consecutive_divergent = 0;
    }
}

uint32_t vtx_sota_recomp_hysteresis_count(const vtx_sota_recomp_t *recomp,
                                            uint32_t method_id)
{
    if (recomp == NULL) return 0;
    /* Const-cast: find_slot may create, but we pass create=false so it
     * never mutates. */
    vtx_sota_recomp_t *mut = (vtx_sota_recomp_t *)recomp;
    vtx_recomp_hysteresis_t *slot = hysteresis_find_slot(mut, method_id, false);
    return slot ? slot->consecutive_divergent : 0;
}

/* ========================================================================== */
/* Sprint 1.3: Backpressure-aware queue                                        */
/* ========================================================================== */

/* Helper: monotonic time in nanoseconds. */
static uint64_t recomp_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Helper: find the lowest-priority (lowest KL divergence) unprocessed entry
 * in the queue. Returns the index, or UINT32_MAX if none. */
static uint32_t find_lowest_priority_entry(const vtx_sota_recomp_t *recomp)
{
    uint32_t lowest_idx = UINT32_MAX;
    double   lowest_kl  = 0.0;
    for (uint32_t i = 0; i < recomp->recomp_queue_count; i++) {
        if (recomp->recomp_queue[i].processed) continue;
        if (lowest_idx == UINT32_MAX ||
            recomp->recomp_queue[i].kl_divergence < lowest_kl) {
            lowest_idx = i;
            lowest_kl  = recomp->recomp_queue[i].kl_divergence;
        }
    }
    return lowest_idx;
}

/* Helper: find an existing unprocessed entry for the given method_id. */
static int32_t find_existing_entry(const vtx_sota_recomp_t *recomp,
                                     uint32_t method_id)
{
    for (uint32_t i = 0; i < recomp->recomp_queue_count; i++) {
        if (!recomp->recomp_queue[i].processed &&
            recomp->recomp_queue[i].method_id == method_id) {
            return (int32_t)i;
        }
    }
    return -1;
}

/* Helper: remove the entry at the given index by shifting subsequent
 * entries down. Preserves queue order. */
static void queue_remove_at(vtx_sota_recomp_t *recomp, uint32_t idx)
{
    if (idx >= recomp->recomp_queue_count) return;
    for (uint32_t i = idx; i + 1 < recomp->recomp_queue_count; i++) {
        recomp->recomp_queue[i] = recomp->recomp_queue[i + 1];
    }
    recomp->recomp_queue_count--;
}

bool vtx_sota_recomp_queue_backpressure(vtx_sota_recomp_t *recomp,
                                          uint32_t method_id,
                                          const vtx_profile_global_t *new_profile,
                                          uint64_t now_ns_)
{
    if (recomp == NULL) return false;

    uint64_t now = (now_ns_ == 0) ? recomp_now_ns() : now_ns_;
    const uint64_t window_ns =
        (uint64_t)VTX_RECOMP_COALESCE_WINDOW_SEC * 1000000000ull;

    /* ---- COALESCE: check if this method is being recompiled too often ---- */
    vtx_recomp_hysteresis_t *hyst = hysteresis_find_slot(recomp, method_id, true);
    if (hyst != NULL) {
        /* Reset the coalesce window if it has elapsed. */
        if (hyst->window_start_ns == 0 ||
            (now - hyst->window_start_ns) > window_ns) {
            hyst->window_start_ns = now;
            hyst->recomp_count_in_window = 0;
        }
        hyst->recomp_count_in_window++;
        hyst->last_recomp_time_ns = now;

        /* If this method has been recompiled too many times in the window,
         * coalesce: replace any existing unprocessed entry with this one
         * (updating the KL divergence) rather than adding a duplicate. */
        if (hyst->recomp_count_in_window > VTX_RECOMP_COALESCE_MAX_DUPLICATES) {
            int32_t existing = find_existing_entry(recomp, method_id);
            if (existing >= 0) {
                /* Update the existing entry's KL divergence and timestamp. */
                double kl = 0.0;
                if (new_profile != NULL) {
                    const vtx_profile_method_t *cur =
                        vtx_profile_get_method(new_profile, method_id);
                    if (cur != NULL) {
                        vtx_profile_method_t old_method;
                        memset(&old_method, 0, sizeof(old_method));
                        old_method.method_id = method_id;
                        for (uint32_t i = 0; i < recomp->snapshot_count; i++) {
                            if (recomp->snapshots[i].method_id == method_id &&
                                recomp->snapshots[i].valid) {
                                old_method.call_sites = recomp->snapshots[i].call_sites;
                                old_method.call_site_count = recomp->snapshots[i].call_site_count;
                                break;
                            }
                        }
                        kl = vtx_sota_recomp_compute_divergence(&old_method, cur);
                    }
                }
                recomp->recomp_queue[existing].kl_divergence = kl;
                recomp->recomp_queue[existing].enqueue_time_ns = now;
                recomp->recomp_queue[existing].processed = false;
                recomp->total_coalesced++;
                return true;
            }
            /* No existing entry — fall through to normal enqueue. */
        }
    }

    /* ---- Check for duplicate (unprocessed entry for the same method) ----
     * P12 fix: The old code did the soft-cap eviction BEFORE the duplicate
     * check. If the new entry was a duplicate, we evicted a valid entry
     * for nothing (data loss). Fix: check for duplicates FIRST, before
     * any eviction. */
    if (find_existing_entry(recomp, method_id) >= 0) {
        return true;  /* already queued — not an error */
    }

    /* ---- HARD CAP: reject if the queue is full ---- */
    if (recomp->recomp_queue_count >= VTX_RECOMP_QUEUE_HARD_CAP) {
        recomp->total_dropped_hard_cap++;
        return false;
    }

    /* ---- SOFT CAP: evict the lowest-priority entry if over the cap ----
     * P13 fix: The old code evicted the lowest-KL entry unconditionally,
     * even if the new entry had lower priority (lower KL). That's priority
     * inversion — we'd evict a more important entry for a less important one.
     * Fix: compute the new entry's KL first, then only evict if the new
     * entry has HIGHER priority (higher KL divergence = more urgent). */
    if (recomp->recomp_queue_count >= VTX_RECOMP_QUEUE_SOFT_CAP) {
        /* Compute the new entry's KL divergence for priority comparison. */
        double new_kl = 0.0;
        if (new_profile != NULL) {
            const vtx_profile_method_t *cur =
                vtx_profile_get_method(new_profile, method_id);
            if (cur != NULL) {
                vtx_profile_method_t old_method;
                memset(&old_method, 0, sizeof(old_method));
                old_method.method_id = method_id;
                for (uint32_t i = 0; i < recomp->snapshot_count; i++) {
                    if (recomp->snapshots[i].method_id == method_id &&
                        recomp->snapshots[i].valid) {
                        old_method.call_sites = recomp->snapshots[i].call_sites;
                        old_method.call_site_count = recomp->snapshots[i].call_site_count;
                        break;
                    }
                }
                new_kl = vtx_sota_recomp_compute_divergence(&old_method, cur);
            }
        }

        uint32_t lowest = find_lowest_priority_entry(recomp);
        if (lowest != UINT32_MAX) {
            /* P13 fix: Only evict if the new entry has higher priority
             * (higher KL divergence = more urgent). If the new entry has
             * lower or equal priority, drop it instead — don't evict a
             * more important entry for a less important one. */
            if (new_kl > recomp->recomp_queue[lowest].kl_divergence) {
                queue_remove_at(recomp, lowest);
                recomp->total_dropped_soft_cap++;
            } else {
                /* New entry has lower priority — drop it, don't evict. */
                recomp->total_dropped_soft_cap++;
                return false;
            }
        }
    }

    /* ---- Grow the queue if at capacity (still under HARD_CAP) ---- */
    if (recomp->recomp_queue_count >= recomp->recomp_queue_capacity) {
        uint32_t new_cap = recomp->recomp_queue_capacity * 2;
        if (new_cap > VTX_RECOMP_QUEUE_HARD_CAP) {
            new_cap = VTX_RECOMP_QUEUE_HARD_CAP;
        }
        if (new_cap <= recomp->recomp_queue_capacity) {
            recomp->total_dropped_hard_cap++;
            return false;
        }
        vtx_recomp_queue_entry_t *new_queue = (vtx_recomp_queue_entry_t *)realloc(
            recomp->recomp_queue, new_cap * sizeof(vtx_recomp_queue_entry_t));
        if (new_queue == NULL) {
            recomp->total_dropped_hard_cap++;
            return false;
        }
        memset(new_queue + recomp->recomp_queue_capacity, 0,
               (new_cap - recomp->recomp_queue_capacity) * sizeof(vtx_recomp_queue_entry_t));
        recomp->recomp_queue = new_queue;
        recomp->recomp_queue_capacity = new_cap;
    }

    /* ---- Compute KL divergence for the new entry ---- */
    double kl = 0.0;
    if (new_profile != NULL) {
        const vtx_profile_method_t *cur =
            vtx_profile_get_method(new_profile, method_id);
        if (cur != NULL) {
            vtx_profile_method_t old_method;
            memset(&old_method, 0, sizeof(old_method));
            old_method.method_id = method_id;
            for (uint32_t i = 0; i < recomp->snapshot_count; i++) {
                if (recomp->snapshots[i].method_id == method_id &&
                    recomp->snapshots[i].valid) {
                    old_method.call_sites = recomp->snapshots[i].call_sites;
                    old_method.call_site_count = recomp->snapshots[i].call_site_count;
                    break;
                }
            }
            kl = vtx_sota_recomp_compute_divergence(&old_method, cur);
        }
    }

    /* ---- Enqueue ---- */
    vtx_recomp_queue_entry_t *entry = &recomp->recomp_queue[recomp->recomp_queue_count];
    entry->method_id = method_id;
    entry->kl_divergence = kl;
    entry->enqueue_time_ns = now;
    entry->processed = false;
    recomp->recomp_queue_count++;
    recomp->total_recompilations_triggered++;

    /* Save a new snapshot with the current profile. */
    if (new_profile != NULL) {
        vtx_sota_recomp_save_snapshot(recomp, method_id, new_profile);
    }

    return true;
}

void vtx_sota_recomp_backpressure_stats(const vtx_sota_recomp_t *recomp,
                                          uint64_t *dropped_soft_cap,
                                          uint64_t *dropped_hard_cap,
                                          uint64_t *coalesced,
                                          uint64_t *hysteresis_blocks)
{
    if (recomp == NULL) {
        if (dropped_soft_cap) *dropped_soft_cap = 0;
        if (dropped_hard_cap) *dropped_hard_cap = 0;
        if (coalesced)        *coalesced = 0;
        if (hysteresis_blocks) *hysteresis_blocks = 0;
        return;
    }
    if (dropped_soft_cap)  *dropped_soft_cap  = recomp->total_dropped_soft_cap;
    if (dropped_hard_cap)  *dropped_hard_cap  = recomp->total_dropped_hard_cap;
    if (coalesced)         *coalesced         = recomp->total_coalesced;
    if (hysteresis_blocks) *hysteresis_blocks = recomp->total_hysteresis_blocks;
}
