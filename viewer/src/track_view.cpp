#include "track_view.hpp"

#ifdef OPENRTS_HAS_WGPU
#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

#ifdef OPENRTS_HAS_IMGUI
#include <imgui.h>
#endif

namespace openrts::viewer {
namespace {

using renderer::debug_colour;
using renderer::DebugVertex;

constexpr float line_lift = 0.35f; // above the rail head
constexpr std::uint64_t probe_owner = 1;
constexpr float probe_speed = 15.0f;
constexpr float boost_factor = 5.0f;
constexpr double junction_reach = 300.0;
constexpr double indicator_range = 1500.0;

constexpr auto colour_track = debug_colour(255, 210, 0);
constexpr auto colour_junction = debug_colour(230, 0, 230);
constexpr auto colour_end = debug_colour(255, 40, 40);
constexpr auto colour_probe = debug_colour(255, 255, 255);
constexpr auto colour_branch = debug_colour(0, 255, 90);

[[nodiscard]] std::uint32_t item_colour(const formats::TrackItemKind kind) {
    switch (kind) {
    case formats::TrackItemKind::signal: return debug_colour(255, 60, 60);
    case formats::TrackItemKind::speed_post: return debug_colour(60, 120, 255);
    case formats::TrackItemKind::platform: return debug_colour(0, 230, 230);
    case formats::TrackItemKind::siding: return debug_colour(230, 230, 230);
    case formats::TrackItemKind::level_crossing: return debug_colour(255, 140, 0);
    case formats::TrackItemKind::crossover: return debug_colour(120, 255, 120);
    default: return 0;
    }
}

[[nodiscard]] float item_height(const formats::TrackItemKind kind) {
    return kind == formats::TrackItemKind::signal ? 5.0f : kind == formats::TrackItemKind::speed_post ? 3.0f : 2.0f;
}

void vertical(std::vector<DebugVertex>& out, const glm::vec3& base, const float height, const std::uint32_t colour) {
    out.push_back({base, colour});
    out.push_back({base + glm::vec3{0.0f, height, 0.0f}, colour});
}

[[nodiscard]] glm::vec3 heading_vector(const float heading) {
    return {std::sin(heading), 0.0f, std::cos(heading)};
}

} // namespace

TrackView::TrackView(std::unique_ptr<simulation::TrackNetwork> network, const formats::RoutePosition origin)
    : network_(std::move(network)), origin_(origin) {
    joints_ = network_->measure_joints(3);
}

glm::vec3 TrackView::local(const simulation::TrackPoint& point, const float lift) const {
    return {
        static_cast<float>((point.tile_x - origin_.tile_x) * simulation::tile_size + point.x - origin_.x),
        static_cast<float>(point.y - origin_.y) + lift,
        static_cast<float>((point.tile_z - origin_.tile_z) * simulation::tile_size + point.z - origin_.z),
    };
}

void TrackView::attach(const WGPUDevice device, const WGPUTextureFormat target_format) {
    lines_ = std::make_unique<renderer::DebugLineRenderer>(device, target_format);
    if (!lines_->ready()) {
        lines_.reset();
        return;
    }
    build_lines();
}

void TrackView::detach() {
    lines_.reset();
}

void TrackView::build_lines() {
    std::vector<DebugVertex> vertices;
    const auto& network = *network_;
    for (std::uint32_t i = 1; i < network.node_count(); ++i) {
        const auto* node = network.node(i);
        if (!node) continue;
        if (node->kind == formats::TrackNodeKind::vector) {
            // Polyline sampled every 5 m plus every section boundary.
            std::vector<double> offsets;
            for (const auto& section : node->sections) {
                for (double s = 0.0; s < section.length; s += 5.0) offsets.push_back(section.start + s);
            }
            offsets.push_back(node->length);
            std::optional<glm::vec3> previous;
            for (const auto offset : offsets) {
                const auto point = network.point(i, offset);
                if (!point) continue;
                const auto here = local(*point, line_lift);
                if (previous) {
                    vertices.push_back({*previous, colour_track});
                    vertices.push_back({here, colour_track});
                }
                previous = here;
            }
            for (const auto& entry : node->items) {
                const auto& item = network.database().items[entry.item];
                const auto colour = item_colour(item.kind);
                if (colour == 0) continue;
                if (const auto point = network.point(i, entry.offset)) {
                    vertical(vertices, local(*point, line_lift), item_height(item.kind), colour);
                }
            }
        } else if (const auto point = network.node_position(i)) {
            const bool junction = node->kind == formats::TrackNodeKind::junction;
            vertical(vertices, local(*point, line_lift), junction ? 3.0f : 2.0f, junction ? colour_junction : colour_end);
        }
    }
    lines_->set_lines(vertices);
}

void TrackView::place_probe(const glm::vec3& eye, const double reach) {
    const simulation::TrackPoint target{
        .tile_x = origin_.tile_x,
        .tile_z = origin_.tile_z,
        .x = origin_.x + eye.x,
        .y = origin_.y + eye.y,
        .z = origin_.z + eye.z,
    };
    const auto location = network_->locate(target, reach);
    if (!location) {
        message_ = "no track within " + std::to_string(static_cast<int>(reach)) + " m";
        return;
    }
    probe_.emplace(*network_, location->node, location->offset, true);
    message_ = "probe placed on node " + std::to_string(location->node);
}

void TrackView::throw_nearest_junction(const glm::vec3& eye) {
    // Nearest junction to the probe, or to the camera without one.
    auto from = probe_ ? local(probe_->pose()) : eye;
    std::optional<std::uint32_t> best;
    float best_distance = static_cast<float>(junction_reach);
    for (std::uint32_t i = 1; i < network_->node_count(); ++i) {
        const auto* node = network_->node(i);
        if (!node || node->kind != formats::TrackNodeKind::junction) continue;
        const auto position = network_->node_position(i);
        if (!position) continue;
        const float d = glm::length(local(*position) - from);
        if (d < best_distance) {
            best_distance = d;
            best = i;
        }
    }
    if (!best) {
        message_ = "no junction within 300 m";
        return;
    }
    const auto* node = network_->node(*best);
    const auto* state = network_->junction(*best);
    const auto branch = static_cast<std::uint32_t>((state->selected + 1) % node->branch_count());
    message_ = network_->throw_junction(*best, branch)
        ? "junction " + std::to_string(*best) + " -> branch " + std::to_string(branch)
        : "junction " + std::to_string(*best) + " is occupied";
}

void TrackView::advance_switches(const float elapsed) {
    // Blades move at the shape's frame rate (30 fps: one frame per 1/30 s).
    for (auto& [junction, frame] : switch_frames_) {
        const auto* state = network_->junction(junction);
        if (!state) continue;
        const float target = state->selected == state->main_route ? 0.0f : 1.0f;
        const float step = elapsed * 30.0f;
        frame = frame < target ? (std::min)(target, frame + step) : (std::max)(target, frame - step);
    }
}

void TrackView::update(const SceneInput& input, ViewerCamera& camera, const float elapsed) {
    if (input.toggle_track) visible_ = !visible_;
    if (input.place_probe) place_probe(camera.eye(), junction_reach);
    if (input.throw_junction) throw_nearest_junction(camera.eye());
    if (input.toggle_follow) follow_ = !follow_ && probe_.has_value();

    if (probe_ && (input.forward || input.backward)) {
        const float speed = probe_speed * (input.boost ? boost_factor : 1.0f);
        const double distance = (input.forward ? 1.0 : -1.0) * speed * elapsed;
        last_move_ = probe_->move(distance, simulation::JunctionMode::occupy, probe_owner);
        junction_passes_ += last_move_.junctions.size();
        // A probe has no length: release what it passed without the tail's
        // reset, so a run-through keeps the switch where it pushed it.
        for (const auto& pass : last_move_.junctions) {
            if (auto* state = network_->junction(pass.junction)) state->occupied = false;
        }
        if (!last_move_.ok()) {
            message_ = last_move_.stop == simulation::MoveStop::end_of_track ? "end of track" : "junction set against";
        }
    }
    advance_switches(elapsed);

    if (follow_ && probe_) {
        const auto pose = probe_->pose();
        const auto ahead = heading_vector(pose.heading);
        camera.fly = true;
        camera.position = local(pose) - ahead * 18.0f + glm::vec3{0.0f, 5.0f, 0.0f};
        camera.yaw = pose.heading + std::numbers::pi_v<float>;
        camera.pitch = 0.12f;
    }
}

void TrackView::draw(const WGPURenderPassEncoder pass, const glm::mat4& view_projection) {
    if (!lines_ || !visible_) return;
    std::vector<DebugVertex> overlay;
    if (probe_) {
        const auto pose = probe_->pose();
        const auto base = local(pose, line_lift);
        vertical(overlay, base, 8.0f, colour_probe);
        overlay.push_back({base + glm::vec3{0.0f, 1.0f, 0.0f}, colour_probe});
        overlay.push_back({base + glm::vec3{0.0f, 1.0f, 0.0f} + heading_vector(pose.heading) * 6.0f, colour_probe});
    }
    // Selected branch of every junction near the probe or camera origin.
    const auto centre = probe_ ? local(probe_->pose()) : glm::vec3{};
    for (std::uint32_t i = 1; i < network_->node_count(); ++i) {
        const auto* node = network_->node(i);
        if (!node || node->kind != formats::TrackNodeKind::junction) continue;
        const auto position = network_->node_position(i);
        const auto* state = network_->junction(i);
        if (!position || !state || 1 + state->selected >= node->pins.size()) continue;
        if (glm::length(local(*position) - centre) > indicator_range) continue;
        const auto& pin = node->pins[1 + state->selected];
        const auto* branch = network_->node(pin.node);
        if (!branch || branch->kind != formats::TrackNodeKind::vector) continue;
        const double offset = pin.direction == 1 ? (std::min)(8.0, branch->length) : (std::max)(0.0, branch->length - 8.0);
        if (const auto tip = network_->point(pin.node, offset)) {
            overlay.push_back({local(*position, line_lift + 0.2f), colour_branch});
            overlay.push_back({local(*tip, line_lift + 0.2f), colour_branch});
        }
    }
    lines_->set_overlay(overlay);
    lines_->render(pass, view_projection);
}

void TrackView::start_ride(const glm::vec3& eye) {
    // A route start may lie away from the track (TestRoute_RTS: 600 m).
    place_probe(eye, 5000.0);
    follow_ = probe_.has_value();
}

std::optional<float> TrackView::switch_frame(const renderer::StreamedInstance& instance) {
    if (instance.kind != formats::WorldObjectKind::track) return std::nullopt;
    const auto junction = network_->junction_for_object(instance.world_tile_x, instance.world_tile_z, instance.uid);
    if (!junction) return std::nullopt;
    auto [it, inserted] = switch_frames_.try_emplace(*junction, 0.0f);
    if (inserted) {
        const auto* state = network_->junction(*junction);
        it->second = state && state->selected != state->main_route ? 1.0f : 0.0f;
    }
    return it->second;
}

void TrackView::draw_overlay() const {
#ifdef OPENRTS_HAS_IMGUI
    if (!ImGui::CollapsingHeader("Track", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::Text("Network: %zu nodes, %zu data issues%s", network_->node_count() - 1, network_->issues().size(),
                visible_ ? "" : " (hidden: T)");
    ImGui::Text("Joints: median %.1f mm, p95 %.1f cm, max %.2f m", joints_.median * 1000.0, joints_.p95 * 100.0,
                joints_.max);
    for (const auto& joint : joints_.worst) {
        ImGui::Text("  worst: %s node %u%s%u  %.2f m", joint.between_nodes ? "pin" : "section", joint.node,
                    joint.between_nodes ? " -> " : " #",
                    joint.between_nodes ? joint.other : static_cast<std::uint32_t>(joint.section), joint.gap);
    }
    if (probe_) {
        const auto pose = probe_->pose();
        ImGui::Text("Probe: node %u, section %zu, %.1f m, %s", probe_->node(), probe_->section(), probe_->offset(),
                    probe_->forward() ? "forward" : "backward");
        ImGui::Text("  heading %.1f deg, grade %.2f %%, %s%s", pose.heading * 180.0 / std::numbers::pi,
                    -std::tan(pose.pitch) * 100.0, pose.curved ? "curve" : "straight", follow_ ? ", camera follows" : "");
        ImGui::Text("  junctions passed: %zu", junction_passes_);
    } else {
        ImGui::TextUnformatted("Probe: P places it on the nearest track");
    }
    if (!message_.empty()) ImGui::Text("%s", message_.c_str());
#endif
}

} // namespace openrts::viewer
#endif
