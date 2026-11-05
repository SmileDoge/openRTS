#include "net_session.hpp"

#include <cmath>
#include <iostream>

#include <openrts/net/packages.hpp>
#include <openrts/net/play.hpp>
#include <openrts/simulation/signals.hpp>
#include <openrts/simulation/track_network.hpp>
#include <openrts/simulation/train.hpp>

#include "train_view.hpp"

namespace openrts::viewer {

namespace {

constexpr std::uint16_t P(const net::Package package) noexcept { return net::id(package); }

TrainView::PlacedTrain placed_train(const net::SnapshotTrain& train, const simulation::TrackLocation& front) {
    TrainView::PlacedTrain p;
    p.name = net::train_name(train);
    p.role = TrainView::Role::standing;
    p.front = front;
    for (const auto& car : train.cars) {
        const auto file = net::car_file(car);
        formats::ConsistEntry entry;
        entry.engine = file.engine;
        entry.uid = car.uid;
        entry.name = file.name;
        entry.folder = file.folder;
        entry.flipped = car.flipped();
        entry.cold = (car.flags & 2u) != 0;
        if (car.handbrake) entry.handbrake = *car.handbrake * 100.0f;
        p.cars.push_back(std::move(entry));
    }
    return p;
}

} // namespace

NetSession::NetSession(std::shared_ptr<net::RtsConnection> connection, net::Snapshot snapshot,
                       std::vector<net::ReceivedPackage> early)
    : connection_(std::move(connection)), snapshot_(std::move(snapshot)), early_(std::move(early)) {
    if (snapshot_.time) time_of_day_ = *snapshot_.time;
}

double NetSession::now() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
}

void NetSession::update(simulation::TrackNetwork& network, simulation::SignalSystem* signals, TrainView& trains,
                        const std::optional<formats::RoutePosition>& camera) {
    const double t = now();
    // The camera's place, as rts1.exe sends it: four times a second while it
    // moves, every 5 s anyway. The server relays a train's lights, look and
    // sounds only to players within 3 km (audience 4).
    if (connection_ && camera && t >= position_at_) {
        const auto moved = [&] {
            if (!sent_camera_) return true;
            const auto& a = *sent_camera_;
            const double dx = (camera->tile_x - a.tile_x) * 2048.0 + camera->x - a.x;
            const double dz = (camera->tile_z - a.tile_z) * 2048.0 + camera->z - a.z;
            return dx * dx + dz * dz + (camera->y - a.y) * (camera->y - a.y) > 0.25;
        }();
        if (moved || t >= position_at_ + 4.75) {
            net::PlayerPosition p;
            p.tile_x = static_cast<std::int16_t>(camera->tile_x);
            p.tile_z = static_cast<std::int16_t>(camera->tile_z);
            p.x = static_cast<float>(camera->x);
            p.y = static_cast<float>(camera->y);
            p.z = static_cast<float>(camera->z);
            connection_->send(net::id(net::Package::player_pos), net::encode_player_pos(p));
            sent_camera_ = camera;
            position_at_ = t + 0.25;
        }
    }
    if (!started_) {
        started_ = true;
        std::size_t junctions = 0;
        for (const auto& j : snapshot_.junctions) junctions += net::set_junction(network, j) ? 1 : 0;
        waiting_heads_ = snapshot_.signals;
        std::size_t placed = 0;
        std::uint64_t own = 0;
        for (const auto& train : snapshot_.trains) {
            auto& remote = remotes_[train.id];
            remote.record = train;
            remote.record.pos.reset();
            std::cout << "net: train " << train.id << " \"" << net::train_name(train) << "\": kind " << train.kind << ", "
                      << train.cars.size() << " cars ("
                      << (train.cars.empty() ? std::string("none") : net::to_utf8(train.cars.front().file)) << " first)"
                      << (train.mine() ? ", ours" : "");
            if (train.pos) {
                std::cout << ", rear at node " << train.pos->node << " offset " << train.pos->offset
                          << (train.pos->forward() ? " along" : " against");
            }
            std::cout << std::endl;
            if (train.pos) {
                position(remote, *train.pos, network, trains, t);
                if (train.stands) remote.motion.stand(t);
                placed += remote.train != 0 ? 1 : 0;
                if (train.mine() && remote.train != 0 && own == 0) own = remote.train;
            }
        }
        // The camera goes to the train the server gave us.
        if (own != 0) trains.follow_train(own);
        std::cout << "net: " << junctions << " of " << snapshot_.junctions.size() << " junctions set, " << placed << " of "
                  << snapshot_.trains.size() << " trains on the track" << std::endl;
        for (const auto& package : std::exchange(early_, {})) apply(package, network, signals, trains, t);
    }
    if (signals && !heads_) {
        heads_.emplace(*signals);
        std::size_t applied = 0;
        for (const auto& head : waiting_heads_) applied += heads_->apply(*signals, head) ? 1 : 0;
        std::cout << "net: " << applied << " of " << waiting_heads_.size() << " signal heads set" << std::endl;
        waiting_heads_.clear();
    }
    if (connection_) {
        for (const auto& package : connection_->receive()) {
            ++received_[package.id];
            apply(package, network, signals, trains, t);
        }
    }
    move_trains(network, trains, t);
    // What came from the server, every 10 s while anything comes; the ids
    // the observer leaves are marked.
    if (t >= report_at_ && !received_.empty()) {
        report_at_ = t + 10.0;
        std::cout << "net: received:";
        for (const auto& [id, count] : received_) {
            const auto name = net::package_name(id);
            std::cout << ' ' << (name.empty() ? std::to_string(id) : std::string(name)) << " x" << count
                      << (not_handled_.contains(id) ? " (left)" : "");
        }
        std::cout << std::endl;
        received_.clear();
        not_handled_.clear();
    }
}

void NetSession::position(Remote& remote, const net::TrainPos& pos, simulation::TrackNetwork& network, TrainView& trains,
                          const double now) {
    if (pos.cars.size() != remote.record.cars.size()) {
        // RTS drops it too (the car count must match, 0x4C3DE0): a coupling
        // or a split the observer does not repeat yet.
        if (!remote.warned) {
            remote.warned = true;
            std::cout << "net: train " << remote.record.id << " now has " << pos.cars.size() << " cars, not "
                      << remote.record.cars.size() << ": left where it was" << std::endl;
        }
        return;
    }
    auto record = remote.record;
    record.pos = pos;
    std::string why;
    const auto front = net::train_front(network, record, &why);
    if (!front) {
        std::cout << "net: train " << remote.record.id << ": " << why << std::endl;
        return;
    }
    remote.record.pos = pos;
    if (remote.train == 0) {
        std::string error;
        remote.train = trains.add_remote(placed_train(remote.record, *front), &error);
        if (remote.train == 0) {
            std::cout << "net: train " << remote.record.id << " (" << net::train_name(remote.record)
                      << ") does not fit: " << error << std::endl;
            return;
        }
        remote.motion = {};
        // Its lights, sound and look come from its owner on request, as
        // rts1.exe asks when it loads another player's train (0x4D0228).
        if (connection_) {
            connection_->send(net::id(net::Package::state_request), net::encode_state_request(2, remote.record.id));
        }
    }
    remote.motion.add(network, now, *front, pos.speed);
}

void NetSession::apply(const net::ReceivedPackage& package, simulation::TrackNetwork& network,
                       simulation::SignalSystem* signals, TrainView& trains, const double now) {
    const net::ByteView body = package.body;
    switch (package.id) {
    case P(net::Package::time):
        if (const auto time = net::decode_time(body)) time_of_day_ = *time;
        return;
    case P(net::Package::junction):
        if (const auto j = net::decode_junction(body); j && !net::set_junction(network, *j)) {
            std::cout << "net: junction " << j->node() << " branch " << j->branch << " does not fit" << std::endl;
        }
        return;
    case P(net::Package::signal_head):
        if (const auto head = net::decode_signal_head(body)) {
            if (heads_ && signals) {
                if (!heads_->apply(*signals, *head)) std::cout << "net: signal item " << head->item << " has no head" << std::endl;
            } else {
                waiting_heads_.push_back(*head);
            }
        }
        return;
    case P(net::Package::train_add):
        if (auto train = net::decode_train_add(body)) {
            auto& remote = remotes_[train->id];
            if (remote.train != 0) trains.remove_train(remote.train);
            remote = {};
            remote.record = std::move(*train);
            std::cout << "net: new train " << remote.record.id << " (" << net::train_name(remote.record) << ", "
                      << remote.record.cars.size() << " cars)" << std::endl;
        }
        return;
    case P(net::Package::train_pos):
    case P(net::Package::train_pos_forced):
        if (const auto update = net::decode_train_pos(body)) {
            const auto it = remotes_.find(update->train);
            if (it != remotes_.end()) position(it->second, update->pos, network, trains, now);
        }
        return;
    case P(net::Package::train_stands):
        if (const auto train = net::decode_train_id(body)) {
            const auto it = remotes_.find(*train);
            if (it != remotes_.end()) it->second.motion.stand(now);
        }
        return;
    case P(net::Package::train_remove):
        if (const auto remove = net::decode_train_remove(body)) {
            const auto it = remotes_.find(remove->train);
            if (it == remotes_.end()) return;
            if (it->second.train != 0) trains.remove_train(it->second.train);
            it->second.train = 0;
            it->second.motion = {};
            std::cout << "net: train " << remove->train << (remove->remove ? " deleted" : " taken off the track")
                      << std::endl;
            if (remove->remove) remotes_.erase(it);
        }
        return;
    case P(net::Package::train_info):
        if (const auto info = net::decode_train_info(body)) {
            const auto it = remotes_.find(info->train);
            if (it != remotes_.end()) {
                auto& r = it->second.record;
                r.kind = info->kind;
                r.subtype = info->subtype;
                r.state = info->state;
            }
        }
        return;
    case P(net::Package::train_assign):
        if (const auto a = net::decode_train_assign(body)) {
            std::cout << "net: the server assigns train " << a->train << (a->assign.mine ? " to us" : " away from us")
                      << " (the observer does not drive)" << std::endl;
        }
        return;
    case P(net::Package::car_light):
        if (const auto light = net::decode_car_light(body)) {
            const auto it = remotes_.find(light->train);
            if (it != remotes_.end() && it->second.train != 0) {
                trains.set_remote_light(it->second.train, light->car, light->light, light->lit);
            }
        }
        return;
    case P(net::Package::car_state):
        if (const auto look = net::decode_car_state(body)) {
            const auto it = remotes_.find(look->train);
            if (it != remotes_.end() && it->second.train != 0) {
                trains.set_remote_look(it->second.train, look->car, look->pantographs, look->running);
                auto& seen = it->second.pantographs;
                if (look->car >= seen.size()) seen.resize(look->car + 1, 0xFF);
                if (seen[look->car] != look->pantographs) {
                    seen[look->car] = look->pantographs;
                    std::cout << "net: train " << look->train << " car " << look->car << ": pantographs 0x" << std::hex
                              << int(look->pantographs) << std::dec << ", kind " << int(look->kind) << ", flags 0x"
                              << std::hex << look->flags << std::dec << std::endl;
                }
            }
        }
        return;
    case P(net::Package::message):
        if (const auto text = net::decode_message(body)) std::cout << "net: message: " << net::to_utf8(*text) << std::endl;
        return;
    case P(net::Package::pause):
        if (const auto paused = net::decode_pause(body)) std::cout << "net: " << (*paused ? "paused" : "running") << std::endl;
        return;
    case P(net::Package::server_down):
        if (!down_) std::cout << "net: the server is gone" << std::endl;
        down_ = true;
        return;
    default:
        ++not_handled_[package.id];
        return;
    }
}

void NetSession::move_trains(simulation::TrackNetwork& network, TrainView& trains, const double now) {
    for (auto& [id, remote] : remotes_) {
        if (remote.train == 0 || remote.motion.empty()) continue;
        auto* train = trains.find_train(remote.train);
        if (!train) {
            remote.train = 0;
            continue;
        }
        remote.motion.tick(now);
        const auto target = remote.motion.at(network, now);
        const auto front = train->front_end();
        if (!target || !front) continue;
        const simulation::TrackLocation here{front->node(), front->offset(), front->forward()};
        if (const auto d = net::track_distance(network, here, *target, 300.0)) {
            if (std::abs(*d) > 1e-4) train->shift(*d);
        } else {
            std::string error;
            if (!train->place(network, target->node, target->offset, target->forward, &error)) {
                std::cout << "net: train " << id << ": " << error << std::endl;
            }
        }
        const float speed = remote.motion.speed_at(now);
        if (std::abs(train->speed() - speed) > 1e-3) train->add_velocity(speed - static_cast<float>(train->speed()));
    }
}

} // namespace openrts::viewer
