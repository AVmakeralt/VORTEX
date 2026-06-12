#include "inliner/feedback.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

int vtx_feedback_init(vtx_inline_feedback_t *feedback)
{
    if (feedback == NULL) return -1;

    feedback->decisions = (vtx_inline_decision_t *)malloc(
        VTX_FEEDBACK_INITIAL_CAPACITY * sizeof(vtx_inline_decision_t));
    if (feedback->decisions == NULL) return -1;

    feedback->decision_count = 0;
    feedback->decision_capacity = VTX_FEEDBACK_INITIAL_CAPACITY;
    feedback->profitable_count = 0;
    feedback->unprofitable_count = 0;
    feedback->unknown_count = 0;

    return 0;
}

void vtx_feedback_destroy(vtx_inline_feedback_t *feedback)
{
    if (feedback == NULL) return;

    if (feedback->decisions != NULL) {
        free(feedback->decisions);
        feedback->decisions = NULL;
    }

    feedback->decision_count = 0;
    feedback->decision_capacity = 0;
}

/* ========================================================================== */
/* Internal: grow the decisions array                                          */
/* ========================================================================== */

static int feedback_grow(vtx_inline_feedback_t *feedback)
{
    uint32_t new_capacity = feedback->decision_capacity * 2;
    vtx_inline_decision_t *new_decisions = (vtx_inline_decision_t *)realloc(
        feedback->decisions, new_capacity * sizeof(vtx_inline_decision_t));
    if (new_decisions == NULL) return -1;

    feedback->decisions = new_decisions;
    feedback->decision_capacity = new_capacity;
    return 0;
}

/* ========================================================================== */
/* Internal: find decision by call_site_id                                     */
/* ========================================================================== */

static int32_t feedback_find(const vtx_inline_feedback_t *feedback,
                              uint64_t call_site_id)
{
    for (uint32_t i = 0; i < feedback->decision_count; i++) {
        if (feedback->decisions[i].call_site_id == call_site_id) {
            return (int32_t)i;
        }
    }
    return -1;
}

/* ========================================================================== */
/* Recording                                                                   */
/* ========================================================================== */

int vtx_feedback_record_decision(vtx_inline_feedback_t *feedback,
                                  uint64_t call_site_id,
                                  uint32_t method_id,
                                  const vtx_inline_features_t *features,
                                  bool inlined)
{
    if (feedback == NULL || features == NULL) return -1;

    /* Check if we already have a decision for this call site */
    int32_t existing = feedback_find(feedback, call_site_id);
    if (existing >= 0) {
        /* Update the existing decision with new features and decision */
        vtx_inline_decision_t *dec = &feedback->decisions[existing];

        /* BS-5 fix: decrement old classification counter before resetting */
        if (dec->outcome == VTX_OUTCOME_PROFITABLE) {
            feedback->profitable_count--;
        } else if (dec->outcome == VTX_OUTCOME_UNPROFITABLE) {
            feedback->unprofitable_count--;
        } else {
            feedback->unknown_count--;
        }

        dec->features = *features;
        dec->inlined = inlined;
        dec->outcome = VTX_OUTCOME_UNKNOWN;
        dec->execution_count = 0;
        dec->deopt_count = 0;
        /* BS-20 fix: set the decision timestamp */
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        dec->decision_timestamp = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
        feedback->unknown_count++;
        return 0;
    }

    /* Grow if needed */
    if (feedback->decision_count >= feedback->decision_capacity) {
        if (feedback_grow(feedback) != 0) return -1;
    }

    /* Add new decision */
    vtx_inline_decision_t *dec = &feedback->decisions[feedback->decision_count];
    memset(dec, 0, sizeof(*dec));

    dec->call_site_id = call_site_id;
    dec->method_id = method_id; /* BS-3/BS-4 fix: store method_id */
    dec->features = *features;
    dec->inlined = inlined;
    dec->outcome = VTX_OUTCOME_UNKNOWN;
    dec->execution_count = 0;
    dec->deopt_count = 0;
    /* BS-20 fix: set the decision timestamp */
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        dec->decision_timestamp = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    }

    feedback->decision_count++;
    feedback->unknown_count++;

    return 0;
}

int vtx_feedback_record_outcome(vtx_inline_feedback_t *feedback,
                                 uint64_t call_site_id,
                                 bool profitable)
{
    if (feedback == NULL) return -1;

    int32_t idx = feedback_find(feedback, call_site_id);
    if (idx < 0) return -1;

    vtx_inline_decision_t *dec = &feedback->decisions[idx];

    vtx_inline_outcome_t new_outcome = profitable
        ? VTX_OUTCOME_PROFITABLE
        : VTX_OUTCOME_UNPROFITABLE;

    /* Only allow transitions that make the outcome worse:
     * UNKNOWN → PROFITABLE or UNPROFITABLE
     * PROFITABLE → UNPROFITABLE (late deopt)
     * UNPROFITABLE → UNPROFITABLE (no change) */
    if (dec->outcome == VTX_OUTCOME_UNKNOWN) {
        /* Update counters */
        feedback->unknown_count--;
        if (new_outcome == VTX_OUTCOME_PROFITABLE) {
            feedback->profitable_count++;
        } else {
            feedback->unprofitable_count++;
        }
        dec->outcome = new_outcome;
    } else if (dec->outcome == VTX_OUTCOME_PROFITABLE &&
               new_outcome == VTX_OUTCOME_UNPROFITABLE) {
        /* Late deopt after previously profitable — downgrade */
        feedback->profitable_count--;
        feedback->unprofitable_count++;
        dec->outcome = VTX_OUTCOME_UNPROFITABLE;
    }
    /* UNPROFITABLE stays UNPROFITABLE */

    return 0;
}

void vtx_feedback_increment_executions(vtx_inline_feedback_t *feedback,
                                        uint64_t call_site_id)
{
    if (feedback == NULL) return;

    int32_t idx = feedback_find(feedback, call_site_id);
    if (idx < 0) return;

    vtx_inline_decision_t *dec = &feedback->decisions[idx];

    /* Saturating increment */
    if (dec->execution_count < UINT64_MAX) {
        dec->execution_count++;
    }

    /* Auto-promote to PROFITABLE after feedback window */
    if (dec->outcome == VTX_OUTCOME_UNKNOWN &&
        dec->execution_count >= VTX_FEEDBACK_WINDOW) {
        feedback->unknown_count--;
        feedback->profitable_count++;
        dec->outcome = VTX_OUTCOME_PROFITABLE;
    }
}

void vtx_feedback_increment_deopts(vtx_inline_feedback_t *feedback,
                                    uint64_t call_site_id)
{
    if (feedback == NULL) return;

    int32_t idx = feedback_find(feedback, call_site_id);
    if (idx < 0) return;

    vtx_inline_decision_t *dec = &feedback->decisions[idx];

    /* Saturating increment */
    if (dec->deopt_count < UINT64_MAX) {
        dec->deopt_count++;
    }

    /* Auto-mark as UNPROFITABLE if deopt rate exceeds threshold */
    if (dec->execution_count > 0 && dec->outcome != VTX_OUTCOME_UNPROFITABLE) {
        double deopt_rate = (double)dec->deopt_count / (double)dec->execution_count;
        if (deopt_rate > VTX_INLINE_DEOPT_THRESHOLD) {
            if (dec->outcome == VTX_OUTCOME_UNKNOWN) {
                feedback->unknown_count--;
            } else if (dec->outcome == VTX_OUTCOME_PROFITABLE) {
                feedback->profitable_count--;
            }
            feedback->unprofitable_count++;
            dec->outcome = VTX_OUTCOME_UNPROFITABLE;
        }
    }
}

/* ========================================================================== */
/* Queries                                                                     */
/* ========================================================================== */

bool vtx_feedback_should_recompile(vtx_inline_feedback_t *feedback,
                                    uint64_t call_site_id)
{
    if (feedback == NULL) return false;

    int32_t idx = feedback_find(feedback, call_site_id);
    if (idx < 0) return false;

    const vtx_inline_decision_t *dec = &feedback->decisions[idx];

    /* Recompile if inlined and outcome is unprofitable */
    if (dec->inlined && dec->outcome == VTX_OUTCOME_UNPROFITABLE) {
        return true;
    }

    /* Recompile if NOT inlined but the site has high execution count
     * and no deopt history, suggesting it might be safe to inline now.
     * This is a heuristic: if we've executed many times without issue,
     * the callee is likely stable. */
    if (!dec->inlined &&
        dec->outcome == VTX_OUTCOME_UNKNOWN &&
        dec->execution_count > VTX_FEEDBACK_WINDOW / 2) {
        return true;
    }

    return false;
}

const vtx_inline_decision_t *vtx_feedback_lookup(
    const vtx_inline_feedback_t *feedback,
    uint64_t call_site_id)
{
    if (feedback == NULL) return NULL;

    int32_t idx = feedback_find(feedback, call_site_id);
    if (idx < 0) return NULL;

    return &feedback->decisions[idx];
}

/* ========================================================================== */
/* CSV export                                                                  */
/* ========================================================================== */

int vtx_feedback_write_csv(const vtx_inline_feedback_t *feedback,
                            const char *filename)
{
    if (feedback == NULL || filename == NULL) return -1;

    FILE *fp = fopen(filename, "w");
    if (fp == NULL) return -1;

    /* Write header */
    for (uint32_t i = 0; i < VTX_INLINE_FEATURE_COUNT; i++) {
        if (i > 0) fprintf(fp, ",");
        fprintf(fp, "%s", vtx_feature_name(i));
    }
    fprintf(fp, ",label\n");

    /* Write data rows */
    int records_written = 0;
    char feature_buf[1024];

    for (uint32_t i = 0; i < feedback->decision_count; i++) {
        const vtx_inline_decision_t *dec = &feedback->decisions[i];

        /* Skip decisions with unknown outcome */
        if (dec->outcome == VTX_OUTCOME_UNKNOWN) continue;

        /* Write features as CSV */
        int n = vtx_features_to_csv(&dec->features, feature_buf, sizeof(feature_buf));
        if (n < 0) {
            fclose(fp);
            return -1;
        }

        /* Write label: 1 = profitable, 0 = unprofitable */
        int label = (dec->outcome == VTX_OUTCOME_PROFITABLE) ? 1 : 0;
        fprintf(fp, "%s,%d\n", feature_buf, label);
        records_written++;
    }

    fclose(fp);
    return records_written;
}
