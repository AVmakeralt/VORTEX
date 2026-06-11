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
