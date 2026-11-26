#pragma once

// openRTS's own models of bulk cargo (docs/cargo.md): a pile in a car's
// loading area and the stream falling from a loader, built in code like a
// transfer and loaded through load_model_asset by their extension.
//
// - `<folder>/<texture file>@<width>x<length>x<pile>.cargopile`: a heap one
//   metre high over a width x length footprint (x across, z along, y up from
//   0), its slopes by the cargo's pile factor (rts1.exe 0x50D1F0), the
//   texture tiled every 2 m. Scaled up and down to its volume.
// - `<folder>/<texture file>.cargostream`: a column 1 m across and 1 m high
//   (y 0..1), scaled to the fall.

#include <string>
#include <string_view>

namespace openrts::renderer {

// The pile's file name in its texture's folder.
[[nodiscard]] std::string cargo_pile_name(std::string_view texture_file, float width, float length, float pile);
[[nodiscard]] std::string cargo_stream_name(std::string_view texture_file);
// The heap's height (0..1) at (x, z) of its footprint.
[[nodiscard]] float cargo_pile_height(float width, float length, float pile, float x, float z);
// The heap's volume over its box (width x length x 1 m): the pile of a
// volume V stands V / (fill * width * length) high.
[[nodiscard]] float cargo_pile_fill(float width, float length, float pile);

} // namespace openrts::renderer
