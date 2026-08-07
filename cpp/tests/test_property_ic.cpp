// tests/test_property_ic.cpp — Property-access IC tests.

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include "vortex/property_ic.hpp"

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { if (cond) g_pass++; else { g_fail++; fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, msg); } } while(0)

static void test_init_destroy() { CHECK(vtx_property_ic_init(1024) == 0, "init"); vtx_property_ic_destroy(); g_pass++; }
static void test_uninit_miss() { vtx_property_ic_init(1024); CHECK(vtx_property_ic_lookup(0, 1) == UINT32_MAX, "uninit miss"); vtx_property_ic_destroy(); }
static void test_mono_hit() {
    vtx_property_ic_init(1024);
    vtx_property_ic_update(0, 1, 5);
    CHECK(vtx_property_ic_lookup(0, 1) == 5, "mono hit");
    CHECK(vtx_property_ic_lookup(0, 2) == UINT32_MAX, "diff shape miss");
    CHECK(vtx_property_ic_state(0) == (uint8_t)vortex::ICState::MONOMORPHIC, "state MONO");
    vtx_property_ic_destroy();
}
static void test_poly_scan() {
    vtx_property_ic_init(1024);
    vtx_property_ic_update(0, 10, 100); vtx_property_ic_update(0, 20, 200); vtx_property_ic_update(0, 30, 300);
    CHECK(vtx_property_ic_lookup(0, 10) == 100, "poly 10");
    CHECK(vtx_property_ic_lookup(0, 20) == 200, "poly 20");
    CHECK(vtx_property_ic_lookup(0, 30) == 300, "poly 30");
    CHECK(vtx_property_ic_state(0) == (uint8_t)vortex::ICState::POLYMORPHIC, "state POLY");
    vtx_property_ic_destroy();
}
static void test_mega() {
    vtx_property_ic_init(1024);
    for (uint32_t i = 0; i <= vortex::kPropertyICMaxEntries; i++) vtx_property_ic_update(0, 100+i, i*10);
    CHECK(vtx_property_ic_state(0) == (uint8_t)vortex::ICState::MEGAMORPHIC, "state MEGA");
    CHECK(vtx_property_ic_lookup(0, 100) == UINT32_MAX, "mega miss");
    vtx_property_ic_destroy();
}
static void test_update_offset() {
    vtx_property_ic_init(1024);
    vtx_property_ic_update(0, 1, 5); vtx_property_ic_update(0, 1, 99);
    CHECK(vtx_property_ic_lookup(0, 1) == 99, "updated offset");
    vtx_property_ic_destroy();
}
static void test_site_isolation() {
    vtx_property_ic_init(1024);
    vtx_property_ic_update(0, 1, 10); vtx_property_ic_update(1, 1, 20);
    CHECK(vtx_property_ic_lookup(0, 1) == 10, "site 0");
    CHECK(vtx_property_ic_lookup(1, 1) == 20, "site 1");
    vtx_property_ic_destroy();
}
static void test_no_init() { vtx_property_ic_destroy(); CHECK(vtx_property_ic_lookup(0, 1) == UINT32_MAX, "no init miss"); }
static void test_many_sites() {
    vtx_property_ic_init(4096);
    for (uint32_t i = 0; i < 4096; i++) vtx_property_ic_update(i, i+1, i*2);
    bool ok = true;
    for (uint32_t i = 0; i < 4096; i++) if (vtx_property_ic_lookup(i, i+1) != i*2) { ok = false; break; }
    CHECK(ok, "4096 sites"); vtx_property_ic_destroy();
}
static void test_fuzz() {
    vtx_property_ic_init(4096); std::mt19937 rng(12345);
    for (int i = 0; i < 10000; i++) {
        uint32_t site = rng()%100, shape = rng()%50+1, off = rng()%1000;
        if (rng()%2) vtx_property_ic_update(site, shape, off); else vtx_property_ic_lookup(site, shape);
    }
    g_pass++; vtx_property_ic_destroy();
}

int main() {
    fprintf(stderr, "=== Property-Access IC Tests ===\n\n");
    test_init_destroy(); test_uninit_miss(); test_mono_hit(); test_poly_scan(); test_mega();
    test_update_offset(); test_site_isolation(); test_no_init(); test_many_sites(); test_fuzz();
    fprintf(stderr, "\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
