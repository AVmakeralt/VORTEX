# VORTEX source CMake

# Common library with shared types and utilities
add_library(vortex_common STATIC
    runtime/arena.c
)
target_include_directories(vortex_common PUBLIC ${VORTEX_INCLUDE_DIRS})

# Runtime: object model, type system, GC, bytecode, helpers
add_library(vortex_runtime STATIC
    runtime/object.c
    runtime/type_system.c
    runtime/gc.c
    runtime/helpers.c
    runtime/bytecode.c
)
target_include_directories(vortex_runtime PUBLIC ${VORTEX_INCLUDE_DIRS})
target_link_libraries(vortex_runtime PUBLIC vortex_common)

# Interpreter T0
add_library(vortex_interp STATIC
    interp/dispatch.c
    interp/frame.c
    interp/profiler.c
    interp/lookup.c
    interp/type_feedback.c
)
target_include_directories(vortex_interp PUBLIC ${VORTEX_INCLUDE_DIRS})
target_link_libraries(vortex_interp PUBLIC vortex_runtime)

# Baseline JIT T1
add_library(vortex_baseline STATIC
    baseline/codegen.c
    baseline/guards.c
    baseline/frame_layout.c
    baseline/deopt_stubs.c
    baseline/instrument.c
)
target_include_directories(vortex_baseline PUBLIC ${VORTEX_INCLUDE_DIRS})
target_link_libraries(vortex_baseline PUBLIC vortex_runtime vortex_deopt)

# Sea-of-Nodes IR
add_library(vortex_ir STATIC
    ir/node.c
    ir/graph.c
    ir/gvn.c
    ir/constant_prop.c
    ir/dce.c
    ir/schedule.c
    ir/verify.c
    ir/licm.c
    ir/bounds_check.c
    ir/induction.c
    ir/tbaa.c
    ir/spec_elim.c
)
target_include_directories(vortex_ir PUBLIC ${VORTEX_INCLUDE_DIRS})
target_link_libraries(vortex_ir PUBLIC vortex_common)

# Trace Recorder
add_library(vortex_trace STATIC
    trace/selector.c
    trace/recorder.c
    trace/tree.c
    trace/side_exit.c
)
target_include_directories(vortex_trace PUBLIC ${VORTEX_INCLUDE_DIRS})
target_link_libraries(vortex_trace PUBLIC vortex_runtime vortex_ir vortex_profile)

# Region Formation
add_library(vortex_region STATIC
    region/stitch.c
    region/budget.c
    region/cross_trace.c
)
target_include_directories(vortex_region PUBLIC ${VORTEX_INCLUDE_DIRS})
target_link_libraries(vortex_region PUBLIC vortex_ir vortex_trace)

# Partial Escape Analysis
add_library(vortex_pea STATIC
    pea/analysis.c
    pea/cross_object_sr.c
    pea/materialize.c
    pea/virtual.c
    pea/speculative_pea.c
)
target_include_directories(vortex_pea PUBLIC ${VORTEX_INCLUDE_DIRS})
target_link_libraries(vortex_pea PUBLIC vortex_ir vortex_guard)

# Deoptimization
add_library(vortex_deopt STATIC
    deopt/frame_state.c
    deopt/osr.c
    deopt/deoptless.c
    deopt/side_table.c
    deopt/stack_walk.c
)
target_include_directories(vortex_deopt PUBLIC ${VORTEX_INCLUDE_DIRS})
target_link_libraries(vortex_deopt PUBLIC vortex_runtime vortex_ir)

# ML Inliner
add_library(vortex_inliner STATIC
    inliner/features.c
    inliner/inference.c
    inliner/feedback.c
    inliner/transform.c
)
target_include_directories(vortex_inliner PUBLIC ${VORTEX_INCLUDE_DIRS})
target_link_libraries(vortex_inliner PUBLIC vortex_ir vortex_profile)

# Profile Collection
add_library(vortex_profile STATIC
    profile/data.c
    profile/persist.c
    profile/merge.c
    profile/phase.c
)
target_include_directories(vortex_profile PUBLIC ${VORTEX_INCLUDE_DIRS})
target_link_libraries(vortex_profile PUBLIC vortex_runtime)

# x86-64 Lowering
add_library(vortex_lower STATIC
    lower/isel.c
    lower/regalloc.c
    lower/emit.c
    lower/guard_emit.c
    lower/reloc.c
)
target_include_directories(vortex_lower PUBLIC ${VORTEX_INCLUDE_DIRS})
target_link_libraries(vortex_lower PUBLIC vortex_ir vortex_deopt)

# Code Cache
add_library(vortex_codecache STATIC
    codecache/cache.c
    codecache/install.c
    codecache/evict.c
    codecache/invalidate.c
)
target_include_directories(vortex_codecache PUBLIC ${VORTEX_INCLUDE_DIRS})
target_link_libraries(vortex_codecache PUBLIC vortex_runtime vortex_lower)

# Concurrent Compilation
add_library(vortex_compile STATIC
    compile/threadpool.c
    compile/priority.c
    compile/safepoint.c
    compile/version.c
    compile/pipeline.c
    compile/phase_react.c
    compile/spec_versioning.c
    compile/orchestrator.c
)
target_include_directories(vortex_compile PUBLIC ${VORTEX_INCLUDE_DIRS})
target_link_libraries(vortex_compile PUBLIC vortex_ir vortex_pea vortex_inliner vortex_lower vortex_deopt vortex_codecache vortex_guard vortex_profile $<$<BOOL:${VORTEX_ENABLE_SOTA}>:vortex_sota> Threads::Threads)

find_package(Threads REQUIRED)

# Adaptive Guards
add_library(vortex_guard STATIC
    guard/metadata.c
    guard/ewma.c
    guard/hoist.c
    guard/merge.c
    guard/value_profile.c
    guard/guard_deps.c
    guard/guard_page_type.c
    guard/heap_value_prop.c
)
target_include_directories(vortex_guard PUBLIC ${VORTEX_INCLUDE_DIRS})
target_link_libraries(vortex_guard PUBLIC vortex_ir vortex_deopt vortex_pea)

# SOTA Features
if(VORTEX_ENABLE_SOTA)
add_library(vortex_sota STATIC
    sota/phase.c
    sota/recomp.c
    sota/loop_spec.c
    sota/fdi.c
    sota/markov.c
)
target_include_directories(vortex_sota PUBLIC ${VORTEX_INCLUDE_DIRS})
target_link_libraries(vortex_sota PUBLIC vortex_ir vortex_profile vortex_inliner vortex_compile)
endif()

# Mid-Tier JIT T1.5
add_library(vortex_midtier STATIC
    midtier/midtier.c
)
target_include_directories(vortex_midtier PUBLIC ${VORTEX_INCLUDE_DIRS})
target_link_libraries(vortex_midtier PUBLIC vortex_ir vortex_interp vortex_lower vortex_deopt)

# Main VORTEX executable
add_executable(vortex
    main_new.c
    runtime_stubs.c
)
target_include_directories(vortex PRIVATE ${VORTEX_INCLUDE_DIRS})
target_link_libraries(vortex PRIVATE
    -Wl,--start-group
    vortex_runtime
    vortex_interp
    vortex_baseline
    vortex_ir
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
    -Wl,--end-group
    m
    pthread
)
