#include "track_view.hpp"

#ifdef OPENRTS_HAS_WGPU
#include <algorithm>
#include <cmath>
#include <vector>

#ifdef OPENRTS_HAS_IMGUI
#include <imgui.h>
#endif

namespace openrts::viewer {
namespace {

using renderer::debug_colour;
using renderer::DebugVertex;

constexpr float line_lift = 0.35f; // above the rail head
constexpr double junction_reach = 300.0;
constexpr double indicator_range = 1500.0;

constexpr auto colour_track = debug_colour(255, 210, 0);
constexpr auto colour_junction = debug_colour(230, 0, 230);
constexpr auto colour_end = debug_colour(255, 40, 40);
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

void TrackView::set_origin(const formats::RoutePosition& origin) {
    origin_ = origin;
    if (lines_) build_lines();
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

std::optional<simulation::TrackTraveller> TrackView::nearest_track(const glm::vec3& eye) {
    const simulation::TrackPoint target{
        .tile_x = origin_.tile_x,
        .tile_z = origin_.tile_z,
        .x = origin_.x + eye.x,
        .y = origin_.y + eye.y,
        .z = origin_.z + eye.z,
    };
    // A route start may lie away from the track (TestRoute_RTS: 600 m).
    const auto location = network_->locate(target, 5000.0);
    if (!location) {
        message_ = "no track within 5 km";
        return std::nullopt;
    }
    return simulation::TrackTraveller(*network_, location->node, location->offset, true);
}

void TrackView::throw_junction(const std::uint32_t junction) {
    const auto* node = network_->node(junction);
    const auto* state = network_->junction(junction);
    if (!node || !state || node->branch_count() == 0) return;
    const auto branch = static_cast<std::uint32_t>((state->selected + 1) % node->branch_count());
    message_ = network_->throw_junction(junction, branch)
        ? "junction " + std::to_string(junction) + " -> branch " + std::to_string(branch)
        : "junction " + std::to_string(junction) + (state->occupied ? " is occupied" : " is held by a train's path");
}

void TrackView::throw_junction_ahead(const simulation::TrackTraveller& from) {
    // Walk along the set route (changing nothing) to the first facing junction.
    auto walker = from;
    double walked = 0.0;
    while (walked < junction_reach) {
        const auto result = walker.move((std::min)(25.0, junction_reach - walked), simulation::JunctionMode::plain);
        for (const auto& pass : result.junctions) {
            if (!pass.facing) continue;
            throw_junction(pass.junction);
            return;
        }
        walked += result.moved;
        if (!result.ok() || result.moved <= 0.0) break;
    }
    message_ = "no facing junction within " + std::to_string(static_cast<int>(junction_reach)) + " m ahead";
}

void TrackView::throw_nearest_junction(const glm::vec3& point) {
    std::optional<std::uint32_t> best;
    float best_distance = static_cast<float>(junction_reach);
    for (std::uint32_t i = 1; i < network_->node_count(); ++i) {
        const auto* node = network_->node(i);
        if (!node || node->kind != formats::TrackNodeKind::junction) continue;
        const auto position = network_->node_position(i);
        if (!position) continue;
        const float d = glm::length(local(*position) - point);
        if (d < best_distance) {
            best_distance = d;
            best = i;
        }
    }
    if (!best) {
        message_ = "no junction within 300 m";
        return;
    }
    throw_junction(*best);
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

void TrackView::update(const SceneInput& input, const float elapsed) {
    if (input.toggle_track) visible_ = !visible_;
    advance_switches(elapsed);
}

void TrackView::draw(const WGPURenderPassEncoder pass, const glm::mat4& view_projection, const glm::vec3& focus) {
    if (!lines_ || !visible_) return;
    std::vector<DebugVertex> overlay;
    // Selected branch of every junction near the focus.
    const auto& centre = focus;
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

void TrackView::collect_places(std::vector<WorldLabel>& labels, std::vector<WorldLine>& lines, const glm::vec3& eye,
                               const float range) const {
    if (item_places_.empty()) {
        auto& places = const_cast<std::map<std::uint32_t, std::pair<std::uint32_t, double>>&>(item_places_);
        for (std::uint32_t n = 1; n < network_->node_count(); ++n) {
            const auto* node = network_->node(n);
            if (!node || node->kind != formats::TrackNodeKind::vector) continue;
            for (const auto& entry : node->items) places[entry.item] = {n, entry.offset};
        }
    }
    const auto& items = network_->database().items;
    constexpr float lift = 0.6f; // over the TDB line (rail top 0.33)
    for (const auto& [id, place] : item_places_) {
        if (id >= items.size()) continue;
        const auto& item = items[id];
        const bool platform = item.kind == formats::TrackItemKind::platform;
        if (!platform && item.kind != formats::TrackItemKind::siding) continue;
        // Each pair once: from the end with the lower id.
        const auto other = item_places_.find(item.linked);
        if (other != item_places_.end() && item.linked < id) continue;
        const std::uint32_t line_colour = platform ? 0xffff7030u : 0xff00d0ffu; // blue / yellow (ABGR)
        const std::uint32_t text_colour = platform ? 0xffff9a60u : 0xff40e0ffu;
        std::vector<glm::vec3> stretch;
        if (other != item_places_.end() && other->second.first == place.first) {
            const double a = place.second, b = other->second.second;
            const int steps = (std::max)(1, static_cast<int>(std::abs(b - a) / 5.0));
            for (int k = 0; k <= steps; ++k) {
                if (const auto p = network_->point(place.first, a + (b - a) * k / steps)) stretch.push_back(local(*p, lift));
            }
        } else {
            if (const auto p = network_->point(place.first, place.second)) stretch.push_back(local(*p, lift));
            if (other != item_places_.end()) {
                if (const auto p = network_->point(other->second.first, other->second.second)) stretch.push_back(local(*p, lift));
            }
        }
        if (stretch.empty()) continue;
        const auto middle = stretch[stretch.size() / 2];
        if (glm::distance(middle, eye) > range) continue;
        if (stretch.size() == 1) stretch.push_back(stretch.front() + glm::vec3{0.0f, 0.01f, 0.0f});
        for (std::size_t k = 0; k + 1 < stretch.size(); ++k) lines.push_back({stretch[k], stretch[k + 1], line_colour, 6.0f});
        std::string text = item.name;
        if (platform && !item.station.empty() && item.station != item.name) text = item.station + " (" + item.name + ")";
        if (text.empty()) continue;
        const auto top = middle + glm::vec3{0.0f, platform ? 8.0f : 6.0f, 0.0f};
        lines.push_back({middle, top, line_colour, 1.5f});
        labels.push_back({.position = top, .text = std::move(text), .colour = text_colour});
    }
}

void TrackView::collect_labels(std::vector<WorldLabel>& labels, const glm::vec3& eye, const float range) const {
    for (std::uint32_t i = 1; i < network_->node_count(); ++i) {
        const auto* node = network_->node(i);
        if (!node || node->kind != formats::TrackNodeKind::junction) continue;
        const auto position = network_->node_position(i);
        const auto* state = network_->junction(i);
        if (!position || !state) continue;
        const auto at = local(*position, 2.5f);
        if (glm::distance(at, eye) > range) continue;
        std::string text = "J" + std::to_string(i) + "  branch " + std::to_string(state->selected) + "/"
            + std::to_string(node->branch_count());
        if (state->selected == state->main_route) text += " (main)";
        const bool held = state->occupied || !state->clearance.empty();
        if (state->occupied) text += "\noccupied";
        if (!state->clearance.empty()) text += state->occupied ? ", clearance" : "\nclearance";
        if (!state->reservations.empty()) text += "\nreserved x" + std::to_string(state->reservations.size());
        labels.push_back({.position = at, .text = std::move(text), .colour = held ? 0xff5080ffu : 0xff60e0ffu});
    }
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
    if (ImGui::TreeNode("Legend")) {
        const auto row = [](const std::uint32_t colour, const char* text) {
            // debug_colour packs 0xAABBGGRR like ImGui.
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(colour | 0xff000000u), "|");
            ImGui::SameLine();
            ImGui::TextUnformatted(text);
        };
        row(colour_track, "track");
        row(colour_junction, "junction");
        row(colour_branch, "set branch of a junction");
        row(colour_end, "end of track");
        row(item_colour(formats::TrackItemKind::signal), "signal (5 m post)");
        row(item_colour(formats::TrackItemKind::speed_post), "speed post (3 m)");
        row(item_colour(formats::TrackItemKind::platform), "platform start / end (2 m)");
        row(item_colour(formats::TrackItemKind::siding), "siding start / end (2 m)");
        row(item_colour(formats::TrackItemKind::level_crossing), "level crossing (2 m)");
        row(item_colour(formats::TrackItemKind::crossover), "crossover (2 m)");
        ImGui::TreePop();
    }
    if (!message_.empty()) ImGui::Text("%s", message_.c_str());
#endif
}

} // namespace openrts::viewer
#endif
