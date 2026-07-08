# VORTEX Tools CMake

# Assembler library
add_library(vortex_assembler STATIC
    assembler.c
)
target_include_directories(vortex_assembler PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${VORTEX_INCLUDE_DIRS}
)
target_link_libraries(vortex_assembler PUBLIC vortex_runtime)

# Disassembler library
add_library(vortex_disassembler STATIC
    disassembler.c
)
target_include_directories(vortex_disassembler PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${VORTEX_INCLUDE_DIRS}
)
target_link_libraries(vortex_disassembler PUBLIC vortex_runtime)

# Assembler CLI tool
add_executable(vortex_asm
    assembler_main.c
)
target_link_libraries(vortex_asm PRIVATE vortex_assembler vortex_runtime)

# Disassembler CLI tool
add_executable(vortex_disasm
    disassembler_main.c
)
target_link_libraries(vortex_disasm PRIVATE vortex_disassembler vortex_runtime)

# Profile introspection CLI tool (Sprint 1.5)
# Reads a persisted VORTEX profile file and dumps its contents in
# human-readable form. Used to debug PGO behavior.
add_executable(vortex-profile
    profile_dump.c
)
target_link_libraries(vortex-profile PRIVATE
    vortex_profile
    vortex_runtime
)
