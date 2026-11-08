#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

#include <glm/mat4x4.hpp>

#include <openrts/core/vfs.hpp>
#include <openrts/formats/environment.hpp>
#include <openrts/renderer/rts_sky.hpp>

#ifdef OPENRTS_HAS_WGPU
#include <webgpu/webgpu.h>
#endif

namespace openrts::renderer {

#ifdef OPENRTS_HAS_WGPU
// Draws an environment's sky dome layers and satellites the way RTS does:
// camera-centred, without depth test, in layer order with the satellites
// inserted after `layers_behind_satellites` layers. Call before scene geometry.
class SkyRenderer {
public:
    SkyRenderer(WGPUDevice device, WGPUTextureFormat target_format);
    ~SkyRenderer();
    SkyRenderer(const SkyRenderer&) = delete;
    SkyRenderer& operator=(const SkyRenderer&) = delete;
    SkyRenderer(SkyRenderer&&) noexcept;
    SkyRenderer& operator=(SkyRenderer&&) noexcept;

    [[nodiscard]] bool ready() const noexcept;

    // Builds layer meshes and uploads textures. Textures are looked up in
    // `texture_directories` of the content VFS in order. `environment` must
    // outlive the renderer.
    void load(const formats::EnvironmentDefinition& environment,
              const core::Vfs& vfs,
              const std::vector<core::VirtualPath>& texture_directories);

    // `view` is the scene view matrix; its translation is ignored.
    void render(WGPURenderPassEncoder pass, const glm::mat4& projection, const glm::mat4& view, const rts::SkyState& state);

    // openRTS (RTS had none): the same sky drawn into a small cube map round
    // the camera, which the openRTS water reflects. Records six passes, one
    // a face, outside any pass, after render() in the same frame. The cube
    // holds the target format's colours (display encoded, as the frame).
    // `draw`, when given, draws each face instead of RTS's sky (openRTS's own
    // sky: NativeSky::render and render_satellites), with the face's
    // projection, view (no translation) and uniform slot (1..6).
    using ProbeDraw = std::function<void(WGPURenderPassEncoder pass, const glm::mat4& projection, const glm::mat4& view, std::uint32_t slot)>;
    void render_reflection_probe(WGPUCommandEncoder encoder, const rts::SkyState& state, const ProbeDraw& draw = {});
    // The cube's view (texture_cube); nullptr until a render_reflection_probe()
    // was recorded (its sky is there from the next frame on) or when it could
    // not be made.
    [[nodiscard]] WGPUTextureView reflection_probe() const noexcept;

    // Over openRTS's own sky (NativeSky): the satellites but the lighting one
    // (the moon), on the far plane, where only the sky is (after the opaque
    // objects). `slot`: 0 the frame, 1..6 a probe face.
    void render_satellites(WGPURenderPassEncoder pass, const glm::mat4& projection, const glm::mat4& view, const rts::SkyState& state,
                           std::uint32_t slot = 0);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
#endif

} // namespace openrts::renderer
