# openrts_trackmap: the map of a route's track from above (Track map,
# include/openrts/trackmap/track_map.hpp), drawn with ImGui: the viewer's
# Track control and the server's window are built on it.
add_library(openrts_trackmap STATIC
    src/track_map.cpp)
openrts_configure_target(openrts_trackmap)
target_link_libraries(openrts_trackmap PUBLIC openrts_simulation openrts_formats openrts_ui openrts_imgui)
