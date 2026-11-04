# The viewer's code as a library: openrts_viewer (the viewer and the game
# window) and openrts_game (the start menu) are built from it.
add_library(openrts_viewer_core STATIC
    src/camera.cpp
    src/debug_view.cpp
    src/gpu_timer.cpp
    src/viewer_scene.cpp
    src/track_view.cpp
    src/crossing_view.cpp
    src/road_traffic.cpp
    src/particle_effects.cpp
    src/train_view.cpp
    src/signal_view.cpp
    src/track_control.cpp
    src/file_dialog.cpp
    src/viewer_state.cpp
    src/ui_text.cpp
    src/legacy_cab.cpp
    src/cab_view.cpp
    src/cab_screens.cpp
    src/engineer.cpp
    src/key_layout.cpp
    src/window_viewer.cpp)
openrts_configure_target(openrts_viewer_core)
target_include_directories(openrts_viewer_core PUBLIC src)
target_link_libraries(openrts_viewer_core PUBLIC openrts_assets openrts_formats openrts_renderer openrts_platform openrts_simulation openrts_audio openrts_trainplugin)

if(OPENRTS_ENABLE_GLFW)
    target_link_libraries(openrts_viewer_core PUBLIC glfw)
    target_compile_definitions(openrts_viewer_core PUBLIC OPENRTS_HAS_GLFW=1)
endif()

if(TARGET openrts_imgui)
    target_link_libraries(openrts_viewer_core PUBLIC openrts_imgui)
    target_compile_definitions(openrts_viewer_core PUBLIC OPENRTS_HAS_IMGUI=1)
endif()

if(WIN32)
    target_link_libraries(openrts_viewer_core PUBLIC comdlg32)
endif()

add_executable(openrts_viewer src/main.cpp)
openrts_configure_target(openrts_viewer)
target_link_libraries(openrts_viewer PRIVATE openrts_viewer_core)

openrts_copy_wgpu_runtime(openrts_viewer)
openrts_copy_dllhost(openrts_viewer)
