#include <openrts/server/game_server.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

#include <openrts/core/virtual_path.hpp>
#include <openrts/net/packages.hpp>
#include <openrts/net/play.hpp>
#include <openrts/net/world.hpp>
#include <openrts/simulation/signals.hpp>
#include <openrts/simulation/track_network.hpp>
#include <openrts/simulation/train.hpp>
#include <openrts/simulation/train_route.hpp>

namespace openrts::server {

namespace {

using net::Package;
constexpr std::uint16_t P(const Package p) { return net::id(p); }

constexpr std::uint16_t protocol_version = 101;
constexpr std::uint16_t kind_static = 0;
constexpr std::uint16_t kind_player = 1;
constexpr std::uint16_t kind_loose = 3;
// A train's sound, look and lights reach players within this of it
// (rts_server.exe audience 4, 0x442E10).
constexpr double look_range = 3000.0;
// Without a place for this long a train's extrapolation stops (rts1.exe: 5 s).
constexpr double place_timeout = 5.0;
// rts_server.exe lowers a standing train's state: "Включен" 480 s after it
// stopped, "Холодный" 1 200 s after (0x40DCDC, live capture 2026-11-05).
constexpr double warm_after = 480.0;
constexpr double cold_after = 1200.0;
// Flood limits per 5-s window (rts_server.exe's defaults: 1 000 packages a
// second, 500 sound packages a second).
constexpr double flood_window = 5.0;
constexpr std::uint32_t flood_all = 5000;
constexpr std::uint32_t flood_sound = 2500;

net::Bytes framed(const std::uint16_t id, const net::ByteView body) {
    net::WireWriter w;
    w.frame(id, body);
    return w.take();
}

template <class Fill>
net::Bytes package(const std::uint16_t id, Fill fill) {
    net::WireWriter w;
    w.begin(id);
    fill(w);
    w.end();
    return w.take();
}

net::Bytes code_only(const std::uint16_t code) {
    net::WireWriter w;
    w.u16(code);
    return w.take();
}

std::string utf8(const std::u16string_view text) { return net::to_utf8(text); }

// Absolute metres of a route place (x east, z north).
struct Flat {
    double x{}, z{};
};
Flat flat(const std::int32_t tile_x, const std::int32_t tile_z, const double x, const double z) {
    return {tile_x * 2048.0 + x, tile_z * 2048.0 + z};
}

} // namespace

struct GameServer::User {
    std::uint32_t id{};
    std::u16string nick;
    std::string address;
    bool console = false;      // logged in as the dispatcher's console (0x4500)
    ConnectionId connection{}; // 0: not connected
    bool playing = false;      // has the snapshot
    bool played = false;       // had it once: may come back with no train (RTS +92 bit 1)
    std::optional<net::Bytes> position; // the last player_pos body (44 bytes)
    double window_start{};
    std::uint32_t window_all{}, window_sound{};

    [[nodiscard]] std::optional<Flat> where() const {
        if (!position || position->size() < 28) return std::nullopt;
        net::WireReader r(*position);
        r.skip(12);
        const auto tile_x = r.i16();
        const auto tile_z = r.i16();
        const auto x = r.f32();
        r.f32();
        const auto z = r.f32();
        return flat(tile_x, tile_z, x, z);
    }
};

struct GameServer::Train {
    net::SnapshotTrain record;
    std::unique_ptr<simulation::Train> sim; // the server's copy on its network; null when it could not be made
    std::uint32_t owner{};                  // user id, 0 none
    std::uint32_t number{};
    double placed_at = -1.0;                // when record.pos last came (server clock)
    double stood_at = -1.0;                 // when it last came to a stand
};

struct GameServer::Impl {
    simulation::TrackNetwork& network;
    simulation::SignalSystem* signals;
    std::map<std::uint32_t, User> users;
    std::map<ConnectionId, std::uint32_t> by_connection; // logged in
    std::map<ConnectionId, std::string> fresh;           // connected, no login yet: address
    std::map<std::uint32_t, Train> trains;
    std::uint32_t next_user = 1;
    std::set<std::uint32_t> locked; // junction nodes the dispatcher locked
    // Routes the dispatcher set, by train: the leg and its junctions with
    // their distances along it, held until the train's rear has passed.
    struct HeldRoute {
        simulation::RouteLeg leg;
        std::vector<std::pair<std::uint32_t, double>> junctions; // node, distance
        bool rear_first = false; // laid from the rear (0x4619's flag)
        bool ended = false;      // the head has run past its end
        float sent = -2.0f;      // the distance left last sent (0x4619)
        double send_at = 0.0;
    };
    std::map<std::uint32_t, HeldRoute> routes;
    [[nodiscard]] std::optional<std::uint32_t> holder(const std::uint32_t node) const {
        for (const auto& [train, route] : routes) {
            for (const auto& [junction, _] : route.junctions) {
                if (junction == node) return train;
            }
        }
        return std::nullopt;
    }
    std::set<std::uint32_t> manual; // signal items the dispatcher holds
    struct Shown {
        float state{}, draw_state{}, speed{}, mode{};
        bool operator==(const Shown&) const = default;
    };
    std::vector<Shown> shown; // by head: what the players were last sent
    double now{};
    double last_tick = -1.0;
    double next_time = 0.0;
    double signal_step = 0.0; // unspent seconds of the 0.05-s signal step
    double next_console = 0.0; // the consoles' next console_state
    // Who asked a train's owner for its look (state_request), until when:
    // the answer reaches them wherever they are. A player's place is its
    // camera's (player_pos), not its train's: a client asks when it loads the
    // train, and its camera may then still be kilometres off (its first
    // place is where the camera started), so the 3-km rule dropped the answer.
    std::map<std::pair<std::uint32_t, std::uint32_t>, double> asked; // (train, user) -> until
    std::vector<std::string> log;

    Impl(simulation::TrackNetwork& n, simulation::SignalSystem* s) : network(n), signals(s) {}

    User* user_of(const ConnectionId connection) {
        const auto it = by_connection.find(connection);
        if (it == by_connection.end()) return nullptr;
        const auto user = users.find(it->second);
        return user == users.end() ? nullptr : &user->second;
    }
    User* user_named(const std::u16string_view nick) {
        for (auto& [_, user] : users) {
            if (user.nick == nick) return &user;
        }
        return nullptr;
    }
    Train* train(const std::uint32_t id) {
        const auto it = trains.find(id);
        return it == trains.end() ? nullptr : &it->second;
    }
    [[nodiscard]] std::uint32_t free_train_id() const {
        std::uint32_t id = 1;
        while (trains.contains(id)) ++id;
        return id;
    }
    void note(std::string line) { log.push_back(std::move(line)); }

    // The train's server copy put where its record says.
    void place_sim(Train& t) {
        if (!t.sim || !t.record.pos) return;
        std::string why;
        const auto front = net::train_front(network, t.record, &why);
        if (!front) {
            t.sim->vacate();
            return;
        }
        if (!t.sim->place(network, front->node, front->offset, front->forward, &why)) return;
        t.sim->add_velocity(t.record.pos->speed - static_cast<float>(t.sim->speed()));
    }

    // Whether `a` covers an end of `b` (their spans share track).
    [[nodiscard]] bool covers(const simulation::Train& a, const simulation::Train& b) {
        const auto rear = a.rear_end();
        const auto b_front = b.front_end();
        const auto b_rear = b.rear_end();
        if (!rear || !b_front || !b_rear) return false;
        const double length = a.length();
        const simulation::TrackLocation from{rear->node(), rear->offset(), rear->forward()}; // facing a's front
        for (const auto& end : {*b_front, *b_rear}) {
            for (const bool same : {true, false}) {
                const simulation::TrackLocation to{end.node(), end.offset(), same ? end.forward() : !end.forward()};
                if (const auto d = net::track_distance(network, from, to, length + 1.0); d && *d >= -0.5 && *d <= length + 0.5) return true;
            }
        }
        return false;
    }

    // Where a train is, for the players near it.
    [[nodiscard]] std::optional<Flat> where(const Train& t) const {
        if (!t.record.pos) return std::nullopt;
        const auto point = network.point(t.record.pos->node, t.record.pos->offset);
        if (!point) return std::nullopt;
        return flat(point->tile_x, point->tile_z, point->x, point->z);
    }

    [[nodiscard]] std::vector<net::JunctionState> junction_states() const {
        std::vector<net::JunctionState> out;
        for (std::uint32_t node = 1; node < network.node_count(); ++node) {
            const auto* n = network.node(node);
            const auto* state = network.junction(node);
            if (!n || n->kind != formats::TrackNodeKind::junction || !state) continue;
            out.push_back({.index = node - 1, .branch = state->selected, .lock = static_cast<std::uint8_t>(locked.contains(node) ? 1 : 0)});
        }
        return out;
    }

    [[nodiscard]] Shown shown_of(const simulation::SignalHead& head) const {
        // The mode field is the dispatcher's: 0 manual, 1 the head's own
        // logic, 2 automatic block (live capture 2026-11-05).
        return {.state = head.state, .draw_state = head.draw_state, .speed = head.speed,
                .mode = manual.contains(head.track_item) ? 0.0f : 1.0f};
    }

    [[nodiscard]] std::vector<net::SignalHeadState> head_states() const {
        std::vector<net::SignalHeadState> out;
        if (!signals) return out;
        for (const auto& head : signals->heads()) {
            const auto s = shown_of(head);
            out.push_back({.item = head.track_item, .semaphore = 0, .state = s.state, .draw_state = s.draw_state, .value_92 = 0.0f,
                           .mode = s.mode, .speed = s.speed});
        }
        return out;
    }
};

GameServer::GameServer(Transport& transport, ServerOptions options, simulation::TrackNetwork& network, simulation::SignalSystem* signals)
    : impl_(std::make_unique<Impl>(network, signals)), transport_(transport), options_(std::move(options)), time_(options_.time_of_day) {
    if (signals) {
        // The players' trains run the heads ahead of both ends (RTS train kinds 1 and 4).
        signals->set_player([this](const simulation::Train& train) {
            for (const auto& [_, t] : impl_->trains) {
                if (t.sim.get() == &train) return t.record.kind == kind_player;
            }
            return false;
        });
        signals->update({}, simulation::train_step);
        for (const auto& head : signals->heads()) impl_->shown.push_back(impl_->shown_of(head));
    }
}

GameServer::~GameServer() = default;

// ---- sending -----------------------------------------------------------------

namespace {

net::Bytes assign_package(const std::uint32_t train, const bool mine, const std::uint32_t number, const std::uint16_t subtype,
                          const std::uint16_t kind) {
    return package(P(Package::train_assign), [&](net::WireWriter& w) {
        w.u32(train);
        w.u8(mine ? 1 : 0);
        w.u32(number);
        w.u16(subtype);
        w.u16(kind);
    });
}

net::Bytes info_package(const net::SnapshotTrain& t, const std::uint32_t number) {
    return package(P(Package::train_info), [&](net::WireWriter& w) {
        w.u32(t.id);
        w.u16(t.kind);
        w.u16(t.subtype);
        w.u32(number);
        w.u8(t.state);
        w.u32(0); // no path
    });
}

net::Bytes junction_package(const net::JunctionState& j) {
    return package(P(Package::junction), [&](net::WireWriter& w) {
        w.u32(j.index);
        w.u32(j.branch);
        w.u8(j.lock);
    });
}

net::Bytes head_package(const net::SignalHeadState& h) {
    return package(P(Package::signal_head), [&](net::WireWriter& w) {
        w.u32(h.item);
        w.u16(h.semaphore);
        w.f32(h.state);
        w.f32(h.draw_state);
        w.f32(h.value_92);
        w.f32(h.mode);
        w.f32(h.speed);
    });
}

net::Bytes time_package(const double seconds) {
    return package(P(Package::time), [&](net::WireWriter& w) { w.f32(static_cast<float>(seconds)); });
}

// A train as it goes to players joining it in play: 0x4607, 0x4603, the
// names, 0x4606 (the snapshot's order).
net::Bytes train_packages(const net::SnapshotTrain& record) {
    net::Snapshot one;
    one.trains.push_back(record);
    one.trains.back().assign.reset();
    return net::write_snapshot(one);
}

net::Bytes pos_package(const net::SnapshotTrain& t) {
    return framed(P(Package::train_pos), net::encode_train_pos(t.id, *t.pos));
}

} // namespace

// ---- the transport's events ------------------------------------------------------

void GameServer::connected(const ConnectionId connection, std::string address) { impl_->fresh[connection] = std::move(address); }

void GameServer::closed(const ConnectionId connection) {
    auto& impl = *impl_;
    impl.fresh.erase(connection);
    auto* user = impl.user_of(connection);
    impl.by_connection.erase(connection);
    if (!user || user->connection != connection) return;
    impl.note("user " + utf8(user->nick) + " left");
    user->connection = 0;
    user->playing = false;
    user->position.reset();
    // Its trains stand where they are for the others (nobody drives them now).
    for (auto& [id, t] : impl.trains) {
        if (t.owner != user->id || t.record.stands || !t.record.pos) continue;
        t.record.stands = true;
        t.record.pos->speed = 0.0f;
        t.record.pos->acceleration = 0.0f;
        t.stood_at = impl.now;
        for (auto& [_, other] : impl.users) {
            if (!other.playing || !other.connection) continue;
            transport_.send(other.connection, framed(P(Package::train_stands), net::encode_train_id(id)));
        }
    }
}

void GameServer::received(const ConnectionId connection, const std::uint16_t id, const net::ByteView body) {
    auto& impl = *impl_;
    const auto send_to = [&](const User& user, net::Bytes bytes) {
        if (user.connection) transport_.send(user.connection, std::move(bytes));
    };
    const auto to_playing = [&](const net::Bytes& bytes, const std::uint32_t except) {
        for (auto& [uid, user] : impl.users) {
            if (uid != except && user.playing && user.connection) transport_.send(user.connection, bytes);
        }
    };

    // The login (rts_server.exe 0x4436C8): the first package fixes the kind.
    if (const auto fresh = impl.fresh.find(connection); fresh != impl.fresh.end()) {
        const auto address = fresh->second;
        impl.fresh.erase(fresh);
        const auto refuse = [&](const std::uint16_t code, const std::string& why) {
            transport_.send(connection, code_only(code));
            transport_.close(connection);
            impl.note(address + ": refused (" + why + ")");
        };
        const bool console = id == P(Package::console_login);
        if (!console && id != P(Package::login)) return refuse(P(Package::not_a_login), "not a login");
        net::WireReader r(body);
        const auto version = r.u16();
        const auto length = r.u16();
        std::u16string nick;
        for (std::uint16_t i = 0; i < length && r.ok(); ++i) nick.push_back(static_cast<char16_t>(r.u16()));
        if (!r.ok()) return refuse(P(Package::drop_bad), "a short login");
        if (version != protocol_version) return refuse(P(Package::bad_version), "protocol " + std::to_string(version));
        if (nick.empty() || nick.size() > 15) {
            transport_.close(connection);
            impl.note(address + ": refused (a nickname of " + std::to_string(nick.size()) + " characters)");
            return;
        }
        // A console: openRTS's (rts_server_DSP.exe expects 0x4501.. replies
        // that are not read yet). From this computer, or a nick allowed.
        if (console && !options_.consoles) return refuse(P(Package::not_admitted), "a dispatcher console: not taken yet");
        if (console) {
            const bool local = address.rfind("127.", 0) == 0 || address.rfind("::1", 0) == 0 || address.rfind("::ffff:127.", 0) == 0;
            bool allowed = local;
            for (const auto& name : options_.dispatchers) allowed = allowed || name == utf8(nick);
            if (!allowed) return refuse(P(Package::not_admitted), "a console that is not a dispatcher (--dispatcher)");
        }
        auto* user = impl.user_named(nick);
        if (user && user->console != console && user->connection) return refuse(P(Package::nick_busy), "the nick is taken");
        if (!user && !options_.admit && !console) return refuse(P(Package::not_admitted), "no new players");
        if (!user) {
            const auto uid = impl.next_user++;
            user = &impl.users[uid];
            user->id = uid;
            user->nick = nick;
        } else if (user->connection) {
            // The same name again: the new connection takes the user over
            // (rts_server.exe "User N reconnected"), the old one is closed.
            impl.by_connection.erase(user->connection);
            transport_.close(user->connection);
        }
        user->connection = connection;
        user->address = address;
        user->playing = false;
        user->console = console;
        impl.by_connection[connection] = user->id;

        net::WireWriter reply;
        reply.u16(P(Package::login));
        reply.u16(protocol_version);
        reply.text_u16(options_.description);
        reply.u8(options_.season);
        reply.u8(options_.weather);
        reply.text_u16(net::from_utf8(options_.route_id));
        reply.u32(options_.counts.nodes);
        reply.u32(options_.counts.items);
        reply.u32(options_.counts.substations);
        reply.u32(options_.counts.sections);
        transport_.send(connection, reply.take());
        impl.note((console ? "dispatcher " : "user ") + utf8(nick) + " (" + std::to_string(user->id) + ") logged in from " + address);
        if (console) return; // it asks for the world at once

        // The menu waits for the dispatcher's assignment; a player coming
        // back gets its trains again.
        for (const auto& [tid, t] : impl.trains) {
            if (t.owner == user->id) send_to(*user, assign_package(tid, true, t.number, t.record.subtype, t.record.kind));
        }
        return;
    }

    auto* user = impl.user_of(connection);
    if (!user) return;

    // Flood control (rts_server.exe): two counters in 5-s windows.
    if (impl.now - user->window_start >= flood_window) {
        user->window_start = impl.now;
        user->window_all = user->window_sound = 0;
    }
    ++user->window_all;
    if (id == P(Package::sound_trigger) || id == P(Package::sound_state) || id == P(Package::sound_values)) ++user->window_sound;
    if (user->window_all > flood_all || user->window_sound > flood_sound) {
        transport_.send(connection, code_only(P(Package::flooding)));
        transport_.close(connection);
        impl.note("user " + utf8(user->nick) + " flooded: dropped");
        return;
    }

    const auto relay = [&](const std::uint32_t except) { to_playing(framed(id, body), except); };

    // The startup data: only for a player with a train, or one coming back.
    if (id == P(Package::snapshot)) {
        bool owns = false;
        for (const auto& [_, t] : impl.trains) owns = owns || t.owner == user->id;
        if (user->playing || (!owns && !user->played && !user->console)) {
            send_to(*user, code_only(P(Package::snapshot_again)));
            return;
        }
        net::Snapshot s;
        s.root = true;
        s.complete = true;
        s.time = static_cast<float>(time_);
        s.junctions = impl.junction_states();
        s.signals = impl.head_states();
        for (const auto& [tid, t] : impl.trains) {
            auto record = t.record;
            record.assign.reset();
            if (t.owner == user->id) record.assign = net::TrainAssign{.mine = 1, .number = t.number, .subtype = record.subtype, .kind = record.kind};
            s.trains.push_back(std::move(record));
        }
        for (const auto& [uid, other] : impl.users) {
            if (uid == user->id || !other.playing || !other.position || other.position->size() < 44) continue;
            // The record: the user's id, its place from the train on, its name (32 bytes).
            net::WireWriter w;
            w.u32(uid);
            w.bytes(net::ByteView(*other.position).subspan(4, 40));
            const auto name = other.nick.substr(0, 15);
            for (std::size_t i = 0; i < 16; ++i) w.u16(i < name.size() ? static_cast<std::uint16_t>(name[i]) : 0);
            net::WireReader r(w.data());
            net::PlayerRecord p;
            p.user = r.u32();
            p.train = r.u32();
            p.value_8 = r.u32();
            p.value_12 = r.u32();
            for (auto& v : p.position) v = r.f32();
            p.value_28 = r.u32();
            for (auto& v : p.direction) v = r.f32();
            const auto bytes = r.bytes(p.name.size());
            std::copy(bytes.begin(), bytes.end(), p.name.begin());
            s.players.push_back(p);
        }
        if (paused_) s.pause = 1;
        send_to(*user, net::write_snapshot(s));
        user->playing = true;
        user->played = true;
        impl.note("user " + utf8(user->nick) + " got the world: " + std::to_string(s.trains.size()) + " trains");
        return;
    }
    if (!user->playing) return;

    if (id == P(Package::console_command)) {
        if (!user->console) return;
        net::WireReader r(body);
        const auto line = utf8(r.text_u32());
        if (!r.ok()) return;
        impl.note(utf8(user->nick) + "> " + line);
        auto answer = console_commands_ ? console_commands_(line) : std::string("this server takes no commands");
        send_to(*user, package(P(Package::console_reply), [&](net::WireWriter& w) { w.text_u32(net::from_utf8(answer)); }));
        impl.next_console = 0.0; // the state at once
        return;
    }

    // A console's RTS packages (Legacy): its hand route and its junctions.
    if (user->console && id == P(Package::route_left)) {
        net::WireReader r(body);
        const auto train = r.u32();
        const auto length = r.f32();
        const auto flag = r.u8();
        if (!r.ok()) return;
        std::string result;
        if (length < 0.0f) {
            if (!cancel_route(train, result)) impl.note(utf8(user->nick) + ": " + result);
        } else if (!lay_route(train, length, (flag & 1) != 0, result)) {
            impl.note(utf8(user->nick) + ": " + result);
            send_to(*user, package(P(Package::console_reply), [&](net::WireWriter& w) { w.text_u32(net::from_utf8("error: " + result)); }));
        }
        impl.next_console = 0.0;
        return;
    }
    if (user->console && id == P(Package::junction)) {
        net::WireReader r(body);
        const auto index = r.u32();
        const auto branch = r.u32();
        if (!r.ok()) return;
        std::string error;
        if (!set_junction(index + 1, branch, std::nullopt, error)) {
            send_to(*user, package(P(Package::console_reply), [&](net::WireWriter& w) { w.text_u32(net::from_utf8("error: " + error)); }));
        }
        return;
    }

    if (id == P(Package::console_place)) {
        if (!user->console) return;
        const auto place = net::decode_console_place(body);
        if (!place) return;
        impl.note(utf8(user->nick) + "> place " + utf8(place->consist) + " (" + std::to_string(place->cars.size()) + " cars)");
        auto answer = console_place_ ? console_place_(*place) : std::string("this server takes no consists");
        send_to(*user, package(P(Package::console_reply), [&](net::WireWriter& w) { w.text_u32(net::from_utf8(answer)); }));
        impl.next_console = 0.0;
        return;
    }

    switch (id) {
    // Relayed to everyone else, unread.
    case P(Package::joint_action):
    case P(Package::object_state):
    case P(Package::object_sound):
    case P(Package::car_action):
        relay(user->id);
        return;
    // Relayed and kept.
    case P(Package::car_cargo): {
        net::WireReader r(body);
        const auto tid = r.u32();
        const auto car = r.u32();
        if (auto* t = impl.train(tid); t && r.ok()) {
            std::erase_if(t->record.cargo, [&](const net::Bytes& b) {
                net::WireReader k(b);
                return k.u32() == tid && k.u32() == car;
            });
            t->record.cargo.emplace_back(body.begin(), body.end());
        }
        relay(user->id);
        return;
    }
    case P(Package::derail): {
        net::WireReader r(body);
        if (auto* t = impl.train(r.u32()); t && r.ok()) t->record.derail = net::Bytes(body.begin(), body.end());
        relay(user->id);
        return;
    }
    case P(Package::train_wake): {
        // A STATIC train set moving becomes LOOSE (0x4605 to everyone).
        net::WireReader r(body);
        if (auto* t = impl.train(r.u32()); t && r.ok() && t->record.kind == kind_static) {
            t->record.kind = kind_loose;
            to_playing(info_package(t->record, t->number), 0);
        }
        relay(user->id);
        return;
    }
    // The looks, lights and sounds: to the players within 3 km of the train.
    case P(Package::car_light):
    case P(Package::sound_trigger):
    case P(Package::sound_state):
    case P(Package::car_state):
    case P(Package::sound_values): {
        net::WireReader r(body);
        const auto* t = impl.train(r.u32());
        if (!t || !r.ok()) return; // RTS sends nothing for a train it does not know
        const auto at = impl.where(*t);
        const auto bytes = framed(id, body);
        for (auto& [uid, other] : impl.users) {
            if (uid == user->id || !other.playing || !other.connection) continue;
            const auto there = other.where();
            const auto asked = impl.asked.find({t->record.id, uid});
            const bool answer = asked != impl.asked.end() && asked->second >= impl.now;
            if (!answer && at && there && std::hypot(at->x - there->x, at->z - there->z) > look_range) continue;
            transport_.send(other.connection, bytes);
        }
        return;
    }
    case P(Package::player_pos): {
        if (body.size() < 44) return;
        user->position = net::Bytes(body.begin(), body.begin() + 44);
        // Relayed with the sender's id in front and its name after (84 bytes in play).
        net::WireWriter w;
        w.begin(P(Package::player_pos));
        w.u32(user->id);
        w.bytes(body.subspan(4, 40));
        for (std::size_t i = 0; i < 42; ++i) w.u16(i < user->nick.size() ? static_cast<std::uint16_t>(user->nick[i]) : 0);
        w.end();
        to_playing(w.take(), user->id);
        return;
    }
    case P(Package::state_request): {
        // Forwarded to the train's owner (rts_server.exe 0x40FB90 takes mode 2).
        net::WireReader r(body);
        r.u32();
        const auto* t = impl.train(r.u32());
        if (!t || !r.ok() || !t->owner) return;
        if (const auto owner = impl.users.find(t->owner); owner != impl.users.end() && owner->second.playing) {
            send_to(owner->second, framed(id, body));
            std::erase_if(impl.asked, [&](const auto& entry) { return entry.second < impl.now; });
            impl.asked[{t->record.id, user->id}] = impl.now + 15.0;
        }
        return;
    }
    case P(Package::train_pos): {
        const auto update = net::decode_train_pos(body);
        if (!update) return;
        auto* t = impl.train(update->train);
        // Taken for a PLAYER train from any sender, as rts_server.exe does.
        if (!t || t->record.kind != kind_player || update->pos.cars.size() != t->record.cars.size()) return;
        t->record.pos = update->pos;
        t->record.forced = false;
        if (std::abs(update->pos.speed) > 0.01f) {
            t->record.stands = false;
            t->record.state = 2;
            t->stood_at = -1.0;
        }
        t->placed_at = impl.now;
        impl.place_sim(*t);
        relay(user->id);
        return;
    }
    case P(Package::train_stands): {
        const auto tid = net::decode_train_id(body);
        auto* t = tid ? impl.train(*tid) : nullptr;
        if (!t) return;
        t->record.stands = true;
        if (t->record.pos) {
            t->record.pos->speed = 0.0f;
            t->record.pos->acceleration = 0.0f;
        }
        t->stood_at = impl.now;
        if (t->sim) t->sim->add_velocity(-static_cast<float>(t->sim->speed()));
        to_playing(framed(id, body), user->id);
        if (t->record.pos) to_playing(pos_package(t->record), user->id);
        return;
    }
    case P(Package::junction):
    case P(Package::junction_request): {
        net::WireReader r(body);
        const auto index = r.u32();
        const auto branch = r.u32();
        if (!r.ok()) return;
        const auto node = index + 1;
        const auto* n = impl.network.node(node);
        const auto* state = impl.network.junction(node);
        if (!n || n->kind != formats::TrackNodeKind::junction || !state || branch >= n->branch_count()) return;
        // A locked junction, or one a train stands on, stays: the sender
        // gets its state back.
        const bool held = impl.holder(node).has_value() && state->selected != branch;
        const bool done = !impl.locked.contains(node) && !held && (state->selected == branch || impl.network.throw_junction(node, branch));
        const auto now_state = net::JunctionState{.index = index, .branch = impl.network.junction(node)->selected,
                                                  .lock = static_cast<std::uint8_t>(impl.locked.contains(node) ? 1 : 0)};
        if (done) {
            to_playing(junction_package(now_state), 0);
        } else {
            send_to(*user, junction_package(now_state));
            impl.note("junction " + std::to_string(node) + " refused to " + utf8(user->nick) +
                      (impl.locked.contains(node) ? " (locked)" : held ? " (on train " + std::to_string(*impl.holder(node)) + "'s route)" : " (occupied)"));
        }
        return;
    }
    case P(Package::junctions): {
        // A list of changes, each a nested 0x460A.
        net::WireReader r(body);
        while (r.remaining() >= 6 && r.ok()) {
            const auto frame = r.frame();
            if (!r.ok()) break;
            if (frame.id == P(Package::junction)) received(connection, frame.id, frame.body);
        }
        return;
    }
    case P(Package::couple): {
        const auto c = net::decode_couple(body);
        if (!c) return;
        auto* a = impl.train(c->a);
        auto* b = impl.train(c->b);
        if (!a || !b || a == b) return; // the second of two clients' couplings names a train gone
        const auto old_b = c->b;
        const auto b_owner = b->owner;
        // The taken-in player is told first that the train is no longer theirs.
        if (b->record.kind == kind_player && b_owner && b_owner != a->owner) {
            if (const auto it = impl.users.find(b_owner); it != impl.users.end()) {
                send_to(it->second, assign_package(old_b, false, b->number, b->record.subtype, b->record.kind));
            }
        }
        a->record.cars = net::coupled_cars(a->record.cars, b->record.cars, c->end_a, c->end_b);
        if (a->sim && b->sim && a->sim->placed() && b->sim->placed()) {
            const bool at_front = c->end_a == 1;
            const bool other_front = c->end_b == 1;
            double gap = 0.0;
            const auto end_a = at_front ? a->sim->front_end() : a->sim->rear_end();
            const auto end_b = other_front ? b->sim->front_end() : b->sim->rear_end();
            if (end_a && end_b) {
                const simulation::TrackLocation from{end_a->node(), end_a->offset(), at_front ? end_a->forward() : !end_a->forward()};
                const simulation::TrackLocation to{end_b->node(), end_b->offset(), other_front ? !end_b->forward() : end_b->forward()};
                if (const auto d = net::track_distance(impl.network, from, to, 50.0)) gap = std::max(0.0, *d);
            }
            a->sim->couple(*b->sim, at_front, other_front, gap);
            if (const auto pos = net::position_of(*a->sim)) a->record.pos = *pos;
        }
        impl.trains.erase(old_b);
        b = nullptr;
        net::WireWriter merged;
        merged.u32(c->a);
        merged.u32(old_b);
        merged.u8(c->end_a);
        merged.u8(c->end_b);
        to_playing(framed(P(Package::couple), merged.data()), user->id);
        if (b_owner && b_owner != a->owner) {
            if (const auto it = impl.users.find(b_owner); it != impl.users.end()) {
                send_to(it->second, package(P(Package::train_id), [&](net::WireWriter& w) {
                    w.u32(old_b);
                    w.u32(c->a);
                }));
            }
        }
        if (a->record.pos && a->record.pos->cars.size() == a->record.cars.size()) to_playing(pos_package(a->record), user->id);
        impl.note("train " + std::to_string(c->a) + " took in train " + std::to_string(old_b) + " (" + utf8(user->nick) + ")");
        return;
    }
    case P(Package::uncouple): {
        const auto u = net::decode_uncouple(body);
        if (!u) return;
        auto* t = impl.train(u->train);
        if (!t || u->car == 0 || u->car >= t->record.cars.size()) return;
        auto [stay, part_cars] = net::cut_cars(t->record.cars, u->car, u->side);
        // The part keeps the kind, the owner, the number and the state of
        // its train (rts_server.exe 0x44FE34 -> 0x456958).
        Train part;
        part.record = t->record;
        part.record.id = impl.free_train_id();
        part.record.cars = std::move(part_cars);
        part.record.names.reset();
        part.record.cargo.clear();
        part.record.derail.reset();
        part.owner = t->owner;
        part.number = t->number;
        part.placed_at = impl.now;
        t->record.cars = std::move(stay);
        if (t->sim && t->sim->placed()) {
            auto rear = t->sim->uncouple(u->car, true, true);
            // Side 0: the part is the cars from `car` on, the rear ones.
            if (u->side == 0) {
                part.sim = std::move(rear);
            } else {
                part.sim = std::move(t->sim);
                t->sim = std::move(rear);
            }
            if (t->sim) {
                if (const auto pos = net::position_of(*t->sim)) t->record.pos = *pos;
            }
            if (part.sim) {
                if (const auto pos = net::position_of(*part.sim)) part.record.pos = *pos;
            }
        }
        const auto new_id = part.record.id;
        impl.trains.emplace(new_id, std::move(part));
        send_to(*user, package(P(Package::train_id), [&](net::WireWriter& w) {
            w.u32(u->part);
            w.u32(new_id);
        }));
        to_playing(framed(P(Package::uncouple), net::encode_uncouple({.train = u->train, .car = u->car, .part = new_id, .side = u->side})), user->id);
        if (const auto* p = impl.train(new_id); p && p->record.pos) to_playing(pos_package(p->record), user->id);
        impl.note("train " + std::to_string(u->train) + " cut at car " + std::to_string(u->car) + ": part " + std::to_string(new_id) + " (" + utf8(user->nick) + ")");
        return;
    }
    // A player's train changes the server keeps for its consoles alone
    // (rts_server.exe drops them from players).
    default:
        return;
    }
}

// ---- the world's step ------------------------------------------------------------------

void GameServer::tick(const double now) {
    auto& impl = *impl_;
    const double dt = impl.last_tick < 0.0 ? 0.0 : std::clamp(now - impl.last_tick, 0.0, 0.5);
    impl.last_tick = now;
    impl.now = now;
    // The consoles' view of the game, every second (even while paused).
    if (now >= impl.next_console) {
        impl.next_console = now + 1.0;
        net::ConsoleState state;
        state.time = static_cast<float>(time_);
        state.paused = paused_;
        state.admits = options_.admit;
        bool any = false;
        for (const auto& [uid, user] : impl.users) {
            any = any || (user.console && user.playing && user.connection);
            net::ConsoleUser u{.id = uid, .nick = user.nick, .connected = user.connection != 0, .playing = user.playing, .console = user.console};
            for (const auto& [tid, t] : impl.trains) {
                if (t.owner == uid) u.trains.push_back(tid);
            }
            if (const auto at = user.where()) {
                u.camera_x = static_cast<float>(at->x);
                u.camera_z = static_cast<float>(at->z);
                u.placed = true;
            }
            state.users.push_back(std::move(u));
        }
        if (any) {
            for (const auto& [tid, t] : impl.trains) {
                net::ConsoleTrain c{.id = tid, .number = t.number, .kind = t.record.kind, .subtype = t.record.subtype, .state = t.record.state,
                                    .owner = t.owner, .speed = t.record.pos ? t.record.pos->speed : 0.0f, .stands = t.record.stands,
                                    .cars = static_cast<std::uint32_t>(t.record.cars.size())};
                if (const auto route = impl.routes.find(tid); route != impl.routes.end()) c.route_junctions = static_cast<std::uint32_t>(route->second.junctions.size());
                if (!t.record.cars.empty()) c.first_car = t.record.cars.front().file;
                state.trains.push_back(std::move(c));
            }
            const auto bytes = framed(P(Package::console_state), net::encode_console_state(state));
            for (const auto& [_, user] : impl.users) {
                if (user.console && user.playing && user.connection) transport_.send(user.connection, bytes);
            }
        }
    }
    if (paused_) return; // nothing at all goes out while paused (live capture)
    const auto to_playing = [&](const net::Bytes& bytes) {
        for (auto& [_, user] : impl.users) {
            if (user.playing && user.connection) transport_.send(user.connection, bytes);
        }
    };

    time_ = std::fmod(time_ + dt, 86400.0);
    if (now >= impl.next_time) {
        if (impl.next_time > 0.0) to_playing(time_package(time_));
        impl.next_time = now + options_.time_period;
    }

    // The trains between their places: on at the speed they last had, for
    // at most 5 s (rts1.exe's extrapolation).
    for (auto& [id, t] : impl.trains) {
        if (!t.sim || !t.sim->placed() || !t.record.pos || t.record.stands) continue;
        const double age = now - t.placed_at;
        if (t.placed_at < 0.0 || age > place_timeout) continue;
        const double speed = t.record.pos->speed + t.record.pos->acceleration * age;
        if (speed * t.record.pos->speed <= 0.0) continue;
        t.sim->shift(speed * dt);
        t.sim->add_velocity(static_cast<float>(speed) - static_cast<float>(t.sim->speed()));
    }

    // A route's junctions are freed once the train's trailing end has
    // passed them (and their clearance, 30 m); the distance its head has
    // left goes to everyone (0x4619, every 0.5 s while it changes, as
    // rts_server.exe); it ends once the head is past its end and the
    // junctions are free (-1).
    for (auto it = impl.routes.begin(); it != impl.routes.end();) {
        const auto* t = impl.train(it->first);
        if (!t) {
            it = impl.routes.erase(it);
            continue;
        }
        auto& route = it->second;
        if (t->sim && t->sim->placed()) {
            const auto trailing = route.rear_first ? t->sim->front_end() : t->sim->rear_end();
            const auto leading = route.rear_first ? t->sim->rear_end() : t->sim->front_end();
            if (trailing) {
                if (const auto at = route.leg.locate(trailing->node(), trailing->offset())) {
                    std::erase_if(route.junctions, [&](const auto& j) { return j.second + 30.0 < *at; });
                }
            }
            float left = 0.0f;
            if (const auto at = leading ? route.leg.locate(leading->node(), leading->offset()) : std::nullopt) {
                left = static_cast<float>((std::max)(0.0, route.leg.length() - *at));
            } else {
                route.ended = true;
            }
            if (left <= 0.5f) route.ended = true;
            if (now >= route.send_at && std::abs(left - route.sent) >= 0.5f) {
                send_route_left(it->first, left, route.rear_first);
                route.sent = left;
                route.send_at = now + 0.5;
            }
        }
        if (route.ended && route.junctions.empty()) {
            send_route_left(it->first, -1.0f, route.rear_first);
            it = impl.routes.erase(it);
            continue;
        }
        ++it;
    }

    // Standing trains grow cold (rts_server.exe's timers).
    for (auto& [id, t] : impl.trains) {
        if (!t.record.stands || t.stood_at < 0.0) continue;
        const double stood = now - t.stood_at;
        const std::uint8_t state = stood >= cold_after ? 0 : stood >= warm_after ? 1 : t.record.state;
        if (state < t.record.state) {
            t.record.state = state;
            to_playing(info_package(t.record, t.number));
        }
    }

    // The signals on the server's 0.05-s step, with every train on it.
    if (impl.signals) {
        impl.signal_step += dt;
        if (impl.signal_step >= simulation::train_step) {
            std::vector<const simulation::Train*> placed;
            for (const auto& [_, t] : impl.trains) {
                if (t.sim && t.sim->placed()) placed.push_back(t.sim.get());
            }
            int steps = 0;
            while (impl.signal_step >= simulation::train_step && steps++ < 4) {
                impl.signals->update(placed, simulation::train_step);
                impl.signal_step -= simulation::train_step;
            }
            impl.signal_step = std::min(impl.signal_step, simulation::train_step);
            const auto& heads = impl.signals->heads();
            impl.shown.resize(heads.size());
            for (std::size_t h = 0; h < heads.size(); ++h) {
                const auto s = impl.shown_of(heads[h]);
                if (s == impl.shown[h]) continue;
                impl.shown[h] = s;
                to_playing(head_package({.item = heads[h].track_item, .semaphore = 0, .state = s.state, .draw_state = s.draw_state,
                                         .value_92 = 0.0f, .mode = s.mode, .speed = s.speed}));
            }
        }
    }
}

// ---- the dispatcher ---------------------------------------------------------------------

std::optional<std::uint32_t> GameServer::add_train(NewTrain made, std::string& error) {
    auto& impl = *impl_;
    if (made.cars.empty() || made.cars.size() != made.records.size()) {
        error = "no cars";
        return std::nullopt;
    }
    Train t;
    t.record.id = impl.free_train_id();
    t.record.kind = made.kind;
    t.record.subtype = made.subtype;
    t.record.top_speed = made.top_speed;
    t.record.state = made.state;
    t.record.cars = std::move(made.records);
    t.record.value_84 = 1.0f; // acceleration and deceleration (PROBABLE), as the console's default
    t.record.value_88 = 2.0f;
    if (!made.name.empty()) t.record.names = std::array<std::u16string, 2>{made.name, u""};
    t.number = made.number;
    t.sim = std::make_unique<simulation::Train>(std::move(made.cars));
    if (!t.sim->place(impl.network, made.node, made.offset, made.forward, &error)) return std::nullopt;
    for (const auto& [other_id, other] : impl.trains) {
        if (!other.sim || !other.sim->placed()) continue;
        if (impl.covers(*t.sim, *other.sim) || impl.covers(*other.sim, *t.sim)) {
            t.sim->vacate();
            error = "occupied by train " + std::to_string(other_id);
            return std::nullopt;
        }
    }
    const auto pos = net::position_of(*t.sim);
    if (!pos) {
        error = "the train could not be placed";
        return std::nullopt;
    }
    t.record.pos = *pos;
    t.record.stands = true;
    t.stood_at = impl.now;
    t.placed_at = impl.now;
    const auto id = t.record.id;
    const auto bytes = train_packages(t.record);
    impl.trains.emplace(id, std::move(t));
    for (auto& [_, user] : impl.users) {
        if (user.playing && user.connection) transport_.send(user.connection, bytes);
    }
    impl.note("train " + std::to_string(id) + " placed");
    return id;
}

bool GameServer::assign(const std::uint32_t train, const std::string_view nick, std::string& error) {
    auto& impl = *impl_;
    auto* t = impl.train(train);
    if (!t) {
        error = "no train " + std::to_string(train);
        return false;
    }
    auto* user = impl.user_named(net::from_utf8(nick));
    if (!user) {
        error = "no player " + std::string(nick) + " (they get a train once they have logged in)";
        return false;
    }
    if (t->owner && t->owner != user->id) {
        if (const auto old = impl.users.find(t->owner); old != impl.users.end() && old->second.connection) {
            transport_.send(old->second.connection, assign_package(train, false, t->number, t->record.subtype, t->record.kind));
        }
    }
    t->owner = user->id;
    const bool kind_changed = t->record.kind != kind_player;
    t->record.kind = kind_player;
    if (user->connection) transport_.send(user->connection, assign_package(train, true, t->number, t->record.subtype, kind_player));
    if (kind_changed) {
        const auto info = info_package(t->record, t->number);
        for (auto& [uid, other] : impl.users) {
            if (uid != user->id && other.playing && other.connection) transport_.send(other.connection, info);
        }
    }
    impl.note("train " + std::to_string(train) + " assigned to " + std::string(nick));
    return true;
}

bool GameServer::unassign(const std::uint32_t train, std::string& error) {
    auto& impl = *impl_;
    auto* t = impl.train(train);
    if (!t) {
        error = "no train " + std::to_string(train);
        return false;
    }
    if (t->owner) {
        if (const auto old = impl.users.find(t->owner); old != impl.users.end() && old->second.connection) {
            transport_.send(old->second.connection, assign_package(train, false, t->number, t->record.subtype, t->record.kind));
        }
    }
    t->owner = 0;
    // Nobody drives it: it stands as a STATIC train.
    t->record.kind = kind_static;
    t->record.stands = true;
    if (t->stood_at < 0.0) t->stood_at = impl.now;
    const auto info = info_package(t->record, t->number);
    for (auto& [_, user] : impl.users) {
        if (user.playing && user.connection) transport_.send(user.connection, info);
    }
    return true;
}

bool GameServer::remove_train(const std::uint32_t train, std::string& error) {
    auto& impl = *impl_;
    auto* t = impl.train(train);
    if (!t) {
        error = "no train " + std::to_string(train);
        return false;
    }
    // The owner is told first: rts1.exe crashed when its own train was taken
    // off the track under it (live capture 2026-11-05).
    if (t->owner) unassign(train, error);
    if (t->sim) t->sim->vacate();
    impl.routes.erase(train);
    impl.trains.erase(train);
    const auto bytes = package(P(Package::train_remove), [&](net::WireWriter& w) {
        w.u32(train);
        w.u8(1);
    });
    for (auto& [_, user] : impl.users) {
        if (user.playing && user.connection) transport_.send(user.connection, bytes);
    }
    return true;
}

bool GameServer::message(const std::string_view nick, const std::u16string_view text, std::string& error) {
    auto& impl = *impl_;
    const auto bytes = package(P(Package::message), [&](net::WireWriter& w) { w.text_u32(text); });
    bool sent = false;
    for (auto& [_, user] : impl.users) {
        if (!user.connection) continue;
        if (nick != "*" && utf8(user.nick) != nick) continue;
        transport_.send(user.connection, bytes);
        sent = true;
    }
    if (!sent) error = nick == "*" ? "nobody is connected" : "no player " + std::string(nick) + " connected";
    return sent;
}

void GameServer::set_paused(const bool paused) {
    if (paused == paused_) return;
    paused_ = paused;
    const auto bytes = package(P(Package::pause), [&](net::WireWriter& w) { w.u8(paused ? 1 : 0); });
    for (auto& [_, user] : impl_->users) {
        if (user.playing && user.connection) transport_.send(user.connection, bytes);
    }
}

void GameServer::set_time(const double seconds) {
    time_ = std::fmod(std::fmod(seconds, 86400.0) + 86400.0, 86400.0);
    const auto bytes = time_package(time_);
    for (auto& [_, user] : impl_->users) {
        if (user.playing && user.connection) transport_.send(user.connection, bytes);
    }
}

bool GameServer::set_junction(const std::uint32_t node, const std::optional<std::uint32_t> branch, const std::optional<bool> lock,
                              std::string& error) {
    auto& impl = *impl_;
    const auto* n = impl.network.node(node);
    auto* state = impl.network.junction(node);
    if (!n || n->kind != formats::TrackNodeKind::junction || !state) {
        error = "node " + std::to_string(node) + " is no junction";
        return false;
    }
    if (branch) {
        if (*branch >= n->branch_count()) {
            error = "junction " + std::to_string(node) + " has " + std::to_string(n->branch_count()) + " branches";
            return false;
        }
        if (state->selected != *branch && !impl.network.throw_junction(node, *branch)) {
            error = "junction " + std::to_string(node) + " is occupied or reserved";
            return false;
        }
    }
    if (lock) {
        if (*lock) impl.locked.insert(node);
        else impl.locked.erase(node);
    }
    const auto bytes = junction_package({.index = node - 1, .branch = impl.network.junction(node)->selected,
                                         .lock = static_cast<std::uint8_t>(impl.locked.contains(node) ? 1 : 0)});
    for (auto& [_, user] : impl.users) {
        if (user.playing && user.connection) transport_.send(user.connection, bytes);
    }
    return true;
}

bool GameServer::set_signal(const std::uint32_t item, const std::optional<formats::SignalAspect> aspect, std::string& error) {
    auto& impl = *impl_;
    if (!impl.signals) {
        error = "the route's signals did not load";
        return false;
    }
    bool found = false;
    const auto& heads = impl.signals->heads();
    for (std::uint32_t h = 0; h < heads.size(); ++h) {
        if (heads[h].track_item != item) continue;
        found = true;
        if (aspect) {
            simulation::SignalOverride value;
            value.aspect = *aspect;
            value.draw_state = -2; // the type's draw state for the aspect
            impl.signals->set_override(h, value);
        } else {
            impl.signals->clear_override(h);
        }
    }
    if (!found) {
        error = "no signal head on item " + std::to_string(item);
        return false;
    }
    if (aspect) impl.manual.insert(item);
    else impl.manual.erase(item);
    return true;
}

bool GameServer::kick(const std::string_view nick, std::string& error) {
    auto* user = impl_->user_named(net::from_utf8(nick));
    if (!user || !user->connection) {
        error = "no player " + std::string(nick) + " connected";
        return false;
    }
    transport_.close(user->connection);
    return true;
}

bool GameServer::set_route(const std::uint32_t train, const std::vector<std::vector<std::uint32_t>>& stops, std::string& result) {
    auto& impl = *impl_;
    auto* t = impl.train(train);
    if (!t || !t->sim || !t->sim->placed()) {
        result = t ? "train " + std::to_string(train) + " is not on the server's track" : "no train " + std::to_string(train);
        return false;
    }
    if (!impl.signals) {
        result = "the route's signals did not load";
        return false;
    }
    if (stops.empty()) {
        result = "no signal to run to";
        return false;
    }
    // Where each candidate head stands (the direction it protects).
    const auto& heads = impl.signals->heads();
    const auto places = [&](const std::vector<std::uint32_t>& items) {
        std::vector<simulation::TrackLocation> out;
        for (const auto& head : heads) {
            if (!head.placed) continue;
            if (std::find(items.begin(), items.end(), head.track_item) != items.end()) out.push_back({head.node, head.offset, head.forward});
        }
        return out;
    };
    // From the front, else from the rear running rear first.
    std::vector<simulation::TrackLocation> starts;
    if (const auto front = t->sim->front_end()) starts.push_back({front->node(), front->offset(), front->forward()});
    if (const auto rear = t->sim->rear_end()) starts.push_back({rear->node(), rear->offset(), !rear->forward()});
    std::optional<simulation::TrainRoute> best;
    bool from_rear = false;
    for (std::size_t which = 0; which < starts.size(); ++which) {
        const auto& start = starts[which];
        simulation::PathPlan plan;
        plan.name = "route";
        plan.start = start;
        bool reached = true;
        for (std::size_t i = 0; i < stops.size() && reached; ++i) {
            // The nearest of the stop's heads from where the plan has got to.
            std::optional<simulation::TrackLocation> chosen;
            double shortest = 0.0;
            for (const auto& place : places(stops[i])) {
                auto trial = plan;
                trial.points.push_back({.kind = simulation::PlanPointKind::end, .node = place.node, .offset = place.offset});
                const auto route = simulation::build_route(impl.network, trial);
                if (!route.ok() || route.legs.size() != 1) continue;
                const double length = route.legs[0].length();
                if (!chosen || length < shortest) {
                    chosen = place;
                    shortest = length;
                }
            }
            if (!chosen) {
                reached = false;
                break;
            }
            if (!plan.points.empty()) plan.points.back().kind = simulation::PlanPointKind::via;
            plan.points.push_back({.kind = simulation::PlanPointKind::end, .node = chosen->node, .offset = chosen->offset});
        }
        if (!reached) continue;
        auto route = simulation::build_route(impl.network, plan);
        if (route.ok() && route.legs.size() == 1) {
            best = std::move(route);
            from_rear = which == 1 || !t->sim->front_end();
            break;
        }
    }
    if (!best) {
        result = "no way from train " + std::to_string(train) + " to the signal";
        return false;
    }
    const auto& leg = best->legs[0];
    // Every junction must be free to set: none under a train, locked or on
    // another train's route the other way.
    for (const auto& j : leg.junctions) {
        const auto* state = impl.network.junction(j.junction);
        if (!state || state->selected == j.branch) continue;
        if (impl.locked.contains(j.junction)) {
            result = "junction " + std::to_string(j.junction) + " is locked";
            return false;
        }
        if (const auto other = impl.holder(j.junction); other && *other != train) {
            result = "junction " + std::to_string(j.junction) + " is on train " + std::to_string(*other) + "'s route";
            return false;
        }
        if (state->occupied) {
            result = "junction " + std::to_string(j.junction) + " is occupied";
            return false;
        }
    }
    std::size_t thrown = 0;
    Impl::HeldRoute held{.leg = leg, .rear_first = from_rear};
    for (const auto& j : leg.junctions) {
        const auto* state = impl.network.junction(j.junction);
        if (!state) continue;
        if (state->selected != j.branch) {
            if (!impl.network.throw_junction(j.junction, j.branch)) {
                result = "junction " + std::to_string(j.junction) + " could not be thrown";
                return false;
            }
            ++thrown;
            const auto bytes = junction_package({.index = j.junction - 1, .branch = j.branch,
                                                 .lock = static_cast<std::uint8_t>(impl.locked.contains(j.junction) ? 1 : 0)});
            for (auto& [_, user] : impl.users) {
                if (user.playing && user.connection) transport_.send(user.connection, bytes);
            }
        }
        held.junctions.emplace_back(j.junction, j.distance);
    }
    const auto count = held.junctions.size();
    held.sent = static_cast<float>(leg.length());
    impl.routes[train] = std::move(held);
    send_route_left(train, static_cast<float>(leg.length()), from_rear);
    char text[160];
    std::snprintf(text, sizeof text, "train %u's route: %.0f m, %zu junctions (%zu thrown), held until its rear has passed them", train,
                  leg.length(), count, thrown);
    result = text;
    impl.note(result);
    return true;
}

std::vector<std::uint32_t> GameServer::route_junctions(const std::uint32_t train) const {
    std::vector<std::uint32_t> out;
    if (const auto it = impl_->routes.find(train); it != impl_->routes.end()) {
        for (const auto& [node, _] : it->second.junctions) out.push_back(node);
    }
    return out;
}

bool GameServer::cancel_route(const std::uint32_t train, std::string& error) {
    const auto it = impl_->routes.find(train);
    if (it == impl_->routes.end()) {
        error = "train " + std::to_string(train) + " has no route";
        return false;
    }
    const bool rear_first = it->second.rear_first;
    impl_->routes.erase(it);
    send_route_left(train, -1.0f, rear_first);
    return true;
}

void GameServer::send_route_left(const std::uint32_t train, const float left, const bool rear_first) {
    auto& impl = *impl_;
    if (auto* t = impl.train(train)) {
        if (left < 0.0f) t->record.route_left.reset();
        else t->record.route_left = std::pair<float, std::uint8_t>{left, static_cast<std::uint8_t>(rear_first ? 1 : 0)};
    }
    const auto bytes = package(P(Package::route_left), [&](net::WireWriter& w) {
        w.u32(train);
        w.f32(left);
        w.u8(rear_first ? 1 : 0);
    });
    for (auto& [_, user] : impl.users) {
        if (user.playing && user.connection) transport_.send(user.connection, bytes);
    }
}

bool GameServer::lay_route(const std::uint32_t train, const double length, const bool rear_first, std::string& result) {
    auto& impl = *impl_;
    auto* t = impl.train(train);
    if (!t || !t->sim || !t->sim->placed()) {
        result = t ? "train " + std::to_string(train) + " is not on the server's track" : "no train " + std::to_string(train);
        return false;
    }
    if (length <= 0.0) {
        result = "a route of no length";
        return false;
    }
    // From the head (the rear, running rear first), along the junctions as
    // they stand.
    const auto end = rear_first ? t->sim->rear_end() : t->sim->front_end();
    if (!end) {
        result = "train " + std::to_string(train) + " has no end on the track";
        return false;
    }
    const simulation::TrackLocation start{end->node(), end->offset(), rear_first ? !end->forward() : end->forward()};
    simulation::TrackTraveller walk(impl.network, start.node, start.offset, start.forward);
    const auto moved = walk.move(length);
    if (moved.moved <= 0.0) {
        result = "no track ahead of train " + std::to_string(train);
        return false;
    }
    simulation::PathPlan plan;
    plan.name = "route";
    plan.start = start;
    simulation::PlanPoint point{.kind = simulation::PlanPointKind::end, .node = walk.node(), .offset = walk.offset()};
    for (const auto& pass : moved.junctions) point.through.push_back(pass.junction);
    plan.points.push_back(std::move(point));
    const auto built = simulation::build_route(impl.network, plan);
    if (!built.ok() || built.legs.size() != 1) {
        result = "the route could not be laid: " + built.error;
        return false;
    }
    Impl::HeldRoute held{.leg = built.legs[0], .rear_first = rear_first};
    for (const auto& j : held.leg.junctions) held.junctions.emplace_back(j.junction, j.distance);
    const auto count = held.junctions.size();
    held.sent = static_cast<float>(held.leg.length());
    held.send_at = impl.now + 0.5;
    impl.routes[train] = std::move(held);
    send_route_left(train, static_cast<float>(impl.routes[train].leg.length()), rear_first);
    char text[160];
    std::snprintf(text, sizeof text, "train %u's route: %.0f m along the junctions as they stand%s, %zu junctions held", train,
                  impl.routes[train].leg.length(), moved.ok() ? "" : " (to the end of the track)", count);
    result = text;
    impl.note(result);
    return true;
}

std::vector<TrainSummary> GameServer::trains() const {
    std::vector<TrainSummary> out;
    for (const auto& [id, t] : impl_->trains) {
        TrainSummary s{.id = id, .number = t.number, .kind = t.record.kind, .cars = t.record.cars.size()};
        if (const auto route = impl_->routes.find(id); route != impl_->routes.end()) s.route_junctions = route->second.junctions.size();
        if (t.owner) {
            if (const auto it = impl_->users.find(t.owner); it != impl_->users.end()) s.owner = utf8(it->second.nick);
        }
        s.placed = t.sim && t.sim->placed();
        s.stands = t.record.stands;
        if (t.record.pos) {
            s.speed = t.record.pos->speed;
            char text[64];
            std::snprintf(text, sizeof text, "node %u at %.1f m", t.record.pos->node, t.record.pos->offset);
            s.where = text;
        }
        out.push_back(std::move(s));
    }
    return out;
}

std::vector<UserSummary> GameServer::users() const {
    std::vector<UserSummary> out;
    for (const auto& [id, user] : impl_->users) {
        UserSummary s{.id = id, .nick = utf8(user.nick), .address = user.address, .connected = user.connection != 0, .playing = user.playing,
                      .console = user.console};
        for (const auto& [tid, t] : impl_->trains) {
            if (t.owner == id) s.trains.push_back(tid);
        }
        // The camera's place as the route has it: its tile and the place in
        // it (x east, y up, z north, from the tile's middle).
        if (user.position && user.position->size() >= 28) {
            net::WireReader r(*user.position);
            r.skip(12);
            const auto tile_x = r.i16();
            const auto tile_z = r.i16();
            const auto x = r.f32();
            const auto y = r.f32();
            const auto z = r.f32();
            char text[128];
            std::snprintf(text, sizeof text, "camera at tile %d, %d (x %.0f, y %.0f, z %.0f)", tile_x, tile_z, x, y, z);
            s.where = text;
            // How far it is from the user's trains.
            const auto at = user.where();
            for (const auto tid : s.trains) {
                const auto t = impl_->trains.find(tid);
                const auto there = t != impl_->trains.end() ? impl_->where(t->second) : std::nullopt;
                if (!at || !there) continue;
                std::snprintf(text, sizeof text, ", %.0f m from train %u", std::hypot(at->x - there->x, at->z - there->z), tid);
                s.where += text;
            }
        }
        out.push_back(std::move(s));
    }
    return out;
}

std::uint32_t GameServer::free_number() const {
    std::set<std::uint32_t> used;
    for (const auto& [_, t] : impl_->trains) used.insert(t.number);
    std::uint32_t number = 101;
    while (used.contains(number)) ++number;
    return number;
}

std::vector<std::string> GameServer::take_log() { return std::exchange(impl_->log, {}); }

} // namespace openrts::server
