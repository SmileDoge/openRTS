#include <openrts/formats/shape_animation.hpp>

#include <algorithm>
#include <cmath>

namespace openrts::formats {
namespace {

// Index of the last key at or before `frame`, and the interpolation factor
// towards the next key.
template <typename Key>
[[nodiscard]] std::pair<std::size_t, float> bracket(const std::vector<Key>& keys, const float frame) {
    if (keys.size() < 2 || frame <= static_cast<float>(keys.front().frame)) return {0, 0.0f};
    for (std::size_t i = 0; i + 1 < keys.size(); ++i) {
        const auto a = static_cast<float>(keys[i].frame);
        const auto b = static_cast<float>(keys[i + 1].frame);
        if (frame < b) return {i, b > a ? (frame - a) / (b - a) : 0.0f};
    }
    return {keys.size() - 1, 0.0f};
}

void slerp(const float (&a)[4], const float (&b)[4], const float t, float (&out)[4]) {
    float to[4] = {b[0], b[1], b[2], b[3]};
    float cosine = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (cosine < 0.0f) { // shortest path
        cosine = -cosine;
        for (auto& v : to) v = -v;
    }
    float wa = 1.0f - t;
    float wb = t;
    if (cosine < 0.9995f) {
        const float angle = std::acos(std::clamp(cosine, -1.0f, 1.0f));
        const float sine = std::sin(angle);
        wa = std::sin((1.0f - t) * angle) / sine;
        wb = std::sin(t * angle) / sine;
    }
    float length = 0.0f;
    for (int i = 0; i < 4; ++i) {
        out[i] = wa * a[i] + wb * to[i];
        length += out[i] * out[i];
    }
    length = std::sqrt(length);
    if (length > 0.0f) {
        for (auto& v : out) v /= length;
    }
}

void set_rotation(ShapeMatrix& matrix, const float (&q)[4]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    auto& m = matrix.values;
    m[0] = 1.0f - 2.0f * (y * y + z * z);
    m[1] = 2.0f * (x * y - w * z);
    m[2] = 2.0f * (x * z + w * y);
    m[3] = 2.0f * (x * y + w * z);
    m[4] = 1.0f - 2.0f * (x * x + z * z);
    m[5] = 2.0f * (y * z - w * x);
    m[6] = 2.0f * (x * z - w * y);
    m[7] = 2.0f * (y * z + w * x);
    m[8] = 1.0f - 2.0f * (x * x + y * y);
}

} // namespace

std::vector<ShapeMatrix> animate_matrices(const ShapeModel& model, const std::size_t animation, const float frame) {
    auto matrices = model.matrices;
    if (animation >= model.animations.size()) return matrices;
    for (const auto& node : model.animations[animation].nodes) {
        if (node.matrix_index < 0 || static_cast<std::size_t>(node.matrix_index) >= matrices.size()) continue;
        auto& matrix = matrices[static_cast<std::size_t>(node.matrix_index)];
        if (!node.rotations.empty()) {
            const auto [i, t] = bracket(node.rotations, frame);
            float q[4];
            if (i + 1 < node.rotations.size()) {
                slerp(node.rotations[i].value, node.rotations[i + 1].value, t, q);
            } else {
                std::copy(std::begin(node.rotations[i].value), std::end(node.rotations[i].value), q);
            }
            set_rotation(matrix, q);
        }
        if (!node.positions.empty()) {
            const auto [i, t] = bracket(node.positions, frame);
            for (int c = 0; c < 3; ++c) {
                const float a = node.positions[i].value[c];
                const float b = i + 1 < node.positions.size() ? node.positions[i + 1].value[c] : a;
                matrix.values[9 + c] = a + (b - a) * t;
            }
        }
    }
    return matrices;
}

} // namespace openrts::formats
