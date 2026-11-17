# openrts-shape: MSTS / RTS shapes for artists (PBR stage 4): a shape's
# nearest level as glTF 2.0 with its textures as PNG (docs/pbr-materials.md).
add_executable(openrts_shape src/main.cpp)
set_target_properties(openrts_shape PROPERTIES OUTPUT_NAME openrts-shape)
openrts_configure_target(openrts_shape)
target_link_libraries(openrts_shape PRIVATE openrts_formats openrts_core)
