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
    return pow(0.9, (double)distance);
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
