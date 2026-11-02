if(TARGET openrts_viewer_core)
    # The start menu: route, consist, path, time, settings; it runs the game
    # (openrts_viewer) as a process of its own.
    add_executable(openrts_game src/main.cpp src/launcher.cpp)
    openrts_configure_target(openrts_game)
    target_link_libraries(openrts_game PRIVATE openrts_viewer_core)
    openrts_copy_wgpu_runtime(openrts_game)
    # The game next to the menu, so that a build folder starts as is.
    add_dependencies(openrts_game openrts_viewer)
else()
    add_executable(openrts_game src/main.cpp)
    openrts_configure_target(openrts_game)
    target_link_libraries(openrts_game PRIVATE openrts_assets openrts_renderer openrts_platform openrts_simulation)
endif()
