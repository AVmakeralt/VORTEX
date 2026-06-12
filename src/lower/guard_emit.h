#ifndef VORTEX_LOWER_GUARD_EMIT_H
#define VORTEX_LOWER_GUARD_EMIT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vortex_config.h"
#include "ir/node.h"
#include "lower/isel.h"
#include "lower/emit.h"
#include "deopt/side_table.h"
#include "runtime/arena.h"

/**
 * VORTEX Guard Emission for JIT-compiled Code
 *
 * Emits guard checks and deoptimization stubs from SoN Guard/DeoptGuard
 * nodes. For each guard:
 *   1. Emit a compare + conditional jump in the main code stream
 *   2. If the guard fails, jump to a deopt stub
 *   3. Record a side table entry (native PC, live registers, frame_state_index)
 *
 * Deopt stubs are emitted after the main code and perform:
 *   1. Save all live registers to the frame
 *   2. Load the deopt handler address
 *   3. Jump to the deopt runtime
 */

/* ========================================================================== */
/* Guard descriptor (for lowering)                                             */
/* ========================================================================== */

typedef struct {
    vtx_nodeid_t    guard_node;     /* the Guard/DeoptGuard SoN node */
    vtx_cond_t      cond;           /* condition that triggers deopt */
    uint32_t        bytecode_pc;    /* bytecode PC for deopt continuation */
    uint32_t        frame_state_index; /* index into FrameState array */
    uint32_t        type_id;        /* TypeID this guard depends on (0 = none) */
    uint32_t        shape_id;       /* ShapeID this guard depends on (0 = none) */

    /* Native code offsets, filled during guard emission pipeline:
     *   vtx_guard_emit_lower   -> jcc_native_offset
     *   vtx_guard_emit_deopt_stubs -> deopt_stub_offset
     *   vtx_guard_emit_patch   -> uses both to compute rel32 displacement */
    uint32_t        jcc_native_offset;   /* native offset of the JCC instruction (6 bytes: 0F 8x cd) */
    uint32_t        deopt_stub_offset;   /* native offset of the deopt stub for this guard */

    /* Zero-cost deopt flags */
    bool            is_implicit_null;    /* true if null check is implicit (SIGSEGV-based, no TEST+JCC) */
    bool            is_value_guard;      /* true if this is a value speculation guard */
    uint64_t        expected_value;      /* for value guards: the speculated constant value */
} vtx_guard_desc_t;

/* ========================================================================== */
/* Guard array (for lowering)                                                  */
/* ========================================================================== */

#define VTX_GUARD_DESC_INITIAL_CAPACITY 32

typedef struct {
    vtx_guard_desc_t *guards;
    uint32_t          count;
    uint32_t          capacity;
} vtx_guard_desc_array_t;

/**
 * Initialize a guard descriptor array.
 */
int vtx_guard_desc_array_init(vtx_guard_desc_array_t *arr, vtx_arena_t *arena);

/**
 * Add a guard descriptor.
 */
uint32_t vtx_guard_desc_array_add(vtx_guard_desc_array_t *arr,
                                   vtx_guard_desc_t guard, vtx_arena_t *arena);

/* ========================================================================== */
/* Guard lowering entry point                                                  */
/* ========================================================================== */

/**
 * Lower guard nodes into x86-64 instructions and record side table entries.
 *
 * For each guard in the guard array:
 *   1. Emits a compare + conditional jump into the emit buffer
 *   2. Records a side table entry with the native PC, live registers,
 *      and frame state index
 *   3. The deopt stubs are emitted after the main code by emit_deopt_stubs
 *
 * @param guards      Array of guard descriptors
 * @param inst_stream Instruction stream (for finding live registers at guard points)
 * @param emit        x86-64 code emitter (main code section)
 * @param side_table  Side table to record deopt entries
 * @param arena       Arena for allocations
 * @return            Number of guards lowered, or -1 on failure
 */
int vtx_guard_emit_lower(vtx_guard_desc_array_t *guards,
                          vtx_inst_stream_t *inst_stream,
                          vtx_x86_emit_t *emit,
                          vtx_side_table_t *side_table,
                          vtx_arena_t *arena,
                          const vtx_regalloc_result_t *ra);

/**
 * Emit deopt stubs after the main code.
 *
 * For each guard, emits a deopt stub that:
 *   1. Pushes all callee-saved registers
 *   2. Stores the frame state index in RDI
 *   3. Loads the deopt handler address
 *   4. Jumps to the deopt runtime
 *
 * @param guards      Array of guard descriptors
 * @param emit        x86-64 code emitter (will append stubs)
 * @param side_table  Side table for recording native PCs
 * @param code_start  Start address of the compiled code (for offset calculation)
 * @param arena       Arena for allocations
 * @return            Number of stubs emitted, or -1 on failure
 */
int vtx_guard_emit_deopt_stubs(vtx_guard_desc_array_t *guards,
                                vtx_x86_emit_t *emit,
                                vtx_side_table_t *side_table,
                                uint8_t *code_start,
                                vtx_arena_t *arena);

/**
 * Patch guard conditional jumps to point to their deopt stubs.
 *
 * After both main code and deopt stubs are emitted, this function
 * patches the 32-bit displacement in each guard's JCC instruction
 * to point to the corresponding deopt stub.
 *
 * @param guards      Array of guard descriptors
 * @param emit        x86-64 code emitter (code buffer to patch)
 * @param code_start  Start of the compiled code
 * @param arena       Arena for allocations
 * @return            0 on success, -1 on failure
 */
int vtx_guard_emit_patch(vtx_guard_desc_array_t *guards,
                          vtx_x86_emit_t *emit,
                          uint8_t *code_start,
                          vtx_arena_t *arena);

/* ========================================================================== */
/* Deopt handler configuration                                                 */
/* ========================================================================== */

/**
 * Set the global deopt handler function pointer.
 *
 * The deopt handler is called when a guard fails. It receives:
 *   - RDI = frame_state_index (which FrameState to use for reconstitution)
 *   - RSI = native_pc_offset (where in the compiled code the failure occurred)
 *
 * If no handler is set, a default stub is used that prints diagnostic
 * information and calls abort().
 *
 * @param handler  Function pointer for the deopt handler
 */
void vtx_guard_emit_set_deopt_handler(void *handler);

/**
 * Get the current deopt handler function pointer.
 * Returns NULL if no handler has been set (the default stub will be used).
 */
void *vtx_guard_emit_get_deopt_handler(void);

/* ========================================================================== */
/* Predicated guard emission (Proposal #11)                                     */
/* ========================================================================== */

/**
 * Emit a predicated guard using CMOVCC + conditional INT3.
 * Used for guards with very low failure rates where branch
 * misprediction penalty exceeds the cost of always-decoded deopt path.
 *
 * The emission strategy:
 *   1. Evaluate the guard condition into a register
 *   2. CMOVCC: conditionally set a flag byte to 0xCC (INT3 opcode)
 *   3. The flag byte is placed right after the CMOVCC
 *   4. If the condition fails, the flag becomes 0xCC → INT3 fires → deopt handler
 *   5. If the condition passes, the flag remains 0x90 (NOP) → no trap
 *
 * @param guard       Guard descriptor
 * @param code_buf    Output code buffer
 * @param buf_size    Size of code buffer
 * @return            Number of bytes emitted, or -1 on failure
 */
int vtx_guard_emit_predicated(const vtx_guard_desc_t *guard,
                                uint8_t *code_buf,
                                uint32_t buf_size);

/* ========================================================================== */
/* Implicit null check emission (zero-cost deopt)                              */
/* ========================================================================== */

/**
 * Emit an implicit null check — no code emitted.
 *
 * When the guard page / SIGSEGV mechanism is available, null checks
 * can be made implicit: instead of emitting TEST reg,reg + JCC, we
 * simply let the next memory load from that register trigger SIGSEGV
 * if the pointer is null. The signal handler catches the fault and
 * performs deoptimization.
 *
 * This function marks the guard as implicit_null and sets jcc_native_offset
 * to UINT32_MAX (no JCC to patch). The actual "guard" is the subsequent
 * load instruction from the checked register, which will SIGSEGV on null.
 *
 * The side table entry is recorded so the SIGSEGV handler can find the
 * deopt metadata for this code location.
 *
 * Hot-path cost: 0 instructions (the load was going to happen anyway).
 * Cold-path cost: SIGSEGV signal delivery + handler (~1-2 microseconds).
 *
 * Prerequisites:
 *   - vtx_guard_page_init() must have been called (guard page available)
 *   - The checked register must be used by a subsequent load instruction
 *   - The load must be at an offset < VTX_NULL_PAGE_LIMIT (64KB) from null
 *
 * @param guard       Guard descriptor (is_implicit_null will be set to true)
 * @param code_buf    Output code buffer (unused, but required for API consistency)
 * @param buf_size    Size of code buffer
 * @return            0 bytes emitted (always succeeds if guard != NULL)
 */
int vtx_guard_emit_implicit_null(vtx_guard_desc_t *guard,
                                   uint8_t *code_buf,
                                   uint32_t buf_size);

/* ========================================================================== */
/* Value guard emission (speculative constant folding)                         */
/* ========================================================================== */

/**
 * Emit a value speculation guard — CMP + JCC for now, guard-page in future.
 *
 * Value speculation guards verify that a loaded value matches a previously
 * observed constant. When the guard passes, all downstream uses of the
 * value can be constant-folded, enabling:
 *   - Dead code elimination of branches that depend on the constant
 *   - Loop unrolling with known trip counts
 *   - Strength reduction (e.g., multiply by constant → shifts)
 *
 * Current emission (CMP+JCC, same as type guard):
 *   mov rax, [obj + field_offset]    ; load the field
 *   cmp rax, <expected_value>         ; is it the expected constant?
 *   jne deopt_stub                    ; if not, deopt
 *   ; rax is now "proven" constant → replace all uses with <expected_value>
 *
 * Future emission (guard page, zero-cost):
 *   mov rax, [obj + field_offset]    ; load the field
 *   mov rbx, [value_check_page + rax*8] ; trap if wrong value
 *   ; rax is "proven" to be <expected_value> → constant fold
 *
 * @param guard       Guard descriptor (is_value_guard must be true,
 *                    expected_value must be set)
 * @param code_buf    Output code buffer
 * @param buf_size    Size of code buffer
 * @return            Number of bytes emitted, or -1 on failure
 */
int vtx_guard_emit_value_guard(const vtx_guard_desc_t *guard,
                                 uint8_t *code_buf,
                                 uint32_t buf_size);

#endif /* VORTEX_LOWER_GUARD_EMIT_H */
