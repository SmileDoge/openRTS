# openrts_net: the RTS network protocol (docs/net.md). No sockets yet: the
# framing, the package names and the startup snapshot.
add_library(openrts_net STATIC
    src/packages.cpp
    src/snapshot.cpp
    src/wire.cpp)
openrts_configure_target(openrts_net)

# openrts_net_world: a snapshot in openRTS terms (junctions, signal
# overrides, train fronts), on top of the simulation.
add_library(openrts_net_world STATIC
    src/world.cpp)
openrts_configure_target(openrts_net_world)
target_link_libraries(openrts_net_world PUBLIC openrts_net openrts_simulation)
