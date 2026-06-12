#include "runtime/type_system.h"

#include <stdlib.h>
#include <string.h>
#include <stddef.h>

/* ========================================================================== */
/* Global type system instance (used by interpreter and runtime stubs)          */
/* ========================================================================== */

static vtx_type_system_t *the_type_system = NULL;

vtx_type_system_t *vtx_get_current_type_system(void) { return the_type_system; }
void vtx_set_current_type_system(vtx_type_system_t *ts) { the_type_system = ts; }

/* ========================================================================== */
/* Type system init/destroy                                                    */
/* ========================================================================== */

int vtx_type_system_init(vtx_type_system_t *ts)
{
    VTX_ASSERT(ts != NULL, "type system must not be NULL");

    ts->capacity = VTX_TYPE_SYSTEM_INITIAL_CAPACITY;
    ts->types = (vtx_type_desc_t *)calloc(ts->capacity, sizeof(vtx_type_desc_t));
    if (ts->types == NULL) {
        ts->type_count = 0;
        ts->shape_counter = 0;
        return -1;
    }

    /* Slot 0 = VTX_TYPE_INVALID (leave zeroed) */
    ts->type_count = 1;
    ts->shape_counter = 2; /* 0=invalid, 1=Object */

    /* Register the root Object type at index 1 */
    vtx_type_desc_t *obj = &ts->types[VTX_TYPE_OBJECT];
    obj->name            = "Object";
    obj->type_id         = VTX_TYPE_OBJECT;
    obj->parent_type     = VTX_TYPE_INVALID;
    obj->field_count     = 0;
    obj->fields          = NULL;
    obj->method_count    = 0;
    obj->methods         = NULL;
    obj->shape_id        = VTX_SHAPE_OBJECT;
    obj->instance_size   = (uint32_t)VTX_HEAP_OBJECT_HEADER_SIZE; /* header only, no fields */
    obj->vtable          = NULL;
    obj->vtable_size     = 0;
    obj->interface_count = 0;
    obj->interfaces      = NULL;

    ts->type_count = 2; /* VTX_TYPE_INVALID (0) + VTX_TYPE_OBJECT (1) */

    return 0;
}

void vtx_type_system_destroy(vtx_type_system_t *ts)
{
    VTX_ASSERT(ts != NULL, "type system must not be NULL");

    for (uint32_t i = 0; i < ts->type_count; i++) {
        vtx_type_desc_t *td = &ts->types[i];
        if (td->fields != NULL) {
            free(td->fields);
            td->fields = NULL;
        }
        if (td->methods != NULL) {
            free(td->methods);
            td->methods = NULL;
        }
        if (td->vtable != NULL) {
            free(td->vtable);
            td->vtable = NULL;
        }
        if (td->interfaces != NULL) {
            free(td->interfaces);
            td->interfaces = NULL;
        }
    }

    free(ts->types);
    ts->types = NULL;
    ts->type_count = 0;
    ts->capacity = 0;
    ts->shape_counter = 0;
}

/* ========================================================================== */
/* Type registration                                                          */
/* ========================================================================== */

/**
 * Grow the types array if needed to accommodate at least `needed` slots.
 * Returns 0 on success, -1 on failure.
 */
static int type_system_ensure_capacity(vtx_type_system_t *ts, uint32_t needed)
{
    if (needed <= ts->capacity) {
        return 0;
    }

    uint32_t new_capacity = ts->capacity;
    while (new_capacity < needed) {
        new_capacity *= 2;
    }

    vtx_type_desc_t *new_types = (vtx_type_desc_t *)realloc(
        ts->types, new_capacity * sizeof(vtx_type_desc_t));
    if (new_types == NULL) {
        return -1;
    }

    /* Zero-initialize new slots */
    memset(new_types + ts->capacity, 0,
           (new_capacity - ts->capacity) * sizeof(vtx_type_desc_t));

    ts->types = new_types;
    ts->capacity = new_capacity;
    return 0;
}

/**
 * Compute field offsets for a type based on alignment rules.
 * Fields are laid out in order, each aligned to 8 bytes (since all values
 * are vtx_value_t = uint64_t = 8 bytes). The object header size is
 * VTX_HEAP_OBJECT_HEADER_SIZE.
 */
static void compute_field_offsets(vtx_field_desc_t *fields, uint32_t field_count,
                                  uint32_t *out_instance_size)
{
    uint32_t offset = (uint32_t)VTX_HEAP_OBJECT_HEADER_SIZE;

    for (uint32_t i = 0; i < field_count; i++) {
        /* All fields are vtx_value_t (8 bytes), aligned to 8 bytes */
        offset = (offset + 7) & ~(uint32_t)7;
        fields[i].offset = offset;
        offset += sizeof(vtx_value_t); /* 8 bytes per field */
    }

    /* Final alignment to 8 bytes */
    *out_instance_size = (offset + 7) & ~(uint32_t)7;
}

vtx_typeid_t vtx_type_register(vtx_type_system_t *ts,
                                const char *name,
                                vtx_typeid_t parent_id,
                                uint32_t field_count,
                                vtx_field_desc_t *fields,
                                uint32_t method_count,
                                vtx_method_desc_t *methods)
{
    VTX_ASSERT(ts != NULL, "type system must not be NULL");
    VTX_ASSERT(name != NULL, "type name must not be NULL");
    VTX_ASSERT(parent_id < ts->type_count || parent_id == VTX_TYPE_INVALID,
               "parent type must be valid or INVALID");

    /* Ensure we have space for the new type */
    if (type_system_ensure_capacity(ts, ts->type_count + 1) != 0) {
        return VTX_TYPE_INVALID;
    }

    vtx_typeid_t new_id = ts->type_count;
    vtx_type_desc_t *td = &ts->types[new_id];

    td->name            = name;
    td->type_id         = new_id;
    td->parent_type     = parent_id;
    td->field_count     = field_count;
    td->fields          = fields;  /* ownership transfer */
    td->method_count    = method_count;
    td->methods         = methods; /* ownership transfer */
    td->vtable          = NULL;
    td->vtable_size     = 0;
    td->interface_count = 0;
    td->interfaces      = NULL;
    td->shape_id        = VTX_SHAPE_INVALID; /* computed later */

    /* Ensure compiled_code is initialized to NULL for all methods */
    for (uint32_t i = 0; i < method_count; i++) {
        methods[i].compiled_code = NULL;
    }

    /* Compute field offsets and instance size */
    if (fields != NULL && field_count > 0) {
        compute_field_offsets(fields, field_count, &td->instance_size);
    } else {
        td->instance_size = (uint32_t)VTX_HEAP_OBJECT_HEADER_SIZE;
    }

    /* Compute vtable: inherit parent's vtable, then add our virtual methods */
    /* Count virtual methods from parent chain */
    uint32_t parent_vtable_size = 0;
    if (parent_id != VTX_TYPE_INVALID && parent_id < ts->type_count) {
        const vtx_type_desc_t *parent = &ts->types[parent_id];
        parent_vtable_size = parent->vtable_size;
    }

    /* Count our own virtual methods */
    uint32_t own_virtual_count = 0;
    for (uint32_t i = 0; i < method_count; i++) {
        if (methods[i].is_virtual) {
            own_virtual_count++;
        }
    }

    td->vtable_size = parent_vtable_size + own_virtual_count;

    if (td->vtable_size > 0) {
        td->vtable = (void **)calloc(td->vtable_size, sizeof(void *));
        if (td->vtable == NULL) {
            td->vtable_size = 0;
            return VTX_TYPE_INVALID;
        }

        /* Copy parent's vtable entries */
        if (parent_id != VTX_TYPE_INVALID && parent_id < ts->type_count) {
            const vtx_type_desc_t *parent = &ts->types[parent_id];
            if (parent->vtable != NULL) {
                memcpy(td->vtable, parent->vtable,
                       parent_vtable_size * sizeof(void *));
            }
        }

        /* Assign vtable indices to our virtual methods */
        uint32_t vtable_idx = parent_vtable_size;
        for (uint32_t i = 0; i < method_count; i++) {
            if (methods[i].is_virtual) {
                /* Check if this overrides a parent method */
                bool overridden = false;
                if (parent_id != VTX_TYPE_INVALID && parent_id < ts->type_count) {
                    const vtx_type_desc_t *parent = &ts->types[parent_id];
                    for (uint32_t j = 0; j < parent->method_count; j++) {
                        if (strcmp(parent->methods[j].name, methods[i].name) == 0 &&
                            parent->methods[j].is_virtual) {
                            /* Override: use the parent's vtable slot.
                             * RT-10 fix: set the vtable entry to the child's
                             * implementation (compiled_code), not NULL. If the
                             * method isn't compiled yet, we still set it to NULL
                             * and update it later when compilation happens. */
                            methods[i].vtable_index = parent->methods[j].vtable_index;
                            td->vtable[methods[i].vtable_index] = methods[i].compiled_code;
                            overridden = true;
                            break;
                        }
                    }
                }
                if (!overridden) {
                    methods[i].vtable_index = vtable_idx;
                    /* RT-13 fix: populate vtable with compiled_code */
                    td->vtable[vtable_idx] = methods[i].compiled_code;
                    vtable_idx++;
                }
            }
        }
    }

    /* Compute shape ID */
    td->shape_id = vtx_type_compute_shape(ts, new_id);

    ts->type_count++;
    return new_id;
}

/* ========================================================================== */
/* Type queries                                                                */
/* ========================================================================== */

const vtx_type_desc_t *vtx_type_get(const vtx_type_system_t *ts, vtx_typeid_t id)
{
    VTX_ASSERT(ts != NULL, "type system must not be NULL");
    if (id >= ts->type_count) {
        return NULL;
    }
    return &ts->types[id];
}

bool vtx_type_is_subtype(const vtx_type_system_t *ts,
                         vtx_typeid_t child_id,
                         vtx_typeid_t parent_id)
{
    VTX_ASSERT(ts != NULL, "type system must not be NULL");

    /* A type is a subtype of itself */
    if (child_id == parent_id) {
        return true;
    }

    /* Walk the parent chain */
    vtx_typeid_t current = child_id;
    while (current != VTX_TYPE_INVALID && current < ts->type_count) {
        const vtx_type_desc_t *td = &ts->types[current];
        if (td->parent_type == parent_id) {
            return true;
        }
        current = td->parent_type;
    }

    return false;
}

bool vtx_type_is_instance(const vtx_type_system_t *ts,
                          vtx_typeid_t obj_typeid,
                          vtx_typeid_t target_typeid)
{
    VTX_ASSERT(ts != NULL, "type system must not be NULL");

    /* Check class hierarchy (subtype) */
    if (vtx_type_is_subtype(ts, obj_typeid, target_typeid)) {
        return true;
    }

    /* Check if target is an interface and obj_typeid implements it */
    if (obj_typeid >= ts->type_count) {
        return false;
    }

    const vtx_type_desc_t *obj_td = &ts->types[obj_typeid];
    for (uint32_t i = 0; i < obj_td->interface_count; i++) {
        if (obj_td->interfaces[i] == target_typeid) {
            return true;
        }
        /* Check if the implemented interface is a subtype of the target interface */
        if (vtx_type_is_subtype(ts, obj_td->interfaces[i], target_typeid)) {
            return true;
        }
    }

    /* Walk parent chain checking interfaces */
    vtx_typeid_t current = obj_td->parent_type;
    while (current != VTX_TYPE_INVALID && current < ts->type_count) {
        const vtx_type_desc_t *parent_td = &ts->types[current];
        for (uint32_t i = 0; i < parent_td->interface_count; i++) {
            if (parent_td->interfaces[i] == target_typeid) {
                return true;
            }
            if (vtx_type_is_subtype(ts, parent_td->interfaces[i], target_typeid)) {
                return true;
            }
        }
        current = parent_td->parent_type;
    }

    return false;
}

const vtx_method_desc_t *vtx_type_resolve_method(const vtx_type_system_t *ts,
                                                  vtx_typeid_t typeid_,
                                                  const char *method_name)
{
    VTX_ASSERT(ts != NULL, "type system must not be NULL");
    VTX_ASSERT(method_name != NULL, "method name must not be NULL");

    if (typeid_ >= ts->type_count) {
        return NULL;
    }

    /* Walk the type hierarchy from the given type upward */
    vtx_typeid_t current = typeid_;
    while (current != VTX_TYPE_INVALID && current < ts->type_count) {
        const vtx_type_desc_t *td = &ts->types[current];
        for (uint32_t i = 0; i < td->method_count; i++) {
            if (strcmp(td->methods[i].name, method_name) == 0) {
                return &td->methods[i];
            }
        }
        current = td->parent_type;
    }

    return NULL;
}

uint32_t vtx_type_instance_size(vtx_type_system_t *ts, vtx_typeid_t typeid_)
{
    VTX_ASSERT(ts != NULL, "type system must not be NULL");
    if (typeid_ >= ts->type_count) {
        return 0;
    }
    return ts->types[typeid_].instance_size;
}

vtx_shapeid_t vtx_type_compute_shape(vtx_type_system_t *ts, vtx_typeid_t typeid_)
{
    VTX_ASSERT(ts != NULL, "type system must not be NULL");

    if (typeid_ >= ts->type_count) {
        return VTX_SHAPE_INVALID;
    }

    vtx_type_desc_t *td = &ts->types[typeid_];

    /* If already computed, return it */
    if (td->shape_id != VTX_SHAPE_INVALID) {
        return td->shape_id;
    }

    /* Compute shape as a fingerprint based on field offsets and types.
     * We use a simple hash combining field offsets and type IDs.
     * Two types with the same shape can share inline cache entries. */
    uint32_t hash = 2166136261u; /* FNV offset basis */

    /* Include the field count */
    hash ^= td->field_count;
    hash *= 16777619u; /* FNV prime */

    /* Include each field's offset and type */
    for (uint32_t i = 0; i < td->field_count; i++) {
        hash ^= td->fields[i].offset;
        hash *= 16777619u;
        hash ^= td->fields[i].type;
        hash *= 16777619u;
    }

    /* Ensure we don't return VTX_SHAPE_INVALID (0) or VTX_SHAPE_OBJECT (1) */
    vtx_shapeid_t shape = (vtx_shapeid_t)hash;
    if (shape <= VTX_SHAPE_OBJECT) {
        shape += VTX_SHAPE_OBJECT + 1;
    }

    td->shape_id = shape;
    return shape;
}

int vtx_type_add_interface(vtx_type_system_t *ts,
                           vtx_typeid_t impl_type,
                           vtx_typeid_t interface_type)
{
    VTX_ASSERT(ts != NULL, "type system must not be NULL");

    if (impl_type >= ts->type_count || interface_type >= ts->type_count) {
        return -1;
    }

    vtx_type_desc_t *td = &ts->types[impl_type];

    /* Check if already implemented */
    for (uint32_t i = 0; i < td->interface_count; i++) {
        if (td->interfaces[i] == interface_type) {
            return 0; /* already implemented */
        }
    }

    /* Grow the interfaces array */
    uint32_t new_count = td->interface_count + 1;
    vtx_typeid_t *new_interfaces = (vtx_typeid_t *)realloc(
        td->interfaces, new_count * sizeof(vtx_typeid_t));
    if (new_interfaces == NULL) {
        return -1;
    }

    new_interfaces[td->interface_count] = interface_type;
    td->interfaces = new_interfaces;
    td->interface_count = new_count;

    return 0;
}

/* ========================================================================== */
/* Vtable update                                                               */
/* ========================================================================== */

int vtx_type_update_vtable(vtx_type_system_t *ts,
                             vtx_typeid_t typeid,
                             const vtx_method_desc_t *method)
{
    VTX_ASSERT(ts != NULL, "type system must not be NULL");
    VTX_ASSERT(method != NULL, "method must not be NULL");

    if (typeid >= ts->type_count) return -1;

    vtx_type_desc_t *td = &ts->types[typeid];

    /* If this method is virtual and has a vtable index, update the entry */
    if (method->is_virtual && method->vtable_index != 0xFFFFFFFF &&
        method->vtable_index < td->vtable_size && td->vtable != NULL) {
        td->vtable[method->vtable_index] = method->compiled_code;
        return 0;
    }

    return -1;
}

/* ========================================================================== */
/* Inline cache                                                                */
/* ========================================================================== */

void vtx_ic_init(vtx_inline_cache_t *ic)
{
    VTX_ASSERT(ic != NULL, "inline cache must not be NULL");
    ic->state = VT_IC_MONOMORPHIC;
    ic->count = 0;
    memset(ic->entries, 0, sizeof(ic->entries));
}

const vtx_method_desc_t *vtx_ic_lookup(const vtx_inline_cache_t *ic,
                                        vtx_typeid_t typeid_)
{
    VTX_ASSERT(ic != NULL, "inline cache must not be NULL");

    /* Megamorphic: no fast path lookup, return NULL to trigger vtable walk */
    if (ic->state == VT_IC_MEGAMORPHIC) {
        return NULL;
    }

    /* Linear scan through entries */
    for (uint32_t i = 0; i < ic->count; i++) {
        if (ic->entries[i].typeid_ == typeid_) {
            return ic->entries[i].method;
        }
    }

    return NULL;
}

void vtx_ic_update(vtx_inline_cache_t *ic,
                   vtx_typeid_t typeid_,
                   const vtx_method_desc_t *method)
{
    VTX_ASSERT(ic != NULL, "inline cache must not be NULL");

    /* Already megamorphic — no updates */
    if (ic->state == VT_IC_MEGAMORPHIC) {
        return;
    }

    /* Check if this typeid is already in the cache */
    for (uint32_t i = 0; i < ic->count; i++) {
        if (ic->entries[i].typeid_ == typeid_) {
            /* Already cached, nothing to do */
            return;
        }
    }

    /* Check if we have room */
    if (ic->count < VTX_POLY_LIMIT) {
        /* Add the entry */
        ic->entries[ic->count].typeid_ = typeid_;
        ic->entries[ic->count].method  = method;
        ic->count++;

        /* Transition: monomorphic → polymorphic */
        if (ic->count > 1) {
            ic->state = VT_IC_POLYMORPHIC;
        }
    } else {
        /* Exceeded polymorphic limit → megamorphic */
        ic->state = VT_IC_MEGAMORPHIC;
        ic->count = VTX_POLY_LIMIT; /* keep existing entries for potential future use */
    }
}
