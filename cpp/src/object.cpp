// vortex/object.cpp — Implementation of Object/Array methods.

#include "vortex/object.hpp"
#include "vortex/runtime.hpp"
#include <cstring>
#include <cstdlib>

namespace vortex {

// Duplicate a string into heap-allocated memory.
// The caller owns the returned pointer (free with free()).
// NOTE: These strings are NOT GC-managed. They leak for the process
// lifetime. For production use, integrate with GC-allocated strings.
static char* dup_string(const char* s) {
    size_t len = std::strlen(s) + 1;
    char* copy = static_cast<char*>(std::malloc(len));
    if (copy) std::memcpy(copy, s, len);
    return copy;
}

Object Object::create(Runtime& rt, uint32_t max_fields) {
    // Allocate via the runtime's GC.
    // Field 0 is reserved for the prototype pointer.
    // User properties start at field 1.
    uint32_t total_fields = max_fields + 1;  // +1 for prototype slot
    size_t size = vtx_heap_object_alloc_size(total_fields);
    vtx_heap_object_t* obj = vtx_gc_alloc(rt.gc(), size, 0 /* type_id */);
    if (!obj) return Object(nullptr);

    // Initialize the header
    vtx_heap_object_init(obj, 0 /* type_id */, 1 /* root shape */,
                         total_fields, size);

    // Initialize all fields to undefined
    for (uint32_t i = 0; i < total_fields; i++) {
        obj->fields[i] = VTX_VALUE_UNDEFINED;
    }

    return Object(obj);
}

Value Object::get(Runtime& rt, uint32_t symbol_id) {
    if (!obj_) return Value::undefined();

    // Look up property in our own fields using the shape
    // The shape tells us the field offset for this symbol_id
    // For now, we do a linear scan of fields 1..field_count
    // (field 0 is the prototype)
    //
    // TODO: Use the ShapeTable for O(1) lookup. For now, we store
    // the symbol_id in the field itself (as the high bits of the
    // value) — this is a simple encoding that works for prototyping.
    //
    // Actually, let me use a simpler approach: store properties as
    // key-value pairs in consecutive fields. Field 0 = prototype,
    // field 1 = key1, field 2 = val1, field 3 = key2, field 4 = val2, ...
    //
    // This is O(n) per lookup but simple and correct. We'll optimize
    // with shapes later.

    // Walk own properties (field pairs: [symbol_id_as_smi, value])
    for (uint32_t i = FIRST_PROPERTY_OFFSET; i + 1 < obj_->field_count; i += 2) {
        Value key(vtx_object_get_field(obj_, i));
        if (key.is_int() && key.as_int() == symbol_id) {
            return Value(vtx_object_get_field(obj_, i + 1));
        }
    }

    // Not found on this object — walk the prototype chain
    Object proto = prototype();
    if (proto.valid()) {
        return proto.get(rt, symbol_id);
    }

    return Value::undefined();
}

Value Object::get(Runtime& rt, const std::string& name) {
    if (!obj_) return Value::undefined();
    // For now, use a simple string comparison approach.
    // Walk own properties, comparing the key string.
    for (uint32_t i = FIRST_PROPERTY_OFFSET; i + 1 < obj_->field_count; i += 2) {
        Value key(vtx_object_get_field(obj_, i));
        // The key is stored as... we need a way to store strings as keys.
        // For now, let's use a different encoding: store the property name
        // as a raw C string pointer in the field (heap_ptr tag).
        // This is NOT GC-safe but works for prototyping.
        if (key.is_object()) {
            const char* key_str = reinterpret_cast<const char*>(vtx_heap_ptr(key.raw()));
            if (key_str && name == key_str) {
                return Value(vtx_object_get_field(obj_, i + 1));
            }
        }
    }

    // Walk prototype chain
    Object proto = prototype();
    if (proto.valid()) {
        return proto.get(rt, name);
    }

    return Value::undefined();
}

void Object::set(Runtime& rt, const std::string& name, Value value) {
    if (!obj_) return;

    // Check if property already exists (update in place)
    for (uint32_t i = FIRST_PROPERTY_OFFSET; i + 1 < obj_->field_count; i += 2) {
        Value key(vtx_object_get_field(obj_, i));
        if (key.is_object()) {
            const char* key_str = reinterpret_cast<const char*>(vtx_heap_ptr(key.raw()));
            if (key_str && name == key_str) {
                vtx_object_set_field(obj_, i + 1, value.raw());
                return;
            }
        }
    }

    // Add new property — find a free slot
    for (uint32_t i = FIRST_PROPERTY_OFFSET; i + 1 < obj_->field_count; i += 2) {
        Value key(vtx_object_get_field(obj_, i));
        if (key.is_undefined()) {
            // Duplicate the string so it survives after the caller's
            // std::string is destroyed.
            // NOTE: The duplicate is NOT GC-managed. It leaks for the
            // process lifetime. For production use, integrate with
            // GC-allocated string objects.
            char* name_copy = dup_string(name.c_str());
            if (!name_copy) return;
            vtx_value_t key_val = vtx_make_heap_ptr(name_copy);
            vtx_object_set_field(obj_, i, key_val);
            vtx_object_set_field(obj_, i + 1, value.raw());
            return;
        }
    }
    // No free slot — object is full.
    // TODO: implement object growth via GC reallocation.
}

void Object::set(Runtime& rt, uint32_t symbol_id, Value value) {
    if (!obj_) return;
    // Check if property already exists
    for (uint32_t i = FIRST_PROPERTY_OFFSET; i + 1 < obj_->field_count; i += 2) {
        Value key(vtx_object_get_field(obj_, i));
        if (key.is_int() && key.as_int() == symbol_id) {
            vtx_object_set_field(obj_, i + 1, value.raw());
            return;
        }
    }
    // Add new property in first free slot
    for (uint32_t i = FIRST_PROPERTY_OFFSET; i + 1 < obj_->field_count; i += 2) {
        Value key(vtx_object_get_field(obj_, i));
        if (key.is_undefined()) {
            vtx_object_set_field(obj_, i, Value::smi(symbol_id).raw());
            vtx_object_set_field(obj_, i + 1, value.raw());
            return;
        }
    }
}

bool Object::has(Runtime& rt, const std::string& name) {
    if (!obj_) return false;
    for (uint32_t i = FIRST_PROPERTY_OFFSET; i + 1 < obj_->field_count; i += 2) {
        Value key(vtx_object_get_field(obj_, i));
        if (key.is_object()) {
            const char* key_str = reinterpret_cast<const char*>(vtx_heap_ptr(key.raw()));
            if (key_str && name == key_str) return true;
        }
    }
    Object proto = prototype();
    if (proto.valid()) return proto.has(rt, name);
    return false;
}

bool Object::del(Runtime& rt, const std::string& name) {
    if (!obj_) return false;
    for (uint32_t i = FIRST_PROPERTY_OFFSET; i + 1 < obj_->field_count; i += 2) {
        Value key(vtx_object_get_field(obj_, i));
        if (key.is_object()) {
            const char* key_str = reinterpret_cast<const char*>(vtx_heap_ptr(key.raw()));
            if (key_str && name == key_str) {
                // Set both key and value to undefined (marks slot as free)
                vtx_object_set_field(obj_, i, VTX_VALUE_UNDEFINED);
                vtx_object_set_field(obj_, i + 1, VTX_VALUE_UNDEFINED);
                return true;
            }
        }
    }
    return false;
}

Object Object::prototype() const {
    if (!obj_) return Object(nullptr);
    Value proto_val(vtx_object_get_field(obj_, PROTOTYPE_FIELD_OFFSET));
    if (!proto_val.is_object()) return Object(nullptr);
    return Object(reinterpret_cast<vtx_heap_object_t*>(vtx_heap_ptr(proto_val.raw())));
}

void Object::set_prototype(Runtime& rt, Object proto) {
    if (!obj_) return;
    if (proto.valid()) {
        vtx_object_set_field(obj_, PROTOTYPE_FIELD_OFFSET, proto.as_value());
    } else {
        vtx_object_set_field(obj_, PROTOTYPE_FIELD_OFFSET, VTX_VALUE_NULL);
    }
}

void Object::transition_shape(Runtime& rt, uint32_t symbol_id) {
    // TODO: Implement shape transitions via ShapeTable
    // For now, shapes are not used — property access is linear scan.
    (void)rt;
    (void)symbol_id;
}

/* ---- Array ---- */

Array Array::create(Runtime& rt, uint32_t length) {
    // Allocate: field 0 = length (SMI), fields 1..length = elements
    uint32_t total_fields = 1 + length;
    size_t size = vtx_heap_object_alloc_size(total_fields);
    vtx_heap_object_t* obj = vtx_gc_alloc(rt.gc(), size, 0);
    if (!obj) return Array(nullptr);

    vtx_heap_object_init(obj, 0, 1, total_fields, size);
    // Set length
    vtx_object_set_field(obj, 0, Value::smi(length).raw());
    // Initialize elements to undefined
    for (uint32_t i = 0; i < length; i++) {
        vtx_object_set_field(obj, 1 + i, VTX_VALUE_UNDEFINED);
    }
    return Array(obj);
}

} // namespace vortex
