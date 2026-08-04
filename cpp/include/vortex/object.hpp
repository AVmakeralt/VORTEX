// vortex/object.hpp — Dynamic heap object with shape-based property access.
//
// An Object is a heap-allocated value with named properties. Property
// access uses a Shape (hidden class) to map property names → field
// offsets, enabling fast inline caches.
//
// Objects support prototype-based inheritance: each object has an
// optional __proto__ field pointing to another Object. Property
// reads that miss on the object walk up the prototype chain.
//
// Usage:
//   vortex::Runtime rt = ...;
//   vortex::Object obj = vortex::Object::create(rt);
//   obj.set("x", vortex::Value::smi(42));
//   obj.set("y", vortex::Value::smi(99));
//   vortex::Value x = obj.get("x");  // → 42
//
//   // Prototype chain
//   vortex::Object proto = vortex::Object::create(rt);
//   proto.set("greet", vortex::Value::smi(0));  // method slot
//   vortex::Object child = vortex::Object::create(rt);
//   child.set_prototype(proto);
//   // child.get("greet") walks the prototype chain → finds proto.greet

#ifndef VORTEX_OBJECT_HPP
#define VORTEX_OBJECT_HPP

#include <cstdint>
#include <string>

#define typeid typeid_
extern "C" {
#include "runtime/object.h"
#include "runtime/gc.h"
}
#undef typeid
#include "vortex/value.hpp"
#include "vortex/shape.hpp"

namespace vortex {

class Runtime;

class Object {
public:
    /* ---- Construction ---- */

    // Create a new empty object with the given max field capacity.
    // The object is allocated via the runtime's GC.
    static Object create(Runtime& rt, uint32_t max_fields = 16);

    // Wrap an existing raw heap object pointer.
    // Does NOT take ownership — the GC still owns the memory.
    explicit Object(vtx_heap_object_t* obj = nullptr) : obj_(obj) {}

    /* ---- Type predicates ---- */
    bool valid() const { return obj_ != nullptr; }
    bool is_null() const { return obj_ == nullptr; }
    explicit operator bool() const { return valid(); }

    /* ---- Property access (shape-based) ---- */

    // Get a property by name. Walks the prototype chain if not found.
    Value get(Runtime& rt, const std::string& name);
    Value get(Runtime& rt, uint32_t symbol_id);

    // Set a property. If the property doesn't exist, adds it (which may
    // transition the shape). If it exists, updates the value in place.
    void set(Runtime& rt, const std::string& name, Value value);
    void set(Runtime& rt, uint32_t symbol_id, Value value);

    // Check if a property exists (walks prototype chain).
    bool has(Runtime& rt, const std::string& name);

    // Delete a property. Returns true if it existed.
    // NOTE: Does NOT compact fields (sets to undefined, keeps shape).
    bool del(Runtime& rt, const std::string& name);

    // Get the number of own (non-inherited) properties.
    uint32_t field_count() const {
        return obj_ ? obj_->field_count : 0;
    }

    /* ---- Field access (by offset, no shape lookup) ---- */

    Value get_field(uint32_t offset) const {
        if (!obj_ || offset >= obj_->field_count) return Value::undefined();
        return Value(vtx_object_get_field(obj_, offset));
    }

    void set_field(uint32_t offset, Value value) {
        if (obj_ && offset < obj_->field_count) {
            vtx_object_set_field(obj_, offset, value.raw());
        }
    }

    /* ---- Prototype chain ---- */

    // Get this object's prototype (or null).
    Object prototype() const;

    // Set this object's prototype.
    void set_prototype(Runtime& rt, Object proto);

    /* ---- Shape access ---- */

    uint32_t shape_id() const {
        return obj_ ? obj_->shape_id : 0;
    }

    // Transition to a new shape that includes `name`.
    // Called internally by set() when adding a new property.
    void transition_shape(Runtime& rt, uint32_t symbol_id);

    /* ---- Raw access ---- */

    vtx_heap_object_t* raw() const { return obj_; }
    vtx_value_t as_value() const {
        // Heap-pointer NaN-boxing
        return vtx_make_heap_ptr(obj_);
    }
    Value as_wrapper() const { return Value(vtx_make_heap_ptr(obj_)); }

private:
    vtx_heap_object_t* obj_;

    // The prototype is stored as a hidden field at offset 0.
    // (Or we could store it in the type_id field's upper bits —
    // but offset 0 is simpler and the prototype is rarely accessed.)
    static constexpr uint32_t PROTOTYPE_FIELD_OFFSET = 0;
    static constexpr uint32_t FIRST_PROPERTY_OFFSET = 1;

    // For objects created via create(), we reserve field 0 for the
    // prototype. User properties start at field 1.
    // Objects created from C with raw vtx_heap_object_t won't have
    // this reservation — use the low-level get_field/set_field for those.
};

/* ---- Value downcast helpers (defined here, not in value.hpp) ---- */

inline Object Value::as_object_wrapper() const {
    return Object(as_object());
}

// Array wrapper — a heap object used as an array.
class Array {
public:
    static Array create(Runtime& rt, uint32_t length);

    explicit Array(vtx_heap_object_t* obj = nullptr) : obj_(obj) {}

    bool valid() const { return obj_ != nullptr; }

    uint32_t length() const {
        // Length is stored in field 0 (as an SMI)
        if (!obj_) return 0;
        Value len_val = Value(vtx_object_get_field(obj_, 0));
        return static_cast<uint32_t>(len_val.to_int());
    }

    Value get(uint32_t index) const {
        if (!obj_) return Value::undefined();
        // Element i is stored at field (1 + i)
        if (index >= length()) return Value::undefined();
        return Value(vtx_object_get_field(obj_, 1 + index));
    }

    void set(uint32_t index, Value value) {
        if (!obj_) return;
        uint32_t len = length();
        if (index < len) {
            vtx_object_set_field(obj_, 1 + index, value.raw());
        }
        // TODO: grow array if index >= len
    }

    vtx_heap_object_t* raw() const { return obj_; }
    Value as_value() const { return Value(vtx_make_heap_ptr(obj_)); }

private:
    vtx_heap_object_t* obj_;
};

inline Array Value::as_array() const {
    return Array(as_object());
}

// String wrapper — a heap object holding a UTF-8 string.
class String {
public:
    // Create a VORTEX string value from a C++ string.
    // NOTE: The current runtime stores strings as raw C pointers in the
    // NaN-boxed value (heap_ptr tag). A full string object with GC
    // integration is future work — for now, this is a thin wrapper.
    static Value create(const std::string& s) {
        // For now, return an SMI of the string length as a placeholder.
        // Real string support requires GC-allocated string objects.
        // TODO: integrate with vtx_gc_alloc for proper string objects.
        return Value::smi(static_cast<int64_t>(s.length()));
    }

    // Convert a VORTEX value to a C++ string.
    static std::string to_cpp_str(Value v) {
        if (v.is_int())    return std::to_string(v.as_int());
        if (v.is_double()) return std::to_string(v.as_double());
        if (v.is_bool())   return v.as_bool() ? "true" : "false";
        if (v.is_null())   return "null";
        if (v.is_undefined()) return "undefined";
        if (v.is_object()) {
            // If it's a heap object, try to interpret as a string
            // (future: check type_id for string type)
            return "[Object]";
        }
        return "";
    }
};

inline String Value::as_string() const {
    return String();
}

} // namespace vortex

#endif // VORTEX_OBJECT_HPP
