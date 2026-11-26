cmake_minimum_required(VERSION 3.24)

project(openRTS
    VERSION 0.48.1
    DESCRIPTION "Modular RTS/MSTS-compatible train simulator"
    LANGUAGES C CXX)
if(WIN32)
    # The programs' icon (resources/openrts.rc).
    enable_language(RC)
endif()

include(CTest)
include(FetchContent)

option(OPENRTS_BUILD_VIEWER "Build the asset viewer" ON)
option(OPENRTS_BUILD_GAME "Build the game host skeleton" ON)
option(OPENRTS_BUILD_TOOLS "Build command-line tools" ON)
option(OPENRTS_ENABLE_GLFW "Build the GLFW-based viewer window" ON)
option(OPENRTS_ENABLE_WGPU "Enable the wgpu-native WebGPU backend" OFF)
set(OPENRTS_WGPU_ROOT "" CACHE PATH "Root of an extracted wgpu-native SDK")
# Microsoft's DirectX Shader Compiler, a release from its GitHub (bin/x64:
# dxcompiler.dll, dxil.dll): beside the programs, Direct3D 12 compiles the
# shaders with it (seconds) instead of FXC (tens of seconds to minutes).
# Optional; docs/shaders.md.
set(OPENRTS_DXC_ROOT "" CACHE PATH "Root of an extracted DirectX Shader Compiler release (optional)")
option(OPENRTS_FETCH_ZLIB "Fetch zlib when a system package is unavailable" ON)
option(OPENRTS_FETCH_ZSTD "Fetch Zstandard when a system package is unavailable" ON)
option(OPENRTS_ENABLE_TRACY "Build the Tracy profiler client in (docs/profiling.md)" OFF)

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

# MikkTSpace (zlib): the tangents normal maps are baked with (Blender,
# Substance Painter, glTF), for the models with an openRTS normal map
# (PBR stage 4, docs/pbr-materials.md).
FetchContent_Declare(mikktspace
    URL https://github.com/mmikk/MikkTSpace/archive/3e895b49d05ea07e4c2133156cfa94369e19e409.tar.gz
    URL_HASH SHA256=aeba65ddee85a679133d510d71d00b108f603752f90b15b9ecf7777033f6e351
    SOURCE_SUBDIR openrts-sources-only)
FetchContent_MakeAvailable(mikktspace)
add_library(openrts_mikktspace STATIC "${mikktspace_SOURCE_DIR}/mikktspace.c")
target_include_directories(openrts_mikktspace SYSTEM PUBLIC "${mikktspace_SOURCE_DIR}")
if(MSVC)
    target_compile_options(openrts_mikktspace PRIVATE /W0)
else()
    target_compile_options(openrts_mikktspace PRIVATE -w)
endif()

# Luau (MIT): the scripting language of openRTS's own modules (a brake
# distributor now; train plugins later, docs/scripting.md). Its VM and
# compiler only.
set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_WEB OFF CACHE BOOL "" FORCE)
FetchContent_Declare(luau
    GIT_REPOSITORY https://github.com/luau-lang/luau.git
    GIT_TAG 0.740
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(luau)
# Only what openrts_script links is built.
foreach(unused Luau.Analysis Luau.CodeGen Luau.Config Luau.Require Luau.CLI.lib Luau.Inliner isocline osthreads)
    if(TARGET ${unused})
        set_target_properties(${unused} PROPERTIES EXCLUDE_FROM_ALL TRUE)
    endif()
endforeach()

function(openrts_configure_target target)
    target_compile_features(${target} PUBLIC cxx_std_20)
    target_include_directories(${target} PUBLIC "${PROJECT_SOURCE_DIR}/include")

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

# The openRTS icon in a program (resources/openrts.ico): Explorer's, and its
# windows' through GLFW.
function(openrts_add_icon target)
    if(WIN32)
        target_sources(${target} PRIVATE "${PROJECT_SOURCE_DIR}/resources/openrts.rc")
    endif()
endfunction()

# openrts-dllhost32.exe: legacy RTS locomotive DLLs are 32-bit, so they run
# in a separate Win32 process (docs/train-dll.md). Built as an external
# project for the Win32 platform and copied next to the programs that use it.
# With a Visual Studio generator it is the same one; with another (Ninja, as
# CI builds for its compiler cache) the newest Visual Studio's, or
# OPENRTS_DLLHOST_GENERATOR. Its output keeps the configuration's folder
# (<bin32>/<config>/) either way.
set(OPENRTS_DLLHOST_GENERATOR "" CACHE STRING "Visual Studio generator for openrts-dllhost32 when the build's is not one (empty: the newest installed)")
set(openrts_dllhost_generator "")
if(WIN32 AND MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    if(CMAKE_GENERATOR MATCHES "Visual Studio")
        set(openrts_dllhost_generator "${CMAKE_GENERATOR}")
    elseif(OPENRTS_DLLHOST_GENERATOR)
        set(openrts_dllhost_generator "${OPENRTS_DLLHOST_GENERATOR}")
    else()
        set(vswhere "$ENV{ProgramFiles\(x86\)}/Microsoft Visual Studio/Installer/vswhere.exe")
        if(EXISTS "${vswhere}")
            execute_process(COMMAND "${vswhere}" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64
                                    -property installationVersion
                            OUTPUT_VARIABLE vs_version OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
            if(vs_version MATCHES "^18\\.")
                set(openrts_dllhost_generator "Visual Studio 18 2026")
            elseif(vs_version MATCHES "^17\\.")
                set(openrts_dllhost_generator "Visual Studio 17 2022")
            endif()
        endif()
        if(NOT openrts_dllhost_generator)
            message(WARNING "openrts-dllhost32: no Visual Studio generator found (OPENRTS_DLLHOST_GENERATOR); the 32-bit DLL host is not built")
        endif()
    endif()
endif()
if(openrts_dllhost_generator)
    include(ExternalProject)
    set(OPENRTS_DLLHOST_DIR "${CMAKE_BINARY_DIR}/bin32")
    ExternalProject_Add(openrts_dllhost32
        SOURCE_DIR "${PROJECT_SOURCE_DIR}/dllhost"
        BINARY_DIR "${CMAKE_BINARY_DIR}/dllhost32"
        CMAKE_GENERATOR "${openrts_dllhost_generator}"
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

# The Tracy profiler's client, optional (docs/profiling.md): the zones of
# include/openrts/core/profile.hpp. Off, they compile to nothing. On demand
# (nothing kept until the Tracy program connects) and on localhost only.
add_library(openrts_profile INTERFACE)
if(OPENRTS_ENABLE_TRACY)
    FetchContent_Declare(tracy
        GIT_REPOSITORY https://github.com/wolfpld/tracy.git
        GIT_TAG v0.14.1
        GIT_SHALLOW TRUE)
    set(TRACY_ENABLE ON CACHE BOOL "" FORCE)
    set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE)
    set(TRACY_ONLY_LOCALHOST ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(tracy)
    # Its headers as system ones: our warnings are not theirs.
    get_target_property(OPENRTS_TRACY_INCLUDES TracyClient INTERFACE_INCLUDE_DIRECTORIES)
    set_target_properties(TracyClient PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${OPENRTS_TRACY_INCLUDES}")
    target_link_libraries(openrts_profile INTERFACE TracyClient)
    target_compile_definitions(openrts_profile INTERFACE OPENRTS_TRACY=1)
    message(STATUS "openRTS: Tracy profiler client v0.14.1 built in (connect Tracy 0.14.1)")
endif()

add_subdirectory(core)
add_subdirectory(script)
add_subdirectory(formats)
add_subdirectory(assets)
add_subdirectory(renderer)
add_subdirectory(platform)
add_subdirectory(simulation)
add_subdirectory(trainplugin)
add_subdirectory(sdk)
add_subdirectory(audio)
add_subdirectory(compat)
add_subdirectory(net)
add_subdirectory(server)

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
        add_subdirectory(ui)
        add_subdirectory(trackmap)
        # The server's window (server/src/server_window.cpp: its start form and
        # its map console); without it the server runs at the console. The
        # WebGPU DLL is loaded only when the window opens (--no-window runs
        # without it).
        if(TARGET openrts_server_app)
            target_sources(openrts_server_app PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/server/src/server_window.cpp")
            target_link_libraries(openrts_server_app PRIVATE openrts_imgui openrts_ui openrts_trackmap openrts_renderer)
            target_compile_definitions(openrts_server_app PRIVATE OPENRTS_SERVER_WINDOW=1)
            if(MSVC)
                target_link_options(openrts_server_app PRIVATE /DELAYLOAD:wgpu_native.dll)
                target_link_libraries(openrts_server_app PRIVATE delayimp)
            endif()
        endif()
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

# openRTS's own packs (docs/vfs.md, "openRTS's own packs"): packs/thunder,
# packs/stock (the stock rules in Luau, docs/scripting.md), packs/snow (the
# bodies' snow masks in MATERIALS, docs/pbr-materials.md), packs/rails (the
# track's PBR materials in MATERIALS) and packs/cargo (openRTS's own cargo
# kinds and their textures, docs/cargo.md)
# packed by openrts-pack into PACKS beside the game and the viewer, on every
# build (a second).
if(OPENRTS_BUILD_TOOLS AND TARGET openrts_pack)
    set(openrts_pack_targets "")
    foreach(program openrts_game openrts_viewer)
        if(TARGET ${program})
            list(APPEND openrts_pack_targets ${program})
        endif()
    endforeach()
    set(openrts_pack_commands "")
    foreach(program ${openrts_pack_targets})
        list(APPEND openrts_pack_commands
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${program}>/PACKS"
            COMMAND $<TARGET_FILE:openrts_pack> pack "${CMAKE_SOURCE_DIR}/packs/thunder" --as SOUND/OPENRTS/THUNDER
                    -o "$<TARGET_FILE_DIR:${program}>/PACKS/openrts-thunder.zip" --method deflate --name openrts-thunder
                    --kind global
            COMMAND $<TARGET_FILE:openrts_pack> pack "${CMAKE_SOURCE_DIR}/packs/stock" --as TRAINS/OPENRTS/STOCK
                    -o "$<TARGET_FILE_DIR:${program}>/PACKS/openrts-stock.zip" --method deflate --name openrts-stock
                    --kind trains
            COMMAND $<TARGET_FILE:openrts_pack> pack "${CMAKE_SOURCE_DIR}/packs/snow" --as MATERIALS
                    -o "$<TARGET_FILE_DIR:${program}>/PACKS/openrts-snow.zip" --method deflate --name openrts-snow
                    --kind trains
            COMMAND $<TARGET_FILE:openrts_pack> pack "${CMAKE_SOURCE_DIR}/packs/rails" --as MATERIALS
                    -o "$<TARGET_FILE_DIR:${program}>/PACKS/openrts-rails.zip" --method deflate --name openrts-rails
                    --kind global
            COMMAND $<TARGET_FILE:openrts_pack> pack "${CMAKE_SOURCE_DIR}/packs/cargo"
                    -o "$<TARGET_FILE_DIR:${program}>/PACKS/openrts-cargo.zip" --method deflate --name openrts-cargo
                    --kind global)
    endforeach()
    if(openrts_pack_commands)
        add_custom_target(openrts_own_packs ALL ${openrts_pack_commands} VERBATIM)
        add_dependencies(openrts_own_packs openrts_pack ${openrts_pack_targets})
    endif()
endif()
