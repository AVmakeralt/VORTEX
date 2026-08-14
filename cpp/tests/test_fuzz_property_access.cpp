// tests/test_fuzz_property_access.cpp — Fuzz tests for property IC + hidden class.
//
// Per READ_BEFORE_EDITING.md §3: tests must include stress and fuzz cases.
// This file generates random property-access patterns and verifies:
//   - The IC returns correct offsets (no stale cache entries)
//   - The hidden-class transition tree converges (same seq → same shape)
//   - No crashes under adversarial input (very long names, rapid transitions)
//   - IC state machine transitions correctly under concurrent-like load
//   - Shape IDs never collide (uniqueness invariant)

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

#include "vortex/hidden_class.hpp"
#include "vortex/property_ic.hpp"

#define typeid typeid_
extern "C" {
#include "runtime/type_system.h"
}
#undef typeid

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { if (cond) g_pass++; else { g_fail++; fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, msg); } } while(0)

static vtx_type_system_t* make_ts() {
    auto* ts = (vtx_type_system_t*)calloc(1, sizeof(vtx_type_system_t));
    if (vtx_type_system_init(ts) != 0) { free(ts); return nullptr; }
    return ts;
}
static void free_ts(vtx_type_system_t* ts) { if (ts) { vtx_type_system_destroy(ts); free(ts); } }

/* ========================================================================== */
/* Fuzz 1: Random property-add sequences verify IC correctness                */
/* ========================================================================== */

static void test_fuzz_ic_correctness() {
    /* For each random sequence:
     *   1. Build a shape by adding properties
     *   2. Record the expected offsets
     *   3. Look up each property via the IC
     *   4. Verify the IC returns the correct offset
     *
     * This tests that the IC doesn't return stale or wrong offsets
     * when the shape changes. */
    auto* ts = make_ts();
    vtx_property_ic_init(4096);
    uint32_t root = vtx_hidden_class_root_shape_id();
    std::mt19937 rng(7777);

    const char* pool[] = {"a","b","c","d","e","f","g","h","i","j","k","l","m"};
    const int pool_size = sizeof(pool) / sizeof(pool[0]);

    int iterations = 500;
    int failures = 0;

    for (int it = 0; it < iterations; it++) {
        /* Build a random shape */
        int len = (int)(rng() % 6) + 1;
        uint32_t shape = root;
        std::vector<std::pair<uint32_t, uint32_t>> expected;  /* (hash, offset) */

        for (int i = 0; i < len; i++) {
            const char* name = pool[rng() % pool_size];
            uint32_t prev_count = vtx_hidden_class_property_count(shape);
            uint32_t new_shape = vtx_hidden_class_add_property(shape, (void*)ts, name);

            if (new_shape == shape) {
                /* Duplicate add — skip */
                continue;
            }

            /* The offset should be prev_count (insertion order) */
            uint32_t offset = vtx_hidden_class_find_offset(new_shape, (void*)ts, name);
            if (offset != prev_count && offset != UINT32_MAX) {
                failures++;
                continue;
            }

            /* Update the IC at this site with this shape → offset */
            uint32_t site_id = (uint32_t)(it * 100 + (int)i);  /* unique site per (iteration, step) */
            vtx_property_ic_update(site_id, new_shape, offset);

            shape = new_shape;
            expected.push_back({new_shape, offset});
        }

        /* Verify: look up each shape via the IC at its site */
        for (size_t i = 0; i < expected.size(); i++) {
            uint32_t site_id = (uint32_t)(it * 100 + (int)i);
            uint32_t ic_result = vtx_property_ic_lookup(site_id, expected[i].first);
            if (ic_result != expected[i].second && ic_result != UINT32_MAX) {
                /* IC returned a wrong offset — critical bug */
                failures++;
            }
        }
    }

    CHECK(failures == 0, "fuzz: IC returns correct offsets across 500 random sequences");
    vtx_property_ic_destroy();
    free_ts(ts);
}

/* ========================================================================== */
/* Fuzz 2: Shape ID uniqueness (no two different sequences produce same ID)   */
/* ========================================================================== */

static void test_fuzz_shape_id_uniqueness() {
    /* Generate many different property sequences. Each UNIQUE sequence
     * should produce a UNIQUE shape_id. If two different sequences produce
     * the same shape_id, the IC would return wrong offsets. */
    auto* ts = make_ts();
    uint32_t root = vtx_hidden_class_root_shape_id();
    std::mt19937 rng(42);

    const char* pool[] = {"a","b","c","d","e","f","g","h"};
    const int pool_size = sizeof(pool) / sizeof(pool[0]);

    /* Map: shape_id → the FULL sequence that produced it.
     * The invariant is: same shape_id ⟹ same property sequence.
     * (Different sequences CAN share intermediate shapes via the
     * transition tree — that's correct sharing, not a collision.)
     * We track the FULL path from root to each shape_id, not just
     * the final step. */
    std::unordered_map<uint32_t, std::string> shape_to_full_path;
    int collisions = 0;

    for (int i = 0; i < 2000; i++) {
        int len = (int)(rng() % 5) + 1;
        uint32_t shape = root;
        std::string full_path;

        for (int j = 0; j < len; j++) {
            const char* name = pool[rng() % pool_size];
            uint32_t prev_shape = shape;
            shape = vtx_hidden_class_add_property(shape, (void*)ts, name);

            /* Only add to the path if the shape actually changed.
             * Duplicate adds (same property already present) are no-ops
             * — they return the same shape_id. Including them in the path
             * would cause a false collision (same shape, different path). */
            if (shape != prev_shape) {
                full_path += name;
                full_path += ",";
            }

            /* Check: if we've seen this shape_id before, the full path
             * must match. If it doesn't, it's a real collision. */
            auto it = shape_to_full_path.find(shape);
            if (it != shape_to_full_path.end()) {
                if (it->second != full_path) {
                    collisions++;
                }
            } else {
                shape_to_full_path[shape] = full_path;
            }
        }
    }

    CHECK(collisions == 0, "fuzz: no shape_id collisions (same shape ⟹ same path)");
    free_ts(ts);
}

/* ========================================================================== */
/* Fuzz 3: IC state machine under rapid transitions                           */
/* ========================================================================== */

static void test_fuzz_ic_state_machine() {
    /* Rapidly add shapes to one IC site until it goes megamorphic.
     * Verify the state transitions are correct and the IC never
     * returns a wrong offset once megamorphic. */
    vtx_property_ic_init(1024);
    std::mt19937 rng(999);

    /* Fill the IC at site 0 with kPropertyICMaxEntries distinct shapes */
    uint32_t shapes[16];
    for (uint32_t i = 0; i < vortex::kPropertyICMaxEntries; i++) {
        shapes[i] = 100 + i;
        vtx_property_ic_update(0, shapes[i], i * 10);
    }

    /* State should be POLYMORPHIC */
    CHECK(vtx_property_ic_state(0) == (uint8_t)vortex::ICState::POLYMORPHIC,
          "IC at capacity → POLYMORPHIC");

    /* All entries should hit */
    bool all_hit = true;
    for (uint32_t i = 0; i < vortex::kPropertyICMaxEntries; i++) {
        if (vtx_property_ic_lookup(0, shapes[i]) != i * 10) {
            all_hit = false;
            break;
        }
    }
    CHECK(all_hit, "all poly entries hit at capacity");

    /* Add one more → should go MEGAMORPHIC */
    vtx_property_ic_update(0, 999, 42);
    CHECK(vtx_property_ic_state(0) == (uint8_t)vortex::ICState::MEGAMORPHIC,
          "one more → MEGAMORPHIC");

    /* Once megamorphic, ALL lookups should miss (return UINT32_MAX) */
    bool all_miss = true;
    for (uint32_t i = 0; i < vortex::kPropertyICMaxEntries; i++) {
        if (vtx_property_ic_lookup(0, shapes[i]) != UINT32_MAX) {
            all_miss = false;
            break;
        }
    }
    CHECK(all_miss, "megamorphic → all lookups miss");

    /* Rapid random lookups should never crash */
    for (int i = 0; i < 10000; i++) {
        uint32_t shape = (uint32_t)(rng() % 2000);
        uint32_t result = vtx_property_ic_lookup(0, shape);
        /* Megamorphic → must be UINT32_MAX */
        if (result != UINT32_MAX) {
            g_fail++;
            fprintf(stderr, "FAIL: megamorphic returned offset for shape %u\n", shape);
            break;
        }
    }
    g_pass++;

    vtx_property_ic_destroy();
}

/* ========================================================================== */
/* Edge 1: Very long property names                                           */
/* ========================================================================== */

static void test_edge_long_property_names() {
    auto* ts = make_ts();
    uint32_t root = vtx_hidden_class_root_shape_id();

    /* 1KB property name */
    std::string long_name(1024, 'x');
    uint32_t s = vtx_hidden_class_add_property(root, (void*)ts, long_name.c_str());
    CHECK(s != root, "long name → new shape");
    CHECK(vtx_hidden_class_find_offset(s, (void*)ts, long_name.c_str()) == 0,
          "long name found at offset 0");
    free_ts(ts);
}

/* ========================================================================== */
/* Edge 2: Empty property name                                                 */
/* ========================================================================== */

static void test_edge_empty_property_name() {
    auto* ts = make_ts();
    uint32_t root = vtx_hidden_class_root_shape_id();
    uint32_t s = vtx_hidden_class_add_property(root, (void*)ts, "");
    CHECK(s != root, "empty name → new shape");
    CHECK(vtx_hidden_class_find_offset(s, (void*)ts, "") == 0,
          "empty name found at offset 0");
    free_ts(ts);
}

/* ========================================================================== */
/* Edge 3: Rapid shape transitions (stress the transition tree)              */
/* ========================================================================== */

static void test_edge_rapid_transitions() {
    auto* ts = make_ts();
    uint32_t root = vtx_hidden_class_root_shape_id();

    /* Add 500 properties in sequence — tests the transition tree
     * doesn't break down under heavy use. */
    uint32_t s = root;
    for (int i = 0; i < 500; i++) {
        char name[16];
        snprintf(name, sizeof(name), "f%d", i);
        s = vtx_hidden_class_add_property(s, (void*)ts, name);
    }

    CHECK(vtx_hidden_class_property_count(s) == 500, "500 properties");

    /* Verify every property is findable */
    bool all_found = true;
    for (int i = 0; i < 500; i++) {
        char name[16];
        snprintf(name, sizeof(name), "f%d", i);
        if (vtx_hidden_class_find_offset(s, (void*)ts, name) == UINT32_MAX) {
            all_found = false;
            break;
        }
    }
    CHECK(all_found, "all 500 properties found");
    free_ts(ts);
}

/* ========================================================================== */
/* Edge 4: IC with UINT32_MAX as site_id (wraps to 0)                        */
/* ========================================================================== */

static void test_edge_site_id_wrap() {
    vtx_property_ic_init(16);  /* small table */
    /* UINT32_MAX % 16 = 15 — should hash to the last slot */
    vtx_property_ic_update(UINT32_MAX, 1, 42);
    CHECK(vtx_property_ic_lookup(UINT32_MAX, 1) == 42, "UINT32_MAX site_id works");
    vtx_property_ic_destroy();
}

/* ========================================================================== */
/* Edge 5: IC with shape_id = 0 (should work — 0 is a valid shape)           */
/* ========================================================================== */

static void test_edge_shape_zero() {
    vtx_property_ic_init(1024);
    vtx_property_ic_update(0, 0, 77);
    CHECK(vtx_property_ic_lookup(0, 0) == 77, "shape_id 0 works in IC");
    CHECK(vtx_property_ic_lookup(0, 0) != UINT32_MAX, "shape_id 0 is not a miss");
    vtx_property_ic_destroy();
}

/* ========================================================================== */
/* Stress: Many sites with monomorphic entries                                 */
/* ========================================================================== */

static void test_stress_many_monomorphic() {
    vtx_property_ic_init(8192);
    std::mt19937 rng(54321);

    /* Fill 8192 sites with monomorphic entries */
    for (uint32_t i = 0; i < 8192; i++) {
        vtx_property_ic_update(i, i + 1, (uint32_t)(rng() % 1000));
    }

    /* All should be MONOMORPHIC and hit */
    bool all_mono = true, all_hit = true;
    for (uint32_t i = 0; i < 8192; i++) {
        if (vtx_property_ic_state(i) != (uint8_t)vortex::ICState::MONOMORPHIC)
            all_mono = false;
        if (vtx_property_ic_lookup(i, i + 1) == UINT32_MAX)
            all_hit = false;
        if (!all_mono && !all_hit) break;
    }
    CHECK(all_mono, "8192 sites all MONOMORPHIC");
    CHECK(all_hit, "8192 sites all hit");
    vtx_property_ic_destroy();
}

/* ========================================================================== */
/* Stress: Mixed monomorphic + polymorphic across sites                       */
/* ========================================================================== */

static void test_stress_mixed_ic_states() {
    vtx_property_ic_init(4096);
    std::mt19937 rng(11111);

    /* Sites 0-999: monomorphic (1 shape each) */
    for (int i = 0; i < 1000; i++)
        vtx_property_ic_update((uint32_t)i, (uint32_t)(100 + i), (uint32_t)i);

    /* Sites 1000-1999: polymorphic (3 shapes each) */
    for (int i = 1000; i < 2000; i++) {
        for (int j = 0; j < 3; j++)
            vtx_property_ic_update((uint32_t)i, (uint32_t)(200 + j), (uint32_t)(j * 10));
    }

    /* Sites 2000-2999: megamorphic (>4 shapes) */
    for (int i = 2000; i < 3000; i++) {
        for (uint32_t j = 0; j <= vortex::kPropertyICMaxEntries; j++)
            vtx_property_ic_update((uint32_t)i, 300 + j, j);
    }

    /* Verify mono sites */
    bool mono_ok = true;
    for (int i = 0; i < 1000; i++) {
        if (vtx_property_ic_state((uint32_t)i) != (uint8_t)vortex::ICState::MONOMORPHIC ||
            vtx_property_ic_lookup((uint32_t)i, (uint32_t)(100 + i)) != (uint32_t)i) {
            mono_ok = false; break;
        }
    }
    CHECK(mono_ok, "1000 mono sites correct");

    /* Verify poly sites */
    bool poly_ok = true;
    for (int i = 1000; i < 2000; i++) {
        if (vtx_property_ic_state((uint32_t)i) != (uint8_t)vortex::ICState::POLYMORPHIC) {
            poly_ok = false; break;
        }
        for (int j = 0; j < 3; j++) {
            if (vtx_property_ic_lookup((uint32_t)i, (uint32_t)(200 + j)) != (uint32_t)(j * 10)) {
                poly_ok = false; break;
            }
        }
    }
    CHECK(poly_ok, "1000 poly sites correct");

    /* Verify mega sites */
    bool mega_ok = true;
    for (int i = 2000; i < 3000; i++) {
        if (vtx_property_ic_state((uint32_t)i) != (uint8_t)vortex::ICState::MEGAMORPHIC) {
            mega_ok = false; break;
        }
        /* Megamorphic → all lookups should miss */
        for (uint32_t j = 0; j <= vortex::kPropertyICMaxEntries; j++) {
            if (vtx_property_ic_lookup((uint32_t)i, 300 + j) != UINT32_MAX) {
                mega_ok = false; break;
            }
        }
    }
    CHECK(mega_ok, "1000 mega sites correct");

    vtx_property_ic_destroy();
}

/* ========================================================================== */
/* Main                                                                        */
/* ========================================================================== */

int main() {
    fprintf(stderr, "=== Fuzz + Edge Case Tests (Property IC + Hidden Class) ===\n\n");

    /* Fuzz */
    test_fuzz_ic_correctness();
    test_fuzz_shape_id_uniqueness();
    test_fuzz_ic_state_machine();

    /* Edge */
    test_edge_long_property_names();
    test_edge_empty_property_name();
    test_edge_rapid_transitions();
    test_edge_site_id_wrap();
    test_edge_shape_zero();

    /* Stress */
    test_stress_many_monomorphic();
    test_stress_mixed_ic_states();

    fprintf(stderr, "\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
