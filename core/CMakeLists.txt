add_library(openrts_core STATIC
    src/core.cpp
    src/directory_watcher.cpp
    src/installation.cpp
    src/vfs.cpp
    src/virtual_path.cpp
    src/zip_archive.cpp)
openrts_configure_target(openrts_core)
# The version (core::version()) and the build's label (core::build_label():
# CI gives "nightly <commit>" for a push to master, nothing for a version
# tag): only core.cpp is built again when they change.
set(OPENRTS_BUILD_LABEL "" CACHE STRING "Shown after the version in the windows' titles, e.g. \"nightly f1a6b90\" (CI)")
set_source_files_properties(src/core.cpp PROPERTIES COMPILE_DEFINITIONS
    "OPENRTS_VERSION=\"${PROJECT_VERSION}\";OPENRTS_BUILD_LABEL=\"${OPENRTS_BUILD_LABEL}\"")
target_link_libraries(openrts_core PUBLIC openrts_profile PRIVATE ZLIB::ZLIB ${OPENRTS_ZSTD_TARGET})
if(OPENRTS_ZSTD_INCLUDE_DIR)
    target_include_directories(openrts_core SYSTEM PRIVATE "${OPENRTS_ZSTD_INCLUDE_DIR}")
endif()
