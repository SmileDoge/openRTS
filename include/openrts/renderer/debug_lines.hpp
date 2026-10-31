#pragma once

#ifdef OPENRTS_HAS_WGPU
#include <cstdint>
#include <memory>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <webgpu/webgpu.h>

namespace openrts::renderer {

struct DebugVertex {
    glm::vec3 position;
    std::uint32_t colour; // 0xAABBGGRR
};

// Coloured line lists for debugging overlays (track network, probes).
// Lines are depth-tested against the scene but do not write depth, and can be
// drawn on top of everything (`through` lines).
class DebugLineRenderer {
public:
    DebugLineRenderer(WGPUDevice device, WGPUTextureFormat target_format);
    ~DebugLineRenderer();
    DebugLineRenderer(const DebugLineRenderer&) = delete;
    DebugLineRenderer& operator=(const DebugLineRenderer&) = delete;

    [[nodiscard]] bool ready() const noexcept;
    // Persistent lines (uploaded once); pairs of vertices.
    void set_lines(const std::vector<DebugVertex>& lines);
    // Lines for this frame only, drawn without depth test.
    void set_overlay(const std::vector<DebugVertex>& lines);
    void render(WGPURenderPassEncoder pass, const glm::mat4& view_projection);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] constexpr std::uint32_t debug_colour(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 255) {
    return static_cast<std::uint32_t>(r) | (static_cast<std::uint32_t>(g) << 8) | (static_cast<std::uint32_t>(b) << 16)
        | (static_cast<std::uint32_t>(a) << 24);
}

} // namespace openrts::renderer
#endif
