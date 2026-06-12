#include "interp/type_feedback.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ========================================================================== */
/* Type feedback init / destroy                                                */
/* ========================================================================== */

int vtx_type_feedback_init(vtx_type_feedback_t *feedback, uint32_t max_sites)
{
    VTX_ASSERT(feedback != NULL, "feedback must not be NULL");

    if (max_sites == 0) {
        max_sites = 1; /* ensure at least 1 site */
    }

    /* Allocate call site feedback array */
    feedback->call_sites = (vtx_tf_call_site_t *)calloc(
        max_sites, sizeof(vtx_tf_call_site_t));
    if (feedback->call_sites == NULL) {
        feedback->call_site_count = 0;
        feedback->field_sites = NULL;
        feedback->field_site_count = 0;
        feedback->branch_sites = NULL;
        feedback->branch_site_count = 0;
        return -1;
    }
    feedback->call_site_count = max_sites;

    /* Allocate field site feedback array */
    feedback->field_sites = (vtx_tf_field_site_t *)calloc(
        max_sites, sizeof(vtx_tf_field_site_t));
    if (feedback->field_sites == NULL) {
        free(feedback->call_sites);
        feedback->call_sites = NULL;
        feedback->call_site_count = 0;
        feedback->field_site_count = 0;
        feedback->branch_sites = NULL;
        feedback->branch_site_count = 0;
        return -1;
    }
    feedback->field_site_count = max_sites;

    /* Allocate branch site feedback array */
    feedback->branch_sites = (vtx_tf_branch_site_t *)calloc(
        max_sites, sizeof(vtx_tf_branch_site_t));
    if (feedback->branch_sites == NULL) {
        free(feedback->call_sites);
        free(feedback->field_sites);
        feedback->call_sites = NULL;
        feedback->call_site_count = 0;
        feedback->field_sites = NULL;
        feedback->field_site_count = 0;
        feedback->branch_site_count = 0;
        return -1;
    }
    feedback->branch_site_count = max_sites;

    return 0;
}

void vtx_type_feedback_destroy(vtx_type_feedback_t *feedback)
{
    VTX_ASSERT(feedback != NULL, "feedback must not be NULL");

    free(feedback->call_sites);
    feedback->call_sites = NULL;
    feedback->call_site_count = 0;

    free(feedback->field_sites);
    feedback->field_sites = NULL;
    feedback->field_site_count = 0;

    free(feedback->branch_sites);
    feedback->branch_sites = NULL;
    feedback->branch_site_count = 0;
}

/* ========================================================================== */
/* Dynamic site array growth                                                   */
/* ========================================================================== */

/**
 * Grow an array of call site feedback entries to accommodate site_index.
 * Returns 0 on success, -1 on failure.
 */
static int grow_call_sites(vtx_type_feedback_t *feedback, uint32_t site_index)
{
    uint32_t needed = site_index + 1;
    if (needed <= feedback->call_site_count) {
        return 0;
    }

    uint32_t new_count = feedback->call_site_count;
    while (new_count < needed) {
        new_count *= 2;
    }

    vtx_tf_call_site_t *new_arr = (vtx_tf_call_site_t *)realloc(
        feedback->call_sites, new_count * sizeof(vtx_tf_call_site_t));
    if (new_arr == NULL) {
        return -1;
    }
    memset(new_arr + feedback->call_site_count, 0,
           (new_count - feedback->call_site_count) * sizeof(vtx_tf_call_site_t));
    feedback->call_sites = new_arr;
    feedback->call_site_count = new_count;
    return 0;
}

static int grow_field_sites(vtx_type_feedback_t *feedback, uint32_t site_index)
{
    uint32_t needed = site_index + 1;
    if (needed <= feedback->field_site_count) {
        return 0;
    }

    uint32_t new_count = feedback->field_site_count;
    while (new_count < needed) {
        new_count *= 2;
    }

    vtx_tf_field_site_t *new_arr = (vtx_tf_field_site_t *)realloc(
        feedback->field_sites, new_count * sizeof(vtx_tf_field_site_t));
    if (new_arr == NULL) {
        return -1;
    }
    memset(new_arr + feedback->field_site_count, 0,
           (new_count - feedback->field_site_count) * sizeof(vtx_tf_field_site_t));
    feedback->field_sites = new_arr;
    feedback->field_site_count = new_count;
    return 0;
}

static int grow_branch_sites(vtx_type_feedback_t *feedback, uint32_t site_index)
{
    uint32_t needed = site_index + 1;
    if (needed <= feedback->branch_site_count) {
        return 0;
    }

    uint32_t new_count = feedback->branch_site_count;
    while (new_count < needed) {
        new_count *= 2;
    }

    vtx_tf_branch_site_t *new_arr = (vtx_tf_branch_site_t *)realloc(
        feedback->branch_sites, new_count * sizeof(vtx_tf_branch_site_t));
    if (new_arr == NULL) {
        return -1;
    }
    memset(new_arr + feedback->branch_site_count, 0,
           (new_count - feedback->branch_site_count) * sizeof(vtx_tf_branch_site_t));
    feedback->branch_sites = new_arr;
    feedback->branch_site_count = new_count;
    return 0;
}

int vtx_type_feedback_ensure_site(vtx_type_feedback_t *feedback, uint32_t site_index)
{
    VTX_ASSERT(feedback != NULL, "feedback must not be NULL");

    if (grow_call_sites(feedback, site_index) != 0) return -1;
    if (grow_field_sites(feedback, site_index) != 0) return -1;
    if (grow_branch_sites(feedback, site_index) != 0) return -1;
    return 0;
}

/* ========================================================================== */
/* Recording functions                                                         */
/* ========================================================================== */

void vtx_type_feedback_record_call(vtx_type_feedback_t *feedback,
                                    uint32_t site_index,
                                    vtx_typeid_t receiver_typeid,
                                    vtx_typeid_t result_typeid)
{
    VTX_ASSERT(feedback != NULL, "feedback must not be NULL");

    /* Bug #12 fix: Filter out VTX_TYPE_INVALID as result_typeid.
     * Recording VTX_TYPE_INVALID provides no useful information to the
     * optimizer — it only pollutes the circular buffer with useless
     * observations. This commonly happens at virtual call sites where
     * the result type is not yet known. */
    if (result_typeid == VTX_TYPE_INVALID) return;

    /* Ensure the site exists (grow if needed) */
    if (site_index >= feedback->call_site_count) {
        if (grow_call_sites(feedback, site_index) != 0) {
            return; /* out of memory, silently drop */
        }
    }

    vtx_tf_call_site_t *site = &feedback->call_sites[site_index];

    /* Write the observation into the circular buffer */
    site->observations[site->write_index].receiver_typeid = receiver_typeid;
    site->observations[site->write_index].result_typeid = result_typeid;

    /* Advance write index (wraps around) */
    site->write_index = (site->write_index + 1) % VTX_TYPE_FEEDBACK_BUFFER_SIZE;

    /* Increment count up to the buffer size */
    if (site->count < VTX_TYPE_FEEDBACK_BUFFER_SIZE) {
        site->count++;
    }

    /* D5: Update per-type frequency tracking.
     * This enables accurate KL divergence computation for recompilation.
     * Without it, the KL divergence uses uniform weights (each observed type
     * gets equal weight), which makes the divergence metric meaningless. */
    vtx_type_freq_t *freq = &site->type_freq;
    freq->total_count++;

    /* Find existing entry for this type, or create a new one */
    for (uint32_t i = 0; i < freq->entry_count; i++) {
        if (freq->entries[i].type_id == receiver_typeid) {
            freq->entries[i].count++;
            return;
        }
    }

    /* New type — add entry if there's room */
    if (freq->entry_count < VTX_TYPE_FREQ_MAX_SLOTS) {
        freq->entries[freq->entry_count].type_id = receiver_typeid;
        freq->entries[freq->entry_count].count = 1;
        freq->entry_count++;
    }
    /* If we've exceeded VTX_TYPE_FREQ_MAX_SLOTS, total_count still
     * increments (tracking overall observation volume) but this new
     * type doesn't get its own slot. The KL divergence computation
     * handles this via the "unseen type penalty" path. */

    /* Update stable-type signature tracking (Proposal #2) */
    vtx_tf_call_site_update_signature(site, receiver_typeid, result_typeid);
}

void vtx_type_feedback_record_field(vtx_type_feedback_t *feedback,
                                     uint32_t site_index,
                                     vtx_shapeid_t holder_shapeid,
                                     vtx_typeid_t value_typeid)
{
    VTX_ASSERT(feedback != NULL, "feedback must not be NULL");

    if (site_index >= feedback->field_site_count) {
        if (grow_field_sites(feedback, site_index) != 0) {
            return;
        }
    }

    vtx_tf_field_site_t *site = &feedback->field_sites[site_index];

    site->observations[site->write_index].holder_shapeid = holder_shapeid;
    site->observations[site->write_index].value_typeid = value_typeid;

    site->write_index = (site->write_index + 1) % VTX_TYPE_FEEDBACK_BUFFER_SIZE;

    if (site->count < VTX_TYPE_FEEDBACK_BUFFER_SIZE) {
        site->count++;
    }

    /* Update shape stability tracking (Proposal #9) */
    {
        vtx_tf_field_site_t *fsite = &feedback->field_sites[site_index];
        if (fsite->shape_stability_count == 0 || fsite->last_shapeid != holder_shapeid) {
            fsite->last_shapeid = holder_shapeid;
            fsite->shape_stability_count = 1;
            fsite->is_shape_stable = false;
        } else {
            if (fsite->shape_stability_count < UINT32_MAX) {
                fsite->shape_stability_count++;
            }
            if (fsite->shape_stability_count >= VTX_SHAPE_STABILITY_WINDOW) {
                fsite->is_shape_stable = true;
            }
        }
    }
}

void vtx_type_feedback_record_branch(vtx_type_feedback_t *feedback,
                                      uint32_t site_index,
                                      bool taken)
{
    VTX_ASSERT(feedback != NULL, "feedback must not be NULL");

    if (site_index >= feedback->branch_site_count) {
        if (grow_branch_sites(feedback, site_index) != 0) {
            return;
        }
    }

    vtx_tf_branch_site_t *site = &feedback->branch_sites[site_index];

    site->observations[site->write_index].taken = taken;

    site->write_index = (site->write_index + 1) % VTX_TYPE_FEEDBACK_BUFFER_SIZE;

    if (site->count < VTX_TYPE_FEEDBACK_BUFFER_SIZE) {
        site->count++;
    }
}

/* ========================================================================== */
/* Precomputed decay weights                                                   */
/* ========================================================================== */

/**
 * Precomputed exponential decay weights: 0.9^i for i = 0..31.
 * Avoids calling pow() 32 times per dominant-type query.
 *
 * decay_weights[0] = 0.9^0 = 1.0  (distance=0 means most recent for a full buffer)
 * decay_weights[1] = 0.9^1 = 0.9
 * decay_weights[2] = 0.9^2 = 0.81
 * ...
 * decay_weights[31] = 0.9^31 ≈ 0.0378
 */
static const double decay_weights[32] = {
    1.0,
    0.9,
    0.81,
    0.729,
    0.6561,
    0.59049,
    0.531441,
    0.4782969,
    0.43046721,
    0.387420489,
    0.3486784401,
    0.31381059609,
    0.282429536481,
    0.2541865828329,
    0.22876792454961,
    0.205891132094649,
    0.1853020188851841,
    0.1667718169966657,
    0.1500946352969991,
    0.1350851717672992,
    0.1215766545905693,
    0.1094189891315124,
    0.0984770902183611,
    0.0886293811965250,
    0.0797664430768725,
    0.0717897987691853,
    0.0646108188922667,
    0.0581497370030401,
    0.0523347633027361,
    0.0471012869724624,
    0.0423911582752162,
    0.0381520424476946
};

/* ========================================================================== */
/* Query helpers                                                               */
/* ========================================================================== */

/**
 * Compute the exponential decay weight for an observation at position
 * obs_index relative to the current write_index.
 *
 * weight = 0.9^distance
 *
 * distance = (write_index - obs_index + SIZE) % SIZE
 * When distance == 0, it means the observation was written at write_index
 * positions ago, which for a full buffer is the oldest observation.
 * We treat distance=0 as distance=SIZE to give the oldest entry the
 * lowest weight.
 *
 * The most recent observation (at write_index - 1) gets distance=1,
 * weight = 0.9. The second most recent gets distance=2, weight = 0.81.
 */
static double compute_decay_weight(uint8_t write_index, uint8_t obs_index)
{
    int distance = ((int)write_index - (int)obs_index + VTX_TYPE_FEEDBACK_BUFFER_SIZE)
                   % VTX_TYPE_FEEDBACK_BUFFER_SIZE;
    if (distance == 0) {
        /* Oldest observation in a full buffer */
        distance = VTX_TYPE_FEEDBACK_BUFFER_SIZE;
    }
    /* Use precomputed table instead of pow(0.9, distance) */
    if (distance >= 0 && distance < 32) {
        return decay_weights[distance];
    }
    /* Fallback for distances beyond table (shouldn't happen with SIZE=32) */
    return 0.0;
}

vtx_typeid_t vtx_type_feedback_get_dominant_call_type(
    const vtx_type_feedback_t *feedback, uint32_t site_index)
{
    VTX_ASSERT(feedback != NULL, "feedback must not be NULL");

    if (site_index >= feedback->call_site_count) {
        return VTX_TYPE_INVALID;
    }

    const vtx_tf_call_site_t *site = &feedback->call_sites[site_index];

    if (site->count == 0) {
        return VTX_TYPE_INVALID;
    }

    /* Aggregate weights by typeid.
     * We use a simple linear scan since we have at most 32 observations
     * and typically very few unique types. */
    struct {
        vtx_typeid_t typeid_;
        double       weight;
    } type_weights[VTX_TYPE_FEEDBACK_BUFFER_SIZE];
    int unique_count = 0;

    for (uint8_t i = 0; i < site->count; i++) {
        vtx_typeid_t tid = site->observations[i].receiver_typeid;
        double w = compute_decay_weight(site->write_index, i);

        /* Find existing entry or add new */
        bool found = false;
        for (int j = 0; j < unique_count; j++) {
            if (type_weights[j].typeid_ == tid) {
                type_weights[j].weight += w;
                found = true;
                break;
            }
        }
        if (!found && unique_count < VTX_TYPE_FEEDBACK_BUFFER_SIZE) {
            type_weights[unique_count].typeid_ = tid;
            type_weights[unique_count].weight = w;
            unique_count++;
        }
    }

    /* Find the typeid with the highest weight */
    vtx_typeid_t dominant = VTX_TYPE_INVALID;
    double max_weight = -1.0;
    for (int j = 0; j < unique_count; j++) {
        if (type_weights[j].weight > max_weight) {
            max_weight = type_weights[j].weight;
            dominant = type_weights[j].typeid_;
        }
    }

    return dominant;
}

vtx_shapeid_t vtx_type_feedback_get_dominant_field_shape(
    const vtx_type_feedback_t *feedback, uint32_t site_index)
{
    VTX_ASSERT(feedback != NULL, "feedback must not be NULL");

    if (site_index >= feedback->field_site_count) {
        return VTX_SHAPE_INVALID;
    }

    const vtx_tf_field_site_t *site = &feedback->field_sites[site_index];

    if (site->count == 0) {
        return VTX_SHAPE_INVALID;
    }

    /* Aggregate weights by shapeid */
    struct {
        vtx_shapeid_t shapeid;
        double        weight;
    } shape_weights[VTX_TYPE_FEEDBACK_BUFFER_SIZE];
    int unique_count = 0;

    for (uint8_t i = 0; i < site->count; i++) {
        vtx_shapeid_t sid = site->observations[i].holder_shapeid;
        double w = compute_decay_weight(site->write_index, i);

        bool found = false;
        for (int j = 0; j < unique_count; j++) {
            if (shape_weights[j].shapeid == sid) {
                shape_weights[j].weight += w;
                found = true;
                break;
            }
        }
        if (!found && unique_count < VTX_TYPE_FEEDBACK_BUFFER_SIZE) {
            shape_weights[unique_count].shapeid = sid;
            shape_weights[unique_count].weight = w;
            unique_count++;
        }
    }

    /* Find the shapeid with the highest weight */
    vtx_shapeid_t dominant = VTX_SHAPE_INVALID;
    double max_weight = -1.0;
    for (int j = 0; j < unique_count; j++) {
        if (shape_weights[j].weight > max_weight) {
            max_weight = shape_weights[j].weight;
            dominant = shape_weights[j].shapeid;
        }
    }

    return dominant;
}

vtx_typeid_t vtx_type_feedback_get_dominant_field_value_type(
    const vtx_type_feedback_t *feedback, uint32_t site_index)
{
    VTX_ASSERT(feedback != NULL, "feedback must not be NULL");

    if (site_index >= feedback->field_site_count) {
        return VTX_TYPE_INVALID;
    }

    const vtx_tf_field_site_t *site = &feedback->field_sites[site_index];

    if (site->count == 0) {
        return VTX_TYPE_INVALID;
    }

    /* Aggregate weights by value typeid */
    struct {
        vtx_typeid_t typeid_;
        double       weight;
    } type_weights[VTX_TYPE_FEEDBACK_BUFFER_SIZE];
    int unique_count = 0;

    for (uint8_t i = 0; i < site->count; i++) {
        vtx_typeid_t tid = site->observations[i].value_typeid;
        double w = compute_decay_weight(site->write_index, i);

        bool found = false;
        for (int j = 0; j < unique_count; j++) {
            if (type_weights[j].typeid_ == tid) {
                type_weights[j].weight += w;
                found = true;
                break;
            }
        }
        if (!found && unique_count < VTX_TYPE_FEEDBACK_BUFFER_SIZE) {
            type_weights[unique_count].typeid_ = tid;
            type_weights[unique_count].weight = w;
            unique_count++;
        }
    }

    vtx_typeid_t dominant = VTX_TYPE_INVALID;
    double max_weight = -1.0;
    for (int j = 0; j < unique_count; j++) {
        if (type_weights[j].weight > max_weight) {
            max_weight = type_weights[j].weight;
            dominant = type_weights[j].typeid_;
        }
    }

    return dominant;
}

double vtx_type_feedback_get_branch_probability(
    const vtx_type_feedback_t *feedback, uint32_t site_index)
{
    VTX_ASSERT(feedback != NULL, "feedback must not be NULL");

    if (site_index >= feedback->branch_site_count) {
        return 0.5; /* unknown */
    }

    const vtx_tf_branch_site_t *site = &feedback->branch_sites[site_index];

    if (site->count == 0) {
        return 0.5; /* unknown */
    }

    /* Compute weighted probability with exponential decay */
    double taken_weight = 0.0;
    double total_weight = 0.0;

    for (uint8_t i = 0; i < site->count; i++) {
        double w = compute_decay_weight(site->write_index, i);
        total_weight += w;
        if (site->observations[i].taken) {
            taken_weight += w;
        }
    }

    if (total_weight == 0.0) {
        return 0.5;
    }

    return taken_weight / total_weight;
}

uint32_t vtx_type_feedback_get_call_site_type_count(
    const vtx_type_feedback_t *feedback, uint32_t site_index)
{
    VTX_ASSERT(feedback != NULL, "feedback must not be NULL");

    if (site_index >= feedback->call_site_count) {
        return 0;
    }

    const vtx_tf_call_site_t *site = &feedback->call_sites[site_index];

    if (site->count == 0) {
        return 0;
    }

    /* Count unique receiver typeids */
    vtx_typeid_t seen[VTX_TYPE_FEEDBACK_BUFFER_SIZE];
    uint32_t unique = 0;

    for (uint8_t i = 0; i < site->count; i++) {
        vtx_typeid_t tid = site->observations[i].receiver_typeid;
        bool found = false;
        for (uint32_t j = 0; j < unique; j++) {
            if (seen[j] == tid) {
                found = true;
                break;
            }
        }
        if (!found) {
            seen[unique++] = tid;
        }
    }

    return unique;
}

/* ========================================================================== */
/* D5: Per-type frequency queries and KL divergence                            */
/* ========================================================================== */

const vtx_type_freq_t *vtx_type_feedback_get_type_freq(
    const vtx_type_feedback_t *feedback, uint32_t site_index)
{
    VTX_ASSERT(feedback != NULL, "feedback must not be NULL");

    if (site_index >= feedback->call_site_count) {
        return NULL;
    }

    const vtx_tf_call_site_t *site = &feedback->call_sites[site_index];
    return &site->type_freq;
}

double vtx_type_freq_kl_divergence(const vtx_type_freq_t *current,
                                     const vtx_type_freq_t *compiled)
{
    if (current == NULL || compiled == NULL) return 0.0;
    if (current->total_count == 0 || compiled->total_count == 0) return 0.0;

    double kl = 0.0;

    for (uint32_t i = 0; i < current->entry_count; i++) {
        double p_i = (double)current->entries[i].count / (double)current->total_count;

        /* Find matching type in compiled profile */
        double q_i = 0.0;
        for (uint32_t j = 0; j < compiled->entry_count; j++) {
            if (compiled->entries[j].type_id == current->entries[i].type_id) {
                q_i = (double)compiled->entries[j].count / (double)compiled->total_count;
                break;
            }
        }

        if (q_i > 0.0 && p_i > 0.0) {
            kl += p_i * log(p_i / q_i);
        } else if (q_i == 0.0 && p_i > 0.0) {
            /* New type not seen at compilation time → high divergence.
             * This is the key insight: if a type appears in the current
             * profile that wasn't observed when the code was compiled,
             * the compiled code's guards may miss this type entirely.
             * The penalty of 10.0 ensures this triggers recompilation. */
            kl += p_i * 10.0;
        }
    }

    /* KL divergence is non-negative by definition */
    return kl > 0.0 ? kl : 0.0;
}

/* ========================================================================== */
/* Stable-type signature management (Proposal #2)                                */
/* ========================================================================== */

void vtx_tf_call_site_update_signature(vtx_tf_call_site_t *site,
                                        vtx_typeid_t receiver,
                                        vtx_typeid_t result)
{
    if (site == NULL) return;

    vtx_type_signature_t *sig = &site->stable_signature;

    /* Build the new signature: [receiver, result] */
    vtx_typeid_t new_types[2] = { receiver, result };
    uint32_t new_count = 2;

    /* Check if the new observation matches the existing signature */
    bool matches = true;
    if (sig->slot_count != new_count || sig->slot_count == 0) {
        matches = false;
    } else {
        for (uint32_t i = 0; i < new_count && i < VTX_TYPE_SIGNATURE_MAX_SLOTS; i++) {
            if (sig->types[i] != new_types[i]) {
                matches = false;
                break;
            }
        }
    }

    if (matches) {
        /* Signature matches — increment stability counter (saturating) */
        if (sig->stability_count < UINT32_MAX) {
            sig->stability_count++;
        }
        /* Check for hyper-stability */
        if (sig->stability_count >= VTX_TYPE_STABILITY_WINDOW) {
            sig->is_hyper_stable = true;
        }
    } else {
        /* Signature changed — reset stability and update the signature */
        for (uint32_t i = 0; i < new_count && i < VTX_TYPE_SIGNATURE_MAX_SLOTS; i++) {
            sig->types[i] = new_types[i];
        }
        sig->slot_count = new_count;
        sig->stability_count = 1;
        sig->is_hyper_stable = false;
    }
}

bool vtx_tf_call_site_is_hyper_stable(const vtx_tf_call_site_t *site)
{
    if (site == NULL) return false;
    return site->stable_signature.is_hyper_stable;
}

const vtx_type_signature_t *vtx_tf_call_site_get_signature(
    const vtx_tf_call_site_t *site)
{
    if (site == NULL) return NULL;
    if (site->stable_signature.slot_count == 0) return NULL;
    return &site->stable_signature;
}

uint64_t vtx_type_signature_hash(const vtx_type_signature_t *sig)
{
    if (sig == NULL || sig->slot_count == 0) return 0;

    /* FNV-1a hash over the type IDs */
    uint64_t h = 14695981039346656037ULL;
    for (uint32_t i = 0; i < sig->slot_count && i < VTX_TYPE_SIGNATURE_MAX_SLOTS; i++) {
        uint32_t tid = sig->types[i];
        h ^= (uint64_t)(tid & 0xFF);
        h *= 1099511628211ULL;
        h ^= (uint64_t)((tid >> 8) & 0xFF);
        h *= 1099511628211ULL;
        h ^= (uint64_t)((tid >> 16) & 0xFF);
        h *= 1099511628211ULL;
        h ^= (uint64_t)((tid >> 24) & 0xFF);
        h *= 1099511628211ULL;
    }
    return h;
}

/* ========================================================================== */
/* Shape stability queries (Proposal #9)                                          */
/* ========================================================================== */

bool vtx_tf_field_site_is_shape_stable(const vtx_tf_field_site_t *site)
{
    if (site == NULL) return false;
    return site->is_shape_stable;
}

vtx_shapeid_t vtx_tf_field_site_get_stable_shape(const vtx_tf_field_site_t *site)
{
    if (site == NULL) return VTX_SHAPE_INVALID;
    if (!site->is_shape_stable) return VTX_SHAPE_INVALID;
    return site->last_shapeid;
}
