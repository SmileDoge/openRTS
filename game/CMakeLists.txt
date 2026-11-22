if(TARGET openrts_viewer_core)
    # The one game window: the menu, the route setup and the simulation live
    # in this process. src/shell.cpp switches between them.
    add_executable(openrts_game
        src/main.cpp
        src/launcher.cpp
        src/shell.cpp
        src/menu/ui_image.cpp
        src/menu/ui_sound.cpp)
    openrts_configure_target(openrts_game)
    target_link_libraries(openrts_game PRIVATE openrts_viewer_core winmm)
    if(WIN32)
        # WIC decodes the menu's pictures; ShellExecute opens its links.
        target_link_libraries(openrts_game PRIVATE windowscodecs ole32 shell32)
    endif()
    openrts_copy_wgpu_runtime(openrts_game)
    # The menu's faces and pictures beside the program, so a build folder
    # starts as is.
    add_custom_command(TARGET openrts_game POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/menu"
            "$<TARGET_FILE_DIR:openrts_game>/menu"
        COMMENT "Copying the menu assets")
    # The game next to the menu, so that a build folder starts as is.
    add_dependencies(openrts_game openrts_viewer)
else()
    add_executable(openrts_game src/main.cpp)
    openrts_configure_target(openrts_game)
    target_link_libraries(openrts_game PRIVATE openrts_assets openrts_renderer openrts_platform openrts_simulation)
endif()
