cmake_minimum_required(VERSION 3.24)

project(openRTS
    VERSION 0.1.0
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

function(openrts_configure_target target)
    target_compile_features(${target} PUBLIC cxx_std_20)
    target_include_directories(${target} PUBLIC "${PROJECT_SOURCE_DIR}/include")

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

add_subdirectory(core)
add_subdirectory(formats)
add_subdirectory(assets)
add_subdirectory(renderer)
add_subdirectory(platform)
add_subdirectory(simulation)
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
