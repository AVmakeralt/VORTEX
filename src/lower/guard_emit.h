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
                          vtx_arena_t *arena);

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

#endif /* VORTEX_LOWER_GUARD_EMIT_H */
