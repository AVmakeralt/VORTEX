#ifndef VORTEX_TYPE_SYSTEM_H
#define VORTEX_TYPE_SYSTEM_H

#include <stdint.h>
#include <stdbool.h>
#include "vortex_config.h"
#include "runtime/bytecode.h"

/**
 * VORTEX Type System
 *
 * Types are represented as TypeIDs that index into a type descriptor table.
 * Type descriptors contain: name, field layout, method table, parent type,
 * implemented interfaces, and shape ID.
 *
 * Type identity is structural for interfaces, nominal for classes.
 * Inline caches support monomorphic, polymorphic (up to VTX_POLY_LIMIT
 * types per site), and megamorphic transitions.
 */

/* ========================================================================== */
/* Type and shape IDs                                                          */
/* ========================================================================== */

typedef uint32_t vtx_typeid_t;
typedef uint32_t vtx_shapeid_t;

#define VTX_TYPE_INVALID 0
#define VTX_TYPE_OBJECT  1
/* First user-defined type starts at 2 */

#define VTX_SHAPE_INVALID 0
#define VTX_SHAPE_OBJECT  1

/* ========================================================================== */
/* Field and method descriptors                                                */
/* ========================================================================== */

typedef struct {
    const char   *name;      /* field name (non-owning pointer) */
    vtx_typeid_t  type;      /* field type ID */
    uint32_t      offset;    /* byte offset within the object (computed) */
} vtx_field_desc_t;

typedef struct {
    const char       *name;          /* method name */
    const char       *signature;     /* method signature string (e.g., "(II)I") */
    vtx_bytecode_t   *bytecode;      /* method bytecode (may be NULL for native) */
    void             *compiled_code; /* JIT-compiled native code (NULL if not compiled) */
    uint32_t          vtable_index;  /* index in the vtable (0xFFFFFFFF if not virtual) */
    bool              is_virtual;    /* true if this is a virtual method */
} vtx_method_desc_t;

/* ========================================================================== */
/* Type descriptor                                                             */
/* ========================================================================== */

typedef struct vtx_type_desc vtx_type_desc_t;

struct vtx_type_desc {
    const char          *name;           /* type name */
    vtx_typeid_t         type_id;        /* this type's ID */
    vtx_typeid_t         parent_type;    /* parent type ID (VTX_TYPE_INVALID if none) */
    uint32_t             field_count;    /* number of fields */
    vtx_field_desc_t    *fields;         /* field descriptors (owned) */
    uint32_t             method_count;   /* number of methods */
    vtx_method_desc_t   *methods;        /* method descriptors (owned) */
    vtx_shapeid_t        shape_id;       /* layout fingerprint */
    uint32_t             instance_size;  /* total instance size in bytes */
    void               **vtable;         /* virtual method table (function pointers) */
    uint32_t             vtable_size;    /* number of vtable entries */
    uint32_t             interface_count;/* number of implemented interfaces */
    vtx_typeid_t        *interfaces;     /* interface type IDs (owned) */
};

/* ========================================================================== */
/* Type system                                                                 */
/* ========================================================================== */

#define VTX_TYPE_SYSTEM_INITIAL_CAPACITY 64

typedef struct {
    vtx_type_desc_t  *types;       /* array of type descriptors, indexed by typeid */
    uint32_t          type_count;  /* number of registered types */
    uint32_t          capacity;    /* capacity of the types array */
    vtx_shapeid_t     shape_counter; /* next shape ID to assign */
} vtx_type_system_t;

/**
 * Initialize the type system with the root Object type pre-registered.
 * Returns 0 on success, -1 on failure.
 */
int vtx_type_system_init(vtx_type_system_t *ts);

/**
 * Destroy the type system and free all allocated memory.
 */
void vtx_type_system_destroy(vtx_type_system_t *ts);

/**
 * Register a new type. Returns the new TypeID on success,
 * VTX_TYPE_INVALID on failure.
 *
 * The type system takes ownership of the fields and methods arrays
 * (but NOT the strings they point to — those must outlive the type system).
 */
vtx_typeid_t vtx_type_register(vtx_type_system_t *ts,
                                const char *name,
                                vtx_typeid_t parent_id,
                                uint32_t field_count,
                                vtx_field_desc_t *fields,
                                uint32_t method_count,
                                vtx_method_desc_t *methods);

/**
 * Get a type descriptor by ID. Returns NULL if the ID is invalid.
 */
const vtx_type_desc_t *vtx_type_get(const vtx_type_system_t *ts, vtx_typeid_t id);

/**
 * Check if child_id is a subtype of parent_id by walking the parent chain.
 * A type is considered a subtype of itself.
 */
bool vtx_type_is_subtype(const vtx_type_system_t *ts,
                         vtx_typeid_t child_id,
                         vtx_typeid_t parent_id);

/**
 * Check if obj_typeid is an instance of target_typeid.
 * This includes subtype checking AND interface implementation checking.
 */
bool vtx_type_is_instance(const vtx_type_system_t *ts,
                          vtx_typeid_t obj_typeid,
                          vtx_typeid_t target_typeid);

/**
 * Resolve a method by name for a given type.
 * For virtual methods, walks the vtable. For non-virtual, looks up directly.
 * Returns NULL if not found.
 */
const vtx_method_desc_t *vtx_type_resolve_method(const vtx_type_system_t *ts,
                                                  vtx_typeid_t typeid,
                                                  const char *method_name);

/**
 * Compute the instance size for a type based on its field layout
 * with alignment rules. The instance size includes the heap object header.
 */
uint32_t vtx_type_instance_size(vtx_type_system_t *ts, vtx_typeid_t typeid);

/**
 * Compute a shape ID for the type. The shape is a layout fingerprint based
 * on field offsets and types. Two types with the same shape have compatible
 * field layouts and can share inline cache entries.
 */
vtx_shapeid_t vtx_type_compute_shape(vtx_type_system_t *ts, vtx_typeid_t typeid);

/**
 * Add an interface to a type's implemented interfaces list.
 * Returns 0 on success, -1 on failure.
 */
int vtx_type_add_interface(vtx_type_system_t *ts,
                           vtx_typeid_t impl_type,
                           vtx_typeid_t interface_type);

/* ========================================================================== */
/* Inline cache                                                                */
/* ========================================================================== */

typedef enum {
    VT_IC_MONOMORPHIC  = 0,  /* one type observed */
    VT_IC_POLYMORPHIC  = 1,  /* 2..VTX_POLY_LIMIT types observed */
    VT_IC_MEGAMORPHIC  = 2   /* too many types, fallback to vtable */
} vtx_ic_state_t;

typedef struct {
    vtx_typeid_t           typeid_;
    const vtx_method_desc_t *method;
} vtx_ic_entry_t;

typedef struct {
    vtx_ic_state_t  state;
    vtx_ic_entry_t  entries[VTX_POLY_LIMIT + 1]; /* +1 for megamorphic sentinel */
    uint32_t        count;
} vtx_inline_cache_t;

/**
 * Initialize an inline cache to empty monomorphic state.
 */
void vtx_ic_init(vtx_inline_cache_t *ic);

/**
 * Look up a method in the inline cache for the given typeid.
 * Returns the method pointer if found, NULL otherwise.
 */
const vtx_method_desc_t *vtx_ic_lookup(const vtx_inline_cache_t *ic,
                                        vtx_typeid_t typeid_);

/**
 * Update the inline cache with a new typeid → method mapping.
 * Transitions the state: monomorphic → polymorphic → megamorphic.
 */
void vtx_ic_update(vtx_inline_cache_t *ic,
                   vtx_typeid_t typeid_,
                   const vtx_method_desc_t *method);

#endif /* VORTEX_TYPE_SYSTEM_H */
