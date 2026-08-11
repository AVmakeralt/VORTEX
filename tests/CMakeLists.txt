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
    $<$<BOOL:${VORTEX_ENABLE_SOTA}>:vortex_sota>
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
    $<$<BOOL:${VORTEX_ENABLE_SOTA}>:vortex_sota>
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
    $<$<BOOL:${VORTEX_ENABLE_SOTA}>:vortex_sota>
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
    $<$<BOOL:${VORTEX_ENABLE_SOTA}>:vortex_sota>
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
    vortex_codecache
    m
    pthread
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
    $<$<BOOL:${VORTEX_ENABLE_SOTA}>:vortex_sota>
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
    vortex_codecache
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
    $<$<BOOL:${VORTEX_ENABLE_SOTA}>:vortex_sota>
    vortex_common
    m
    pthread
-Wl,--end-group
)
add_test(NAME test_opcodes_b4_b8 COMMAND test_opcodes_b4_b8)

# ==========================================================================
# TEST-001 fix: Build the orphaned test files in tests/opcode/ that were
# previously not declared as add_executable() targets.
# ==========================================================================

# test_jit_deopt: JIT compilation + interpreter→JIT re-enter + deopt
add_executable(test_jit_deopt
    opcode/test_jit_deopt.c
)
target_link_libraries(test_jit_deopt PRIVATE
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
    $<$<BOOL:${VORTEX_ENABLE_SOTA}>:vortex_sota>
    vortex_common
    m
    pthread
-Wl,--end-group
)
add_test(NAME test_jit_deopt COMMAND test_jit_deopt)

# test_mul5_trace: T2 pipeline mul5 trace bug test
add_executable(test_mul5_trace
    opcode/test_mul5_trace.c
)
target_link_libraries(test_mul5_trace PRIVATE
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
    $<$<BOOL:${VORTEX_ENABLE_SOTA}>:vortex_sota>
    vortex_common
    m
    pthread
-Wl,--end-group
)
add_test(NAME test_mul5_trace COMMAND test_mul5_trace)

# test_rep_infer: rep_infer pass test
add_executable(test_rep_infer
    opcode/test_rep_infer.c
)
target_link_libraries(test_rep_infer PRIVATE
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
    $<$<BOOL:${VORTEX_ENABLE_SOTA}>:vortex_sota>
    vortex_common
    m
    pthread
-Wl,--end-group
)
add_test(NAME test_rep_infer COMMAND test_rep_infer)

# test_deopt_dynamic: dynamic deopt system test
add_executable(test_deopt_dynamic
    opcode/test_deopt_dynamic.c
)
target_link_libraries(test_deopt_dynamic PRIVATE
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
    $<$<BOOL:${VORTEX_ENABLE_SOTA}>:vortex_sota>
    vortex_common
    m
    pthread
-Wl,--end-group
)
add_test(NAME test_deopt_dynamic COMMAND test_deopt_dynamic)

# Note: tests/opcode/test_opcodes.c is intentionally NOT built — it uses
# popen() to invoke /tmp/vortex (TEST-003), which is not portable.

# ==========================================================================
# OSR Cluster D regression tests (Concurrency & GC):
#   OSR-4  — stack walker reads return addr from [RBP+32]
#   OSR-18 — atomic dedup + atomic profiler counters
#   OSR-20/21 — code-cache quarantine defers metadata free
#   OSR-22 — versioned cache on_enter/on_exit wired into dispatch loop
#   OSR-26 — GC conservative scanner window widened + writeback
# ==========================================================================

# Shared link group for OSR regression tests. These exercise the full
# pipeline (interp → JIT → GC → deopt) so they need every library.
set(VORTEX_OSR_TEST_LIBS
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
    $<$<BOOL:${VORTEX_ENABLE_SOTA}>:vortex_sota>
    vortex_common
    m
    pthread
)

# OSR-4: stack-walker return-address offset
add_executable(test_osr4_stack_walk_return_addr
    regression/osr/test_osr4_stack_walk_return_addr.c
)
target_link_libraries(test_osr4_stack_walk_return_addr PRIVATE
    -Wl,--start-group
    ${VORTEX_OSR_TEST_LIBS}
    -Wl,--end-group
)
add_test(NAME test_osr4_stack_walk_return_addr
    COMMAND test_osr4_stack_walk_return_addr)

# OSR-18: atomic dedup in vtx_request_compilation + atomic profiler counters
add_executable(test_osr18_atomic_dedup
    regression/osr/test_osr18_atomic_dedup.c
)
target_link_libraries(test_osr18_atomic_dedup PRIVATE
    -Wl,--start-group
    ${VORTEX_OSR_TEST_LIBS}
    -Wl,--end-group
)
add_test(NAME test_osr18_atomic_dedup
    COMMAND test_osr18_atomic_dedup)

# OSR-20 / OSR-21: code-cache quarantine defers metadata free
add_executable(test_osr20_21_quarantine
    regression/osr/test_osr20_21_quarantine.c
)
target_link_libraries(test_osr20_21_quarantine PRIVATE
    -Wl,--start-group
    ${VORTEX_OSR_TEST_LIBS}
    -Wl,--end-group
)
add_test(NAME test_osr20_21_quarantine
    COMMAND test_osr20_21_quarantine)

# OSR-22: versioned cache on_enter/on_exit wired into dispatch loop's JIT path
add_executable(test_osr22_versioned_cache_on_enter_exit
    regression/osr/test_osr22_versioned_cache_on_enter_exit.c
)
target_link_libraries(test_osr22_versioned_cache_on_enter_exit PRIVATE
    -Wl,--start-group
    ${VORTEX_OSR_TEST_LIBS}
    -Wl,--end-group
)
add_test(NAME test_osr22_versioned_cache_on_enter_exit
    COMMAND test_osr22_versioned_cache_on_enter_exit)

# OSR-26: GC conservative scanner window widened + forwarded-pointer writeback
add_executable(test_osr26_gc_conservative_scanner
    regression/osr/test_osr26_gc_conservative_scanner.c
)
target_link_libraries(test_osr26_gc_conservative_scanner PRIVATE
    -Wl,--start-group
    ${VORTEX_OSR_TEST_LIBS}
    -Wl,--end-group
)
add_test(NAME test_osr26_gc_conservative_scanner
    COMMAND test_osr26_gc_conservative_scanner)

# ==========================================================================
# OSR Cluster A/B/C regression tests (OSR-up trampoline, OSR-down/deopt,
# OSR entry registration & dispatch).
# These tests were previously on disk but NOT registered as build targets
# (Meta-review Blocker B2). Wiring them below so they are compiled and run.
# Tests that fail to compile or fail at runtime due to known limitations
# (B3-B6: tests that don't trigger the actual bug path) are still wired
# here so the build/test status is visible; the test bodies themselves
# were updated by the FIXUP task to either trigger the bug path or to
# verify the fix via source-grep.
# ==========================================================================

# OSR-2: codegen records VTX_STF_OSR_ENTRY side-table entries for loop headers
add_executable(test_osr_2
    regression/osr/test_osr_2.c
)
target_link_libraries(test_osr_2 PRIVATE
    -Wl,--start-group
    ${VORTEX_OSR_TEST_LIBS}
    -Wl,--end-group
)
add_test(NAME test_osr_2 COMMAND test_osr_2)

# OSR-3: vtx_osr_up is void (not bool) — asm-jump success is detected by
# the function not returning
add_executable(test_osr_3
    regression/osr/test_osr_3.c
)
target_link_libraries(test_osr_3 PRIVATE
    -Wl,--start-group
    ${VORTEX_OSR_TEST_LIBS}
    -Wl,--end-group
)
add_test(NAME test_osr_3 COMMAND test_osr_3)

# OSR-5: side-table OSR entry lookup filters by bytecode_pc
add_executable(test_osr_5
    regression/osr/test_osr_5.c
)
target_link_libraries(test_osr_5 PRIVATE
    -Wl,--start-group
    ${VORTEX_OSR_TEST_LIBS}
    -Wl,--end-group
)
add_test(NAME test_osr_5 COMMAND test_osr_5)

# OSR-6: OSR-up asm saves caller's RBX/R12 to JIT frame's saved-register slots
add_executable(test_osr_6
    regression/osr/test_osr_6.c
)
target_link_libraries(test_osr_6 PRIVATE
    -Wl,--start-group
    ${VORTEX_OSR_TEST_LIBS}
    -Wl,--end-group
)
add_test(NAME test_osr_6 COMMAND test_osr_6)

# OSR-11: vtx_osr_up re-checks registry's current cm version before asm jump
add_executable(test_osr_11
    regression/osr/test_osr_11.c
)
target_link_libraries(test_osr_11 PRIVATE
    -Wl,--start-group
    ${VORTEX_OSR_TEST_LIBS}
    -Wl,--end-group
)
add_test(NAME test_osr_11 COMMAND test_osr_11)

# OSR-12: vtx_osr_up polls GC safepoint immediately before the asm jump
add_executable(test_osr_12
    regression/osr/test_osr_12.c
)
target_link_libraries(test_osr_12 PRIVATE
    -Wl,--start-group
    ${VORTEX_OSR_TEST_LIBS}
    -Wl,--end-group
)
add_test(NAME test_osr_12 COMMAND test_osr_12)

# OSR-16: vtx_osr_up refuses OSR into inlined code (frame shape mismatch)
add_executable(test_osr_16
    regression/osr/test_osr_16.c
)
target_link_libraries(test_osr_16 PRIVATE
    -Wl,--start-group
    ${VORTEX_OSR_TEST_LIBS}
    -Wl,--end-group
)
add_test(NAME test_osr_16 COMMAND test_osr_16)

# OSR-24: vtx_osr_up verifies stack_depth matches interp->stack_top
add_executable(test_osr_24
    regression/osr/test_osr_24.c
)
target_link_libraries(test_osr_24 PRIVATE
    -Wl,--start-group
    ${VORTEX_OSR_TEST_LIBS}
    -Wl,--end-group
)
add_test(NAME test_osr_24 COMMAND test_osr_24)

# OSR-27: NULL check on compiled_code->code in bc_pc_map loop
add_executable(test_osr_27
    regression/osr/test_osr_27.c
)
target_link_libraries(test_osr_27 PRIVATE
    -Wl,--start-group
    ${VORTEX_OSR_TEST_LIBS}
    -Wl,--end-group
)
add_test(NAME test_osr_27 COMMAND test_osr_27)

# OSR-28: asm clobber list includes "rbp" and "rsp" (defensive)
add_executable(test_osr_28
    regression/osr/test_osr_28.c
)
target_link_libraries(test_osr_28 PRIVATE
    -Wl,--start-group
    ${VORTEX_OSR_TEST_LIBS}
    -Wl,--end-group
)
add_test(NAME test_osr_28 COMMAND test_osr_28)

# OSR-30: asm writes profile_data to [RBP+8] (was hardcoded NULL)
add_executable(test_osr_30
    regression/osr/test_osr_30.c
)
target_link_libraries(test_osr_30 PRIVATE
    -Wl,--start-group
    ${VORTEX_OSR_TEST_LIBS}
    -Wl,--end-group
)
add_test(NAME test_osr_30 COMMAND test_osr_30)

# OSR-31: int3 safety trap is preceded by __builtin_unreachable
add_executable(test_osr_31
    regression/osr/test_osr_31.c
)
target_link_libraries(test_osr_31 PRIVATE
    -Wl,--start-group
    ${VORTEX_OSR_TEST_LIBS}
    -Wl,--end-group
)
add_test(NAME test_osr_31 COMMAND test_osr_31)

# OSR-33: vtx_osr_up verifies entry_register_convention == VTX_OSR_CONV_DEFAULT
add_executable(test_osr_33
    regression/osr/test_osr_33.c
)
target_link_libraries(test_osr_33 PRIVATE
    -Wl,--start-group
    ${VORTEX_OSR_TEST_LIBS}
    -Wl,--end-group
)
add_test(NAME test_osr_33 COMMAND test_osr_33)

# ==========================================================================
# OSR Cluster C-retry-2: 5 missing regression tests (17, 23, 25, 29, 32)
# Added by Subagent C-retry-2 to address Meta findings (M3 + M4).
# ==========================================================================

# OSR-17: OSR trigger fires at IF_TRUE/IF_FALSE backedges (not just GOTO)
add_executable(test_osr_17_backedge_triggers
    regression/osr/test_osr_17_backedge_triggers.c
)
target_link_libraries(test_osr_17_backedge_triggers PRIVATE
    -Wl,--start-group
    ${VORTEX_OSR_TEST_LIBS}
    -Wl,--end-group
)
add_test(NAME osr_17_backedge_triggers COMMAND test_osr_17_backedge_triggers)

# OSR-23: vtx_side_table_lookup_osr_entry filters by flag + bytecode_pc
add_executable(test_osr_23_lookup_filter
    regression/osr/test_osr_23_lookup_filter.c
)
target_link_libraries(test_osr_23_lookup_filter PRIVATE
    -Wl,--start-group
    ${VORTEX_OSR_TEST_LIBS}
    -Wl,--end-group
)
add_test(NAME osr_23_lookup_filter COMMAND test_osr_23_lookup_filter)

# OSR-25: cm->frame_layout.codegen override (locals_base=-24, not -8)
add_executable(test_osr_25_layout
    regression/osr/test_osr_25_layout.c
)
target_link_libraries(test_osr_25_layout PRIVATE
    -Wl,--start-group
    ${VORTEX_OSR_TEST_LIBS}
    -Wl,--end-group
)
add_test(NAME osr_25_layout COMMAND test_osr_25_layout)

# OSR-29: per-method OSR re-attempt rate limiter (helpers + cooldown)
add_executable(test_osr_29_rate_limit
    regression/osr/test_osr_29_rate_limit.c
)
target_link_libraries(test_osr_29_rate_limit PRIVATE
    -Wl,--start-group
    ${VORTEX_OSR_TEST_LIBS}
    -Wl,--end-group
)
add_test(NAME osr_29_rate_limit COMMAND test_osr_29_rate_limit)

# OSR-32: dead `osr_active` field removed (compile-time _Static_assert)
add_executable(test_osr_32_dead_flag_removed
    regression/osr/test_osr_32_dead_flag_removed.c
)
target_link_libraries(test_osr_32_dead_flag_removed PRIVATE
    -Wl,--start-group
    ${VORTEX_OSR_TEST_LIBS}
    -Wl,--end-group
)
add_test(NAME osr_32_dead_flag_removed COMMAND test_osr_32_dead_flag_removed)

# ==========================================================================
# PEA use-after-realloc regression tests (Task PEA-FIX-1):
#   PEA-3-1 — cross_object_sr.c stale `node` after vtx_node_create
#   PEA-3-2 — cross_object_sr.c stale `alloc_node` after vtx_node_create
#   PEA-2-1 — materialize.c stale `node` after insert_materialization_code
#   PEA-2-2 — virtual.c resolve_virtual_phis stale `node` after per-field
#             vtx_node_create
#
# Each test constructs a synthetic SoN graph whose node count is exactly
# VTX_NODE_TABLE_INITIAL_CAPACITY (256) immediately before invoking the
# PEA pass, so the first vtx_node_create() inside the pass triggers
# node_table_grow() → realloc(table->nodes). The tests then verify the
# post-pass graph state (dead flags, replaced inputs, virtual_states) to
# confirm the surgical re-fetch fixes landed.
# ==========================================================================

# PEA-3-1: stale `node` pointer in cross_object_sr after realloc
add_executable(test_pea3_1_cross_object_sr_realloc
    regression/pea/test_pea3_1_cross_object_sr_realloc.c
)
target_link_libraries(test_pea3_1_cross_object_sr_realloc PRIVATE
    -Wl,--start-group
    ${VORTEX_OSR_TEST_LIBS}
    -Wl,--end-group
)
add_test(NAME pea3_1_cross_object_sr_realloc
    COMMAND test_pea3_1_cross_object_sr_realloc)

# PEA-3-2: stale `alloc_node` pointer in cross_object_sr after realloc
add_executable(test_pea3_2_alloc_node_realloc
    regression/pea/test_pea3_2_alloc_node_realloc.c
)
target_link_libraries(test_pea3_2_alloc_node_realloc PRIVATE
    -Wl,--start-group
    ${VORTEX_OSR_TEST_LIBS}
    -Wl,--end-group
)
add_test(NAME pea3_2_alloc_node_realloc
    COMMAND test_pea3_2_alloc_node_realloc)

# PEA-2-1: stale `node` pointer in materialize.c after insert_materialization_code
add_executable(test_pea2_1_materialize_realloc
    regression/pea/test_pea2_1_materialize_realloc.c
)
target_link_libraries(test_pea2_1_materialize_realloc PRIVATE
    -Wl,--start-group
    ${VORTEX_OSR_TEST_LIBS}
    -Wl,--end-group
)
add_test(NAME pea2_1_materialize_realloc
    COMMAND test_pea2_1_materialize_realloc)

# PEA-2-2: stale `node` pointer in virtual.c resolve_virtual_phis
add_executable(test_pea2_2_virtual_phi_realloc
    regression/pea/test_pea2_2_virtual_phi_realloc.c
)
target_link_libraries(test_pea2_2_virtual_phi_realloc PRIVATE
    -Wl,--start-group
    ${VORTEX_OSR_TEST_LIBS}
    -Wl,--end-group
)
add_test(NAME pea2_2_virtual_phi_realloc
    COMMAND test_pea2_2_virtual_phi_realloc)

# PEA-2-3: per-field Phi missing Region input
add_executable(test_pea2_3_phi_region_input
    regression/pea/test_pea2_3_phi_region_input.c
)
target_link_libraries(test_pea2_3_phi_region_input PRIVATE
    -Wl,--start-group
    ${VORTEX_OSR_TEST_LIBS}
    -Wl,--end-group
)
add_test(NAME pea2_3_phi_region_input
    COMMAND test_pea2_3_phi_region_input)

# PEA-2-6: materialization memory chain connected to escape point
add_executable(test_pea2_6_materialize_memory_chain
    regression/pea/test_pea2_6_materialize_memory_chain.c
)
target_link_libraries(test_pea2_6_materialize_memory_chain PRIVATE
    -Wl,--start-group
    ${VORTEX_OSR_TEST_LIBS}
    -Wl,--end-group
)
add_test(NAME pea2_6_materialize_memory_chain
    COMMAND test_pea2_6_materialize_memory_chain)

# PEA-1-2: non-allocation nodes return GlobalEscape
add_executable(test_pea1_2_non_alloc_global_escape
    regression/pea/test_pea1_2_non_alloc_global_escape.c
)
target_link_libraries(test_pea1_2_non_alloc_global_escape PRIVATE
    -Wl,--start-group
    ${VORTEX_OSR_TEST_LIBS}
    -Wl,--end-group
)
add_test(NAME pea1_2_non_alloc_global_escape
    COMMAND test_pea1_2_non_alloc_global_escape)

# PEA-1-1: Phi-chain escape propagation
add_executable(test_pea1_1_phi_escape_propagation
    regression/pea/test_pea1_1_phi_escape_propagation.c
)
target_link_libraries(test_pea1_1_phi_escape_propagation PRIVATE
    -Wl,--start-group
    ${VORTEX_OSR_TEST_LIBS}
    -Wl,--end-group
)
add_test(NAME pea1_1_phi_escape_propagation
    COMMAND test_pea1_1_phi_escape_propagation)

# PEA-1-4: StoreField no unconditional ARG bump
add_executable(test_pea1_4_storefield_no_arg_bump
    regression/pea/test_pea1_4_storefield_no_arg_bump.c
)
target_link_libraries(test_pea1_4_storefield_no_arg_bump PRIVATE
    -Wl,--start-group
    ${VORTEX_OSR_TEST_LIBS}
    -Wl,--end-group
)
add_test(NAME pea1_4_storefield_no_arg_bump
    COMMAND test_pea1_4_storefield_no_arg_bump)

# PEA-2-8: union of all virtual inputs' field sets in resolve_virtual_phis
add_executable(test_pea2_8
    regression/pea/test_pea2_8_union_field_set.c
)
target_link_libraries(test_pea2_8 PRIVATE
    -Wl,--start-group
    ${VORTEX_OSR_TEST_LIBS}
    -Wl,--end-group
)
add_test(NAME pea2_8_union_field_set
    COMMAND test_pea2_8)

# PEA-2-11: dead StoreField nodes drop input edges (output_count)
add_executable(test_pea2_11
    regression/pea/test_pea2_11_dead_storefield_inputs.c
)
target_link_libraries(test_pea2_11 PRIVATE
    -Wl,--start-group
    ${VORTEX_OSR_TEST_LIBS}
    -Wl,--end-group
)
add_test(NAME pea2_11_dead_storefield_inputs
    COMMAND test_pea2_11)

# PEA-2-12: CmpP identity fold for virtual objects
add_executable(test_pea2_12
    regression/pea/test_pea2_12_cmp_identity_virtuals.c
)
target_link_libraries(test_pea2_12 PRIVATE
    -Wl,--start-group
    ${VORTEX_OSR_TEST_LIBS}
    -Wl,--end-group
)
add_test(NAME pea2_12_cmp_identity_virtuals
    COMMAND test_pea2_12)

# PEA-3-3: primitive-typed default value for never-stored fields
add_executable(test_pea3_3
    regression/pea/test_pea3_3_primitive_default.c
)
target_link_libraries(test_pea3_3 PRIVATE
    -Wl,--start-group
    ${VORTEX_OSR_TEST_LIBS}
    -Wl,--end-group
)
add_test(NAME pea3_3_primitive_default
    COMMAND test_pea3_3)

# PEA-3-4: multiple reaching stores disables LoadField rewrite
add_executable(test_pea3_4
    regression/pea/test_pea3_4_multiple_stores_disabled.c
)
target_link_libraries(test_pea3_4 PRIVATE
    -Wl,--start-group
    ${VORTEX_OSR_TEST_LIBS}
    -Wl,--end-group
)
add_test(NAME pea3_4_multiple_stores_disabled
    COMMAND test_pea3_4)

# PEA-1-3: build_block_node_lists consumes vtx_schedule_t (replaces
# buggy region/control/memory + bytecode_pc heuristic).
add_executable(test_pea1_3_block_node_assignment
    regression/pea/test_pea1_3_block_node_assignment.c
)
target_link_libraries(test_pea1_3_block_node_assignment PRIVATE
    -Wl,--start-group
    ${VORTEX_OSR_TEST_LIBS}
    -Wl,--end-group
)
add_test(NAME pea1_3_block_node_assignment
    COMMAND test_pea1_3_block_node_assignment)
