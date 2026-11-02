#pragma once

#ifdef OPENRTS_HAS_WGPU
#include <cstdint>
#include <memory>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <webgpu/webgpu.h>

namespace openrts::renderer {

// A line of the scene `thickness` pixels wide (helper overlays: platforms,
// a signal to its track).
struct WideLine {
    glm::vec3 a{};
    glm::vec3 b{};
    std::uint32_t colour = 0xffffffffu; // 0xAABBGGRR
    float thickness = 2.0f;             // pixels
};

// Draws a frame's wide lines in the scene pass after the models: each a
// screen-space strip, depth-tested against what is drawn (pulled 2 % of its
// distance nearer, so lines lying on a surface show) without writing depth,
// blended by its alpha.
class WideLineRenderer {
public:
    WideLineRenderer(WGPUDevice device, WGPUTextureFormat target_format);
    ~WideLineRenderer();
    WideLineRenderer(const WideLineRenderer&) = delete;
    WideLineRenderer& operator=(const WideLineRenderer&) = delete;

    [[nodiscard]] bool ready() const noexcept;
    void render(WGPURenderPassEncoder pass, const std::vector<WideLine>& lines, const glm::mat4& view_projection,
                glm::vec2 viewport);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace openrts::renderer
#endif
