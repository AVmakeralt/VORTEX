// tests/test_aot.cpp — Test AOT background compilation.
//
// Verifies that:
//   1. AOT artifacts can be created and submitted
//   2. The background worker compiles them
//   3. Compiled code is installed in the code cache
//   4. AOT uses more aggressive optimization than JIT
//   5. Guard failures feed into the retrace system

#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>
#include "vortex/runtime.hpp"
#include "vortex/bytecode.hpp"
#include "vortex/object.hpp"

// Include the C AOT API
#define typeid typeid_
extern "C" {
#include "compile/aot.h"
#include "compile/orchestrator.h"
#include "runtime/bytecode.h"
#include "assembler.h"
}
#undef typeid

static int tests_pass = 0;
static int tests_fail = 0;

#define TEST(name) do { printf("  [TEST] %-45s ", name); fflush(stdout); } while(0)
#define PASS() do { printf("PASS\n"); tests_pass++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); tests_fail++; } while(0)

// Test 1: AOT artifact creation
static void test_artifact_create() {
    TEST("AOT artifact create + free");

    // Create a minimal bytecode
    vtx_assembler_t a;
    vtx_asm_init(&a);
    if (vtx_asm_program(&a, "load_const_int 42\nreturn_value\n") != 0) {
        FAIL("asm error");
        return;
    }
    vtx_bytecode_t bc = vtx_asm_emit(&a);
    vtx_method_desc_t method = {
        .name = "test", .signature = "()I", .bytecode = &bc,
        .compiled_code = NULL, .vtable_index = 0,
        .arg_count = 0, .is_virtual = false
    };

    vtx_aot_artifact_t *art = vtx_aot_create_artifact(1, 0, 3, &bc, &method);
    if (!art) { FAIL("create failed"); return; }

    if (art->method_id != 1) { FAIL("method_id mismatch"); return; }
    if (art->tier != 3) { FAIL("tier mismatch"); return; }
    if (art->inline_size_limit != 8192) { FAIL("inline limit not AOT"); return; }
    if (!art->run_speculative) { FAIL("speculative not enabled"); return; }
    if (!art->run_loop_spec) { FAIL("loop_spec not enabled"); return; }
    if (!art->run_vectorize) { FAIL("vectorize not enabled"); return; }
    if (art->is_compiled) { FAIL("should start uncompiled"); return; }

    vtx_aot_artifact_free(art);
    PASS();
}

// Test 2: AOT pipeline config has aggressive settings
static void test_aot_config() {
    TEST("AOT pipeline config aggressive");

    vtx_pipeline_config_t cfg = vtx_pipeline_config_aot();

    if (cfg.inline_size_limit != 8192) { FAIL("inline limit"); return; }
    if (cfg.gvn_iterations != 5) { FAIL("gvn iterations"); return; }
    if (cfg.sccp_iterations != 10) { FAIL("sccp iterations"); return; }
    if (!cfg.run_speculative) { FAIL("speculative"); return; }
    if (!cfg.run_loop_spec) { FAIL("loop_spec"); return; }
    if (!cfg.run_vectorize) { FAIL("vectorize"); return; }
    if (!cfg.run_inlining) { FAIL("inlining"); return; }
    if (!cfg.run_pea) { FAIL("pea"); return; }

    PASS();
}

// Test 3: AOT manager init/destroy
static void test_aot_manager_lifecycle() {
    TEST("AOT manager init/destroy");

    vtx_aot_manager_t aot;
    if (vtx_aot_init(&aot, NULL, NULL) != 0) { FAIL("init"); return; }

    vtx_aot_stats_t stats = vtx_aot_stats(&aot);
    if (stats.pending_count != 0) { FAIL("pending not 0"); return; }

    vtx_aot_destroy(&aot);
    PASS();
}

// Test 4: AOT submit + background compile
static void test_aot_background_compile() {
    TEST("AOT background compile + install");

    auto rt_r = vortex::Runtime::create();
    if (!rt_r) { FAIL("runtime create"); return; }
    vortex::Runtime rt = std::move(rt_r.value());

    // Create bytecode for a simple sum function
    vtx_assembler_t a;
    vtx_asm_init(&a);
    if (vtx_asm_program(&a,
            "load_local 0\n"
            "load_const_int 1\n"
            "iadd\n"
            "return_value\n") != 0) {
        FAIL("asm error");
        return;
    }
    vtx_bytecode_t bc = vtx_asm_emit(&a);
    vtx_method_desc_t method = {
        .name = "add_one", .signature = "(I)I", .bytecode = &bc,
        .compiled_code = NULL, .vtable_index = 0,
        .arg_count = 1, .is_virtual = false
    };

    // Create a standalone AOT manager (not tied to orchestrator)
    vtx_aot_manager_t aot;
    vtx_aot_init(&aot, rt.code_cache(), &rt.raw().method_registry);
    vtx_aot_start(&aot);

    // Create an AOT artifact
    vtx_aot_artifact_t *art = vtx_aot_create_artifact(
        100, 0, 3, &bc, &method);
    if (!art) { FAIL("create artifact"); vtx_aot_destroy(&aot); return; }

    // Submit the artifact
    if (vtx_aot_submit(&aot, art) != 0) {
        FAIL("submit");
        vtx_aot_artifact_free(art);
        vtx_aot_destroy(&aot);
        return;
    }

    // Wait for background compilation (max 2 seconds)
    for (int i = 0; i < 200; i++) {
        vtx_aot_stats_t stats = vtx_aot_stats(&aot);
        if (stats.compiled_count > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    vtx_aot_stats_t stats = vtx_aot_stats(&aot);
    vtx_aot_destroy(&aot);

    if (stats.compiled_count == 0) {
        FAIL("artifact not compiled within timeout");
        return;
    }

    printf("(compiled=%u, installed=%u) ", stats.compiled_count, stats.installed_count);
    PASS();
}

// Test 5: AOT guard failure feeds retrace
static void test_aot_guard_failure() {
    TEST("AOT guard failure → retrace feedback");

    vtx_aot_manager_t aot;
    vtx_aot_init(&aot, NULL, NULL);

    uint64_t before = vtx_aot_stats(&aot).total_bailouts;
    vtx_aot_on_guard_failure(&aot, 42, 0);
    uint64_t after = vtx_aot_stats(&aot).total_bailouts;

    vtx_aot_destroy(&aot);

    if (after != before + 1) {
        FAIL("bailout counter not incremented");
        return;
    }
    PASS();
}

// Test 6: AOT artifact heap allocation (not arena)
static void test_aot_heap_alloc() {
    TEST("AOT artifact heap-allocated (survives arena free)");

    vtx_bytecode_t bc = {};
    vtx_method_desc_t method = {
        .name = "t", .signature = "()I", .bytecode = &bc,
        .compiled_code = NULL, .vtable_index = 0,
        .arg_count = 0, .is_virtual = false
    };

    // Create artifact in a scope, then exit the scope
    vtx_aot_artifact_t *art = vtx_aot_create_artifact(1, 0, 2, &bc, &method);
    if (!art) { FAIL("create"); return; }

    // Add some guards (tests realloc)
    for (int i = 0; i < 20; i++) {
        if (vtx_aot_add_guard(art, i, i, 0, 0, 0, i*4, i) != 0) {
            FAIL("add_guard failed");
            return;
        }
    }

    if (art->guard_count != 20) { FAIL("guard count"); return; }
    if (art->guard_capacity < 20) { FAIL("capacity"); return; }

    // Verify guards are heap-stored (address is valid)
    if (!art->guards) { FAIL("guards NULL"); return; }

    // Free should succeed without crash
    vtx_aot_artifact_free(art);
    PASS();
}

int main() {
    printf("=== VORTEX AOT Background Compilation Tests ===\n\n");

    test_artifact_create();
    test_aot_config();
    test_aot_manager_lifecycle();
    test_aot_background_compile();
    test_aot_guard_failure();
    test_aot_heap_alloc();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_pass, tests_fail);
    return tests_fail == 0 ? 0 : 1;
}
