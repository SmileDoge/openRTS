#pragma once

#include <cstddef>
#include <vector>

#include <openrts/formats/shape_binary.hpp>

namespace openrts::formats {

// Local matrices of `model` at `frame` of animation `animation`. Animated
// nodes get the interpolated key rotation (slerp) and/or translation (linear);
// other matrices keep their stored value. Frames outside the keys clamp.
//
// Rotation keys are applied like world QDirection: in MSTS axes the rows of
// the stored matrix are the rows of the standard rotation matrix of the key
// quaternion. Checked on GLOBAL/SHAPES/A1tPnt3dBLft.s: at frame 1 the left
// blade tip moves 0.13 m towards the track centre (opening) instead of
// through its stock rail.
[[nodiscard]] std::vector<ShapeMatrix> animate_matrices(const ShapeModel& model, std::size_t animation, float frame);

// One node's local matrix at `frame` (its stored value when the animation has
// no keys for it): what animate_matrices gives for that node, without the rest.
[[nodiscard]] ShapeMatrix animate_matrix(const ShapeModel& model, std::size_t animation, std::size_t matrix, float frame);
// The last key frame the animation has for node `matrix` (0: no keys). Cab
// switches and gauges spread their states over the frames 0..last.
[[nodiscard]] float last_key_frame(const ShapeModel& model, std::size_t animation, std::size_t matrix);

} // namespace openrts::formats
