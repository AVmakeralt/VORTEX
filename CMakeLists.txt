cmake_minimum_required(VERSION 3.25)
project(VORTEX VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Compile flags: strict warnings, no external dependencies
add_compile_options(-Wall -Wextra -Werror)
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions> $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-fPIC)
endif()

# Configuration options
option(VORTEX_ENABLE_ASSERTIONS "Enable debug assertions" ON)
option(VORTEX_ENABLE_VERIFY "Enable IR verification after each pass" ON)
option(VORTEX_ENABLE_PROFILING "Enable profiling in interpreter and T1" ON)
option(VORTEX_ENABLE_SOTA "Enable SOTA features" ON)
option(VORTEX_ENABLE_TESTS "Build tests" ON)
option(VORTEX_ENABLE_BENCHMARKS "Build benchmarks" ON)

set(VORTEX_CACHE_MAX_SIZE 268435456 CACHE STRING "Maximum code cache size in bytes")
set(VORTEX_T1_THRESHOLD 1000 CACHE STRING "Method heat threshold for T1")
set(VORTEX_T2_THRESHOLD 10000 CACHE STRING "Trace heat threshold for T2")
set(VORTEX_COMPILE_THREADS 0 CACHE STRING "Compilation threads (0=auto)")

# Generate config header
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vortex_config.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/vortex_config.h
)

# Common include paths
set(VORTEX_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}
)

# Subdirectories
add_subdirectory(src)

if(VORTEX_ENABLE_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(VORTEX_ENABLE_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

add_subdirectory(tools)
