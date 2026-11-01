#pragma once

#ifdef OPENRTS_HAS_WGPU
#include <future>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <tuple>
#include <vector>

#include <glm/mat4x4.hpp>
#include <webgpu/webgpu.h>

#include <openrts/core/vfs.hpp>
#include <openrts/formats/route_world.hpp>
#include <openrts/renderer/debug_lines.hpp>
#include <openrts/renderer/glow_sprites.hpp>
#include <openrts/renderer/model_library.hpp>
#include <openrts/renderer/model_renderer.hpp>
#include <openrts/renderer/route_streamer.hpp>
#include <openrts/simulation/signals.hpp>

#include "viewer_scene.hpp"

namespace openrts::viewer {

// The route's signals in the viewer: the signal system (loaded in the
// background: sigcfg, compiled scripts, every world signal), stepped with
// the train on the 0.05 s step, lit lights on the drawn signal shapes, and a
// panel with the train's next signal and a picked head's script state.
class SignalView {
public:
    SignalView(std::shared_ptr<const core::Vfs> vfs, core::VirtualPath route, formats::RoutePosition origin,
               simulation::TrackNetwork& network);
    ~SignalView();

    void attach(WGPUDevice device, WGPUTextureFormat target_format);
    void detach();
    // `train`: the driven train (panel: next signal); `trains`: every train
    // on the track, for occupancy and the signal chains.
    void update(float elapsed, const simulation::Train* train, std::span<const simulation::Train* const> trains,
                const glm::vec3& eye);
    // Lights of the signal instances drawn this frame.
    void collect_lights(std::span<const renderer::StreamedInstance> instances, renderer::ModelLibrary& library,
                        const glm::vec3& eye, float draw_distance);
    void draw(WGPURenderPassEncoder pass, const FrameView& frame, float viewport_height);
    // Optional head sub-objects a signal instance does not use (its
    // SignalSubObjs), collapsed so they are not drawn. Empty: draw as is.
    [[nodiscard]] std::vector<renderer::NodeAdjustment> hidden_parts(const renderer::StreamedInstance& instance,
                                                                     renderer::ModelLibrary& library) const;
    void draw_overlay();
    void set_origin(const formats::RoutePosition& origin) { origin_ = origin; }
    // One label per signal within `range` of `eye`, above its highest light:
    // the world UiD and, per head, its index, type, aspect and draw state.
    void collect_labels(std::vector<WorldLabel>& labels, const glm::vec3& eye, float range) const;
    [[nodiscard]] bool ready() const noexcept { return system_ != nullptr; }

private:
    struct Loaded;
    std::shared_ptr<const core::Vfs> vfs_;
    core::VirtualPath route_;
    formats::RoutePosition origin_;
    simulation::TrackNetwork* network_;
    std::future<std::unique_ptr<Loaded>> loading_;
    std::unique_ptr<Loaded> loaded_;
    std::unique_ptr<simulation::SignalSystem> system_;
    std::map<std::tuple<std::int32_t, std::int32_t, std::uint32_t>, std::uint32_t> by_world_object; // tile x, z, uid -> object
    std::unique_ptr<renderer::GlowSpriteRenderer> glows_;
    std::unique_ptr<renderer::DebugLineRenderer> markers_;
    std::map<std::string, std::uint32_t> light_textures_; // LightTex name (lower case) -> sprite texture
    std::map<std::string, std::uint32_t> light_texture_widths_; // pixels
    bool textures_uploaded_ = false;
    bool show_markers_ = false;
    std::vector<renderer::GlowSprite> sprites_;
    double accumulator_{};
    const simulation::Train* train_ = nullptr;
    glm::vec3 eye_{};
    std::size_t passes_ = 0, passes_at_danger_ = 0;
    std::string last_pass_;
    std::string status_ = "loading signals...";
    bool lights_ = true;
};

} // namespace openrts::viewer
#endif
