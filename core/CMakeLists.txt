add_library(openrts_core STATIC
    src/core.cpp
    src/directory_watcher.cpp
    src/installation.cpp
    src/vfs.cpp
    src/virtual_path.cpp
    src/zip_archive.cpp)
openrts_configure_target(openrts_core)
# The version (core::version()): only core.cpp is built again when it changes.
set_source_files_properties(src/core.cpp PROPERTIES COMPILE_DEFINITIONS "OPENRTS_VERSION=\"${PROJECT_VERSION}\"")
target_link_libraries(openrts_core PUBLIC openrts_profile PRIVATE ZLIB::ZLIB ${OPENRTS_ZSTD_TARGET})
if(OPENRTS_ZSTD_INCLUDE_DIR)
    target_include_directories(openrts_core SYSTEM PRIVATE "${OPENRTS_ZSTD_INCLUDE_DIR}")
endif()
