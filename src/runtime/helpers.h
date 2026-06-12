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
/* D8: Register-based call helpers                                             */
/* ========================================================================== */

/**
 * Call a method with arguments passed in registers.
 *
 * This replaces the variadic vtx_runtime_call_static/virtual/interface
 * functions with register-based dispatch. The JIT codegen places arguments
 * directly into the System V AMD64 ABI registers (RDI, RSI, RDX, RCX, R8, R9),
 * and this function receives them as a flat array.
 *
 * The key advantage: no variadic argument marshaling. The JIT-compiled code
 * at the call site moves arguments directly from the expression stack to the
 * correct registers, and the callee receives them in the same registers
 * without any intermediate memory operations.
 *
 * @param interp     Interpreter state (provides type system, GC, profiler).
 *                   Opaque pointer — cast from vtx_interp_t* in the implementation.
 * @param method     Method descriptor for the target method
 * @param args       Array of argument values (already in the right order)
 * @param arg_count  Number of arguments in the args array
 * @return           The method's return value
 */
vtx_value_t vtx_runtime_call_reg(void *interp,
                                   const vtx_method_desc_t *method,
                                   vtx_value_t *args,
                                   uint32_t arg_count);

/**
 * Call a virtual method with register-based dispatch.
 *
 * Similar to vtx_runtime_call_reg but performs virtual dispatch:
 * looks up the actual method based on the receiver's type, then
 * calls it with register-based argument passing.
 *
 * @param interp       Interpreter state (opaque pointer)
 * @param method_name  Name of the virtual method to resolve
 * @param receiver     The receiver object (first implicit argument)
 * @param args         Remaining arguments (after receiver)
 * @param arg_count    Number of remaining arguments (not counting receiver)
 * @return             The method's return value
 */
vtx_value_t vtx_runtime_call_virtual_reg(void *interp,
                                           const char *method_name,
                                           vtx_value_t receiver,
                                           vtx_value_t *args,
                                           uint32_t arg_count);

/**
 * Call an interface method with register-based dispatch.
 *
 * @param interp            Interpreter state (opaque pointer)
 * @param interface_typeid  TypeID of the interface
 * @param method_name       Name of the interface method
 * @param receiver          The receiver object
 * @param args              Remaining arguments
 * @param arg_count         Number of remaining arguments
 * @return                  The method's return value
 */
vtx_value_t vtx_runtime_call_interface_reg(void *interp,
                                             vtx_typeid_t interface_typeid,
                                             const char *method_name,
                                             vtx_value_t receiver,
                                             vtx_value_t *args,
                                             uint32_t arg_count);

#endif /* VORTEX_HELPERS_H */
