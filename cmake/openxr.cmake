# The OpenXR loader (prebuilt, the Khronos SDK's release) as
# openrts_openxr_loader, Windows x64; and OPENRTS_HAS_XR: the wgpu-native
# in use is openRTS's (third_party/wgpu-native: its D3D11 shared-texture
# import), so the game can show its frames in a headset (docs/vr.md).
if(TARGET openrts_openxr_loader)
    return()
endif()
include(FetchContent)
FetchContent_Declare(openxr_loader
    URL https://github.com/KhronosGroup/OpenXR-SDK-Source/releases/download/release-1.1.43/openxr_loader_windows-1.1.43.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(openxr_loader)
add_library(openrts_openxr_loader SHARED IMPORTED GLOBAL)
set_target_properties(openrts_openxr_loader PROPERTIES
    IMPORTED_IMPLIB "${openxr_loader_SOURCE_DIR}/x64/lib/openxr_loader.lib"
    IMPORTED_LOCATION "${openxr_loader_SOURCE_DIR}/x64/bin/openxr_loader.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${openxr_loader_SOURCE_DIR}/include")
set(OPENRTS_OPENXR_LOADER_DLL "${openxr_loader_SOURCE_DIR}/x64/bin/openxr_loader.dll" CACHE INTERNAL "")

set(OPENRTS_HAS_XR OFF CACHE INTERNAL "")
if(OPENRTS_WGPU_INCLUDE_DIR AND EXISTS "${OPENRTS_WGPU_INCLUDE_DIR}/webgpu/wgpu.h")
    file(STRINGS "${OPENRTS_WGPU_INCLUDE_DIR}/webgpu/wgpu.h" xr_import REGEX "wgpuDeviceImportD3D11SharedTexture")
    if(xr_import)
        set(OPENRTS_HAS_XR ON CACHE INTERNAL "")
    endif()
endif()
message(STATUS "openRTS VR (OpenXR): ${OPENRTS_HAS_XR}")
