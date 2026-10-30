add_library(openrts_formats STATIC
    src/ace_file.cpp
    src/formats.cpp
    src/shape_binary.cpp
    src/shape_file.cpp
    src/shape_description.cpp
    src/route_world.cpp
    src/environment.cpp
    src/terrain.cpp)
openrts_configure_target(openrts_formats)
target_link_libraries(openrts_formats PUBLIC openrts_core)
target_link_libraries(openrts_formats PRIVATE ZLIB::ZLIB)
