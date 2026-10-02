add_executable(openrts_viewer src/main.cpp)
openrts_configure_target(openrts_viewer)
target_link_libraries(openrts_viewer PRIVATE openrts_assets openrts_renderer openrts_platform)

if(OPENRTS_ENABLE_GLFW)
    target_link_libraries(openrts_viewer PRIVATE glfw)
    target_compile_definitions(openrts_viewer PRIVATE OPENRTS_HAS_GLFW=1)
endif()

if(OPENRTS_ENABLE_WGPU AND WIN32)
    add_custom_command(TARGET openrts_viewer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:openrts_wgpu_native>" "$<TARGET_FILE_DIR:openrts_viewer>")
endif()
