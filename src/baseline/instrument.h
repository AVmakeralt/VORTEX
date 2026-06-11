#ifndef VORTEX_BASELINE_INSTRUMENT_H
#define VORTEX_BASELINE_INSTRUMENT_H

#include <stdint.h>
#include <stdbool.h>
#include "vortex_config.h"
#include "runtime/object.h"
#include "runtime/type_system.h"
#include "interp/profiler.h"
#include "baseline/guards.h"

/**
 * VORTEX Baseline JIT Profiling Instrumentation
 *
 * T1 compiled code includes the same profiling counters as the
 * interpreter, so that T2 compilation can use profile data
 * collected during T1 execution. The counters are stored in the
 * same vtx_profile_data_t structures used by the interpreter.
 *
 * Instrumentation is emitted as inline x86-64 machine code that
 * directly increments counters in the profile data structure.
 * This avoids function call overhead for the common case.
 *
 * Profile data pointer is stored in the JIT frame header at
 * [RBP + VTX_FRAME_PROFILE_DATA_OFFSET] and can be loaded at
 * any point during execution.
 */

/* ========================================================================== */
/* Profile data field offsets                                                  */
/* ========================================================================== */

/* Offsets within vtx_profile_data_t for direct access from JIT code */

#define VTX_PD_INVOCATION_COUNT_OFFSET  \
    offsetof(vtx_profile_data_t, invocation_count)

#define VTX_PD_BACKWARD_BRANCH_COUNT_OFFSET \
    offsetof(vtx_profile_data_t, backward_branch_count)

#define VTX_PD_CALL_SITE_TYPES_OFFSET \
    offsetof(vtx_profile_data_t, call_site_types)

#define VTX_PD_BRANCH_TAKEN_COUNTS_OFFSET \
    offsetof(vtx_profile_data_t, branch_taken_counts)

#define VTX_PD_BRANCH_TOTAL_COUNTS_OFFSET \
    offsetof(vtx_profile_data_t, branch_total_counts)

#define VTX_PD_BRANCH_ARRAY_SIZE_OFFSET \
    offsetof(vtx_profile_data_t, branch_array_size)

/* Offset of the call_site_types array element (vtx_call_site_profile_t) */
#define VTX_CSP_ENTRIES_OFFSET \
    offsetof(vtx_call_site_profile_t, entries)

#define VTX_CSP_COUNT_OFFSET \
    offsetof(vtx_call_site_profile_t, count)

/* Size of a single IC entry */
#define VTX_IC_ENTRY_SIZE sizeof(vtx_ic_entry_t)

/* Offset of typeid within IC entry */
#define VTX_IC_ENTRY_TYPEID_OFFSET offsetof(vtx_ic_entry_t, typeid_)

/* ========================================================================== */
/* Instrumentation emission functions                                          */
/* ========================================================================== */

/**
 * Emit code to increment the method invocation counter.
 *
 * Generated code:
 *   1. Load profile_data pointer from [RBP + VTX_FRAME_PROFILE_DATA_OFFSET]
 *   2. Increment invocation_count (saturating at UINT64_MAX)
 *      inc qword ptr [profile_data + VTX_PD_INVOCATION_COUNT_OFFSET]
 *      ; or: add qword ptr [...], 1 + jno skip + mov qword ptr [...], UINT64_MAX
 *
 * @param buf           Code buffer to emit into
 * @param profile_data  If non-NULL, can be used for direct address;
 *                      if NULL, load from frame
 */
void vtx_instrument_emit_invocation_increment(vtx_code_buffer_t *buf,
                                               vtx_profile_data_t *profile_data);

/**
 * Emit code to record the receiver type at a call site.
 *
 * This records the TypeID of the receiver in the call site's type
 * profile array. The profile data is used by the T2 optimizer for
 * speculative inlining and inline cache optimization.
 *
 * Generated code:
 *   1. Load profile_data pointer from frame
 *   2. Compute call_site_types[call_site_pc] address
 *   3. Load the current count of type observations
 *   4. If count < VTX_POLY_LIMIT, store the new TypeID at entries[count]
 *   5. Increment count (saturating)
 *
 * @param buf           Code buffer to emit into
 * @param profile_data  Profile data structure (may be NULL)
 * @param call_site_pc  Bytecode PC of the call site
 * @param receiver_reg  Register holding the receiver value (tagged)
 * @param typeid_reg    Register holding the TypeID to record
 *                      (if VTX_REG_NONE, will extract from receiver_reg)
 */
void vtx_instrument_emit_call_type_record(vtx_code_buffer_t *buf,
                                           vtx_profile_data_t *profile_data,
                                           uint32_t call_site_pc,
                                           vtx_reg_t receiver_reg,
                                           vtx_reg_t typeid_reg);

/**
 * Emit code to record a branch outcome.
 *
 * This increments both the taken count (if taken) and the total count
 * for the branch at the given bytecode PC.
 *
 * Generated code:
 *   1. Load profile_data pointer from frame
 *   2. Increment branch_total_counts[branch_pc] (saturating at UINT32_MAX)
 *   3. If taken, increment branch_taken_counts[branch_pc]
 *
 * @param buf           Code buffer to emit into
 * @param profile_data  Profile data structure (may be NULL)
 * @param branch_pc     Bytecode PC of the branch instruction
 * @param taken         True if the branch is taken, false otherwise
 *                      (the JIT knows this at compile time for unconditional
 *                       branches; for conditional, emit on the taken path)
 */
void vtx_instrument_emit_branch_record(vtx_code_buffer_t *buf,
                                        vtx_profile_data_t *profile_data,
                                        uint32_t branch_pc,
                                        bool taken);

/**
 * Emit code to increment the backward branch counter.
 * Used at loop back-edges to track loop iteration counts.
 *
 * Generated code:
 *   1. Load profile_data pointer from frame
 *   2. Increment backward_branch_count (saturating at UINT64_MAX)
 *
 * @param buf           Code buffer to emit into
 * @param profile_data  Profile data structure (may be NULL)
 */
void vtx_instrument_emit_backward_branch_increment(vtx_code_buffer_t *buf,
                                                     vtx_profile_data_t *profile_data);

#endif /* VORTEX_BASELINE_INSTRUMENT_H */
