#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace openrts::formats {

struct ShapeImageTable {
    std::vector<std::filesystem::path> filenames;
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

// CPU representation used by the first shape-viewer milestone.
struct ShapeMeshVertex {
    float position[3]{};
    float uv[2]{};
    float normal[3]{};
    // Raw 0xAARRGGBB values from the vertex record: colour1 is the diffuse
    // (ambient) colour, colour2 the emissive colour used by vertex-colour lighting.
    std::uint32_t colour1{};
    std::uint32_t colour2{};
};

struct ShapeMesh {
    std::vector<ShapeMeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return error.empty() && !vertices.empty() && !indices.empty(); }
};

// Renderable contents of the nearest LOD.  The parser deliberately retains
// the source state indices: a viewer can apply the original transform and
// texture selection to every primitive list without guessing a material.
struct ShapeMatrix {
    // MSTS affine matrix storage: three basis rows followed by translation.
    float values[12]{};
    std::string name; // node name, e.g. POINT_LEFT, WHEELS1
};

// Rigid node animation (not skinning): keys replace the rotation and/or the
// translation of one matrix. See docs/reverse/shape-animation.md.
struct ShapePositionKey {
    std::uint32_t frame{};
    float value[3]{};
};

struct ShapeRotationKey {
    std::uint32_t frame{};
    float value[4]{}; // quaternion x y z w as stored
};

struct ShapeAnimationNode {
    std::string name;
    std::int32_t matrix_index{-1}; // matrix with the same name (case-insensitive); -1 if none
    std::vector<ShapePositionKey> positions;
    std::vector<ShapeRotationKey> rotations;
};

struct ShapeAnimation {
    std::uint32_t frame_count{};
    std::uint32_t frame_rate{};
    std::vector<ShapeAnimationNode> nodes; // only nodes with keys
};

struct ShapeTextureState {
    std::uint32_t image_index{};
    // Index into ShapeModel::filter_names.
    std::uint32_t filter_mode_index{};
    float mipmap_lod_bias{};
};

struct ShapeVertexState {
    std::uint32_t flags{};
    std::int32_t matrix_index{};
    // MSTS light material: -5 normal, -6/-7 shiny, -8 full bright, -9 circumference,
    // -11 half bright, -12 dark shade, -1 vertex colour (emissive).
    std::int32_t light_material_index{-5};
    std::uint32_t light_config_index{};
};

struct ShapePrimitiveState {
    std::uint32_t flags{};
    std::uint32_t texture_index{};
    std::uint32_t texture_count{};
    std::uint32_t shader_index{};
    std::int32_t vertex_state_index{-1};
    float z_bias{};
    std::uint32_t alpha_test_mode{};
    std::uint32_t z_buffer_mode{1};
};

struct ShapeDrawRange {
    std::uint32_t first_index{};
    std::uint32_t index_count{};
    std::uint32_t primitive_state_index{};
    std::int32_t matrix_index{-1};
};

struct ShapeModel {
    std::vector<ShapeMeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<ShapeMatrix> matrices;
    // Parent matrix for each local matrix in the selected distance level;
    // -1 denotes a root transform.
    std::vector<std::int32_t> hierarchy;
    std::vector<ShapeTextureState> textures;
    std::vector<std::string> shader_names;
    std::vector<std::string> filter_names;
    std::vector<ShapeVertexState> vertex_states;
    std::vector<ShapePrimitiveState> primitive_states;
    std::vector<std::filesystem::path> images;
    std::vector<ShapeDrawRange> draws;
    std::vector<ShapeAnimation> animations;
    std::string error;

    [[nodiscard]] bool ok() const noexcept {
        return error.empty() && !vertices.empty() && !indices.empty() && !draws.empty();
    }
};

// Reads only the canonical binary shape root's images table. `body` begins
// with JINX0s1b________ (the output of narrow shape inflation). Unknown root
// siblings are skipped using their validated bounded block lengths.
[[nodiscard]] ShapeImageTable extract_image_filenames_from_binary_shape(std::string_view body);

// Extracts the first directly renderable indexed triangle list when the shape
// uses the canonical MSTS binary layout.
[[nodiscard]] ShapeMesh extract_first_mesh_from_binary_shape(std::string_view body);

// Parses every subobject from the first distance level, retaining shape
// matrices and primitive state texture/vertex-state relationships.
[[nodiscard]] ShapeModel parse_binary_shape_model(std::string_view body);

} // namespace openrts::formats
