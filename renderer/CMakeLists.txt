add_library(openrts_renderer STATIC src/renderer.cpp)
openrts_configure_target(openrts_renderer)
target_link_libraries(openrts_renderer PUBLIC openrts_core)

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
