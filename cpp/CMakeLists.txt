# cpp/CMakeLists.txt — VORTEX C++ Embedding Library
#
# Builds libvortex_cpp.a (the C++ embedding API) and links it against
# the core C VORTEX libraries.
#
# Targets:
#   vortex_cpp        — static library (vortex::Runtime, Object, etc.)
#   vortex_embed      — C API wrapper (vortex_embed.h)
#   vortex_cpp_example — example program
#   vortex_cpp_tests  — test suite

cmake_minimum_required(VERSION 3.14)
project(vortex_cpp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- Find the core VORTEX C library ----
# We expect ../build/src/ to contain the compiled C libraries.
# For an out-of-tree build, we compile the C sources directly.

set(VORTEX_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../src)
set(VORTEX_BUILD_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../build)

# Include paths
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${VORTEX_SRC_DIR}
    ${VORTEX_BUILD_DIR}
)

# Collect C++ sources
file(GLOB VORTEX_CPP_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
file(GLOB VORTEX_CPP_HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/include/vortex/*.hpp)

# ---- C++ Embedding Library (static) ----
add_library(vortex_cpp STATIC ${VORTEX_CPP_SOURCES})
target_include_directories(vortex_cpp PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${VORTEX_SRC_DIR}
    ${VORTEX_BUILD_DIR}
)
target_compile_features(vortex_cpp PUBLIC cxx_std_17)
target_compile_options(vortex_cpp PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable
)

# Link against the core C VORTEX libraries.
# If they exist as pre-built archives, use them; otherwise compile from source.
if(EXISTS ${VORTEX_BUILD_DIR}/src/libvortex_runtime.a)
    target_link_libraries(vortex_cpp PUBLIC
        ${VORTEX_BUILD_DIR}/src/libvortex_compile.a
        ${VORTEX_BUILD_DIR}/src/libvortex_midtier.a
        ${VORTEX_BUILD_DIR}/src/libvortex_inliner.a
        ${VORTEX_BUILD_DIR}/src/libvortex_sota.a
        ${VORTEX_BUILD_DIR}/src/libvortex_profile.a
        ${VORTEX_BUILD_DIR}/src/libvortex_pea.a
        ${VORTEX_BUILD_DIR}/src/libvortex_trace.a
        ${VORTEX_BUILD_DIR}/src/libvortex_region.a
        ${VORTEX_BUILD_DIR}/src/libvortex_lower.a
        ${VORTEX_BUILD_DIR}/src/libvortex_baseline.a
        ${VORTEX_BUILD_DIR}/src/libvortex_ir.a
        ${VORTEX_BUILD_DIR}/src/libvortex_guard.a
        ${VORTEX_BUILD_DIR}/src/libvortex_deopt.a
        ${VORTEX_BUILD_DIR}/src/libvortex_codecache.a
        ${VORTEX_BUILD_DIR}/src/libvortex_interp.a
        ${VORTEX_BUILD_DIR}/src/libvortex_runtime.a
        ${VORTEX_BUILD_DIR}/src/libvortex_common.a
        -lm -lpthread
    )
endif()

# ---- Example ----
add_executable(vortex_cpp_example examples/hello.cpp)
target_link_libraries(vortex_cpp_example PRIVATE vortex_cpp)

# ---- Tests ----
enable_testing()
add_executable(vortex_cpp_tests tests/test_embed.cpp)
target_link_libraries(vortex_cpp_tests PRIVATE vortex_cpp)
add_test(NAME vortex_cpp_tests COMMAND vortex_cpp_tests)

# ---- Install (optional) ----
install(TARGETS vortex_cpp
    ARCHIVE DESTINATION lib
)
install(DIRECTORY include/
    DESTINATION include
)
