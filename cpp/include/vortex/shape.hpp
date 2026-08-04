// vortex/shape.hpp — V8-style hidden classes (shapes) for fast property access.
//
// A Shape represents the layout of an Object: which property names are
// stored at which field offsets. When an object adds a new property, it
// "transitions" to a new Shape that includes the new property.
//
// Objects with the same shape share the same layout, so inline caches
// (IC) can cache (shape_id → field_offset) and skip the property name
// lookup on the fast path.
//
// Shape transitions form a tree rooted at the empty shape. Each transition
// adds one property. Two objects that add the same properties in the same
// order end up at the same shape → same IC hit.
//
// Thread safety: ShapeTable is NOT thread-safe. Use one per Runtime.

#ifndef VORTEX_SHAPE_HPP
#define VORTEX_SHAPE_HPP

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#define typeid typeid_
extern "C" {
#include "runtime/type_system.h"  // for vtx_symbol_intern
}
#undef typeid

namespace vortex {

// A Shape describes the layout of an Object's properties.
// Shapes are immutable once created — adding a property creates a new
// child shape via a transition.
class Shape {
public:
    static Shape invalid() { return Shape(0); }

    Shape() : id_(0) {}
    explicit Shape(uint32_t id) : id_(id) {}

    uint32_t id() const { return id_; }
    bool valid() const { return id_ != 0; }

private:
    uint32_t id_ = 0;
};

// A property slot: name (symbol ID) → field offset.
struct PropertySlot {
    uint32_t symbol_id;   // interned property name
    uint32_t field_offset; // index into Object's fields[]
};

// Internal shape data: the list of properties and transition table.
// Stored in ShapeTable, indexed by shape_id.
struct ShapeData {
    std::vector<PropertySlot> properties;     // ordered by insertion
    std::unordered_map<uint32_t, uint32_t> transitions; // symbol_id → child shape_id
    uint32_t parent_shape_id = 0;  // 0 = root (empty shape)
};

// The ShapeTable owns all shapes for a Runtime. It creates new shapes
// on property transitions and looks up existing shapes.
class ShapeTable {
public:
    ShapeTable() {
        // Shape 0 is reserved (INVALID), shape 1 is the root (empty).
        shapes_.emplace_back();  // index 0 = invalid
        shapes_.emplace_back();  // index 1 = root empty shape
        root_shape_ = 1;
    }

    // Get the root (empty) shape.
    Shape root() const { return Shape(root_shape_); }

    // Look up a property's field offset in a shape.
    // Returns UINT32_MAX if not found.
    uint32_t find_property(uint32_t shape_id, uint32_t symbol_id) const {
        if (shape_id >= shapes_.size()) return UINT32_MAX;
        const auto& s = shapes_[shape_id];
        for (const auto& slot : s.properties) {
            if (slot.symbol_id == symbol_id) return slot.field_offset;
        }
        return UINT32_MAX;
    }

    // Add a property to a shape, returning the child shape.
    // If the transition already exists, returns the existing child.
    Shape add_property(uint32_t parent_shape_id, uint32_t symbol_id) {
        if (parent_shape_id >= shapes_.size()) return Shape::invalid();

        // Check if transition already exists
        auto& parent = shapes_[parent_shape_id];
        auto it = parent.transitions.find(symbol_id);
        if (it != parent.transitions.end()) {
            return Shape(it->second);
        }

        // Create new child shape
        uint32_t new_id = static_cast<uint32_t>(shapes_.size());
        ShapeData& child = shapes_.emplace_back();
        child.properties = parent.properties;  // copy parent's properties
        child.properties.push_back({symbol_id, static_cast<uint32_t>(child.properties.size())});
        child.parent_shape_id = parent_shape_id;

        // Record transition
        parent.transitions[symbol_id] = new_id;

        return Shape(new_id);
    }

    // Convenience: add a property by name (interns the symbol)
    Shape add_property(uint32_t parent_shape_id, const char* name) {
        // Need a type system to intern — caller should pass symbol_id
        // For now, hash the name directly (simple, no type system dep)
        uint32_t sym = intern_symbol(name);
        return add_property(parent_shape_id, sym);
    }

    // Get the property count for a shape (== field_count of objects with this shape)
    uint32_t property_count(uint32_t shape_id) const {
        if (shape_id >= shapes_.size()) return 0;
        return static_cast<uint32_t>(shapes_[shape_id].properties.size());
    }

    // Iterate properties of a shape
    const std::vector<PropertySlot>& properties(uint32_t shape_id) const {
        static const std::vector<PropertySlot> empty;
        if (shape_id >= shapes_.size()) return empty;
        return shapes_[shape_id].properties;
    }

    // Simple symbol interning (without requiring a vtx_type_system_t)
    // Maps name → stable numeric ID. IDs are stable for the ShapeTable's
    // lifetime. Not shared across ShapeTables.
    uint32_t intern_symbol(const std::string& name) {
        auto it = symbol_to_id_.find(name);
        if (it != symbol_to_id_.end()) return it->second;
        uint32_t id = static_cast<uint32_t>(next_symbol_id_++);
        symbol_to_id_[name] = id;
        id_to_symbol_[id] = name;
        return id;
    }

    const std::string& symbol_name(uint32_t symbol_id) const {
        static const std::string empty;
        auto it = id_to_symbol_.find(symbol_id);
        return (it != id_to_symbol_.end()) ? it->second : empty;
    }

    // Statistics
    size_t shape_count() const { return shapes_.size() - 2; }  // minus invalid + root

private:
    std::vector<ShapeData> shapes_;
    uint32_t root_shape_ = 1;

    std::unordered_map<std::string, uint32_t> symbol_to_id_;
    std::unordered_map<uint32_t, std::string> id_to_symbol_;
    uint32_t next_symbol_id_ = 1;  // 0 reserved
};

} // namespace vortex

#endif // VORTEX_SHAPE_HPP
