cmake_minimum_required(VERSION 3.25)
project(VORTEX VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- Build type defaults ----
# Default to Release if not specified. This is the #1 performance fix:
# -O0 → -O3 is a 5-20x throughput improvement for the JIT host binary.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Compile flags: strict warnings, no external dependencies
#
# ╔══════════════════════════════════════════════════════════════════════════╗
# ║  DO NOT HIDE WARNINGS, KEEP THEM AS ERRORS                                ║
# ║                                                                          ║
# ║  This is a JIT compiler. A suppressed warning can become a               ║
# ║  miscompilation that produces wrong machine code silently.               ║
# ║  - If a warning appears, FIX THE CODE, not the warning.                  ║
# ║  - Do NOT add -Wno-* to silence warnings.                                ║
# ║  - Do NOT add casts or unused-variable tricks just to silence.           ║
# ║  - If a warning is a false positive, use #pragma GCC diagnostic          ║
# ║    push/pop around the specific line, with a comment explaining why.     ║
# ║                                                                          ║
# ║  Every warning is a latent bug. Fix it at the source.                    ║
# ╚══════════════════════════════════════════════════════════════════════════╝
add_compile_options(-Wall -Wextra -Wpedantic)
add_compile_options(-Wshadow -Wnull-dereference -Wdouble-promotion -Wformat=2 -Wundef)
add_compile_options(-Wconversion -Wsign-conversion)
add_compile_options(-Werror)
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions> $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-fPIC)

    # BUILD-002 fix: VORTEX uses NaN-boxed vtx_value_t (uint64_t) with
    # type-punning casts (heap pointer → uint64_t) and signed arithmetic
    # on SMI values that can overflow. Without -fno-strict-aliasing
    # and -fwrapv, GCC/Clang under -O3 aggressively exploit UB and
    # may mis-compile the tagged-pointer arithmetic.
    add_compile_options(-fno-strict-aliasing -fwrapv)

    # BUILD-003 fix: VORTEX parses binary .vtbc files and accepts
    # untrusted input via CLI tools. Without stack canaries or fortify,
    # a malformed bytecode file can smash stack buffers and achieve RCE.
    # Skip under sanitizers (they conflict with fortify).
    if(NOT CMAKE_C_COMPILER_FLAGS MATCHES "sanitize")
        add_compile_options(-fstack-protector-strong)
        add_compile_definitions(_FORTIFY_SOURCE=2)
    endif()

    # ---- Optimization flags for Release builds ----
    # These give 5-20x host throughput improvement over -O0.
    if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        add_compile_options(-O3)
        add_compile_options(-fomit-frame-pointer -fno-plt)
        add_compile_options(-ffunction-sections -fdata-sections)
        add_link_options(-Wl,--gc-sections)

        # LTO for cross-module inlining across the 14 static libs
        if(CMAKE_C_COMPILER_ID MATCHES "GNU")
            add_compile_options(-flto)
            add_link_options(-flto)
        endif()

        # -march=native enables SSE4.2/AVX2 for the host binary
        # (GBDT inference, arena memcpy, hash table operations)
        include(CheckCXXCompilerFlag)
        check_cxx_compiler_flag(-march=native HAS_MARCH_NATIVE)
        if(HAS_MARCH_NATIVE)
            add_compile_options(-march=native)
        endif()
    endif()
endif()

# Configuration options — assertions OFF by default in Release
# (every arena alignment check, every NULL check does a runtime branch)
option(VORTEX_ENABLE_ASSERTIONS "Enable debug assertions" OFF)
option(VORTEX_ENABLE_VERIFY "Enable IR verification after each pass" OFF)
option(VORTEX_ENABLE_PROFILING "Enable profiling in interpreter and T1" ON)
option(VORTEX_ENABLE_SOTA "Enable SOTA features" ON)
option(VORTEX_ENABLE_TESTS "Build tests" ON)
option(VORTEX_ENABLE_BENCHMARKS "Build benchmarks" ON)
option(VORTEX_ENABLE_CPP "Build C++ embedding library (vortex_cpp) and C++ runtime features (superinstructions, hidden classes, property IC)" ON)

# CONFIG-001 fix: sanitizer options. Enable with -DVORTEX_ENABLE_ASAN=ON.
# Note: sanitizers conflict with _FORTIFY_SOURCE (already skipped above
# when CMAKE_C_COMPILER_FLAGS matches "sanitize"), and with -flto.
option(VORTEX_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(VORTEX_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(VORTEX_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(VORTEX_ENABLE_FUZZING "Enable fuzzing mode (libFuzzer)" OFF)
set(VORTEX_TARGET_ARCH "x86_64" CACHE STRING "Target architecture (x86_64, arm64, riscv64)")

if(VORTEX_ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()
if(VORTEX_ENABLE_UBSAN)
    add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=undefined)
endif()
if(VORTEX_ENABLE_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()
if(VORTEX_ENABLE_FUZZING)
    add_compile_options(-fsanitize=fuzzer-no-link)
endif()

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

if(VORTEX_ENABLE_CPP)
    # C++ embedding library + runtime features (hidden classes,
    # property IC, superinstruction pre-decode).
    # Must come BEFORE tests/benchmarks so the vortex_cpp target is
    # available for them to link against.
    add_subdirectory(cpp)
endif()

if(VORTEX_ENABLE_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(VORTEX_ENABLE_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

add_subdirectory(tools)
