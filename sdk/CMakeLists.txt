# The openRTS train plugin SDK: the C ABI (include/openrts/plugin/train_plugin.h),
# its C++ layer (train_plugin.hpp) and samples. A plugin needs nothing else
# from openRTS.
add_library(openrts_fake_native SHARED samples/fake_native/fake_native.cpp)
target_compile_features(openrts_fake_native PRIVATE cxx_std_20)
target_include_directories(openrts_fake_native PRIVATE "${PROJECT_SOURCE_DIR}/include")
if(MSVC)
    target_compile_options(openrts_fake_native PRIVATE /W4 /permissive- /utf-8)
endif()
