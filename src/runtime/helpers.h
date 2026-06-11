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

#endif /* VORTEX_HELPERS_H */
