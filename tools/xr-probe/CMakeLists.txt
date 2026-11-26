# openrts-xr-probe: the OpenXR prototype (docs/vr.md): a wgpu scene in a
# headset through OpenXR (D3D11 binding), Windows only. The OpenXR loader
# comes prebuilt from the Khronos SDK's release.
#
# OPENRTS_XR_WGPU_ROOT: openRTS's wgpu-native fork (docs/vr.md, "The shared
# texture"): the frame handed to OpenXR on the GPU. Empty: the official
# wgpu-native, the frame through the CPU.
set(OPENRTS_XR_WGPU_ROOT "" CACHE PATH "openRTS's wgpu-native fork (its package: include/, lib/) for the XR probe's shared texture")

include("${PROJECT_SOURCE_DIR}/cmake/openxr.cmake")

add_executable(openrts_xr_probe src/main.cpp)
set_target_properties(openrts_xr_probe PROPERTIES OUTPUT_NAME openrts-xr-probe)
openrts_configure_target(openrts_xr_probe)
target_link_libraries(openrts_xr_probe PRIVATE openrts_openxr_loader glm::glm d3d11 dxgi)

if(OPENRTS_XR_WGPU_ROOT)
    add_library(openrts_xr_wgpu SHARED IMPORTED)
    set_target_properties(openrts_xr_wgpu PROPERTIES
        IMPORTED_IMPLIB "${OPENRTS_XR_WGPU_ROOT}/lib/wgpu_native.dll.lib"
        IMPORTED_LOCATION "${OPENRTS_XR_WGPU_ROOT}/lib/wgpu_native.dll"
        INTERFACE_INCLUDE_DIRECTORIES "${OPENRTS_XR_WGPU_ROOT}/include")
    target_link_libraries(openrts_xr_probe PRIVATE openrts_xr_wgpu)
    target_compile_definitions(openrts_xr_probe PRIVATE OPENRTS_XR_SHARED=1)
    set(xr_wgpu_dll "${OPENRTS_XR_WGPU_ROOT}/lib/wgpu_native.dll")
else()
    target_link_libraries(openrts_xr_probe PRIVATE openrts_wgpu_native)
    target_include_directories(openrts_xr_probe PRIVATE "${OPENRTS_WGPU_INCLUDE_DIR}")
    set(xr_wgpu_dll "$<TARGET_FILE:openrts_wgpu_native>")
endif()

add_custom_command(TARGET openrts_xr_probe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OPENRTS_OPENXR_LOADER_DLL}" "$<TARGET_FILE_DIR:openrts_xr_probe>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${xr_wgpu_dll}" "$<TARGET_FILE_DIR:openrts_xr_probe>")
