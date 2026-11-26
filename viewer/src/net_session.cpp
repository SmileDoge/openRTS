#include "net_session.hpp"

#include <openrts/net/placement.hpp>
#include <openrts/core/profile.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include <zlib.h>

#include <openrts/dllhost/protocol.hpp>

#include <openrts/net/packages.hpp>
#include <openrts/net/play.hpp>
#include <openrts/simulation/signals.hpp>
#include <openrts/simulation/track_network.hpp>
#include <openrts/simulation/train.hpp>

#include "train_view.hpp"
#include "ui_text.hpp"

namespace openrts::viewer {

namespace {

std::optional<std::string> environment(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) return std::nullopt;
    std::string text(value);
    std::free(value);
    return text;
#else
    const char* value = std::getenv(name);
    return value ? std::optional<std::string>(value) : std::nullopt;
#endif
}

// OPENRTS_MP_HELP_SWITCH=<id>:<state>.
std::optional<std::pair<std::uint32_t, std::uint32_t>> test_switch() {
    static const auto value = [] () -> std::optional<std::pair<std::uint32_t, std::uint32_t>> {
        const auto text = environment("OPENRTS_MP_HELP_SWITCH");
        if (!text) return std::nullopt;
        const auto colon = text->find(':');
        if (colon == std::string::npos) return std::nullopt;
        return std::pair{static_cast<std::uint32_t>(std::stoul(text->substr(0, colon))),
                         static_cast<std::uint32_t>(std::stoul(text->substr(colon + 1)))};
    }();
    return value;
}

} // namespace


namespace {

constexpr std::uint16_t P(const net::Package package) noexcept { return net::id(package); }

// The keys a record's cars were given (none: never placed here).
std::vector<std::uint32_t> record_keys(const net::SnapshotTrain& train) {
    std::vector<std::uint32_t> keys;
    for (const auto& car : train.cars)
        if (car.key != 0) keys.push_back(car.key);
    return keys;
}

} // namespace

std::set<std::uint32_t> NetSession::record_keys_but(const net::SnapshotTrain& record) const {
    std::set<std::uint32_t> keys;
    const auto add = [&](const net::SnapshotTrain& other) {
        if (&other == &record) return;
        for (const auto& car : other.cars)
            if (car.key != 0) keys.insert(car.key);
    };
    for (const auto& [_, own] : owned_) add(own.record);
    for (const auto& [_, remote] : remotes_) add(remote.record);
    return keys;
}

namespace {

// The record's cars get their keys here once (TrainView::PlacedTrain::keys):
// a car keeps its key, and its DLL, through couplings and cuts. The key is
// the car of its server uid in the view's world (car_for_server_uid): a
// client that connects again finds the cars it already has.
TrainView::PlacedTrain placed_train(net::SnapshotTrain& train, const simulation::TrackLocation& front, TrainView& trains,
                                    std::set<std::uint32_t> taken) {
    TrainView::PlacedTrain p;
    for (auto& car : train.cars) {
        if (car.key == 0) car.key = trains.car_for_server_uid(car.uid, taken);
        taken.insert(car.key);
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
        entry.place_drag = static_cast<std::uint8_t>((car.flags >> 8) & 3u); // the chocks (0x100 / 0x200)
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
        if (train.names) names_[train.id] = {(*train.names)[0], (*train.names)[1]};
    }
}

const NetSession::TrainNames* NetSession::train_names(const std::uint32_t server_train) const {
    const auto it = names_.find(server_train);
    return it == names_.end() ? nullptr : &it->second;
}

std::uint32_t NetSession::train_number(const std::uint32_t server_train) const {
    if (console_state_) {
        for (const auto& t : console_state_->trains) {
            if (t.id == server_train) return t.number;
        }
    }
    return 0;
}

void NetSession::send_train_names(const std::uint32_t server_train, const std::u16string& name, const std::u16string& notes) {
    if (!connection_ || server_train == 0) return;
    net::WireWriter w;
    w.u32(server_train);
    w.text_u32(name);
    w.text_u32(notes);
    connection_->send(net::id(net::Package::train_names), w.data());
    console_log_.push_back("> train " + std::to_string(server_train) + ": name \"" + net::to_utf8(name) + "\", notes " +
                           std::to_string(notes.size()) + " characters");
}

void NetSession::send_command(const std::string_view line) {
    if (!connection_ || line.empty()) return;
    net::WireWriter w;
    w.text_u32(net::from_utf8(line));
    connection_->send(net::id(net::Package::console_command), w.data());
    console_log_.push_back("> " + std::string(line.size() > 120 ? line.substr(0, 120) : line) + (line.size() > 120 ? "..." : ""));
}

void NetSession::send_place(const net::ConsolePlace& place) {
    if (!connection_ || place.cars.empty()) return;
    connection_->send(net::id(net::Package::console_place), net::encode_console_place(place));
    console_log_.push_back("> " + net::to_utf8(place.consist) + ": " + std::to_string(place.cars.size()) + " cars");
}

void NetSession::ask_next_routes(const std::uint32_t train, const bool back) {
    if (!connection_ || train == 0) return;
    connection_->send(net::id(net::Package::console_next), net::encode_console_next_ask({.train = train, .back = back}));
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

void NetSession::ask_junction(const std::uint32_t node, const std::uint32_t branch) {
    if (!connection_ || node == 0) return;
    net::WireWriter w;
    w.u32(node - 1);
    w.u32(branch);
    connection_->send(net::id(net::Package::junction_request), w.data());
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
            if (const auto error = trains.place_driven(placed_train(o.record, *front, trains, record_keys_but(o.record))); !error.empty()) {
                std::cout << "net: our train " << train.id << ": " << error << std::endl;
                continue;
            }
            if (const auto* driven = trains.train()) {
                o.train = driven->id();
                trains.set_unit_start_state(o.train, train.state == 0 ? 0 : train.state == 1 ? 1 : -1);
                trains.set_category(o.train, category_of(train.subtype));
                trains.share_sound(o.train, true);
                trains.set_boardable(o.train, true);
                own = o.train;
                ++placed;
                std::cout << "net: train " << train.id << " \"" << net::train_name(train) << "\" is ours: driven here, "
                          << train.cars.size() << " cars" << std::endl;
                put_back_state(train.id, trains);
                apply_record_cargo(train.cargo, trains);
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
        // No train of ours (none given yet): free by the first train, not
        // riding with it as if it were ours; the train comes with 0x4601.
        if (own == 0 && !console_) trains.look_once_then_free();
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
    // Our couplings and cuts of the last step first, while the trains taken
    // in are still known by their place here: move_trains forgets a remote
    // train that is no longer on the track, and the coupling went unsent
    // (the server and its consoles kept the other train and dropped every
    // position of ours with the cars taken in; testers 2026-11-15).
    send_couplings(trains);
    // The switches our trains set as they run (a run-through, back to
    // MainRoute behind the tail): ours to set and to tell the server, as
    // rts1.exe does (0x460A, 0x4CBFC4); the others' trains are their
    // owners' (the server tells us).
    const auto wanted = network.take_wanted_switches();
    std::set<std::uint32_t> under_others;
    if (!wanted.empty()) {
        for (const auto& [_, remote] : remotes_) {
            if (remote.train == 0) continue;
            if (const auto* train = trains.find_train(remote.train))
                for (const auto j : net::junctions_under(network, *train)) under_others.insert(j);
        }
    }
    for (const auto& w : wanted) {
        const bool ours = std::any_of(owned_.begin(), owned_.end(), [&](const auto& o) { return o.second.train != 0 && o.second.train == w.owner; });
        if (!ours || console_) continue;
        // Not under another player's train, nor just in front of it.
        if (under_others.contains(w.node)) continue;
        auto* state = network.junction(w.node);
        if (!state || state->selected == w.branch) continue;
        state->selected = w.branch;
        request_junction(w.node, w.branch);
    }
    // Switches thrown by hand on foot: the server decides.
    for (const auto& [node, branch] : trains.take_junction_throws()) ask_junction(node, branch);
    // openRTS's own packages go to an openRTS server only: it answers this
    // (rts_server.exe leaves an id it does not know to its main thread).
    if (connection_ && !hello_sent_ && !console_) {
        hello_sent_ = true;
        connection_->send(net::id(net::Package::openrts_hello), net::encode_openrts_hello());
    }
    if (connection_) {
        for (const auto& package : connection_->receive()) {
            ++received_[package.id];
            apply(package, network, signals, trains, t);
        }
    }
    move_trains(network, trains, t);
    helper_streams(trains, t);
    if (!paused_) send_own(trains, t);
    send_states(trains, t);
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
                          const double now, const Timed* timed) {
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
        remote.train = trains.add_remote(placed_train(remote.record, *front, trains, record_keys_but(remote.record)), &error);
        if (remote.train == 0) {
            std::cout << "net: train " << remote.record.id << " (" << net::train_name(remote.record)
                      << ") does not fit: " << error << std::endl;
            return;
        }
        remote.motion = {};
        trains.set_category(remote.train, category_of(remote.record.subtype));
        apply_record_cargo(remote.record.cargo, trains);
        // Its lights, sound and look come from its owner on request, as
        // rts1.exe asks when it loads another player's train (0x4D0228).
        if (connection_) {
            connection_->send(net::id(net::Package::state_request), net::encode_state_request(2, remote.record.id));
        }
    }
    if (timed) {
        remote.motion.add_timed(network, timed->time, *front, pos.speed, timed->acceleration);
        // A package interval (0.1 s, up to a third more at the sender's
        // frame rate), how late they come lately, a frame or two.
        remote.motion.set_timed_delay(std::clamp(0.15 + remote.fast.late + 0.03, 0.15, 1.0));
        return;
    }
    if (now - remote.fast_heard < 2.0) return; // the timed ones move it
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
    case P(net::Package::openrts_train_motion):
        if (const auto m = net::decode_openrts_motion(body); m && !owned_.contains(m->train)) {
            const auto it = remotes_.find(m->train);
            if (it == remotes_.end() || it->second.record.cars.empty()) return;
            auto& remote = it->second;
            // The sender's clock on ours: its time unwrapped, our time at its
            // earliest arrival (the network's least delay), how late this one is.
            auto& c = remote.fast;
            const auto step = static_cast<std::int32_t>(m->sent_ms - c.last_ms);
            if (!c.set || step < -2000 || step > 60000 || now - remote.fast_heard > 10.0) {
                c = {};
                c.set = true;
                c.sent = m->sent_ms / 1000.0;
                c.least = c.base = now - c.sent;
            } else {
                const double gap = (std::max)(step / 1000.0, 0.0);
                c.sent += step / 1000.0;
                c.least = (std::min)(c.least + 0.0001 * gap, now - c.sent);
                c.base += std::clamp(c.least - c.base, -0.002 * gap, 0.002 * gap);
                // Up at once, down slowly (halved in some 14 s): the delay
                // drawn behind moves seldom.
                c.late = (std::max)(c.late * 0.995, now - c.sent - c.base);
            }
            c.last_ms = m->sent_ms;
            remote.fast_heard = now;
            net::TrainPos pos = remote.record.pos ? *remote.record.pos : net::TrainPos{};
            if (pos.cars.size() != remote.record.cars.size()) pos.cars.resize(remote.record.cars.size());
            pos.node = m->node;
            pos.offset = m->offset;
            pos.direction = m->direction;
            pos.speed = m->speed;
            pos.acceleration = m->acceleration;
            const Timed timed{.time = c.sent + c.base, .acceleration = m->acceleration};
            position(remote, pos, network, trains, now, &timed);
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
                if (remove->remove) {
                    // Deleted on the server: its cars are gone for good.
                    trains.release_cars(record_keys(mine->second.record));
                    owned_.erase(mine);
                }
                return;
            }
            const auto it = remotes_.find(remove->train);
            if (it == remotes_.end()) return;
            if (it->second.train != 0) trains.remove_train(it->second.train);
            it->second.train = 0;
            it->second.motion = {};
            std::cout << "net: train " << remove->train << (remove->remove ? " deleted" : " taken off the track")
                      << std::endl;
            if (remove->remove) {
                trains.release_cars(record_keys(it->second.record));
                remotes_.erase(it);
            }
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
    case P(net::Package::car_cargo):
        apply_cargo(body, trains);
        return;
    case P(net::Package::openrts_car_action):
        if (const auto a = net::decode_openrts_car_action(body)) {
            if (const auto view = view_train(a->train)) {
                trains.apply_car_action({.train = view, .car = a->car, .coupling = a->coupling, .front = a->front, .item = static_cast<int>(a->item)});
                std::cout << "net: train " << a->train << " car " << a->car + 1 << ": menu item " << a->item << " from another player" << std::endl;
            }
        }
        return;
    case P(net::Package::openrts_cab_doors):
        if (const auto d = net::decode_openrts_cab_doors(body)) {
            const std::uint32_t on = help_ ? help_->train : driven_id(trains);
            if (on != 0 && d->train == on) {
                trains.set_cab_doors(d->car, d->open);
                cab_doors_seen_[d->car] = d->open; // not told back
            }
        }
        return;
    case P(net::Package::openrts_doors):
        if (const auto d = net::decode_openrts_doors(body)) {
            if (const auto it = remotes_.find(d->train); it != remotes_.end() && it->second.train != 0)
                trains.set_remote_doors(it->second.train, d->groups);
        }
        return;
    case P(net::Package::openrts_train_state):
        if (const auto part = net::decode_train_state(body); part && part->parts <= 64) {
            auto& parts = state_parts_[part->train];
            if (part->part == 0) parts.assign(part->parts, {});
            if (parts.size() != part->parts) return;
            parts[part->part] = part->bytes;
            if (part->part + 1u != part->parts) return;
            net::Bytes packed;
            for (const auto& p : parts) packed.insert(packed.end(), p.begin(), p.end());
            state_parts_.erase(part->train);
            if (packed.size() < 4) return;
            std::uint32_t size = 0;
            std::memcpy(&size, packed.data(), 4);
            if (size > (64u << 20)) return;
            std::string text(size, '\0');
            uLongf length = size;
            if (uncompress(reinterpret_cast<Bytef*>(text.data()), &length, packed.data() + 4, static_cast<uLong>(packed.size() - 4)) != Z_OK || length != size)
                return;
            std::cout << "net: train " << part->train << ": its state from the server (" << size << " bytes)" << std::endl;
            pending_states_[part->train] = std::move(text);
            put_back_state(part->train, trains);
        }
        return;
    case P(net::Package::derail):
        if (const auto d = net::decode_derail(body)) {
            if (const auto it = remotes_.find(d->train); it != remotes_.end() && it->second.train != 0) {
                std::vector<std::array<float, 3>> rolls;
                for (const auto& c : d->cars) rolls.push_back({static_cast<float>(c.car), c.tip, c.tip_rest});
                trains.set_remote_derail(it->second.train, d->derailed, rolls);
                const auto off = std::count(d->derailed.begin(), d->derailed.end(), std::uint8_t{1});
                std::cout << "net: train " << d->train << ": " << off << " cars derailed" << std::endl;
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
    case P(net::Package::openrts_clock):
        if (const auto c = net::decode_openrts_clock(body)) {
            server_clock_ = *c;
            std::cout << "net: the server's clock x" << c->speed << ", season " << static_cast<int>(c->season) << std::endl;
        }
        return;
    case P(net::Package::openrts_weather):
        if (const auto w = net::decode_openrts_weather(body)) {
            server_weather_ = *w;
            ++weather_serial_;
        }
        return;
    case P(net::Package::console_reply): {
        net::WireReader r(body);
        const auto text = net::to_utf8(r.text_u32());
        if (!r.ok()) return;
        // A placement's file, or the list: kept, a line in the log.
        if (text.rfind(net::placement_header, 0) == 0 || text.rfind("openRTS server placement 1", 0) == 0) {
            std::string error;
            const auto count = net::read_placement(text, error).size();
            placement_text_ = text;
            console_log_.push_back("placement: " + std::to_string(count) + " trains");
            return;
        }
        if (text.rfind("placements:", 0) == 0) {
            placement_names_.clear();
            std::size_t from = text.find('\n');
            while (from != std::string::npos && from + 1 < text.size()) {
                const auto end = text.find('\n', from + 1);
                auto name = text.substr(from + 1, end == std::string::npos ? std::string::npos : end - from - 1);
                while (!name.empty() && name.back() == '\r') name.pop_back();
                if (!name.empty()) placement_names_.push_back(std::move(name));
                from = end;
            }
            console_log_.push_back("placements on the server: " + std::to_string(placement_names_.size()));
            return;
        }
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
    case P(net::Package::train_names): {
        // A train's name and notes (the console's train form), to everyone.
        net::WireReader r(body);
        const auto train = r.u32();
        auto name = r.text_u32();
        auto notes = r.text_u32();
        if (!r.ok()) return;
        if (name.empty() && notes.empty()) names_.erase(train);
        else names_[train] = {std::move(name), std::move(notes)};
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
    case P(net::Package::console_next_reply):
        if (auto next = net::decode_console_next(body)) next_routes_ = std::move(*next);
        return;
    case P(net::Package::couple):
        if (const auto c = net::decode_couple(body)) coupled(*c, network, trains);
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
    case P(net::Package::openrts_hello):
        if (!openrts_server_) std::cout << "net: the server is openRTS's: players and helpers" << std::endl;
        openrts_server_ = true;
        if (const auto features = net::decode_openrts_hello(body)) {
            const bool fast = (*features & net::openrts_feature_fast_positions) != 0;
            if (fast != fast_positions_)
                std::cout << (fast ? "net: fast positions: our trains' motion 10 times a second" : "net: fast positions off: as RTS, every 1.5 s")
                          << std::endl;
            // Off: the others' trains go by their RTS positions at once,
            // eased back to their delay (else they stood till those came).
            if (fast_positions_ && !fast) {
                for (auto& [_, remote] : remotes_) {
                    remote.motion.untimed();
                    remote.fast_heard = -1e9;
                }
            }
            fast_positions_ = fast;
        }
        if (net::openrts_hello_storm(body) && !server_storm_) std::cout << "net: the server's weather is a thunderstorm" << std::endl;
        server_storm_ = net::openrts_hello_storm(body);
        if (const auto discord = net::openrts_hello_discord(body)) {
            if (!discord_join_) std::cout << "net: the server may be joined from Discord" << std::endl;
            discord_join_ = DiscordJoin{.address = net::to_utf8(discord->address), .players = discord->players, .places = discord->places};
        } else {
            discord_join_.reset();
        }
        return;
    case P(net::Package::player_pos):
        if (auto other = net::decode_other_player(body)) {
            auto& p = players_[other->user];
            p.user = other->user;
            p.nick = net::to_utf8(other->nick);
            p.train = other->train;
            p.at = {.tile_x = other->tile_x, .tile_z = other->tile_z, .x = other->x, .y = other->y, .z = other->z};
            p.heard = now;
            auto& trail = player_trails_[other->user];
            trail.emplace_back(now, p.at);
            while (trail.size() > 2 && now - trail.front().first > 6.0) trail.pop_front();
        }
        return;
    case P(net::Package::helper):
        if (const auto note = net::decode_helper_note(body)) helper_note(*note, trains);
        return;
    case P(net::Package::helper_cab): {
        // The engineer's cab, for us his helper.
        const auto cab = net::decode_helper_cab(body);
        if (!cab || !help_ || help_->state != net::HelperState::helping || cab->train != help_->train || !helping_view_) return;
        if (cab->packed.size() < 4) return;
        std::uint32_t size{};
        std::memcpy(&size, cab->packed.data(), 4);
        if (size == 0 || size > (8u << 20)) return;
        std::vector<std::uint8_t> raw(size);
        uLongf length = size;
        if (uncompress(raw.data(), &length, cab->packed.data() + 4, static_cast<uLong>(cab->packed.size() - 4)) != Z_OK || length != size)
            return;
        dllhost::Reader r(raw.data(), size);
        dllhost::CarOutput out;
        dllhost::read(r, out);
        if (r.failed()) return;
        auto& seen = cab_seen_[cab->car];
        const auto tested = test_switch();
        const std::uint32_t state = tested && tested->first < out.switches.size() ? out.switches[tested->first].state : 0xFFFFFFFFu;
        if (seen.first++ == 0) {
            std::cout << "net: the engineer's cab of car " << cab->car + 1 << " came: " << out.switches.size() << " switches, "
                      << out.displays.size() << " displays, " << out.screens.size() << " screens, " << cab->packed.size() << " bytes packed"
                      << std::endl;
        } else if (seen.first % 100 == 0) {
            std::cout << "net: the engineer's cab of car " << cab->car + 1 << ": " << seen.first << " packages" << std::endl;
        }
        if (seen.first > 1 && tested && state != seen.second) {
            std::cout << "net: the engineer's cab of car " << cab->car + 1 << ": switch " << tested->first << " is at " << state << " now"
                      << std::endl;
        }
        seen.second = state;
        trains.set_helper_output(cab->car, std::move(out));
        if (tested && !test_switched_ && seen.first >= 10) {
            test_switched_ = true;
            std::cout << "net: test: the helper's hand puts switch " << tested->first << " at " << tested->second << std::endl;
            trains.helper_set_switch(tested->first, tested->second);
        }
        return;
    }
    case P(net::Package::helper_input): {
        // A helper's hand on our cab.
        const auto input = net::decode_helper_input(body);
        if (!input || helpers_.empty()) return;
        const auto own = owned_.find(input->train);
        if (own == owned_.end() || own->second.train == 0 || trains.train() == nullptr ||
            trains.train()->id() != own->second.train)
            return;
        switch (input->kind) {
        case net::HelperInputKind::watch:
            if (input->car != 0xFFFFFFFFu) {
                if (!cab_watched_.contains(input->car)) std::cout << "net: a helper is in car " << input->car + 1 << ": its cab goes to him" << std::endl;
                cab_watched_[input->car] = now + 3.0;
            }
            return;
        case net::HelperInputKind::operate:
            cab_due_ = 0.0; // its result to the helper with the next frame
            std::cout << "net: the helper's hand: car " << input->car + 1 << ", switch " << input->id << " to " << input->state << ": "
                      << trains.helper_hand({.kind = TrainView::HelperHand::Kind::operate, .car = input->car, .id = input->id, .state = input->state})
                      << std::endl;
            return;
        case net::HelperInputKind::drag:
            cab_due_ = 0.0;
            (void)trains.helper_hand({.kind = TrainView::HelperHand::Kind::drag, .car = input->car, .id = input->id, .delta = input->delta,
                                .release = input->release});
            return;
        }
        return;
    }
    default:
        ++not_handled_[package.id];
        return;
    }
}

std::string NetSession::train_label(const std::uint32_t server_train) const {
    if (const auto it = remotes_.find(server_train); it != remotes_.end()) return net::train_name(it->second.record);
    if (const auto it = owned_.find(server_train); it != owned_.end()) return net::train_name(it->second.record);
    return std::to_string(server_train);
}

std::vector<NetSession::Player> NetSession::players() const {
    std::vector<Player> out;
    const double t = now();
    for (const auto& [_, p] : players_) {
        if (t - p.heard > 20.0) continue;
        out.push_back(p);
        // A player in another client's train: his place as far back as the
        // train is drawn (RemoteMotion::delay, a package interval and a
        // margin), so his name rides with it. Shown as heard, it ran ahead
        // of the train by its speed x 1.75 s (testers 2026-11-18: 30 m at
        // 60 km/h).
        const auto remote = p.train != 0 ? remotes_.find(p.train) : remotes_.end();
        const auto trail = player_trails_.find(p.user);
        if (remote == remotes_.end() || remote->second.train == 0 || trail == player_trails_.end() || trail->second.empty()) continue;
        const double when = t - remote->second.motion.delay();
        const auto& samples = trail->second;
        if (when <= samples.front().first) {
            out.back().at = samples.front().second;
            continue;
        }
        for (std::size_t k = 1; k < samples.size(); ++k) {
            if (samples[k].first < when) continue;
            const auto& [ta, a] = samples[k - 1];
            const auto& [tb, b] = samples[k];
            const double s = tb > ta ? (when - ta) / (tb - ta) : 1.0;
            // Across tiles: b in a's tile, then the share of the way.
            const double bx = (b.tile_x - a.tile_x) * 2048.0 + b.x, bz = (b.tile_z - a.tile_z) * 2048.0 + b.z;
            out.back().at = {.tile_x = a.tile_x, .tile_z = a.tile_z, .x = a.x + (bx - a.x) * s, .y = a.y + (b.y - a.y) * s, .z = a.z + (bz - a.z) * s};
            break;
        }
    }
    return out;
}

void NetSession::ask_to_help(const std::uint32_t server_train) {
    if (!connection_ || !openrts_server_ || server_train == 0) return;
    connection_->send(net::id(net::Package::helper), net::encode_helper_note({.kind = net::HelperKind::ask, .train = server_train}));
}

void NetSession::stop_helping() {
    if (!connection_ || !help_) return;
    connection_->send(net::id(net::Package::helper), net::encode_helper_note({.kind = net::HelperKind::ask, .train = 0}));
}

void NetSession::answer(const std::uint32_t user, const bool take) {
    const auto it = std::find_if(asking_.begin(), asking_.end(), [&](const Helper& h) { return h.user == user; });
    if (it == asking_.end() || !connection_) return;
    connection_->send(net::id(net::Package::helper),
                      net::encode_helper_note({.kind = net::HelperKind::answer, .train = it->train, .user = user, .value = take ? 1u : 0u}));
    asking_.erase(it);
}

void NetSession::drop_helper(const std::uint32_t user) {
    const auto it = std::find_if(helpers_.begin(), helpers_.end(), [&](const Helper& h) { return h.user == user; });
    if (it == helpers_.end() || !connection_) return;
    connection_->send(net::id(net::Package::helper),
                      net::encode_helper_note({.kind = net::HelperKind::drop, .train = it->train, .user = user}));
}

void NetSession::helper_note(const net::HelperNote& note, TrainView& trains) {
    using net::HelperKind;
    using net::HelperState;
    const auto nick = net::to_utf8(note.nick);
    switch (note.kind) {
    case HelperKind::asked:
        // A player asks to help us.
        std::erase_if(asking_, [&](const Helper& h) { return h.user == note.user; });
        asking_.push_back({.user = note.user, .train = note.train, .nick = nick});
        std::cout << "net: " << nick << " asks to be the helper on train " << note.train << std::endl;
        if (environment("OPENRTS_MP_TAKE_HELPERS")) answer(note.user, true);
        return;
    case HelperKind::helper:
        std::erase_if(asking_, [&](const Helper& h) { return h.user == note.user; });
        std::erase_if(helpers_, [&](const Helper& h) { return h.user == note.user; });
        if (note.value != 0) helpers_.push_back({.user = note.user, .train = note.train, .nick = nick});
        std::cout << "net: " << nick << (note.value != 0 ? " helps on train " : " helps no more on train ") << note.train << std::endl;
        if (helpers_.empty()) {
            cab_sent_.clear();
            cab_watched_.clear();
        }
        return;
    case HelperKind::helping: {
        const auto state = static_cast<HelperState>(note.value);
        help_ = Help{.train = note.train, .engineer = note.user, .nick = nick, .state = state};
        std::cout << "net: help on train " << note.train << " (" << nick << "): "
                  << (state == HelperState::waiting   ? "asked"
                      : state == HelperState::helping ? "helping"
                      : state == HelperState::refused ? "refused"
                      : state == HelperState::cannot  ? "not possible"
                                                      : "ended")
                  << std::endl;
        if (state == HelperState::helping) {
            const auto view = view_train(note.train);
            const auto problem = view ? trains.help(view) : std::string("the train is not here");
            helping_view_ = problem.empty();
            if (!problem.empty()) std::cout << "net: cannot help here: " << problem << std::endl;
        } else if (helping_view_) {
            helping_view_ = false;
            trains.stop_helping();
        }
        return;
    }
    default:
        return;
    }
}

void NetSession::helper_streams(TrainView& trains, const double t) {
    if (!connection_ || paused_) return;
    if (!test_asked_ && openrts_server_) {
        static const auto nick = environment("OPENRTS_MP_HELP");
        if (!nick) {
            test_asked_ = true;
        } else {
            for (const auto& p : players()) {
                if (p.nick != *nick || p.train == 0) continue;
                test_asked_ = true;
                std::cout << "net: test: asking " << p.nick << " to take us as the helper on train " << p.train << std::endl;
                ask_to_help(p.train);
            }
        }
    }
    // The helper: where he is, and what his hands did.
    if (helping_view_ && help_ && trains.helping() != 0) {
        for (const auto& hand : trains.take_helper_hands()) {
            net::HelperInput input{.train = help_->train, .car = static_cast<std::uint32_t>(hand.car), .id = hand.id, .state = hand.state,
                                   .delta = hand.delta, .release = hand.release};
            input.kind = hand.kind == TrainView::HelperHand::Kind::operate ? net::HelperInputKind::operate : net::HelperInputKind::drag;
            connection_->send(net::id(net::Package::helper_input), net::encode_helper_input(input));
        }
        if (t >= watch_due_) {
            watch_due_ = t + 1.0;
            const auto car = trains.helper_car();
            net::HelperInput input{.kind = net::HelperInputKind::watch, .train = help_->train,
                                   .car = car == SIZE_MAX ? 0xFFFFFFFFu : static_cast<std::uint32_t>(car)};
            connection_->send(net::id(net::Package::helper_input), net::encode_helper_input(input));
        }
    } else if (helping_view_ && trains.helping() == 0) {
        // He left the train here (drove one of his own): the help ends.
        helping_view_ = false;
        stop_helping();
    }
    // The engineer: the cabs his helpers are in, twenty times a second while
    // they change, every second anyway; at once after a helper's hand
    // (testers 2026-11-21: the helper's buttons answered late).
    if (helpers_.empty() || t < cab_due_) return;
    cab_due_ = t + 0.05;
    const auto driven = driven_id(trains);
    if (driven == 0) return;
    bool helped = false;
    for (const auto& h : helpers_) helped = helped || h.train == driven;
    if (!helped) return;
    std::erase_if(cab_watched_, [&](const auto& entry) {
        if (entry.second >= t) return false;
        std::cout << "net: no helper in car " << entry.first + 1 << " now" << std::endl;
        return true;
    });
    static std::vector<std::uint8_t> buffer(4u << 20);
    for (const auto& [car, _] : cab_watched_) {
        const auto out = trains.helper_output(car);
        if (!out) continue;
        if (const auto tested = test_switch(); tested && tested->first < out->switches.size()) {
            static std::uint32_t last = 0xFFFFFFFFu;
            const auto& sw = out->switches[tested->first];
            if (sw.state != last) std::cout << "net: test: our cab's switch " << tested->first << " is at " << sw.state << " (set " << sw.set_state << ")" << std::endl;
            last = sw.state;
        }
        dllhost::Writer w(buffer.data(), static_cast<std::uint32_t>(buffer.size()));
        dllhost::write(w, *out);
        if (w.overflow()) continue;
        net::Bytes packed(4 + compressBound(w.size()));
        const std::uint32_t size = w.size();
        std::memcpy(packed.data(), &size, 4);
        uLongf length = static_cast<uLongf>(packed.size() - 4);
        if (compress2(packed.data() + 4, &length, buffer.data(), size, Z_BEST_SPEED) != Z_OK) continue;
        packed.resize(4 + length);
        auto& sent = cab_sent_[car];
        if (sent.packed == packed && t - sent.at < 1.0 && out->sound_cab.empty()) continue;
        // The server takes packages up to 0x3C00 bytes.
        if (packed.size() > 0x3B00) {
            std::cout << "net: the cab of car " << car + 1 << " is too big for a package (" << packed.size() << " bytes)" << std::endl;
            continue;
        }
        sent = {packed, t};
        connection_->send(net::id(net::Package::helper_cab), net::encode_helper_cab({.train = driven, .car = car, .packed = std::move(packed)}));
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
        if (!own) {
            std::cout << "net: a coupling here (train " << c.survivor << " took " << c.absorbed
                      << " in) is none of our trains': not sent" << std::endl;
            continue;
        }
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
        if (b == 0) {
            std::cout << "net: our train " << a << " took in a train the server does not know (" << c.absorbed
                      << " here): not sent" << std::endl;
            continue;
        }
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

void NetSession::coupled(const net::Coupling& c, simulation::TrackNetwork& network, TrainView& trains) {
    if (const auto mine = owned_.find(c.a); mine != owned_.end()) {
        // Our train took B in on the server: its own coupling, a LOOSE train
        // run into ours (rts_server.exe 0x40DCDC -> 0x4501E0, sent to
        // everyone, us too). B's cars join ours here as there, or the server
        // drops every position of ours from now on (the car count).
        auto& own = mine->second;
        std::vector<net::TrainCar> b_cars;
        std::uint64_t b_train = 0;
        if (const auto b = remotes_.find(c.b); b != remotes_.end()) {
            b_cars = b->second.record.cars;
            b_train = b->second.train;
            remotes_.erase(b);
        } else if (const auto other = owned_.find(c.b); other != owned_.end() && other != mine) {
            b_cars = other->second.record.cars;
            b_train = other->second.train;
            owned_.erase(other);
        } else {
            std::cout << "net: the server put train " << c.b << " into our train " << c.a << ": no such train here" << std::endl;
            return;
        }
        const bool at_front = c.end_a == 1;
        const bool other_front = c.end_b == 1;
        auto* ours = own.train != 0 ? trains.find_train(own.train) : nullptr;
        auto* theirs = b_train != 0 ? trains.find_train(b_train) : nullptr;
        double gap = 0.0;
        if (ours && theirs) {
            const auto end_a = at_front ? ours->front_end() : ours->rear_end();
            const auto end_b = other_front ? theirs->front_end() : theirs->rear_end();
            if (end_a && end_b) {
                const simulation::TrackLocation from{end_a->node(), end_a->offset(), at_front ? end_a->forward() : !end_a->forward()};
                const simulation::TrackLocation to{end_b->node(), end_b->offset(), other_front ? !end_b->forward() : end_b->forward()};
                if (const auto d = net::track_distance(network, from, to, 50.0)) gap = (std::max)(0.0, *d);
            }
        }
        if (ours && theirs && trains.join_trains(own.train, b_train, at_front, other_front, gap)) {
            own.record.cars = net::coupled_cars(own.record.cars, b_cars, c.end_a, c.end_b);
            own.announced = false;
            std::cout << "net: the server put train " << c.b << " into our train " << c.a << ": " << own.record.cars.size()
                      << " cars" << std::endl;
        } else {
            if (b_train != 0) trains.remove_train(b_train);
            std::cout << "net: the server put train " << c.b << " into our train " << c.a
                      << ", which cannot take it in here: the server will drop our positions" << std::endl;
        }
        return;
    }
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
    if (const auto mine = owned_.find(u.train); mine != owned_.end()) {
        // Someone else cut our train (another player at its joint): the
        // server made the part train u.part, ours as the train was
        // (0x44FE34 -> 0x456958). The same cut here, or our positions carry
        // cars the server no longer gives the train and are dropped.
        auto& own = mine->second;
        if (own.train == 0 || u.car == 0 || u.car >= own.record.cars.size()) return;
        const auto halves = trains.cut_train(own.train, u.car);
        if (!halves) {
            std::cout << "net: train " << u.train << " of ours cut before car " << u.car << " on the server: not here" << std::endl;
            return;
        }
        auto [stay, part_cars] = net::cut_cars(own.record.cars, u.car, u.side);
        // Side 0: the part is the cars from the cut on (the rear half);
        // side 1 the cars before it.
        const auto [front_train, rear_train] = *halves;
        Owned part;
        part.record = own.record;
        part.record.id = u.part;
        part.record.cars = std::move(part_cars);
        part.record.pos.reset();
        part.train = u.side == 0 ? rear_train : front_train;
        own.record.cars = std::move(stay);
        own.train = u.side == 0 ? front_train : rear_train;
        own.announced = false;
        trains.share_sound(part.train, true);
        trains.set_boardable(part.train, true);
        trains.set_category(part.train, category_of(part.record.subtype));
        std::cout << "net: our train " << u.train << " cut before car " << u.car << " on the server: the part is train "
                  << u.part << " (" << part.record.cars.size() << " cars), ours too" << std::endl;
        owned_[u.part] = std::move(part);
        return;
    }
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
    own.train = trains.add_local(placed_train(own.record, *front, trains, record_keys_but(own.record)), &error);
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
    trains.set_unit_start_state(own.train, own.record.state == 0 ? 0 : own.record.state == 1 ? 1 : -1);
    if (drive) {
        const auto state = own.record.state;
        trains.set_start_state(state == 0 ? 0 : state == 1 ? 1 : -1);
        if (const auto problem = trains.board(own.train); !problem.empty()) {
            std::cout << "net: our train " << id << ": " << problem << std::endl;
        }
    }
    std::cout << "net: train " << id << " \"" << net::train_name(own.record) << "\" is ours: "
              << (drive ? "driven here" : "standing here") << ", " << own.record.cars.size() << " cars" << std::endl;
    put_back_state(id, trains);
    apply_record_cargo(own.record.cargo, trains);
    return true;
}

// Our train's own state, every 30 s while it is ours here (0x45FB): the
// server keeps it, and gives it back when the train is ours again.
void NetSession::send_states(TrainView& trains, const double t) {
    if (!connection_ || console_ || !openrts_server_) return;
    // Cargo loaded here, on any train (0x461C, openRTS's body): the server
    // keeps each car's last for those who join.
    for (const auto& change : trains.take_cargo_changes(t)) {
        const auto sid = server_train(change.train);
        if (!sid) continue;
        net::OpenrtsCarCargo cargo{.train = sid, .car = static_cast<std::uint32_t>(change.car)};
        for (const auto& load : change.loads) {
            cargo.loads.push_back({.cargo = load.cargo,
                                   .object = load.object,
                                   .area = load.area,
                                   .volume = load.volume,
                                   .mass = load.mass,
                                   .count = load.count,
                                   .position = load.position,
                                   .extent = load.extent});
        }
        connection_->send(net::id(net::Package::car_cargo), net::encode_openrts_car_cargo(cargo));
    }
    // Car menu items done here, on any train (0x45FE).
    for (const auto& a : trains.take_car_actions()) {
        if (const auto sid = server_train(a.train))
            connection_->send(net::id(net::Package::openrts_car_action),
                              net::encode_openrts_car_action({.train = sid, .car = static_cast<std::uint32_t>(a.car), .coupling = a.coupling,
                                                              .front = a.front, .item = static_cast<std::uint32_t>(a.item)}));
    }
    // The doors of the cab we are in, when they change (0x45FD): the
    // engineer's and his helpers' alike.
    {
        const std::uint32_t on = help_ ? help_->train : driven_id(trains);
        if (on != cab_doors_train_) {
            cab_doors_train_ = on;
            cab_doors_seen_.clear();
            cab_doors_view_.clear();
        }
        if (on != 0) {
            for (auto& d : trains.cab_doors()) {
                auto& seen = cab_doors_seen_[d.car];
                // A view made anew (he came in, the view changed): its doors
                // are as last told, not a change to tell.
                if (auto& view = cab_doors_view_[d.car]; view != d.cab) {
                    view = d.cab;
                    if (!seen.empty() && seen != d.open) trains.set_cab_doors(d.car, seen);
                    else seen = d.open;
                    continue;
                }
                if (seen.empty()) {
                    seen = d.open; // as it stood when we came in: nothing to tell
                    continue;
                }
                if (seen == d.open) continue;
                seen = d.open;
                connection_->send(net::id(net::Package::openrts_cab_doors),
                                  net::encode_openrts_cab_doors({.train = on, .car = static_cast<std::uint32_t>(d.car), .open = d.open}));
            }
        }
    }
    // The passenger doors, when they change (0x45FC; RTS shares none).
    for (auto& [id, own] : owned_) {
        if (own.train == 0 || own.waiting) continue;
        auto doors = trains.door_groups(own.train);
        if (doors.empty() || doors == own.doors_sent) continue;
        own.doors_sent = doors;
        connection_->send(net::id(net::Package::openrts_doors), net::encode_openrts_doors({.train = id, .groups = std::move(doors)}));
    }
    const std::uint32_t driven = driven_id(trains);
    for (auto& [id, own] : owned_) {
        if (own.train == 0 || own.waiting) continue;
        if (own.ours_since < 0.0) own.ours_since = t;
        // States of it kept here for this server, less than a day old: the
        // player chooses one or none (a window), a few seconds on (the
        // server's own is in by then).
        if (!own.offered && id == driven && t - own.ours_since >= 3.0) {
            own.offered = true;
            if (!kept_states(trains).empty()) {
                states_offer_ = true;
                std::cout << "net: train " << id << ": states of it kept here, offered" << std::endl;
            }
        }
        if (restore_asked_ && id == driven) {
            std::ifstream in(*restore_asked_, std::ios::binary);
            if (in) {
                std::ostringstream text;
                text << in.rdbuf();
                trains.restore_own_state(own.train, text.str());
                own.next_state = t + 10.0;
                own.ours_since = t; // nothing kept over it till it is in
                std::cout << "net: train " << id << ": state " << restore_asked_->filename().string() << " put back by hand" << std::endl;
            }
            restore_asked_.reset();
        }
        if (t < own.next_state) continue;
        own.next_state = t + 30.0;
        const auto text = trains.own_state(own.train);
        if (text.empty()) continue;
        // Not while the kept ones are offered: the state now may be the one to be replaced.
        if (id == driven && !states_offer_ && t - own.ours_since >= 40.0) {
            const bool dated = t >= own.next_keep;
            if (dated) own.next_keep = t + 300.0;
            keep_state(own, trains, text, t, dated);
        }
        net::Bytes packed(4 + compressBound(static_cast<uLong>(text.size())));
        const auto size = static_cast<std::uint32_t>(text.size());
        std::memcpy(packed.data(), &size, 4);
        uLongf length = static_cast<uLongf>(packed.size() - 4);
        if (compress2(packed.data() + 4, &length, reinterpret_cast<const Bytef*>(text.data()), size, Z_BEST_SPEED) != Z_OK) continue;
        packed.resize(4 + length);
        for (const auto& part : net::encode_train_state(id, packed)) connection_->send(net::id(net::Package::openrts_train_state), part);
    }
}

// A state the server gave back, into our train once it is on the track here.
void NetSession::apply_cargo(const net::ByteView body, TrainView& trains) {
    const auto cargo = net::decode_openrts_car_cargo(body);
    if (!cargo) return;
    if (const auto view = view_train(cargo->train)) {
        std::vector<simulation::CarLoad> loads;
        for (const auto& load : cargo->loads) {
            loads.push_back({.cargo = load.cargo,
                             .object = load.object,
                             .area = load.area,
                             .volume = load.volume,
                             .mass = load.mass,
                             .count = load.count,
                             .position = load.position,
                             .extent = load.extent});
        }
        trains.apply_remote_cargo(view, cargo->car, std::move(loads));
        return;
    }
    // Not placed here yet: kept with its record, put on it when it is.
    if (const auto it = remotes_.find(cargo->train); it != remotes_.end()) {
        auto& kept = it->second.record.cargo;
        std::erase_if(kept, [&](const net::Bytes& b) {
            const auto other = net::decode_openrts_car_cargo(b);
            return other && other->car == cargo->car;
        });
        kept.emplace_back(body.begin(), body.end());
    }
}

void NetSession::apply_record_cargo(const std::vector<net::Bytes>& bodies, TrainView& trains) {
    for (const auto& body : bodies) apply_cargo(body, trains);
}

void NetSession::put_back_state(const std::uint32_t id, TrainView& trains) {
    const auto it = pending_states_.find(id);
    if (it == pending_states_.end()) return;
    const auto own = owned_.find(id);
    if (own == owned_.end() || own->second.train == 0) return;
    trains.restore_own_state(own->second.train, it->second);
    pending_states_.erase(it);
    // Not sent back at once: the state put back is kept by the server already.
    own->second.next_state = 0.0;
}

std::filesystem::path NetSession::kept_states_folder(const Owned& own, const TrainView& trains) const {
    // FNV-1a over the cars' files: the same name with other cars is another train.
    std::uint64_t hash = 1469598103934665603ull;
    for (const auto& car : own.record.cars) {
        for (const char16_t c : car.file) {
            hash ^= static_cast<std::uint64_t>(c);
            hash *= 1099511628211ull;
        }
        hash ^= 0x2Cu;
        hash *= 1099511628211ull;
    }
    std::string name = net::train_name(own.record);
    for (auto& c : name)
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || (c >= 0 && c < 32)) c = '_';
    if (name.size() > 60) name.resize(60);
    char tail[24];
    std::snprintf(tail, sizeof tail, "_%08llx", static_cast<unsigned long long>(hash & 0xFFFFFFFFull));
    const std::u8string folder(reinterpret_cast<const char8_t*>(name.data()), name.size());
    std::string server = server_key_.empty() ? std::string("server") : server_key_;
    for (auto& c : server)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-')) c = '_';
    return trains.train_states_folder() / server
           / (std::filesystem::path(folder).u8string() + std::u8string(reinterpret_cast<const char8_t*>(tail)));
}

void NetSession::keep_state(const Owned& own, const TrainView& trains, const std::string& text, const double t, const bool dated) {
    static_cast<void>(t);
    std::error_code error;
    const auto folder = kept_states_folder(own, trains);
    std::filesystem::create_directories(folder, error);
    if (error) return;
    const auto write = [&](const std::filesystem::path& file) {
        const auto temporary = std::filesystem::path(file).concat(".tmp");
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            if (!out) return;
            out << text;
        }
        std::error_code e;
        std::filesystem::rename(temporary, file, e);
    };
    write(folder / "last.state");
    if (!dated) return;
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char name[48];
    std::strftime(name, sizeof name, "%Y-%m-%d_%H-%M-%S.state", &local);
    write(folder / name);
    // The newest 12 dated ones kept.
    std::vector<std::filesystem::path> dated_files;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error))
        if (entry.path().extension() == ".state" && entry.path().filename() != "last.state") dated_files.push_back(entry.path());
    std::sort(dated_files.begin(), dated_files.end());
    for (std::size_t i = 0; i + 12 < dated_files.size(); ++i) std::filesystem::remove(dated_files[i], error);
}

std::vector<NetSession::KeptState> NetSession::kept_states(const TrainView& trains) const {
    std::vector<KeptState> out;
    const auto it = owned_.find(driven_id(trains));
    if (it == owned_.end()) return out;
    const auto folder = kept_states_folder(it->second, trains);
    std::error_code error;
    // Newest first, less than a day old (an older one is of another game).
    std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> files;
    const auto now = std::filesystem::file_time_type::clock::now();
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        if (entry.path().extension() != ".state") continue;
        std::error_code e;
        const auto when = std::filesystem::last_write_time(entry.path(), e);
        if (e || now - when > std::chrono::hours(24)) continue;
        files.emplace_back(when, entry.path());
    }
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    // A dated copy written with the last one is the same state: once.
    std::optional<std::filesystem::file_time_type> last;
    for (const auto& [when, file] : files)
        if (file.filename() == "last.state") last = when;
    for (const auto& [when, file] : files) {
        if (last && file.filename() != "last.state" && (when > *last ? when - *last : *last - when) < std::chrono::seconds(3)) continue;
        const auto time = std::chrono::system_clock::to_time_t(std::chrono::clock_cast<std::chrono::system_clock>(when));
        std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &time);
#else
        localtime_r(&time, &local);
#endif
        char text[32];
        std::strftime(text, sizeof text, "%d.%m %H:%M:%S", &local);
        std::string label = text;
        if (file.filename() == "last.state") label = std::string(tr("the last one")) + ", " + label;
        out.push_back({file, label});
    }
    return out;
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
            // Its DLLs start as the server keeps it, cold too (the host
            // takes the start state from the train on this way in).
            trains.set_unit_start_state(kept, placed.record.state == 0 ? 0 : placed.record.state == 1 ? 1 : -1);
            if (driven_id(trains) == 0) {
                trains.set_start_state(placed.record.state == 0 ? 0 : placed.record.state == 1 ? 1 : -1);
                if (const auto problem = trains.board(kept); !problem.empty()) std::cout << "net: " << problem << std::endl;
            }
            put_back_state(a.train, trains);
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
    // OPENRTS_NET_DRIVE=<m/s>:<from s>:<to s> (automated runs of the network
    // game): our first train sped up at 0.5 m/s² to that speed from that
    // time, slowed at 1 m/s² to a stop after the other.
    if (static const char* drive = std::getenv("OPENRTS_NET_DRIVE"); drive) {
        static double started = -1.0, last = 0.0;
        float speed = 10.0f;
        double from = 5.0, to = 60.0;
        std::sscanf(drive, "%f:%lf:%lf", &speed, &from, &to);
        for (auto& [id, own] : owned_) {
            auto* train = own.train != 0 ? trains.find_train(own.train) : nullptr;
            if (!train || !train->placed()) continue;
            if (started < 0.0) started = last = now;
            const double clock = now - started;
            const auto dt = static_cast<float>(now - last);
            last = now;
            const float v = train->cars().front().velocity;
            if (clock >= to) {
                train->add_velocity((v > 0.0f ? (std::max)(0.0f, v - dt) : (std::min)(0.0f, v + dt)) - v);
            } else if (clock >= from) {
                train->add_velocity((std::min)(speed, v + 0.5f * dt) - v);
            }
            break;
        }
    }
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
        // Its motion 10 times a second while it moves, and once when it
        // stops (the server's fast positions; docs/net.md).
        if (fast_positions_ && ((moving && now >= own.next_motion) || stopped)) {
            if (const auto pos = net::position_of(*train)) {
                net::OpenrtsMotion m;
                m.train = id;
                // When the train was there: its last step by the steps' own
                // clock, not this frame's (the steps are 0.05 s; a frame's
                // clock and the steps' drift apart: the samples' spacing
                // would not be the way the train went, ±3 %).
                const double at = trains.step_time();
                m.sent_ms = static_cast<std::uint32_t>(static_cast<std::uint64_t>((std::max)(at, 0.0) * 1000.0) & 0xFFFFFFFFull);
                m.node = pos->node;
                m.offset = pos->offset;
                m.direction = pos->direction;
                m.speed = moving ? pos->speed : 0.0f;
                m.acceleration = moving ? pos->acceleration : 0.0f;
                connection_->send(net::id(net::Package::openrts_train_motion), net::encode_openrts_motion(m));
                if (static const bool trace = std::getenv("OPENRTS_NET_TRACE") != nullptr; trace)
                    std::cout << "net trace sent: " << at << " train " << id << " node " << m.node << " offset " << m.offset << " speed "
                              << m.speed << " acceleration " << m.acceleration << std::endl;
                own.next_motion = std::max(own.next_motion + 0.1, now + 0.05);
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
        // Laid again from its head every 5 s: its cars behind follow the
        // junctions as they are here, and one set otherwise till the server
        // told it again left them on the wrong track (testers 2026-11-21).
        bool relay = false;
        if (now >= remote.next_relay) {
            remote.next_relay = now + 5.0;
            // Only when its tail, laid from its head along the junctions as
            // they are now, is not where it lies (nor while derailed: laying
            // it again puts its cars back on the rails).
            const auto rear = train->rear_end();
            const auto& cars = train->cars();
            const bool derailed = std::any_of(cars.begin(), cars.end(), [](const auto& c) { return c.derailed; });
            simulation::TrackTraveller laid(network, here.node, here.offset, here.forward);
            if (!derailed && rear && laid.move(-train->length()).ok()) {
                const bool apart = laid.node() != rear->node() || std::abs(laid.offset() - rear->offset()) > 1.0;
                if (apart) {
                    relay = true;
                    std::cout << "net: train " << id << " laid again from its head: its cars were on another track than the junctions here give"
                              << std::endl;
                }
            }
        }
        const auto d = relay ? std::optional<double>{} : net::track_distance(network, here, *target, 300.0);
        if (d) {
            if (std::abs(*d) > 1e-4) train->shift(*d);
        } else {
            std::string error;
            if (!train->place(network, target->node, target->offset, target->forward, &error)) {
                std::cout << "net: train " << id << ": " << error << std::endl;
            }
        }
        const float speed = remote.motion.speed_at(now);
        if (std::abs(train->speed() - speed) > 1e-3) train->add_velocity(speed - static_cast<float>(train->speed()));
        // OPENRTS_NET_TRACE=1 (automated runs): each frame, how the train
        // moved since the last and how far behind it is drawn.
        if (static const bool trace = std::getenv("OPENRTS_NET_TRACE") != nullptr; trace) {
            std::cout << "net trace: " << now << " train " << id << " moved " << (d ? *d : 0.0) << " speed " << speed << " delay "
                      << remote.motion.delay() << (remote.motion.timed() ? " timed" : " rts") << std::endl;
        }
    }
}

} // namespace openrts::viewer
