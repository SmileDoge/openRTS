# openrts_ui: the interface's text in its languages (tr) and its fonts,
# shared by the viewer, the game, Track map and the server's window.
add_library(openrts_ui STATIC
    src/text.cpp
    src/fonts.cpp)
openrts_configure_target(openrts_ui)
target_link_libraries(openrts_ui PUBLIC openrts_core openrts_imgui)
target_compile_definitions(openrts_ui PUBLIC OPENRTS_HAS_IMGUI=1)
