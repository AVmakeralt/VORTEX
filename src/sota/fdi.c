#include "sota/fdi.h"
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* Lifecycle                                                                   */
/* ========================================================================== */

int vtx_sota_fdi_init(vtx_sota_fdi_t *fdi, vtx_inline_feedback_t *feedback)
{
    if (fdi == NULL) return -1;

    fdi->records = (vtx_fdi_method_record_t *)calloc(
        VTX_FDI_INITIAL_CAPACITY, sizeof(vtx_fdi_method_record_t));
    if (fdi->records == NULL) return -1;

    fdi->record_count = 0;
    fdi->record_capacity = VTX_FDI_INITIAL_CAPACITY;
    fdi->feedback = feedback;
    fdi->total_recompilations = 0;
    fdi->total_methods_improved = 0;
    fdi->total_methods_worse = 0;

    return 0;
}

void vtx_sota_fdi_destroy(vtx_sota_fdi_t *fdi)
{
    if (fdi == NULL) return;

    if (fdi->records != NULL) {
        for (uint32_t i = 0; i < fdi->record_count; i++) {
            vtx_fdi_method_record_t *rec = &fdi->records[i];
            if (rec->no_inline_sites != NULL) free(rec->no_inline_sites);
            if (rec->force_inline_sites != NULL) free(rec->force_inline_sites);
            if (rec->alternative_versions != NULL) free(rec->alternative_versions);
        }
        free(fdi->records);
        fdi->records = NULL;
    }

    fdi->record_count = 0;
    fdi->record_capacity = 0;
}

/* ========================================================================== */
/* Internal: find or create method record                                      */
/* ========================================================================== */

static vtx_fdi_method_record_t *find_record(vtx_sota_fdi_t *fdi, uint32_t method_id)
{
    for (uint32_t i = 0; i < fdi->record_count; i++) {
        if (fdi->records[i].method_id == method_id) {
            return &fdi->records[i];
        }
    }
    return NULL;
}

static vtx_fdi_method_record_t *create_record(vtx_sota_fdi_t *fdi, uint32_t method_id)
{
    /* Check if already exists */
    vtx_fdi_method_record_t *existing = find_record(fdi, method_id);
    if (existing != NULL) return existing;

    /* Grow if needed */
    if (fdi->record_count >= fdi->record_capacity) {
        uint32_t new_cap = fdi->record_capacity * 2;
        vtx_fdi_method_record_t *new_recs = (vtx_fdi_method_record_t *)realloc(
            fdi->records, new_cap * sizeof(vtx_fdi_method_record_t));
        if (new_recs == NULL) return NULL;
        memset(new_recs + fdi->record_capacity, 0,
               (new_cap - fdi->record_capacity) * sizeof(vtx_fdi_method_record_t));
        fdi->records = new_recs;
        fdi->record_capacity = new_cap;
    }

    vtx_fdi_method_record_t *rec = &fdi->records[fdi->record_count];
    memset(rec, 0, sizeof(*rec));
    rec->method_id = method_id;

    /* Allocate site arrays */
    rec->no_inline_capacity = 8;
    rec->no_inline_sites = (uint64_t *)malloc(
        rec->no_inline_capacity * sizeof(uint64_t));
    rec->no_inline_count = 0;

    rec->force_inline_capacity = 8;
    rec->force_inline_sites = (uint64_t *)malloc(
        rec->force_inline_capacity * sizeof(uint64_t));
    rec->force_inline_count = 0;

    /* Initialize performance tracking */
    rec->spill_rate = 0.0;
    rec->deopt_rate = 0.0;
    rec->spill_count = 0;
    rec->total_instructions = 0;

    /* Initialize version tracking */
    rec->alternative_version_capacity = 4;
    rec->alternative_versions = (uint32_t *)malloc(
        rec->alternative_version_capacity * sizeof(uint32_t));
    rec->alternative_version_count = 0;
    rec->best_version_id = 0;
    rec->best_version_score = 0.0;

    if (rec->no_inline_sites == NULL || rec->force_inline_sites == NULL ||
        rec->alternative_versions == NULL) {
        if (rec->no_inline_sites != NULL) free(rec->no_inline_sites);
        if (rec->force_inline_sites != NULL) free(rec->force_inline_sites);
        if (rec->alternative_versions != NULL) free(rec->alternative_versions);
        return NULL;
    }

    fdi->record_count++;
    return rec;
}

/* ========================================================================== */
/* Internal: add a call site to the no-inline list                             */
/* ========================================================================== */

static void add_no_inline_site(vtx_fdi_method_record_t *rec, uint64_t call_site_id)
{
    /* Check for duplicates */
    for (uint32_t i = 0; i < rec->no_inline_count; i++) {
        if (rec->no_inline_sites[i] == call_site_id) return;
    }

    /* Grow if needed */
    if (rec->no_inline_count >= rec->no_inline_capacity) {
        uint32_t new_cap = rec->no_inline_capacity * 2;
        uint64_t *new_arr = (uint64_t *)realloc(
            rec->no_inline_sites, new_cap * sizeof(uint64_t));
        if (new_arr == NULL) return;
        rec->no_inline_sites = new_arr;
        rec->no_inline_capacity = new_cap;
    }

    rec->no_inline_sites[rec->no_inline_count++] = call_site_id;
}

/* ========================================================================== */
/* Internal: add a call site to the force-inline list                          */
/* ========================================================================== */

static void add_force_inline_site(vtx_fdi_method_record_t *rec, uint64_t call_site_id)
{
    /* Check for duplicates */
    for (uint32_t i = 0; i < rec->force_inline_count; i++) {
        if (rec->force_inline_sites[i] == call_site_id) return;
    }

    /* Grow if needed */
    if (rec->force_inline_count >= rec->force_inline_capacity) {
        uint32_t new_cap = rec->force_inline_capacity * 2;
        uint64_t *new_arr = (uint64_t *)realloc(
            rec->force_inline_sites, new_cap * sizeof(uint64_t));
        if (new_arr == NULL) return;
        rec->force_inline_sites = new_arr;
        rec->force_inline_capacity = new_cap;
    }

    rec->force_inline_sites[rec->force_inline_count++] = call_site_id;
}

/* ========================================================================== */
/* Evaluation                                                                  */
/* ========================================================================== */

bool vtx_sota_fdi_evaluate(vtx_sota_fdi_t *fdi, uint32_t method_id)
{
    if (fdi == NULL) return false;

    vtx_fdi_method_record_t *rec = find_record(fdi, method_id);

    /* Check if any inlined call sites are unprofitable */
    bool has_unprofitable = false;

    if (fdi->feedback != NULL) {
        /* Scan feedback decisions for this method's call sites */
        for (uint32_t i = 0; i < fdi->feedback->decision_count; i++) {
            const vtx_inline_decision_t *dec = &fdi->feedback->decisions[i];

            /* We need to know which decisions belong to this method.
             * For now, we check if the decision is unprofitable and
             * the call site was inlined. A more precise implementation
             * would track method_id in the feedback decisions. */
            if (dec->inlined && dec->outcome == VTX_OUTCOME_UNPROFITABLE) {
                has_unprofitable = true;

                /* Create or update the method record */
                if (rec == NULL) {
                    rec = create_record(fdi, method_id);
                }
                if (rec != NULL) {
                    rec->has_unprofitable_inlines = true;
                    rec->unprofitable_site_count++;
                    add_no_inline_site(rec, dec->call_site_id);
                }
            }

            /* Check for missed inline opportunities: call site was NOT
             * inlined, has high execution count, and low deopt rate.
             * This suggests it might benefit from inlining. */
            if (!dec->inlined &&
                dec->outcome == VTX_OUTCOME_UNKNOWN &&
                dec->execution_count > VTX_FEEDBACK_WINDOW / 2 &&
                dec->deopt_count == 0) {
                if (rec == NULL) {
                    rec = create_record(fdi, method_id);
                }
                if (rec != NULL) {
                    rec->has_missed_inlines = true;
                    rec->missed_site_count++;
                    add_force_inline_site(rec, dec->call_site_id);
                }
            }
        }
    }

    /* Also check the method record's deopt rate */
    if (rec != NULL && rec->total_executions > 0) {
        double deopt_rate = (double)rec->deopt_count_at_inlined_sites /
                            (double)rec->total_executions;
        if (deopt_rate > VTX_INLINE_DEOPT_THRESHOLD) {
            has_unprofitable = true;
        }
    }

    /* Recommend recompilation if there are unprofitable or missed inlines */
    return (rec != NULL && (rec->has_unprofitable_inlines || rec->has_missed_inlines));
}

/* ========================================================================== */
/* Get directives                                                              */
/* ========================================================================== */

int vtx_sota_fdi_get_directives(vtx_sota_fdi_t *fdi,
                                  uint32_t method_id,
                                  vtx_arena_t *arena,
                                  uint64_t **no_inline,
                                  uint32_t *no_inline_count,
                                  uint64_t **force_inline,
                                  uint32_t *force_inline_count)
{
    if (fdi == NULL || arena == NULL || no_inline == NULL ||
        no_inline_count == NULL || force_inline == NULL ||
        force_inline_count == NULL) {
        return -1;
    }

    *no_inline = NULL;
    *no_inline_count = 0;
    *force_inline = NULL;
    *force_inline_count = 0;

    vtx_fdi_method_record_t *rec = find_record(fdi, method_id);
    if (rec == NULL) return 0;

    /* Copy no-inline sites to arena-allocated array */
    if (rec->no_inline_count > 0) {
        uint64_t *arr = (uint64_t *)vtx_arena_alloc(
            arena, rec->no_inline_count * sizeof(uint64_t));
        if (arr == NULL) return -1;
        memcpy(arr, rec->no_inline_sites, rec->no_inline_count * sizeof(uint64_t));
        *no_inline = arr;
        *no_inline_count = rec->no_inline_count;
    }

    /* Copy force-inline sites to arena-allocated array */
    if (rec->force_inline_count > 0) {
        uint64_t *arr = (uint64_t *)vtx_arena_alloc(
            arena, rec->force_inline_count * sizeof(uint64_t));
        if (arr == NULL) return -1;
        memcpy(arr, rec->force_inline_sites, rec->force_inline_count * sizeof(uint64_t));
        *force_inline = arr;
        *force_inline_count = rec->force_inline_count;
    }

    return 0;
}

/* ========================================================================== */
/* Recording                                                                   */
/* ========================================================================== */

void vtx_sota_fdi_record_deopt(vtx_sota_fdi_t *fdi,
                                 uint32_t method_id,
                                 uint64_t call_site_id)
{
    if (fdi == NULL) return;

    vtx_fdi_method_record_t *rec = find_record(fdi, method_id);
    if (rec == NULL) {
        rec = create_record(fdi, method_id);
    }
    if (rec == NULL) return;

    /* Increment deopt count (saturating) */
    if (rec->deopt_count_at_inlined_sites < UINT64_MAX) {
        rec->deopt_count_at_inlined_sites++;
    }

    /* Add this call site to the no-inline list for next recompilation */
    add_no_inline_site(rec, call_site_id);

    /* Also record in the feedback system */
    if (fdi->feedback != NULL) {
        vtx_feedback_increment_deopts(fdi->feedback, call_site_id);
    }
}

void vtx_sota_fdi_record_execution(vtx_sota_fdi_t *fdi, uint32_t method_id)
{
    if (fdi == NULL) return;

    vtx_fdi_method_record_t *rec = find_record(fdi, method_id);
    if (rec == NULL) {
        rec = create_record(fdi, method_id);
    }
    if (rec == NULL) return;

    /* Increment execution count (saturating) */
    if (rec->total_executions < UINT64_MAX) {
        rec->total_executions++;
    }

    /* Also record in the feedback system for any tracked call sites */
    if (fdi->feedback != NULL) {
        for (uint32_t i = 0; i < fdi->feedback->decision_count; i++) {
            vtx_inline_decision_t *dec = &fdi->feedback->decisions[i];
            vtx_feedback_increment_executions(fdi->feedback, dec->call_site_id);
        }
    }
}

void vtx_sota_fdi_record_recompilation(vtx_sota_fdi_t *fdi,
                                         uint32_t method_id,
                                         uint32_t new_version)
{
    if (fdi == NULL) return;

    vtx_fdi_method_record_t *rec = find_record(fdi, method_id);
    if (rec == NULL) {
        rec = create_record(fdi, method_id);
    }
    if (rec == NULL) return;

    rec->current_version_id = new_version;
    rec->recompilation_count++;
    rec->deopt_count_at_inlined_sites = 0;
    rec->total_executions = 0;

    /* Clear the no-inline and force-inline lists — they've been applied */
    rec->no_inline_count = 0;
    rec->force_inline_count = 0;
    rec->has_unprofitable_inlines = false;
    rec->has_missed_inlines = false;
    rec->unprofitable_site_count = 0;
    rec->missed_site_count = 0;

    fdi->total_recompilations++;
}

/* ========================================================================== */
/* Performance tracking                                                        */
/* ========================================================================== */

void vtx_sota_fdi_record_performance(vtx_sota_fdi_t *fdi,
                                       uint32_t method_id,
                                       double spill_rate,
                                       double deopt_rate)
{
    if (fdi == NULL) return;

    vtx_fdi_method_record_t *rec = find_record(fdi, method_id);
    if (rec == NULL) {
        rec = create_record(fdi, method_id);
    }
    if (rec == NULL) return;

    /* Update performance metrics */
    rec->spill_rate = spill_rate;
    rec->deopt_rate = deopt_rate;

    /* Compute performance score for this version:
     *   score = (1 - spill_rate) * (1 - deopt_rate)
     * This gives a value in [0, 1] where 1.0 is perfect (no spills, no deopts).
     * A score of 0 means the code is entirely useless. */
    double score = (1.0 - spill_rate) * (1.0 - deopt_rate);

    /* Update best version tracking */
    if (score > rec->best_version_score) {
        rec->best_version_score = score;
        rec->best_version_id = rec->current_version_id;
    }

    /* Check if spill rate exceeds threshold.
     * High spill rate means inlining increased register pressure too much.
     * The method should be recompiled with fewer inlines. */
    if (spill_rate > VTX_SPILL_THRESHOLD) {
        rec->has_unprofitable_inlines = true;

        /* Force all currently inlined call sites to no-inline
         * in the next recompilation. We don't know which specific
         * site caused the register pressure, so we conservatively
         * disable all inlined sites. A more precise implementation
         * would use register pressure estimation per call site. */
        if (fdi->feedback != NULL) {
            for (uint32_t i = 0; i < fdi->feedback->decision_count; i++) {
                const vtx_inline_decision_t *dec = &fdi->feedback->decisions[i];
                if (dec->inlined) {
                    add_no_inline_site(rec, dec->call_site_id);
                }
            }
        }
    }

    /* Check if deopt rate exceeds threshold.
     * High deopt rate means inlined guards are failing too often.
     * The specific call sites causing deopts should be forced no-inline. */
    if (deopt_rate > VTX_INLINE_DEOPT_THRESHOLD) {
        rec->has_unprofitable_inlines = true;

        /* Find call sites with high deopt counts and force them no-inline */
        if (fdi->feedback != NULL) {
            for (uint32_t i = 0; i < fdi->feedback->decision_count; i++) {
                const vtx_inline_decision_t *dec = &fdi->feedback->decisions[i];
                if (dec->inlined && dec->deopt_count > 0) {
                    double site_deopt_rate = (dec->execution_count > 0)
                        ? (double)dec->deopt_count / (double)dec->execution_count
                        : 1.0;
                    if (site_deopt_rate > VTX_INLINE_DEOPT_THRESHOLD) {
                        add_no_inline_site(rec, dec->call_site_id);
                    }
                }
            }
        }
    }

    /* Feed back to the ML model immediately.
     * Record the outcome in the feedback system for each inlined call site.
     * The outcome (profitable vs unprofitable) is determined by whether
     * the spill/deopt rates exceed their thresholds. */
    if (fdi->feedback != NULL) {
        bool overall_profitable = (spill_rate <= VTX_SPILL_THRESHOLD &&
                                    deopt_rate <= VTX_INLINE_DEOPT_THRESHOLD);

        for (uint32_t i = 0; i < fdi->feedback->decision_count; i++) {
            const vtx_inline_decision_t *dec = &fdi->feedback->decisions[i];
            if (dec->inlined) {
                vtx_feedback_record_outcome(fdi->feedback, dec->call_site_id,
                                             overall_profitable);
            }
        }
    }
}

uint32_t vtx_sota_fdi_get_replacement_version(vtx_sota_fdi_t *fdi,
                                                 uint32_t method_id)
{
    if (fdi == NULL) return 0;

    vtx_fdi_method_record_t *rec = find_record(fdi, method_id);
    if (rec == NULL) return 0;

    /* If we have a best version that's different from the current one,
     * return it. Otherwise return the current version. */
    if (rec->best_version_id != 0 &&
        rec->best_version_id != rec->current_version_id &&
        rec->best_version_score > 0.0) {
        return rec->best_version_id;
    }

    /* Check if there are alternative versions with better scores.
     * We pick the one with the highest score. */
    double best_score = rec->best_version_score;
    uint32_t best_version = rec->best_version_id;

    /* The alternative_versions array tracks versions we know about.
     * We need their scores to compare. For now, since we track
     * best_version_id/score when updating, we just return that.
     * A more complete implementation would store per-version scores. */

    return best_version != 0 ? best_version : rec->current_version_id;
}

void vtx_sota_fdi_register_version(vtx_sota_fdi_t *fdi,
                                     uint32_t method_id,
                                     uint32_t version_id)
{
    if (fdi == NULL) return;

    vtx_fdi_method_record_t *rec = find_record(fdi, method_id);
    if (rec == NULL) {
        rec = create_record(fdi, method_id);
    }
    if (rec == NULL) return;

    /* Check for duplicate */
    for (uint32_t i = 0; i < rec->alternative_version_count; i++) {
        if (rec->alternative_versions[i] == version_id) return;
    }

    /* Grow if needed */
    if (rec->alternative_version_count >= rec->alternative_version_capacity) {
        uint32_t new_cap = rec->alternative_version_capacity * 2;
        uint32_t *new_arr = (uint32_t *)realloc(
            rec->alternative_versions, new_cap * sizeof(uint32_t));
        if (new_arr == NULL) return;
        rec->alternative_versions = new_arr;
        rec->alternative_version_capacity = new_cap;
    }

    rec->alternative_versions[rec->alternative_version_count++] = version_id;

    /* If this is the first version, set it as the best */
    if (rec->best_version_id == 0) {
        rec->best_version_id = version_id;
        rec->best_version_score = 1.0; /* assume best until proven otherwise */
    }
}

void vtx_sota_fdi_update_version_score(vtx_sota_fdi_t *fdi,
                                         uint32_t method_id,
                                         uint32_t version_id,
                                         double score)
{
    if (fdi == NULL) return;

    vtx_fdi_method_record_t *rec = find_record(fdi, method_id);
    if (rec == NULL) return;

    /* Update best version if this score is better */
    if (score > rec->best_version_score) {
        rec->best_version_score = score;
        rec->best_version_id = version_id;
    }
}
