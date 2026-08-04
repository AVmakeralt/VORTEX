// vortex/value.hpp — Type-safe wrapper around the NaN-boxed vtx_value_t.
//
// A Value is 8 bytes, copyable, and holds one of:
//   - SMI (small integer, 46-bit signed)
//   - double (IEEE 754)
//   - bool
//   - null
//   - undefined
//   - heap pointer (Object, Array, String)
//
// All constructors and accessors are inline — zero overhead vs raw
// vtx_value_t.
//
// Usage:
//   vortex::Value v = vortex::Value::smi(42);
//   if (v.is_int())  int64_t i = v.as_int();
//   vortex::Value d = vortex::Value::double_val(3.14);
//   vortex::Value s = vortex::Value::boolean(true);

#ifndef VORTEX_VALUE_HPP
#define VORTEX_VALUE_HPP

#include <cstdint>
#include <cmath>
#include <cstring>
#include <string>
#include <type_traits>

// C headers use `typeid` as a parameter name, which is a C++ reserved
// keyword. Rename it via macro before including.
#define typeid typeid_
extern "C" {
#include "runtime/object.h"
}
#undef typeid

namespace vortex {

class Object;
class Array;
class String;

class Value {
public:
    /* ---- Constructors ---- */
    Value() : raw_(VTX_VALUE_UNDEFINED) {}
    explicit Value(vtx_value_t v) : raw_(v) {}

    /* ---- Type tags ---- */
    enum class Type {
        Int,        // SMI (small integer)
        Double,     // IEEE 754 float
        Bool,       // true/false
        Null,
        Undefined,
        Object,     // heap pointer (Object/Array/String)
    };

    /* ---- Factory methods ---- */
    static Value smi(int64_t val) {
        return Value(vtx_make_smi(val));
    }
    static Value double_val(double d) {
        return Value(vtx_make_double(d));
    }
    static Value boolean(bool b) {
        return Value(b ? VTX_VALUE_TRUE : VTX_VALUE_FALSE);
    }
    static Value null() { return Value(VTX_VALUE_NULL); }
    static Value undefined() { return Value(VTX_VALUE_UNDEFINED); }

    /* Handle integer/double ambiguity: prefer SMI when value fits */
    static Value number(double d) {
        // If d is integral and fits in SMI range, use SMI
        if (d == static_cast<double>(static_cast<int64_t>(d))) {
            int64_t i = static_cast<int64_t>(d);
            if (i >= VTX_SMI_MIN && i <= VTX_SMI_MAX) {
                return smi(i);
            }
        }
        return double_val(d);
    }

    /* ---- Type predicates ---- */
    bool is_int()       const { return vtx_is_smi(raw_); }
    bool is_double()    const { return vtx_is_double(raw_); }
    bool is_bool()      const { return vtx_is_bool(raw_); }
    bool is_null()      const { return vtx_is_null(raw_); }
    bool is_undefined() const { return vtx_is_undefined(raw_); }
    bool is_object()    const { return vtx_is_heap_ptr(raw_); }
    bool is_truthy()    const {
        // JS semantics: false, 0, "", null, undefined, NaN are falsy
        if (is_bool())    return as_bool();
        if (is_int())      return as_int() != 0;
        if (is_null() || is_undefined()) return false;
        if (is_double()) {
            double d = as_double();
            return !std::isnan(d) && d != 0.0;
        }
        return true; // objects are truthy
    }

    /* ---- Accessors (UB if type doesn't match — check first) ---- */
    int64_t  as_int()    const { return vtx_smi_value(raw_); }
    double   as_double() const { return vtx_double_value(raw_); }
    bool     as_bool()   const { return vtx_bool_value(raw_); }

    /* Numeric coercion: SMI → int64_t, double → int64_t (truncates) */
    int64_t  to_int() const {
        if (is_int()) return as_int();
        if (is_double()) return static_cast<int64_t>(as_double());
        if (is_bool()) return as_bool() ? 1 : 0;
        return 0;
    }
    double   to_double() const {
        if (is_double()) return as_double();
        if (is_int()) return static_cast<double>(as_int());
        if (is_bool()) return as_bool() ? 1.0 : 0.0;
        return 0.0;
    }

    /* ---- Object accessors ---- */
    // Returns non-null only if is_object(); caller must check.
    vtx_heap_object_t* as_object() const {
        return reinterpret_cast<vtx_heap_object_t*>(vtx_heap_ptr(raw_));
    }

    // Downcast helpers — return wrapper types. These are zero-overhead.
    Object as_object_wrapper() const;     // defined in object.hpp
    Array  as_array() const;               // defined in object.hpp
    String as_string() const;              // defined in object.hpp

    /* ---- Raw access (for FFI) ---- */
    vtx_value_t raw() const { return raw_; }
    operator vtx_value_t() const { return raw_; }

    /* ---- Comparison ---- */
    bool operator==(const Value& other) const {
        // NaN-boxed equality: raw bits match.
        // For doubles, this is IEEE equality (NaN != NaN), which is correct.
        return raw_ == other.raw_;
    }
    bool operator!=(const Value& other) const { return !(*this == other); }

    /* ---- String representation (for debugging) ---- */
    std::string to_string() const {
        if (is_int())       return std::to_string(as_int());
        if (is_double())    return std::to_string(as_double());
        if (is_bool())      return as_bool() ? "true" : "false";
        if (is_null())      return "null";
        if (is_undefined()) return "undefined";
        if (is_object()) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "[Object@%p]", as_object());
            return buf;
        }
        return "<?>";
    }

private:
    vtx_value_t raw_;
};

static_assert(sizeof(Value) == sizeof(vtx_value_t),
              "Value must be exactly 8 bytes (zero-overhead wrapper)");

} // namespace vortex

#endif // VORTEX_VALUE_HPP
