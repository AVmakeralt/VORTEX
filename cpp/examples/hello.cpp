// examples/hello.cpp — VORTEX C++ embedding example.
//
// Shows: Runtime creation, JIT, bytecode loading, dynamic objects,
// prototype chains, and host function registration.

#include <cstdio>
#include <iostream>
#include "vortex/runtime.hpp"
#include "vortex/bytecode.hpp"
#include "vortex/object.hpp"
#include "vortex/host_function.hpp"

int main() {
    // 1. Create runtime
    auto rt_result = vortex::Runtime::create();
    if (!rt_result) {
        std::cerr << "Failed to create runtime: " << rt_result.error() << "\n";
        return 1;
    }
    vortex::Runtime rt = std::move(rt_result.value());

    // 2. Enable JIT (2 compile threads)
    rt.enable_jit(2);

    // 3. Register a host function callable from bytecode
    uint32_t add_id = vortex::register_host_function("print_int",
        [](int argc, const vortex::Value* argv) -> vortex::Value {
            if (argc >= 1) {
                std::cout << "[host] result = " << argv[0].to_int() << "\n";
            }
            return vortex::Value::undefined();
        });
    std::cout << "Registered print_int as function ID " << add_id << "\n";

    // 4. Create dynamic objects with prototype chain
    vortex::Object animal = vortex::Object::create(rt);
    animal.set(rt, "legs", vortex::Value::smi(4));
    animal.set(rt, "sound", vortex::Value::smi(0));  // placeholder

    vortex::Object dog = vortex::Object::create(rt);
    dog.set(rt, "breed", vortex::Value::smi(1));
    dog.set_prototype(rt, animal);

    // 5. Test property access (including prototype chain)
    std::cout << "\n=== Property Access ===\n";
    std::cout << "dog.legs (inherited) = " << dog.get(rt, "legs").to_int() << "\n";
    std::cout << "dog.breed (own)      = " << dog.get(rt, "breed").to_int() << "\n";
    std::cout << "dog.sound (inherited) = " << dog.get(rt, "sound").to_int() << "\n";
    std::cout << "dog.missing           = " << dog.get(rt, "missing").to_string() << "\n";

    // 6. Test array
    std::cout << "\n=== Arrays ===\n";
    vortex::Array arr = vortex::Array::create(rt, 5);
    for (uint32_t i = 0; i < 5; i++) {
        arr.set(i, vortex::Value::smi(i * i));
    }
    std::cout << "Array length: " << arr.length() << "\n";
    for (uint32_t i = 0; i < arr.length(); i++) {
        std::cout << "  arr[" << i << "] = " << arr.get(i).to_int() << "\n";
    }

    // 7. Test values
    std::cout << "\n=== Values ===\n";
    auto smi_val   = vortex::Value::smi(42);
    auto dbl_val   = vortex::Value::double_val(3.14);
    auto bool_val  = vortex::Value::boolean(true);
    auto null_val  = vortex::Value::null();
    auto undef_val = vortex::Value::undefined();

    std::cout << "smi(42)     = " << smi_val.to_string() << " (is_int=" << smi_val.is_int() << ")\n";
    std::cout << "double(3.14) = " << dbl_val.to_string() << " (is_double=" << dbl_val.is_double() << ")\n";
    std::cout << "bool(true)  = " << bool_val.to_string() << " (is_bool=" << bool_val.is_bool() << ")\n";
    std::cout << "null        = " << null_val.to_string() << " (is_null=" << null_val.is_null() << ")\n";
    std::cout << "undefined   = " << undef_val.to_string() << " (is_undef=" << undef_val.is_undefined() << ")\n";

    // 8. Heap stats
    std::cout << "\n=== Heap Stats ===\n";
    auto stats = rt.heap_stats();
    std::cout << "young: " << stats.young_used << "/" << stats.young_size << " bytes\n";
    std::cout << "old:   " << stats.old_used << "/" << stats.old_size << " bytes\n";
    std::cout << "allocations: " << stats.total_allocations << "\n";
    std::cout << "collections: " << stats.total_collections << "\n";

    std::cout << "\n=== Done ===\n";
    return 0;
}
