# VORTEX Tests CMake

# Test framework library
add_library(vortex_test_framework STATIC
    test_framework.c
)
target_include_directories(vortex_test_framework PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${VORTEX_INCLUDE_DIRS}
)

# ---- Unit tests ----

# test_arena
add_executable(test_arena
    unit/test_arena.c
)
target_link_libraries(test_arena PRIVATE vortex_test_framework vortex_common)
add_test(NAME test_arena COMMAND test_arena)

# test_object
add_executable(test_object
    unit/test_object.c
)
target_link_libraries(test_object PRIVATE vortex_test_framework vortex_runtime)
add_test(NAME test_object COMMAND test_object)

# test_bytecode
add_executable(test_bytecode
    unit/test_bytecode.c
)
target_link_libraries(test_bytecode PRIVATE vortex_test_framework vortex_runtime)
add_test(NAME test_bytecode COMMAND test_bytecode)

# test_type_system
add_executable(test_type_system
    unit/test_type_system.c
)
target_link_libraries(test_type_system PRIVATE vortex_test_framework vortex_runtime)
add_test(NAME test_type_system COMMAND test_type_system)

# test_node
add_executable(test_node
    unit/test_node.c
)
target_link_libraries(test_node PRIVATE vortex_test_framework vortex_ir)
add_test(NAME test_node COMMAND test_node)

# test_graph
add_executable(test_graph
    unit/test_graph.c
)
target_link_libraries(test_graph PRIVATE vortex_test_framework vortex_ir vortex_runtime)
add_test(NAME test_graph COMMAND test_graph)

# test_bugfixes — comprehensive bug fix verification
add_executable(test_bugfixes
    unit/test_bugfixes.c
)
target_link_libraries(test_bugfixes PRIVATE
    vortex_test_framework
    vortex_ir
    vortex_interp
    vortex_runtime
    m
)
add_test(NAME test_bugfixes COMMAND test_bugfixes)

# test_ir_bugfixes — IR-specific edge case tests for bug audit
add_executable(test_ir_bugfixes
    unit/test_ir_bugfixes.c
)
target_link_libraries(test_ir_bugfixes PRIVATE
    vortex_test_framework
    vortex_ir
    vortex_runtime
    m
)
add_test(NAME test_ir_bugfixes COMMAND test_ir_bugfixes)

# test_zero_cost_deopt — zero-cost deopt feature verification
add_executable(test_zero_cost_deopt
    unit/test_zero_cost_deopt.c
    ${CMAKE_SOURCE_DIR}/src/runtime_stubs.c
)
target_link_libraries(test_zero_cost_deopt PRIVATE
    vortex_test_framework
    -Wl,--start-group
    vortex_guard
    vortex_compile
    vortex_lower
    vortex_codecache
    vortex_deopt
    vortex_ir
    vortex_interp
    vortex_runtime
    vortex_profile
    vortex_baseline
    vortex_inliner
    vortex_pea
    -Wl,--end-group
    m
    pthread
)
add_test(NAME test_zero_cost_deopt COMMAND test_zero_cost_deopt)

# test_p2_features — P2 feature verification (guard-page type checking,
# guard dependency graph, phase-reactive reactivation, speculative PEA + lock elision)
add_executable(test_p2_features
    unit/test_p2_features.c
    ${CMAKE_SOURCE_DIR}/src/runtime_stubs.c
)
target_link_libraries(test_p2_features PRIVATE
    vortex_test_framework
    -Wl,--start-group
    vortex_guard
    vortex_compile
    vortex_lower
    vortex_codecache
    vortex_deopt
    vortex_ir
    vortex_interp
    vortex_runtime
    vortex_profile
    vortex_baseline
    vortex_inliner
    vortex_pea
    -Wl,--end-group
    m
    pthread
)
add_test(NAME test_p2_features COMMAND test_p2_features)

# ---- Integration tests ----

# test_interp_basics
add_executable(test_interp_basics
    integration/test_interp_basics.c
)
target_link_libraries(test_interp_basics PRIVATE
    vortex_test_framework
    vortex_compile
    vortex_interp
    vortex_runtime
    m
    pthread
)
add_test(NAME test_interp_basics COMMAND test_interp_basics)

# test_jit_e2e — End-to-end JIT compilation and execution tests
add_executable(test_jit_e2e
    integration/test_jit_e2e.c
    ${CMAKE_SOURCE_DIR}/src/runtime_stubs.c
)
target_link_libraries(test_jit_e2e PRIVATE
    vortex_test_framework
    -Wl,--start-group
    vortex_baseline
    vortex_codecache
    vortex_compile
    vortex_interp
    vortex_runtime
    vortex_ir
    vortex_guard
    vortex_deopt
    vortex_profile
    -Wl,--end-group
    m
    pthread
)
add_test(NAME test_jit_e2e COMMAND test_jit_e2e)

# ---- Stress tests (1000 tests) ----

# test_stress_runtime: Arena, Object, Type System, Bytecode, GC (200 tests)
add_executable(test_stress_runtime
    stress/test_stress_runtime.c
)
target_link_libraries(test_stress_runtime PRIVATE
    vortex_test_framework
    vortex_runtime
    m
)
add_test(NAME test_stress_runtime COMMAND test_stress_runtime)

# test_stress_ir: Node, Graph, GVN, SCCP, DCE, Schedule, TBAA, LICM, Bounds, Induction (200 tests)
add_executable(test_stress_ir
    stress/test_stress_ir.c
)
target_link_libraries(test_stress_ir PRIVATE
    vortex_test_framework
    vortex_ir
    vortex_runtime
    m
)
add_test(NAME test_stress_ir COMMAND test_stress_ir)

# test_stress_profile_trace: Profile, Trace, Region, Guard, Deopt (200 tests)
add_executable(test_stress_profile_trace
    stress/test_stress_profile_trace.c
)
target_link_libraries(test_stress_profile_trace PRIVATE
    vortex_test_framework
    vortex_profile
    vortex_trace
    vortex_region
    vortex_guard
    vortex_deopt
    vortex_ir
    vortex_interp
    vortex_runtime
    m
)
add_test(NAME test_stress_profile_trace COMMAND test_stress_profile_trace)

# test_stress_compile_lower: Compile, Inliner, Lower, CodeCache, SOTA, PEA (200 tests)
add_executable(test_stress_compile_lower
    stress/test_stress_compile_lower.c
    ${CMAKE_SOURCE_DIR}/src/runtime_stubs.c
)
target_link_libraries(test_stress_compile_lower PRIVATE
    vortex_test_framework
    vortex_compile
    vortex_inliner
    vortex_lower
    vortex_codecache
    vortex_sota
    vortex_pea
    vortex_ir
    vortex_runtime
    vortex_profile
    vortex_deopt
    vortex_guard
    vortex_interp
    vortex_baseline
    m
)
add_test(NAME test_stress_compile_lower COMMAND test_stress_compile_lower)

# test_stress_integration: Full pipeline, Interpreter, Profile+Graph, Deopt+Graph, Multi-subsystem (200 tests)
add_executable(test_stress_integration
    stress/test_stress_integration.c
)
target_link_libraries(test_stress_integration PRIVATE
    vortex_test_framework
    vortex_interp
    vortex_ir
    vortex_runtime
    vortex_profile
    vortex_guard
    vortex_deopt
    vortex_trace
    vortex_region
    m
    pthread
)
add_test(NAME test_stress_integration COMMAND test_stress_integration)
