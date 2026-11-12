# The viewer's code as a library: openrts_viewer (the viewer and the game
# window) and openrts_game (the start menu) are built from it.
add_library(openrts_viewer_core STATIC
    src/camera.cpp
    src/debug_view.cpp
    src/viewer_scene.cpp
    src/net_session.cpp
    src/console_panel.cpp
    src/track_view.cpp
    src/crossing_view.cpp
    src/road_traffic.cpp
    src/particle_effects.cpp
    src/train_view.cpp
    src/signal_view.cpp
    src/track_control.cpp
    src/inspector.cpp
    src/tape_view.cpp
    src/key_repeat.cpp
    src/file_dialog.cpp
    src/viewer_state.cpp
    src/ui_text.cpp
    src/legacy_cab.cpp
    src/cab_view.cpp
    src/cab_screens.cpp
    src/engineer.cpp
    src/walk_rules.cpp
    src/key_layout.cpp
    src/frame_limiter.cpp
    src/head_motion.cpp
    src/activity_view.cpp
    src/graphics_settings.cpp
    src/controls_help.cpp
    src/window_mode.cpp
    src/window_viewer.cpp)
openrts_configure_target(openrts_viewer_core)
target_include_directories(openrts_viewer_core PUBLIC src)
target_link_libraries(openrts_viewer_core PUBLIC openrts_assets openrts_formats openrts_renderer openrts_platform openrts_simulation openrts_audio openrts_trainplugin openrts_net_world)

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

add_executable(openrts_viewer src/main.cpp src/crash_report.cpp)
openrts_configure_target(openrts_viewer)
target_link_libraries(openrts_viewer PRIVATE openrts_viewer_core)
if(WIN32)
    # dbghelp: the crash report; shell32: the command line as UTF-16.
    target_link_libraries(openrts_viewer PRIVATE dbghelp shell32)
endif()
if(MSVC)
    # A .pdb beside the exe in every configuration: the crash report names
    # the functions of a crash from it (crash_report.cpp).
    target_link_options(openrts_viewer PRIVATE /DEBUG /OPT:REF /OPT:ICF)
endif()

openrts_copy_wgpu_runtime(openrts_viewer)
openrts_copy_dllhost(openrts_viewer)
