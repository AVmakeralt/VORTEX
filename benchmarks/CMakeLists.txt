# VORTEX Benchmarks CMake

# Benchmark framework library
add_library(vortex_bench_framework STATIC
    bench_framework.c
)
target_include_directories(vortex_bench_framework PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${VORTEX_INCLUDE_DIRS}
)

# ---- Micro benchmarks ----

# bench_arith
add_executable(bench_arith
    micro/bench_arith.c
)
target_link_libraries(bench_arith PRIVATE
    vortex_bench_framework
    vortex_interp
    vortex_runtime
    m
)

# bench_call
add_executable(bench_call
    micro/bench_call.c
)
target_link_libraries(bench_call PRIVATE
    vortex_bench_framework
    vortex_interp
    vortex_runtime
    m
)

# ---- Macro benchmarks ----

# bench_fib
add_executable(bench_fib
    macro/bench_fib.c
)
target_link_libraries(bench_fib PRIVATE
    vortex_bench_framework
    vortex_interp
    vortex_runtime
    m
)

# ---- JIT execution benchmarks ----

# bench_jit_exec
add_executable(bench_jit_exec
    jit_exec/jit_exec_bench.c
)
target_link_libraries(bench_jit_exec PRIVATE
    vortex_bench_framework
    vortex_interp
    vortex_runtime
    vortex_codecache
    vortex_lower
    m
)

# ---- Real workload benchmarks ----

# bench_real_workload
add_executable(bench_real_workload
    bench_real_workload.c
)
target_link_libraries(bench_real_workload PRIVATE
    vortex_bench_framework
    vortex_interp
    vortex_runtime
    vortex_codecache
    vortex_lower
    vortex_baseline
    vortex_deopt
    m
    pthread
)

# ---- T2 JIT pipeline benchmark (T0 vs T2 vs Native C) ----

add_executable(bench_t2
    bench_t2.c
)
target_link_libraries(bench_t2 PRIVATE
    -Wl,--start-group
    vortex_compile
    vortex_codecache
    vortex_lower
    vortex_interp
    vortex_runtime
    vortex_ir
    vortex_deopt
    vortex_baseline
    vortex_inliner
    vortex_pea
    vortex_guard
    $<$<BOOL:${VORTEX_ENABLE_SOTA}>:vortex_sota>
    vortex_region
    vortex_trace
    vortex_midtier
    vortex_profile
    vortex_assembler
    vortex_common
    $<$<BOOL:${VORTEX_ENABLE_CPP}>:vortex_cpp>
    -Wl,--end-group
    m
    pthread
)

# ---- SciMark-style benchmark ----

add_executable(bench_scimark
    bench_scimark.c
)
target_link_libraries(bench_scimark PRIVATE
    -Wl,--start-group
    vortex_compile
    vortex_codecache
    vortex_lower
    vortex_interp
    vortex_runtime
    vortex_ir
    vortex_deopt
    vortex_baseline
    vortex_inliner
    vortex_pea
    vortex_guard
    $<$<BOOL:${VORTEX_ENABLE_SOTA}>:vortex_sota>
    vortex_region
    vortex_trace
    vortex_midtier
    vortex_profile
    vortex_assembler
    vortex_common
    -Wl,--end-group
    m
    pthread
)

# ---- V8 comparison benchmark (VORTEX vs V8/Node.js) ----

add_executable(bench_v8_comparison
    bench_v8_comparison.c
)
target_link_libraries(bench_v8_comparison PRIVATE
    -Wl,--start-group
    vortex_compile
    vortex_codecache
    vortex_lower
    vortex_interp
    vortex_runtime
    vortex_ir
    vortex_deopt
    vortex_baseline
    vortex_inliner
    vortex_pea
    vortex_guard
    $<$<BOOL:${VORTEX_ENABLE_SOTA}>:vortex_sota>
    vortex_region
    vortex_trace
    vortex_midtier
    vortex_profile
    vortex_assembler
    vortex_common
    # §2.6 superinstruction pre-decode pass (C++). Links the strong
    # symbol vtx_superinstruction_predecode over the weak C fallback
    # declared in bench_v8_comparison.c.
    $<$<BOOL:${VORTEX_ENABLE_CPP}>:vortex_cpp>
    -Wl,--end-group
    m
    pthread
)


# ---- T2 JIT vs V8 comparison ----
add_executable(bench_t2_vs_v8
    bench_t2_vs_v8.c
)
target_link_libraries(bench_t2_vs_v8 PRIVATE
    -Wl,--start-group
    vortex_compile
    vortex_codecache
    vortex_lower
    vortex_interp
    vortex_runtime
    vortex_ir
    vortex_deopt
    vortex_baseline
    vortex_inliner
    vortex_pea
    vortex_guard
    $<$<BOOL:${VORTEX_ENABLE_SOTA}>:vortex_sota>
    vortex_region
    vortex_trace
    vortex_midtier
    vortex_profile
    vortex_assembler
    vortex_common
    -Wl,--end-group
    m
    pthread
)

# ---- Object-heavy benchmark (partial virtualization) ----
add_executable(bench_object_heavy
    bench_object_heavy.c
)
target_link_libraries(bench_object_heavy PRIVATE
    -Wl,--start-group
    vortex_compile
    vortex_codecache
    vortex_lower
    vortex_interp
    vortex_runtime
    vortex_ir
    vortex_deopt
    vortex_baseline
    vortex_inliner
    vortex_pea
    vortex_guard
    $<$<BOOL:${VORTEX_ENABLE_SOTA}>:vortex_sota>
    vortex_region
    vortex_trace
    vortex_midtier
    vortex_profile
    vortex_assembler
    vortex_common
    $<$<BOOL:${VORTEX_ENABLE_CPP}>:vortex_cpp>
    -Wl,--end-group
    m
    pthread
)

# ---- Superinstruction gain measurement ----
add_executable(measure_si_gain
    measure_si_gain.c
)
target_link_libraries(measure_si_gain PRIVATE
    -Wl,--start-group
    vortex_compile
    vortex_codecache
    vortex_lower
    vortex_interp
    vortex_runtime
    vortex_ir
    vortex_deopt
    vortex_baseline
    vortex_inliner
    vortex_pea
    vortex_guard
    $<$<BOOL:${VORTEX_ENABLE_SOTA}>:vortex_sota>
    vortex_region
    vortex_trace
    vortex_midtier
    vortex_profile
    vortex_assembler
    vortex_common
    $<$<BOOL:${VORTEX_ENABLE_CPP}>:vortex_cpp>
    -Wl,--end-group
    m
    pthread
)

# ---- JIT vs JIT benchmark ----
add_executable(bench_jit_vs_jit
    bench_jit_vs_jit.c
)
target_link_libraries(bench_jit_vs_jit PRIVATE
    -Wl,--start-group
    vortex_compile
    vortex_codecache
    vortex_lower
    vortex_interp
    vortex_runtime
    vortex_ir
    vortex_deopt
    vortex_baseline
    vortex_inliner
    vortex_pea
    vortex_guard
    $<$<BOOL:${VORTEX_ENABLE_SOTA}>:vortex_sota>
    vortex_region
    vortex_trace
    vortex_midtier
    vortex_profile
    vortex_assembler
    vortex_common
    $<$<BOOL:${VORTEX_ENABLE_CPP}>:vortex_cpp>
    -Wl,--end-group
    m
    pthread
)

# ---- Scaling diagnostic ----
add_executable(bench_scaling
    bench_scaling.c
)
target_link_libraries(bench_scaling PRIVATE
    -Wl,--start-group
    vortex_compile
    vortex_codecache
    vortex_lower
    vortex_interp
    vortex_runtime
    vortex_ir
    vortex_deopt
    vortex_baseline
    vortex_inliner
    vortex_pea
    vortex_guard
    $<$<BOOL:${VORTEX_ENABLE_SOTA}>:vortex_sota>
    vortex_region
    vortex_trace
    vortex_midtier
    vortex_profile
    vortex_assembler
    vortex_common
    $<$<BOOL:${VORTEX_ENABLE_CPP}>:vortex_cpp>
    -Wl,--end-group
    m
    pthread
)

# ---- Dump sum assembly for inspection ----
add_executable(dump_sum_asm
    dump_sum_asm.c
)
target_link_libraries(dump_sum_asm PRIVATE
    -Wl,--start-group
    vortex_compile
    vortex_codecache
    vortex_lower
    vortex_interp
    vortex_runtime
    vortex_ir
    vortex_deopt
    vortex_baseline
    vortex_inliner
    vortex_pea
    vortex_guard
    $<$<BOOL:${VORTEX_ENABLE_SOTA}>:vortex_sota>
    vortex_region
    vortex_trace
    vortex_midtier
    vortex_profile
    vortex_assembler
    vortex_common
    $<$<BOOL:${VORTEX_ENABLE_CPP}>:vortex_cpp>
    -Wl,--end-group
    m
    pthread
)
add_executable(dump_fib_asm dump_fib_asm.c)
target_link_libraries(dump_fib_asm PRIVATE
    -Wl,--start-group
    vortex_compile vortex_codecache vortex_lower vortex_interp vortex_runtime
    vortex_ir vortex_deopt vortex_baseline vortex_inliner vortex_pea vortex_guard
    vortex_sota vortex_region vortex_trace vortex_midtier vortex_profile
    vortex_assembler vortex_common vortex_cpp
    -Wl,--end-group m pthread)
