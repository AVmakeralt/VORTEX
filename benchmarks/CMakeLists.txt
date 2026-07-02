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
    vortex_sota
    vortex_region
    vortex_trace
    vortex_midtier
    vortex_profile
    vortex_assembler
    vortex_common
    m
    pthread
)
