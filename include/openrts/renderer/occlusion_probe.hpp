#pragma once

#ifdef OPENRTS_HAS_WGPU
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <glm/vec2.hpp>
#include <webgpu/webgpu.h>

namespace openrts::renderer {

// Whether screen points at given depths are in front of the scene drawn so
// far: an occlusion query per point, a 5-pixel square at its depth (pulled 4 %
// of its distance nearer) recorded in the scene pass before the translucent
// geometry. So text labels hide behind models and the cab but not behind
// glass, and a thin wire in front does not hide them. Results come back a
// frame or two late through mapped buffers; a frame is skipped while every
// slot is in flight. The scene pass is begun with query_set().
class OcclusionProbe {
public:
    static constexpr std::uint32_t max_points = 1024;
    struct Point {
        glm::vec2 ndc{};  // x, y in -1..1
        float depth = 0.0f; // reversed Z (1 near, 0 far)
    };

    OcclusionProbe(WGPUDevice device, WGPUTextureFormat target_format);
    ~OcclusionProbe();
    OcclusionProbe(const OcclusionProbe&) = delete;
    OcclusionProbe& operator=(const OcclusionProbe&) = delete;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] WGPUQuerySet query_set() const noexcept;
    // The points to test this frame (beyond max_points dropped) and a tag
    // that comes back with the result.
    void request(const std::vector<Point>& points, std::uint64_t tag, glm::vec2 viewport);
    // In the scene pass (begun with query_set()), after the opaque geometry.
    void draw(WGPURenderPassEncoder pass);
    // After the pass ended, before the encoder is finished.
    void resolve(WGPUCommandEncoder encoder);
    // After the queue submit.
    void submitted();
    // The newest result not taken yet: its tag and, per point, whether any of
    // it passed the depth test.
    [[nodiscard]] bool take(std::uint64_t& tag, std::vector<bool>& visible);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace openrts::renderer
#endif
