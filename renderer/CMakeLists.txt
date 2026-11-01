add_library(openrts_renderer STATIC
    src/renderer.cpp
    src/debug_lines.cpp
    src/glow_sprites.cpp
    src/frustum.cpp
    src/model_assets.cpp
    src/model_library.cpp
    src/model_lighting.cpp
    src/model_renderer.cpp
    src/rts_material.cpp
    src/rts_sky.cpp
    src/route_space.cpp
    src/route_streamer.cpp
    src/sky_renderer.cpp
    src/terrain_renderer.cpp)
openrts_configure_target(openrts_renderer)
# Public renderer headers expose formats types (ShapeModel, RoutePosition).
target_link_libraries(openrts_renderer PUBLIC openrts_core openrts_formats glm::glm)
target_link_libraries(openrts_renderer PUBLIC openrts_assets)

if(OPENRTS_ENABLE_WGPU)
    if(NOT OPENRTS_WGPU_ROOT)
        message(FATAL_ERROR "OPENRTS_ENABLE_WGPU requires OPENRTS_WGPU_ROOT to name an extracted wgpu-native SDK.")
    endif()

    find_path(OPENRTS_WGPU_INCLUDE_DIR webgpu/webgpu.h
        PATHS "${OPENRTS_WGPU_ROOT}/include" NO_DEFAULT_PATH REQUIRED)
    if(WIN32)
        find_library(OPENRTS_WGPU_IMPLIB wgpu_native.dll.lib
            PATHS "${OPENRTS_WGPU_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
        find_file(OPENRTS_WGPU_RUNTIME wgpu_native.dll
            PATHS "${OPENRTS_WGPU_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
        add_library(openrts_wgpu_native SHARED IMPORTED GLOBAL)
        set_target_properties(openrts_wgpu_native PROPERTIES
            IMPORTED_IMPLIB "${OPENRTS_WGPU_IMPLIB}"
            IMPORTED_LOCATION "${OPENRTS_WGPU_RUNTIME}")
    else()
        find_library(OPENRTS_WGPU_LIBRARY wgpu_native
            PATHS "${OPENRTS_WGPU_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
        add_library(openrts_wgpu_native SHARED IMPORTED GLOBAL)
        set_target_properties(openrts_wgpu_native PROPERTIES IMPORTED_LOCATION "${OPENRTS_WGPU_LIBRARY}")
    endif()

    target_sources(openrts_renderer PRIVATE src/wgpu_context.cpp src/wgpu_texture.cpp)
    target_include_directories(openrts_renderer PUBLIC "${OPENRTS_WGPU_INCLUDE_DIR}")
    target_compile_definitions(openrts_renderer PUBLIC OPENRTS_HAS_WGPU=1)
    target_link_libraries(openrts_renderer PUBLIC openrts_wgpu_native)
endif()

# Places the wgpu-native runtime next to an executable so it starts without a
# PATH setup; no-op unless the WebGPU backend is enabled on Windows.
function(openrts_copy_wgpu_runtime target)
    if(OPENRTS_ENABLE_WGPU AND WIN32)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:openrts_wgpu_native>" "$<TARGET_FILE_DIR:${target}>")
    endif()
endfunction()
