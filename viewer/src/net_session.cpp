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

void NetSession::update(simulation::TrackNetwork& network, simulation::SignalSystem* signals, TrainView& trains) {
    const double t = now();
    if (!started_) {
        started_ = true;
        std::size_t junctions = 0;
        for (const auto& j : snapshot_.junctions) junctions += net::set_junction(network, j) ? 1 : 0;
        waiting_heads_ = snapshot_.signals;
        std::size_t placed = 0;
        for (const auto& train : snapshot_.trains) {
            auto& remote = remotes_[train.id];
            remote.record = train;
            remote.record.pos.reset();
            if (train.pos) {
                position(remote, *train.pos, network, trains, t);
                if (train.stands) remote.motion.stand(t);
                placed += remote.train != 0 ? 1 : 0;
            }
        }
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
        for (const auto& package : connection_->receive()) apply(package, network, signals, trains, t);
    }
    move_trains(network, trains, t);
    if (t >= report_at_ && !not_handled_.empty()) {
        report_at_ = t + 60.0;
        std::cout << "net: not handled yet:";
        for (const auto& [id, count] : not_handled_) {
            const auto name = net::package_name(id);
            std::cout << ' ' << (name.empty() ? std::to_string(id) : std::string(name)) << " x" << count;
        }
        std::cout << std::endl;
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
        if (const auto j = net::decode_junction(body)) net::set_junction(network, *j);
        return;
    case P(net::Package::signal_head):
        if (const auto head = net::decode_signal_head(body)) {
            if (heads_ && signals) {
                heads_->apply(*signals, *head);
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
