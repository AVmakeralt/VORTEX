#include "deopt/osr.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ========================================================================== */
/* Internal: node resolution                                                  */
/* ========================================================================== */

/**
 * The register map is a sparse array indexed by NodeID. For nodes that
 * are not in the register map (e.g., constants, nodes that were spilled
 * to the stack), we need to look up the value differently.
 *
 * For OSR down, the register_map is laid out as:
 *   register_map[0] = number of entries
 *   register_map[1..2*N] = (node_id, value) pairs
 *
 * This allows for sparse mapping without allocating a huge array.
 */
typedef struct {
    vtx_nodeid_t node_id;
    vtx_value_t  value;
} vtx_node_value_pair_t;

vtx_value_t vtx_osr_resolve_node(vtx_nodeid_t node_id,
                                   const vtx_value_t *register_map,
                                   uint32_t map_size)
{
    if (!register_map || map_size == 0) return VTX_VALUE_UNDEFINED;

    /* The register_map is an array of vtx_node_value_pair_t entries,
     * preceded by a count. We search linearly for the matching NodeID.
     * In production this would use a hash map for O(1) lookup, but
     * the linear scan is correct and sufficient for correctness. */
    const vtx_node_value_pair_t *pairs =
        (const vtx_node_value_pair_t *)register_map;
    uint32_t pair_count = map_size / 2; /* each pair is 2 vtx_value_t wide */

    for (uint32_t i = 0; i < pair_count; i++) {
        if (pairs[i].node_id == node_id) {
            return pairs[i].value;
        }
    }

    return VTX_VALUE_UNDEFINED;
}

/* ========================================================================== */
/* Internal: resolve NodeID from FrameState using register map               */
/* ========================================================================== */

/**
 * Context for the node-to-value resolution callback.
 */
typedef struct {
    const vtx_value_t *register_map;
    uint32_t           register_map_size;
} vtx_resolve_context_t;

static vtx_value_t resolve_node_callback(vtx_nodeid_t node_id, void *ctx)
{
    vtx_resolve_context_t *rc = (vtx_resolve_context_t *)ctx;
    if (node_id == VTX_NODEID_INVALID) {
        return VTX_VALUE_UNDEFINED;
    }
    return vtx_osr_resolve_node(node_id, rc->register_map,
                                 rc->register_map_size);
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
    frame->stack_capacity = fs->stack_count;
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

    /* Allocate and fill operand stack */
    if (fs->stack_count > 0) {
        frame->stack = calloc(fs->stack_count, sizeof(vtx_value_t));
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
            frame->monitors[i].local_index = 0; /* will be resolved during deopt */
            if (fs->monitors[i].monitor_object != VTX_NODEID_INVALID) {
                frame->monitors[i].object = node_to_value(fs->monitors[i].monitor_object, context);
            } else {
                frame->monitors[i].object = VTX_VALUE_UNDEFINED;
            }
        }
    }

    return frame;
}

/* ========================================================================== */
/* OSR Up: Interpreter → Compiled Code                                        */
/* ========================================================================== */

__attribute__((optimize("O0")))
bool vtx_osr_up(vtx_interp_frame_t *interp,
                 uint32_t method_id,
                 const vtx_compiled_code_t *compiled_code,
                 uint32_t loop_header_pc)
{
    if (!interp || !compiled_code || !compiled_code->entry_point) {
        return false;
    }

    /* Verify the method matches */
    if (compiled_code->method_id != method_id) {
        return false;
    }

    /* Verify the interpreter is at the loop header PC */
    if (interp->bytecode_pc != loop_header_pc) {
        return false;
    }

    /* Verify frame size compatibility */
    if (interp->local_count > compiled_code->local_slots) {
        return false;
    }
    if (interp->stack_top > compiled_code->stack_slots) {
        return false;
    }

    /* ---- Step 1: Look up the OSR entry point in the side table ---- */
    void *osr_entry = compiled_code->entry_point;  /* default entry */

    if (compiled_code->side_table != NULL) {
        /* Search the side table for an OSR entry point at loop_header_pc.
         * The side table entries may have VTX_STF_OSR_ENTRY flag. */
        for (uint32_t i = 0; i < vtx_side_table_entry_count(compiled_code->side_table); i++) {
            const vtx_side_table_entry_t *entry = vtx_side_table_get_entry(
                compiled_code->side_table, i);
            if (entry && (entry->flags & VTX_STF_OSR_ENTRY)) {
                /* Found an OSR entry point. Compute the native code address
                 * from the code start + native_pc_offset. */
                osr_entry = (uint8_t *)compiled_code->entry_point + entry->native_pc_offset;
                break;
            }
        }
    }

    /* ---- Step 2: Look up the bytecode-to-native PC mapping ---- */
    /* If bc_pc_map is available, find the native offset for the loop header.
     * This gives us the exact entry point in the compiled code. */
    if (compiled_code->bc_pc_map != NULL && compiled_code->bc_pc_map_count > 0) {
        for (uint32_t i = 0; i < compiled_code->bc_pc_map_count; i++) {
            if (compiled_code->bc_pc_map[i].bytecode_pc == loop_header_pc) {
                /* Found the mapping — use this as the OSR entry */
                osr_entry = (uint8_t *)compiled_code->code +
                            compiled_code->bc_pc_map[i].native_offset;
                break;
            }
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

    /* Mark the interpreter frame as OSR'd so GC doesn't collect it.
     * The frame is now superseded by the JIT frame. */
    interp->osr_active = true;

    /* Prepare ALL parameters in a struct on the stack to reduce
     * register pressure on the inline asm. x86-64 has limited
     * registers and we clobber many, so passing a single pointer
     * avoids "impossible constraints" errors.
     *
     * Struct layout (all 8-byte slots, natural alignment):
     *   [0]  frame_sz     (uint64_t)
     *   [8]  l_base       (int64_t, sign-extended)
     *   [16] s_base       (int64_t, sign-extended)
     *   [24] nlocals      (uint64_t)
     *   [32] nstack       (uint64_t)
     *   [40] src_locals   (pointer)
     *   [48] src_stack    (pointer)
     *   [56] target       (pointer)
     *   [64] method_desc  (pointer)
     *   [72] deopt_ptr    (pointer)
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
        "andq $-16, %%rax\n\t"              /* align to 16 bytes */
        "subq %%rax, %%rsp\n\t"             /* allocate frame on stack */

        /* ---- Step 2: Compute new RBP ---- */
        "movq 0(%%r15), %%rax\n\t"          /* reload frame_sz */
        "leaq 8(%%rsp, %%rax), %%r14\n\t"   /* r14 = new RBP */

        /* ---- Step 3: Write frame header above RBP ---- */
        "movq %%r12, 0(%%r14)\n\t"          /* [RBP+0]  = caller RBP */
        "xorq %%rax, %%rax\n\t"
        "movq %%rax, 8(%%r14)\n\t"          /* [RBP+8]  = profile_data (NULL) */
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

        /* Should never reach here */
        "int3\n\t"

        : /* no outputs — we never return */
        : [params]  "r"(&params)
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

    /* Step 2: Set up the resolution context */
    vtx_resolve_context_t ctx;
    ctx.register_map = deopt_ctx->register_map;
    ctx.register_map_size = deopt_ctx->register_count;

    /* Step 3: Build the interpreter frame for the innermost method */
    vtx_interp_frame_t *new_frame = vtx_osr_build_interp_frame(
        fs, resolve_node_callback, &ctx);
    if (!new_frame) return NULL;

    /* Step 4: Handle monitors — relock if needed.
     * For each monitor in the FrameState, we need to reacquire the lock
     * on the monitor object. The monitor object's value is resolved from
     * the register map. */
    if (fs->monitor_count > 0) {
        for (uint32_t i = 0; i < fs->monitor_count; i++) {
            vtx_nodeid_t mon_node = fs->monitors[i].monitor_object;
            if (mon_node != VTX_NODEID_INVALID) {
                vtx_value_t mon_val = resolve_node_callback(mon_node, &ctx);
                /* In a real implementation, we would call the runtime
                 * to re-enter the monitor on the resolved object.
                 * The object must be a heap pointer. */
                if (vtx_is_heap_ptr(mon_val)) {
                    /* vtx_runtime_monitor_enter(vtx_heap_ptr(mon_val)); */
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

    /* Step 6: Walk the caller chain and reconstruct caller frames */
    vtx_interp_frame_t *current = new_frame;
    const vtx_frame_state_t *caller_fs = fs->caller;
    while (caller_fs != NULL) {
        vtx_interp_frame_t *caller_frame = vtx_osr_build_interp_frame(
            caller_fs, resolve_node_callback, &ctx);
        if (!caller_frame) {
            /* Clean up already-built frames */
            vtx_interp_frame_t *f = new_frame;
            while (f) {
                vtx_interp_frame_t *next = (vtx_interp_frame_t *)f->caller;
                free(f->locals);
                free(f->stack);
                free(f);
                f = next;
            }
            return NULL;
        }
        current->caller = (vtx_frame_state_t *)caller_frame;
        current = caller_frame;
        caller_fs = caller_fs->caller;
    }

    /* Step 7: Transfer to interpreter — in a real implementation,
     * we would set the interpreter's frame pointer and PC and
     * jump to the dispatch loop. Here we return the reconstructed
     * frame for the caller to resume. */
    if (interp) {
        interp->method_id = new_frame->method_id;
        interp->bytecode_pc = new_frame->bytecode_pc;
        /* The caller is responsible for copying locals/stack if needed */
    }

    return new_frame;
}
