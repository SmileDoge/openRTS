#pragma once

#include <filesystem>
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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
#endif

} // namespace openrts::renderer
