add_library(openrts_assets STATIC
    src/assets.cpp
    src/content_resolver.cpp)
openrts_configure_target(openrts_assets)
target_link_libraries(openrts_assets PUBLIC openrts_core openrts_formats)
