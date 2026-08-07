// tests/test_hidden_class.cpp — Hidden-class transition tree tests.

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>
#include "vortex/hidden_class.hpp"

#define typeid typeid_
extern "C" { #include "runtime/type_system.h" }
#undef typeid

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { if (cond) g_pass++; else { g_fail++; fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, msg); } } while(0)

static vtx_type_system_t* make_ts() {
    auto* ts = (vtx_type_system_t*)calloc(1, sizeof(vtx_type_system_t));
    if (vtx_type_system_init(ts) != 0) { free(ts); return nullptr; }
    return ts;
}
static void free_ts(vtx_type_system_t* ts) { if (ts) { vtx_type_system_destroy(ts); free(ts); } }

static void test_root_stable() {
    CHECK(vtx_hidden_class_root_shape_id() == vtx_hidden_class_root_shape_id(), "root stable");
    CHECK(vtx_hidden_class_root_shape_id() == vortex::kShapeIdRoot, "root = 1");
}
static void test_single_add() {
    auto* ts = make_ts(); uint32_t root = vtx_hidden_class_root_shape_id();
    uint32_t s = vtx_hidden_class_add_property(root, (void*)ts, "x");
    CHECK(s != root, "shape changed"); CHECK(vtx_hidden_class_property_count(s) == 1, "1 prop");
    CHECK(vtx_hidden_class_find_offset(s, (void*)ts, "x") == 0, "x@0"); free_ts(ts);
}
static void test_sequential() {
    auto* ts = make_ts(); uint32_t root = vtx_hidden_class_root_shape_id();
    uint32_t s = vtx_hidden_class_add_property(
        vtx_hidden_class_add_property(root, (void*)ts, "x"), (void*)ts, "y");
    s = vtx_hidden_class_add_property(s, (void*)ts, "z");
    CHECK(vtx_hidden_class_property_count(s) == 3, "3 props");
    CHECK(vtx_hidden_class_find_offset(s, (void*)ts, "x") == 0, "x@0");
    CHECK(vtx_hidden_class_find_offset(s, (void*)ts, "y") == 1, "y@1");
    CHECK(vtx_hidden_class_find_offset(s, (void*)ts, "z") == 2, "z@2"); free_ts(ts);
}
static void test_shared_transitions() {
    auto* ts = make_ts(); uint32_t root = vtx_hidden_class_root_shape_id();
    uint32_t a = vtx_hidden_class_add_property(vtx_hidden_class_add_property(root, (void*)ts, "x"), (void*)ts, "y");
    uint32_t b = vtx_hidden_class_add_property(vtx_hidden_class_add_property(root, (void*)ts, "x"), (void*)ts, "y");
    CHECK(a == b, "same seq → same shape"); free_ts(ts);
}
static void test_different_sequences_diverge() {
    auto* ts = make_ts(); uint32_t root = vtx_hidden_class_root_shape_id();
    uint32_t xy = vtx_hidden_class_add_property(vtx_hidden_class_add_property(root, (void*)ts, "x"), (void*)ts, "y");
    uint32_t yx = vtx_hidden_class_add_property(vtx_hidden_class_add_property(root, (void*)ts, "y"), (void*)ts, "x");
    CHECK(xy != yx, "different order → different shape"); free_ts(ts);
}
static void test_duplicate_noop() {
    auto* ts = make_ts(); uint32_t root = vtx_hidden_class_root_shape_id();
    uint32_t s1 = vtx_hidden_class_add_property(root, (void*)ts, "x");
    uint32_t s2 = vtx_hidden_class_add_property(s1, (void*)ts, "x");
    CHECK(s1 == s2, "duplicate no-op"); free_ts(ts);
}
static void test_null_inputs() {
    auto* ts = make_ts(); uint32_t root = vtx_hidden_class_root_shape_id();
    CHECK(vtx_hidden_class_add_property(root, (void*)ts, nullptr) == root, "NULL name no-op");
    CHECK(vtx_hidden_class_add_property(root, nullptr, "x") == root, "NULL ts no-op"); free_ts(ts);
}
static void test_unknown_shape() {
    auto* ts = make_ts();
    uint32_t s = vtx_hidden_class_add_property(999999, (void*)ts, "x");
    CHECK(s != 999999, "unknown shape → new"); free_ts(ts);
}
static void test_long_chain() {
    auto* ts = make_ts(); uint32_t s = vtx_hidden_class_root_shape_id();
    const int N = 1000; std::vector<std::string> names;
    for (int i = 0; i < N; i++) { char b[32]; snprintf(b, sizeof(b), "p_%d", i); names.emplace_back(b); s = vtx_hidden_class_add_property(s, (void*)ts, names[i].c_str()); }
    CHECK(vtx_hidden_class_property_count(s) == N, "1000 props");
    bool ok = true;
    for (int i = 0; i < N; i++) if (vtx_hidden_class_find_offset(s, (void*)ts, names[i].c_str()) != (uint32_t)i) { ok = false; break; }
    CHECK(ok, "all offsets correct"); free_ts(ts);
}
static void test_many_same_seq() {
    auto* ts = make_ts(); uint32_t root = vtx_hidden_class_root_shape_id();
    size_t before = vtx_hidden_class_table_size();
    uint32_t exp = 0; bool same = true;
    for (int i = 0; i < 10000; i++) { uint32_t s = vtx_hidden_class_add_property(root, (void*)ts, "x"); if (i == 0) exp = s; else if (s != exp) { same = false; break; } }
    CHECK(same, "10000 same → same shape");
    CHECK(vtx_hidden_class_table_size() <= before + 1, "≤1 new class"); free_ts(ts);
}
static void test_fuzz() {
    auto* ts = make_ts(); uint32_t root = vtx_hidden_class_root_shape_id();
    std::mt19937 rng(42);
    const char* pool[] = {"a","b","c","d","e","f","g","h"};
    int fails = 0;
    for (int seq = 0; seq < 1000; seq++) {
        int len = rng() % 8 + 1; uint32_t sa = root, sb = root;
        for (int i = 0; i < len; i++) { const char* n = pool[rng() % 8]; sa = vtx_hidden_class_add_property(sa, (void*)ts, n); sb = vtx_hidden_class_add_property(sb, (void*)ts, n); }
        if (sa != sb) fails++;
    }
    CHECK(fails == 0, "fuzz convergence"); free_ts(ts);
}

int main() {
    fprintf(stderr, "=== Hidden-Class Tests ===\n\n");
    test_root_stable(); test_single_add(); test_sequential(); test_shared_transitions();
    test_different_sequences_diverge(); test_duplicate_noop(); test_null_inputs();
    test_unknown_shape(); test_long_chain(); test_many_same_seq(); test_fuzz();
    fprintf(stderr, "\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
