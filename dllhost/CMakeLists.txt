# openrts-dllhost32: the 32-bit process that runs legacy RTS locomotive DLLs
# (docs/train-dll.md). A separate project because it must be built for Win32
# while openRTS is x64; the top-level CMakeLists builds it as an external
# project and copies it next to the game.
cmake_minimum_required(VERSION 3.24)
project(openrts_dllhost32 LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "openrts-dllhost32 must be configured for Win32 (-A Win32)")
endif()

set(OPENRTS_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH "openRTS source root")
set(OPENRTS_DLLHOST_OUTPUT "" CACHE PATH "Where the executable goes")

add_executable(openrts-dllhost32
    src/main.cpp
    src/host.cpp
    src/exports.cpp)
target_compile_features(openrts-dllhost32 PRIVATE cxx_std_20)
target_include_directories(openrts-dllhost32 PRIVATE "${OPENRTS_SOURCE_ROOT}/include")
target_compile_definitions(openrts-dllhost32 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
# Static runtime: no x86 Visual C++ Redistributable needed next to the x64 one.
set_property(TARGET openrts-dllhost32 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(MSVC)
    # /EHa: structured exceptions from DLL code unwind through the host.
    target_compile_options(openrts-dllhost32 PRIVATE /W4 /permissive- /utf-8 /EHa)
    # An executable exporting functions: CMake passes .def files only for
    # libraries, so give it to the linker directly.
    target_link_options(openrts-dllhost32 PRIVATE "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/src/exports.def")
    set_property(TARGET openrts-dllhost32 APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/exports.def")
endif()
if(OPENRTS_DLLHOST_OUTPUT)
    set_target_properties(openrts-dllhost32 PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${OPENRTS_DLLHOST_OUTPUT}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${OPENRTS_DLLHOST_OUTPUT}/Debug"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${OPENRTS_DLLHOST_OUTPUT}/Release"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${OPENRTS_DLLHOST_OUTPUT}/RelWithDebInfo"
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${OPENRTS_DLLHOST_OUTPUT}/MinSizeRel")
endif()

# A locomotive DLL with the RTS ABI for testing the host without RTS content
# (tests/dllhost_test.cpp).
add_library(openrts_fake_loco SHARED test/fake_loco.cpp test/fake_loco.def)
target_compile_features(openrts_fake_loco PRIVATE cxx_std_20)
target_compile_definitions(openrts_fake_loco PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
set_property(TARGET openrts_fake_loco PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(MSVC)
    target_compile_options(openrts_fake_loco PRIVATE /W4 /permissive- /utf-8)
endif()
if(OPENRTS_DLLHOST_OUTPUT)
    set_target_properties(openrts_fake_loco PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${OPENRTS_DLLHOST_OUTPUT}/Debug"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${OPENRTS_DLLHOST_OUTPUT}/Release"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${OPENRTS_DLLHOST_OUTPUT}/RelWithDebInfo"
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${OPENRTS_DLLHOST_OUTPUT}/MinSizeRel")
endif()
