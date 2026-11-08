#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openrts::formats {

struct ShapeModel;

// openRTS's own changes to a shape, kept beside it in a mod as
// `<shape file>.openrts` (the inspector writes them; RTS never reads them,
// and the shape itself is left as it is). Text, MSTS-like:
//
//   OpenRTSShapeOverride ( 1
//       PrimState ( 12 Image ( "platform.ace" ) LightMaterial ( -5 ) )
//   )
//
// PrimState: the primitive state's index in the shape, the name of its
// texture's image (a check: a shape replaced since no longer matches) and
// what to change: LightMaterial, MSTS's light material (-5 normal, -6 / -7
// shiny, -8 FullBright, -9 circumference, -11 half bright, -12 dark shade).
struct ShapeOverride {
    struct PrimState {
        std::uint32_t index{};
        std::string image; // the image's file name, lower case; empty: not checked
        std::optional<std::int32_t> light_material;
    };
    std::vector<PrimState> prim_states;

    [[nodiscard]] bool empty() const noexcept { return prim_states.empty(); }
    [[nodiscard]] const PrimState* find(std::uint32_t index) const noexcept;
    // Sets, or with no change left removes, primitive state `index`'s entry.
    void set_light_material(std::uint32_t index, std::string image, std::optional<std::int32_t> light_material);
};

[[nodiscard]] ShapeOverride parse_shape_override(std::string_view text);
[[nodiscard]] std::string write_shape_override(const ShapeOverride& data);

// The image file name primitive state `index` of `model` draws (lower case,
// no folder), empty without one.
[[nodiscard]] std::string primitive_image(const ShapeModel& model, std::uint32_t index);

// Applies `data` to `model`: a primitive state with a new light material
// gets a vertex state of its own (a copy of its own with that material), so
// the others sharing it keep theirs. An entry whose image no longer matches
// is skipped. Returns the light material each primitive state had before
// (-5 for one without a vertex state).
std::vector<std::int32_t> apply_shape_override(ShapeModel& model, const ShapeOverride& data);

} // namespace openrts::formats
