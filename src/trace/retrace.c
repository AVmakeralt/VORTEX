/* ========================================================================== */
/* Trace-Based PGO Re-Scoping — Implementation                                 */
/* ========================================================================== */

#include "trace/retrace.h"
#include "compile/orchestrator.h"  /* full definition for vtx_orchestrator_t */
#include "compile/decision.h"
#include "profile/data.h"
#include "guard/metadata.h"

#include <string.h>
#include <stdlib.h>

/* Hash a method_id to a slot index using Fibonacci hashing. */
static uint32_t hash_method(uint32_t method_id, uint32_t capacity) {
    return (method_id * 2654435761u) & (capacity - 1);
}

int vtx_trace_retrace_init(vtx_trace_retrace_t *rt, uint32_t capacity)
{
    if (!rt) return -1;
    if (capacity == 0) capacity = 256;
    /* Round up to power of 2 for fast masking */
    uint32_t cap = 1;
    while (cap < capacity) cap <<= 1;

    rt->states = (vtx_retrace_state_t *)calloc(cap, sizeof(vtx_retrace_state_t));
    if (!rt->states) return -1;
    rt->capacity = cap;
    rt->retrace_count = 0;
    return 0;
}

void vtx_trace_retrace_destroy(vtx_trace_retrace_t *rt)
{
    if (!rt) return;
    free(rt->states);
    rt->states = NULL;
    rt->capacity = 0;
    rt->retrace_count = 0;
}

/* Find or create the re-trace state for a method.
 * Uses open addressing with linear probing. */
static vtx_retrace_state_t *get_or_create_state(vtx_trace_retrace_t *rt,
                                                   uint32_t method_id)
{
    if (!rt || !rt->states) return NULL;

    uint32_t mask = rt->capacity - 1;
    uint32_t idx = hash_method(method_id, rt->capacity);

    /* Linear probe for existing entry or empty slot */
    for (uint32_t i = 0; i < rt->capacity; i++) {
        vtx_retrace_state_t *s = &rt->states[idx];
        if (!s->active) {
            /* Empty slot — create new entry */
            s->method_id = method_id;
            s->active = true;
            s->check_cooldown = 0;
            s->attempt_count = 0;
            s->last_failed_guard = 0;
            s->total_retraces = 0;
            return s;
        }
        if (s->method_id == method_id) {
            return s;  /* Found existing entry */
        }
        idx = (idx + 1) & mask;
    }
    return NULL;  /* Table full */
}

/* Find existing state (no creation). Returns NULL if not found. */
static vtx_retrace_state_t *find_state(vtx_trace_retrace_t *rt,
                                         uint32_t method_id)
{
    if (!rt || !rt->states) return NULL;

    uint32_t mask = rt->capacity - 1;
    uint32_t idx = hash_method(method_id, rt->capacity);

    for (uint32_t i = 0; i < rt->capacity; i++) {
        vtx_retrace_state_t *s = &rt->states[idx];
        if (!s->active) return NULL;  /* Empty slot → not found */
        if (s->method_id == method_id) return s;
        idx = (idx + 1) & mask;
    }
    return NULL;
}

void vtx_trace_retrace_record_failure(vtx_trace_retrace_t *rt,
                                        uint32_t method_id,
                                        uint32_t guard_id,
                                        vtx_profile_global_t *profile,
                                        vtx_guard_meta_table_t *guard_table)
{
    if (!rt) return;

    vtx_retrace_state_t *state = get_or_create_state(rt, method_id);
    if (!state) return;

    state->last_failed_guard = guard_id;
    state->failure_count++;  /* increment for threshold-based re-tracing */

    /* Feed the guard failure back into the profile data.
     *
     * When a guard fails, the branch that the trace assumed was always-taken
     * (or always-not-taken) is now wrong. We update the branch profile to
     * reflect the actual execution: if the trace took the "true" branch and
     * the guard failed, increment "not_taken" so the next trace recording
     * picks the other path.
     *
     * The guard_id IS the NodeID of the DeoptGuard node. We don't have a
     * direct mapping from guard_id to branch PC here (that would require
     * the guard metadata table's bytecode_pc field). Let's look it up. */
    if (guard_table != NULL) {
        vtx_guard_meta_t *meta = vtx_guard_meta_lookup(guard_table, guard_id);
        if (meta != NULL) {
            /* Update the branch profile for this PC.
             * The guard's bytecode_pc tells us which branch to update.
             * If the guard was a "taken" guard (cond=NE), the failure means
             * the branch was NOT taken → increment not_taken.
             * If the guard was a "not taken" guard (cond=EQ), the failure
             * means the branch WAS taken → increment taken. */
            if (profile != NULL && meta->bytecode_pc > 0) {
                vtx_profile_method_t *m = vtx_profile_add_method(
                    profile, method_id);
                if (m != NULL) {
                    /* Find the branch profile for this PC, or create it.
                     * Increment not_taken to signal the trace's path was wrong. */
                    vtx_branch_profile_t *bp = NULL;
                    for (uint32_t i = 0; i < m->branch_count; i++) {
                        if (m->branches[i].bytecode_pc == meta->bytecode_pc) {
                            bp = &m->branches[i];
                            break;
                        }
                    }
                    if (bp == NULL && m->branch_count < 256) {
                        bp = &m->branches[m->branch_count++];
                        bp->bytecode_pc = meta->bytecode_pc;
                        bp->taken = 0;
                        bp->not_taken = 0;
                    }
                    if (bp) {
                        /* The guard failed, meaning the trace's assumed path
                         * was wrong. Increment the "wrong" direction. */
                        bp->not_taken++;  /* conservative: assume trace assumed taken */
                    }
                }
            }
        }
    }
}

uint32_t vtx_trace_retrace_check(vtx_trace_retrace_t *rt,
                                   vtx_orchestrator_fwd_t *orch_fwd)
{
    if (!rt || !rt->states || !orch_fwd) return 0;
    /* Cast to the full orchestrator type — safe because orchestrator.h
     * is included in this .c file. */
    vtx_orchestrator_t *orch = (vtx_orchestrator_t *)orch_fwd;

    uint32_t submitted = 0;

    for (uint32_t i = 0; i < rt->capacity; i++) {
        vtx_retrace_state_t *s = &rt->states[i];
        if (!s->active) continue;

        /* Decrement cooldown */
        if (s->check_cooldown > 0) {
            s->check_cooldown--;
            continue;
        }

        /* Check if we've exceeded max attempts */
        if (s->attempt_count >= VTX_RETRACE_MAX_ATTEMPTS) {
            continue;  /* Give up on this method */
        }

        /* Check the guard metadata for the last failed guard.
         * If the failure rate is above the threshold, trigger re-tracing. */
        vtx_guard_meta_table_t *guard_table = NULL;
        /* TODO: wire the guard table from the orchestrator. For now,
         * we use a simpler heuristic: if the method has any re-trace
         * state at all (meaning it deoptimized), and the cooldown is
         * zero, submit a re-trace.
         *
         * In a full implementation, we'd check:
         *   meta = vtx_guard_meta_lookup(guard_table, s->last_failed_guard)
         *   if (meta->execution_count >= VTX_RETRACE_MIN_EXECUTIONS &&
         *       meta->failure_count >= VTX_RETRACE_MIN_FAILURES &&
         *       vtx_guard_meta_failure_rate(meta) >= VTX_RETRACE_FAILURE_RATE_THRESHOLD)
         *       { submit re-trace }
         */

        /* Check if the failure count exceeds the re-trace threshold.
         * This replaces the guard metadata table check — we track
         * failures directly in the re-trace state. */
        if (s->failure_count >= VTX_RETRACE_MIN_FAILURES) {
            vtx_compile_task_t task;
            memset(&task, 0, sizeof(task));
            task.method_id = s->method_id;
            task.tier = VTX_TIER_T2;  /* Re-trace at T2 */
            task.priority = VTX_COMPILE_PRIORITY_HIGH;

            if (vtx_decision_submit_compile(orch, &task,
                                              VTX_DECISION_REASON_RETRACE) == 0) {
                s->attempt_count++;
                s->total_retraces++;
                s->check_cooldown = VTX_RETRACE_COOLDOWN_CHECKS;
                s->last_failed_guard = 0;
                s->failure_count = 0;  /* Reset until next failure cycle */
                rt->retrace_count++;
                submitted++;
            }
        }
    }

    return submitted;
}

void vtx_trace_retrace_on_compile_done(vtx_trace_retrace_t *rt,
                                         uint32_t method_id)
{
    if (!rt) return;
    vtx_retrace_state_t *s = find_state(rt, method_id);
    if (s) {
        /* Reset the cooldown — the new trace is installed, ready for
         * new guard failures. Don't reset attempt_count — that's
         * cumulative to prevent infinite re-trace loops. */
        s->check_cooldown = 0;
        s->last_failed_guard = 0;
    }
}

vtx_retrace_stats_t vtx_trace_retrace_stats(const vtx_trace_retrace_t *rt)
{
    vtx_retrace_stats_t stats = {};
    if (!rt || !rt->states) return stats;

    for (uint32_t i = 0; i < rt->capacity; i++) {
        const vtx_retrace_state_t *s = &rt->states[i];
        if (!s->active) continue;
        stats.active_methods++;
        if (s->attempt_count >= VTX_RETRACE_MAX_ATTEMPTS) {
            stats.methods_at_max++;
        }
    }
    stats.total_retraces = rt->retrace_count;
    return stats;
}
