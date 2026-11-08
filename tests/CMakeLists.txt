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

add_executable(openrts_frustum_test frustum_test.cpp)
openrts_configure_target(openrts_frustum_test)
openrts_copy_wgpu_runtime(openrts_frustum_test)
target_link_libraries(openrts_frustum_test PRIVATE openrts_renderer)
add_test(NAME openrts.frustum COMMAND openrts_frustum_test)

add_executable(openrts_vfs_test vfs_test.cpp)
openrts_configure_target(openrts_vfs_test)
target_link_libraries(openrts_vfs_test PRIVATE openrts_core)
add_test(NAME openrts.vfs COMMAND openrts_vfs_test)

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

add_executable(openrts_ui_text_test ui_text_test.cpp ../viewer/src/ui_text.cpp)
openrts_configure_target(openrts_ui_text_test)
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
        ../viewer/src/cab_view.cpp ../viewer/src/cab_screens.cpp ../viewer/src/engineer.cpp ../viewer/src/key_layout.cpp ../viewer/src/camera.cpp ../viewer/src/ui_text.cpp)
    openrts_configure_target(openrts_engineer_test)
    target_link_libraries(openrts_engineer_test PRIVATE openrts_assets openrts_audio openrts_formats openrts_renderer openrts_trainplugin)
    add_test(NAME openrts.content.engineer_vl10k COMMAND openrts_engineer_test "${OPENRTS_TEST_ROOT}")
endif()
