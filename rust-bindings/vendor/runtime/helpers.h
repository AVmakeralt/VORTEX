#ifndef VORTEX_HELPERS_H
#define VORTEX_HELPERS_H

#include <stdint.h>
#include <stdbool.h>
#include "vortex_config.h"
#include "runtime/object.h"
#include "runtime/type_system.h"

/**
 * VORTEX Runtime Helper Functions
 *
 * These functions are called from JIT-compiled code and the interpreter.
 * They implement type checks, null checks, bounds checks, overflow checks,
 * virtual dispatch resolution, and string comparison.
 */

/* ========================================================================== */
/* D8: Register-based calling convention                                       */
/* ========================================================================== */

/**
 * VORTEX JIT Calling Convention (based on System V AMD64 ABI)
 *
 * All production JITs pass arguments in registers, eliminating the
 * variadic dispatch overhead of pushing arguments onto the stack in
 * a type-erased array and then unpacking them in the callee.
 *
 * Register assignment for JIT call arguments:
 *   arg[0] → RDI   (interp pointer — always first arg)
 *   arg[1] → RSI   (method descriptor pointer)
 *   arg[2] → RDX   (first value argument)
 *   arg[3] → RCX   (second value argument)
 *   arg[4] → R8    (third value argument)
 *   arg[5] → R9    (fourth value argument)
 *   Additional args → stack (7th arg at [RSP+8], 8th at [RSP+16], etc.)
 *   Return value → RAX
 *
 * This matches the standard C calling convention on Linux x86-64,
 * so transitions between JIT code and C runtime helpers are seamless:
 * no argument shuffling is needed. The interp and method pointers
 * are always passed as the first two arguments, providing the runtime
 * context that every JIT-compiled function needs.
 */

/* Maximum arguments passed in registers (excluding implicit interp/method) */
#define VTX_CALL_ARG_REGS 6

/* Register encoding for x86-64 ModRM byte (register numbers).
 * These are used by the baseline codegen when emitting MOV instructions
 * to place arguments into the correct registers before a call. */
static const uint8_t vtx_call_arg_regs[VTX_CALL_ARG_REGS] = {
    7,  /* RDI */
    6,  /* RSI */
    2,  /* RDX */
    1,  /* RCX */
    8,  /* R8  */
    9   /* R9  */
};

/* Forward declaration */
struct vtx_method_desc;

/* Include bytecode.h for vtx_method_desc_t definition.
 * This is needed so that the register-based call helpers can access
 * the method descriptor's fields directly. */
#include "runtime/bytecode.h"

/* ========================================================================== */
/* Type checking                                                               */
/* ========================================================================== */

/**
 * Type check: verify that a value's heap object is an instance of
 * the expected type. Returns true if the check passes.
 * If the value is not a heap pointer, returns false.
 */
bool vtx_helpers_type_check(const vtx_type_system_t *ts,
                            vtx_value_t obj_value,
                            vtx_typeid_t expected_typeid);

/* ========================================================================== */
/* Null and bounds checks                                                      */
/* ========================================================================== */

/**
 * Null check: returns true if the value is NOT null.
 * Calls abort() (trap) if the value IS null. This is a runtime trap
 * used by the interpreter and JIT code.
 */
bool vtx_helpers_null_check(vtx_value_t value);

/**
 * Bounds check: returns true if 0 <= index < length.
 * Calls abort() (trap) if the index is out of bounds.
 */
bool vtx_helpers_bounds_check(int64_t index, int64_t length);

/* ========================================================================== */
/* Overflow checks                                                             */
/* ========================================================================== */

/**
 * Check if integer addition would overflow.
 * Returns true if a + b does NOT overflow int64_t.
 */
bool vtx_helpers_overflow_check_iadd(int64_t a, int64_t b);

/**
 * Check if integer multiplication would overflow.
 * Returns true if a * b does NOT overflow int64_t.
 */
bool vtx_helpers_overflow_check_imul(int64_t a, int64_t b);

/* ========================================================================== */
/* Virtual dispatch resolution                                                 */
/* ========================================================================== */

/**
 * Resolve a virtual method call using inline caching.
 * First attempts IC lookup; on miss, does a full vtable walk and
 * updates the IC.
 *
 * Returns the resolved method descriptor, or NULL if not found.
 */
const vtx_method_desc_t *vtx_helpers_resolve_virtual(vtx_type_system_t *ts,
                                                      vtx_inline_cache_t *ic,
                                                      vtx_value_t obj_value,
                                                      const char *method_name);

/**
 * Resolve an interface method call using inline caching.
 * Similar to virtual resolution but also checks interface implementations.
 *
 * Returns the resolved method descriptor, or NULL if not found.
 */
const vtx_method_desc_t *vtx_helpers_resolve_interface(vtx_type_system_t *ts,
                                                        vtx_inline_cache_t *ic,
                                                        vtx_value_t obj_value,
                                                        vtx_typeid_t interface_typeid,
                                                        const char *method_name);

/* ========================================================================== */
/* String comparison                                                           */
/* ========================================================================== */

/**
 * Compare two string objects stored as heap objects.
 * Both values must be heap pointers to string objects.
 * Returns <0, 0, >0 like strcmp.
 *
 * String objects have their character data stored starting at field index 0
 * as a sequence of bytes. The first field (fields[0]) stores the length
 * as an SMI, and subsequent fields store the character data packed into
 * vtx_value_t words.
 *
 * Actually, for simplicity, we store strings with:
 *   field[0] = length (SMI)
 *   field[1] through field[N] = packed char data in vtx_value_t slots
 *
 * For a simpler approach, we store the string data as a C string
 * in the fields array starting at offset 1.
 */
int vtx_helpers_string_compare(vtx_value_t a, vtx_value_t b);

/* ========================================================================== */
/* String object helpers                                                       */
/* ========================================================================== */

/**
 * Get the C-string data from a string heap object.
 * Returns a pointer to a null-terminated string, or "" if not a string.
 */
const char *vtx_helpers_string_data(vtx_value_t str_value);

/**
 * Get the length of a string heap object.
 */
uint32_t vtx_helpers_string_length(vtx_value_t str_value);

/* ========================================================================== */
/* GC write barrier                                                            */
/* ========================================================================== */

/**
 * GC write barrier: must be called after storing a reference to an object.
 * Implements a card-table based write barrier for generational GC.
 * Marks the card containing [obj + offset] as dirty so the GC can
 * find cross-generational references during young-gen collection.
 *
 * @param obj           Pointer to the heap object containing the field
 * @param field_offset  Byte offset of the field within the object
 */
void vtx_helpers_write_barrier(void *obj, uint32_t field_offset);

/* ========================================================================== */
/* D8: Register-based call helpers                                             */
/* ========================================================================== */

/**
 * Call a method with arguments passed in registers.
 *
 * B7/B8 fix: The JIT entry calling convention (T2) is:
 *   RDI = method_ptr, RSI = deopt_info, RDX = profile_data,
 *   RCX = args array pointer, R8 = arg_count
 *
 * The baseline codegen sets up exactly these registers and calls this
 * helper. The helper ignores deopt_info/profile_data (they are
 * per-callsite values that the callee resolves from its own
 * compiled_code metadata), looks up the interpreter via
 * vtx_get_current_interp(), and dispatches to vtx_interp_run().
 *
 * @param method        Method descriptor for the target method
 * @param deopt_info    Deopt info pointer (NULL for fresh calls — ignored)
 * @param profile_data  Profile data pointer (NULL or sentinel 1 — ignored)
 * @param args          Array of argument values (already in the right order)
 * @param arg_count     Number of arguments in the args array
 * @return              The method's return value
 */
vtx_value_t vtx_runtime_call_reg(const vtx_method_desc_t *method,
                                   void *deopt_info,
                                   void *profile_data,
                                   vtx_value_t *args,
                                   uint32_t arg_count);

/**
 * Call a virtual method with register-based dispatch.
 *
 * B7/B8 fix: Uses the same JIT entry calling convention as
 * vtx_runtime_call_reg. The receiver is args[0]; the method name is
 * derived from `method->name`. The receiver's type is used to resolve
 * the actual target method.
 *
 * @param method        Method descriptor (provides method name) — this is
 *                      the statically-known method at the call site
 * @param deopt_info    Deopt info pointer (NULL — ignored)
 * @param profile_data  Profile data pointer (NULL or sentinel 1 — ignored)
 * @param args          Array of argument values; args[0] is the receiver
 * @param arg_count     Number of arguments in the args array (incl. receiver)
 * @return              The method's return value
 */
vtx_value_t vtx_runtime_call_virtual_reg(const vtx_method_desc_t *method,
                                           void *deopt_info,
                                           void *profile_data,
                                           vtx_value_t *args,
                                           uint32_t arg_count);

/**
 * Call an interface method with register-based dispatch.
 *
 * B7/B8 fix: Uses the JIT entry calling convention. The interface
 * typeid is derived from the receiver's type via the type system.
 *
 * @param method        Method descriptor (provides method name)
 * @param deopt_info    Deopt info pointer (NULL — ignored)
 * @param profile_data  Profile data pointer (NULL or sentinel 1 — ignored)
 * @param args          Array of argument values; args[0] is the receiver
 * @param arg_count     Number of arguments (incl. receiver)
 * @return              The method's return value
 */
vtx_value_t vtx_runtime_call_interface_reg(const vtx_method_desc_t *method,
                                             void *deopt_info,
                                             void *profile_data,
                                             vtx_value_t *args,
                                             uint32_t arg_count);

/**
 * Dispatch a runtime builtin call from JIT-compiled code.
 *
 * The JIT emits CALL_RUNTIME with a runtime function ID (0-6).
 * This function dispatches to the correct implementation:
 *   0 = typeof, 1 = monitor_enter, 2 = monitor_exit, 3 = throw,
 *   4 = print_ln, 5 = print, 6 = exit
 *
 * @param func_id  Runtime function ID (from the bytecode operand)
 * @param arg      The argument value (popped from the stack)
 * @return         Result value (undefined for void calls, SMI for typeof/exit)
 */
vtx_value_t vtx_runtime_builtin_call(uint32_t func_id, vtx_value_t arg);

#endif /* VORTEX_HELPERS_H */
