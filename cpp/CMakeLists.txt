# cpp/CMakeLists.txt — VORTEX C++ Embedding Library
cmake_minimum_required(VERSION 3.14)
project(vortex_cpp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(VORTEX_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../src)
set(VORTEX_BUILD_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../build)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${VORTEX_SRC_DIR}
    ${VORTEX_BUILD_DIR}
)

file(GLOB VORTEX_CPP_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

add_library(vortex_cpp STATIC ${VORTEX_CPP_SOURCES})
target_include_directories(vortex_cpp PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${VORTEX_SRC_DIR}
    ${VORTEX_BUILD_DIR}
)
target_compile_features(vortex_cpp PUBLIC cxx_std_20)
target_compile_options(vortex_cpp PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable
)

if(EXISTS ${VORTEX_BUILD_DIR}/src/libvortex_runtime.a)
    target_link_libraries(vortex_cpp PUBLIC
        -Wl,--start-group
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
        -Wl,--end-group
        -lm -lpthread
    )
else()
    message(FATAL_ERROR
        "Pre-built VORTEX C libraries not found in ${VORTEX_BUILD_DIR}/src/. "
        "Build the root VORTEX project first: "
        "cd .. && mkdir -p build && cd build && cmake .. && make -j4")
endif()

add_executable(vortex_cpp_example examples/hello.cpp)
target_link_libraries(vortex_cpp_example PRIVATE vortex_cpp)

enable_testing()
add_executable(vortex_cpp_tests tests/test_embed.cpp)
target_link_libraries(vortex_cpp_tests PRIVATE vortex_cpp)
add_test(NAME vortex_cpp_tests COMMAND vortex_cpp_tests)

add_executable(test_hidden_class tests/test_hidden_class.cpp)
target_link_libraries(test_hidden_class PRIVATE vortex_cpp)
add_test(NAME test_hidden_class COMMAND test_hidden_class)

add_executable(test_property_ic tests/test_property_ic.cpp)
target_link_libraries(test_property_ic PRIVATE vortex_cpp)
add_test(NAME test_property_ic COMMAND test_property_ic)

add_executable(test_fuzz_property_access tests/test_fuzz_property_access.cpp)
target_link_libraries(test_fuzz_property_access PRIVATE vortex_cpp)
add_test(NAME test_fuzz_property_access COMMAND test_fuzz_property_access)

add_executable(test_superinstruction tests/test_superinstruction.cpp)
target_link_libraries(test_superinstruction PRIVATE vortex_cpp)
add_test(NAME test_superinstruction COMMAND test_superinstruction)

install(TARGETS vortex_cpp ARCHIVE DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)
