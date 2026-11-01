add_executable(openrts_viewer
    src/main.cpp
    src/camera.cpp
    src/debug_view.cpp
    src/gpu_timer.cpp
    src/viewer_scene.cpp
    src/track_view.cpp
    src/train_view.cpp
    src/signal_view.cpp
    src/track_control.cpp
    src/window_viewer.cpp)
openrts_configure_target(openrts_viewer)
target_link_libraries(openrts_viewer PRIVATE openrts_assets openrts_formats openrts_renderer openrts_platform openrts_simulation openrts_audio)

if(OPENRTS_ENABLE_GLFW)
    target_link_libraries(openrts_viewer PRIVATE glfw)
    target_compile_definitions(openrts_viewer PRIVATE OPENRTS_HAS_GLFW=1)
endif()

if(TARGET openrts_imgui)
    target_link_libraries(openrts_viewer PRIVATE openrts_imgui)
    target_compile_definitions(openrts_viewer PRIVATE OPENRTS_HAS_IMGUI=1)
endif()

openrts_copy_wgpu_runtime(openrts_viewer)
