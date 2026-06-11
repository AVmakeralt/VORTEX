#ifndef VORTEX_LOOKUP_H
#define VORTEX_LOOKUP_H

#include "vortex_config.h"
#include <stdint.h>
#include "runtime/object.h"
#include "runtime/bytecode.h"
#include "runtime/type_system.h"

/**
 * VORTEX Method Lookup with Inline Caches
 *
 * Provides virtual and interface method dispatch using inline caches.
 * The inline cache per call site stores:
 *   - Monomorphic: one typeid → method mapping (fastest)
 *   - Polymorphic: up to VTX_POLY_LIMIT mappings (linear scan)
 *   - Megamorphic: fallback to vtable walk
 *
 * The IC is stored in the profile data (not in the bytecode), so
 * it can be shared across tiers and invalidated independently.
 */

/* ========================================================================== */
/* Method lookup functions                                                     */
/* ========================================================================== */

/**
 * Look up a virtual method using an inline cache.
 *
 * Algorithm:
 *   1. If receiver is a heap pointer, extract its typeid
 *   2. Check the IC for a cached mapping (monomorphic fast path)
 *   3. On miss, do a full vtable walk via vtx_type_resolve_method
 *   4. Update the IC with the new mapping
 *
 * The IC transitions through states:
 *   - Monomorphic (1 entry): direct match on typeid
 *   - Polymorphic (2..VTX_POLY_LIMIT entries): linear scan
 *   - Megamorphic (>VTX_POLY_LIMIT types): vtable fallback, no IC update
 *
 * Returns the resolved method descriptor, or NULL if not found.
 */
const vtx_method_desc_t *vtx_lookup_method(vtx_type_system_t *ts,
                                            vtx_inline_cache_t *ic,
                                            vtx_value_t receiver,
                                            const char *method_name);

/**
 * Look up an interface method using an inline cache.
 *
 * Algorithm:
 *   1. If receiver is a heap pointer, extract its typeid
 *   2. Check the IC for a cached mapping
 *   3. On miss, verify that the receiver's type implements the interface
 *   4. Walk the type hierarchy to find the method
 *   5. Update the IC
 *
 * Returns the resolved method descriptor, or NULL if not found.
 */
const vtx_method_desc_t *vtx_lookup_interface_method(vtx_type_system_t *ts,
                                                      vtx_inline_cache_t *ic,
                                                      vtx_value_t receiver,
                                                      vtx_typeid_t interface_typeid,
                                                      const char *method_name);

/**
 * Look up a static method by name from a given type.
 * No IC is used for static calls (the target is always the same).
 *
 * Returns the resolved method descriptor, or NULL if not found.
 */
const vtx_method_desc_t *vtx_lookup_static_method(vtx_type_system_t *ts,
                                                    vtx_typeid_t typeid_,
                                                    const char *method_name);

#endif /* VORTEX_LOOKUP_H */
