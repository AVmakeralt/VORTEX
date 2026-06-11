#include "runtime/object.h"

#include <string.h>

void vtx_heap_object_init(vtx_heap_object_t *obj, uint32_t type_id,
                          uint32_t shape_id, uint32_t field_count, uint32_t total_size)
{
    VTX_ASSERT(obj != NULL, "object must not be NULL");

    obj->type_id       = type_id;
    obj->gc_mark       = 0;
    obj->gc_age        = 0;
    obj->gc_pinned     = 0;
    obj->gc_remembered = 0;
    obj->size          = total_size;
    obj->shape_id      = shape_id;
    obj->field_count   = field_count;

    /* Zero-initialize all fields to VTX_VALUE_UNDEFINED */
    for (uint32_t i = 0; i < field_count; i++) {
        obj->fields[i] = vtx_make_undefined();
    }
}

uint32_t vtx_value_typeid(vtx_value_t v)
{
    if (!vtx_is_heap_ptr(v)) {
        return 0; /* VTX_TYPE_INVALID — not a heap object */
    }
    vtx_heap_object_t *obj = (vtx_heap_object_t *)vtx_heap_ptr(v);
    return obj->type_id;
}
