#include "sota/recomp.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ========================================================================== */
/* Profile snapshot                                                            */
/* ========================================================================== */

/**
 * Snapshot of a method's profile at compilation time.
 * Stores the type distribution at each call site.
 */
typedef struct {
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

    recomp->total_checks = 0;
    recomp->total_recompilations_triggered = 0;
    recomp->total_false_positives = 0;

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

    recomp->snapshot_count = 0;
    recomp->snapshot_capacity = 0;
    recomp->recomp_queue_count = 0;
    recomp->recomp_queue_capacity = 0;
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
     * The callsite_profile stores types but not individual frequencies.
     * We use uniform weighting (each observed type gets equal frequency)
     * since the profile only records distinct types, not their counts.
     * A more precise implementation would track per-type invocation counts. */
    vtx_typeid_t types_a[VTX_POLY_LIMIT + 1];
    uint64_t freqs_a[VTX_POLY_LIMIT + 1];
    vtx_typeid_t types_b[VTX_POLY_LIMIT + 1];
    uint64_t freqs_b[VTX_POLY_LIMIT + 1];

    for (uint32_t i = 0; i < current->count && i <= VTX_POLY_LIMIT; i++) {
        types_a[i] = current->types[i];
        freqs_a[i] = 1; /* uniform weight per observed type */
    }

    for (uint32_t i = 0; i < compiled->count && i <= VTX_POLY_LIMIT; i++) {
        types_b[i] = compiled->types[i];
        freqs_b[i] = 1;
    }

    return vtx_kl_divergence(types_a, freqs_a, current->count,
                               types_b, freqs_b, compiled->count);
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
