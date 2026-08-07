// vortex/hidden_class.hpp — Hidden-class (Map) transition tree.
// Models V8's Map / JSC's Structure / SpiderMonkey's Shape.

#ifndef VORTEX_HIDDEN_CLASS_HPP
#define VORTEX_HIDDEN_CLASS_HPP

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#ifdef __cplusplus
extern "C" {
#endif
struct vtx_type_system_t;
#ifdef __cplusplus
}
#endif

namespace vortex {

struct PropertyDescriptor {
    uint32_t symbol_id;
    uint32_t offset;
    uint8_t  writable : 1;
    uint8_t  enumerable : 1;
    uint8_t  configurable : 1;
    uint8_t  reserved : 5;
};

using ShapeId = uint32_t;
constexpr ShapeId kShapeIdInvalid = 0;
constexpr ShapeId kShapeIdRoot = 1;

class HiddenClass {
public:
    static HiddenClass* create_root();
    HiddenClass* add_property_transition(uint32_t symbol_id,
                                          bool writable = true,
                                          bool enumerable = true,
                                          bool configurable = true);
    const PropertyDescriptor* find_property(uint32_t symbol_id) const;
    const HiddenClass* lookup_transition(uint32_t symbol_id) const;
    ShapeId shape_id() const { return shape_id_; }
    uint32_t property_count() const { return static_cast<uint32_t>(properties_.size()); }
    const HiddenClass* parent() const { return parent_; }
    const std::vector<PropertyDescriptor>& properties() const { return properties_; }
private:
    friend class HiddenClassTable;
    HiddenClass(ShapeId id, HiddenClass* parent);
    HiddenClass(const HiddenClass&) = delete;
    HiddenClass& operator=(const HiddenClass&) = delete;
    size_t property_index(uint32_t symbol_id) const;
    ShapeId                            shape_id_;
    HiddenClass*                       parent_;
    std::vector<PropertyDescriptor>   properties_;
    std::unordered_map<uint32_t, HiddenClass*> transitions_;
};

class HiddenClassTable {
public:
    static HiddenClassTable& instance() {
        static HiddenClassTable t;
        return t;
    }
    HiddenClass* root() { return root_; }
    std::shared_mutex& mutex() { return mutex_; }
    size_t class_count() const { return classes_.size(); }
    const std::vector<std::unique_ptr<HiddenClass>>& classes() const { return classes_; }
private:
    HiddenClassTable();
    ~HiddenClassTable() = default;
    HiddenClassTable(const HiddenClassTable&) = delete;
    HiddenClassTable& operator=(const HiddenClassTable&) = delete;
    friend class HiddenClass;
    HiddenClass* allocate_child(HiddenClass* parent);
    std::shared_mutex                          mutex_;
    std::vector<std::unique_ptr<HiddenClass>>  classes_;
    HiddenClass*                               root_;
    ShapeId                                    next_shape_id_;
};

}  /* namespace vortex */

extern "C" {
uint32_t vtx_hidden_class_root_shape_id(void);
uint32_t vtx_hidden_class_add_property(uint32_t current_shape_id, void* ts, const char* property_name);
uint32_t vtx_hidden_class_find_offset(uint32_t shape_id, void* ts, const char* property_name);
uint32_t vtx_hidden_class_property_count(uint32_t shape_id);
size_t vtx_hidden_class_table_size(void);
}  /* extern "C" */

#endif  /* VORTEX_HIDDEN_CLASS_HPP */
