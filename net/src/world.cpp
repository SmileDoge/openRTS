#include <openrts/net/world.hpp>

#include <algorithm>
#include <cctype>
#include <unordered_map>

#include <openrts/simulation/signals.hpp>
#include <openrts/simulation/track_network.hpp>
#include <openrts/simulation/track_traveller.hpp>

namespace openrts::net {

WorldApplied apply_junctions(simulation::TrackNetwork& network, const Snapshot& snapshot) {
    WorldApplied result;
    for (const auto& j : snapshot.junctions) {
        const auto* node = network.node(j.node());
        if (!node || node->kind != formats::TrackNodeKind::junction) {
            result.problems.push_back("junction index " + std::to_string(j.index) + ": node " + std::to_string(j.node()) +
                                      " is no junction");
            continue;
        }
        if (j.branch >= node->branch_count()) {
            result.problems.push_back("junction " + std::to_string(j.node()) + ": no branch " + std::to_string(j.branch));
            continue;
        }
        if (network.throw_junction(j.node(), j.branch)) {
            ++result.applied;
        } else {
            result.problems.push_back("junction " + std::to_string(j.node()) + " would not move (occupied or held)");
        }
    }
    return result;
}

WorldApplied apply_signals(simulation::SignalSystem& signals, const Snapshot& snapshot) {
    WorldApplied result;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> by_item;
    const auto& heads = signals.heads();
    for (std::uint32_t i = 0; i < heads.size(); ++i) by_item[heads[i].track_item].push_back(i);
    for (const auto& h : snapshot.signals) {
        const auto it = by_item.find(h.item);
        if (it == by_item.end()) {
            result.problems.push_back("signal item " + std::to_string(h.item) + ": no head");
            continue;
        }
        simulation::SignalOverride value;
        const auto aspect = static_cast<int>(h.state);
        if (aspect >= 0 && aspect <= static_cast<int>(formats::SignalAspect::clear_2)) {
            value.aspect = static_cast<formats::SignalAspect>(aspect);
        }
        value.draw_state = static_cast<int>(h.draw_state);
        value.speed = h.speed;
        for (const auto head : it->second) signals.set_override(head, value);
        ++result.applied;
    }
    return result;
}

std::optional<simulation::TrackLocation> train_front(simulation::TrackNetwork& network, const SnapshotTrain& train,
                                                     std::string* error) {
    if (!train.pos) {
        if (error) *error = "no position";
        return std::nullopt;
    }
    const auto& pos = *train.pos;
    const auto* node = network.node(pos.node);
    if (!node || node->kind != formats::TrackNodeKind::vector) {
        if (error) *error = "node " + std::to_string(pos.node) + " is no vector node";
        return std::nullopt;
    }
    if (pos.offset < -0.5f || pos.offset > node->length + 0.5) {
        if (error) *error = "offset " + std::to_string(pos.offset) + " is off node " + std::to_string(pos.node);
        return std::nullopt;
    }
    simulation::TrackTraveller traveller(network, pos.node, std::clamp<double>(pos.offset, 0.0, node->length), pos.forward());
    const auto moved = traveller.move(front_distance(train));
    if (!moved.ok()) {
        if (error) *error = "the track ends before the train's front";
        return std::nullopt;
    }
    return simulation::TrackLocation{traveller.node(), traveller.offset(), traveller.forward()};
}

CarFile car_file(const TrainCar& car) {
    auto path = to_utf8(car.file);
    std::replace(path.begin(), path.end(), '/', '\\');
    CarFile out;
    const auto slash = path.rfind('\\');
    out.folder = slash == std::string::npos ? std::string{} : path.substr(0, slash);
    out.name = slash == std::string::npos ? path : path.substr(slash + 1);
    if (const auto dot = out.name.rfind('.'); dot != std::string::npos) {
        auto extension = out.name.substr(dot + 1);
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        out.engine = extension == "eng";
        out.name.resize(dot);
    } else {
        out.engine = car.kind != 0;
    }
    return out;
}

std::string train_name(const SnapshotTrain& train) {
    if (train.names && !(*train.names)[0].empty()) return to_utf8((*train.names)[0]);
    if (train.assign && train.assign->number != 0) return std::to_string(train.assign->number);
    return "train " + std::to_string(train.id);
}

} // namespace openrts::net
