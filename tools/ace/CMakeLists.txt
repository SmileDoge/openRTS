# openrts-ace: ACE textures to PNG and back (DXT1, RGB, RGB + mask, RGBA),
# a file, a folder, or a double click on an .ace once registered.
add_executable(openrts_ace src/main.cpp)
set_target_properties(openrts_ace PROPERTIES OUTPUT_NAME openrts-ace)
openrts_configure_target(openrts_ace)
target_link_libraries(openrts_ace PRIVATE openrts_formats openrts_core)
if(WIN32)
    target_link_libraries(openrts_ace PRIVATE shell32 user32 advapi32)
endif()
