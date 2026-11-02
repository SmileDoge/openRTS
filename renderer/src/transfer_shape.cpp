#include "transfer_shape.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <openrts/formats/route_world.hpp>
#include <openrts/formats/terrain.hpp>

namespace openrts::renderer {
namespace {

// The terrain under a point, as openRTS's terrain mesh has it (TerrainRenderer:
// each 2x2 block of samples fanned around its odd-odd centre, so a cell's
// diagonal alternates in a checkerboard), so a transfer lies on what is drawn.
class TerrainHeights {
public:
    TerrainHeights(const core::Vfs& vfs, core::VirtualPath tiles) : vfs_(vfs), tiles_(std::move(tiles)) {}

    // Height at metres (x east, z north) from the centre of tile (tile_x, tile_z).
    [[nodiscard]] std::optional<float> at(std::int32_t tile_x, std::int32_t tile_z, double x, double z) {
        const auto* tile = load(tile_x, tile_z);
        if (!tile) return std::nullopt;
        const double size = tile->sample_size;
        const double half = static_cast<double>(tile->sample_count) * size * 0.5;
        // Into the tile that holds the point.
        const auto shift_x = static_cast<std::int32_t>(std::floor((x + half) / (2.0 * half)));
        const auto shift_z = static_cast<std::int32_t>(std::floor((z + half) / (2.0 * half)));
        if (shift_x != 0 || shift_z != 0) {
            return at(tile_x + shift_x, tile_z + shift_z, x - shift_x * 2.0 * half, z - shift_z * 2.0 * half);
        }
        const double column = (x + half) / size;
        const double row = (half - z) / size;
        const auto c0 = static_cast<std::int64_t>(std::floor(column));
        const auto r0 = static_cast<std::int64_t>(std::floor(row));
        const auto fc = static_cast<float>(column - static_cast<double>(c0));
        const auto fr = static_cast<float>(row - static_cast<double>(r0));
        const auto a = sample(tile_x, tile_z, r0, c0);
        const auto b = sample(tile_x, tile_z, r0, c0 + 1);
        const auto c = sample(tile_x, tile_z, r0 + 1, c0);
        const auto d = sample(tile_x, tile_z, r0 + 1, c0 + 1);
        if (!a || !b || !c || !d) return std::nullopt;
        if ((r0 % 2) == (c0 % 2)) { // diagonal a-d
            if (fc >= fr) return *a + (*b - *a) * fc + (*d - *b) * fr;
            return *a + (*c - *a) * fr + (*d - *c) * fc;
        }
        // diagonal b-c
        if (fc + fr <= 1.0f) return *a + (*b - *a) * fc + (*c - *a) * fr;
        return *d + (*c - *d) * (1.0f - fc) + (*b - *d) * (1.0f - fr);
    }

private:
    [[nodiscard]] const formats::TerrainTile* load(const std::int32_t tile_x, const std::int32_t tile_z) {
        const auto key = std::pair{tile_x, tile_z};
        auto found = cache_.find(key);
        if (found == cache_.end()) {
            auto tile = std::make_unique<formats::TerrainTile>(formats::load_terrain_tile(vfs_, tiles_, tile_x, tile_z));
            if (!tile->ok() || tile->sample_count < 2) tile.reset();
            found = cache_.emplace(key, std::move(tile)).first;
        }
        return found->second.get();
    }

    // Sample (row, column) of a tile; the row and column just past its edge
    // are the south and east neighbours' first ones (rows grow south).
    [[nodiscard]] std::optional<float> sample(std::int32_t tile_x, std::int32_t tile_z, std::int64_t row, std::int64_t column) {
        const auto* tile = load(tile_x, tile_z);
        if (!tile) return std::nullopt;
        const auto n = static_cast<std::int64_t>(tile->sample_count);
        while (column >= n) {
            column -= n;
            ++tile_x;
        }
        while (column < 0) {
            column += n;
            --tile_x;
        }
        while (row >= n) {
            row -= n;
            --tile_z;
        }
        while (row < 0) {
            row += n;
            ++tile_z;
        }
        tile = load(tile_x, tile_z);
        if (!tile) return std::nullopt;
        return tile->height(static_cast<std::uint32_t>(row), static_cast<std::uint32_t>(column));
    }

    const core::Vfs& vfs_;
    core::VirtualPath tiles_;
    std::map<std::pair<std::int32_t, std::int32_t>, std::unique_ptr<formats::TerrainTile>> cache_;
};

} // namespace

std::optional<core::VirtualPath> transfer_shape_path(const core::VirtualPath& world_file, const std::uint32_t uid) {
    const auto stem = world_file.with_extension("");
    return world_file.parent().resolve(std::string(stem.filename()) + "." + std::to_string(uid) + ".transfer");
}

TransferShape build_transfer_shape(const core::Vfs& vfs, const core::VirtualPath& path) {
    TransferShape result;
    // <world file stem>.<UiD>.transfer in the route's WORLD folder.
    const auto name = std::string(path.with_extension("").filename());
    const auto dot = name.rfind('.');
    if (dot == std::string::npos) {
        result.error = "not a transfer path";
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
        if (candidate.kind == formats::WorldObjectKind::transfer && candidate.uid == uid) {
            object = &candidate;
            break;
        }
    }
    if (!object) {
        result.error = "transfer " + std::to_string(uid) + " not in " + world_file->str();
        return result;
    }
    const float width = object->transfer_width;
    const float height = object->transfer_height;
    if (width == 0.0f || height == 0.0f) {
        result.error = "transfer without size";
        return result;
    }
    result.texture = object->shape_file.generic_string();
    const auto route = world_folder.parent();
    if (const auto textures = route.resolve(std::string_view("TEXTURES"))) result.texture_folder = *textures;

    // rts1.exe 0x4B4E60: a grid of 4 m cells (4 % of the width when wider
    // than 100 m), the same cell across; heights from the terrain at each
    // vertex, relative to the object; u along x, v from the north edge.
    std::uint32_t cell = 4;
    if (width > 100.0f) cell = static_cast<std::uint32_t>(4.0 * (0.01 * static_cast<double>(width)));
    cell = (std::max)(cell, 1u);
    const auto columns = static_cast<std::uint32_t>(std::ceil(static_cast<double>(width) / cell));
    const auto rows = static_cast<std::uint32_t>(std::ceil(static_cast<double>(height) / cell));
    const float dx = width / static_cast<float>(columns);
    const float dz = height / static_cast<float>(rows);
    const float half_w = width * 0.5f;
    const float half_h = height * 0.5f;

    const auto& q = object->rotation;
    const glm::quat rotation{q[3], -q[0], -q[1], -q[2]}; // as placement_transform
    TerrainHeights terrain(vfs, route.resolve(std::string_view("TILES")).value_or(route));
    auto& model = result.model;
    model.vertices.reserve(static_cast<std::size_t>(rows + 1) * (columns + 1));
    for (std::uint32_t row = 0; row <= rows; ++row) {
        for (std::uint32_t column = 0; column <= columns; ++column) {
            const float x = -half_w + dx * static_cast<float>(column);
            const float z = -half_h + dz * static_cast<float>(row);
            const glm::vec3 offset = rotation * glm::vec3{x, 0.0f, z};
            float y = 0.0f;
            if (const auto ground = terrain.at(tile.tile_x, tile.tile_z, object->position.x + offset.x, object->position.z + offset.z)) {
                y = *ground - static_cast<float>(object->position.y);
            }
            formats::ShapeMeshVertex vertex;
            vertex.position[0] = x;
            vertex.position[1] = y;
            vertex.position[2] = z;
            vertex.normal[1] = 1.0f;
            vertex.uv[0] = (x + half_w) / width;
            vertex.uv[1] = 1.0f - (z + half_h) / height;
            vertex.colour1 = 0xFFFFFFFFu;
            vertex.colour2 = 0xFF000000u;
            model.vertices.push_back(vertex);
        }
    }
    // Faces up as MSTS stores them: clockwise seen from above (x east, z north).
    const auto stride = columns + 1;
    for (std::uint32_t row = 0; row < rows; ++row) {
        for (std::uint32_t column = 0; column < columns; ++column) {
            const auto a = row * stride + column;
            const auto b = a + 1;
            const auto c = a + stride;
            const auto d = c + 1;
            model.indices.insert(model.indices.end(), {a, c, b, b, c, d});
        }
    }
    const auto grid_count = static_cast<std::uint32_t>(model.indices.size());
    // The far level: one quad over the corners.
    const auto c0 = 0u, c1 = columns, c2 = rows * stride, c3 = rows * stride + columns;
    model.indices.insert(model.indices.end(), {c0, c2, c1, c1, c2, c3});

    formats::ShapeMatrix identity;
    identity.values[0] = identity.values[4] = identity.values[8] = 1.0f;
    identity.name = "TRANSFER";
    model.matrices.push_back(identity);
    model.hierarchy.push_back(-1);
    model.images.emplace_back(result.texture);
    model.filter_names.emplace_back("MipLinear");
    // GL_LINEAR_MIPMAP_LINEAR with a mip bias of -3.
    model.textures.push_back({.image_index = 0, .filter_mode_index = 0, .mipmap_lod_bias = -3.0f});
    model.shader_names.emplace_back("BlendATexDiff");
    model.vertex_states.push_back({.flags = 0, .matrix_index = 0, .light_material_index = -5});
    model.primitive_states.push_back({.flags = 0,
                                      .texture_index = 0,
                                      .texture_count = 1,
                                      .shader_index = 0,
                                      .vertex_state_index = 0,
                                      .alpha_test_mode = 0,
                                      .z_buffer_mode = 1});
    model.draws.push_back({.first_index = 0, .index_count = grid_count, .primitive_state_index = 0, .matrix_index = 0, .level = 0});
    model.draws.push_back({.first_index = grid_count, .index_count = 6, .primitive_state_index = 0, .matrix_index = 0, .level = 1});
    model.level_distances = {300.0f, 2000.0f};
    return result;
}

} // namespace openrts::renderer
