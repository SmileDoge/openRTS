add_library(openrts_simulation STATIC
    src/simulation.cpp
    src/track_geometry.cpp
    src/track_network.cpp
    src/track_traveller.cpp)
openrts_configure_target(openrts_simulation)
target_link_libraries(openrts_simulation PUBLIC openrts_core openrts_formats)
