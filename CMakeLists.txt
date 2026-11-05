cmake_minimum_required(VERSION 3.24)

project(openRTS
    VERSION 0.3.1
    DESCRIPTION "Modular RTS/MSTS-compatible train simulator"
    LANGUAGES C CXX)

include(CTest)
include(FetchContent)

option(OPENRTS_BUILD_VIEWER "Build the asset viewer" ON)
option(OPENRTS_BUILD_GAME "Build the game host skeleton" ON)
option(OPENRTS_BUILD_TOOLS "Build command-line tools" ON)
option(OPENRTS_ENABLE_GLFW "Build the GLFW-based viewer window" ON)
option(OPENRTS_ENABLE_WGPU "Enable the wgpu-native WebGPU backend" OFF)
set(OPENRTS_WGPU_ROOT "" CACHE PATH "Root of an extracted wgpu-native SDK")
option(OPENRTS_FETCH_ZLIB "Fetch zlib when a system package is unavailable" ON)
option(OPENRTS_FETCH_ZSTD "Fetch Zstandard when a system package is unavailable" ON)

find_package(ZLIB QUIET)
if(NOT ZLIB_FOUND)
    if(NOT OPENRTS_FETCH_ZLIB)
        message(FATAL_ERROR "ZLIB is required for compressed MSTS/RTS assets. Install it or enable OPENRTS_FETCH_ZLIB.")
    endif()

    FetchContent_Declare(zlib
        GIT_REPOSITORY https://github.com/madler/zlib.git
        GIT_TAG v1.3.1
        GIT_SHALLOW TRUE)
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "Disable zlib example targets in openRTS builds" FORCE)
    FetchContent_MakeAvailable(zlib)
    add_library(ZLIB::ZLIB ALIAS zlibstatic)
endif()

# Zstandard: ZIP method 93 in content packs (docs/vfs.md).
find_package(zstd CONFIG QUIET)
if(TARGET zstd::libzstd_static)
    set(OPENRTS_ZSTD_TARGET zstd::libzstd_static)
elseif(TARGET zstd::libzstd_shared)
    set(OPENRTS_ZSTD_TARGET zstd::libzstd_shared)
else()
    if(NOT OPENRTS_FETCH_ZSTD)
        message(FATAL_ERROR "Zstandard is required for content packs. Install it or enable OPENRTS_FETCH_ZSTD.")
    endif()
    FetchContent_Declare(zstd
        GIT_REPOSITORY https://github.com/facebook/zstd.git
        GIT_TAG v1.5.6
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR build/cmake)
    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
    set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(zstd)
    set(OPENRTS_ZSTD_TARGET libzstd_static)
    set(OPENRTS_ZSTD_INCLUDE_DIR "${zstd_SOURCE_DIR}/lib")
endif()

FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.1
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(glm)

# miniaudio (public domain / MIT-0): sound output and decoding. Only its
# sources are used; SOURCE_SUBDIR keeps its own CMake project out.
FetchContent_Declare(miniaudio
    URL https://github.com/mackron/miniaudio/archive/refs/tags/0.11.22.tar.gz
    URL_HASH SHA256=bcb07bfb27e6fa94d34da73ba2d5642d4940b208ec2a660dbf4e52e6b7cd492f
    SOURCE_SUBDIR openrts-sources-only)
FetchContent_MakeAvailable(miniaudio)

function(openrts_configure_target target)
    target_compile_features(${target} PUBLIC cxx_std_20)
    target_include_directories(${target} PUBLIC "${PROJECT_SOURCE_DIR}/include")

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

# openrts-dllhost32.exe: legacy RTS locomotive DLLs are 32-bit, so they run
# in a separate Win32 process (docs/train-dll.md). Built as an external
# project for the Win32 platform and copied next to the programs that use it.
if(WIN32 AND MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_GENERATOR MATCHES "Visual Studio")
    include(ExternalProject)
    set(OPENRTS_DLLHOST_DIR "${CMAKE_BINARY_DIR}/bin32")
    ExternalProject_Add(openrts_dllhost32
        SOURCE_DIR "${PROJECT_SOURCE_DIR}/dllhost"
        BINARY_DIR "${CMAKE_BINARY_DIR}/dllhost32"
        CMAKE_GENERATOR "${CMAKE_GENERATOR}"
        CMAKE_GENERATOR_PLATFORM Win32
        CMAKE_ARGS "-DOPENRTS_DLLHOST_OUTPUT=${OPENRTS_DLLHOST_DIR}" "-DOPENRTS_SOURCE_ROOT=${PROJECT_SOURCE_DIR}"
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config $<CONFIG>
        INSTALL_COMMAND ""
        BUILD_ALWAYS TRUE)
    function(openrts_copy_dllhost target)
        add_dependencies(${target} openrts_dllhost32)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${OPENRTS_DLLHOST_DIR}/$<CONFIG>/openrts-dllhost32.exe" "$<TARGET_FILE_DIR:${target}>")
    endfunction()
else()
    function(openrts_copy_dllhost target)
    endfunction()
endif()

add_subdirectory(core)
add_subdirectory(formats)
add_subdirectory(assets)
add_subdirectory(renderer)
add_subdirectory(platform)
add_subdirectory(simulation)
add_subdirectory(trainplugin)
add_subdirectory(sdk)
add_subdirectory(audio)
add_subdirectory(compat)

if(OPENRTS_BUILD_VIEWER)
    if(OPENRTS_ENABLE_GLFW)
        FetchContent_Declare(glfw
            GIT_REPOSITORY https://github.com/glfw/glfw.git
            GIT_TAG 3.4
            GIT_SHALLOW TRUE)
        set(GLFW_BUILD_DOCS OFF CACHE BOOL "Disable GLFW docs" FORCE)
        set(GLFW_BUILD_TESTS OFF CACHE BOOL "Disable GLFW tests" FORCE)
        set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "Disable GLFW examples" FORCE)
        set(GLFW_INSTALL OFF CACHE BOOL "Disable GLFW install rules" FORCE)
        FetchContent_MakeAvailable(glfw)
    endif()
    if(OPENRTS_ENABLE_GLFW AND OPENRTS_ENABLE_WGPU)
        FetchContent_Declare(imgui
            GIT_REPOSITORY https://github.com/ocornut/imgui.git
            GIT_TAG v1.91.9b
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(imgui)
        add_library(openrts_imgui STATIC
            "${imgui_SOURCE_DIR}/imgui.cpp"
            "${imgui_SOURCE_DIR}/imgui_draw.cpp"
            "${imgui_SOURCE_DIR}/imgui_tables.cpp"
            "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
            "${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp"
            "${imgui_SOURCE_DIR}/backends/imgui_impl_wgpu.cpp")
        target_compile_features(openrts_imgui PUBLIC cxx_std_20)
        target_include_directories(openrts_imgui SYSTEM PUBLIC
            "${imgui_SOURCE_DIR}"
            "${imgui_SOURCE_DIR}/backends"
            "${OPENRTS_WGPU_INCLUDE_DIR}")
        target_compile_definitions(openrts_imgui PRIVATE IMGUI_IMPL_WEBGPU_BACKEND_WGPU=1)
        target_link_libraries(openrts_imgui PUBLIC glfw openrts_wgpu_native)
    endif()
    add_subdirectory(viewer)
endif()

if(OPENRTS_BUILD_GAME)
    add_subdirectory(game)
endif()

if(OPENRTS_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
