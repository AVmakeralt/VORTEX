#ifndef VORTEX_DEOPT_STACK_WALK_H
#define VORTEX_DEOPT_STACK_WALK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vortex_config.h"
#include "ir/node.h"
#include "deopt/frame_state.h"
#include "deopt/side_table.h"

/**
 * VORTEX Stack Walking
 *
 * Walks the compiled frame stack to reconstruct the interpreter call stack.
 * This is needed at deoptimization time to convert all compiled frames back
 * to interpreter frames.
 *
 * The stack walker handles:
 *   - Compiled frames: follow frame pointer chain, look up side table
 *   - Interpreter frames: read locals/stack from the interpreter frame layout
 *   - Mixed frames: a stack with both interpreter and compiled frames
 *   - Inlined methods: a single physical compiled frame may contain multiple
 *     logical frames (the inlined callee's frame state is chained off the
 *     outer frame's frame state)
 *   - Deoptless continuations: frames that have been patched to continuation
 *     versions — the side table for the continuation version is used
 *
 * Frame pointer chain (x86-64 System V ABI):
 *   [frame_pointer + 0]  = saved RBP (caller's frame pointer)
 *   [frame_pointer + 8]  = return address
 *   [frame_pointer - 8]  = method pointer (or code pointer)
 *   [frame_pointer - 16] = deopt info pointer / side table pointer
 */

/* ========================================================================== */
/* Frame kind                                                                 */
/* ========================================================================== */

/**
 * The kind of a frame encountered during stack walking.
 */
typedef enum {
    VTX_FRAME_INTERPRETED,  /* interpreter frame */
    VTX_FRAME_COMPILED,     /* JIT-compiled frame (T1/T2/T3) */
    VTX_FRAME_NATIVE,       /* native C/C++ frame (not walkable) */
    VTX_FRAME_DEOPTLESS,    /* frame running a deoptless continuation */
    VTX_FRAME_STUB          /* runtime stub frame (deopt handler, etc.) */
} vtx_frame_kind_t;

/* ========================================================================== */
/* Reconstructed interpreter frame                                            */
/* ========================================================================== */

/**
 * A single reconstructed interpreter frame. This is the output of stack
 * walking: for each physical or logical frame on the compiled stack,
 * we produce one of these with the interpreter-visible state.
 */
typedef struct {
    uint32_t         method_id;      /* method being executed */
    uint32_t         bytecode_pc;    /* bytecode PC to resume at */
    vtx_value_t     *locals;         /* local variable values (malloc'd) */
    uint32_t         local_count;    /* number of locals */
    vtx_value_t     *stack;          /* operand stack values (malloc'd) */
    uint32_t         stack_count;    /* number of stack values */
    vtx_frame_kind_t original_kind;  /* what kind of frame this was compiled from */
    bool             is_inlined;     /* true if this is a logical frame from inlining */
} vtx_reconstructed_frame_t;

/* ========================================================================== */
/* Reconstructed stack                                                        */
/* ========================================================================== */

/**
 * The result of stack walking: an array of reconstructed interpreter frames.
 */
typedef struct {
    vtx_reconstructed_frame_t *frames;       /* array of frames (callee first) */
    uint32_t                   frame_count;
    uint32_t                   frame_capacity;
} vtx_reconstructed_stack_t;

/* ========================================================================== */
/* Side table registry                                                        */
/* ========================================================================== */

/**
 * Maps a code entry point to its side table.
 * Used during stack walking to look up the side table for each compiled frame.
 */
typedef struct {
    void              *code_start;     /* start address of compiled code */
    uint32_t           code_size;      /* size of compiled code in bytes */
    vtx_side_table_t  *side_table;     /* side table for this code */
} vtx_code_descriptor_t;

/**
 * Registry of all compiled code descriptors, used for side table lookup.
 */
typedef struct {
    vtx_code_descriptor_t *descriptors;
    uint32_t               descriptor_count;
    uint32_t               descriptor_capacity;
} vtx_side_table_registry_t;

/* ========================================================================== */
/* Stack walking configuration                                                */
/* ========================================================================== */

/**
 * Configuration for a stack walk operation.
 */
typedef struct {
    void                       *start_fp;         /* starting frame pointer */
    vtx_side_table_registry_t  *registry;         /* side table registry */
    vtx_frame_state_t         **frame_states;      /* global FrameState array */
    uint32_t                    frame_state_count; /* number of FrameStates */
    uint32_t                    max_depth;         /* max frames to walk (0 = unlimited) */
} vtx_stack_walk_config_t;

/* ========================================================================== */
/* Stack walking                                                              */
/* ========================================================================== */

/**
 * Walk the compiled frame stack starting from the given frame pointer.
 *
 * For each compiled frame:
 *   1. Look up the side table from the registry using the frame's code pointer.
 *   2. Determine the native PC (from the saved return address minus 1).
 *   3. Look up the FrameState from the side table.
 *   4. For inlined methods, the FrameState chain contains multiple logical frames.
 *   5. Reconstruct each logical frame as an interpreter frame.
 *
 * For each interpreter frame:
 *   1. Read the method pointer, PC, locals, and stack directly.
 *   2. Add to the reconstructed stack.
 *
 * Returns a newly allocated reconstructed stack (caller must free with
 * vtx_reconstructed_stack_destroy), or NULL on failure.
 */
vtx_reconstructed_stack_t *vtx_stack_walk(
    const vtx_stack_walk_config_t *config);

/* ========================================================================== */
/* Reconstructed stack lifecycle                                              */
/* ========================================================================== */

/**
 * Create an empty reconstructed stack.
 */
vtx_reconstructed_stack_t *vtx_reconstructed_stack_create(void);

/**
 * Destroy a reconstructed stack and free all frame memory.
 */
void vtx_reconstructed_stack_destroy(vtx_reconstructed_stack_t *stack);

/**
 * Add a reconstructed frame to the stack.
 * Returns 0 on success, -1 on failure.
 */
int vtx_reconstructed_stack_add_frame(
    vtx_reconstructed_stack_t *stack,
    const vtx_reconstructed_frame_t *frame);

/* ========================================================================== */
/* Side table registry                                                        */
/* ========================================================================== */

/**
 * Initialize a side table registry.
 */
int vtx_side_table_registry_init(vtx_side_table_registry_t *registry);

/**
 * Destroy a side table registry.
 */
void vtx_side_table_registry_destroy(vtx_side_table_registry_t *registry);

/**
 * Register a compiled code range with its side table.
 */
int vtx_side_table_registry_add(vtx_side_table_registry_t *registry,
                                 void *code_start,
                                 uint32_t code_size,
                                 vtx_side_table_t *side_table);

/**
 * Look up the side table for a code address.
 * Returns the side table, or NULL if not found.
 */
vtx_side_table_t *vtx_side_table_registry_lookup(
    const vtx_side_table_registry_t *registry,
    const void *code_addr);

/* ========================================================================== */
/* Frame pointer chain walking                                                */
/* ========================================================================== */

/**
 * Read the saved frame pointer from a compiled frame.
 * On x86-64, this is the value at [fp + 0].
 * Returns 0 on success, -1 on failure (e.g., invalid fp).
 */
int vtx_stack_walk_read_caller_fp(void *fp, void **out_caller_fp);

/**
 * Read the return address from a compiled frame.
 * On x86-64, this is the value at [fp + 8].
 * Returns 0 on success, -1 on failure.
 */
int vtx_stack_walk_read_return_addr(void *fp, void **out_return_addr);

/**
 * Determine the kind of frame at the given frame pointer.
 * Uses heuristics: if the code at the return address falls within a known
 * compiled code range, it's a compiled frame. Otherwise it's native or
 * interpreted.
 */
vtx_frame_kind_t vtx_stack_walk_classify_frame(
    const vtx_side_table_registry_t *registry,
    void *fp);

#endif /* VORTEX_DEOPT_STACK_WALK_H */
