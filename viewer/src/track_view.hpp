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

#include "camera.hpp"
#include "viewer_scene.hpp"

namespace openrts::viewer {

// The route's track network in the viewer: drawn as lines over the rails,
// driven by a probe traveller, junctions thrown by key, and switch shapes
// animated from their junction state.
class TrackView {
public:
    TrackView(std::unique_ptr<simulation::TrackNetwork> network, formats::RoutePosition origin);

    void attach(WGPUDevice device, WGPUTextureFormat target_format);
    void detach();
    void update(const SceneInput& input, ViewerCamera& camera, float elapsed);
    void draw(WGPURenderPassEncoder pass, const glm::mat4& view_projection);
    // Animation frame for a world instance whose shape is a TDB junction's
    // switch: MainRoute at frame 0, the other branch at frame 1.
    [[nodiscard]] std::optional<float> switch_frame(const renderer::StreamedInstance& instance);
    void draw_overlay() const;
    // Puts the probe on the track nearest to `eye` and lets the camera follow.
    void start_ride(const glm::vec3& eye);

private:
    [[nodiscard]] glm::vec3 local(const simulation::TrackPoint& point, float lift = 0.0f) const;
    void build_lines();
    void place_probe(const glm::vec3& eye, double reach);
    void throw_nearest_junction(const glm::vec3& eye);
    void advance_switches(float elapsed);

    std::unique_ptr<simulation::TrackNetwork> network_;
    formats::RoutePosition origin_;
    std::unique_ptr<renderer::DebugLineRenderer> lines_;
    bool visible_ = true;
    bool follow_ = false;
    std::optional<simulation::TrackTraveller> probe_;
    simulation::MoveResult last_move_;
    std::size_t junction_passes_ = 0;
    std::string message_;
    std::map<std::uint32_t, float> switch_frames_; // junction -> current blade frame
    simulation::JointStatistics joints_;
};

} // namespace openrts::viewer
#endif
