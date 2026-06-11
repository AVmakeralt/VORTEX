#ifndef VORTEX_BASELINE_GUARDS_H
#define VORTEX_BASELINE_GUARDS_H

#include <stdint.h>
#include <stdbool.h>
#include "vortex_config.h"
#include "runtime/object.h"
#include "runtime/type_system.h"
#include "baseline/frame_layout.h"

/**
 * VORTEX Baseline JIT Guard Emission
 *
 * Guards are speculative checks inserted by the baseline compiler.
 * If a guard fails, execution transfers to a deopt stub that
 * reconstructs the interpreter frame state and resumes in the
 * interpreter at the correct bytecode PC.
 *
 * Each guard records metadata needed for deoptimization:
 *   - Bytecode PC where the guard was inserted
 *   - Guard kind (type check, null check, bounds check, overflow)
 *   - Expected value (e.g., expected TypeID)
 *   - Deopt continuation (bytecode PC to resume in interpreter)
 *   - Stack depth at the guard point
 *
 * Guard emission writes x86-64 machine code into a code buffer.
 * The emitted code performs the check and conditionally jumps to
 * a deopt stub on failure.
 */

/* ========================================================================== */
/* Guard kinds                                                                 */
/* ========================================================================== */

typedef enum {
    VTX_GUARD_TYPE_CHECK = 0,  /* object type_id != expected */
    VTX_GUARD_NULL_CHECK = 1,  /* pointer is null */
    VTX_GUARD_BOUNDS_CHECK = 2,/* index >= length */
    VTX_GUARD_OVERFLOW = 3,    /* arithmetic overflow */
    VTX_GUARD_DIV_ZERO = 4,    /* division by zero */
} vtx_guard_kind_t;

/* ========================================================================== */
/* Guard info record                                                           */
/* ========================================================================== */

/**
 * Metadata for a single guard. Recorded during compilation so that
 * deopt stubs can be generated after the main code is emitted.
 */
typedef struct {
    vtx_guard_kind_t  kind;            /* what kind of check */
    uint32_t          bytecode_pc;      /* bytecode PC of the guarded operation */
    uint32_t          native_pc_offset; /* native code offset of the guard check */
    uint32_t          deopt_continuation; /* bytecode PC to resume in interpreter */
    uint32_t          stack_depth;      /* expression stack depth at guard point */
    uint64_t          expected_value;   /* expected value (e.g., TypeID for type checks) */
    int32_t           branch_offset;    /* relative offset to deopt stub (patched later) */
} vtx_guard_info_t;

/* ========================================================================== */
/* Guard array                                                                 */
/* ========================================================================== */

#define VTX_GUARDS_INITIAL_CAPACITY 32

typedef struct {
    vtx_guard_info_t *guards;
    uint32_t          count;
    uint32_t          capacity;
} vtx_guard_array_t;

/**
 * Initialize a guard array.
 * Returns 0 on success, -1 on failure.
 */
int vtx_guard_array_init(vtx_guard_array_t *arr);

/**
 * Destroy a guard array and free memory.
 */
void vtx_guard_array_destroy(vtx_guard_array_t *arr);

/**
 * Add a guard info record to the array.
 * Returns the index of the new guard, or UINT32_MAX on failure.
 */
uint32_t vtx_guard_array_add(vtx_guard_array_t *arr, vtx_guard_info_t guard);

/* ========================================================================== */
/* x86-64 register enumeration                                                 */
/* ========================================================================== */

/**
 * Register numbering used by the baseline compiler.
 * These correspond to x86-64 physical registers.
 */
typedef enum {
    VTX_REG_RAX = 0,
    VTX_REG_RCX = 1,
    VTX_REG_RDX = 2,
    VTX_REG_RBX = 3,
    VTX_REG_RSP = 4,
    VTX_REG_RBP = 5,
    VTX_REG_RSI = 6,
    VTX_REG_RDI = 7,
    VTX_REG_R8  = 8,
    VTX_REG_R9  = 9,
    VTX_REG_R10 = 10,
    VTX_REG_R11 = 11,
    VTX_REG_R12 = 12,
    VTX_REG_R13 = 13,
    VTX_REG_R14 = 14,
    VTX_REG_R15 = 15,
    VTX_REG_NONE = 255
} vtx_reg_t;

/* ========================================================================== */
/* Code buffer (forward declaration, defined in codegen.h)                     */
/* ========================================================================== */

typedef struct vtx_code_buffer vtx_code_buffer_t;

/* ========================================================================== */
/* Guard emission functions                                                    */
/* ========================================================================== */

/**
 * Emit a type check guard: verify that the object in `obj_reg` has the
 * expected TypeID. If the type doesn't match, jump to a deopt stub.
 *
 * Generated code sequence:
 *   1. Extract the heap object pointer from the tagged value in obj_reg
 *   2. Load the type_id from the object header at offset 0
 *   3. Compare against expected_typeid
 *   4. If not equal, jump to deopt stub
 *
 * @param buf             Code buffer to emit into
 * @param obj_reg         Register holding the tagged value (heap pointer)
 * @param expected_typeid Expected TypeID to check against
 * @param guard_info      Guard metadata (bytecode_pc, stack_depth, etc.)
 *                        native_pc_offset is set by this function
 * @param guard_arr       Guard array to record the guard (for deopt stub gen)
 */
void vtx_guard_emit_type_check(vtx_code_buffer_t *buf,
                                vtx_reg_t obj_reg,
                                vtx_typeid_t expected_typeid,
                                vtx_guard_info_t guard_info,
                                vtx_guard_array_t *guard_arr);

/**
 * Emit a null check guard: verify that the value in `obj_reg` is not null.
 * If the value is null, jump to a deopt stub.
 *
 * Generated code sequence:
 *   1. Test the tagged value against VTX_VALUE_NULL
 *   2. If equal, jump to deopt stub
 *
 * Alternatively (for heap pointers):
 *   1. Test the raw pointer value
 *   2. If zero, jump to deopt stub
 *
 * @param buf         Code buffer to emit into
 * @param obj_reg     Register holding the value to check
 * @param guard_info  Guard metadata
 * @param guard_arr   Guard array to record the guard
 */
void vtx_guard_emit_null_check(vtx_code_buffer_t *buf,
                                vtx_reg_t obj_reg,
                                vtx_guard_info_t guard_info,
                                vtx_guard_array_t *guard_arr);

/**
 * Emit a bounds check guard: verify that 0 <= index < length.
 * If the index is out of bounds, jump to a deopt stub.
 *
 * Generated code sequence:
 *   1. Compare index_reg (sign-extended) against 0 (unsigned comparison)
 *   2. If below (index < 0 unsigned), jump to deopt
 *   3. Compare index_reg against length_reg
 *   4. If above-or-equal, jump to deopt
 *
 * @param buf          Code buffer to emit into
 * @param index_reg    Register holding the index value
 * @param length_reg   Register holding the length value
 * @param guard_info   Guard metadata
 * @param guard_arr    Guard array to record the guard
 */
void vtx_guard_emit_bounds_check(vtx_code_buffer_t *buf,
                                  vtx_reg_t index_reg,
                                  vtx_reg_t length_reg,
                                  vtx_guard_info_t guard_info,
                                  vtx_guard_array_t *guard_arr);

/**
 * Emit an overflow check guard for integer addition.
 * Uses the CPU's overflow flag after an add instruction.
 *
 * Generated code sequence:
 *   1. After the add instruction, check the overflow flag (JO)
 *   2. If overflow, jump to deopt stub
 *
 * @param buf         Code buffer to emit into
 * @param guard_info  Guard metadata
 * @param guard_arr   Guard array to record the guard
 */
void vtx_guard_emit_overflow_check(vtx_code_buffer_t *buf,
                                    vtx_guard_info_t guard_info,
                                    vtx_guard_array_t *guard_arr);

/**
 * Emit a division-by-zero guard.
 *
 * Generated code sequence:
 *   1. Test the divisor register
 *   2. If zero, jump to deopt stub
 *
 * @param buf          Code buffer to emit into
 * @param divisor_reg  Register holding the divisor value
 * @param guard_info   Guard metadata
 * @param guard_arr    Guard array to record the guard
 */
void vtx_guard_emit_div_zero_check(vtx_code_buffer_t *buf,
                                    vtx_reg_t divisor_reg,
                                    vtx_guard_info_t guard_info,
                                    vtx_guard_array_t *guard_arr);

#endif /* VORTEX_BASELINE_GUARDS_H */
