#include "cargo_shape.hpp"

#include <openrts/renderer/cargo_shape.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace openrts::renderer {

namespace {

constexpr int pile_steps = 24;     // grid cells across and along
constexpr int stream_sides = 12;
constexpr float texture_metres = 2.0f; // the texture repeats so often

[[nodiscard]] float smooth(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

void finish(formats::ShapeModel& model, const std::string& texture, bool lit) {
    formats::ShapeMatrix identity;
    identity.values[0] = identity.values[4] = identity.values[8] = 1.0f;
    identity.name = "CARGO";
    model.matrices.push_back(identity);
    model.hierarchy.push_back(-1);
    model.images.emplace_back(texture);
    model.filter_names.emplace_back("MipLinear");
    model.textures.push_back({.image_index = 0, .filter_mode_index = 0, .mipmap_lod_bias = 0.0f});
    model.shader_names.emplace_back("TexDiff");
    model.vertex_states.push_back({.flags = 0, .matrix_index = 0, .light_material_index = lit ? -5 : -8});
    model.primitive_states.push_back({.flags = 0,
                                      .texture_index = 0,
                                      .texture_count = 1,
                                      .shader_index = 0,
                                      .vertex_state_index = 0,
                                      .alpha_test_mode = 0,
                                      .z_buffer_mode = 1});
    model.draws.push_back({.first_index = 0,
                           .index_count = static_cast<std::uint32_t>(model.indices.size()),
                           .primitive_state_index = 0,
                           .matrix_index = 0,
                           .level = 0});
    model.level_distances = {2000.0f};
}

void add_vertex(formats::ShapeModel& model, float x, float y, float z, float u, float v, float nx, float ny, float nz) {
    formats::ShapeMeshVertex vertex;
    vertex.position[0] = x;
    vertex.position[1] = y;
    vertex.position[2] = z;
    vertex.uv[0] = u;
    vertex.uv[1] = v;
    vertex.normal[0] = nx;
    vertex.normal[1] = ny;
    vertex.normal[2] = nz;
    vertex.colour1 = 0xFFFFFFFFu;
    vertex.colour2 = 0xFF000000u;
    model.vertices.push_back(vertex);
}

} // namespace

std::string cargo_pile_name(std::string_view texture_file, float width, float length, float pile) {
    char sizes[96];
    std::snprintf(sizes, sizeof(sizes), "@%.2fx%.2fx%.2f.cargopile", width, length, pile);
    return std::string(texture_file) + sizes;
}

std::string cargo_stream_name(std::string_view texture_file) {
    return std::string(texture_file) + ".cargostream";
}

float cargo_pile_height(float width, float length, float pile, float x, float z) {
    // A heap whose slopes take part of the narrower side, steeper for a
    // larger pile factor (sand 0.7, coal 0.5, gravel 0.3, cement 0.1: a
    // powder lies flat), its crest raised along the middle, a little uneven.
    const float edge = (std::min)(width * 0.5f - std::abs(x), length * 0.5f - std::abs(z));
    const float slope = (std::max)(0.3f, (std::min)(width, length) * (0.55f - 0.3f * std::clamp(pile, 0.0f, 1.0f)));
    const float body = smooth(edge / slope);
    const float across = 1.0f - std::pow(std::abs(x) / (std::max)(width * 0.5f, 0.01f), 2.0f);
    const float crest = 0.2f * std::clamp(pile, 0.0f, 1.0f) * across;
    const float lumps = 0.03f * std::sin(x * 2.3f + z * 1.7f) * std::sin(z * 3.1f - x * 0.7f);
    return std::clamp(body * (0.8f + crest + lumps), 0.0f, 1.0f);
}

float cargo_pile_fill(float width, float length, float pile) {
    constexpr int n = 48;
    double sum = 0.0;
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            const float x = (static_cast<float>(i) + 0.5f) / n * width - width * 0.5f;
            const float z = (static_cast<float>(j) + 0.5f) / n * length - length * 0.5f;
            sum += cargo_pile_height(width, length, pile, x, z);
        }
    }
    return static_cast<float>(sum / (n * n));
}

CargoShape build_cargo_shape(const core::VirtualPath& path) {
    CargoShape result;
    const std::string name(path.with_extension("").filename());
    const bool stream = path.extension() == ".cargostream";
    std::string texture = name;
    float width = 1.0f, length = 1.0f, pile = 0.5f;
    if (!stream) {
        const auto at = name.rfind('@');
        bool read = at != std::string::npos;
        if (read) {
            // <width>x<length>x<pile>
            char* end = nullptr;
            const char* p = name.c_str() + at + 1;
            width = std::strtof(p, &end);
            read = end && *end == 'x';
            if (read) length = std::strtof(end + 1, &end), read = end && *end == 'x';
            if (read) pile = std::strtof(end + 1, &end);
        }
        if (!read || width <= 0.0f || length <= 0.0f) {
            result.error = "not a cargo pile path";
            return result;
        }
        texture = name.substr(0, at);
    }
    result.texture = path.parent().resolve(texture);
    auto& model = result.model;
    if (!stream) {
        // The heap's grid, faces up: each cell's two triangles clockwise seen
        // from above (x east, z north), as MSTS stores them.
        const int nx = pile_steps, nz = pile_steps;
        const auto height = [&](float x, float z) { return cargo_pile_height(width, length, pile, x, z); };
        for (int j = 0; j <= nz; ++j) {
            for (int i = 0; i <= nx; ++i) {
                const float x = static_cast<float>(i) / nx * width - width * 0.5f;
                const float z = static_cast<float>(j) / nz * length - length * 0.5f;
                const float dx = width / nx, dz = length / nz;
                const float gx = (height(x + dx, z) - height(x - dx, z)) / (2.0f * dx);
                const float gz = (height(x, z + dz) - height(x, z - dz)) / (2.0f * dz);
                const float n = std::sqrt(gx * gx + 1.0f + gz * gz);
                add_vertex(model, x, height(x, z), z, x / texture_metres, z / texture_metres, -gx / n, 1.0f / n, -gz / n);
            }
        }
        const auto at = [&](int i, int j) { return static_cast<std::uint32_t>(j * (nx + 1) + i); };
        for (int j = 0; j < nz; ++j) {
            for (int i = 0; i < nx; ++i) {
                const auto a = at(i, j), b = at(i + 1, j), c = at(i, j + 1), d = at(i + 1, j + 1);
                model.indices.insert(model.indices.end(), {a, c, d, a, d, b});
            }
        }
    } else {
        // A column, both faces (seen from in it too, when it falls by the cab).
        for (int k = 0; k <= stream_sides; ++k) {
            const float angle = static_cast<float>(k) / stream_sides * 6.2831853f;
            const float c = std::cos(angle), s = std::sin(angle);
            for (int level = 0; level <= 1; ++level) {
                add_vertex(model, 0.5f * c, static_cast<float>(level), 0.5f * s, static_cast<float>(k) / stream_sides * 2.0f,
                           static_cast<float>(level) * 3.0f, c, 0.0f, s);
            }
        }
        for (int k = 0; k < stream_sides; ++k) {
            const auto a = static_cast<std::uint32_t>(k * 2), b = a + 1, c = a + 2, d = a + 3;
            model.indices.insert(model.indices.end(), {a, b, d, a, d, c, a, d, b, a, c, d});
        }
    }
    finish(model, texture, true);
    return result;
}

} // namespace openrts::renderer
