#include "net_session.hpp"

#include <openrts/core/profile.hpp>

#include <algorithm>
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

// The record's cars get their keys here once (TrainView::PlacedTrain::keys):
// a car keeps its key, and its DLL, through couplings and cuts.
TrainView::PlacedTrain placed_train(net::SnapshotTrain& train, const simulation::TrackLocation& front) {
    TrainView::PlacedTrain p;
    for (auto& car : train.cars) {
        if (car.key == 0) car.key = TrainView::new_car_key();
        p.keys.push_back(car.key);
    }
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

// The console's category ("Прочий" 0 / "Пассажирский" 1 / "Грузовой" 2) as
// the train's for speed posts (rts1.exe train byte +1).
simulation::TrainCategory category_of(const std::uint16_t subtype) {
    return subtype == 1 ? simulation::TrainCategory::passenger
         : subtype == 2 ? simulation::TrainCategory::freight
                        : simulation::TrainCategory::other;
}

} // namespace

NetSession::NetSession(std::shared_ptr<net::RtsConnection> connection, net::Snapshot snapshot,
                       std::vector<net::ReceivedPackage> early, const bool console)
    : connection_(std::move(connection)), snapshot_(std::move(snapshot)), early_(std::move(early)), console_(console) {
    if (snapshot_.time) time_of_day_ = *snapshot_.time;
    for (const auto& train : snapshot_.trains) {
        if (train.route_left && train.route_left->first >= 0.0f) routes_[train.id] = {train.route_left->first, (train.route_left->second & 1) != 0};
    }
}

void NetSession::send_command(const std::string_view line) {
    if (!connection_ || line.empty()) return;
    net::WireWriter w;
    w.text_u32(net::from_utf8(line));
    connection_->send(net::id(net::Package::console_command), w.data());
    console_log_.push_back("> " + std::string(line));
}

void NetSession::send_place(const net::ConsolePlace& place) {
    if (!connection_ || place.cars.empty()) return;
    connection_->send(net::id(net::Package::console_place), net::encode_console_place(place));
    console_log_.push_back("> " + net::to_utf8(place.consist) + ": " + std::to_string(place.cars.size()) + " cars");
}

void NetSession::send_route(const std::uint32_t server_train, const float length, const bool rear_first) {
    if (!connection_ || server_train == 0) return;
    net::WireWriter w;
    w.u32(server_train);
    w.f32(length);
    w.u8(rear_first ? 1 : 0);
    connection_->send(net::id(net::Package::route_left), w.data());
    char text[96];
    if (length < 0.0f) {
        std::snprintf(text, sizeof text, "> train %u: route cancelled", server_train);
    } else {
        std::snprintf(text, sizeof text, "> train %u: route %.0f m%s", server_train, length, rear_first ? " (rear first)" : "");
    }
    console_log_.push_back(text);
}

std::optional<std::pair<float, bool>> NetSession::route_left(const std::uint32_t server_train) const {
    if (const auto it = routes_.find(server_train); it != routes_.end()) return it->second;
    return std::nullopt;
}

void NetSession::request_junction(const std::uint32_t node, const std::uint32_t branch) {
    if (!connection_ || node == 0) return;
    net::WireWriter w;
    w.u32(node - 1);
    w.u32(branch);
    w.u8(0);
    connection_->send(net::id(net::Package::junction), w.data());
}

std::uint32_t NetSession::server_train(const std::uint64_t view_train) const {
    if (view_train == 0) return 0;
    for (const auto& [id, remote] : remotes_) {
        if (remote.train == view_train) return id;
    }
    for (const auto& [id, own] : owned_) {
        if (own.train == view_train) return id;
    }
    return 0;
}

std::uint64_t NetSession::view_train(const std::uint32_t server_train) const {
    if (const auto it = remotes_.find(server_train); it != remotes_.end()) return it->second.train;
    if (const auto it = owned_.find(server_train); it != owned_.end()) return it->second.train;
    return 0;
}

double NetSession::now() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
}

void NetSession::update(simulation::TrackNetwork& network, simulation::SignalSystem* signals, TrainView& trains,
                        const std::optional<formats::RoutePosition>& camera) {
    OPENRTS_ZONE("network");
    const double t = now();
    // The camera's place, as rts1.exe sends it: four times a second while it
    // moves, every 5 s anyway. The server relays a train's lights, look and
    // sounds only to players within 3 km (audience 4).
    // RTS sends nothing at all while the server pauses (live capture).
    if (connection_ && camera && t >= position_at_ && !paused_) {
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
            p.train = driven_id(trains); // the train the player is in, the second field (as rts1.exe)
            connection_->send(net::id(net::Package::player_pos), net::encode_player_pos(p));
            sent_camera_ = camera;
            position_at_ = t + 0.25;
        }
    }
    // A remote train the camera comes back to (within 3 km after being
    // beyond): its owner is asked for its whole look again, as rts1.exe
    // asks when it loads the train again (0x4D0228). Meanwhile the server
    // sent its lights, looks and sounds only to cameras near it.
    if (connection_ && sent_camera_ && t >= range_at_ && !paused_) {
        range_at_ = t + 1.0;
        const auto& c = *sent_camera_;
        for (auto& [id, remote] : remotes_) {
            const auto* train = remote.train ? trains.find_train(remote.train) : nullptr;
            const auto front = train ? train->front_end() : std::nullopt;
            if (!front) continue;
            const auto at = front->pose();
            const double dx = (at.tile_x - c.tile_x) * 2048.0 + at.x - c.x;
            const double dz = (at.tile_z - c.tile_z) * 2048.0 + at.z - c.z;
            const double distance = std::sqrt(dx * dx + dz * dz);
            if (!remote.far && distance > 3000.0) {
                remote.far = true;
            } else if (remote.far && distance < 2900.0) {
                remote.far = false;
                connection_->send(net::id(net::Package::state_request), net::encode_state_request(2, id));
                std::cout << "net: train " << id << " is near again (" << static_cast<int>(distance) << " m): its look asked for"
                          << std::endl;
            }
        }
    }
    if (!started_) {
        started_ = true;
        std::size_t junctions = 0;
        for (const auto& j : snapshot_.junctions) junctions += net::set_junction(network, j) ? 1 : 0;
        // The line as the server keeps it (the console's "Электроснабжение").
        auto& line = trains.electrification();
        for (const auto& s : snapshot_.substations) line.set_substation(s.id, (s.flags & 1u) != 0, s.voltage);
        for (const auto& p : snapshot_.phases) line.set_phase_voltages(p.id, p.voltages);
        for (const auto& s : snapshot_.sections) line.set_section_source(s.section, s.source);
        for (const auto& p : snapshot_.pieces) line.set_piece_supply(p.node, p.piece, p.source);
        if (!snapshot_.substations.empty()) {
            std::cout << "net: line: " << snapshot_.substations.size() << " substations, " << snapshot_.sections.size()
                      << " section sources, " << snapshot_.pieces.size() << " pieces of track from the server" << std::endl;
        }
        waiting_heads_ = snapshot_.signals;
        std::size_t placed = 0;
        std::uint64_t own = 0;
        // Our trains first; the first is driven: placing it clears the track.
        for (const auto& train : snapshot_.trains) {
            if (!train.mine()) continue;
            auto& o = owned_[train.id];
            o.record = train;
            if (!train.pos) {
                std::cout << "net: our train " << train.id << " is off the track" << std::endl;
                continue;
            }
            if (own != 0) {
                if (place_owned(train.id, o, network, trains, false)) ++placed;
                continue;
            }
            std::string why;
            const auto front = net::train_front(network, train, &why);
            if (!front) {
                std::cout << "net: our train " << train.id << ": " << why << std::endl;
                continue;
            }
            // Its DLLs start as the server keeps the train: "Холодный" cold,
            // "Включен" on, "Движется" running.
            trains.set_start_state(train.state == 0 ? 0 : train.state == 1 ? 1 : -1);
            if (const auto error = trains.place_driven(placed_train(o.record, *front)); !error.empty()) {
                std::cout << "net: our train " << train.id << ": " << error << std::endl;
                continue;
            }
            if (const auto* driven = trains.train()) {
                o.train = driven->id();
                trains.set_category(o.train, category_of(train.subtype));
                trains.share_sound(o.train, true);
                trains.set_boardable(o.train, true);
                own = o.train;
                ++placed;
                std::cout << "net: train " << train.id << " \"" << net::train_name(train) << "\" is ours: driven here, "
                          << train.cars.size() << " cars" << std::endl;
            }
        }
        for (const auto& train : snapshot_.trains) {
            if (owned_.contains(train.id)) continue;
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
    send_couplings(trains);
    if (!paused_) send_own(trains, t);
    // What our electric trains drew, to the dispatchers (0x4649, every 2 s
    // window, as rts1.exe 0x5B6030).
    for (const auto& load : trains.electrification().take_closed_loads()) {
        if (!connection_ || paused_) continue;
        net::SubstationLoadReport report;
        report.substation = load.substation;
        report.phase = static_cast<std::uint16_t>(load.phase);
        report.energy = load.energy;
        report.window = load.window;
        connection_->send(net::id(net::Package::substation_load), net::encode_substation_load(report));
    }
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
        if (triggers_sent_ + values_sent_ > 0) {
            std::cout << "net: sent sound: " << triggers_sent_ << " triggers, " << values_sent_ << " values" << std::endl;
            triggers_sent_ = values_sent_ = 0;
        }
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
        trains.set_category(remote.train, category_of(remote.record.subtype));
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
            if (const auto it = owned_.find(train->id); it != owned_.end()) {
                // Ours, sent again: keep it, take what the server says it is.
                const auto pos = it->second.record.pos;
                it->second.record = std::move(*train);
                if (!it->second.record.pos) it->second.record.pos = pos;
                return;
            }
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
        if (const auto update = net::decode_train_pos(body); update && owned_.contains(update->train)) {
            auto& own = owned_[update->train];
            // Off the track: where the server puts it back (0x4603, then
            // 0x4606 puts it in play, rts1.exe 0x4C3DE0).
            if (own.train == 0) {
                own.back_at = update->pos;
                return;
            }
            // On it: a client keeps its own position, except a forced one
            // (rts1.exe takes 0x462F for its own trains, 0x4603 not).
            if (package.id != P(net::Package::train_pos_forced)) return;
            auto* train = trains.find_train(own.train);
            if (!train) return;
            auto record = own.record;
            record.pos = update->pos;
            std::string why;
            if (const auto front = net::train_front(network, record, &why)) {
                train->place(network, front->node, front->offset, front->forward, &why);
                std::cout << "net: the server put our train at node " << front->node << std::endl;
            } else {
                std::cout << "net: the server's place for our train: " << why << std::endl;
            }
            return;
        }
        if (const auto update = net::decode_train_pos(body)) {
            const auto it = remotes_.find(update->train);
            if (it != remotes_.end()) position(it->second, update->pos, network, trains, now);
        }
        return;
    case P(net::Package::train_stands):
        if (const auto train = net::decode_train_id(body)) {
            if (const auto mine = owned_.find(*train); mine != owned_.end()) {
                auto& own = mine->second;
                if (own.train != 0 || !own.back_at) return;
                own.record.pos = std::exchange(own.back_at, std::nullopt);
                // Back in play; the player takes it when he has no train.
                if (place_owned(*train, own, network, trains, driven_id(trains) == 0)) {
                    std::cout << "net: our train " << *train << " is back on the track" << std::endl;
                }
                return;
            }
            const auto it = remotes_.find(*train);
            if (it != remotes_.end()) it->second.motion.stand(now);
        }
        return;
    case P(net::Package::train_remove):
        if (const auto remove = net::decode_train_remove(body)) {
            if (const auto mine = owned_.find(remove->train); mine != owned_.end()) {
                // Ours: off the track (the player let out if he drove it),
                // kept to be put back; or deleted.
                take_off(remove->train, mine->second, trains);
                std::cout << "net: our train " << remove->train
                          << (remove->remove ? " deleted" : " taken off the track: kept to be put back") << std::endl;
                if (remove->remove) owned_.erase(mine);
                return;
            }
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
            if (const auto mine = owned_.find(info->train); mine != owned_.end()) {
                auto& r = mine->second.record;
                r.kind = info->kind;
                r.subtype = info->subtype;
                r.state = info->state;
                if (mine->second.train != 0) trains.set_category(mine->second.train, category_of(info->subtype));
            }
            const auto it = remotes_.find(info->train);
            if (it != remotes_.end()) {
                auto& r = it->second.record;
                r.kind = info->kind;
                r.subtype = info->subtype;
                r.state = info->state;
                if (it->second.train != 0) trains.set_category(it->second.train, category_of(info->subtype));
            }
        }
        return;
    case P(net::Package::train_assign):
        if (const auto a = net::decode_train_assign(body)) assigned(*a, network, trains, now);
        return;
    case P(net::Package::car_light):
        if (const auto light = net::decode_car_light(body)) {
            const auto it = remotes_.find(light->train);
            if (it != remotes_.end() && it->second.train != 0) {
                trains.set_remote_light(it->second.train, light->car, light->light, light->lit);
            }
        }
        return;
    case P(net::Package::sound_trigger):
        if (const auto t = net::decode_sound_trigger(body); t && t->slot == 3) {
            const auto it = remotes_.find(t->train);
            if (it != remotes_.end() && it->second.train != 0) trains.remote_sound_trigger(it->second.train, t->car, t->code);
        }
        return;
    case P(net::Package::sound_state):
        if (const auto s = net::decode_sound_state(body); s && s->values.slot == 3) {
            const auto it = remotes_.find(s->values.train);
            if (it != remotes_.end() && it->second.train != 0) {
                trains.set_remote_sound(it->second.train, s->values.car, s->values.vars);
                std::vector<std::uint32_t> current;
                for (const auto& stream : s->streams) current.push_back(stream.current);
                trains.restore_remote_sound(it->second.train, s->values.car, current);
            }
        }
        return;
    case P(net::Package::sound_values):
        if (const auto v = net::decode_sound_values(body); v && v->slot == 3) {
            const auto it = remotes_.find(v->train);
            if (it != remotes_.end() && it->second.train != 0) trains.set_remote_sound(it->second.train, v->car, v->vars);
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
    case P(net::Package::state_request): {
        // Another client loads our train and asks for its look (mode 2).
        net::WireReader r(body);
        const auto mode = r.u32();
        const auto train = r.u32();
        if (const auto mine = owned_.find(train); r.ok() && mode == 2 && mine != owned_.end() && mine->second.train != 0) {
            send_look(train, mine->second, trains, true);
            // Every car's exterior sound as a whole (rts1.exe 0x4CE290).
            const auto sound = trains.sound_state_of(mine->second.train);
            for (std::size_t car = 0; car < sound.size() && connection_; ++car) {
                net::SoundState state;
                state.values.train = train;
                state.values.car = static_cast<std::uint32_t>(car);
                state.values.vars = sound[car].vars;
                state.values.travelled = sound[car].travelled;
                for (const auto& s : sound[car].streams) {
                    net::SoundStreamState stream;
                    stream.current = s.current;
                    stream.volume = s.volume;
                    stream.triggers.assign(s.triggers, 0u);
                    if (s.current > 0 && s.current <= s.triggers) stream.triggers[s.current - 1] = 1u;
                    state.streams.push_back(std::move(stream));
                }
                connection_->send(net::id(net::Package::sound_state), net::encode_sound_state(state));
            }
        }
        return;
    }
    case P(net::Package::message):
        if (const auto text = net::decode_message(body)) {
            std::cout << "net: message: " << net::to_utf8(*text) << std::endl;
            console_log_.push_back(net::to_utf8(*text));
        }
        return;
    case P(net::Package::console_reply): {
        net::WireReader r(body);
        const auto text = net::to_utf8(r.text_u32());
        if (!r.ok()) return;
        std::size_t from = 0;
        while (from < text.size()) {
            const auto end = text.find('\n', from);
            const auto line = text.substr(from, end == std::string::npos ? std::string::npos : end - from);
            if (!line.empty()) console_log_.push_back(line);
            if (end == std::string::npos) break;
            from = end + 1;
        }
        if (console_log_.size() > 500) console_log_.erase(console_log_.begin(), console_log_.end() - 500);
        return;
    }
    case P(net::Package::route_left): {
        // The distance a train's route has left (RTS sends it to everyone,
        // players too; -1: no route).
        net::WireReader r(body);
        const auto train = r.u32();
        const auto left = r.f32();
        const auto flag = r.u8();
        if (!r.ok()) return;
        if (left < 0.0f) {
            routes_.erase(train);
        } else {
            routes_[train] = {left, (flag & 1) != 0};
        }
        return;
    }
    case P(net::Package::console_state):
        if (auto state = net::decode_console_state(body)) console_state_ = std::move(*state);
        return;
    case P(net::Package::couple):
        if (const auto c = net::decode_couple(body)) coupled(*c, trains);
        return;
    case P(net::Package::uncouple):
        if (const auto u = net::decode_uncouple(body)) uncoupled(*u, trains);
        return;
    case P(net::Package::train_id):
        if (const auto r = net::decode_train_id_change(body)) renamed(r->first, r->second);
        return;
    case P(net::Package::substation):
        if (const auto s = net::decode_substation(body)) {
            trains.electrification().set_substation(s->id, (s->flags & 1u) != 0, s->voltage);
            std::cout << "net: substation " << s->id << (s->flags & 1u ? " off" : " on") << ", " << s->voltage << " V" << std::endl;
        }
        return;
    case P(net::Package::phase_voltages):
        if (const auto phases = net::decode_phase_voltages(body)) {
            for (const auto& p : *phases) trains.electrification().set_phase_voltages(p.id, p.voltages);
        }
        return;
    case P(net::Package::section_source):
        if (const auto s = net::decode_section_source(body)) {
            trains.electrification().set_section_source(s->section, s->source);
            std::cout << "net: section " << s->section << " source " << s->source << std::endl;
        }
        return;
    case P(net::Package::piece_supply):
        if (const auto p = net::decode_piece_supply(body)) trains.electrification().set_piece_supply(p->node, p->piece, p->source);
        return;
    case P(net::Package::pause):
        if (const auto paused = net::decode_pause(body)) {
            paused_ = *paused;
            std::cout << "net: " << (*paused ? "paused" : "running") << std::endl;
        }
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

void NetSession::send_couplings(TrainView& trains) {
    const auto owned_by = [&](const std::uint64_t train) -> std::pair<std::uint32_t, Owned*> {
        for (auto& [id, own] : owned_)
            if (own.train == train) return {id, &own};
        return {0, nullptr};
    };
    for (const auto& cut : trains.take_cuts()) {
        const auto [id, own] = owned_by(cut.from);
        if (!own) continue;
        const std::uint8_t side = cut.front_part ? 1 : 0;
        auto [stay, part_cars] = net::cut_cars(own->record.cars, cut.index, side);
        own->record.cars = std::move(stay);
        own->train = cut.kept;
        own->announced = false;
        // The part is ours under a local id until the server names it
        // (0x4610); the server keeps its kind and owner (0x44FE34).
        const auto local = next_local_++;
        Owned part;
        part.record = own->record;
        part.record.id = local;
        part.record.cars = std::move(part_cars);
        part.record.pos.reset();
        part.train = cut.part;
        part.waiting = true;
        trains.share_sound(cut.part, true);
        trains.set_boardable(cut.part, true);
        trains.set_category(cut.part, category_of(part.record.subtype));
        net::Uncoupling u;
        u.train = id;
        u.car = static_cast<std::uint32_t>(cut.index);
        u.part = local;
        u.side = side;
        const auto parent = id;
        owned_[local] = std::move(part);
        if (connection_) connection_->send(net::id(net::Package::uncouple), net::encode_uncouple(u));
        std::cout << "net: our train " << parent << " cut before car " << cut.index << ": the part waits for its id ("
                  << local << ")" << std::endl;
    }
    for (const auto& c : trains.take_couplings()) {
        const auto [a, own] = owned_by(c.survivor);
        if (!own) continue;
        // The other train: ours, another client's or the server's.
        std::uint32_t b = 0;
        net::SnapshotTrain other;
        if (const auto [id, mine] = owned_by(c.absorbed); mine) {
            b = id;
            other = mine->record;
            owned_.erase(id);
        } else {
            for (auto it = remotes_.begin(); it != remotes_.end(); ++it) {
                if (it->second.train != c.absorbed) continue;
                b = it->first;
                other = it->second.record;
                remotes_.erase(it);
                break;
            }
        }
        if (b == 0) continue;
        // The ends: 1 the front, 0 the rear (A's cars, then B's, is A's rear
        // to B's front).
        net::Coupling out;
        out.a = a;
        out.b = b;
        out.end_a = c.absorbed_ahead ? 1 : 0;
        out.end_b = c.absorbed_ahead ? (c.absorbed_turned ? 1 : 0) : (c.absorbed_turned ? 0 : 1);
        own->record.cars = net::coupled_cars(own->record.cars, other.cars, out.end_a, out.end_b);
        own->announced = false;
        if (connection_) connection_->send(net::id(net::Package::couple), net::encode_couple(out));
        std::cout << "net: our train " << a << " took train " << b << " in: " << own->record.cars.size() << " cars" << std::endl;
    }
}

void NetSession::coupled(const net::Coupling& c, TrainView& trains) {
    // Another client coupled: A took B in. A's next position has every car.
    const auto a = remotes_.find(c.a);
    if (a == remotes_.end()) return;
    std::vector<net::TrainCar> b_cars;
    if (const auto b = remotes_.find(c.b); b != remotes_.end()) {
        b_cars = b->second.record.cars;
        if (b->second.train != 0) trains.remove_train(b->second.train);
        remotes_.erase(b);
    } else if (const auto mine = owned_.find(c.b); mine != owned_.end()) {
        b_cars = mine->second.record.cars; // ours, though the server took it first (0x4601)
        take_off(c.b, mine->second, trains);
        owned_.erase(mine);
    } else {
        return;
    }
    auto& remote = a->second;
    remote.record.cars = net::coupled_cars(remote.record.cars, b_cars, c.end_a, c.end_b);
    if (remote.train != 0) trains.remove_train(remote.train);
    remote.train = 0;
    remote.motion = {};
    remote.warned = false;
    remote.pantographs.clear();
    remote.record.pos.reset();
    std::cout << "net: train " << c.a << " took train " << c.b << " in: " << remote.record.cars.size() << " cars" << std::endl;
}

void NetSession::uncoupled(const net::Uncoupling& u, TrainView& trains) {
    // Another client cut its train: the part is a train of its own (kind and
    // owner as the train's, 0x44FE34); both come back with their positions.
    const auto t = remotes_.find(u.train);
    if (t == remotes_.end()) return;
    auto [stay, part_cars] = net::cut_cars(t->second.record.cars, u.car, u.side);
    auto& remote = t->second;
    Remote part;
    part.record = remote.record;
    part.record.id = u.part;
    part.record.cars = std::move(part_cars);
    part.record.pos.reset();
    remote.record.cars = std::move(stay);
    if (remote.train != 0) trains.remove_train(remote.train);
    remote.train = 0;
    remote.motion = {};
    remote.warned = false;
    remote.record.pos.reset();
    std::cout << "net: train " << u.train << " cut before car " << u.car << ": the part is train " << u.part << " ("
              << part.record.cars.size() << " cars)" << std::endl;
    remotes_[u.part] = std::move(part);
}

void NetSession::renamed(const std::uint32_t from, const std::uint32_t to) {
    if (const auto mine = owned_.find(from); mine != owned_.end()) {
        auto own = std::move(mine->second);
        owned_.erase(mine);
        own.record.id = to;
        own.waiting = false;
        own.announced = false;
        owned_[to] = std::move(own);
        std::cout << "net: our train " << from << " is train " << to << " on the server" << std::endl;
    } else if (const auto it = remotes_.find(from); it != remotes_.end()) {
        auto remote = std::move(it->second);
        remotes_.erase(it);
        remote.record.id = to;
        remotes_[to] = std::move(remote);
    }
}

std::uint32_t NetSession::driven_id(const TrainView& trains) const {
    for (const auto& [id, own] : owned_) {
        if (own.train != 0 && trains.is_player(own.train)) return id;
    }
    return 0;
}

bool NetSession::place_owned(const std::uint32_t id, Owned& own, simulation::TrackNetwork& network, TrainView& trains,
                             const bool drive) {
    if (!own.record.pos) return false;
    std::string why;
    const auto front = net::train_front(network, own.record, &why);
    if (!front) {
        std::cout << "net: our train " << id << ": " << why << std::endl;
        return false;
    }
    std::string error;
    own.train = trains.add_local(placed_train(own.record, *front), &error);
    if (own.train == 0) {
        std::cout << "net: our train " << id << " does not fit: " << error << std::endl;
        return false;
    }
    trains.share_sound(own.train, true);
    trains.set_boardable(own.train, true);
    trains.set_category(own.train, category_of(own.record.subtype));
    own.moving = false;
    own.announced = false;
    own.lights_sent.clear();
    own.looks_sent.clear();
    own.sound_sent.clear();
    if (drive) {
        const auto state = own.record.state;
        trains.set_start_state(state == 0 ? 0 : state == 1 ? 1 : -1);
        if (const auto problem = trains.board(own.train); !problem.empty()) {
            std::cout << "net: our train " << id << ": " << problem << std::endl;
        }
    }
    std::cout << "net: train " << id << " \"" << net::train_name(own.record) << "\" is ours: "
              << (drive ? "driven here" : "standing here") << ", " << own.record.cars.size() << " cars" << std::endl;
    return true;
}

void NetSession::take_off(const std::uint32_t id, Owned& own, TrainView& trains) {
    static_cast<void>(id);
    if (own.train == 0) return;
    // Where it stood, for the record (the server keeps its own).
    if (const auto* train = trains.find_train(own.train)) {
        if (const auto pos = net::position_of(*train)) own.record.pos = *pos;
    }
    trains.share_sound(own.train, false);
    trains.drop(own.train);
    own.train = 0;
    own.back_at.reset();
}

// 0x4601 during play: a train becomes ours, or stops being ours. RTS keeps
// it where it is (kind 1 or kind 2 + 0x10000, 0x4C3DE0).
void NetSession::assigned(const net::TrainAssignUpdate& a, simulation::TrackNetwork& network, TrainView& trains,
                          const double now) {
    static_cast<void>(now);
    if (a.assign.mine) {
        if (owned_.contains(a.train)) return;
        Owned own;
        const auto it = remotes_.find(a.train);
        std::uint64_t kept = 0; // the train on the track here, taken back as it is
        if (it != remotes_.end()) {
            own.record = it->second.record;
            if (auto* train = it->second.train != 0 ? trains.find_train(it->second.train) : nullptr) {
                if (const auto pos = net::position_of(*train)) own.record.pos = *pos;
                if (trains.take_back(it->second.train)) kept = it->second.train;
            }
            remotes_.erase(it);
        } else {
            own.record.id = a.train;
        }
        own.record.assign = a.assign;
        own.record.kind = a.assign.kind;
        auto& placed = owned_[a.train] = std::move(own);
        std::cout << "net: the server gives us train " << a.train << std::endl;
        if (kept != 0) {
            // Ours where it stands; its DLLs, if they ran here before, go on.
            placed.train = kept;
            placed.moving = false;
            placed.announced = false;
            trains.share_sound(kept, true);
            trains.set_boardable(kept, true);
            trains.set_category(kept, category_of(placed.record.subtype));
            if (driven_id(trains) == 0) {
                trains.set_start_state(placed.record.state == 0 ? 0 : placed.record.state == 1 ? 1 : -1);
                if (const auto problem = trains.board(kept); !problem.empty()) std::cout << "net: " << problem << std::endl;
            }
        } else if (placed.record.pos && !placed.record.cars.empty()) {
            place_owned(a.train, placed, network, trains, driven_id(trains) == 0);
        }
        return;
    }
    const auto mine = owned_.find(a.train);
    if (mine == owned_.end()) return;
    std::cout << "net: the server takes train " << a.train << " away from us: its DLLs stop where they are" << std::endl;
    auto own = std::move(mine->second);
    owned_.erase(mine);
    // Another client's now (RTS kind 2): the same train here, drawn by the
    // network; its DLLs bound but not run, as rts1.exe (0x424488).
    auto& remote = remotes_[a.train];
    remote = {};
    remote.record = std::move(own.record);
    remote.record.assign = a.assign;
    if (own.train != 0) {
        if (const auto* train = trains.find_train(own.train)) {
            if (const auto pos = net::position_of(*train)) remote.record.pos = *pos;
        }
        trains.give_away(own.train);
        remote.train = own.train;
        if (connection_) connection_->send(net::id(net::Package::state_request), net::encode_state_request(2, a.train));
    }
}

void NetSession::send_own(TrainView& trains, const double now) {
    if (!connection_) return;
    for (auto& [id, own] : owned_) {
        if (own.train == 0 || own.waiting) continue;
        const auto* train = trains.find_train(own.train);
        if (!train || !train->placed()) continue;
        // The position: every 1.5 s while it moves, at once when it starts,
        // and a last one with train_stands when it stops (as rts1.exe).
        const bool moving = std::abs(train->speed()) > 0.01;
        // Standing: once when it is placed here too, so the server forgets a
        // speed it kept from before.
        const bool stopped = !moving && (own.moving || !own.announced);
        bool position_sent = false;
        if ((moving && (!own.moving || now >= own.next_position)) || stopped) {
            if (auto pos = net::position_of(*train)) {
                // A train that stands sends speed 0 exactly: the server keeps
                // the last speed (train +332), and the ТЧД console holds a
                // PLAYER train with any other as "Движется", its state and
                // player locked (0x4140A0). Physics leaves a few mm/s.
                if (!moving) {
                    pos->speed = 0.0f;
                    pos->acceleration = 0.0f;
                    for (auto& car : pos->cars) car.speed = car.acceleration = 0.0f;
                }
                connection_->send(net::id(net::Package::train_pos), net::encode_train_pos(id, *pos));
                own.next_position = now + 1.5;
                position_sent = moving;
            }
        }
        // Derailed cars (0x4636, as rts1.exe): when a car comes off, and with
        // each position while the train still moves. Every client lets them
        // fall itself; the server marks them (car +32 0x20, train +4 0x1000).
        {
            const auto derail = trains.derail_of(own.train);
            const auto off = static_cast<std::size_t>(std::count(derail.derailed.begin(), derail.derailed.end(), std::uint8_t{1}));
            if (off == 0) {
                own.derailed_sent = 0;
            } else if (off > own.derailed_sent || position_sent) {
                net::Derail d;
                d.train = id;
                d.derailed = derail.derailed;
                for (const auto& car : derail.cars) {
                    net::DerailedCar c;
                    c.car = static_cast<std::uint32_t>(car.car);
                    c.heading = {car.heading.x, car.heading.y, car.heading.z};
                    c.position = {static_cast<float>(car.position.x), static_cast<float>(car.position.y),
                                  static_cast<float>(car.position.z)};
                    c.tip = car.tilt;
                    c.tip_rest = car.tilt_target;
                    c.tip_third = car.tilt_target * 0.7f;
                    c.weight = car.weight;
                    c.state = car.tilt != 0.0f ? 4u : 0u;
                    d.cars.push_back(c);
                }
                connection_->send(net::id(net::Package::derail), net::encode_derail(d));
                std::cout << "net: our train " << id << ": " << off << " cars derailed, sent" << std::endl;
                own.derailed_sent = off;
            }
        }
        if (stopped) connection_->send(net::id(net::Package::train_stands), net::encode_train_id(id));
        own.moving = moving;
        own.announced = true;
        send_look(id, own, trains, false);
        send_sound(id, own, trains, now);
    }
}

void NetSession::send_sound(const std::uint32_t id, Owned& own, TrainView& trains, const double now) {
    // As rts1.exe (0x424488): every discrete trigger of the exterior sound,
    // and its variables at most every 200 ms when they changed. Slot 3.
    const auto sound = trains.take_sound(own.train);
    own.sound_sent.resize(sound.size());
    const bool values_due = now >= own.next_sound;
    for (std::size_t car = 0; car < sound.size(); ++car) {
        const auto& s = sound[car];
        for (const auto code : s.triggers) {
            net::SoundTrigger t;
            t.train = id;
            t.car = static_cast<std::uint32_t>(car);
            t.code = code;
            connection_->send(net::id(net::Package::sound_trigger), net::encode_sound_trigger(t));
            ++triggers_sent_;
        }
        if (!values_due) continue;
        std::array<float, 9> now_values{};
        std::copy(s.vars.begin(), s.vars.end(), now_values.begin());
        now_values[8] = s.travelled;
        if (now_values == own.sound_sent[car]) continue;
        own.sound_sent[car] = now_values;
        net::SoundValues v;
        v.train = id;
        v.car = static_cast<std::uint32_t>(car);
        v.vars = s.vars;
        v.travelled = s.travelled;
        // SMSObject +308; the DLL host does not read it yet, RTS starts it at 1.
        v.volume = 1.0f;
        connection_->send(net::id(net::Package::sound_values), net::encode_sound_values(v));
        ++values_sent_;
    }
    if (values_due) own.next_sound = now + 0.2;
}

void NetSession::send_look(const std::uint32_t id, Owned& own, TrainView& trains, const bool all) {
    if (!connection_ || own.train == 0) return;
    const auto look = trains.look_of(own.train);
    own.lights_sent.resize(look.size());
    own.looks_sent.resize(look.size(), {0xFF, false});
    const double t = now();
    const bool look_due = all || t >= own.next_look;
    for (std::size_t car = 0; car < look.size(); ++car) {
        const auto& c = look[car];
        auto& sent = own.lights_sent[car];
        if (sent.size() != c.lights.size()) sent.assign(c.lights.size(), 0);
        for (std::size_t k = 0; k < c.lights.size(); ++k) {
            const auto& l = c.lights[k];
            const std::uint8_t lit = l.on ? 1 : 0;
            // A new viewer of the train gets the lit ones; it starts with
            // every light off (rts1.exe sends all of them, 275 a car on an ED9M).
            if (all ? !l.on : sent[k] == lit) continue;
            net::CarLightState s;
            s.train = id;
            s.car = static_cast<std::uint32_t>(car);
            s.light = static_cast<std::uint32_t>(k);
            s.lit = l.on;
            if (l.state) {
                s.radius = l.state->radius;
                s.colour = l.state->colour;
                s.position = l.state->position;
            }
            connection_->send(net::id(net::Package::car_light), net::encode_car_light(s));
            sent[k] = lit;
        }
        if (!look_due) continue;
        auto& seen = own.looks_sent[car];
        if (!all && seen.first == c.pantographs && seen.second == c.running) continue;
        net::CarLook out;
        out.train = id;
        out.car = static_cast<std::uint32_t>(car);
        out.kind = c.kind;
        out.pantographs = c.pantographs;
        out.running = c.running;
        connection_->send(net::id(net::Package::car_state), net::encode_car_state(out));
        if (seen.first != c.pantographs) {
            std::cout << "net: our train " << id << " car " << car << ": pantographs 0x" << std::hex << int(c.pantographs) << std::dec
                      << (c.running ? ", running" : "") << std::endl;
        }
        seen = {c.pantographs, c.running};
    }
    if (look_due) own.next_look = t + 0.25;
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
