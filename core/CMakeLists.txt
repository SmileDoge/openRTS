add_library(openrts_core STATIC
    src/core.cpp
    src/directory_watcher.cpp
    src/installation.cpp
    src/vfs.cpp
    src/virtual_path.cpp
    src/zip_archive.cpp)
openrts_configure_target(openrts_core)
target_link_libraries(openrts_core PUBLIC openrts_profile PRIVATE ZLIB::ZLIB ${OPENRTS_ZSTD_TARGET})
if(OPENRTS_ZSTD_INCLUDE_DIR)
    target_include_directories(openrts_core SYSTEM PRIVATE "${OPENRTS_ZSTD_INCLUDE_DIR}")
endif()
