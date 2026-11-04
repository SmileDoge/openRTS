#pragma once

// The terrain height under a point, as openRTS's terrain mesh has it, for the
// shapes RTS builds on the ground (transfers, forests). Internal to
// openrts_renderer.

#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <utility>

#include <openrts/core/vfs.hpp>
#include <openrts/formats/terrain.hpp>

namespace openrts::renderer {

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


} // namespace openrts::renderer
