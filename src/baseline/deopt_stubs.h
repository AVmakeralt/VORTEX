#ifndef VORTEX_BASELINE_DEOPT_STUBS_H
#define VORTEX_BASELINE_DEOPT_STUBS_H

#include <stdint.h>
#include <stdbool.h>
#include "vortex_config.h"
#include "runtime/object.h"
#include "runtime/bytecode.h"
#include "runtime/arena.h"
#include "deopt/side_table.h"
#include "baseline/frame_layout.h"
#include "baseline/guards.h"

/**
 * VORTEX Baseline JIT Deoptimization Stub Generation
 *
 * For each guard that can fail, a deopt stub is generated. When a guard
 * fails, execution jumps to the deopt stub which:
 *
 *   1. Saves all live register values into the JIT frame (locals and
 *      spill slots) so the interpreter can find them.
 *   2. Reconstructs the interpreter operand stack from the JIT frame
 *      state: register-resident values are stored into the interpreter
 *      stack frame, spill slot values are copied.
 *   3. Restores the interpreter frame pointer from the JIT frame header.
 *   4. Sets the interpreter's bytecode PC to the deopt continuation point.
 *   5. Jumps to the interpreter dispatch loop.
 *
 * The deopt stubs are appended after the main method code in the same
 * code buffer. Each stub is a self-contained sequence that does not
 * fall through to the next stub.
 */

/* ========================================================================== */
/* Deopt stub descriptor                                                       */
/* ========================================================================== */

typedef struct {
    uint8_t  *native_code;       /* pointer to the start of the stub code */
    uint32_t  native_code_size;  /* size of the stub code in bytes */
    uint32_t  native_code_offset;/* offset from the start of the compiled method */

    /* Which guard this stub serves */
    uint32_t  guard_index;       /* index into the guard array */
    uint32_t  bytecode_pc;       /* bytecode PC to resume in interpreter */
    uint32_t  stack_depth;       /* expression stack depth at deopt point */

    /* Frame state reconstruction info */
    uint32_t  frame_state_index; /* index into the side table's frame state array */
} vtx_deopt_stub_t;

/* ========================================================================== */
/* Deopt stub array                                                            */
/* ========================================================================== */

#define VTX_DEOPT_STUBS_INITIAL_CAPACITY 32

typedef struct {
    vtx_deopt_stub_t *stubs;
    uint32_t          count;
    uint32_t          capacity;
} vtx_deopt_stub_array_t;

/**
 * Initialize a deopt stub array.
 * Returns 0 on success, -1 on failure.
 */
int vtx_deopt_stub_array_init(vtx_deopt_stub_array_t *arr);

/**
 * Destroy a deopt stub array.
 */
void vtx_deopt_stub_array_destroy(vtx_deopt_stub_array_t *arr);

/**
 * Add a deopt stub to the array.
 * Returns the index, or UINT32_MAX on failure.
 */
uint32_t vtx_deopt_stub_array_add(vtx_deopt_stub_array_t *arr,
                                   vtx_deopt_stub_t stub);

/* ========================================================================== */
/* Deopt stub emission                                                         */
/* ========================================================================== */

/**
 * Context for deopt stub generation. Passed between the stub emitter
 * and the code generator to resolve addresses and offsets.
 */
typedef struct {
    vtx_code_buffer_t    *code_buf;         /* code buffer for stub emission */
    vtx_guard_array_t    *guards;           /* the guard array */
    vtx_jit_frame_layout_t frame_layout;    /* the frame layout */
    vtx_side_table_t     *side_table;       /* side table for deopt info */
    vtx_arena_t          *arena;            /* arena for allocations */
    uint8_t              *code_start;       /* start of the compiled method code */
    uint32_t              method_id;        /* method identifier */
    vtx_deopt_stub_array_t *stub_array;     /* where to store generated stubs */
} vtx_deopt_context_t;

/**
 * Generate deopt stubs for all guards in the guard array.
 *
 * For each guard, emits a deopt stub after the main method code.
 * Patches the guard's conditional jump to point to the stub.
 *
 * Each stub performs:
 *   1. Save all expression stack registers (RAX, RCX, RDX, RBX) to
 *      their spill slots in the JIT frame.
 *   2. Set up the interpreter frame pointer from the JIT frame's
 *      caller RBP slot.
 *   3. Store the bytecode PC (deopt continuation) into a well-known
 *      location that the interpreter reads on entry.
 *   4. Copy locals from the JIT frame to the interpreter frame.
 *   5. Reconstruct the interpreter operand stack from the saved
 *      register values and spill slots.
 *   6. Jump to the interpreter dispatch loop entry point.
 *
 * @param ctx  Deopt context with all necessary references
 * @return     Number of stubs generated, or -1 on failure
 */
int vtx_deopt_stubs_emit_all(vtx_deopt_context_t *ctx);

/**
 * Generate a single deopt stub for a specific guard.
 *
 * @param ctx          Deopt context
 * @param guard_index  Index of the guard in the guard array
 * @return             The deopt stub descriptor, or NULL on failure
 */
const vtx_deopt_stub_t *vtx_deopt_stub_emit(vtx_deopt_context_t *ctx,
                                              uint32_t guard_index);

/**
 * Patch all guard conditional jumps to point to their deopt stubs.
 * Must be called after all stubs have been emitted.
 *
 * @param ctx  Deopt context
 * @return     0 on success, -1 on failure
 */
int vtx_deopt_stubs_patch_guards(vtx_deopt_context_t *ctx);

/* ========================================================================== */
/* Interpreter re-entry                                                        */
/* ========================================================================== */

/**
 * Get the address of the interpreter re-entry point.
 * This is where deopt stubs jump to resume interpretation.
 * The re-entry point expects:
 *   - RBP = interpreter frame pointer
 *   - RIP-relative stored bytecode PC
 *   - Operand stack reconstructed in the interpreter frame
 *
 * This function returns a function pointer that the runtime must
 * provide (the interpreter dispatch loop entry).
 */
typedef void (*vtx_interp_entry_t)(void);

/**
 * Set the interpreter entry point for deopt stubs to jump to.
 * Must be called before any deopt stubs are generated.
 */
void vtx_deopt_set_interp_entry(vtx_interp_entry_t entry);

/**
 * Get the current interpreter entry point.
 */
vtx_interp_entry_t vtx_deopt_get_interp_entry(void);

/* ========================================================================== */
/* Deopt runtime                                                               */
/* ========================================================================== */

/**
 * Deoptimization runtime function. Called by deopt stubs to transition
 * from JIT code back to the interpreter.
 *
 * This function:
 *   1. Reads the deopt info from the current frame
 *   2. Reconstructs the interpreter frame
 *   3. Returns the interpreter frame pointer and bytecode PC
 *
 * @param jit_rbp    The JIT frame's RBP value
 * @param native_pc  The native PC where deopt occurred
 * @return           The interpreter frame pointer (or NULL on error)
 */
void *vtx_deopt_runtime_transition(void *jit_rbp, uint32_t native_pc);

/* ========================================================================== */
/* Tier-aware deoptimization (Proposal #5)                                     */
/* ========================================================================== */

/**
 * Configuration for tier-aware deoptimization (Proposal #5).
 * When a deopt stub is entered, it checks whether a lower-tier compiled
 * version exists before falling back to the interpreter.
 */
typedef struct {
    bool     prefer_lower_tier;     /* true = try T2 before T0 */
    bool     require_same_frame_layout; /* true = only use version with matching frame layout */
} vtx_deopt_tier_config_t;

/**
 * Default tier-aware deopt configuration.
 */
static inline vtx_deopt_tier_config_t vtx_deopt_tier_config_default(void)
{
    vtx_deopt_tier_config_t cfg;
    cfg.prefer_lower_tier = true;
    cfg.require_same_frame_layout = false; /* T2 frame layout may differ */
    return cfg;
}

#endif /* VORTEX_BASELINE_DEOPT_STUBS_H */
