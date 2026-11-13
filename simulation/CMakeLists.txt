add_library(openrts_simulation STATIC
    src/simulation.cpp
    src/track_geometry.cpp
    src/track_network.cpp
    src/rail_joints.cpp
    src/body_motion.cpp
    src/distributor.cpp
    src/brake_shoes.cpp
    src/adhesion.cpp
    src/driver_valve.cpp
    src/track_traveller.cpp
    src/train.cpp
    src/realistic_air.cpp
    src/signals.cpp
    src/train_route.cpp
    src/dispatcher.cpp
    src/speed_limits.cpp
    src/auto_driver.cpp
    src/traffic.cpp
    src/electrification.cpp
    src/train_lights.cpp
    src/tape.cpp)
openrts_configure_target(openrts_simulation)
target_link_libraries(openrts_simulation PUBLIC openrts_core openrts_formats PRIVATE openrts_script)
