# openrts_server: the openRTS server (docs/server.md), Legacy on: the RTS
# protocol for rts1.exe and openRTS clients, the server's own copy of the
# route (junctions, signals running their scripts, trains), the dispatcher's
# console. openrts-server is the program.
add_library(openrts_server STATIC
    src/console.cpp
    src/game_server.cpp
    src/server_route.cpp
    src/tcp_server.cpp)
openrts_configure_target(openrts_server)
target_link_libraries(openrts_server PUBLIC openrts_net_world openrts_simulation openrts_formats openrts_core)
if(WIN32)
    target_link_libraries(openrts_server PUBLIC ws2_32)
endif()

add_executable(openrts_server_app src/main.cpp)
openrts_configure_target(openrts_server_app)
set_target_properties(openrts_server_app PROPERTIES OUTPUT_NAME openrts-server)
target_link_libraries(openrts_server_app PRIVATE openrts_server)
if(WIN32)
    target_link_libraries(openrts_server_app PRIVATE shell32)
endif()
