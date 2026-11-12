# Luau for openRTS's own modules (docs/scripting.md): a sandboxed VM with a
# time limit, compiling and running chunks.
add_library(openrts_script STATIC
    src/luau.cpp)
openrts_configure_target(openrts_script)
target_link_libraries(openrts_script PUBLIC Luau.VM PRIVATE Luau.Compiler)
