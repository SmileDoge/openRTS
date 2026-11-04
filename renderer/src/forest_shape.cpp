#include "forest_shape.hpp"

#include <cmath>
#include <cstdint>
#include <string>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <openrts/formats/route_world.hpp>

#include "terrain_heights.hpp"

namespace openrts::renderer {

std::optional<core::VirtualPath> forest_shape_path(const core::VirtualPath& world_file, const std::uint32_t uid) {
    const auto stem = world_file.with_extension("");
    return world_file.parent().resolve(std::string(stem.filename()) + "." + std::to_string(uid) + ".forest");
}

ForestShape build_forest_shape(const core::Vfs& vfs, const core::VirtualPath& path) {
    ForestShape result;
    const auto name = std::string(path.with_extension("").filename());
    const auto dot = name.rfind('.');
    if (dot == std::string::npos) {
        result.error = "not a forest path";
        return result;
    }
    const auto uid = static_cast<std::uint32_t>(std::strtoul(name.c_str() + dot + 1, nullptr, 10));
    const auto world_folder = path.parent();
    const auto world_file = world_folder.resolve(name.substr(0, dot) + ".w");
    if (!world_file) {
        result.error = "bad world file";
        return result;
    }
    const auto tile = formats::load_world_tile(vfs, *world_file);
    if (!tile.ok()) {
        result.error = tile.error;
        return result;
    }
    const formats::WorldObjectPlacement* object = nullptr;
    for (const auto& candidate : tile.objects) {
        if (candidate.kind == formats::WorldObjectKind::forest && candidate.uid == uid) {
            object = &candidate;
            break;
        }
    }
    if (!object) {
        result.error = "forest " + std::to_string(uid) + " not in " + world_file->str();
        return result;
    }
    if (!object->forest_complete) {
        result.error = "forest without TreeTexture, ScaleRange, Population or TreeSize";
        return result;
    }
    result.texture = object->tree_texture;
    const auto route = world_folder.parent();
    if (const auto textures = route.resolve(std::string_view("TEXTURES"))) result.route_textures = *textures;

    // rts1.exe 0x4B3C40, in its own order and arithmetic: every variable a
    // float, each expression worked out wider (the x87) and stored back.
    const auto count = object->population;
    const float width = std::fabs(object->forest_area[0]);
    const float depth = std::fabs(object->forest_area[1]);
    const float tree_width = std::fabs(object->tree_size[0]);
    const float tree_height = std::fabs(object->tree_size[1]);
    if (tree_width <= 0.0f || count == 0) {
        result.error = "forest of nothing";
        return result;
    }
    const float half_tree = static_cast<float>(static_cast<double>(tree_width) * 0.5);
    const float half_width = static_cast<float>(static_cast<double>(width) * 0.5);
    const float half_depth = static_cast<float>(static_cast<double>(depth) * 0.5);
    float x = static_cast<float>(static_cast<double>(half_width) * 0.5);
    float z = x;
    float step = static_cast<float>((static_cast<double>(width) / tree_width + 1.0) * (static_cast<double>(depth) / tree_width + 1.0));
    step = static_cast<float>(static_cast<double>(step) / static_cast<double>(count) * tree_width);

    const auto& q = object->rotation;
    const glm::quat rotation{q[3], -q[0], -q[1], -q[2]}; // as placement_transform
    TerrainHeights terrain(vfs, route.resolve(std::string_view("TILES")).value_or(route));
    auto& model = result.model;
    model.vertices.reserve(static_cast<std::size_t>(count) * 8);
    model.indices.reserve(static_cast<std::size_t>(count) * 24);
    // UVs by the vertex's place in its quad: bottom near, bottom far, top far, top near.
    constexpr float uvs[4][2] = {{0.0f, 0.99f}, {1.0f, 0.99f}, {1.0f, 0.0f}, {0.0f, 0.0f}};
    for (std::uint32_t i = 0; i < count;) {
        const double quarter = static_cast<double>(tree_width) * 0.25;
        while (static_cast<double>(width) - quarter < x) {
            z = static_cast<float>(static_cast<double>(x) / width * tree_width + z);
            x = static_cast<float>(static_cast<double>(x) - width);
        }
        if (quarter > x) x = static_cast<float>(quarter);
        while (static_cast<double>(depth) - quarter < z) z = static_cast<float>(static_cast<double>(z) - depth);
        if (quarter > z) z = static_cast<float>(static_cast<double>(tree_width) + z);
        const float local_x = static_cast<float>(static_cast<double>(x) - half_width);
        const float local_z = static_cast<float>(static_cast<double>(z) - half_depth);
        const glm::vec3 offset = rotation * glm::vec3{local_x, 0.0f, local_z};
        float ground = 0.0f;
        if (const auto height = terrain.at(tile.tile_x, tile.tile_z, object->position.x + offset.x, object->position.z + offset.z)) {
            ground = *height - static_cast<float>(object->position.y);
        }
        const float lx = local_x;
        const float lz = local_z;
        const float hw = half_tree;
        const float top = ground + tree_height;
        // Two quads: across x (x fixed), then across z (z fixed).
        const float corners[8][3] = {
            {lx, ground, lz + hw}, {lx, ground, lz - hw}, {lx, top, lz - hw}, {lx, top, lz + hw},
            {lx - hw, ground, lz}, {lx + hw, ground, lz}, {lx + hw, top, lz}, {lx - hw, top, lz},
        };
        const auto base = static_cast<std::uint32_t>(model.vertices.size());
        for (std::uint32_t k = 0; k < 8; ++k) {
            formats::ShapeMeshVertex vertex;
            vertex.position[0] = corners[k][0];
            vertex.position[1] = corners[k][1];
            vertex.position[2] = corners[k][2];
            vertex.normal[1] = 1.0f;
            vertex.uv[0] = uvs[k & 3][0];
            vertex.uv[1] = uvs[k & 3][1];
            vertex.colour1 = 0xFFFFFFFFu;
            vertex.colour2 = 0xFF000000u;
            model.vertices.push_back(vertex);
        }
        // Both sides of both quads (0x4B3C40's index list).
        for (const std::uint32_t index : {0u, 1u, 3u, 1u, 2u, 3u, 3u, 1u, 0u, 3u, 2u, 1u,
                                          4u, 5u, 7u, 5u, 6u, 7u, 7u, 5u, 4u, 7u, 6u, 5u}) {
            model.indices.push_back(base + index);
        }
        ++i;
        const double sign = (i & 1u) != 0 ? -1.0 : 1.0;
        x = static_cast<float>(static_cast<double>(i) * step + sign * half_tree + x);
        z = static_cast<float>(sign * half_tree + step + z);
    }

    formats::ShapeMatrix identity;
    identity.values[0] = identity.values[4] = identity.values[8] = 1.0f;
    identity.name = "FOREST";
    model.matrices.push_back(identity);
    model.hierarchy.push_back(-1);
    model.images.emplace_back(result.texture);
    model.filter_names.emplace_back("MipLinear");
    model.textures.push_back({.image_index = 0, .filter_mode_index = 0, .mipmap_lod_bias = -3.0f});
    model.shader_names.emplace_back("BlendATexDiff");
    model.vertex_states.push_back({.flags = 0, .matrix_index = 0, .light_material_index = -9});
    model.primitive_states.push_back({.flags = 0,
                                      .texture_index = 0,
                                      .texture_count = 1,
                                      .shader_index = 0,
                                      .vertex_state_index = 0,
                                      .alpha_test_mode = 1,
                                      .z_buffer_mode = 1});
    model.draws.push_back({.first_index = 0,
                           .index_count = static_cast<std::uint32_t>(model.indices.size()),
                           .primitive_state_index = 0,
                           .matrix_index = 0,
                           .level = 0});
    model.level_distances = {1500.0f};
    return result;
}

} // namespace openrts::renderer
