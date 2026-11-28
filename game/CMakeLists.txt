# The start menu's updates (the latest release on GitHub, laid over the
# program's folder): a library of its own, so the tests reach it.
add_library(openrts_update STATIC src/update.cpp)
openrts_configure_target(openrts_update)
target_include_directories(openrts_update PUBLIC src)
target_link_libraries(openrts_update PUBLIC openrts_core)
if(WIN32)
    target_link_libraries(openrts_update PRIVATE winhttp bcrypt)
endif()

# openrts-updater: the update without the menu or the game (a dedicated
# server's computer): check, download, lay over its folder. A plain Windows
# window (no GPU needed); --check / --yes without one.
add_executable(openrts-updater WIN32 src/updater_main.cpp)
openrts_configure_target(openrts-updater)
openrts_add_icon(openrts-updater)
target_link_libraries(openrts-updater PRIVATE openrts_update openrts_core)
if(WIN32)
    target_link_libraries(openrts-updater PRIVATE shell32 comctl32)
endif()

if(TARGET openrts_viewer_core)
    # The start menu: route, consist, path, time, settings; it runs the game
    # (openrts_viewer) as a process of its own.
    add_executable(openrts_game src/main.cpp src/launcher.cpp)
    openrts_configure_target(openrts_game)
    openrts_add_icon(openrts_game)
    target_link_libraries(openrts_game PRIVATE openrts_viewer_core openrts_update)
    openrts_copy_wgpu_runtime(openrts_game)
    openrts_copy_streamline(openrts_game)
    # The game next to the menu, so that a build folder starts as is.
    add_dependencies(openrts_game openrts_viewer)
else()
    add_executable(openrts_game src/main.cpp)
    openrts_configure_target(openrts_game)
    openrts_add_icon(openrts_game)
    target_link_libraries(openrts_game PRIVATE openrts_assets openrts_renderer openrts_platform openrts_simulation)
endif()
