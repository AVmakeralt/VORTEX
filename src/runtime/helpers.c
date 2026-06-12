#include "runtime/helpers.h"

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "runtime/gc.h"
#include "interp/dispatch.h"

/* ========================================================================== */
/* Type checking                                                               */
/* ========================================================================== */

bool vtx_helpers_type_check(const vtx_type_system_t *ts,
                            vtx_value_t obj_value,
                            vtx_typeid_t expected_typeid)
{
    VTX_ASSERT(ts != NULL, "type system must not be NULL");

    if (!vtx_is_heap_ptr(obj_value)) {
        /* SMI, double, bool, null, undefined are not instances of any class */
        return false;
    }

    vtx_heap_object_t *obj = (vtx_heap_object_t *)vtx_heap_ptr(obj_value);
    vtx_typeid_t obj_typeid = obj->type_id;

    return vtx_type_is_instance(ts, obj_typeid, expected_typeid);
}

/* ========================================================================== */
/* Null and bounds checks                                                      */
/* ========================================================================== */

bool vtx_helpers_null_check(vtx_value_t value)
{
    if (vtx_is_null(value)) {
        /* Trap: null pointer dereference */
        VTX_ASSERT(false, "null pointer dereference");
        abort();
    }
    return true;
}

bool vtx_helpers_bounds_check(int64_t index, int64_t length)
{
    if (index < 0 || index >= length) {
        /* Out of bounds: return false so the calling guard can deoptimize.
         * Do NOT abort — this is a runtime helper that JIT code calls;
         * the result determines whether a guard fails, not whether the
         * process should crash. */
        return false;
    }
    return true;
}

/* ========================================================================== */
/* Overflow checks                                                             */
/* ========================================================================== */

bool vtx_helpers_overflow_check_iadd(int64_t a, int64_t b)
{
    /* a + b overflows iff:
     *   a > 0 and b > 0 and a + b < 0, or
     *   a < 0 and b < 0 and a + b > 0 */
    if (b > 0 && a > INT64_MAX - b) {
        return false; /* positive overflow */
    }
    if (b < 0 && a < INT64_MIN - b) {
        return false; /* negative overflow */
    }
    return true;
}

bool vtx_helpers_overflow_check_imul(int64_t a, int64_t b)
{
    /* Special case: either operand is 0 → no overflow */
    if (a == 0 || b == 0) {
        return true;
    }

    /* Check: |a * b| > INT64_MAX → overflow */
    /* Use the division test: a * b overflows iff a != 0 and b != 0 and
     * (a > INT64_MAX / b or a < INT64_MIN / b) depending on signs */
    if (a > 0) {
        if (b > 0) {
            if (a > INT64_MAX / b) return false;
        } else {
            if (b < INT64_MIN / a) return false;
        }
    } else {
        if (b > 0) {
            if (a < INT64_MIN / b) return false;
        } else {
            if (a < INT64_MAX / b) return false;
        }
    }
    return true;
}

/* ========================================================================== */
/* Virtual dispatch resolution                                                 */
/* ========================================================================== */

const vtx_method_desc_t *vtx_helpers_resolve_virtual(vtx_type_system_t *ts,
                                                      vtx_inline_cache_t *ic,
                                                      vtx_value_t obj_value,
                                                      const char *method_name)
{
    VTX_ASSERT(ts != NULL, "type system must not be NULL");
    VTX_ASSERT(ic != NULL, "inline cache must not be NULL");
    VTX_ASSERT(method_name != NULL, "method name must not be NULL");

    if (!vtx_is_heap_ptr(obj_value)) {
        return NULL;
    }

    vtx_heap_object_t *obj = (vtx_heap_object_t *)vtx_heap_ptr(obj_value);
    vtx_typeid_t typeid_ = obj->type_id;

    /* Try IC lookup first */
    const vtx_method_desc_t *method = vtx_ic_lookup(ic, typeid_);
    if (method != NULL) {
        return method;
    }

    /* IC miss: full vtable walk */
    method = vtx_type_resolve_method(ts, typeid_, method_name);
    if (method != NULL) {
        /* Update the IC */
        vtx_ic_update(ic, typeid_, method);
    }

    return method;
}

const vtx_method_desc_t *vtx_helpers_resolve_interface(vtx_type_system_t *ts,
                                                        vtx_inline_cache_t *ic,
                                                        vtx_value_t obj_value,
                                                        vtx_typeid_t interface_typeid,
                                                        const char *method_name)
{
    VTX_ASSERT(ts != NULL, "type system must not be NULL");
    VTX_ASSERT(ic != NULL, "inline cache must not be NULL");
    VTX_ASSERT(method_name != NULL, "method name must not be NULL");

    if (!vtx_is_heap_ptr(obj_value)) {
        return NULL;
    }

    vtx_heap_object_t *obj = (vtx_heap_object_t *)vtx_heap_ptr(obj_value);
    vtx_typeid_t typeid_ = obj->type_id;

    /* Try IC lookup first */
    const vtx_method_desc_t *method = vtx_ic_lookup(ic, typeid_);
    if (method != NULL) {
        return method;
    }

    /* IC miss: check that the object's type implements the interface,
     * then resolve the method */
    if (!vtx_type_is_instance(ts, typeid_, interface_typeid)) {
        return NULL;
    }

    method = vtx_type_resolve_method(ts, typeid_, method_name);
    if (method != NULL) {
        /* Update the IC */
        vtx_ic_update(ic, typeid_, method);
    }

    return method;
}

/* ========================================================================== */
/* String helpers                                                              */
/* ========================================================================== */

/**
 * String object layout:
 *   field[0] = length (SMI)
 *   field[1..] = character data stored as raw bytes packed into vtx_value_t slots
 *
 * For a simpler implementation, we store a null-terminated C string
 * directly in the fields starting at index 1. The string data occupies
 * ceil(strlen+1 / 8) fields.
 */

const char *vtx_helpers_string_data(vtx_value_t str_value)
{
    if (!vtx_is_heap_ptr(str_value)) {
        return "";
    }

    vtx_heap_object_t *obj = (vtx_heap_object_t *)vtx_heap_ptr(str_value);

    if (obj->field_count < 2) {
        return "";
    }

    /* The string data starts at field index 1, stored as raw bytes */
    return (const char *)&obj->fields[1];
}

uint32_t vtx_helpers_string_length(vtx_value_t str_value)
{
    if (!vtx_is_heap_ptr(str_value)) {
        return 0;
    }

    vtx_heap_object_t *obj = (vtx_heap_object_t *)vtx_heap_ptr(str_value);

    if (obj->field_count < 1) {
        return 0;
    }

    /* Field 0 stores the length as an SMI */
    vtx_value_t len_val = obj->fields[0];
    if (vtx_is_smi(len_val)) {
        int64_t len = vtx_smi_value(len_val);
        return (uint32_t)(len >= 0 ? len : 0);
    }

    return 0;
}

int vtx_helpers_string_compare(vtx_value_t a, vtx_value_t b)
{
    const char *str_a = vtx_helpers_string_data(a);
    const char *str_b = vtx_helpers_string_data(b);

    return strcmp(str_a, str_b);
}

/* ========================================================================== */
/* GC write barrier                                                            */
/* ========================================================================== */

/**
 * GC write barrier: must be called after storing a reference to an object.
 *
 * Implements a card-table based write barrier for generational GC.
 * Divides the heap into 512-byte cards (VTX_CARD_SIZE). Each card has
 * a 1-byte entry in the card table. On write barrier invocation, computes
 * which card the field [obj + field_offset] falls in and marks it dirty (0xFF)
 * so the GC can find cross-generational references during young-gen collection.
 *
 * This function is designed to be called from JIT-compiled code with two
 * arguments per the System V AMD64 ABI:
 *   RDI = obj (pointer to the heap object)
 *   ESI = field_offset (byte offset of the field within the object)
 */
void vtx_helpers_write_barrier(void *obj, uint32_t field_offset)
{
    if (obj == NULL) return;

    /* Get the current GC instance (defined in runtime/gc.c, declared in gc.h) */
    vtx_gc_t *gc = vtx_get_current_gc();
    if (gc == NULL) return;

    /* Only needed in generational mode */
    if (!vtx_gc_mode_needs_barrier(gc->mode)) return;

    /* Compute the address of the field that was written */
    uintptr_t field_addr = (uintptr_t)((uint8_t *)obj + field_offset);

    /* Card-table write barrier:
     * 1. Compute card index from field address: (field_addr - heap_base) >> VTX_CARD_SHIFT
     * 2. Mark the card as dirty: card_table[index] = 0xFF
     *
     * We use 0xFF as the dirty marker (instead of VTX_CARD_DIRTY = 0x01) to
     * allow the GC to use the card byte for additional metadata in the lower
     * bits when not dirty. This matches the HotSpot JVM convention. */
    if (gc->card_table != NULL && gc->heap_base != NULL) {
        uintptr_t offset = field_addr - (uintptr_t)gc->heap_base;
        if (offset < gc->heap_size) {
            size_t card_index = offset >> VTX_CARD_SHIFT;
            if (card_index < gc->card_table_size) {
                gc->card_table[card_index] = 0xFF;
            }
        }
    }

    /* Also maintain the remembered set for correctness.
     * This ensures old→young references are tracked even if the
     * card-table scanning path has a bug, and provides a complete
     * list for the remembered-set rebuild after collection. */
    vtx_heap_object_t *heap_obj = (vtx_heap_object_t *)obj;
    if (vtx_gc_in_old(gc, obj) && !heap_obj->gc_remembered) {
        /* Check if the stored value might be a young-gen pointer.
         * We conservatively assume it might be, since we don't have
         * the value here — the JIT emits this barrier unconditionally
         * for reference stores. The GC will scan the object's fields
         * during collection to find actual young-gen pointers. */
        if (gc->remembered_count < gc->remembered_capacity) {
            gc->remembered_set[gc->remembered_count].obj = heap_obj;
            gc->remembered_count++;
            heap_obj->gc_remembered = 1;
        }
    }
}

/* ========================================================================== */
/* D8: Register-based call helpers                                             */
/* ========================================================================== */

/**
 * Register-based call: static method dispatch.
 *
 * This function is the runtime entry point for JIT-compiled CALL_STATIC
 * instructions. Instead of using variadic argument marshaling (which
 * requires pushing each argument as a va_arg and then unpacking them
 * in the callee), the JIT codegen places arguments directly into the
 * System V AMD64 ABI registers.
 *
 * The JIT-compiled code at the call site:
 *   1. Moves the interp pointer → RDI
 *   2. Moves the method descriptor pointer → RSI
 *   3. Moves arg[0] → RDX, arg[1] → RCX, arg[2] → R8, arg[3] → R9
 *   4. Pushes remaining args onto the stack (if arg_count > 4)
 *   5. Calls this function
 *
 * Inside this function, we use the interpreter's vtx_interp_run() to
 * execute the method. The args array is already in the right order.
 */
vtx_value_t vtx_runtime_call_reg(void *interp,
                                   const vtx_method_desc_t *method,
                                   vtx_value_t *args,
                                   uint32_t arg_count)
{
    vtx_interp_t *interp_ptr = (vtx_interp_t *)interp;
    VTX_ASSERT(interp_ptr != NULL, "interp must not be NULL");
    VTX_ASSERT(method != NULL, "method must not be NULL");

    if (method->bytecode == NULL) {
        return VTX_VALUE_UNDEFINED;
    }

    /* Record invocation in profiler */
    vtx_profiler_record_invocation(&interp_ptr->profiler, method);

    /* Delegate to the interpreter's run function which handles
     * frame creation, argument copying, and the dispatch loop.
     *
     * The key difference from the variadic path: the args are already
     * in a contiguous array in the correct order. No va_arg unpacking
     * needed. The JIT codegen placed them there directly from the
     * expression stack registers. */
    return vtx_interp_run(interp_ptr, method, args, arg_count);
}

/**
 * Register-based call: virtual method dispatch.
 *
 * Resolves the virtual method based on the receiver's type, then
 * calls it with register-based argument passing.
 */
vtx_value_t vtx_runtime_call_virtual_reg(void *interp,
                                           const char *method_name,
                                           vtx_value_t receiver,
                                           vtx_value_t *args,
                                           uint32_t arg_count)
{
    vtx_interp_t *interp_ptr = (vtx_interp_t *)interp;
    VTX_ASSERT(interp_ptr != NULL, "interp must not be NULL");
    VTX_ASSERT(method_name != NULL, "method_name must not be NULL");

    /* Resolve the virtual method based on the receiver's type */
    const vtx_method_desc_t *target_method = NULL;

    if (vtx_is_heap_ptr(receiver)) {
        vtx_heap_object_t *obj = (vtx_heap_object_t *)vtx_heap_ptr(receiver);
        vtx_typeid_t typeid_ = obj->type_id;
        target_method = vtx_type_resolve_method(interp_ptr->type_system, typeid_, method_name);
    }

    if (target_method == NULL || target_method->bytecode == NULL) {
        return VTX_VALUE_UNDEFINED;
    }

    /* Build the full args array: receiver + remaining args.
     * The receiver is always the first argument (local[0]) in the callee. */
    uint32_t total_arg_count = arg_count + 1; /* +1 for receiver */
    vtx_value_t *full_args = (vtx_value_t *)alloca(total_arg_count * sizeof(vtx_value_t));
    full_args[0] = receiver;
    if (arg_count > 0 && args != NULL) {
        memcpy(full_args + 1, args, arg_count * sizeof(vtx_value_t));
    }

    /* Record invocation */
    vtx_profiler_record_invocation(&interp_ptr->profiler, target_method);

    return vtx_interp_run(interp_ptr, target_method, full_args, total_arg_count);
}

/**
 * Register-based call: interface method dispatch.
 *
 * Resolves the interface method based on the receiver's type,
 * verifies that the receiver's type implements the interface,
 * then calls it with register-based argument passing.
 */
vtx_value_t vtx_runtime_call_interface_reg(void *interp,
                                             vtx_typeid_t interface_typeid,
                                             const char *method_name,
                                             vtx_value_t receiver,
                                             vtx_value_t *args,
                                             uint32_t arg_count)
{
    vtx_interp_t *interp_ptr = (vtx_interp_t *)interp;
    VTX_ASSERT(interp_ptr != NULL, "interp must not be NULL");
    VTX_ASSERT(method_name != NULL, "method_name must not be NULL");

    /* Resolve the interface method */
    const vtx_method_desc_t *target_method = NULL;

    if (vtx_is_heap_ptr(receiver)) {
        vtx_heap_object_t *obj = (vtx_heap_object_t *)vtx_heap_ptr(receiver);
        vtx_typeid_t typeid_ = obj->type_id;

        /* Verify the receiver implements the interface */
        if (vtx_type_is_instance(interp_ptr->type_system, typeid_, interface_typeid)) {
            target_method = vtx_type_resolve_method(interp_ptr->type_system, typeid_, method_name);
        }
    }

    if (target_method == NULL || target_method->bytecode == NULL) {
        return VTX_VALUE_UNDEFINED;
    }

    /* Build the full args array: receiver + remaining args */
    uint32_t total_arg_count = arg_count + 1;
    vtx_value_t *full_args = (vtx_value_t *)alloca(total_arg_count * sizeof(vtx_value_t));
    full_args[0] = receiver;
    if (arg_count > 0 && args != NULL) {
        memcpy(full_args + 1, args, arg_count * sizeof(vtx_value_t));
    }

    /* Record invocation */
    vtx_profiler_record_invocation(&interp_ptr->profiler, target_method);

    return vtx_interp_run(interp_ptr, target_method, full_args, total_arg_count);
}
