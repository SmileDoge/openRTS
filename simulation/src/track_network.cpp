#include <openrts/simulation/track_network.hpp>

#include <algorithm>
#include <cmath>

namespace openrts::simulation {
namespace {

[[nodiscard]] TrackPoint uid_point(const formats::TrackNodeUid& uid) {
    return {.tile_x = uid.tile_x, .tile_z = uid.tile_z, .x = uid.x, .y = uid.y, .z = uid.z, .heading = uid.heading,
            .pitch = uid.pitch};
}

} // namespace

TrackNetwork::TrackNetwork(formats::TrackDatabase database, formats::TrackSectionLibrary sections)
    : database_(std::move(database)), sections_(std::move(sections)) {
    nodes_.resize(database_.nodes.size());
    for (const auto& source : database_.nodes) {
        if (source.index == 0) continue;
        auto& node = nodes_[source.index];
        node.index = source.index;
        node.kind = source.kind;
        node.pins = source.pins;
        node.uid = source.uid;

        switch (source.kind) {
        case formats::TrackNodeKind::vector: {
            double offset = 0.0;
            for (const auto& placement : source.sections) {
                const auto* definition = sections_.section(placement.section);
                if (!definition) {
                    issues_.push_back({source.index, "unknown track section " + std::to_string(placement.section)});
                }
                const double length = definition ? definition->length : 0.0;
                node.sections.push_back({&placement, definition, offset, length});
                offset += length;
            }
            node.length = offset;
            for (const auto id : source.items) {
                if (id >= database_.items.size()) {
                    issues_.push_back({source.index, "unknown track item " + std::to_string(id)});
                    continue;
                }
                node.items.push_back({id, database_.items[id].distance});
            }
            std::stable_sort(node.items.begin(), node.items.end(),
                             [](const auto& a, const auto& b) { return a.offset < b.offset; });
            if (node.pins.size() != 2) issues_.push_back({source.index, "vector node without two pins"});
            break;
        }
        case formats::TrackNodeKind::junction: {
            node.shape = sections_.shape(source.junction_shape);
            if (!node.shape) {
                issues_.push_back({source.index, "unknown junction shape " + std::to_string(source.junction_shape)});
            }
            if (node.pins.size() < 3) issues_.push_back({source.index, "junction with fewer than two branches"});
            JunctionState state;
            if (node.shape && node.shape->main_route && *node.shape->main_route < node.branch_count()) {
                state.main_route = *node.shape->main_route;
            }
            state.selected = state.main_route;
            junctions_[source.index] = state;
            if (node.uid) {
                junction_objects_[{node.uid->world_tile_x, node.uid->world_tile_z, node.uid->world_uid}] = source.index;
            }
            break;
        }
        case formats::TrackNodeKind::end:
            break;
        }

        for (const auto& pin : node.pins) {
            if (!database_.node(pin.node)) {
                issues_.push_back({source.index, "pin to missing node " + std::to_string(pin.node)});
            }
        }
    }
}

std::unique_ptr<TrackNetwork> TrackNetwork::load(const core::Vfs& vfs, const core::VirtualPath& route,
                                                 std::string* error) {
    auto sections = formats::load_track_sections(vfs, route);
    if (!sections.ok()) {
        if (error) *error = "tsection.dat: " + sections.error;
        return nullptr;
    }
    auto database = formats::load_track_database(vfs, route);
    if (!database.ok()) {
        if (error) *error = "track database: " + database.error;
        return nullptr;
    }
    return std::make_unique<TrackNetwork>(std::move(database), std::move(sections));
}

const NetworkNode* TrackNetwork::node(const std::uint32_t index) const {
    if (index == 0 || index >= nodes_.size() || nodes_[index].index != index) return nullptr;
    return &nodes_[index];
}

JunctionState* TrackNetwork::junction(const std::uint32_t node) {
    const auto it = junctions_.find(node);
    return it == junctions_.end() ? nullptr : &it->second;
}

const JunctionState* TrackNetwork::junction(const std::uint32_t node) const {
    const auto it = junctions_.find(node);
    return it == junctions_.end() ? nullptr : &it->second;
}

bool TrackNetwork::throw_junction(const std::uint32_t node, const std::uint32_t branch) {
    auto* state = junction(node);
    const auto* info = this->node(node);
    if (!state || !info || branch >= info->branch_count() || state->occupied) return false;
    state->selected = branch;
    return true;
}

std::optional<std::uint32_t> TrackNetwork::junction_for_object(const std::int32_t tile_x, const std::int32_t tile_z,
                                                               const std::uint32_t uid) const {
    const auto it = junction_objects_.find({tile_x, tile_z, uid});
    if (it == junction_objects_.end()) return std::nullopt;
    return it->second;
}

std::optional<TrackPoint> TrackNetwork::point(const std::uint32_t index, const double offset) const {
    const auto* node = this->node(index);
    if (!node || node->sections.empty()) return std::nullopt;
    const auto clamped = std::clamp(offset, 0.0, node->length);
    auto section = std::upper_bound(node->sections.begin(), node->sections.end(), clamped,
                                    [](const double value, const NetworkSection& s) { return value < s.start; });
    if (section != node->sections.begin()) --section;
    if (!section->definition) return section_start(*section->placement);
    return evaluate_section(*section->placement, *section->definition, clamped - section->start);
}

std::optional<TrackPoint> TrackNetwork::node_position(const std::uint32_t index) const {
    const auto* node = this->node(index);
    if (!node) return std::nullopt;
    if (node->uid) return uid_point(*node->uid);
    if (node->kind == formats::TrackNodeKind::vector) return point(index, 0.0);
    return std::nullopt;
}

std::optional<TrackNetwork::Location> TrackNetwork::locate(const TrackPoint& target, const double max_distance,
                                                           const double step) const {
    std::optional<Location> best;
    const auto consider = [&](const std::uint32_t node, const double offset) {
        const auto p = point(node, offset);
        if (!p) return;
        const double d = distance(*p, target);
        if (d <= max_distance && (!best || d < best->distance)) best = Location{node, offset, d};
    };
    for (const auto& node : nodes_) {
        if (node.kind != formats::TrackNodeKind::vector || node.sections.empty()) continue;
        // Skip nodes whose start is far beyond their length (cheap tile cull).
        const auto start = point(node.index, 0.0);
        if (!start || distance(*start, target) > node.length + max_distance) continue;
        for (double offset = 0.0; offset < node.length; offset += step) consider(node.index, offset);
        consider(node.index, node.length);
    }
    if (!best) return best;
    // Refine around the best sample.
    for (double span = step; span > 0.01; span *= 0.5) {
        const auto centre = best->offset;
        for (const double candidate : {centre - span * 0.5, centre + span * 0.5}) {
            const auto* node = this->node(best->node);
            if (candidate < 0.0 || candidate > node->length) continue;
            const auto p = point(best->node, candidate);
            if (p && distance(*p, target) < best->distance) best = Location{best->node, candidate, distance(*p, target)};
        }
    }
    return best;
}

JointStatistics TrackNetwork::measure_joints(const std::size_t worst) const {
    std::vector<GeometryJoint> joints;
    for (const auto& node : nodes_) {
        if (node.kind != formats::TrackNodeKind::vector || node.sections.empty()) continue;
        // Between consecutive sections of the node.
        for (std::size_t i = 0; i + 1 < node.sections.size(); ++i) {
            const auto& a = node.sections[i];
            const auto& b = node.sections[i + 1];
            if (!a.definition) continue;
            const auto end = evaluate_section(*a.placement, *a.definition, a.length);
            const auto start = section_start(*b.placement);
            joints.push_back({node.index, node.index, i, false, distance(end, start),
                              heading_difference(end.heading, start.heading)});
        }
        // At the node ends, against the linked node.
        for (std::size_t end_index = 0; end_index < 2 && end_index < node.pins.size(); ++end_index) {
            const auto mine = point(node.index, end_index == 0 ? 0.0 : node.length);
            const auto& pin = node.pins[end_index];
            const auto* other = this->node(pin.node);
            if (!mine || !other) continue;
            std::optional<TrackPoint> theirs;
            if (other->kind == formats::TrackNodeKind::vector) {
                theirs = point(other->index, pin.direction == 1 ? 0.0 : other->length);
            } else {
                theirs = node_position(other->index);
            }
            if (!theirs) continue;
            joints.push_back({node.index, other->index, end_index, true, distance(*mine, *theirs), 0.0});
        }
    }

    JointStatistics statistics{.count = joints.size()};
    if (joints.empty()) return statistics;
    std::sort(joints.begin(), joints.end(), [](const auto& a, const auto& b) { return a.gap > b.gap; });
    statistics.max = joints.front().gap;
    statistics.median = joints[joints.size() / 2].gap;
    statistics.p95 = joints[joints.size() / 20].gap;
    joints.resize((std::min)(worst, joints.size()));
    statistics.worst = std::move(joints);
    return statistics;
}

} // namespace openrts::simulation
