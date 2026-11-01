#pragma once

#ifdef OPENRTS_HAS_WGPU
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <webgpu/webgpu.h>

#include <openrts/formats/route_world.hpp>
#include <openrts/renderer/debug_lines.hpp>
#include <openrts/renderer/route_streamer.hpp>
#include <openrts/simulation/track_network.hpp>
#include <openrts/simulation/track_traveller.hpp>

#include "viewer_scene.hpp"

namespace openrts::viewer {

// The route's track network in the viewer: drawn as lines over the rails,
// junctions thrown by key, and switch shapes animated from their junction
// state.
class TrackView {
public:
    TrackView(std::unique_ptr<simulation::TrackNetwork> network, formats::RoutePosition origin);

    void attach(WGPUDevice device, WGPUTextureFormat target_format);
    void detach();
    void update(const SceneInput& input, float elapsed);
    // `focus`: where the selected branches of junctions are shown around.
    void draw(WGPURenderPassEncoder pass, const glm::mat4& view_projection, const glm::vec3& focus);
    // Animation frame for a world instance whose shape is a TDB junction's
    // switch: MainRoute at frame 0, the other branch at frame 1.
    [[nodiscard]] std::optional<float> switch_frame(const renderer::StreamedInstance& instance);
    void draw_overlay() const;
    // Labels of the junctions within `range` of `eye`: id, set branch, and
    // whether a train holds it.
    void collect_labels(std::vector<WorldLabel>& labels, const glm::vec3& eye, float range) const;
    [[nodiscard]] simulation::TrackNetwork& network() noexcept { return *network_; }
    // A traveller on the track nearest to `eye` (within 5 km).
    [[nodiscard]] std::optional<simulation::TrackTraveller> nearest_track(const glm::vec3& eye);
    // Throws the first facing junction within reach ahead of `from`.
    void throw_junction_ahead(const simulation::TrackTraveller& from);
    // Throws the junction nearest to `point` (within reach).
    void throw_nearest_junction(const glm::vec3& point);

private:
    [[nodiscard]] glm::vec3 local(const simulation::TrackPoint& point, float lift = 0.0f) const;
    void build_lines();
    void throw_junction(std::uint32_t junction);
    void advance_switches(float elapsed);

    std::unique_ptr<simulation::TrackNetwork> network_;
    formats::RoutePosition origin_;
    std::unique_ptr<renderer::DebugLineRenderer> lines_;
    bool visible_ = true;
    std::string message_;
    std::map<std::uint32_t, float> switch_frames_; // junction -> current blade frame
    simulation::JointStatistics joints_;
};

} // namespace openrts::viewer
#endif
