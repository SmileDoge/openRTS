#include "transfer_shape.hpp"
#include "terrain_heights.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <glm/common.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <openrts/formats/route_world.hpp>
#include <openrts/formats/terrain.hpp>

namespace openrts::renderer {

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

    // rts1.exe 0x4B4E60 lays a grid of 4 m cells (4 % of the width when
    // wider than 100 m) on the terrain, heights sampled at its vertices. Its
    // triangles cross the terrain's, so between their vertices they sink
    // under the ground or float over it, and the two flicker (z-fighting;
    // the ground's triangles show through in patches). openRTS cuts the
    // rectangle by the terrain's own triangles instead (TerrainHeights: the
    // samples, the checkerboard diagonals), so every piece lies in its
    // triangle's plane; the shader draws it a hair nearer for what rounding
    // is left (GpuInstance::decal). u along x, v from the north edge, as RTS.
    const float half_w = width * 0.5f;
    const float half_h = height * 0.5f;
    const auto& q = object->rotation;
    const glm::quat rotation{q[3], -q[0], -q[1], -q[2]}; // as placement_transform
    const glm::quat inverse = glm::inverse(rotation);
    TerrainHeights terrain(vfs, route.resolve(std::string_view("TILES")).value_or(route));
    const auto grid = terrain.grid(tile.tile_x, tile.tile_z);
    if (!grid || grid->size <= 0.0) {
        result.error = "no terrain under the transfer";
        return result;
    }
    const double size = grid->size;
    const double half = static_cast<double>(grid->count) * size * 0.5;
    const double px = object->position.x;
    const double pz = object->position.z;

    // The rectangle (x east, z north, from the tile's centre).
    std::vector<glm::dvec2> rectangle;
    glm::dvec2 lo{1e300}, hi{-1e300};
    for (const auto& [sx, sz] : {std::pair{-1.0f, -1.0f}, std::pair{1.0f, -1.0f}, std::pair{1.0f, 1.0f}, std::pair{-1.0f, 1.0f}}) {
        const glm::vec3 offset = rotation * glm::vec3{sx * half_w, 0.0f, sz * half_h};
        rectangle.push_back({px + offset.x, pz + offset.z});
        lo = glm::min(lo, rectangle.back());
        hi = glm::max(hi, rectangle.back());
    }
    // A convex polygon cut by a triangle's three edges (Sutherland-Hodgman).
    const auto clip = [](std::vector<glm::dvec2> polygon, const std::array<glm::dvec2, 3>& triangle) {
        const auto cross = [](const glm::dvec2& a, const glm::dvec2& b, const glm::dvec2& p) {
            return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
        };
        const double orientation = cross(triangle[0], triangle[1], triangle[2]) > 0.0 ? 1.0 : -1.0;
        for (int e = 0; e < 3 && !polygon.empty(); ++e) {
            const auto& a = triangle[static_cast<std::size_t>(e)];
            const auto& b = triangle[static_cast<std::size_t>((e + 1) % 3)];
            std::vector<glm::dvec2> kept;
            for (std::size_t i = 0; i < polygon.size(); ++i) {
                const auto& p = polygon[i];
                const auto& n = polygon[(i + 1) % polygon.size()];
                const double dp = cross(a, b, p) * orientation;
                const double dn = cross(a, b, n) * orientation;
                if (dp >= 0.0) kept.push_back(p);
                if ((dp >= 0.0) != (dn >= 0.0)) kept.push_back(p + (n - p) * (dp / (dp - dn)));
            }
            polygon = std::move(kept);
        }
        return polygon;
    };

    auto& model = result.model;
    const auto add_vertex = [&](const glm::dvec2& at, const double ground) {
        const glm::vec3 local = inverse * glm::vec3{static_cast<float>(at.x - px), 0.0f, static_cast<float>(at.y - pz)};
        formats::ShapeMeshVertex vertex;
        vertex.position[0] = local.x;
        vertex.position[1] = static_cast<float>(ground - object->position.y);
        vertex.position[2] = local.z;
        vertex.normal[1] = 1.0f;
        vertex.uv[0] = (local.x + half_w) / width;
        vertex.uv[1] = 1.0f - (local.z + half_h) / height;
        vertex.colour1 = 0xFFFFFFFFu;
        vertex.colour2 = 0xFF000000u;
        model.indices.push_back(static_cast<std::uint32_t>(model.vertices.size()));
        model.vertices.push_back(vertex);
    };
    const auto first_row = static_cast<std::int64_t>(std::floor((half - hi.y) / size));
    const auto last_row = static_cast<std::int64_t>(std::floor((half - lo.y) / size));
    const auto first_column = static_cast<std::int64_t>(std::floor((lo.x + half) / size));
    const auto last_column = static_cast<std::int64_t>(std::floor((hi.x + half) / size));
    for (auto row = first_row; row <= last_row; ++row) {
        for (auto column = first_column; column <= last_column; ++column) {
            const auto corner = [&](const std::int64_t r, const std::int64_t c) -> std::optional<glm::dvec3> {
                const auto ground = terrain.corner(tile.tile_x, tile.tile_z, r, c);
                if (!ground) return std::nullopt;
                return glm::dvec3{static_cast<double>(c) * size - half, *ground, half - static_cast<double>(r) * size};
            };
            const auto a = corner(row, column);
            const auto b = corner(row, column + 1);
            const auto c = corner(row + 1, column);
            const auto d = corner(row + 1, column + 1);
            if (!a || !b || !c || !d) continue;
            // As TerrainRenderer: each 2x2 block fanned round its odd-odd
            // centre, the diagonal alternating (a-d or b-c).
            const bool ad = (row & 1) == (column & 1);
            using Triangle = std::array<glm::dvec3, 3>;
            const std::array<Triangle, 2> triangles = ad ? std::array<Triangle, 2>{Triangle{*a, *b, *d}, Triangle{*a, *d, *c}}
                                                         : std::array<Triangle, 2>{Triangle{*a, *b, *c}, Triangle{*b, *d, *c}};
            for (const auto& triangle : triangles) {
                const std::array<glm::dvec2, 3> flat{glm::dvec2{triangle[0].x, triangle[0].z}, glm::dvec2{triangle[1].x, triangle[1].z},
                                                     glm::dvec2{triangle[2].x, triangle[2].z}};
                const auto piece = clip(rectangle, flat);
                if (piece.size() < 3) continue;
                // The ground's height on the triangle's plane.
                const double det = (flat[1].x - flat[0].x) * (flat[2].y - flat[0].y) - (flat[2].x - flat[0].x) * (flat[1].y - flat[0].y);
                if (std::abs(det) < 1e-12) continue;
                const auto ground = [&](const glm::dvec2& p) {
                    const double w1 = ((p.x - flat[0].x) * (flat[2].y - flat[0].y) - (flat[2].x - flat[0].x) * (p.y - flat[0].y)) / det;
                    const double w2 = ((flat[1].x - flat[0].x) * (p.y - flat[0].y) - (p.x - flat[0].x) * (flat[1].y - flat[0].y)) / det;
                    return triangle[0].y + (triangle[1].y - triangle[0].y) * w1 + (triangle[2].y - triangle[0].y) * w2;
                };
                // A fan, each triangle clockwise seen from above (x east,
                // z north), as MSTS stores faces up.
                for (std::size_t k = 1; k + 1 < piece.size(); ++k) {
                    const auto& p0 = piece[0];
                    auto p1 = piece[k];
                    auto p2 = piece[k + 1];
                    const double area = (p1.x - p0.x) * (p2.y - p0.y) - (p2.x - p0.x) * (p1.y - p0.y);
                    if (std::abs(area) < 1e-8) continue;
                    if (area > 0.0) std::swap(p1, p2);
                    add_vertex(p0, ground(p0));
                    add_vertex(p1, ground(p1));
                    add_vertex(p2, ground(p2));
                }
            }
        }
    }
    if (model.indices.empty()) {
        result.error = "transfer over no terrain";
        return result;
    }
    const auto piece_count = static_cast<std::uint32_t>(model.indices.size());

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
    // RTS: the grid to 300 m, one quad of the corners to 2000 m. The pieces
    // all the way here (a flat quad would sink into a hill as the grid did).
    model.draws.push_back({.first_index = 0, .index_count = piece_count, .primitive_state_index = 0, .matrix_index = 0, .level = 0});
    model.level_distances = {2000.0f};
    return result;
}

} // namespace openrts::renderer
