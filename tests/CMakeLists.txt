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

# ---- Integration tests ----

# test_interp_basics
add_executable(test_interp_basics
    integration/test_interp_basics.c
)
target_link_libraries(test_interp_basics PRIVATE
    vortex_test_framework
    vortex_interp
    vortex_runtime
    m
)
add_test(NAME test_interp_basics COMMAND test_interp_basics)
