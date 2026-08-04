#include "interp/lookup.h"

/* ========================================================================== */
/* Virtual method lookup                                                       */
/* ========================================================================== */

const vtx_method_desc_t *vtx_lookup_method(vtx_type_system_t *ts,
                                            vtx_inline_cache_t *ic,
                                            vtx_value_t receiver,
                                            const char *method_name)
{
    VTX_ASSERT(ts != NULL, "type system must not be NULL");
    VTX_ASSERT(ic != NULL, "inline cache must not be NULL");
    VTX_ASSERT(method_name != NULL, "method name must not be NULL");

    /* If the receiver is not a heap pointer, we can't do virtual dispatch */
    if (!vtx_is_heap_ptr(receiver)) {
        return NULL;
    }

    /* Extract the receiver's type ID */
    vtx_heap_object_t *obj = (vtx_heap_object_t *)vtx_heap_ptr(receiver);
    vtx_typeid_t typeid_ = obj->type_id;

    /* Step 1: Check the inline cache (fast path).
     * The IC handles monomorphic, polymorphic, and megamorphic cases.
     * For monomorphic, this is a single comparison.
     * For polymorphic, this is a linear scan of up to VTX_POLY_LIMIT entries.
     * For megamorphic, vtx_ic_lookup returns NULL and we fall through
     * to the vtable walk. */
    const vtx_method_desc_t *method = vtx_ic_lookup(ic, typeid_);
    if (method != NULL) {
        return method;
    }

    /* Step 2: IC miss — do a full vtable walk */
    method = vtx_type_resolve_method(ts, typeid_, method_name);
    if (method != NULL) {
        /* Step 3: Update the IC with the new mapping.
         * vtx_ic_update handles the transition:
         *   - Monomorphic → adds first entry
         *   - Polymorphic → adds entry (up to VTX_POLY_LIMIT)
         *   - Megamorphic → marks as megamorphic (no more entries added) */
        vtx_ic_update(ic, typeid_, method);
    }

    return method;
}

/* ========================================================================== */
/* Interface method lookup                                                     */
/* ========================================================================== */

const vtx_method_desc_t *vtx_lookup_interface_method(vtx_type_system_t *ts,
                                                      vtx_inline_cache_t *ic,
                                                      vtx_value_t receiver,
                                                      vtx_typeid_t interface_typeid,
                                                      const char *method_name)
{
    VTX_ASSERT(ts != NULL, "type system must not be NULL");
    VTX_ASSERT(ic != NULL, "inline cache must not be NULL");
    VTX_ASSERT(method_name != NULL, "method name must not be NULL");

    /* If the receiver is not a heap pointer, we can't do interface dispatch */
    if (!vtx_is_heap_ptr(receiver)) {
        return NULL;
    }

    /* Extract the receiver's type ID */
    vtx_heap_object_t *obj = (vtx_heap_object_t *)vtx_heap_ptr(receiver);
    vtx_typeid_t typeid_ = obj->type_id;

    /* Step 1: Check the inline cache (fast path) */
    const vtx_method_desc_t *method = vtx_ic_lookup(ic, typeid_);
    if (method != NULL) {
        return method;
    }

    /* Step 2: IC miss — verify that the receiver implements the interface.
     * We check both subtype and interface implementation. */
    if (!vtx_type_is_instance(ts, typeid_, interface_typeid)) {
        return NULL;
    }

    /* Step 3: Walk the type hierarchy to find the method */
    method = vtx_type_resolve_method(ts, typeid_, method_name);
    if (method != NULL) {
        /* Step 4: Update the IC with the new mapping */
        vtx_ic_update(ic, typeid_, method);
    }

    return method;
}

/* ========================================================================== */
/* Static method lookup                                                        */
/* ========================================================================== */

const vtx_method_desc_t *vtx_lookup_static_method(vtx_type_system_t *ts,
                                                    vtx_typeid_t typeid_,
                                                    const char *method_name)
{
    VTX_ASSERT(ts != NULL, "type system must not be NULL");
    VTX_ASSERT(method_name != NULL, "method name must not be NULL");

    /* Static calls don't need an IC — the target is always the same.
     * We just resolve the method by name from the declaring type. */
    return vtx_type_resolve_method(ts, typeid_, method_name);
}
