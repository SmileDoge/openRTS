# openrts_net: the RTS network protocol (docs/net.md): the framing, the
# package names, the startup snapshot, the packages of a game in play, and a
# TCP connection to an RTS server as a player.
add_library(openrts_net STATIC
    src/connection.cpp
    src/packages.cpp
    src/snapshot.cpp
    src/wire.cpp)
openrts_configure_target(openrts_net)
if(WIN32)
    target_link_libraries(openrts_net PUBLIC ws2_32)
endif()
find_package(Threads REQUIRED)
target_link_libraries(openrts_net PUBLIC Threads::Threads)

# openrts_net_world: a snapshot in openRTS terms (junctions, signal
# overrides, train fronts, remote trains' motion), on top of the simulation.
add_library(openrts_net_world STATIC
    src/world.cpp)
openrts_configure_target(openrts_net_world)
target_link_libraries(openrts_net_world PUBLIC openrts_net openrts_simulation)
