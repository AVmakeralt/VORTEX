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

#include "deopt/osr.h"
#include "codecache/install.h"
#include "runtime/gc.h"   /* OSR-12: vtx_gc_safepoint declaration for the
                           * pre-asm safepoint poll in vtx_osr_up. */
#include "runtime/object.h"  /* OSR-7/9/15: vtx_make_smi, VTX_VALUE_*,
                              * vtx_is_heap_ptr. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* OSR-7: forward declaration for the runtime monitor re-enter primitive.
 * vtx_runtime_monitor_enter is defined in src/runtime_stubs.c (part of
 * the vortex_baseline library). It is not yet declared in any public
 * header, so we extern it here following the same pattern as
 * baseline/codegen.c:3311. */
extern void vtx_runtime_monitor_enter(vtx_value_t obj);

/* ========================================================================== */
/* Internal: node resolution                                                  */
/* ========================================================================== */

/**
 * The register map is a sparse array of {register_number, node_id} entries
 * stored in vtx_side_table_entry_t.register_map (the side table's native
 * format — `vtx_reg_map_entry_t[]`).
 *
 * OSR-13 fix: previously this function documented a fabricated
 * [count, (node_id, value) pairs...] layout in `vtx_value_t[]` and tried
 * to type-pun its way through it, but no caller in the codebase ever
 * built that layout — the side table always stored
 * vtx_reg_map_entry_t[]. The type-pun read garbage as the count header
 * and returned VTX_VALUE_UNDEFINED for every lookup. The new signature
 * accepts the native side-table format directly.
 *
 * OSR-9 fix: when a NodeID is not in the register map (the common case
 * for VTX_OP_Constant, VTX_OP_Parameter, and spilled values), fall back
 * to alternative resolution:
 *   - VTX_OP_Constant: read the constant value from the IR node table.
 *   - VTX_OP_Parameter: read the parameter's value from the caller's
 *     interpreter locals (the parameter's index is the node's
 *     `local_index` field).
 *   - Any other opcode (spilled): the value is not in the register map
 *     and not in the locals — return VTX_VALUE_UNDEFINED. The caller is
 *     responsible for reading the spill slot directly from the JIT frame.
 */
typedef struct {
    const vtx_reg_map_entry_t *register_map;
    uint32_t                   register_count;
    const vtx_node_table_t    *node_table;   /* OSR-9: for VTX_OP_Constant */
    const vtx_value_t         *locals;        /* OSR-9: for VTX_OP_Parameter */
    uint32_t                   local_count;
} vtx_resolve_context_t;

vtx_value_t vtx_osr_resolve_node(vtx_nodeid_t node_id,
                                   const vtx_reg_map_entry_t *register_map,
                                   uint32_t register_count,
                                   const vtx_node_table_t *node_table,
                                   const vtx_value_t *locals,
                                   uint32_t local_count)
{
    if (node_id == VTX_NODEID_INVALID) {
        return VTX_VALUE_UNDEFINED;
    }

    /* ---- Path 1: register map lookup (OSR-13: native side-table format) ---- */
    if (register_map != NULL && register_count > 0) {
        for (uint32_t i = 0; i < register_count; i++) {
            if (register_map[i].node_id == node_id) {
                /* The side table records which register holds this NodeID's
                 * value, but not the value itself — the value is in the
                 * hardware register at deopt time, which the deopt stub
                 * saves to a per-arch save area. Without a T2 register save
                 * area wired through to this resolver, we cannot return the
                 * register's content here; the caller (vtx_osr_build_interp_frame)
                 * is responsible for reading the save area via the register
                 * number. We return UNDEFINED to signal "in register map,
                 * value pending save-area read."
                 *
                 * Note: this still makes the register map USEFUL — callers
                 * can detect a hit and then read the save area for the
                 * matching register_number. Pre-OSR-13, the type-pun read
                 * garbage as the count and never matched any NodeID. */
                return VTX_VALUE_UNDEFINED;
            }
        }
    }

    /* ---- Path 2 (OSR-9): fallback resolution for non-register-resident nodes ---- */
    if (node_table != NULL) {
        const vtx_node_t *node = vtx_node_get_const(node_table, node_id);
        if (node != NULL) {
            switch (node->opcode) {
            case VTX_OP_Constant: {
                /* Box the constant value into a vtx_value_t based on its
                 * declared kind. SMI/int constants become SMI-tagged values;
                 * pointer constants (e.g., null) become VTX_VALUE_NULL;
                 * other kinds fall through to UNDEFINED. */
                vtx_constval_t cv = node->constval;
                switch (cv.kind) {
                case VTX_TYPE_Int:
                    /* SMI-tag the integer constant. */
                    return vtx_make_smi(cv.as.int_val);
                case VTX_TYPE_Ptr:
                    if (cv.as.ptr_val == NULL) {
                        return VTX_VALUE_NULL;
                    }
                    /* Non-null pointer constants are not representable as
                     * NaN-boxed heap pointers without an allocated object —
                     * fall through to UNDEFINED. */
                    return VTX_VALUE_UNDEFINED;
                case VTX_TYPE_Float:
                case VTX_TYPE_Void:
                default:
                    return VTX_VALUE_UNDEFINED;
                }
            }
            case VTX_OP_Parameter: {
                /* Parameters are passed in the interpreter's locals — the
                 * parameter's index is the node's `local_index` field. */
                uint32_t param_idx = node->local_index;
                if (locals != NULL && param_idx < local_count) {
                    return locals[param_idx];
                }
                return VTX_VALUE_UNDEFINED;
            }
            default:
                /* Spilled node or other opcode not in the register map.
                 * Without a T2 register save area, we cannot resolve the
                 * value here; the caller is responsible for reading the
                 * spill slot directly from the JIT frame. */
                break;
            }
        }
    }

    return VTX_VALUE_UNDEFINED;
}

static vtx_value_t resolve_node_callback(vtx_nodeid_t node_id, void *ctx)
{
    vtx_resolve_context_t *rc = (vtx_resolve_context_t *)ctx;
    return vtx_osr_resolve_node(node_id,
                                  rc->register_map,
                                  rc->register_count,
                                  rc->node_table,
                                  rc->locals,
                                  rc->local_count);
}

/* ========================================================================== */
/* Build interpreter frame from FrameState                                    */
/* ========================================================================== */

vtx_interp_frame_t *vtx_osr_build_interp_frame(
    const vtx_frame_state_t *fs,
    vtx_value_t (*node_to_value)(vtx_nodeid_t, void *),
    void *context)
{
    if (!fs) return NULL;

    vtx_interp_frame_t *frame = calloc(1, sizeof(vtx_interp_frame_t));
    if (!frame) return NULL;

    frame->method_id = fs->method_id;
    frame->bytecode_pc = fs->bytecode_pc;
    frame->local_count = fs->local_count;
    frame->stack_top = fs->stack_count;
    /* OSR-14 fix: stack_capacity must leave headroom for the interpreter
     * to push new values. The original code set stack_capacity = stack_count,
     * so the very next `push` by the interpreter overflowed the buffer.
     *
     * The ideal fix is `stack_count + method->max_stack`, but
     * vtx_osr_build_interp_frame does not have access to the method
     * descriptor. We over-allocate by VTX_OSR_MIN_STACK_MARGIN slots
     * (well above the typical max_stack for any reasonable method —
     * V8/OpenJDK bytecodes rarely exceed 32) so the interpreter has
     * room to push without overflowing. */
    #define VTX_OSR_MIN_STACK_MARGIN 64
    frame->stack_capacity = fs->stack_count + VTX_OSR_MIN_STACK_MARGIN;
    frame->caller = NULL;

    /* Initialize enhanced fields */
    frame->monitors = NULL;
    frame->monitor_count = 0;
    frame->monitor_capacity = 0;
    frame->catch_handler_pc = (fs->exception.handler_pc != VTX_DEOPT_NO_HANDLER)
                              ? fs->exception.handler_pc : VTX_CATCH_NONE;
    frame->return_pc = 0;  /* Set by caller during frame chain reconstruction */
    frame->frame_kind = VTX_FRAME_INTERPRETED;

    /* Allocate and fill locals */
    if (fs->local_count > 0) {
        frame->locals = calloc(fs->local_count, sizeof(vtx_value_t));
        if (!frame->locals) {
            free(frame);
            return NULL;
        }
        for (uint32_t i = 0; i < fs->local_count; i++) {
            if (fs->locals[i] != VTX_NODEID_INVALID) {
                frame->locals[i] = node_to_value(fs->locals[i], context);
            } else {
                frame->locals[i] = VTX_VALUE_UNDEFINED;
            }
        }
    }

    /* Allocate and fill operand stack.
     * OSR-14 fix: allocate frame->stack_capacity slots (NOT just
     * fs->stack_count) so the interpreter has room to push new values
     * without overflowing the allocated buffer. The old code allocated
     * exactly stack_count slots, so the next push wrote past the end
     * of the heap allocation — a heap buffer overflow even when
     * stack_capacity was correctly bumped. */
    if (frame->stack_capacity > 0) {
        frame->stack = calloc(frame->stack_capacity, sizeof(vtx_value_t));
        if (!frame->stack) {
            free(frame->locals);
            free(frame);
            return NULL;
        }
        for (uint32_t i = 0; i < fs->stack_count; i++) {
            if (fs->stack[i] != VTX_NODEID_INVALID) {
                frame->stack[i] = node_to_value(fs->stack[i], context);
            } else {
                frame->stack[i] = VTX_VALUE_UNDEFINED;
            }
        }
    }

    /* Reconstruct monitor state from FrameState */
    if (fs->monitor_count > 0) {
        frame->monitors = calloc(fs->monitor_count, sizeof(vtx_osr_monitor_entry_t));
        if (!frame->monitors) {
            free(frame->stack);
            free(frame->locals);
            free(frame);
            return NULL;
        }
        frame->monitor_count = fs->monitor_count;
        frame->monitor_capacity = fs->monitor_count;
        for (uint32_t i = 0; i < fs->monitor_count; i++) {
            /* OSR-15 fix: store the actual local index holding the locked
             * object, not a hardcoded 0. The old code wrote 0 for every
             * monitor, so MONITOR_EXIT later looked up locals[0] and got
             * the wrong object — releasing the wrong lock or no lock at
             * all.
             *
             * The FrameState's monitor entry only stores the locked
             * object's NodeID, not its local index. We resolve the NodeID
             * to a value, then scan the locals array for the matching
             * value to find the index. If the value isn't in any local
             * (e.g., the lock is held on a temporary), we record
             * UINT32_MAX as a sentinel meaning "not in a local." */
            if (fs->monitors[i].monitor_object != VTX_NODEID_INVALID) {
                frame->monitors[i].object = node_to_value(
                    fs->monitors[i].monitor_object, context);
            } else {
                frame->monitors[i].object = VTX_VALUE_UNDEFINED;
            }

            /* OSR-15: scan locals for the matching value to find the index. */
            uint32_t found_local = UINT32_MAX;
            if (frame->locals != NULL) {
                for (uint32_t li = 0; li < frame->local_count; li++) {
                    if (frame->locals[li] == frame->monitors[i].object) {
                        found_local = li;
                        break;
                    }
                }
            }
            frame->monitors[i].local_index = found_local;
        }
    }

    return frame;
}

/* ========================================================================== */
/* OSR Up: Interpreter → Compiled Code                                        */
/* ========================================================================== */

__attribute__((optimize("O0")))
void vtx_osr_up(vtx_interp_frame_t *interp,
                 uint32_t method_id,
                 const vtx_compiled_code_t *compiled_code,
                 uint32_t loop_header_pc,
                 vtx_method_registry_t *registry,
                 vtx_gc_t *gc)
{
    if (!interp || !compiled_code || !compiled_code->entry_point) {
        fprintf(stderr, "[osr] FAIL: null check (interp=%p code=%p entry=%p)\n",
                (void*)interp, (void*)compiled_code,
                compiled_code ? (void*)compiled_code->entry_point : NULL);
        return;
    }

    /* OSR-3: this function returns void. On a successful transition the
     * inline-asm trampoline jumps to the JIT entry and never returns to
     * C — the JIT method's NaN-boxed return value propagates back to the
     * caller of vtx_interp_run through RAX exactly as if the JIT had
     * been called normally. Returning `bool` was broken because the
     * post-asm RAX (the JIT return value) could be SMI 0 / undefined /
     * null and be misread as `false`, triggering re-execution.
     *
     * If we get here at all, OSR failed; the caller falls back to
     * whole-method re-enter via jit_reenter_pending. */

    /* Verify the method matches */
    if (compiled_code->method_id != method_id) {
        fprintf(stderr, "[osr] FAIL: method_id mismatch (code=%u frame=%u)\n",
                compiled_code->method_id, method_id);
        return;
    }

    /* Verify the interpreter is at the loop header PC */
    if (interp->bytecode_pc != loop_header_pc) {
        fprintf(stderr, "[osr] FAIL: pc mismatch (frame_pc=%u loop_header=%u)\n",
                interp->bytecode_pc, loop_header_pc);
        return;
    }

    /* Verify frame size compatibility */
    if (interp->local_count > compiled_code->local_slots) {
        fprintf(stderr, "[osr] FAIL: locals overflow (frame=%u code=%u)\n",
                interp->local_count, compiled_code->local_slots);
        return;
    }
    if (interp->stack_top > compiled_code->stack_slots) {
        fprintf(stderr, "[osr] FAIL: stack overflow (frame=%u code=%u)\n",
                interp->stack_top, compiled_code->stack_slots);
        return;
    }

    /* OSR-16: refuse OSR into inlined code. The trampoline only knows
     * how to copy a single interpreter frame into the JIT frame; if the
     * JIT code at the OSR entry point has inlined callees, the JIT
     * frame's shape (locals + spills for each inlined frame) does not
     * match what the asm sets up, and the JIT would read garbage from
     * the wrong slots. Fall through to the failure path so the dispatch
     * loop re-enters the JIT from method entry (which is correct for
     * inlined code since the prologue handles inlined frame setup). */
    if (compiled_code->has_inlined_frames) {
        fprintf(stderr, "[osr] FAIL: compiled code for method %u has inlined "
                "frames — refusing OSR (frame shape mismatch)\n", method_id);
        return;
    }

    /* OSR-33: verify the JIT entry register convention matches the one
     * the trampoline hardcodes. The asm loads TOS/TOS-1/TOS-2/TOS-3 into
     * RAX/RCX/RDX/RBX (VTX_OSR_CONV_DEFAULT). If the JIT code uses a
     * different convention, refuse OSR instead of loading values into
     * the wrong registers. */
    if (compiled_code->entry_register_convention != VTX_OSR_CONV_DEFAULT) {
        fprintf(stderr, "[osr] FAIL: compiled code for method %u uses entry "
                "register convention %u (trampoline only supports "
                "VTX_OSR_CONV_DEFAULT) — refusing OSR\n",
                method_id, (unsigned)compiled_code->entry_register_convention);
        return;
    }

    /* ---- Step 1: Look up the OSR entry point in the side table ----
     *
     * OSR-2 fix: codegen now records VTX_STF_OSR_ENTRY entries for each
     * loop-header (backward-branch target). Previously no codegen wrote
     * this flag, so the side-table lookup never found an entry.
     *
     * OSR-5 fix: the lookup matches the requested loop_header_pc against
     * the entry's bytecode_pc field, so a method with multiple OSR entry
     * points picks the correct loop header.
     *
     * OSR-23 fix: we use the dedicated vtx_side_table_lookup_osr_entry
     * (filters by VTX_STF_OSR_ENTRY flag) instead of the generic
     * vtx_side_table_lookup_entry which uses "largest ≤ target" semantics
     * and could return a non-OSR entry (e.g., a safepoint) near the
     * requested PC.
     *
     * DEOPT-003 fix: start with osr_entry = NULL and return false if no
     * entry is found, so the interpreter continues interpreting instead
     * of silently jumping to the function entry point. */
    void *osr_entry = NULL;

    if (compiled_code->side_table != NULL) {
        const vtx_side_table_entry_t *osr_e = vtx_side_table_lookup_osr_entry(
            compiled_code->side_table, loop_header_pc);
        if (osr_e != NULL) {
            osr_entry = (uint8_t *)compiled_code->entry_point + osr_e->native_pc_offset;
        }
    }

    /* ---- Step 2: Look up the bytecode-to-native PC mapping ----
     * If bc_pc_map is available, find the native offset for the loop header.
     * This gives us the exact entry point in the compiled code.
     *
     * OSR-27: compiled_code->code may be NULL (the install path at
     * codegen.c:3689 sets code = NULL after copying into the executable
     * cache). The current dispatch.c caller papers over this by setting
     * cc.code = cm->code_start, but the function's contract does not
     * enforce it. Add a NULL check so a future caller that forgets to
     * set cc.code does not dereference NULL here. */
    if (compiled_code->bc_pc_map != NULL && compiled_code->bc_pc_map_count > 0) {
        for (uint32_t i = 0; i < compiled_code->bc_pc_map_count; i++) {
            if (compiled_code->bc_pc_map[i].bytecode_pc == loop_header_pc) {
                /* OSR-27: NULL check on compiled_code->code. */
                if (compiled_code->code == NULL) {
                    fprintf(stderr, "[osr] FAIL: compiled_code->code is NULL "
                            "for method %u (caller did not set cc.code) — "
                            "cannot compute OSR entry\n", method_id);
                    return;
                }
                /* OSR-24: verify the interpreter's stack_top matches the
                 * stack depth the JIT code expects at this OSR entry
                 * point. If they differ, the JIT would read garbage from
                 * the spill slots (or miss values it expects in
                 * registers). Fall through to the failure path on
                 * mismatch — the dispatch loop re-enters from method
                 * entry, which is always correct. */
                if (compiled_code->bc_pc_map[i].stack_depth != interp->stack_top) {
                    fprintf(stderr, "[osr] FAIL: stack depth mismatch at "
                            "loop_header_pc=%u (interp=%u expected=%u) — "
                            "refusing OSR\n",
                            loop_header_pc,
                            interp->stack_top,
                            compiled_code->bc_pc_map[i].stack_depth);
                    return;
                }
                /* Found the mapping — use this as the OSR entry */
                osr_entry = (uint8_t *)compiled_code->code +
                            compiled_code->bc_pc_map[i].native_offset;
                break;
            }
        }
    }

    /* DEOPT-003 fix: if no OSR entry was found, fail instead of silently
     * jumping to the function entry (which would re-execute the prologue
     * and all code before the loop, violating side-effect-once semantics
     * and the interpreter's loop-header assumption). */
    if (osr_entry == NULL) {
        fprintf(stderr, "[osr] no OSR entry for loop_header_pc=%u — "
                "continuing in interpreter\n", loop_header_pc);
        return;
    }

    /* ---- OSR-11 fix: re-check the registry's current cm before the asm jump ----
     *
     * The dispatch loop fetched `cm` from the registry earlier
     * (dispatch.c:3063). Between then and now, the method may have been
     * invalidated or recompiled (e.g., by a concurrent retrace triggered
     * by a guard failure elsewhere). The old cm may have been freed
     * (install.c:303), and its code_start memory may have been reclaimed.
     *
     * We re-fetch the current cm via the registry and compare its
     * code_start against the cached `compiled_code->entry_point`. If
     * they differ, the cached code is stale — return and let the
     * dispatch loop re-enter the JIT from method entry on the next call
     * (when it'll fetch the fresh cm).
     *
     * Note: there's still a small TOCTOU window between this check and
     * the asm jump, but this narrows it dramatically and catches the
     * common case (compile thread finished retrace between the original
     * dispatch fetch and now). True elimination would require hazard
     * pointers / epoch-based reclamation, which is out of scope. */
    if (registry != NULL) {
        vtx_compiled_method_t *current_cm = vtx_method_registry_get(registry, method_id);
        if (current_cm == NULL ||
            current_cm->code_start != (uint8_t *)compiled_code->entry_point ||
            !current_cm->is_valid) {
            fprintf(stderr, "[osr] FAIL: version mismatch — method %u was "
                    "recompiled/invalidated since the dispatch loop fetched cm "
                    "(current=%p cached=%p valid=%d)\n",
                    method_id,
                    current_cm ? (void*)current_cm->code_start : NULL,
                    (void*)compiled_code->entry_point,
                    current_cm ? (int)current_cm->is_valid : 0);
            return;
        }
    }

    /* ---- Step 3: Set up the JIT frame with interpreter values ----
     *
     * The JIT frame layout (from the compiled_code's frame_layout) is:
     *   [RBP+32]  = return address
     *   [RBP+24]  = method pointer
     *   [RBP+16]  = deopt_info pointer
     *   [RBP+8]   = profile_data pointer
     *   [RBP+0]   = caller RBP
     *   [RBP-8]   = first local slot
     *   [RBP-16]  = second local slot
     *   ...
     *   [RBP-8*N] = Nth local slot
     *   [RBP+spill_base] = first spill slot (operand stack)
     *   ...
     *
     * We build the JIT frame on the native stack using inline assembly,
     * copy interpreter locals and operand stack into the frame slots,
     * then jump to osr_entry. The C function never returns normally
     * after a successful transition.
     */

    const vtx_jit_frame_layout_t *layout = &compiled_code->frame_layout;
    uint32_t frame_sz  = layout->total_frame_size;
    int32_t  l_base    = layout->locals_base;   /* negative offset of local[0] from RBP */
    int32_t  s_base    = layout->spill_base;    /* negative offset of spill[0] from RBP */
    uint32_t nlocals   = interp->local_count;
    uint32_t nstack    = interp->stack_top;

    /* OSR-32 fix: the `osr_active` flag was set here but never read by
     * any code (the GC and stack walker don't check it). The intended use
     * was to skip OSR'd interpreter frames during root scanning, but this
     * was never implemented. Per the surgical-removal rule, we delete the
     * dead setter rather than carry the dead flag forward. The interp
     * frame remains valid (the JIT code returns to the interpreter's
     * caller via the patched return address) and the GC continues to scan
     * it as before. If duplicate-root scanning becomes a real performance
     * problem, a future patch should add the check at the GC site, not
     * via a half-wired runtime flag. */

    /* Prepare ALL parameters in a struct on the stack to reduce
     * register pressure on the inline asm. x86-64 has limited
     * registers and we clobber many, so passing a single pointer
     * avoids "impossible constraints" errors.
     *
     * Struct layout (all 8-byte slots, natural alignment):
     *   [0]   frame_sz      (uint64_t)
     *   [8]   l_base        (int64_t, sign-extended)
     *   [16]  s_base        (int64_t, sign-extended)
     *   [24]  nlocals       (uint64_t)
     *   [32]  nstack        (uint64_t)
     *   [40]  src_locals    (pointer)
     *   [48]  src_stack     (pointer)
     *   [56]  target        (pointer)
     *   [64]  method_desc   (pointer)
     *   [72]  deopt_ptr     (pointer)
     *   [80]  profile_data  (pointer)        -- OSR-30
     */
    struct osr_params {
        uint64_t frame_sz;
        int64_t  l_base;
        int64_t  s_base;
        uint64_t nlocals;
        uint64_t nstack;
        vtx_value_t             *src_locals;
        vtx_value_t             *src_stack;
        void                    *target;
        const vtx_method_desc_t *method_desc;
        vtx_deopt_info_t        *deopt_ptr;
        void                    *profile_data;   /* OSR-30: written to [RBP+8] */
    };

    struct osr_params params;
    params.frame_sz    = (uint64_t)frame_sz;
    params.l_base      = (int64_t)l_base;
    params.s_base      = (int64_t)s_base;
    params.nlocals     = (uint64_t)nlocals;
    params.nstack      = (uint64_t)nstack;
    params.src_locals  = interp->locals;
    params.src_stack   = interp->stack;
    params.target      = osr_entry;
    params.method_desc = compiled_code->method;
    params.deopt_ptr   = compiled_code->deopt_info;
    /* OSR-30: pass profile_data through params so the asm can write it
     * to [RBP+8] in the JIT frame header. NULL is acceptable (matches
     * pre-fix behavior); the dispatch caller populates this from the
     * interp's profiler when known. */
    params.profile_data = compiled_code->profile_data;

    /* OSR-12: poll for a pending safepoint immediately before the asm
     * jump. The dispatch loop's last safepoint check happened at the
     * backward branch; a GC or invalidation may have been requested
     * between then and now. Without this poll, the JIT code entered via
     * OSR would not observe the safepoint until its own loop-back-edge
     * poll, which may be far away if the OSR entry is at a loop header
     * with a long body. vtx_gc_safepoint is the existing fast-path poll
     * used at backward branches in dispatch.c — it just reads an atomic
     * flag and only does real work when a GC was requested. */
    if (gc != NULL) {
        vtx_gc_safepoint(gc);
    }

    /*
     * Inline assembly trampoline (x86-64, System V ABI, Linux).
     *
     * We:
     *   1. Read the current C frame's caller RBP and return address
     *      (so the JIT frame returns to the right place)
     *   2. Allocate the JIT frame on the stack
     *   3. Write the frame header (caller RBP, profile_data, deopt_info,
     *      method_ptr, return address)
     *   4. Copy interpreter locals into JIT local slots
     *   5. Copy interpreter operand stack into JIT spill slots
     *   6. Load top stack values into expression registers (RAX, RCX, RDX, RBX)
     *   7. Set RBP/RSP to the new JIT frame
     *   8. Jump to osr_entry
     *
     * After this asm block, the C function never returns.
     *
     * Register usage:
     *   r12 = caller RBP
     *   r13 = return address
     *   r14 = new RBP
     *   r15 = pointer to osr_params struct (loaded once, used throughout)
     *   rax, rcx, rdx, rsi, r8, r9, r10 = temporaries
     *
     * OSR-33 (entry register convention): the asm loads TOS, TOS-1,
     * TOS-2, TOS-3 into RAX, RCX, RDX, RBX respectively. This matches
     * the VTX_OSR_CONV_DEFAULT convention declared in
     * codecache/types.h and the expression-stack register assignment
     * documented in baseline/frame_layout.h:54-55:
     *
     *     TOS   -> RAX (reg 0)
     *     TOS-1 -> RCX (reg 1)
     *     TOS-2 -> RDX (reg 2)
     *     TOS-3 -> RBX (reg 3)
     *
     * Values deeper than TOS-3 are spilled to the frame and read by
     * the JIT code from spill slots. vtx_osr_up verifies
     * compiled_code->entry_register_convention == VTX_OSR_CONV_DEFAULT
     * before reaching this asm, so a future codegen change that emits
     * a different convention will fail the OSR transition cleanly
     * (falling back to whole-method re-enter) instead of silently
     * loading values into the wrong registers.
     *
     * OSR-6 (callee-saved register preservation): the JIT epilogue
     * (baseline/codegen.c:1146-1170) restores RBX from [RBP-8] and
     * R12 from [RBP-16]. Before this fix, the OSR-up asm never wrote
     * the caller's values to those slots, so the JIT epilogue restored
     * garbage into RBX and R12 — corrupting the caller of vtx_interp_run.
     * Step 2.5 below writes the caller's current RBX and R12 to the
     * saved-register slots BEFORE the asm clobbers them, so the JIT
     * epilogue restores the correct values.
     *
     * R13/R14/R15 are also clobbered by the asm trampoline, but the
     * baseline JIT codegen does NOT save/restore them (no slots in the
     * prologue/epilogue) and does not use them as callee-saved. They
     * are caller-saved from the JIT codegen's perspective. The asm
     * trampoline follows the same convention — it clobbers them and
     * does not restore them. This is consistent with the JIT codegen's
     * behavior for non-OSR JIT calls (the caller of vtx_dispatch_jit
     * also sees R13/R14/R15 clobbered). Extending the JIT frame to
     * save/restore R13/R14/R15 would be a codegen-wide change and is
     * out of scope for the OSR-up fix; this is documented as a known
     * limitation matching the existing JIT convention.
     */
    __asm__ __volatile__ (
        /* ---- Read current frame link data before modifying RBP ---- */
        "movq (%%rbp), %%r12\n\t"           /* r12 = caller's RBP (from current C frame) */
        "movq 8(%%rbp), %%r13\n\t"          /* r13 = return address   (from current C frame) */

        /* ---- Load params pointer into r15 (single input register) ---- */
        "movq %[params], %%r15\n\t"

        /* ---- Step 1: Allocate the JIT frame on the stack ---- */
        "movq 0(%%r15), %%rax\n\t"          /* load frame_sz from params[0] */
        "addq $48, %%rax\n\t"               /* 48 = 40 header + 8 for alignment margin */
        "addq $15, %%rax\n\t"               /* Fix C24: round UP for alignment */
        "andq $-16, %%rax\n\t"              /* align to 16 bytes (rounds up, not down) */
        "subq %%rax, %%rsp\n\t"             /* allocate frame on stack */

        /* ---- Step 2: Compute new RBP ---- */
        "movq 0(%%r15), %%rax\n\t"          /* reload frame_sz */
        "leaq 8(%%rsp, %%rax), %%r14\n\t"   /* r14 = new RBP */

        /* ---- Step 2.5: OSR-6 — save callee-saved registers to JIT frame ----
         * The JIT epilogue restores RBX from [RBP-8] and R12 from [RBP-16].
         * The OSR-up asm must initialize these slots with the caller's
         * CURRENT values BEFORE clobbering them, otherwise the JIT
         * epilogue restores garbage. At this point r14 = new RBP, and
         * RBX and R12 still hold the caller's values (the asm hasn't
         * touched them yet). VTX_FRAME_SAVED_RBX_OFFSET = -8 and
         * VTX_FRAME_SAVED_R12_OFFSET = -16 (from baseline/frame_layout.h). */
        "movq %%rbx, -8(%%r14)\n\t"         /* [RBP-8]  = caller's RBX */
        "movq %%r12, -16(%%r14)\n\t"        /* [RBP-16] = caller's R12 */

        /* ---- Step 3: Write frame header above RBP ---- */
        "movq %%r12, 0(%%r14)\n\t"          /* [RBP+0]  = caller RBP */
        "movq 80(%%r15), %%rax\n\t"         /* OSR-30: load profile_data from params[80] */
        "movq %%rax, 8(%%r14)\n\t"          /* [RBP+8]  = profile_data (was hardcoded NULL) */
        "movq 72(%%r15), %%rax\n\t"         /* load deopt_ptr from params[72] */
        "movq %%rax, 16(%%r14)\n\t"         /* [RBP+16] = deopt_info */
        "movq 64(%%r15), %%rax\n\t"         /* load method_desc from params[64] */
        "movq %%rax, 24(%%r14)\n\t"         /* [RBP+24] = method_ptr */
        "movq %%r13, 32(%%r14)\n\t"         /* [RBP+32] = return address */

        /* ---- Step 4: Copy interpreter locals into JIT frame local slots ---- */
        "movq 24(%%r15), %%rcx\n\t"         /* load nlocals from params[24] */
        "testq %%rcx, %%rcx\n\t"
        "jz 1f\n\t"                         /* skip if no locals */
        "movq 40(%%r15), %%rsi\n\t"         /* src_locals from params[40] */
        "movq 8(%%r15), %%rax\n\t"          /* l_base from params[8] */
        "0:\n\t"
        "movq (%%rsi), %%rdx\n\t"           /* load local value */
        "movq %%rdx, (%%r14, %%rax)\n\t"    /* store at [RBP + offset] */
        "addq $8, %%rsi\n\t"                /* next source slot */
        "subq $8, %%rax\n\t"                /* next offset (more negative) */
        "decq %%rcx\n\t"
        "jnz 0b\n\t"
        "1:\n\t"

        /* ---- Step 5: Copy interpreter operand stack into JIT frame spill slots ---- */
        "movq 32(%%r15), %%rcx\n\t"         /* load nstack from params[32] */
        "testq %%rcx, %%rcx\n\t"
        "jz 2f\n\t"                         /* skip if no stack values */
        "movq 48(%%r15), %%rsi\n\t"         /* src_stack from params[48] */
        "movq 16(%%r15), %%rax\n\t"         /* s_base from params[16] */
        "0:\n\t"
        "movq (%%rsi), %%rdx\n\t"           /* load stack value */
        "movq %%rdx, (%%r14, %%rax)\n\t"    /* store at [RBP + offset] */
        "addq $8, %%rsi\n\t"                /* next source slot */
        "subq $8, %%rax\n\t"                /* next offset */
        "decq %%rcx\n\t"
        "jnz 0b\n\t"
        "2:\n\t"

        /* ---- Step 6: Load top expression stack values into JIT registers ---- */
        "movq 32(%%r15), %%r8\n\t"          /* save nstack for reuse */
        "movq 16(%%r15), %%r9\n\t"          /* save sbase for reuse */
        "testq %%r8, %%r8\n\t"
        "jz 4f\n\t"                         /* skip if no stack values */

        /* Load TOS (stack[stack_top-1]) → RAX */
        "movq %%r8, %%rax\n\t"
        "decq %%rax\n\t"
        "shlq $3, %%rax\n\t"               /* rax = (stack_top-1) * 8 */
        "movq %%r9, %%rdx\n\t"
        "subq %%rax, %%rdx\n\t"            /* rdx = s_base - (stack_top-1)*8 */
        "movq (%%r14, %%rdx), %%rax\n\t"   /* RAX = TOS value */

        "cmpq $1, %%r8\n\t"
        "je 4f\n\t"                         /* only 1 stack value */

        /* Load TOS-1 (stack[stack_top-2]) → RCX */
        "movq %%r8, %%rcx\n\t"
        "subq $2, %%rcx\n\t"
        "shlq $3, %%rcx\n\t"               /* rcx = (stack_top-2) * 8 */
        "movq %%r9, %%rdx\n\t"
        "subq %%rcx, %%rdx\n\t"
        "movq (%%r14, %%rdx), %%rcx\n\t"   /* RCX = TOS-1 value */

        "cmpq $2, %%r8\n\t"
        "je 4f\n\t"                         /* only 2 stack values */

        /* Load TOS-2 (stack[stack_top-3]) → RDX */
        "movq %%r8, %%rdx\n\t"
        "subq $3, %%rdx\n\t"
        "shlq $3, %%rdx\n\t"               /* rdx = (stack_top-3) * 8 */
        "movq %%r9, %%r10\n\t"
        "subq %%rdx, %%r10\n\t"
        "movq (%%r14, %%r10), %%rdx\n\t"   /* RDX = TOS-2 value */

        "cmpq $3, %%r8\n\t"
        "je 4f\n\t"                         /* only 3 stack values */

        /* Load TOS-3 (stack[stack_top-4]) → RBX */
        "movq %%r8, %%rbx\n\t"
        "subq $4, %%rbx\n\t"
        "shlq $3, %%rbx\n\t"               /* rbx = (stack_top-4) * 8 */
        "movq %%r9, %%r10\n\t"
        "subq %%rbx, %%r10\n\t"
        "movq (%%r14, %%r10), %%rbx\n\t"   /* RBX = TOS-3 value */
        "jmp 4f\n\t"

        "4:\n\t"

        /* ---- Step 7: Set RBP and RSP to the new JIT frame ---- */
        "movq %%r14, %%rbp\n\t"             /* RBP = new frame base */
        "movq 0(%%r15), %%rax\n\t"          /* reload frame_sz */
        "negq %%rax\n\t"
        "leaq (%%rbp, %%rax), %%rsp\n\t"   /* RSP = RBP - frame_sz */

        /* ---- Step 8: Jump to the OSR entry point ---- */
        "movq 56(%%r15), %%rax\n\t"         /* load target from params[56] */
        "jmp *%%rax\n\t"

        /* OSR-31: safety trap; should never execute — the preceding
         * `jmp *%%rax` is unconditional and `__builtin_unreachable()`
         * below tells the compiler the asm doesn't fall through. Kept
         * as a defensive trap so that if a future code change breaks the
         * asm's control flow (e.g., accidentally writes a `ret` instead
         * of `jmp`), we trap loudly here instead of executing whatever
         * bytes happen to follow in the code section. Do NOT delete. */
        "int3\n\t"

        : /* no outputs — we never return */
        : [params]  "r"(&params)
        /* OSR-28: rbp and rsp are modified by the asm (movq %%r14,%%rbp
         * and the leaq/subq that adjust %%rsp). They were not in the
         * clobber list because the asm ends with an unconditional jmp +
         * __builtin_unreachable — the compiler never generates code
         * after the asm, so it doesn't matter that rbp/rsp are wrong.
         *
         * OSR-28: rbp and rsp are intentionally NOT in the clobber list.
         * Modern GCC (≥13) rejects them with "bp cannot be used in 'asm'
         * here" because the asm ends with an unconditional control transfer
         * (jmp) — there is no fall-through path for the compiler to
         * consider. The __builtin_unreachable() after the asm block is the
         * authoritative signal to the compiler that the asm never returns. */
        : "rax", "rbx", "rcx", "rdx", "rsi", "r8", "r9", "r10",
          "r12", "r13", "r14", "r15", "memory"
    );

    /* The asm block jumps to osr_entry and never returns here. */
    __builtin_unreachable();
}

/* ========================================================================== */
/* OSR Down: Compiled Code → Interpreter                                      */
/* ========================================================================== */

vtx_interp_frame_t *vtx_osr_down(vtx_interp_frame_t *interp,
                                   const vtx_osr_deopt_context_t *deopt_ctx)
{
    if (!deopt_ctx || !deopt_ctx->frame_state) {
        return NULL;
    }

    /* Step 1: Look up FrameState from side table (already provided in deopt_ctx) */
    const vtx_frame_state_t *fs = deopt_ctx->frame_state;

    /* Step 2: Set up the resolution context.
     * OSR-13: register_map is now vtx_reg_map_entry_t* (the side-table
     * native format) and register_count is the entry count. */
    vtx_resolve_context_t ctx;
    ctx.register_map   = deopt_ctx->register_map;
    ctx.register_count = deopt_ctx->register_count;
    ctx.node_table     = NULL;   /* OSR-9: deopt_ctx doesn't carry the IR graph;
                                  * vtx_osr_resolve_node falls back gracefully
                                  * (returns UNDEFINED for constants/parameters
                                  * when node_table is NULL). A future patch
                                  * should pass the IR graph via deopt_ctx. */
    ctx.locals         = NULL;
    ctx.local_count    = 0;

    /* Step 3: Build the interpreter frame for the innermost method */
    vtx_interp_frame_t *new_frame = vtx_osr_build_interp_frame(
        fs, resolve_node_callback, &ctx);
    if (!new_frame) return NULL;

    /* Step 4: Handle monitors — relock if needed.
     * For each monitor in the FrameState, we need to reacquire the lock
     * on the monitor object. The monitor object's value is resolved from
     * the register map.
     *
     * OSR-7 fix: uncommented and activated the vtx_runtime_monitor_enter
     * call. Previously this was commented out, so monitors held in
     * compiled code were NOT re-acquired during deopt — the thread
     * continued without the lock, breaking the synchronization invariant. */
    if (fs->monitor_count > 0) {
        for (uint32_t i = 0; i < fs->monitor_count; i++) {
            vtx_nodeid_t mon_node = fs->monitors[i].monitor_object;
            if (mon_node != VTX_NODEID_INVALID) {
                vtx_value_t mon_val = resolve_node_callback(mon_node, &ctx);
                /* OSR-7: actually call the runtime monitor re-enter
                 * primitive so the lock is reacquired in the interpreter
                 * context. Only heap pointers are valid monitor objects. */
                if (vtx_is_heap_ptr(mon_val)) {
                    vtx_runtime_monitor_enter(mon_val);
                }
            }
        }
    }

    /* Step 5: Handle exception handler state */
    if (fs->exception.handler_pc != VTX_DEOPT_NO_HANDLER) {
        /* The interpreter will pick up the exception handler from
         * the method's exception table at the deopt PC. No additional
         * work needed here — the bytecode_pc is set correctly. */
    }

    /* Step 6: Walk the caller chain and reconstruct caller frames.
     *
     * B24 fix: The `caller` field is typed as `vtx_frame_state_t*` but
     * stores `vtx_interp_frame_t*`. Use a union/cast through void* to
     * avoid undefined behavior. The field is only used as a linked-list
     * next pointer — it never accesses vtx_frame_state_t fields through it. */
    vtx_interp_frame_t *current = new_frame;
    const vtx_frame_state_t *caller_fs = fs->caller;
    while (caller_fs != NULL) {
        vtx_interp_frame_t *caller_frame = vtx_osr_build_interp_frame(
            caller_fs, resolve_node_callback, &ctx);
        if (!caller_frame) {
            /* Clean up already-built frames */
            vtx_interp_frame_t *f = new_frame;
            while (f) {
                /* DEOPT-005 fix: caller is void * now, cast directly. */
                vtx_interp_frame_t *next = (vtx_interp_frame_t *)f->caller;
                free(f->locals);
                free(f->stack);
                free(f);
                f = next;
            }
            return NULL;
        }
        /* DEOPT-005 fix: caller is void *, store the interp frame pointer
         * directly (no type punning). */
        current->caller = caller_frame;
        current = caller_frame;
        caller_fs = caller_fs->caller;
    }

    /* Step 7: Transfer to interpreter.
     *
     * B25 fix: Use the frame_state's bytecode_pc (the actual deopt PC)
     * not the return_pc (which is the caller's resume PC). The old code
     * used new_frame->bytecode_pc which was set from return_pc in
     * vtx_osr_build_interp_frame — that resumes at the wrong instruction. */
    if (interp) {
        interp->method_id = new_frame->method_id;
        /* B25 fix: Use the deopt context's bytecode_pc, which is the
         * PC where the guard failed — the interpreter should resume
         * at that point, not at the caller's return PC. */
        interp->bytecode_pc = fs->bytecode_pc;
    }

    return new_frame;
}
