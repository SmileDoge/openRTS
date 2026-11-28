if(TARGET openrts_viewer_core)
    # The one game window: the menu, the route setup and the simulation live
    # in this process. src/shell.cpp switches between them.
    add_executable(openrts_game
        src/main.cpp
        src/launcher.cpp
        src/account.cpp
        src/shell.cpp
        src/menu/ui_image.cpp
        src/menu/ui_sound.cpp)
    openrts_configure_target(openrts_game)
    openrts_add_icon(openrts_game)
    openrts_embed_dllhost(openrts_game)
    target_link_libraries(openrts_game PRIVATE openrts_viewer_core winmm)
    if(WIN32)
        # WIC decodes the menu's pictures; ShellExecute opens its links;
        # WinHTTP signs in on the site, BCrypt hashes the hardware id.
        target_link_libraries(openrts_game PRIVATE windowscodecs ole32 shell32 winhttp bcrypt iphlpapi)
    endif()
    openrts_copy_wgpu_runtime(openrts_game)
    openrts_copy_streamline(openrts_game)
    # The 32-bit host is embedded (openrts_embed_dllhost): no separate file.
    # The menu's faces and pictures beside the program, so a build folder
    # starts as is.
    add_custom_command(TARGET openrts_game POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/menu"
            "$<TARGET_FILE_DIR:openrts_game>/menu"
        COMMENT "Copying the menu assets")
    # The game next to the menu, so that a build folder starts as is.
    add_dependencies(openrts_game openrts_viewer)
    add_custom_command(TARGET openrts_game POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:openrts_viewer>" "$<TARGET_FILE_DIR:openrts_game>/openrts_viewer.exe"
        COMMENT "Copying the viewer next to the menu")
else()
    add_executable(openrts_game src/main.cpp)
    openrts_configure_target(openrts_game)
    openrts_add_icon(openrts_game)
    openrts_embed_dllhost(openrts_game)
    target_link_libraries(openrts_game PRIVATE openrts_assets openrts_renderer openrts_platform openrts_simulation)
endif()

if(WIN32 AND TARGET openrts_game)
    # The game is a GUI program: no console window appears when it starts.
    set_target_properties(openrts_game PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
