#include "deopt/frame_state.h"
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* Creation                                                                   */
/* ========================================================================== */

vtx_frame_state_t *vtx_frame_state_create(vtx_arena_t *arena,
                                           uint32_t pc,
                                           uint32_t method_id,
                                           uint32_t local_count,
                                           uint32_t stack_count)
{
    vtx_frame_state_t *fs = vtx_arena_alloc(arena, sizeof(vtx_frame_state_t));
    if (!fs) return NULL;

    memset(fs, 0, sizeof(*fs));
    fs->bytecode_pc = pc;
    fs->method_id = method_id;
    fs->local_count = local_count;
    fs->stack_count = stack_count;
    fs->caller = NULL;
    fs->relock_needed = false;

    /* Exception state: no handler by default */
    fs->exception.handler_pc = VTX_DEOPT_NO_HANDLER;
    fs->exception.catch_type = 0;

    /* Allocate locals array */
    if (local_count > 0) {
        fs->locals = vtx_arena_alloc(arena,
                                      (size_t)local_count * sizeof(vtx_nodeid_t));
        if (!fs->locals) return NULL;
        for (uint32_t i = 0; i < local_count; i++) {
            fs->locals[i] = VTX_NODEID_INVALID;
        }
    } else {
        fs->locals = NULL;
    }

    /* Allocate stack array */
    if (stack_count > 0) {
        fs->stack = vtx_arena_alloc(arena,
                                     (size_t)stack_count * sizeof(vtx_nodeid_t));
        if (!fs->stack) return NULL;
        for (uint32_t i = 0; i < stack_count; i++) {
            fs->stack[i] = VTX_NODEID_INVALID;
        }
    } else {
        fs->stack = NULL;
    }

    /* Monitors: initially none */
    fs->monitors = NULL;
    fs->monitor_count = 0;

    return fs;
}

vtx_frame_state_t *vtx_frame_state_chain_create(vtx_arena_t *arena,
                                                  const uint32_t *frame_pcs,
                                                  const uint32_t *method_ids,
                                                  const uint32_t *local_counts,
                                                  const uint32_t *stack_counts,
                                                  uint32_t depth)
{
    if (depth == 0) return NULL;

    vtx_frame_state_t *innermost = NULL;
    vtx_frame_state_t *prev = NULL;

    for (uint32_t i = 0; i < depth; i++) {
        vtx_frame_state_t *fs = vtx_frame_state_create(
            arena, frame_pcs[i], method_ids[i],
            local_counts[i], stack_counts[i]);
        if (!fs) return NULL;

        if (i == 0) {
            innermost = fs;
        } else {
            /* Link: previous frame's caller = this frame */
            prev->caller = fs;
        }
        prev = fs;
    }

    return innermost;
}

/* ========================================================================== */
/* Accessors                                                                  */
/* ========================================================================== */

uint32_t vtx_frame_state_chain_depth(const vtx_frame_state_t *fs)
{
    uint32_t depth = 0;
    for (const vtx_frame_state_t *cur = fs; cur != NULL; cur = cur->caller) {
        depth++;
    }
    return depth;
}

const vtx_frame_state_t *vtx_frame_state_nth_caller(const vtx_frame_state_t *fs,
                                                       uint32_t n)
{
    const vtx_frame_state_t *cur = fs;
    for (uint32_t i = 0; i < n && cur != NULL; i++) {
        cur = cur->caller;
    }
    return cur;
}
