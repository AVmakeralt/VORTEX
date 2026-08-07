// vortex/property_ic.hpp — Property-access inline cache.

#ifndef VORTEX_PROPERTY_IC_HPP
#define VORTEX_PROPERTY_IC_HPP

#include <cstdint>
#include <cstring>
#include <atomic>

namespace vortex {

enum class ICState : uint8_t {
    UNINITIALIZED = 0, PRE_MONOMORPHIC = 1, MONOMORPHIC = 2,
    POLYMORPHIC = 3, MEGAMORPHIC = 4,
};

constexpr uint32_t kPropertyICMaxEntries = 4;

struct PropertyICEntry {
    uint32_t shape_id;
    uint32_t offset;
};

struct PropertyIC {
    std::atomic<ICState> state;
    std::atomic<uint32_t> count;
    PropertyICEntry entries[kPropertyICMaxEntries];

    void init() {
        state.store(ICState::UNINITIALIZED, std::memory_order_relaxed);
        count.store(0, std::memory_order_relaxed);
        memset(entries, 0, sizeof(entries));
    }

    uint32_t lookup(uint32_t shape_id) const {
        ICState s = state.load(std::memory_order_acquire);
        if (s == ICState::UNINITIALIZED || s == ICState::MEGAMORPHIC)
            return UINT32_MAX;
        uint32_t n = count.load(std::memory_order_acquire);
        for (uint32_t i = 0; i < n && i < kPropertyICMaxEntries; i++)
            if (entries[i].shape_id == shape_id) return entries[i].offset;
        return UINT32_MAX;
    }

    void update(uint32_t shape_id, uint32_t offset) {
        ICState s = state.load(std::memory_order_relaxed);
        uint32_t n = count.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < n && i < kPropertyICMaxEntries; i++) {
            if (entries[i].shape_id == shape_id) { entries[i].offset = offset; return; }
        }
        if (s == ICState::UNINITIALIZED) {
            entries[0] = {shape_id, offset};
            count.store(1, std::memory_order_release);
            state.store(ICState::MONOMORPHIC, std::memory_order_release);
        } else if (s == ICState::MONOMORPHIC || s == ICState::POLYMORPHIC) {
            if (n < kPropertyICMaxEntries) {
                entries[n] = {shape_id, offset};
                count.store(n + 1, std::memory_order_release);
                state.store(ICState::POLYMORPHIC, std::memory_order_release);
            } else {
                state.store(ICState::MEGAMORPHIC, std::memory_order_release);
            }
        }
    }

    bool is_megamorphic() const {
        return state.load(std::memory_order_acquire) == ICState::MEGAMORPHIC;
    }
};

}  /* namespace vortex */

extern "C" {
int vtx_property_ic_init(uint32_t max_sites);
void vtx_property_ic_destroy(void);
uint32_t vtx_property_ic_lookup(uint32_t site_id, uint32_t shape_id);
void vtx_property_ic_update(uint32_t site_id, uint32_t shape_id, uint32_t offset);
uint8_t vtx_property_ic_state(uint32_t site_id);
}

#endif  /* VORTEX_PROPERTY_IC_HPP */
