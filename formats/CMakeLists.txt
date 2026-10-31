add_library(openrts_formats STATIC
    src/ace_file.cpp
    src/formats.cpp
    src/shape_animation.cpp
    src/shape_binary.cpp
    src/shape_file.cpp
    src/shape_text.cpp
    src/sound.cpp
    src/shape_description.cpp
    src/rolling_stock.cpp
    src/train_path.cpp
    src/route_world.cpp
    src/environment.cpp
    src/terrain.cpp
    src/track_database.cpp
    src/track_sections.cpp)
openrts_configure_target(openrts_formats)
target_link_libraries(openrts_formats PUBLIC openrts_core)
target_link_libraries(openrts_formats PRIVATE ZLIB::ZLIB)
