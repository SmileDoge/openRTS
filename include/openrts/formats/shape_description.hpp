#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <openrts/core/vfs.hpp>

namespace openrts::formats {

// ESD_Alternative_Texture bits (MSTS): an image is taken from a subfolder
// of the folder it is found in (TEXTURES\NIGHT, ...) when that applies.
inline constexpr std::uint32_t alternative_snow = 0x1;        // SNOW in winter, or snowing outside summer
inline constexpr std::uint32_t alternative_snow_track = 0x2;  // as snow
inline constexpr std::uint32_t alternative_spring = 0x4;      // SPRING
inline constexpr std::uint32_t alternative_autumn = 0x8;      // AUTUMN
inline constexpr std::uint32_t alternative_winter = 0x10;     // WINTER
inline constexpr std::uint32_t alternative_spring_snow = 0x20;
inline constexpr std::uint32_t alternative_autumn_snow = 0x40;
inline constexpr std::uint32_t alternative_winter_snow = 0x80;
inline constexpr std::uint32_t alternative_night = 0x100;     // NIGHT at night

// A light source of a shape (RTS ESD_LightSources): as a cab's LightSources,
// in the shape's space. RTS lights the scene with it (lights.glsl).
struct ShapeLight {
    std::array<float, 3> position{}, points_to{};
    std::array<float, 2> radius{};      // Radius ( rX rZ ): half sizes at the PointsTo plane
    std::uint32_t colour = 0xFFFFFFFFu; // AARRGGBB
    std::string flags;                  // lower case: light_cone light_sphere night_auto night_only ...
    float distance{};                   // Distance: lit within this far of the camera, m (0: not given)
    std::array<float, 3> attenuation{2.0f, 0.6f, 0.4f}; // kind (2 linear), start share, length share
    float spot_exponent{};
    float source_radius{};
    // NIGHT_AUTO: > 0 lit while the world is at most this lit (RTS 0x4B0284,
    // SkyState::world_lit), else lit at night.
    float auto_when_world_lit{};
    float light_fade{}; // LightFade's first value, s
    // LightArray ( n dx dy dz ): n lamps, each the one before moved by
    // (dx, dy, dz) in the shape's space (a platform's row of lamps); 0 or 1:
    // one lamp.
    std::uint32_t array_count{};
    std::array<float, 3> array_step{};
};

// A box the engineer walks on, climbs or is stopped by (RTS 0x4BB7BC,
// docs/reverse/walking.md): ESD_Complex_Box ( rx ry rz tx ty tz x1 y1 z1 x2
// y2 z2 ), the box between the corners turned by rx, ry, rz degrees (Rx·Ry·Rz,
// in RTS's own frame, X negated) and moved by (tx, ty, tz) in the shape's;
// an ESD_Bounding_Box ( x1 y1 z1 x2 y2 z2 ) is one such box, when there is
// no ESD_Complex. As the file writes them; low and high: the corners sorted.
struct WalkBox {
    std::array<float, 3> rotation{};    // degrees, as written
    std::array<float, 3> translation{}; // as written
    std::array<float, 3> low{}, high{}; // the corners, sorted per axis
};

struct ShapeDescription {
    std::filesystem::path shape_file;
    std::uint32_t alternative_texture{}; // ESD_Alternative_Texture bits
    std::vector<ShapeLight> lights;      // ESD_LightSources
    std::vector<WalkBox> boxes;          // ESD_Complex, else ESD_Bounding_Box
};

struct ShapeDescriptionResult {
    std::optional<ShapeDescription> value;
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return value.has_value(); }
};

// Reads the model filename from the root Shape(...) node of an MSTS/RTS
// text SD file, its ESD_Alternative_Texture and ESD_LightSources.
[[nodiscard]] ShapeDescriptionResult parse_shape_description(std::string_view text);
[[nodiscard]] ShapeDescriptionResult load_shape_description(const std::filesystem::path& file);
[[nodiscard]] ShapeDescriptionResult load_shape_description(const core::Vfs& vfs, const core::VirtualPath& file);

} // namespace openrts::formats
