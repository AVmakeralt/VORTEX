#include "deopt/stack_walk.h"
#include "interp/frame.h"
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* Reconstructed stack lifecycle                                              */
/* ========================================================================== */

vtx_reconstructed_stack_t *vtx_reconstructed_stack_create(void)
{
    vtx_reconstructed_stack_t *stack = calloc(1, sizeof(vtx_reconstructed_stack_t));
    if (!stack) return NULL;

    stack->frame_capacity = 16;
    stack->frames = calloc(stack->frame_capacity,
                            sizeof(vtx_reconstructed_frame_t));
    if (!stack->frames) {
        free(stack);
        return NULL;
    }
    stack->frame_count = 0;

    return stack;
}

void vtx_reconstructed_stack_destroy(vtx_reconstructed_stack_t *stack)
{
    if (!stack) return;

    for (uint32_t i = 0; i < stack->frame_count; i++) {
        free(stack->frames[i].locals);
        free(stack->frames[i].stack);
        free(stack->frames[i].compressed_snapshot);
    }
    free(stack->frames);
    free(stack);
}

int vtx_reconstructed_stack_add_frame(
    vtx_reconstructed_stack_t *stack,
    const vtx_reconstructed_frame_t *frame)
{
    if (!stack || !frame) return -1;

    if (stack->frame_count >= stack->frame_capacity) {
        uint32_t new_cap = stack->frame_capacity * 2;
        vtx_reconstructed_frame_t *new_frames = realloc(
            stack->frames,
            (size_t)new_cap * sizeof(vtx_reconstructed_frame_t));
        if (!new_frames) return -1;
        stack->frames = new_frames;
        stack->frame_capacity = new_cap;
    }

    vtx_reconstructed_frame_t *slot = &stack->frames[stack->frame_count];

    /* Deep copy the frame */
    slot->method_id = frame->method_id;
    slot->bytecode_pc = frame->bytecode_pc;
    slot->local_count = frame->local_count;
    slot->stack_count = frame->stack_count;
    slot->original_kind = frame->original_kind;
    slot->is_inlined = frame->is_inlined;
    slot->is_materialized = frame->is_materialized;
    slot->snapshot_size = frame->snapshot_size;

    if (frame->compressed_snapshot != NULL && frame->snapshot_size > 0) {
        slot->compressed_snapshot = (uint8_t *)malloc(frame->snapshot_size);
        if (!slot->compressed_snapshot) return -1;
        memcpy(slot->compressed_snapshot, frame->compressed_snapshot, frame->snapshot_size);
    } else {
        slot->compressed_snapshot = NULL;
    }

    if (frame->local_count > 0) {
        slot->locals = calloc(frame->local_count, sizeof(vtx_value_t));
        if (!slot->locals) return -1;
        memcpy(slot->locals, frame->locals,
               (size_t)frame->local_count * sizeof(vtx_value_t));
    } else {
        slot->locals = NULL;
    }

    if (frame->stack_count > 0) {
        slot->stack = calloc(frame->stack_count, sizeof(vtx_value_t));
        if (!slot->stack) {
            free(slot->locals);
            return -1;
        }
        memcpy(slot->stack, frame->stack,
               (size_t)frame->stack_count * sizeof(vtx_value_t));
    } else {
        slot->stack = NULL;
    }

    stack->frame_count++;
    return 0;
}

/* ========================================================================== */
/* Side table registry                                                        */
/* ========================================================================== */

int vtx_side_table_registry_init(vtx_side_table_registry_t *registry)
{
    if (!registry) return -1;

    registry->descriptor_capacity = 32;
    registry->descriptors = calloc(registry->descriptor_capacity,
                                    sizeof(vtx_code_descriptor_t));
    if (!registry->descriptors) return -1;

    registry->descriptor_count = 0;
    return 0;
}

void vtx_side_table_registry_destroy(vtx_side_table_registry_t *registry)
{
    if (!registry) return;
    free(registry->descriptors);
    memset(registry, 0, sizeof(*registry));
}

int vtx_side_table_registry_add(vtx_side_table_registry_t *registry,
                                 void *code_start,
                                 uint32_t code_size,
                                 vtx_side_table_t *side_table)
{
    if (!registry || !code_start || !side_table) return -1;

    if (registry->descriptor_count >= registry->descriptor_capacity) {
        uint32_t new_cap = registry->descriptor_capacity * 2;
        vtx_code_descriptor_t *new_desc = realloc(
            registry->descriptors,
            (size_t)new_cap * sizeof(vtx_code_descriptor_t));
        if (!new_desc) return -1;
        registry->descriptors = new_desc;
        registry->descriptor_capacity = new_cap;
    }

    vtx_code_descriptor_t *desc = &registry->descriptors[registry->descriptor_count++];
    desc->code_start = code_start;
    desc->code_size = code_size;
    desc->side_table = side_table;

    return 0;
}

vtx_side_table_t *vtx_side_table_registry_lookup(
    const vtx_side_table_registry_t *registry,
    const void *code_addr)
{
    if (!registry || !code_addr) return NULL;

    const uint8_t *addr = (const uint8_t *)code_addr;

    for (uint32_t i = 0; i < registry->descriptor_count; i++) {
        const vtx_code_descriptor_t *desc = &registry->descriptors[i];
        const uint8_t *start = (const uint8_t *)desc->code_start;
        if (addr >= start && addr < start + desc->code_size) {
            return desc->side_table;
        }
    }

    return NULL;
}

/* ========================================================================== */
/* Frame pointer chain walking                                                */
/* ========================================================================== */

/**
 * Read a pointer from the target process memory at the given address.
 * In a real implementation, this would handle cross-process debugging,
 * signal safety, and stack bounds checking. For an in-process implementation,
 * we do a direct read with basic validity checks.
 */
static bool read_ptr(void *fp, size_t offset, void **out)
{
    if (!fp) return false;

    /* On x86-64, user-space stack addresses are in the range
     * 0x7FFF_FFFF_FFFF downward. We do a basic sanity check. */
    uintptr_t addr = (uintptr_t)fp;
    if (addr < 0x1000 || addr > 0x7FFFFFFFFFFFull) {
        return false;
    }

    void **slot = (void **)((uint8_t *)fp + offset);
    *out = *slot;
    return true;
}

int vtx_stack_walk_read_caller_fp(void *fp, void **out_caller_fp)
{
    if (!out_caller_fp) return -1;
    if (!read_ptr(fp, 0, out_caller_fp)) return -1;
    return 0;
}

int vtx_stack_walk_read_return_addr(void *fp, void **out_return_addr)
{
    if (!out_return_addr) return -1;
    if (!read_ptr(fp, sizeof(void *), out_return_addr)) return -1;
    return 0;
}

vtx_frame_kind_t vtx_stack_walk_classify_frame(
    const vtx_side_table_registry_t *registry,
    void *fp)
{
    if (!fp) return VTX_FRAME_NATIVE;

    void *return_addr;
    if (vtx_stack_walk_read_return_addr(fp, &return_addr) != 0) {
        return VTX_FRAME_NATIVE;
    }

    /* Check if the return address falls within a known compiled code range */
    if (registry) {
        vtx_side_table_t *st = vtx_side_table_registry_lookup(registry,
                                                                return_addr);
        if (st) {
            return VTX_FRAME_COMPILED;
        }
    }

    /* Heuristic: if the return address is in the low address range,
     * it's likely an interpreter dispatch address. In a real implementation,
     * we would check against the interpreter's code range. */
    return VTX_FRAME_NATIVE;
}

/* ========================================================================== */
/* Internal: reconstruct frames from a FrameState chain                      */
/* ========================================================================== */

/**
 * Reconstruct interpreter frames from a FrameState chain.
 * A single compiled frame may produce multiple logical frames if methods
 * were inlined: the FrameState chain encodes the inlined call stack.
 *
 * @param fs          The innermost FrameState
 * @param node_to_val Function to resolve NodeID → vtx_value_t
 * @param ctx         Opaque context for node_to_val
 * @param stack       The reconstructed stack to add frames to
 * @return 0 on success, -1 on failure
 */
static int reconstruct_from_frame_state(
    const vtx_frame_state_t *fs,
    vtx_value_t (*node_to_val)(vtx_nodeid_t, void *),
    void *ctx,
    vtx_reconstructed_stack_t *stack)
{
    for (const vtx_frame_state_t *cur = fs; cur != NULL; cur = cur->caller) {
        vtx_reconstructed_frame_t frame;
        memset(&frame, 0, sizeof(frame));

        frame.method_id = cur->method_id;
        frame.bytecode_pc = cur->bytecode_pc;
        frame.local_count = cur->local_count;
        frame.stack_count = cur->stack_count;
        frame.original_kind = VTX_FRAME_COMPILED;
        frame.is_inlined = (cur != fs && cur->caller != NULL);

        /* Resolve locals */
        if (cur->local_count > 0) {
            frame.locals = calloc(cur->local_count, sizeof(vtx_value_t));
            if (!frame.locals) return -1;
            for (uint32_t i = 0; i < cur->local_count; i++) {
                if (cur->locals[i] != VTX_NODEID_INVALID) {
                    frame.locals[i] = node_to_val(cur->locals[i], ctx);
                } else {
                    frame.locals[i] = VTX_VALUE_UNDEFINED;
                }
            }
        }

        /* Resolve operand stack */
        if (cur->stack_count > 0) {
            frame.stack = calloc(cur->stack_count, sizeof(vtx_value_t));
            if (!frame.stack) {
                free(frame.locals);
                return -1;
            }
            for (uint32_t i = 0; i < cur->stack_count; i++) {
                if (cur->stack[i] != VTX_NODEID_INVALID) {
                    frame.stack[i] = node_to_val(cur->stack[i], ctx);
                } else {
                    frame.stack[i] = VTX_VALUE_UNDEFINED;
                }
            }
        }

        if (vtx_reconstructed_stack_add_frame(stack, &frame) != 0) {
            free(frame.locals);
            free(frame.stack);
            return -1;
        }

        free(frame.locals);
        free(frame.stack);
    }

    return 0;
}

/* ========================================================================== */
/* Main stack walking function                                                */
/* ========================================================================== */

/**
 * Default node-to-value resolution for stack walking.
 * Resolves Constant nodes directly from the node table.
 * For other node kinds, returns VTX_VALUE_UNDEFINED since we don't
 * have access to actual register/stack maps at this level — the OSR
 * module handles full resolution at deopt time.
 */
static vtx_value_t default_node_to_value(vtx_nodeid_t node_id, void *ctx)
{
    if (node_id == VTX_NODEID_INVALID) {
        return VTX_VALUE_UNDEFINED;
    }

    /* Try to resolve constant values directly */
    vtx_node_table_t *table = (vtx_node_table_t *)ctx;
    if (table) {
        const vtx_node_t *node = vtx_node_get_const(table, node_id);
        if (node && node->opcode == VTX_OP_Constant) {
            switch (node->constval.kind) {
            case VTX_TYPE_Int:
                return vtx_make_smi(node->constval.as.int_val);
            case VTX_TYPE_Float:
                return vtx_make_double(node->constval.as.float_val);
            case VTX_TYPE_Ptr:
                if (node->constval.as.ptr_val == NULL) {
                    return VTX_VALUE_NULL;
                }
                return vtx_make_heap_ptr(node->constval.as.ptr_val);
            default:
                break;
            }
        }
    }

    return VTX_VALUE_UNDEFINED;
}

vtx_reconstructed_stack_t *vtx_stack_walk(
    const vtx_stack_walk_config_t *config)
{
    if (!config || !config->start_fp) return NULL;

    vtx_reconstructed_stack_t *stack = vtx_reconstructed_stack_create();
    if (!stack) return NULL;

    void *current_fp = config->start_fp;
    uint32_t depth = 0;
    uint32_t max_depth = config->max_depth;
    if (max_depth == 0) max_depth = 1024; /* reasonable default */

    while (current_fp != NULL && depth < max_depth) {
        vtx_frame_kind_t kind = vtx_stack_walk_classify_frame(
            config->registry, current_fp);

        switch (kind) {
        case VTX_FRAME_COMPILED: {
            /* Read return address to determine native PC */
            void *return_addr;
            if (vtx_stack_walk_read_return_addr(current_fp, &return_addr) != 0) {
                goto done;
            }

            /* Look up side table for this compiled code */
            vtx_side_table_t *side_table =
                vtx_side_table_registry_lookup(config->registry, return_addr);
            if (!side_table) {
                /* Unknown compiled code — treat as native frame */
                goto next_frame;
            }

            /* Compute native PC offset within the compiled code.
             * We need to find the code_start for this side table. */
            uint32_t native_pc_offset = 0;
            for (uint32_t i = 0; i < config->registry->descriptor_count; i++) {
                const vtx_code_descriptor_t *desc =
                    &config->registry->descriptors[i];
                if (desc->side_table == side_table) {
                    uintptr_t offset = (uintptr_t)return_addr -
                                       (uintptr_t)desc->code_start;
                    native_pc_offset = (uint32_t)offset;
                    break;
                }
            }

            /* Look up FrameState from side table */
            uint32_t fs_index = vtx_side_table_lookup(side_table,
                                                        native_pc_offset);
            if (fs_index == UINT32_MAX) {
                /* No FrameState at this PC — skip */
                goto next_frame;
            }

            vtx_frame_state_t *fs = vtx_side_table_get_frame_state(
                side_table, fs_index);
            if (!fs) goto next_frame;

            /* Reconstruct interpreter frames from the FrameState chain.
             * A compiled frame may contain inlined methods, producing
             * multiple logical frames. */
            if (reconstruct_from_frame_state(fs, default_node_to_value,
                                              (void *)config->node_table, stack) != 0) {
                vtx_reconstructed_stack_destroy(stack);
                return NULL;
            }
            break;
        }

        case VTX_FRAME_INTERPRETED: {
            /* For interpreter frames, we read the state directly.
             * The interpreter frame layout is defined in src/interp/frame.h.
             *
             * Layout of vtx_frame_t on the stack:
             *   The frame is a struct, so we read it via the frame pointer.
             *   Key fields:
             *     frame->method->id         → method_id
             *     frame->bytecode->pc_offset + frame->return_pc → PC
             *     frame->locals[]           → local variable values
             *     frame->locals_count       → number of locals
             *     frame->operand_stack[]    → operand stack values
             *     frame->stack_top          → current stack depth
             *
             * Since vtx_frame_t is an in-memory struct (not a stack-allocated
             * frame in the traditional sense), we can read it directly from
             * the frame pointer if the pointer is valid. */
            vtx_reconstructed_frame_t frame;
            memset(&frame, 0, sizeof(frame));
            frame.original_kind = VTX_FRAME_INTERPRETED;
            frame.is_inlined = false;

            /* Try to read the interpreter frame from the frame pointer.
             * The current_fp points to an interpreter frame (vtx_frame_t *).
             * We read the fields through pointer arithmetic. */
            const vtx_frame_t *interp_frame = (const vtx_frame_t *)current_fp;

            /* Validate the frame pointer before reading */
            uintptr_t fp_addr = (uintptr_t)current_fp;
            if (fp_addr >= 0x1000 && fp_addr <= 0x7FFFFFFFFFFFull) {
                /* Read method_id from the frame's method descriptor */
                if (interp_frame->method != NULL) {
                    frame.method_id = 0; /* method_desc has no id field */
                }

                /* Read the bytecode PC.
                 * The interpreter's current PC is tracked by the dispatch
                 * loop, not stored in the frame struct itself. The return_pc
                 * is the PC to resume in the *caller* after return, not the
                 * current method's PC. For a reconstructed frame, we use
                 * return_pc as an approximation — the actual PC is available
                 * only from the dispatch loop state. */
                frame.bytecode_pc = interp_frame->return_pc;

                /* Read local variables */
                frame.local_count = interp_frame->locals_count;
                if (frame.local_count > 0 && interp_frame->locals != NULL) {
                    frame.locals = calloc(frame.local_count, sizeof(vtx_value_t));
                    if (frame.locals) {
                        uint32_t to_copy = frame.local_count;
                        /* Safety: don't copy more than VTX_FRAME_MAX_LOCALS */
                        if (to_copy > VTX_FRAME_MAX_LOCALS) to_copy = VTX_FRAME_MAX_LOCALS;
                        memcpy(frame.locals, interp_frame->locals,
                               to_copy * sizeof(vtx_value_t));
                    }
                }

                /* Read operand stack */
                frame.stack_count = (uint32_t)(interp_frame->stack_top > 0 ?
                                                interp_frame->stack_top : 0);
                if (frame.stack_count > 0 && interp_frame->operand_stack != NULL) {
                    frame.stack = calloc(frame.stack_count, sizeof(vtx_value_t));
                    if (frame.stack) {
                        uint32_t to_copy = frame.stack_count;
                        if (to_copy > VTX_FRAME_MAX_STACK_DEPTH)
                            to_copy = VTX_FRAME_MAX_STACK_DEPTH;
                        memcpy(frame.stack, interp_frame->operand_stack,
                               to_copy * sizeof(vtx_value_t));
                    }
                }
            }

            if (vtx_reconstructed_stack_add_frame(stack, &frame) != 0) {
                free(frame.locals);
                free(frame.stack);
                vtx_reconstructed_stack_destroy(stack);
                return NULL;
            }
            break;
        }

        case VTX_FRAME_DEOPTLESS: {
            /* Deoptless continuation frames are handled similarly to
             * compiled frames, but use the continuation's side table. */
            void *return_addr;
            if (vtx_stack_walk_read_return_addr(current_fp, &return_addr) != 0) {
                goto done;
            }

            vtx_side_table_t *side_table =
                vtx_side_table_registry_lookup(config->registry, return_addr);
            if (!side_table) goto next_frame;

            uint32_t native_pc_offset = 0;
            for (uint32_t i = 0; i < config->registry->descriptor_count; i++) {
                const vtx_code_descriptor_t *desc =
                    &config->registry->descriptors[i];
                if (desc->side_table == side_table) {
                    uintptr_t offset = (uintptr_t)return_addr -
                                       (uintptr_t)desc->code_start;
                    native_pc_offset = (uint32_t)offset;
                    break;
                }
            }

            uint32_t fs_index = vtx_side_table_lookup(side_table,
                                                        native_pc_offset);
            if (fs_index == UINT32_MAX) goto next_frame;

            vtx_frame_state_t *fs = vtx_side_table_get_frame_state(
                side_table, fs_index);
            if (!fs) goto next_frame;

            if (reconstruct_from_frame_state(fs, default_node_to_value,
                                              (void *)config->node_table, stack) != 0) {
                vtx_reconstructed_stack_destroy(stack);
                return NULL;
            }
            break;
        }

        case VTX_FRAME_NATIVE:
        case VTX_FRAME_STUB:
            /* Cannot walk through native or stub frames.
             * Stop the walk here. */
            goto done;
        }

    next_frame:
        depth++;

        /* Follow frame pointer chain to caller */
        void *caller_fp;
        if (vtx_stack_walk_read_caller_fp(current_fp, &caller_fp) != 0) {
            break;
        }

        /* Sanity check: frame pointers should decrease (stack grows down) */
        if (caller_fp != NULL && (uintptr_t)caller_fp <= (uintptr_t)current_fp) {
            break; /* corrupted frame pointer */
        }

        current_fp = caller_fp;
    }

done:
    return stack;
}

/* ========================================================================== */
/* Lazy frame materialization (Proposal #8)                                      */
/* ========================================================================== */

/**
 * RLE entry: a value followed by a count of consecutive occurrences.
 * For vtx_value_t (which is uint64_t / NaN-boxed), we store each value
 * and its repetition count as 8 + 4 = 12 bytes.
 */
#define VTX_RLE_VALUE_SIZE 8   /* sizeof(vtx_value_t) */
#define VTX_RLE_COUNT_SIZE 4   /* uint32_t for count */
#define VTX_RLE_ENTRY_SIZE (VTX_RLE_VALUE_SIZE + VTX_RLE_COUNT_SIZE)

uint8_t *vtx_frame_compress(const vtx_reconstructed_frame_t *frame)
{
    if (!frame) return NULL;
    if (!frame->locals && !frame->stack) return NULL;

    /* Worst-case size: every value is unique, so we need an entry per value.
     * Total values = local_count + stack_count.
     * Each entry = VTX_RLE_ENTRY_SIZE bytes. */
    uint32_t total_values = frame->local_count + frame->stack_count;
    if (total_values == 0) return NULL;

    uint32_t max_size = total_values * VTX_RLE_ENTRY_SIZE + 8; /* 8 for header */
    uint8_t *buf = (uint8_t *)malloc(max_size);
    if (!buf) return NULL;

    uint32_t pos = 0;

    /* Header: local_count (4 bytes) + stack_count (4 bytes) */
    memcpy(buf + pos, &frame->local_count, 4); pos += 4;
    memcpy(buf + pos, &frame->stack_count, 4); pos += 4;

    /* Compress locals + stack as a single RLE stream */
    vtx_value_t *all = (vtx_value_t *)malloc(total_values * sizeof(vtx_value_t));
    if (!all) { free(buf); return NULL; }

    uint32_t idx = 0;
    for (uint32_t i = 0; i < frame->local_count; i++) {
        all[idx++] = frame->locals[i];
    }
    for (uint32_t i = 0; i < frame->stack_count; i++) {
        all[idx++] = frame->stack[i];
    }

    /* RLE encode */
    uint32_t i = 0;
    while (i < total_values) {
        vtx_value_t current = all[i];
        uint32_t count = 1;
        while (i + count < total_values && all[i + count] == current) {
            count++;
        }

        /* Write value (8 bytes) + count (4 bytes) */
        memcpy(buf + pos, &current, VTX_RLE_VALUE_SIZE); pos += VTX_RLE_VALUE_SIZE;
        memcpy(buf + pos, &count, VTX_RLE_COUNT_SIZE); pos += VTX_RLE_COUNT_SIZE;

        i += count;
    }

    free(all);

    /* Shrink to actual size */
    uint8_t *result = (uint8_t *)realloc(buf, pos);
    return result ? result : buf;  /* if realloc fails, use original */
}

int vtx_frame_decompress(vtx_reconstructed_frame_t *frame)
{
    if (!frame || !frame->compressed_snapshot || frame->snapshot_size == 0) return -1;

    uint8_t *buf = frame->compressed_snapshot;
    uint32_t pos = 0;

    /* Read header */
    uint32_t local_count = 0, stack_count = 0;
    memcpy(&local_count, buf + pos, 4); pos += 4;
    memcpy(&stack_count, buf + pos, 4); pos += 4;

    /* Allocate locals and stack */
    if (local_count > 0) {
        frame->locals = (vtx_value_t *)malloc(local_count * sizeof(vtx_value_t));
        if (!frame->locals) return -1;
    }
    frame->local_count = local_count;

    if (stack_count > 0) {
        frame->stack = (vtx_value_t *)malloc(stack_count * sizeof(vtx_value_t));
        if (!frame->stack) {
            free(frame->locals);
            frame->locals = NULL;
            return -1;
        }
    }
    frame->stack_count = stack_count;

    /* Decode RLE */
    uint32_t out_idx = 0;
    vtx_value_t *all = (vtx_value_t *)malloc(
        (local_count + stack_count) * sizeof(vtx_value_t));
    if (!all) {
        free(frame->locals); frame->locals = NULL;
        free(frame->stack); frame->stack = NULL;
        return -1;
    }

    while (pos + VTX_RLE_ENTRY_SIZE <= frame->snapshot_size && out_idx < local_count + stack_count) {
        vtx_value_t value;
        uint32_t count;
        memcpy(&value, buf + pos, VTX_RLE_VALUE_SIZE); pos += VTX_RLE_VALUE_SIZE;
        memcpy(&count, buf + pos, VTX_RLE_COUNT_SIZE); pos += VTX_RLE_COUNT_SIZE;

        for (uint32_t j = 0; j < count && out_idx < local_count + stack_count; j++) {
            all[out_idx++] = value;
        }
    }

    /* Split into locals and stack */
    memcpy(frame->locals, all, local_count * sizeof(vtx_value_t));
    memcpy(frame->stack, all + local_count, stack_count * sizeof(vtx_value_t));
    free(all);

    frame->is_materialized = true;
    return 0;
}

int vtx_frame_materialize_on_demand(vtx_reconstructed_frame_t *frame)
{
    if (!frame) return -1;

    if (frame->is_materialized) return 0; /* already done */

    if (frame->compressed_snapshot != NULL && frame->snapshot_size > 0) {
        return vtx_frame_decompress(frame);
    }

    /* No data available — can't materialize */
    return -1;
}
