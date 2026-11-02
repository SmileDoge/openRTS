# Locomotive logic plugins: the legacy RTS DLL bridge (through the x86
# openrts-dllhost32.exe) and native openRTS plugins (train_plugin.h)
# (docs/train-dll.md).
add_library(openrts_trainplugin STATIC
    src/legacy_host.cpp
    src/legacy_consist.cpp
    src/native_plugin.cpp)
openrts_configure_target(openrts_trainplugin)
target_link_libraries(openrts_trainplugin PUBLIC openrts_simulation openrts_formats openrts_core)
target_link_libraries(openrts_trainplugin PRIVATE ${CMAKE_DL_LIBS})
