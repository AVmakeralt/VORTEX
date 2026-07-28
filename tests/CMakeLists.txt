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
    -Wl,--start-group
    vortex_test_framework
    vortex_ir
    vortex_interp
    vortex_runtime
    vortex_baseline
    vortex_compile
    vortex_lower
    vortex_codecache
    vortex_deopt
    vortex_guard
    vortex_profile
    vortex_inliner
    vortex_pea
    vortex_midtier
    -Wl,--end-group
    m
    pthread
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
)
target_link_libraries(test_zero_cost_deopt PRIVATE
-Wl,--start-group
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
-Wl,--end-group
)
add_test(NAME test_zero_cost_deopt COMMAND test_zero_cost_deopt)

# test_p2_features — P2 feature verification
add_executable(test_p2_features
    unit/test_p2_features.c
)
target_link_libraries(test_p2_features PRIVATE
-Wl,--start-group
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
-Wl,--end-group
)
add_test(NAME test_p2_features COMMAND test_p2_features)

# test_smi_arith — SMI arithmetic correctness (encoding, interpreter, T2 JIT)
add_executable(test_smi_arith
    unit/test_smi_arith.c
)
target_link_libraries(test_smi_arith PRIVATE
-Wl,--start-group
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
    vortex_lower
    vortex_inliner
    vortex_pea
    vortex_sota
    vortex_midtier
    vortex_trace
    vortex_region
    -Wl,--end-group
    m
    pthread
-Wl,--end-group
)
add_test(NAME test_smi_arith COMMAND test_smi_arith)

# ---- Integration tests ----

# test_interp_basics
add_executable(test_interp_basics
    integration/test_interp_basics.c
)
target_link_libraries(test_interp_basics PRIVATE
-Wl,--start-group
    vortex_test_framework
    -Wl,--start-group
    vortex_compile
    vortex_baseline
    vortex_lower
    vortex_codecache
    vortex_interp
    vortex_runtime
    vortex_ir
    vortex_guard
    vortex_deopt
    vortex_profile
    vortex_inliner
    vortex_pea
    -Wl,--end-group
    m
    pthread
-Wl,--end-group
)
add_test(NAME test_interp_basics COMMAND test_interp_basics)

# test_jit_e2e — End-to-end JIT compilation and execution tests
add_executable(test_jit_e2e
    integration/test_jit_e2e.c
)
target_link_libraries(test_jit_e2e PRIVATE
-Wl,--start-group
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
    vortex_lower
    vortex_inliner
    vortex_pea
    vortex_sota
    vortex_midtier
    vortex_trace
    vortex_region
    -Wl,--end-group
    m
    pthread
-Wl,--end-group
)
add_test(NAME test_jit_e2e COMMAND test_jit_e2e)

# ---- Fuzz tests ----

# fuzz_graph_build: 500+ random valid bytecode programs through vtx_graph_build()
add_executable(fuzz_graph_build
    fuzz/fuzz_graph_build.c
)
target_link_libraries(fuzz_graph_build PRIVATE
    vortex_test_framework
    vortex_ir
    vortex_runtime
    m
)
add_test(NAME fuzz_graph_build COMMAND fuzz_graph_build)
set_tests_properties(fuzz_graph_build PROPERTIES TIMEOUT 60)

# test_assembler_prog: program-mode assembler (labels + pseudo-instructions)
add_executable(test_assembler_prog
    unit/test_assembler_prog.c
)
target_link_libraries(test_assembler_prog PRIVATE
    vortex_test_framework
    vortex_ir
    vortex_runtime
    vortex_assembler
    m
)
add_test(NAME test_assembler_prog COMMAND test_assembler_prog)

# test_tier_equivalence: 30+ programs, T0 interpreter == T2 JIT result
add_executable(test_tier_equivalence
    integration/test_tier_equivalence.c
)
target_link_libraries(test_tier_equivalence PRIVATE
-Wl,--start-group
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
    vortex_lower
    vortex_inliner
    vortex_pea
    vortex_sota
    vortex_midtier
    vortex_trace
    vortex_region
    vortex_assembler
    -Wl,--end-group
    m
    pthread
-Wl,--end-group
)
add_test(NAME test_tier_equivalence COMMAND test_tier_equivalence)
set_tests_properties(test_tier_equivalence PROPERTIES TIMEOUT 120)

# test_deopt_rate_limit: per-site, batcher, and global budget primitives
add_executable(test_deopt_rate_limit
    unit/test_deopt_rate_limit.c
)
target_link_libraries(test_deopt_rate_limit PRIVATE
    vortex_test_framework
    vortex_deopt
    m
)
add_test(NAME test_deopt_rate_limit COMMAND test_deopt_rate_limit)

# test_deopt_coordinator: integration of rate_limit into deopt decision path
add_executable(test_deopt_coordinator
    unit/test_deopt_coordinator.c
)
target_link_libraries(test_deopt_coordinator PRIVATE
    vortex_test_framework
    vortex_deopt
    m
)
add_test(NAME test_deopt_coordinator COMMAND test_deopt_coordinator)

# test_versioned_cache: N+1 versioning, patching, reclamation
add_executable(test_versioned_cache
    unit/test_versioned_cache.c
)
target_link_libraries(test_versioned_cache PRIVATE
    vortex_test_framework
    vortex_codecache
    m
    pthread
)
add_test(NAME test_versioned_cache COMMAND test_versioned_cache)

# test_regalloc_upgrade: loop-boundary splitting, rematerialization
add_executable(test_regalloc_upgrade
    unit/test_regalloc_upgrade.c
)
target_link_libraries(test_regalloc_upgrade PRIVATE
    -Wl,--start-group
    vortex_test_framework
    vortex_lower
    vortex_compile
    vortex_ir
    vortex_runtime
    vortex_interp
    vortex_baseline
    vortex_deopt
    vortex_codecache
    vortex_guard
    vortex_profile
    vortex_inliner
    vortex_pea
    vortex_midtier
    -Wl,--end-group
    m
    pthread
)
add_test(NAME test_regalloc_upgrade COMMAND test_regalloc_upgrade)

# test_safepoint_manager: thread-safe GC safepoint coordination
add_executable(test_safepoint_manager
    unit/test_safepoint_manager.c
)
target_link_libraries(test_safepoint_manager PRIVATE
    vortex_test_framework
    vortex_runtime
    m
    pthread
)
add_test(NAME test_safepoint_manager COMMAND test_safepoint_manager)
set_tests_properties(test_safepoint_manager PROPERTIES TIMEOUT 30)

# test_pgo_sprint1: PGO stability features (confidence, hysteresis, backpressure, deterministic)
add_executable(test_pgo_sprint1
    unit/test_pgo_sprint1.c
)
target_link_libraries(test_pgo_sprint1 PRIVATE
    vortex_test_framework
    vortex_profile
    vortex_sota
    vortex_runtime
    m
    pthread
)
add_test(NAME test_pgo_sprint1 COMMAND test_pgo_sprint1)
set_tests_properties(test_pgo_sprint1 PROPERTIES TIMEOUT 30)

# test_pgo_sprint2: PGO phase partitioning (per-phase storage, transition, persistence)
add_executable(test_pgo_sprint2
    unit/test_pgo_sprint2.c
)
target_link_libraries(test_pgo_sprint2 PRIVATE
    vortex_test_framework
    vortex_profile
    vortex_runtime
    m
    pthread
)
add_test(NAME test_pgo_sprint2 COMMAND test_pgo_sprint2)
set_tests_properties(test_pgo_sprint2 PROPERTIES TIMEOUT 30)

# test_pgo_sprint3: PGO ensemble profiles (robust aggregate, demotion, rollback)
add_executable(test_pgo_sprint3
    unit/test_pgo_sprint3.c
)
target_link_libraries(test_pgo_sprint3 PRIVATE
    vortex_test_framework
    vortex_profile
    vortex_runtime
    m
    pthread
)
add_test(NAME test_pgo_sprint3 COMMAND test_pgo_sprint3)
set_tests_properties(test_pgo_sprint3 PROPERTIES TIMEOUT 30)

# test_pgo_sprint4: PGO input-shape-keyed profiles (novel differentiator)
add_executable(test_pgo_sprint4
    unit/test_pgo_sprint4.c
)
target_link_libraries(test_pgo_sprint4 PRIVATE
    vortex_test_framework
    vortex_profile
    vortex_runtime
    m
    pthread
)
add_test(NAME test_pgo_sprint4 COMMAND test_pgo_sprint4)
set_tests_properties(test_pgo_sprint4 PROPERTIES TIMEOUT 30)

# test_pgo_sprint5: PGO T1 code persistence (cold-start killer)
add_executable(test_pgo_sprint5
    unit/test_pgo_sprint5.c
)
target_link_libraries(test_pgo_sprint5 PRIVATE
    vortex_test_framework
    vortex_codecache
    vortex_profile
    vortex_runtime
    vortex_lower
    vortex_ir
    vortex_deopt
    m
    pthread
)
add_test(NAME test_pgo_sprint5 COMMAND test_pgo_sprint5)
set_tests_properties(test_pgo_sprint5 PROPERTIES TIMEOUT 30)

# test_pgo_sprint6: PGO profile patching (append-only updates, crash recovery)
add_executable(test_pgo_sprint6
    unit/test_pgo_sprint6.c
)
target_link_libraries(test_pgo_sprint6 PRIVATE
    vortex_test_framework
    vortex_profile
    vortex_runtime
    m
    pthread
)
add_test(NAME test_pgo_sprint6 COMMAND test_pgo_sprint6)
set_tests_properties(test_pgo_sprint6 PROPERTIES TIMEOUT 30)

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
)
target_link_libraries(test_stress_compile_lower PRIVATE
-Wl,--start-group
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
-Wl,--end-group
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

# ---- Opcode tests (B4-B8 patches + extras) ----
# test_opcodes_b4_b8: Validates B4 (InstanceOf), B5 (IsNull), B6 (NewArray),
#   B7 (CallStatic), B8 (CallVirtual) bugfixes, plus NEW, LOAD_FIELD/STORE_FIELD,
#   ARRAY_LOAD/STORE/LENGTH, THROW/CATCH.
# Built with the manual build script at /home/z/my-project/scripts/build_opcode_test.sh
# when cmake is unavailable.
add_executable(test_opcodes_b4_b8
    opcode/test_opcodes_b4_b8.c
)
target_link_libraries(test_opcodes_b4_b8 PRIVATE
-Wl,--start-group
    vortex_test_framework
    vortex_interp
    vortex_ir
    vortex_runtime
    vortex_baseline
    vortex_trace
    vortex_region
    vortex_pea
    vortex_deopt
    vortex_inliner
    vortex_profile
    vortex_lower
    vortex_codecache
    vortex_compile
    vortex_guard
    vortex_midtier
    vortex_sota
    vortex_common
    m
    pthread
-Wl,--end-group
)
add_test(NAME test_opcodes_b4_b8 COMMAND test_opcodes_b4_b8)
