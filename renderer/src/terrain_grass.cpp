#include "terrain_grass.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iostream>

#include <openrts/renderer/forest_clearance.hpp>
#include <openrts/renderer/grass_blockers.hpp>

namespace openrts::renderer::detail {

namespace {

constexpr float cell_size = 4.0f;          // m: blades cached a cell at a time
constexpr float blades_per_square = 14.0f; // tufts of three // a square metre of the densest ground at density 1
constexpr double tile_size = 2048.0;

std::string lower(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// A small hash: three integers to 0..1.
float hash01(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
    std::uint32_t h = a * 0x8DA6B343u ^ b * 0xD8163841u ^ c * 0xCB1AB31Fu;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return static_cast<float>(h & 0xFFFFFFu) / 16777216.0f;
}

// Clumps: smooth value noise over the route, 0..1, about `scale` m across.
float clump(const double x, const double z, const double scale) {
    const double gx = x / scale, gz = z / scale;
    const auto ix = static_cast<std::int64_t>(std::floor(gx)), iz = static_cast<std::int64_t>(std::floor(gz));
    auto fx = static_cast<float>(gx - static_cast<double>(ix)), fz = static_cast<float>(gz - static_cast<double>(iz));
    fx = fx * fx * (3.0f - 2.0f * fx);
    fz = fz * fz * (3.0f - 2.0f * fz);
    const auto at = [](const std::int64_t a, const std::int64_t b) {
        return hash01(static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(b), 0x51u);
    };
    const float top = at(ix, iz) * (1.0f - fx) + at(ix + 1, iz) * fx;
    const float bottom = at(ix, iz + 1) * (1.0f - fx) + at(ix + 1, iz + 1) * fx;
    return top * (1.0f - fz) + bottom * fz;
}

} // namespace

void GrassField::set_settings(std::optional<formats::RouteGrass> settings) {
    settings_ = std::move(settings);
    cells_.clear();
    built_ = false;
}

void GrassField::add_tile(const TerrainTileMesh& mesh) {
    if (mesh.patches.empty() || mesh.vertices.empty()) return;
    Ground g;
    const auto patches = mesh.patches.size();
    g.patches_per_side = static_cast<std::uint32_t>(std::lround(std::sqrt(static_cast<double>(patches))));
    if (g.patches_per_side == 0 || g.patches_per_side * g.patches_per_side != patches) return;
    const auto per_patch = mesh.vertices.size() / patches;
    g.stride = static_cast<std::uint32_t>(std::lround(std::sqrt(static_cast<double>(per_patch))));
    if (g.stride < 2 || g.stride * g.stride != per_patch) return;
    g.sample_size = mesh.vertices[1].position[0] - mesh.vertices[0].position[0];
    g.half_extent = -mesh.vertices[0].position[0];
    if (g.sample_size <= 0.0f || g.half_extent <= 0.0f) return;
    g.heights.reserve(mesh.vertices.size());
    g.uvs.reserve(mesh.vertices.size() * 2);
    for (const auto& v : mesh.vertices) {
        g.heights.push_back(v.position[1]);
        g.uvs.push_back(v.uv[0]);
        g.uvs.push_back(v.uv[1]);
    }
    for (const auto& p : mesh.patches) {
        auto name = p.texture;
        const auto slash = name.find_last_of("/\\");
        if (slash != std::string::npos) name = name.substr(slash + 1);
        g.textures.push_back(lower(name));
    }
    const std::pair key{mesh.tile_x, mesh.tile_z};
    grounds_[key] = std::move(g);
    std::erase_if(cells_, [&](const auto& entry) { return std::get<0>(entry.first) == key.first && std::get<1>(entry.first) == key.second; });
    built_ = false;
}

void GrassField::remove_tile(const std::int32_t tile_x, const std::int32_t tile_z) {
    grounds_.erase({tile_x, tile_z});
    std::erase_if(cells_, [&](const auto& entry) { return std::get<0>(entry.first) == tile_x && std::get<1>(entry.first) == tile_z; });
    built_ = false;
}

void GrassField::grow(const std::pair<std::int32_t, std::int32_t>& tile, const Ground& g, const std::int32_t cx, const std::int32_t cz,
                      Cell& cell) const {
    const auto spp = g.stride - 1;
    const float patch_size = static_cast<float>(spp) * g.sample_size;
    // The cell's middle: which patch.
    const float mx = (static_cast<float>(cx) + 0.5f) * cell_size - g.half_extent;
    const float mz = (static_cast<float>(cz) + 0.5f) * cell_size - g.half_extent;
    const auto pc = static_cast<std::int32_t>(std::floor((mx + g.half_extent) / patch_size));
    const auto pr = static_cast<std::int32_t>(std::floor((g.half_extent - mz) / patch_size));
    if (pc < 0 || pr < 0 || pc >= static_cast<std::int32_t>(g.patches_per_side) || pr >= static_cast<std::int32_t>(g.patches_per_side)) return;
    cell.patch = static_cast<std::size_t>(pr) * g.patches_per_side + static_cast<std::size_t>(pc);
    // The .rtst's ground (its density and height), else any ground the
    // shader finds green (Kuban's .rtst names its embankments alone).
    float density = 1.0f, height = 0.45f;
    cell.test_colour = true;
    if (settings_) {
        const auto found = settings_->grounds.find(g.textures[cell.patch]);
        if (found != settings_->grounds.end()) {
            density = std::clamp(found->second.density / 2.5f, 0.2f, 1.2f);
            height = std::clamp(found->second.height * 0.3f, 0.25f, 0.9f);
            cell.test_colour = false;
        }
    }
    const auto count = static_cast<int>(blades_per_square * density * density_ * cell_size * cell_size);
    if (count <= 0) return;
    const auto base = static_cast<std::size_t>(cell.patch) * g.stride * g.stride;
    const auto axes = forest_clearance_axes();
    const auto blockers = grass_blockers();
    const float keep_off = settings_ && settings_->from_track > 0.0f ? (std::max)(settings_->from_track - 3.0f, 0.8f) : 1.2f;
    const auto ux = static_cast<std::uint32_t>(cx) * 7919u + static_cast<std::uint32_t>(tile.first) * 104729u;
    const auto uz = static_cast<std::uint32_t>(cz) * 6151u + static_cast<std::uint32_t>(tile.second) * 130363u;
    cell.blades.reserve(static_cast<std::size_t>(count));
    for (int k = 0; k < count; ++k) {
        const auto kk = static_cast<std::uint32_t>(k);
        const float x = (static_cast<float>(cx) + hash01(ux, uz, kk * 3u)) * cell_size - g.half_extent;
        const float z = (static_cast<float>(cz) + hash01(ux, uz, kk * 3u + 1u)) * cell_size - g.half_extent;
        // The sample square in the patch, and where in it.
        const float col = (x + g.half_extent) / g.sample_size - static_cast<float>(pc) * static_cast<float>(spp);
        const float row = (g.half_extent - z) / g.sample_size - static_cast<float>(pr) * static_cast<float>(spp);
        const auto j = static_cast<std::uint32_t>(std::clamp(static_cast<int>(col), 0, static_cast<int>(spp) - 1));
        const auto i = static_cast<std::uint32_t>(std::clamp(static_cast<int>(row), 0, static_cast<int>(spp) - 1));
        const float fx = std::clamp(col - static_cast<float>(j), 0.0f, 1.0f);
        const float fz = std::clamp(row - static_cast<float>(i), 0.0f, 1.0f);
        const auto a = base + i * g.stride + j, b = a + 1, c = a + g.stride, d = c + 1;
        if (d >= g.heights.size()) continue;
        const auto lerp2 = [&](const float va, const float vb, const float vc, const float vd) {
            return (va * (1.0f - fx) + vb * fx) * (1.0f - fz) + (vc * (1.0f - fx) + vd * fx) * fz;
        };
        const double rx = static_cast<double>(tile.first) * tile_size + x;
        const double rz = static_cast<double>(tile.second) * tile_size + z;
        // Off the ways, and short at the ballast's edge, taller away from it.
        float edge = 1.0f;
        if (blockers && blockers->covers(rx, rz)) continue; // under a platform, a building, a yard
        if (axes) {
            const auto near = axes->nearest(rx, rz, keep_off + 2.5f);
            if (near < keep_off) continue;
            edge = 0.3f + 0.7f * std::clamp(static_cast<float>(near - keep_off) / 2.5f, 0.0f, 1.0f);
        }
        // Clumps and bare spots, tall and short tufts: no even wall.
        const float big = clump(rx, rz, 7.0), small = clump(rx + 500.0, rz - 300.0, 1.8);
        const float lush = big * 0.65f + small * 0.35f;
        if (lush < 0.25f + 0.35f * hash01(ux + 3u, uz, kk)) continue;
        GrassInstance blade;
        blade.position[0] = x;
        blade.position[1] = lerp2(g.heights[a], g.heights[b], g.heights[c], g.heights[d]) - 0.02f;
        blade.position[2] = z;
        blade.uv[0] = lerp2(g.uvs[a * 2], g.uvs[b * 2], g.uvs[c * 2], g.uvs[d * 2]);
        blade.uv[1] = lerp2(g.uvs[a * 2 + 1], g.uvs[b * 2 + 1], g.uvs[c * 2 + 1], g.uvs[d * 2 + 1]);
        blade.seed = hash01(ux, uz, kk * 3u + 2u);
        const float r = hash01(uz, ux, kk);
        blade.height = height * edge * (0.25f + 1.1f * lush) * (0.5f + 0.7f * r * r);
        blade.width = 0.02f + 0.02f * hash01(ux + 17u, uz, kk);
        // Flowers in spring and summer, in the lush clumps.
        if (flowers_ && hash01(ux + 29u, uz, kk) < 0.06f * small * small * flower_share_) {
            blade.width = -0.006f;
            blade.height = height * (0.9f + 0.5f * r);
        }
        cell.blades.push_back(blade);
    }
}

bool GrassField::update(const formats::RoutePosition& eye, const rts::Season season, const rts::Weather weather, const float radius,
                        const float density) {
    // Winter, snow lying or falling, a season the route has none in, or off.
    const int season_index = static_cast<int>(season);
    bool off = density <= 0.0f || season == rts::Season::winter || weather == rts::Weather::snow;
    if (settings_ && settings_->no_grass_seasons.contains(season_index)) off = true;
    const auto blockers_now = grass_blockers_generation();
    if (season_index != last_season_ || std::abs(density - last_density_) > 1e-3f || blockers_now != last_blockers_) {
        last_blockers_ = blockers_now;
        cells_.clear();
        built_ = false;
    }
    last_season_ = season_index;
    last_density_ = density;
    density_ = density;
    flowers_ = season == rts::Season::spring || season == rts::Season::summer;
    if (off) {
        const bool changed = !last_off_ || !instances_.empty();
        last_off_ = true;
        instances_.clear();
        groups_.clear();
        return changed;
    }
    const bool was_off = last_off_;
    last_off_ = false;
    // Again when the eye moved 2 m or the tiles changed.
    const double moved = std::hypot((eye.tile_x - last_eye_.tile_x) * tile_size + (eye.x - last_eye_.x),
                                    (eye.tile_z - last_eye_.tile_z) * tile_size + (eye.z - last_eye_.z));
    if (built_ && !was_off && moved < 2.0) return false;
    last_eye_ = eye;
    built_ = true;

    std::map<std::tuple<std::pair<std::int32_t, std::int32_t>, std::size_t, bool>, std::vector<const Cell*>> by_patch;
    std::set<CellKey> wanted;
    for (const auto& [key, g] : grounds_) {
        const double ex = (eye.tile_x - key.first) * tile_size + eye.x;
        const double ez = (eye.tile_z - key.second) * tile_size + eye.z;
        if (std::abs(ex) > g.half_extent + radius || std::abs(ez) > g.half_extent + radius) continue;
        const auto cells_side = static_cast<std::int32_t>(std::lround(2.0f * g.half_extent / cell_size));
        const auto c0 = static_cast<std::int32_t>(std::floor((ex - radius + g.half_extent) / cell_size));
        const auto c1 = static_cast<std::int32_t>(std::floor((ex + radius + g.half_extent) / cell_size));
        const auto r0 = static_cast<std::int32_t>(std::floor((ez - radius + g.half_extent) / cell_size));
        const auto r1 = static_cast<std::int32_t>(std::floor((ez + radius + g.half_extent) / cell_size));
        for (auto cz = (std::max)(r0, 0); cz <= (std::min)(r1, cells_side - 1); ++cz) {
            for (auto cx = (std::max)(c0, 0); cx <= (std::min)(c1, cells_side - 1); ++cx) {
                const double dx = (cx + 0.5) * cell_size - g.half_extent - ex;
                const double dz = (cz + 0.5) * cell_size - g.half_extent - ez;
                if (dx * dx + dz * dz > (radius + cell_size) * (radius + cell_size)) continue;
                const CellKey ck{key.first, key.second, cx, cz};
                wanted.insert(ck);
                auto it = cells_.find(ck);
                if (it == cells_.end()) {
                    Cell cell;
                    grow(key, g, cx, cz, cell);
                    it = cells_.emplace(ck, std::move(cell)).first;
                }
                if (!it->second.blades.empty()) by_patch[{key, it->second.patch, it->second.test_colour}].push_back(&it->second);
            }
        }
    }
    // Cells well out of reach dropped.
    if (cells_.size() > wanted.size() * 2 + 256) std::erase_if(cells_, [&](const auto& entry) { return !wanted.contains(entry.first); });
    instances_.clear();
    groups_.clear();
    for (const auto& [patch_key, cells] : by_patch) {
        GrassGroup group{.tile = std::get<0>(patch_key),
                         .patch = std::get<1>(patch_key),
                         .first = static_cast<std::uint32_t>(instances_.size()),
                         .test_colour = std::get<2>(patch_key)};
        for (const auto* cell : cells) instances_.insert(instances_.end(), cell->blades.begin(), cell->blades.end());
        group.count = static_cast<std::uint32_t>(instances_.size()) - group.first;
        if (group.count > 0) groups_.push_back(group);
    }
    if (static const bool trace = std::getenv("OPENRTS_GRASS_TRACE") != nullptr; trace) {
        std::cout << "grass: " << instances_.size() << " blades, " << groups_.size() << " groups, " << cells_.size() << " cells, settings "
                  << (settings_ ? settings_->grounds.size() : 0) << " grounds, blockers " << (grass_blockers() ? grass_blockers()->size() : 0) << std::endl;
        static int told = 0;
        if (told++ < 2) {
            for (const auto& [k, g] : grounds_) {
                for (std::size_t p = 0; p < g.textures.size() && p < 6; ++p) std::cout << "  ground texture '" << g.textures[p] << "'" << std::endl;
                break;
            }
            if (settings_) for (const auto& [name, _] : settings_->grounds) std::cout << "  rtst ground '" << name << "'" << std::endl;
        }
    }
    return true;
}

} // namespace openrts::renderer::detail
