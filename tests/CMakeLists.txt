add_executable(openrts_core_smoke core_smoke.cpp)
openrts_configure_target(openrts_core_smoke)
openrts_copy_wgpu_runtime(openrts_core_smoke)
target_link_libraries(openrts_core_smoke PRIVATE openrts_core openrts_assets openrts_renderer)
add_test(NAME openrts.core_smoke COMMAND openrts_core_smoke)

add_executable(openrts_rts_material_test rts_material_test.cpp)
openrts_configure_target(openrts_rts_material_test)
openrts_copy_wgpu_runtime(openrts_rts_material_test)
target_link_libraries(openrts_rts_material_test PRIVATE openrts_renderer)
add_test(NAME openrts.rts_material COMMAND openrts_rts_material_test)

add_executable(openrts_light_rings_test light_rings_test.cpp)
openrts_configure_target(openrts_light_rings_test)
openrts_copy_wgpu_runtime(openrts_light_rings_test)
target_link_libraries(openrts_light_rings_test PRIVATE openrts_renderer)
add_test(NAME openrts.light_rings COMMAND openrts_light_rings_test)

add_executable(openrts_pbr_material_test pbr_material_test.cpp)
openrts_configure_target(openrts_pbr_material_test)
target_link_libraries(openrts_pbr_material_test PRIVATE openrts_renderer)
add_test(NAME openrts.pbr_material COMMAND openrts_pbr_material_test)

add_executable(openrts_sky_light_test sky_light_test.cpp)
openrts_configure_target(openrts_sky_light_test)
target_link_libraries(openrts_sky_light_test PRIVATE openrts_renderer)
add_test(NAME openrts.sky_light COMMAND openrts_sky_light_test)

add_executable(openrts_rts_sky_test rts_sky_test.cpp)
openrts_configure_target(openrts_rts_sky_test)
openrts_copy_wgpu_runtime(openrts_rts_sky_test)
target_link_libraries(openrts_rts_sky_test PRIVATE openrts_renderer)
add_test(NAME openrts.rts_sky COMMAND openrts_rts_sky_test)

add_executable(openrts_terrain_test terrain_test.cpp)
openrts_configure_target(openrts_terrain_test)
openrts_copy_wgpu_runtime(openrts_terrain_test)
target_link_libraries(openrts_terrain_test PRIVATE openrts_renderer)
add_test(NAME openrts.terrain COMMAND openrts_terrain_test)

add_executable(openrts_lightning_test lightning_test.cpp)
openrts_configure_target(openrts_lightning_test)
openrts_copy_wgpu_runtime(openrts_lightning_test)
target_link_libraries(openrts_lightning_test PRIVATE openrts_renderer)
add_test(NAME openrts.lightning COMMAND openrts_lightning_test)

add_executable(openrts_frustum_test frustum_test.cpp)
openrts_configure_target(openrts_frustum_test)
openrts_copy_wgpu_runtime(openrts_frustum_test)
target_link_libraries(openrts_frustum_test PRIVATE openrts_renderer)
add_test(NAME openrts.frustum COMMAND openrts_frustum_test)

add_executable(openrts_bc7_test bc7_test.cpp)
openrts_configure_target(openrts_bc7_test)
openrts_copy_wgpu_runtime(openrts_bc7_test)
target_link_libraries(openrts_bc7_test PRIVATE openrts_renderer openrts_formats)
add_test(NAME openrts.bc7 COMMAND openrts_bc7_test)

add_executable(openrts_bc_gpu_test bc_gpu_test.cpp)
openrts_configure_target(openrts_bc_gpu_test)
openrts_copy_wgpu_runtime(openrts_bc_gpu_test)
target_link_libraries(openrts_bc_gpu_test PRIVATE openrts_renderer openrts_formats)
add_test(NAME openrts.bc_gpu COMMAND openrts_bc_gpu_test)
if(OPENRTS_TEST_ROOT)
    add_test(NAME openrts.content.bc7 COMMAND openrts_bc7_test "${OPENRTS_TEST_ROOT}")
endif()

add_executable(openrts_vfs_test vfs_test.cpp)
openrts_configure_target(openrts_vfs_test)
target_link_libraries(openrts_vfs_test PRIVATE openrts_core)
add_test(NAME openrts.vfs COMMAND openrts_vfs_test)

# openRTS's materials: JSON5, the material files, merging and selectors.
add_executable(openrts_material_test material_test.cpp)
openrts_configure_target(openrts_material_test)
target_link_libraries(openrts_material_test PRIVATE openrts_renderer)
target_compile_definitions(openrts_material_test PRIVATE OPENRTS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME openrts.material COMMAND openrts_material_test)

add_executable(openrts_retire_queue_test retire_queue_test.cpp)
openrts_configure_target(openrts_retire_queue_test)
add_test(NAME openrts.retire_queue COMMAND openrts_retire_queue_test)

add_executable(openrts_track_test track_test.cpp)
openrts_configure_target(openrts_track_test)
target_link_libraries(openrts_track_test PRIVATE openrts_simulation)
add_test(NAME openrts.track COMMAND openrts_track_test)

add_executable(openrts_train_test train_test.cpp)
openrts_configure_target(openrts_train_test)
target_link_libraries(openrts_train_test PRIVATE openrts_simulation openrts_formats)
target_compile_definitions(openrts_train_test PRIVATE OPENRTS_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_test(NAME openrts.train COMMAND openrts_train_test)

add_executable(openrts_signal_test signal_test.cpp)
openrts_configure_target(openrts_signal_test)
target_link_libraries(openrts_signal_test PRIVATE openrts_simulation openrts_formats)
add_test(NAME openrts.signal COMMAND openrts_signal_test)

add_executable(openrts_shape_text_test shape_text_test.cpp)
openrts_configure_target(openrts_shape_text_test)
target_link_libraries(openrts_shape_text_test PRIVATE openrts_formats)
add_test(NAME openrts.shape_text COMMAND openrts_shape_text_test)

add_executable(openrts_sound_test sound_test.cpp)
openrts_configure_target(openrts_sound_test)
target_link_libraries(openrts_sound_test PRIVATE openrts_audio openrts_formats openrts_core)
add_test(NAME openrts.sound COMMAND openrts_sound_test)
if(OPENRTS_TEST_ROOT)
    add_test(NAME openrts.content.sound_ed9m COMMAND openrts_sound_test "${OPENRTS_TEST_ROOT}")
endif()

# ACE mipmaps: built levels and the files' own chains.
add_executable(openrts_ace_mip_test ace_mip_test.cpp)
openrts_configure_target(openrts_ace_mip_test)
target_link_libraries(openrts_ace_mip_test PRIVATE openrts_formats openrts_core)
add_test(NAME openrts.ace_mip COMMAND openrts_ace_mip_test)
if(OPENRTS_TEST_ROOT)
    add_test(NAME openrts.content.ace_mips COMMAND openrts_ace_mip_test "${OPENRTS_TEST_ROOT}")
endif()

# Night and seasonal textures, scenery lamps (ESD_Alternative_Texture, ESD_LightSources).
add_executable(openrts_alternative_texture_test alternative_texture_test.cpp)
openrts_configure_target(openrts_alternative_texture_test)
target_link_libraries(openrts_alternative_texture_test PRIVATE openrts_renderer openrts_simulation openrts_formats openrts_core)
add_test(NAME openrts.alternative_texture COMMAND openrts_alternative_texture_test)
if(OPENRTS_TEST_ROOT)
    add_test(NAME openrts.content.night_textures COMMAND openrts_alternative_texture_test "${OPENRTS_TEST_ROOT}")
endif()

# The RTS wire format, package names and the startup snapshot (openrts_net).
add_executable(openrts_net_test net_test.cpp)
openrts_configure_target(openrts_net_test)
target_link_libraries(openrts_net_test PRIVATE openrts_net)
add_test(NAME openrts.net COMMAND openrts_net_test)

# A snapshot in openRTS terms on a synthetic track: distances, junctions,
# train fronts, remote trains' motion.
# The openRTS server: the login, the snapshot, the relays, couplings, over
# TCP with openRTS's client; with an installation, Kuban against rts_server.exe.
add_executable(openrts_server_test server_test.cpp)
openrts_configure_target(openrts_server_test)
target_link_libraries(openrts_server_test PRIVATE openrts_server)
add_test(NAME openrts.server COMMAND openrts_server_test)
if(OPENRTS_TEST_ROOT)
    add_test(NAME openrts.content.server_kuban COMMAND openrts_server_test "${OPENRTS_TEST_ROOT}")
endif()

add_executable(openrts_net_world_test net_world_test.cpp)
openrts_configure_target(openrts_net_world_test)
target_link_libraries(openrts_net_world_test PRIVATE openrts_net_world openrts_formats)
add_test(NAME openrts.net_world COMMAND openrts_net_world_test)
if(OPENRTS_TEST_ROOT)
    add_test(NAME openrts.content.net_multdata COMMAND openrts_net_test "${OPENRTS_TEST_ROOT}")
endif()

add_executable(openrts_path_test path_test.cpp)
openrts_configure_target(openrts_path_test)
target_link_libraries(openrts_path_test PRIVATE openrts_simulation openrts_formats)
add_test(NAME openrts.path COMMAND openrts_path_test)

# Labels hidden behind the scene (occlusion queries) and the wide helper lines, on a headless device.
if(OPENRTS_ENABLE_WGPU)
    add_executable(openrts_occlusion_probe_test occlusion_probe_test.cpp)
    openrts_configure_target(openrts_occlusion_probe_test)
    target_link_libraries(openrts_occlusion_probe_test PRIVATE openrts_renderer)
    openrts_copy_wgpu_runtime(openrts_occlusion_probe_test)
    add_test(NAME openrts.occlusion_probe COMMAND openrts_occlusion_probe_test)

    # Auto exposure (PBR stage 3): the measured light and the factor.
    add_executable(openrts_auto_exposure_test auto_exposure_test.cpp)
    openrts_configure_target(openrts_auto_exposure_test)
    target_link_libraries(openrts_auto_exposure_test PRIVATE openrts_renderer)
    openrts_copy_wgpu_runtime(openrts_auto_exposure_test)
    add_test(NAME openrts.auto_exposure COMMAND openrts_auto_exposure_test)

    # Every scene shader builds into its pipelines (models, terrain, tone pass).
    add_executable(openrts_shader_compile_test shader_compile_test.cpp)
    openrts_configure_target(openrts_shader_compile_test)
    target_link_libraries(openrts_shader_compile_test PRIVATE openrts_renderer)
    openrts_copy_wgpu_runtime(openrts_shader_compile_test)
    add_test(NAME openrts.shader_compile COMMAND openrts_shader_compile_test)
endif()

add_executable(openrts_electrification_test electrification_test.cpp)
openrts_configure_target(openrts_electrification_test)
target_link_libraries(openrts_electrification_test PRIVATE openrts_simulation openrts_formats)
add_test(NAME openrts.electrification COMMAND openrts_electrification_test)

add_executable(openrts_train_lights_test train_lights_test.cpp)
openrts_configure_target(openrts_train_lights_test)
target_link_libraries(openrts_train_lights_test PRIVATE openrts_simulation openrts_formats)
add_test(NAME openrts.train_lights COMMAND openrts_train_lights_test)

# openrts-ace's formats: PNG and ACE written and read back (a real DXT1 one with the content).
add_executable(openrts_ace_png_test ace_png_test.cpp)
openrts_configure_target(openrts_ace_png_test)
target_link_libraries(openrts_ace_png_test PRIVATE openrts_formats ZLIB::ZLIB)
add_test(NAME openrts.ace_png COMMAND openrts_ace_png_test)
if(OPENRTS_TEST_ROOT)
    add_test(NAME openrts.content.ace_png COMMAND openrts_ace_png_test "${OPENRTS_TEST_ROOT}")
endif()

# A world tile's objects: every kind keeps its QDirection.
add_executable(openrts_world_parse_test world_parse_test.cpp)
openrts_configure_target(openrts_world_parse_test)
target_link_libraries(openrts_world_parse_test PRIVATE openrts_formats)
add_test(NAME openrts.world_parse COMMAND openrts_world_parse_test)

# RTS scenarios (.act): header, services, trains, events; Kuban's with the content.
add_executable(openrts_activity_test activity_test.cpp)
openrts_configure_target(openrts_activity_test)
target_link_libraries(openrts_activity_test PRIVATE openrts_formats)
add_test(NAME openrts.activity COMMAND openrts_activity_test)
if(OPENRTS_TEST_ROOT)
    add_test(NAME openrts.content.activity COMMAND openrts_activity_test "${OPENRTS_TEST_ROOT}")
endif()

# A scenario's events run on a made-up world; with the content, Novokuznetsk's
# "Скорый поезд 77" played through when it is installed.
add_executable(openrts_activity_run_test activity_run_test.cpp ../viewer/src/activity_view.cpp ../ui/src/text.cpp)
openrts_configure_target(openrts_activity_run_test)
target_link_libraries(openrts_activity_run_test PRIVATE openrts_formats openrts_simulation)
add_test(NAME openrts.activity_run COMMAND openrts_activity_run_test)
if(OPENRTS_TEST_ROOT)
    add_test(NAME openrts.content.activity_run COMMAND openrts_activity_run_test "${OPENRTS_TEST_ROOT}")
endif()

# The frame limit: the setting in front, 30 frames/s in the background.
add_executable(openrts_frame_limiter_test frame_limiter_test.cpp ../viewer/src/frame_limiter.cpp)
openrts_configure_target(openrts_frame_limiter_test)
add_test(NAME openrts.frame_limiter COMMAND openrts_frame_limiter_test)

# The driver's head on its neck in the cab: braking, the stop, joints, frame rate.
add_executable(openrts_head_motion_test head_motion_test.cpp ../viewer/src/head_motion.cpp)
openrts_configure_target(openrts_head_motion_test)
target_link_libraries(openrts_head_motion_test PRIVATE glm::glm)
add_test(NAME openrts.head_motion COMMAND openrts_head_motion_test)

# openRTS's КЛУБ's electronic map on Kuban's route database (content).
add_executable(openrts_electronic_map_test electronic_map_test.cpp)
openrts_configure_target(openrts_electronic_map_test)
target_link_libraries(openrts_electronic_map_test PRIVATE openrts_simulation)
if(OPENRTS_TEST_ROOT)
    add_test(NAME openrts.content.electronic_map COMMAND openrts_electronic_map_test "${OPENRTS_TEST_ROOT}")
endif()

# The engineer's keys in the prompts: E and Shift+E on foot in openRTS's keys.
add_executable(openrts_key_layout_test key_layout_test.cpp ../viewer/src/key_layout.cpp)
openrts_configure_target(openrts_key_layout_test)
target_link_libraries(openrts_key_layout_test PRIVATE openrts_ui openrts_simulation)
add_test(NAME openrts.key_layout COMMAND openrts_key_layout_test)

# The mouse's look smoothed: the whole move taken, a jump spread over the frames.
add_executable(openrts_mouse_look_test mouse_look_test.cpp)
openrts_configure_target(openrts_mouse_look_test)
add_test(NAME openrts.mouse_look COMMAND openrts_mouse_look_test)

# The rail joints along the track: RTS's layout and openRTS's.
add_executable(openrts_rail_joints_test rail_joints_test.cpp)
openrts_configure_target(openrts_rail_joints_test)
target_link_libraries(openrts_rail_joints_test PRIVATE openrts_simulation)
add_test(NAME openrts.rail_joints COMMAND openrts_rail_joints_test)

# The tape (RTS's "tape roll"): the line as RTS writes it, the file's name, a line a game second.
add_executable(openrts_tape_test tape_test.cpp)
openrts_configure_target(openrts_tape_test)
target_link_libraries(openrts_tape_test PRIVATE openrts_simulation)
add_test(NAME openrts.tape COMMAND openrts_tape_test)

# Trains with legacy DLLs under the realistic air: the DLL's valve, openRTS's
# valve over Brakes.dll (the DLL controller on outputs made by the test).
add_executable(openrts_dll_air_test dll_air_test.cpp)
openrts_configure_target(openrts_dll_air_test)
target_link_libraries(openrts_dll_air_test PRIVATE openrts_trainplugin openrts_simulation openrts_formats)
add_test(NAME openrts.dll_air COMMAND openrts_dll_air_test)

# Brake distributors: the built-in ones on the stand, the Luau 292 as the
# built-in one, Luau's sandbox and time limit.
add_executable(openrts_cab_brakes_test cab_brakes_test.cpp)
openrts_configure_target(openrts_cab_brakes_test)
target_link_libraries(openrts_cab_brakes_test PRIVATE openrts_simulation)
add_test(NAME openrts.cab_brakes COMMAND openrts_cab_brakes_test)

add_executable(openrts_klub_test klub_test.cpp)
openrts_configure_target(openrts_klub_test)
target_link_libraries(openrts_klub_test PRIVATE openrts_simulation)
add_test(NAME openrts.klub COMMAND openrts_klub_test)

add_executable(openrts_distributor_test distributor_test.cpp)
openrts_configure_target(openrts_distributor_test)
target_link_libraries(openrts_distributor_test PRIVATE openrts_simulation openrts_script)
target_compile_definitions(openrts_distributor_test PRIVATE OPENRTS_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_test(NAME openrts.distributor COMMAND openrts_distributor_test)

# Cab keys held down repeat once per key delay (RTS 0x414340).
add_executable(openrts_key_repeat_test key_repeat_test.cpp ../viewer/src/key_repeat.cpp)
openrts_configure_target(openrts_key_repeat_test)
add_test(NAME openrts.key_repeat COMMAND openrts_key_repeat_test)

# openRTS's body motion: the track's errors, cars not in step, curves, joints.
add_executable(openrts_body_motion_test body_motion_test.cpp)
openrts_configure_target(openrts_body_motion_test)
target_link_libraries(openrts_body_motion_test PRIVATE openrts_simulation)
add_test(NAME openrts.body_motion COMMAND openrts_body_motion_test)

# A scenario train's timetable: its stops on the route, the boarding (F6, Ctrl+Num7).
add_executable(openrts_timetable_test timetable_test.cpp)
openrts_configure_target(openrts_timetable_test)
target_link_libraries(openrts_timetable_test PRIVATE openrts_simulation)
add_test(NAME openrts.timetable COMMAND openrts_timetable_test)
if(OPENRTS_TEST_ROOT)
    add_test(NAME openrts.content.timetable COMMAND openrts_timetable_test "${OPENRTS_TEST_ROOT}")
endif()

# The graphics settings the start menu and the game share: defaults, presets, what is written.
add_executable(openrts_graphics_settings_test graphics_settings_test.cpp ../viewer/src/graphics_settings.cpp ../viewer/src/viewer_state.cpp)
openrts_configure_target(openrts_graphics_settings_test)
openrts_copy_wgpu_runtime(openrts_graphics_settings_test)
target_link_libraries(openrts_graphics_settings_test PRIVATE openrts_renderer ZLIB::ZLIB)
add_test(NAME openrts.graphics_settings COMMAND openrts_graphics_settings_test)

# openRTS's dynamic weather: deterministic, without jumps, by the season, a change by hand.
add_executable(openrts_weather_cycle_test weather_cycle_test.cpp ../viewer/src/weather_cycle.cpp)
openrts_configure_target(openrts_weather_cycle_test)
target_include_directories(openrts_weather_cycle_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
add_test(NAME openrts.weather_cycle COMMAND openrts_weather_cycle_test)

# The start menu's updates: versions, the release JSON, an archive laid over a folder.
if(TARGET openrts_update)
    add_executable(openrts_update_test update_test.cpp)
    openrts_configure_target(openrts_update_test)
    target_link_libraries(openrts_update_test PRIVATE openrts_update)
    add_test(NAME openrts.update COMMAND openrts_update_test)
endif()

add_executable(openrts_ui_text_test ui_text_test.cpp ../ui/src/text.cpp)
openrts_configure_target(openrts_ui_text_test)
target_link_libraries(openrts_ui_text_test PRIVATE openrts_core)
add_test(NAME openrts.ui_text COMMAND openrts_ui_text_test)

# The legacy DLL host end to end with its fake locomotive DLL (no RTS content needed).
if(TARGET openrts_dllhost32)
    add_executable(openrts_dllhost_test dllhost_test.cpp)
    openrts_configure_target(openrts_dllhost_test)
    target_link_libraries(openrts_dllhost_test PRIVATE openrts_trainplugin)
    add_dependencies(openrts_dllhost_test openrts_dllhost32)
    add_test(NAME openrts.dllhost COMMAND openrts_dllhost_test
        "${OPENRTS_DLLHOST_DIR}/$<CONFIG>/openrts-dllhost32.exe"
        "${OPENRTS_DLLHOST_DIR}/$<CONFIG>/openrts_fake_loco.dll")
endif()

# A legacy DLL (through the x86 host) and a native plugin with the same logic
# give the same results for one scripted run (docs/train-dll.md, parity).
if(TARGET openrts_dllhost32 AND TARGET openrts_fake_native)
    add_executable(openrts_plugin_parity_test plugin_parity_test.cpp)
    openrts_configure_target(openrts_plugin_parity_test)
    target_link_libraries(openrts_plugin_parity_test PRIVATE openrts_trainplugin)
    add_dependencies(openrts_plugin_parity_test openrts_dllhost32 openrts_fake_native)
    add_test(NAME openrts.plugin_parity COMMAND openrts_plugin_parity_test
        "${OPENRTS_DLLHOST_DIR}/$<CONFIG>/openrts-dllhost32.exe"
        "${OPENRTS_DLLHOST_DIR}/$<CONFIG>/openrts_fake_loco.dll"
        "$<TARGET_FILE:openrts_fake_native>")
endif()

# Tests on real RTS content, when an installation is given:
#   cmake -DOPENRTS_TEST_ROOT=D:/Games/RTS ...
set(OPENRTS_TEST_ROOT "" CACHE PATH "RTS installation for content tests (empty: skipped)")
if(OPENRTS_TEST_ROOT AND TARGET openrts_dllhost32 AND TARGET openrts_train_check)
    # Every installed DLL loads, answers DLLVersion and accepts the host functions.
    add_test(NAME openrts.content.dll_probe COMMAND "${OPENRTS_DLLHOST_DIR}/$<CONFIG>/openrts-dllhost32.exe"
        --probe "${OPENRTS_TEST_ROOT}/TRAINS/TRAINSET" --rts "${OPENRTS_TEST_ROOT}/RTS")
    set_tests_properties(openrts.content.dll_probe PROPERTIES
        PASS_REGULAR_EXPRESSION "[0-9]+ accepted, [0-9]+ refused" FAIL_REGULAR_EXPRESSION "crashed")
    # The VL10k DLL drives the train simulation through the host.
    add_test(NAME openrts.content.dll_vl10k COMMAND openrts_train_check "${OPENRTS_TEST_ROOT}"
        --dll tsrLoco_VL10k-1461 vl10k-1461a vl10k-1461b --script "${CMAKE_CURRENT_SOURCE_DIR}/data/vl10k.script")
    # Section b cut off and coupled back: the DLLs keep their state (RTS 0x4F995C).
    add_test(NAME openrts.content.dll_vl10k_couple COMMAND openrts_train_check "${OPENRTS_TEST_ROOT}"
        --dll tsrLoco_VL10k-1461 vl10k-1461a vl10k-1461b --script "${CMAKE_CURRENT_SOURCE_DIR}/data/vl10k_couple.script")
    # The ED9M's reverser, a KEY switch the DLL locks while another head cab has its key in.
    add_test(NAME openrts.content.dll_ed9m COMMAND openrts_train_check "${OPENRTS_TEST_ROOT}"
        --dll mdd_ED9M-0062 mdd_ED9M-0062_G mdd_ED9M-0062_M mdd_ED9M-0062_P mdd_ED9M-0062_GR
        --script "${CMAKE_CURRENT_SOURCE_DIR}/data/ed9m.script" --seconds 30)
    # Driven from the rear head car: its slaves from it outward, the far head car last.
    add_test(NAME openrts.content.dll_ed9m_rear COMMAND openrts_train_check "${OPENRTS_TEST_ROOT}"
        --dll mdd_ED9M-0062 mdd_ED9M-0062_G mdd_ED9M-0062_M mdd_ED9M-0062_P mdd_ED9M-0062_GR
        --script "${CMAKE_CURRENT_SOURCE_DIR}/data/ed9m_rear.script" --seconds 11)
    # The EP20's two cabs: openRTS's valve takes the working one's handle.
    add_test(NAME openrts.content.dll_ep20_cabs COMMAND openrts_train_check "${OPENRTS_TEST_ROOT}"
        --dll mdd_EP20-063 mdd_EP20-063 --start on --air own
        --script "${CMAKE_CURRENT_SOURCE_DIR}/data/ep20_cabs.script" --seconds 11)
    # Changing ends on a six-car ED9M: the new cab's ЭПК key and КЛУБ.
    add_test(NAME openrts.content.dll_ed9m_change_ends COMMAND openrts_train_check "${OPENRTS_TEST_ROOT}"
        --dll mdd_ED9M-0060 mdd_ED9M-0060_G mdd_ED9M-0060_M mdd_ED9M-0060_P mdd_ED9M-0060_M mdd_ED9M-0060_MR mdd_ED9M-0060_GR
        --script "${CMAKE_CURRENT_SOURCE_DIR}/data/ed9m_change_ends.script" --seconds 35)
    # openRTS's КЛУБ-У beside Brakes.dll's on the ED9M, step by step
    # (docs/brakes-dll-replacement.md, step 3): with the ЭК, and its modes.
    add_test(NAME openrts.content.klub_ed9m COMMAND openrts_train_check "${OPENRTS_TEST_ROOT}"
        --dll mdd_ED9M-0060 mdd_ED9M-0060_G mdd_ED9M-0060_M mdd_ED9M-0060_P mdd_ED9M-0060_M mdd_ED9M-0060_MR mdd_ED9M-0060_GR
        --script "${CMAKE_CURRENT_SOURCE_DIR}/data/golden/ed9m_klub.script" --klub shadow)
    add_test(NAME openrts.content.klub_ed9m_modes COMMAND openrts_train_check "${OPENRTS_TEST_ROOT}"
        --dll mdd_ED9M-0060 mdd_ED9M-0060_G mdd_ED9M-0060_M mdd_ED9M-0060_P mdd_ED9M-0060_M mdd_ED9M-0060_MR mdd_ED9M-0060_GR
        --script "${CMAKE_CURRENT_SOURCE_DIR}/data/golden/ed9m_klub_modes.script" --klub shadow)
    # Its screens beside Brakes.dll's: the БЛОК (2ЭС6, ЭП20), KLUB_BILTYPE 2
    # (ЭП2К), two cabs (ЭП20, ЭП2К, ЭП1), САУТ's speed (ЭП1); standing, the
    # ЭК, К799 and the aspects (tests/data/golden/klub_*.script).
    add_test(NAME openrts.content.klub_2es6 COMMAND openrts_train_check "${OPENRTS_TEST_ROOT}"
        --dll mdd_2ES6-114 mdd_2ES6-114a mdd_2ES6-114b --start on --seconds 66
        --script "${CMAKE_CURRENT_SOURCE_DIR}/data/golden/klub_2es6.script" --klub shadow)
    add_test(NAME openrts.content.klub_ep20 COMMAND openrts_train_check "${OPENRTS_TEST_ROOT}"
        --dll mdd_EP20-001 mdd_EP20-001 --start on --seconds 66
        --script "${CMAKE_CURRENT_SOURCE_DIR}/data/golden/klub_ep20.script" --klub shadow)
    add_test(NAME openrts.content.klub_ep2k COMMAND openrts_train_check "${OPENRTS_TEST_ROOT}"
        --dll mdd_EP2K-025 mdd_EP2k-025 --start on --seconds 66
        --script "${CMAKE_CURRENT_SOURCE_DIR}/data/golden/klub_ep2k.script" --klub shadow)
    add_test(NAME openrts.content.klub_ep1 COMMAND openrts_train_check "${OPENRTS_TEST_ROOT}"
        --dll mdd_EP1-002 mdd_EP1-002_RTS --start on --seconds 66
        --script "${CMAKE_CURRENT_SOURCE_DIR}/data/golden/klub_ep1.script" --klub shadow)
    # The same in the game's way: openRTS's КЛУБ in place of Brakes.dll's
    # (Brakes.dll's switched off), on the realistic air and openRTS's brakes.
    # ТСКБМ (PB_SETUP 4) in openRTS's КЛУБ: beside Brakes.dll's, and working the train.
    add_test(NAME openrts.content.klub_ed9m_tskbm COMMAND openrts_train_check "${OPENRTS_TEST_ROOT}"
        --dll mdd_ED9M-0060 mdd_ED9M-0060_G mdd_ED9M-0060_M mdd_ED9M-0060_P mdd_ED9M-0060_M mdd_ED9M-0060_MR mdd_ED9M-0060_GR
        --script "${CMAKE_CURRENT_SOURCE_DIR}/data/golden/ed9m_tskbm.script" --klub shadow)
    add_test(NAME openrts.content.klub_ed9m_tskbm_own COMMAND openrts_train_check "${OPENRTS_TEST_ROOT}"
        --dll mdd_ED9M-0060 mdd_ED9M-0060_G mdd_ED9M-0060_M mdd_ED9M-0060_P mdd_ED9M-0060_M mdd_ED9M-0060_MR mdd_ED9M-0060_GR
        --script "${CMAKE_CURRENT_SOURCE_DIR}/data/golden/ed9m_tskbm.script" --klub openrts --brakes openrts)
    # Brakes.dll not loaded where every part of it is openRTS's (the КЛУБ's
    # cabs on openRTS's brakes; docs/brakes-dll-replacement.md "The rest", 7).
    add_test(NAME openrts.content.klub_ed9m_no_brakes_dll COMMAND openrts_train_check "${OPENRTS_TEST_ROOT}"
        --dll mdd_ED9M-0060 mdd_ED9M-0060_G mdd_ED9M-0060_M mdd_ED9M-0060_P mdd_ED9M-0060_M mdd_ED9M-0060_MR mdd_ED9M-0060_GR
        --script "${CMAKE_CURRENT_SOURCE_DIR}/data/golden/ed9m_klub.script" --klub openrts --brakes openrts --brakes-dll none)
    add_test(NAME openrts.content.klub_ep20_no_brakes_dll COMMAND openrts_train_check "${OPENRTS_TEST_ROOT}"
        --dll mdd_EP20-001 mdd_EP20-001 --start on --seconds 66
        --script "${CMAKE_CURRENT_SOURCE_DIR}/data/golden/klub_ep20.script" --klub openrts --brakes openrts --brakes-dll none)
    # The speed recorder and ALSN vigilance of a cab without the КЛУБ
    # (SL_SPEED1, the ВЛ10): beside Brakes.dll's, and working the train.
    add_test(NAME openrts.content.recorder_vl10 COMMAND openrts_train_check "${OPENRTS_TEST_ROOT}"
        --dll mdd_vl10-127 mdd_VL10-127A mdd_VL10-127B --start on --seconds 680
        --script "${CMAKE_CURRENT_SOURCE_DIR}/data/golden/vl10_recorder.script" --klub shadow)
    add_test(NAME openrts.content.recorder_vl10_own COMMAND openrts_train_check "${OPENRTS_TEST_ROOT}"
        --dll mdd_vl10-127 mdd_VL10-127A mdd_VL10-127B --start on --seconds 680
        --script "${CMAKE_CURRENT_SOURCE_DIR}/data/golden/vl10_recorder.script" --klub openrts --brakes openrts)
    # САУТ (PB_SETUP 1) in openRTS's КЛУБ: beside Brakes.dll's, and working the train.
    add_test(NAME openrts.content.klub_ed9m_saut COMMAND openrts_train_check "${OPENRTS_TEST_ROOT}"
        --dll mdd_ED9M-0060 mdd_ED9M-0060_G mdd_ED9M-0060_M mdd_ED9M-0060_P mdd_ED9M-0060_M mdd_ED9M-0060_MR mdd_ED9M-0060_GR
        --script "${CMAKE_CURRENT_SOURCE_DIR}/data/golden/ed9m_saut.script" --seconds 600 --klub shadow)
    add_test(NAME openrts.content.klub_ed9m_saut_own COMMAND openrts_train_check "${OPENRTS_TEST_ROOT}"
        --dll mdd_ED9M-0060 mdd_ED9M-0060_G mdd_ED9M-0060_M mdd_ED9M-0060_P mdd_ED9M-0060_M mdd_ED9M-0060_MR mdd_ED9M-0060_GR
        --script "${CMAKE_CURRENT_SOURCE_DIR}/data/golden/ed9m_saut.script" --seconds 600 --klub openrts --brakes openrts)
    add_test(NAME openrts.content.klub_ed9m_own COMMAND openrts_train_check "${OPENRTS_TEST_ROOT}"
        --dll mdd_ED9M-0060 mdd_ED9M-0060_G mdd_ED9M-0060_M mdd_ED9M-0060_P mdd_ED9M-0060_M mdd_ED9M-0060_MR mdd_ED9M-0060_GR
        --script "${CMAKE_CURRENT_SOURCE_DIR}/data/golden/ed9m_klub.script" --klub openrts --brakes openrts)
    add_test(NAME openrts.content.klub_ed9m_modes_own COMMAND openrts_train_check "${OPENRTS_TEST_ROOT}"
        --dll mdd_ED9M-0060 mdd_ED9M-0060_G mdd_ED9M-0060_M mdd_ED9M-0060_P mdd_ED9M-0060_M mdd_ED9M-0060_MR mdd_ED9M-0060_GR
        --script "${CMAKE_CURRENT_SOURCE_DIR}/data/golden/ed9m_klub_modes.script" --klub openrts --brakes openrts)
    # The start state: the ED9M started cold.
    add_test(NAME openrts.content.dll_ed9m_cold COMMAND openrts_train_check "${OPENRTS_TEST_ROOT}"
        --dll mdd_ED9M-0062 mdd_ED9M-0062_G mdd_ED9M-0062_GR --start cold
        --script "${CMAKE_CURRENT_SOURCE_DIR}/data/ed9m_cold.script" --seconds 2)
endif()

# The installed routes' electrification: Pavlovsk AC and DC, Kuban AC,
# Chernyshevsk DC.
if(OPENRTS_TEST_ROOT)
    add_test(NAME openrts.content.electrification COMMAND openrts_electrification_test "${OPENRTS_TEST_ROOT}")
endif()

# The engineer on foot in the VL10k's cab: sits, stands, walks through the
# doors, gets off and climbs in.
if(OPENRTS_TEST_ROOT AND TARGET openrts_viewer)
    add_executable(openrts_engineer_test engineer_test.cpp
        ../viewer/src/cab_view.cpp ../viewer/src/cab_screens.cpp ../viewer/src/engineer.cpp ../viewer/src/walk_rules.cpp ../viewer/src/key_layout.cpp ../viewer/src/camera.cpp ../ui/src/text.cpp)
    openrts_configure_target(openrts_engineer_test)
    target_link_libraries(openrts_engineer_test PRIVATE openrts_assets openrts_audio openrts_formats openrts_renderer openrts_trainplugin)
    add_test(NAME openrts.content.engineer_vl10k COMMAND openrts_engineer_test "${OPENRTS_TEST_ROOT}")
endif()

# openRTS's braking sound: the squeal's highs remade, a train braking to a
# stop squeals, its moan ends with the stop, all quiet standing.
add_executable(openrts_brake_squeal_test brake_squeal_test.cpp ../viewer/src/train_extra_sounds.cpp)
openrts_configure_target(openrts_brake_squeal_test)
target_link_libraries(openrts_brake_squeal_test PRIVATE openrts_audio openrts_core)
add_test(NAME openrts.brake_squeal COMMAND openrts_brake_squeal_test)

add_executable(openrts_discord_presence_test discord_presence_test.cpp)
openrts_configure_target(openrts_discord_presence_test)
target_link_libraries(openrts_discord_presence_test PRIVATE openrts_platform)
add_test(NAME openrts.discord_presence COMMAND openrts_discord_presence_test)

add_executable(openrts_log_console_test log_console_test.cpp)
openrts_configure_target(openrts_log_console_test)
openrts_copy_wgpu_runtime(openrts_log_console_test)
target_link_libraries(openrts_log_console_test PRIVATE openrts_viewer_core)
add_test(NAME openrts.log_console COMMAND openrts_log_console_test)

add_executable(openrts_console_place_test console_place_test.cpp)
openrts_configure_target(openrts_console_place_test)
openrts_copy_wgpu_runtime(openrts_console_place_test)
target_link_libraries(openrts_console_place_test PRIVATE openrts_viewer_core)
add_test(NAME openrts.console_place COMMAND openrts_console_place_test)
