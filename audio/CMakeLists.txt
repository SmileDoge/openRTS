add_library(openrts_audio STATIC
    src/audio_engine.cpp
    src/interior_acoustics.cpp
    src/miniaudio.cpp
    src/sound_bank.cpp
    src/sound_instance.cpp
    src/world_sounds.cpp)
openrts_configure_target(openrts_audio)
target_link_libraries(openrts_audio PUBLIC openrts_core openrts_formats glm::glm)
target_include_directories(openrts_audio SYSTEM PRIVATE "${miniaudio_SOURCE_DIR}" "${miniaudio_SOURCE_DIR}/extras")
# Third-party implementation file: no project warnings.
if(MSVC)
    set_source_files_properties(src/miniaudio.cpp PROPERTIES COMPILE_OPTIONS "/W0")
else()
    set_source_files_properties(src/miniaudio.cpp PROPERTIES COMPILE_OPTIONS "-w")
endif()
