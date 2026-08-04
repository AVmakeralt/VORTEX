// vortex/embed.cpp — C API implementation.
//
// Thin wrapper around the C++ embedding API. All C handles are
// opaque pointers to C++ objects.

#include "vortex/embed.h"
#include "vortex/runtime.hpp"
#include "vortex/bytecode.hpp"
#include "vortex/object.hpp"
#include "vortex/host_function.hpp"

#include <string>
#include <vector>
#include <cstdlib>
#include <optional>

// ---- Error handling ----

static thread_local std::string g_last_error;

const char* vtx_embed_last_error(void) {
    return g_last_error.empty() ? nullptr : g_last_error.c_str();
}

void vtx_embed_clear_error(void) {
    g_last_error.clear();
}

static void set_error(const std::string& msg) {
    g_last_error = msg;
}

// ---- Handle types (use optional for non-default-constructible types) ----

struct vtx_embed_runtime {
    std::optional<vortex::Runtime> rt;
};

struct vtx_embed_bytecode {
    std::optional<vortex::Bytecode> bc;
};

struct vtx_embed_object {
    vortex::Object obj;
};

struct vtx_embed_array {
    vortex::Array arr;
};

// ---- Runtime ----

vtx_embed_runtime_t* vtx_embed_runtime_create(void) {
    auto result = vortex::Runtime::create();
    if (!result) {
        set_error(result.error());
        return nullptr;
    }
    auto* h = new vtx_embed_runtime;
    h->rt = std::move(result.value());
    return h;
}

void vtx_embed_runtime_destroy(vtx_embed_runtime_t* rt) {
    delete rt;
}

int vtx_embed_runtime_enable_jit(vtx_embed_runtime_t* rt, uint32_t nthreads) {
    if (!rt || !rt->rt) { set_error("null runtime"); return -1; }
    rt->rt->enable_jit(nthreads);
    return 0;
}

int vtx_embed_runtime_compile_t1(vtx_embed_runtime_t* rt, vtx_embed_bytecode_t* bc) {
    if (!rt || !rt->rt || !bc || !bc->bc) { set_error("null runtime or bytecode"); return -1; }
    auto r = rt->rt->compile_t1(*bc->bc);
    if (!r) { set_error(r.error()); return -1; }
    return 0;
}

int vtx_embed_runtime_compile_t2(vtx_embed_runtime_t* rt, vtx_embed_bytecode_t* bc) {
    if (!rt || !rt->rt || !bc || !bc->bc) { set_error("null runtime or bytecode"); return -1; }
    auto r = rt->rt->compile_t2(*bc->bc);
    if (!r) { set_error(r.error()); return -1; }
    return 0;
}

vtx_embed_value_t vtx_embed_runtime_run(vtx_embed_runtime_t* rt,
                                          vtx_embed_bytecode_t* bc) {
    if (!rt || !rt->rt || !bc || !bc->bc) return VTX_EMBED_VALUE_UNDEFINED;
    return rt->rt->run(*bc->bc).raw();
}

vtx_embed_value_t vtx_embed_runtime_run_with_args(vtx_embed_runtime_t* rt,
                                                    vtx_embed_bytecode_t* bc,
                                                    const vtx_embed_value_t* args,
                                                    uint32_t arg_count) {
    if (!rt || !rt->rt || !bc || !bc->bc) return VTX_EMBED_VALUE_UNDEFINED;
    std::vector<vortex::Value> vargs;
    vargs.reserve(arg_count);
    for (uint32_t i = 0; i < arg_count; i++) {
        vargs.emplace_back(vortex::Value(args[i]));
    }
    return rt->rt->run_with_args(*bc->bc, vargs).raw();
}

vtx_embed_value_t vtx_embed_runtime_compile_and_run(vtx_embed_runtime_t* rt,
                                                      vtx_embed_bytecode_t* bc,
                                                      uint32_t tier) {
    if (!rt || !rt->rt || !bc || !bc->bc) return VTX_EMBED_VALUE_UNDEFINED;
    auto r = rt->rt->compile_and_run(*bc->bc, tier);
    if (!r) {
        set_error(r.error());
        return VTX_EMBED_VALUE_UNDEFINED;
    }
    return r.value().raw();
}

// ---- Bytecode ----

vtx_embed_bytecode_t* vtx_embed_bytecode_load(const char* path) {
    auto result = vortex::Bytecode::load(path);
    if (!result) {
        set_error(result.error());
        return nullptr;
    }
    auto* h = new vtx_embed_bytecode;
    h->bc = std::move(result.value());
    return h;
}

void vtx_embed_bytecode_destroy(vtx_embed_bytecode_t* bc) {
    delete bc;
}

static thread_local std::string g_disasm_buf;

const char* vtx_embed_bytecode_disassemble(vtx_embed_bytecode_t* bc) {
    if (!bc || !bc->bc) return nullptr;
    g_disasm_buf = bc->bc->disassemble();
    return g_disasm_buf.c_str();
}

// ---- Object ----

vtx_embed_object_t* vtx_embed_object_create(vtx_embed_runtime_t* rt,
                                               uint32_t max_fields) {
    if (!rt || !rt->rt) return nullptr;
    auto* h = new vtx_embed_object;
    h->obj = vortex::Object::create(*rt->rt, max_fields);
    if (!h->obj.valid()) {
        delete h;
        return nullptr;
    }
    return h;
}

vtx_embed_value_t vtx_embed_object_get(vtx_embed_runtime_t* rt,
                                         vtx_embed_object_t* obj,
                                         const char* name) {
    if (!rt || !rt->rt || !obj) return VTX_EMBED_VALUE_UNDEFINED;
    return obj->obj.get(*rt->rt, name).raw();
}

int vtx_embed_object_set(vtx_embed_runtime_t* rt,
                          vtx_embed_object_t* obj,
                          const char* name,
                          vtx_embed_value_t value) {
    if (!rt || !rt->rt || !obj) { set_error("null runtime or object"); return -1; }
    obj->obj.set(*rt->rt, name, vortex::Value(value));
    return 0;
}

bool vtx_embed_object_has(vtx_embed_runtime_t* rt,
                           vtx_embed_object_t* obj,
                           const char* name) {
    if (!rt || !rt->rt || !obj) return false;
    return obj->obj.has(*rt->rt, name);
}

bool vtx_embed_object_del(vtx_embed_runtime_t* rt,
                           vtx_embed_object_t* obj,
                           const char* name) {
    if (!rt || !rt->rt || !obj) return false;
    return obj->obj.del(*rt->rt, name);
}

vtx_embed_object_t* vtx_embed_object_prototype(vtx_embed_runtime_t* rt,
                                                 vtx_embed_object_t* obj) {
    if (!rt || !rt->rt || !obj) return nullptr;
    vortex::Object proto = obj->obj.prototype();
    if (!proto.valid()) return nullptr;
    auto* h = new vtx_embed_object;
    h->obj = proto;
    return h;
}

int vtx_embed_object_set_prototype(vtx_embed_runtime_t* rt,
                                    vtx_embed_object_t* obj,
                                    vtx_embed_object_t* proto) {
    if (!rt || !rt->rt || !obj) { set_error("null runtime or object"); return -1; }
    obj->obj.set_prototype(*rt->rt, proto ? proto->obj : vortex::Object(nullptr));
    return 0;
}

vtx_embed_value_t vtx_embed_object_as_value(vtx_embed_object_t* obj) {
    if (!obj) return VTX_EMBED_VALUE_UNDEFINED;
    return obj->obj.as_value();
}

// ---- Array ----

vtx_embed_array_t* vtx_embed_array_create(vtx_embed_runtime_t* rt,
                                            uint32_t length) {
    if (!rt || !rt->rt) return nullptr;
    auto* h = new vtx_embed_array;
    h->arr = vortex::Array::create(*rt->rt, length);
    if (!h->arr.valid()) {
        delete h;
        return nullptr;
    }
    return h;
}

uint32_t vtx_embed_array_length(vtx_embed_array_t* arr) {
    if (!arr) return 0;
    return arr->arr.length();
}

vtx_embed_value_t vtx_embed_array_get(vtx_embed_array_t* arr, uint32_t index) {
    if (!arr) return VTX_EMBED_VALUE_UNDEFINED;
    return arr->arr.get(index).raw();
}

int vtx_embed_array_set(vtx_embed_array_t* arr, uint32_t index,
                         vtx_embed_value_t value) {
    if (!arr) return -1;
    arr->arr.set(index, vortex::Value(value));
    return 0;
}

vtx_embed_value_t vtx_embed_array_as_value(vtx_embed_array_t* arr) {
    if (!arr) return VTX_EMBED_VALUE_UNDEFINED;
    return arr->arr.as_value();
}

// ---- Host functions ----

struct CHostFnWrapper {
    vtx_embed_host_fn fn;
    void* user_data;
};

static thread_local std::vector<CHostFnWrapper> g_host_fn_wrappers;

uint32_t vtx_embed_register_host_function(const char* name,
                                            vtx_embed_host_fn fn,
                                            void* user_data) {
    uint32_t id = g_host_fn_wrappers.size();
    g_host_fn_wrappers.push_back({fn, user_data});

    return vortex::HostFunctionRegistry::instance().register_function(
        name,
        [id](int argc, const vortex::Value* argv) -> vortex::Value {
            if (id >= g_host_fn_wrappers.size()) return vortex::Value::undefined();
            const auto& w = g_host_fn_wrappers[id];
            std::vector<vtx_embed_value_t> raw_args(argc);
            for (int i = 0; i < argc; i++) raw_args[i] = argv[i].raw();
            return vortex::Value(w.fn(argc, raw_args.data(), w.user_data));
        });
}

// ---- Value helpers ----

bool vtx_embed_value_is_int(vtx_embed_value_t v)       { return vortex::Value(v).is_int(); }
bool vtx_embed_value_is_double(vtx_embed_value_t v)    { return vortex::Value(v).is_double(); }
bool vtx_embed_value_is_bool(vtx_embed_value_t v)      { return vortex::Value(v).is_bool(); }
bool vtx_embed_value_is_null(vtx_embed_value_t v)      { return vortex::Value(v).is_null(); }
bool vtx_embed_value_is_undefined(vtx_embed_value_t v) { return vortex::Value(v).is_undefined(); }
bool vtx_embed_value_is_object(vtx_embed_value_t v)    { return vortex::Value(v).is_object(); }
bool vtx_embed_value_is_truthy(vtx_embed_value_t v)    { return vortex::Value(v).is_truthy(); }

vtx_embed_value_t vtx_embed_value_make_int(int64_t i)       { return vortex::Value::smi(i).raw(); }
vtx_embed_value_t vtx_embed_value_make_double(double d)      { return vortex::Value::double_val(d).raw(); }
vtx_embed_value_t vtx_embed_value_make_bool(bool b)          { return vortex::Value::boolean(b).raw(); }
vtx_embed_value_t vtx_embed_value_make_null(void)            { return VTX_EMBED_VALUE_NULL; }
vtx_embed_value_t vtx_embed_value_make_undefined(void)        { return VTX_EMBED_VALUE_UNDEFINED; }

int64_t vtx_embed_value_as_int(vtx_embed_value_t v)    { return vortex::Value(v).as_int(); }
double  vtx_embed_value_as_double(vtx_embed_value_t v) { return vortex::Value(v).as_double(); }
bool    vtx_embed_value_as_bool(vtx_embed_value_t v)   { return vortex::Value(v).as_bool(); }

int64_t vtx_embed_value_to_int(vtx_embed_value_t v)    { return vortex::Value(v).to_int(); }
double  vtx_embed_value_to_double(vtx_embed_value_t v){ return vortex::Value(v).to_double(); }

static thread_local std::string g_value_str_buf;

const char* vtx_embed_value_to_string(vtx_embed_value_t v) {
    g_value_str_buf = vortex::Value(v).to_string();
    return g_value_str_buf.c_str();
}

// ---- GC ----

void vtx_embed_gc_collect(vtx_embed_runtime_t* rt) {
    if (!rt || !rt->rt) return;
    rt->rt->gc_collect();
}

vtx_embed_heap_stats_t vtx_embed_heap_stats(vtx_embed_runtime_t* rt) {
    vtx_embed_heap_stats_t s{};
    if (!rt || !rt->rt) return s;
    auto stats = rt->rt->heap_stats();
    s.young_used = stats.young_used;
    s.young_size = stats.young_size;
    s.old_used = stats.old_used;
    s.old_size = stats.old_size;
    s.total_allocations = stats.total_allocations;
    s.total_collections = stats.total_collections;
    return s;
}
