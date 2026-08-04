// tests/test_embed.cpp — Test suite for the C++ embedding API.

#include <cassert>
#include <cstdio>
#include <cstring>
#include "vortex/runtime.hpp"
#include "vortex/bytecode.hpp"
#include "vortex/object.hpp"
#include "vortex/host_function.hpp"
#include "vortex/embed.h"

static int tests_pass = 0;
static int tests_fail = 0;

#define TEST(name) \
    do { \
        printf("  [TEST] %-40s ", name); \
        fflush(stdout); \
    } while(0)

#define PASS() do { printf("PASS\n"); tests_pass++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); tests_fail++; } while(0)

// ---- Value tests ----

static void test_value_smi() {
    TEST("Value::smi round-trip");
    auto v = vortex::Value::smi(42);
    if (v.is_int() && v.as_int() == 42) PASS();
    else FAIL("expected 42");
}

static void test_value_double() {
    TEST("Value::double_val round-trip");
    auto v = vortex::Value::double_val(3.14);
    if (v.is_double() && (v.as_double() - 3.14) < 1e-10 && (v.as_double() - 3.14) > -1e-10) PASS();
    else FAIL("expected 3.14");
}

static void test_value_bool() {
    TEST("Value::boolean round-trip");
    auto t = vortex::Value::boolean(true);
    auto f = vortex::Value::boolean(false);
    if (t.is_bool() && t.as_bool() && f.is_bool() && !f.as_bool()) PASS();
    else FAIL("expected true/false");
}

static void test_value_null_undefined() {
    TEST("Value::null and undefined");
    auto n = vortex::Value::null();
    auto u = vortex::Value::undefined();
    if (n.is_null() && u.is_undefined() && !n.is_undefined() && !u.is_null()) PASS();
    else FAIL("expected null/undefined distinct");
}

static void test_value_truthy() {
    TEST("Value::is_truthy (JS semantics)");
    bool ok = true;
    ok &= vortex::Value::smi(0).is_truthy() == false;
    ok &= vortex::Value::smi(42).is_truthy() == true;
    ok &= vortex::Value::boolean(false).is_truthy() == false;
    ok &= vortex::Value::boolean(true).is_truthy() == true;
    ok &= vortex::Value::null().is_truthy() == false;
    ok &= vortex::Value::undefined().is_truthy() == false;
    // NaN should be falsy (R8 fix)
    ok &= vortex::Value::double_val(std::numeric_limits<double>::quiet_NaN()).is_truthy() == false;
    if (ok) PASS();
    else FAIL("truthy semantics mismatch");
}

static void test_value_number() {
    TEST("Value::number (SMI vs double)");
    auto i = vortex::Value::number(42.0);
    auto d = vortex::Value::number(3.14);
    if (i.is_int() && i.as_int() == 42 && d.is_double()) PASS();
    else FAIL("expected 42 as SMI, 3.14 as double");
}

// ---- Object tests ----

static void test_object_create() {
    TEST("Object::create + set/get");
    auto rt_r = vortex::Runtime::create();
    assert(rt_r);
    vortex::Runtime rt = std::move(rt_r.value());

    auto obj = vortex::Object::create(rt, 16);
    if (!obj.valid()) { FAIL("create failed"); return; }

    obj.set(rt, "x", vortex::Value::smi(42));
    obj.set(rt, "y", vortex::Value::smi(99));

    auto x = obj.get(rt, "x");
    auto y = obj.get(rt, "y");
    if (x.to_int() == 42 && y.to_int() == 99) PASS();
    else FAIL("expected x=42, y=99");
}

static void test_object_prototype() {
    TEST("Object prototype chain");
    auto rt_r = vortex::Runtime::create();
    assert(rt_r);
    vortex::Runtime rt = std::move(rt_r.value());

    auto proto = vortex::Object::create(rt);
    proto.set(rt, "inherited", vortex::Value::smi(100));

    auto child = vortex::Object::create(rt);
    child.set(rt, "own", vortex::Value::smi(200));
    child.set_prototype(rt, proto);

    auto own_val = child.get(rt, "own");
    auto inh_val = child.get(rt, "inherited");
    if (own_val.to_int() == 200 && inh_val.to_int() == 100) PASS();
    else FAIL("prototype chain broken");
}

static void test_object_has_del() {
    TEST("Object has/del");
    auto rt_r = vortex::Runtime::create();
    assert(rt_r);
    vortex::Runtime rt = std::move(rt_r.value());

    auto obj = vortex::Object::create(rt);
    obj.set(rt, "key", vortex::Value::smi(1));

    if (!obj.has(rt, "key")) { FAIL("has(key) should be true"); return; }
    if (obj.has(rt, "missing")) { FAIL("has(missing) should be false"); return; }

    if (!obj.del(rt, "key")) { FAIL("del should return true"); return; }
    if (obj.has(rt, "key")) { FAIL("has(key) should be false after del"); return; }

    PASS();
}

// ---- Array tests ----

static void test_array() {
    TEST("Array create/get/set");
    auto rt_r = vortex::Runtime::create();
    assert(rt_r);
    vortex::Runtime rt = std::move(rt_r.value());

    auto arr = vortex::Array::create(rt, 5);
    if (!arr.valid()) { FAIL("create failed"); return; }
    if (arr.length() != 5) { FAIL("length should be 5"); return; }

    for (uint32_t i = 0; i < 5; i++) {
        arr.set(i, vortex::Value::smi(i * i));
    }
    for (uint32_t i = 0; i < 5; i++) {
        if (arr.get(i).to_int() != (int64_t)(i * i)) {
            FAIL("element mismatch");
            return;
        }
    }
    PASS();
}

// ---- Host function tests ----

static void test_host_function() {
    TEST("HostFunction registration + call");
    auto id = vortex::register_host_function("double_it",
        [](int argc, const vortex::Value* argv) -> vortex::Value {
            if (argc < 1) return vortex::Value::undefined();
            return vortex::Value::smi(argv[0].to_int() * 2);
        });

    vortex::Value argv[1] = { vortex::Value::smi(21) };
    auto result = vortex::HostFunctionRegistry::instance().call(id, 1, argv);
    if (result.to_int() == 42) PASS();
    else FAIL("expected 42");
}

// ---- C API tests ----

static void test_c_api() {
    TEST("C API (vortex_embed.h)");
    vtx_embed_runtime_t* rt = vtx_embed_runtime_create();
    if (!rt) { FAIL("runtime create failed"); return; }

    // Value helpers
    vtx_embed_value_t smi = vtx_embed_value_make_int(42);
    if (!vtx_embed_value_is_int(smi) || vtx_embed_value_as_int(smi) != 42) {
        FAIL("smi round-trip"); vtx_embed_runtime_destroy(rt); return;
    }

    // Object
    vtx_embed_object_t* obj = vtx_embed_object_create(rt, 16);
    if (!obj) { FAIL("object create"); vtx_embed_runtime_destroy(rt); return; }

    vtx_embed_object_set(rt, obj, "key", vtx_embed_value_make_int(99));
    vtx_embed_value_t val = vtx_embed_object_get(rt, obj, "key");
    if (vtx_embed_value_as_int(val) != 99) { FAIL("get/set"); }

    if (!vtx_embed_object_has(rt, obj, "key")) { FAIL("has"); }
    if (!vtx_embed_object_del(rt, obj, "key")) { FAIL("del"); }
    if (vtx_embed_object_has(rt, obj, "key")) { FAIL("has after del"); }

    vtx_embed_runtime_destroy(rt);
    PASS();
}

// ---- Main ----

#include <limits>

int main() {
    printf("=== VORTEX C++ Embedding API Tests ===\n\n");

    test_value_smi();
    test_value_double();
    test_value_bool();
    test_value_null_undefined();
    test_value_truthy();
    test_value_number();

    printf("\n--- Object/Array ---\n");
    test_object_create();
    test_object_prototype();
    test_object_has_del();
    test_array();

    printf("\n--- Host Functions ---\n");
    test_host_function();

    printf("\n--- C API ---\n");
    test_c_api();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_pass, tests_fail);
    return tests_fail == 0 ? 0 : 1;
}
