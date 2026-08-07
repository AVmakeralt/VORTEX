// vortex/property_ic.cpp — Property-access IC implementation.

#include "vortex/property_ic.hpp"
#include <cstdlib>
#include <cstring>

namespace vortex {
namespace {
PropertyIC* g_ic_table = nullptr;
uint32_t    g_ic_table_size = 0;
}
}

extern "C" {
using namespace vortex;

int vtx_property_ic_init(uint32_t max_sites) {
    if (g_ic_table) vtx_property_ic_destroy();
    if (max_sites == 0) max_sites = 4096;
    g_ic_table = (PropertyIC*)calloc(max_sites, sizeof(PropertyIC));
    if (!g_ic_table) return -1;
    g_ic_table_size = max_sites;
    for (uint32_t i = 0; i < max_sites; i++) g_ic_table[i].init();
    return 0;
}

void vtx_property_ic_destroy(void) {
    if (g_ic_table) { free(g_ic_table); g_ic_table = nullptr; g_ic_table_size = 0; }
}

uint32_t vtx_property_ic_lookup(uint32_t site_id, uint32_t shape_id) {
    if (!g_ic_table || g_ic_table_size == 0) return UINT32_MAX;
    return g_ic_table[site_id % g_ic_table_size].lookup(shape_id);
}

void vtx_property_ic_update(uint32_t site_id, uint32_t shape_id, uint32_t offset) {
    if (!g_ic_table || g_ic_table_size == 0) return;
    g_ic_table[site_id % g_ic_table_size].update(shape_id, offset);
}

uint8_t vtx_property_ic_state(uint32_t site_id) {
    if (!g_ic_table || g_ic_table_size == 0) return (uint8_t)ICState::UNINITIALIZED;
    return (uint8_t)g_ic_table[site_id % g_ic_table_size].state.load(std::memory_order_acquire);
}

}  /* extern "C" */
