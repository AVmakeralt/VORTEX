// vortex/hidden_class.cpp — Hidden-class transition tree implementation.

#include "vortex/hidden_class.hpp"
#include <algorithm>
#include <cstring>

#define typeid typeid_
extern "C" {
#include "runtime/type_system.h"
}
#undef typeid

namespace vortex {

HiddenClass::HiddenClass(ShapeId id, HiddenClass* parent)
    : shape_id_(id), parent_(parent) {}

HiddenClass* HiddenClass::create_root() {
    return new HiddenClass(kShapeIdRoot, nullptr);
}

size_t HiddenClass::property_index(uint32_t symbol_id) const {
    size_t lo = 0, hi = properties_.size();
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (properties_[mid].symbol_id < symbol_id) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

const PropertyDescriptor* HiddenClass::find_property(uint32_t symbol_id) const {
    size_t idx = property_index(symbol_id);
    if (idx < properties_.size() && properties_[idx].symbol_id == symbol_id)
        return &properties_[idx];
    return nullptr;
}

const HiddenClass* HiddenClass::lookup_transition(uint32_t symbol_id) const {
    auto it = transitions_.find(symbol_id);
    return (it == transitions_.end()) ? nullptr : it->second;
}

HiddenClass* HiddenClass::add_property_transition(uint32_t symbol_id,
                                                   bool writable, bool enumerable, bool configurable) {
    auto it = transitions_.find(symbol_id);
    if (it != transitions_.end()) return it->second;
    if (find_property(symbol_id) != nullptr) return nullptr;
    HiddenClass* child = HiddenClassTable::instance().allocate_child(this);
    if (!child) return nullptr;
    child->properties_ = properties_;
    PropertyDescriptor pd;
    pd.symbol_id = symbol_id;
    pd.offset = static_cast<uint32_t>(properties_.size());
    pd.writable = writable ? 1 : 0;
    pd.enumerable = enumerable ? 1 : 0;
    pd.configurable = configurable ? 1 : 0;
    pd.reserved = 0;
    child->properties_.push_back(pd);
    std::sort(child->properties_.begin(), child->properties_.end(),
              [](const PropertyDescriptor& a, const PropertyDescriptor& b) {
                  return a.symbol_id < b.symbol_id;
              });
    transitions_[symbol_id] = child;
    return child;
}

HiddenClassTable::HiddenClassTable() : root_(nullptr), next_shape_id_(kShapeIdRoot + 1) {
    auto root_uptr = std::unique_ptr<HiddenClass>(HiddenClass::create_root());
    root_ = root_uptr.get();
    classes_.push_back(std::move(root_uptr));
}

HiddenClass* HiddenClassTable::allocate_child(HiddenClass* parent) {
    if (next_shape_id_ == 0) return nullptr;
    ShapeId id = next_shape_id_++;
    auto child = std::unique_ptr<HiddenClass>(new HiddenClass(id, parent));
    HiddenClass* raw = child.get();
    classes_.push_back(std::move(child));
    return raw;
}

}  /* namespace vortex */

/* C API in global namespace */
extern "C" {
using namespace vortex;

namespace {
HiddenClass* find_class_by_shape_id(ShapeId shape_id) {
    for (const auto& uptr : HiddenClassTable::instance().classes())
        if (uptr->shape_id() == shape_id) return uptr.get();
    return nullptr;
}
}

uint32_t vtx_hidden_class_root_shape_id(void) {
    return HiddenClassTable::instance().root()->shape_id();
}

uint32_t vtx_hidden_class_add_property(uint32_t current_shape_id, void* ts_void, const char* property_name) {
    if (!property_name || !ts_void) return current_shape_id;
    vtx_type_system_t* ts = (vtx_type_system_t*)ts_void;
    uint32_t sym = vtx_symbol_intern(ts, property_name);
    if (sym == VTX_SYMBOL_INVALID) return current_shape_id;
    auto& table = HiddenClassTable::instance();
    std::unique_lock<std::shared_mutex> lock(table.mutex());
    HiddenClass* cur = find_class_by_shape_id(current_shape_id);
    if (!cur) cur = table.root();
    if (cur->find_property(sym)) return current_shape_id;
    HiddenClass* child = cur->add_property_transition(sym);
    return child ? child->shape_id() : current_shape_id;
}

uint32_t vtx_hidden_class_find_offset(uint32_t shape_id, void* ts_void, const char* property_name) {
    if (!property_name || !ts_void) return UINT32_MAX;
    vtx_type_system_t* ts = (vtx_type_system_t*)ts_void;
    uint32_t sym = vtx_symbol_intern(ts, property_name);
    if (sym == VTX_SYMBOL_INVALID) return UINT32_MAX;
    auto& table = HiddenClassTable::instance();
    std::shared_lock<std::shared_mutex> lock(table.mutex());
    HiddenClass* cls = find_class_by_shape_id(shape_id);
    if (!cls) return UINT32_MAX;
    const PropertyDescriptor* pd = cls->find_property(sym);
    return pd ? pd->offset : UINT32_MAX;
}

uint32_t vtx_hidden_class_property_count(uint32_t shape_id) {
    auto& table = HiddenClassTable::instance();
    std::shared_lock<std::shared_mutex> lock(table.mutex());
    HiddenClass* cls = find_class_by_shape_id(shape_id);
    return cls ? cls->property_count() : 0;
}

size_t vtx_hidden_class_table_size(void) {
    auto& table = HiddenClassTable::instance();
    std::shared_lock<std::shared_mutex> lock(table.mutex());
    return table.class_count();
}

}  /* extern "C" */
