// The openRTS server (docs/server.md): the login and its refusals, the
// snapshot, the relays by RTS's rules, junctions, couplings and cuts, pause
// and time, a reconnect, flooding; then over real TCP with openRTS's client;
// with an installation, Kuban's counts against rts_server.exe's and its
// signals and junctions from the console.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <openrts/formats/track_database.hpp>
#include <openrts/formats/track_sections.hpp>
#include <openrts/net/connection.hpp>
#include <openrts/net/packages.hpp>
#include <openrts/net/play.hpp>
#include <openrts/net/snapshot.hpp>
#include <openrts/server/console.hpp>
#include <openrts/server/game_server.hpp>
#include <openrts/server/server_route.hpp>
#include <openrts/server/transport.hpp>
#include <openrts/simulation/track_network.hpp>
#include <openrts/simulation/train.hpp>

namespace {

using namespace openrts;
using net::Package;
constexpr std::uint16_t P(const Package p) { return net::id(p); }

int failures = 0;

void check(const bool condition, const std::string& what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

// ---- a synthetic track: end 10 - [1: 100 m] - junction 20; branch 0 -> [2] -> end 11; branch 1 -> [3] -> end 12.

const char* sections_text = R"(SIMISA@@@@@@@@@@JINX0T0t______
_INFO ( header )
TrackSections ( 1
    TrackSection ( 1 SectionSize ( 1.5 100 ) )
)
TrackShapes ( 1
    TrackShape ( 7
        FileName ( Switch.s )
        NumPaths ( 2 )
        MainRoute ( 0 )
        ClearanceDist ( 25 )
        SectionIdx ( 1 0 0 0 0 1 )
        SectionIdx ( 1 1.5 0 0.1 0 1 )
    )
)
)";

std::string section(const double x, const double z) {
    return "1 0 0 0 0 1 1 00 0 0 " + std::to_string(x) + " 0 " + std::to_string(z) + " 0 0 0 ";
}

std::string pin(const int node, const int direction) {
    return "TrPin ( " + std::to_string(node) + " " + std::to_string(direction) + " ) ";
}

std::string vector_node(const int index, const std::string& sections, const std::string& pins) {
    return "TrackNode ( " + std::to_string(index) + " TrVectorNode ( TrVectorSections ( 1 " + sections + ") ) TrPins ( 1 1 " + pins + ") )\n";
}

std::unique_ptr<simulation::TrackNetwork> switch_track() {
    const std::string nodes =
        vector_node(1, section(0, 0), pin(10, 1) + pin(20, 1)) +
        "TrackNode ( 20 TrJunctionNode ( 0 7 0 ) UiD ( 0 0 55 0 0 0 0 0 0 0 0 0 ) TrPins ( 1 2 " + pin(1, 0) + pin(2, 1) + pin(3, 1) +
        ") )\n" + vector_node(2, section(0, 100), pin(20, 0) + pin(11, 1)) + vector_node(3, section(1.5, 100), pin(20, 0) + pin(12, 1)) +
        "TrackNode ( 10 TrEndNode ( 0 ) UiD ( 0 0 0 0 0 0 0 0 0 0 0 0 ) TrPins ( 1 0 " + pin(1, 1) + ") )\n" +
        "TrackNode ( 11 TrEndNode ( 0 ) UiD ( 0 0 0 0 0 0 0 0 0 0 0 0 ) TrPins ( 1 0 " + pin(2, 0) + ") )\n" +
        "TrackNode ( 12 TrEndNode ( 0 ) UiD ( 0 0 0 0 0 0 0 0 0 0 0 0 ) TrPins ( 1 0 " + pin(3, 0) + ") )\n";
    const std::string tdb = "SIMISA@@@@@@@@@@JINX0T0t______\nTrackDB ( Serial ( 1 ) TrackNodes ( 20\n" + nodes + ") TrItemTable ( 0\n) )\n";
    auto sections = formats::parse_track_sections(sections_text);
    auto db = formats::parse_track_database(tdb);
    check(sections.ok() && db.ok(), "synthetic track parses");
    return std::make_unique<simulation::TrackNetwork>(std::move(db), std::move(sections));
}

// ---- what the server sends, per connection.

struct Recorder final : server::Transport {
    std::map<server::ConnectionId, net::Bytes> out;
    std::set<server::ConnectionId> closed;
    void send(const server::ConnectionId to, net::Bytes bytes) override {
        auto& b = out[to];
        b.insert(b.end(), bytes.begin(), bytes.end());
    }
    void close(const server::ConnectionId to) override { closed.insert(to); }
};

struct Got {
    std::uint16_t id{};
    net::Bytes body;
};

// The packages sent to a connection since the last call.
std::vector<Got> take(Recorder& r, const server::ConnectionId c) {
    std::vector<Got> got;
    auto& bytes = r.out[c];
    const auto frames = net::split_frames(bytes);
    for (const auto& f : frames.frames) got.push_back({f.id, net::Bytes(f.body.begin(), f.body.end())});
    bytes.clear();
    return got;
}

std::vector<std::uint16_t> ids(const std::vector<Got>& got) {
    std::vector<std::uint16_t> out;
    for (const auto& g : got) out.push_back(g.id);
    return out;
}

bool has(const std::vector<Got>& got, const std::uint16_t id) {
    for (const auto& g : got) {
        if (g.id == id) return true;
    }
    return false;
}

struct Reply {
    std::uint16_t version{};
    std::u16string description;
    std::uint8_t season{}, weather{};
    std::u16string route;
    std::uint32_t counts[4]{};
};

std::u16string reply_text(net::WireReader& r) {
    const auto n = r.u16();
    std::u16string text;
    for (std::uint16_t i = 0; i < n; ++i) text.push_back(static_cast<char16_t>(r.u16()));
    r.u16();
    return text;
}

// The raw login reply at the front of a connection's bytes (no length field); consumed.
std::optional<Reply> take_reply(Recorder& rec, const server::ConnectionId c) {
    auto& bytes = rec.out[c];
    net::WireReader r(bytes);
    if (r.u16() != P(Package::login)) return std::nullopt;
    Reply reply;
    reply.version = r.u16();
    reply.description = reply_text(r);
    reply.season = r.u8();
    reply.weather = r.u8();
    reply.route = reply_text(r);
    for (auto& v : reply.counts) v = r.u32();
    if (!r.ok()) return std::nullopt;
    bytes.erase(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(r.position()));
    return reply;
}

net::Bytes login(const std::u16string& nick, const std::uint16_t version = 101) {
    net::WireWriter w;
    w.u16(version);
    w.u16(static_cast<std::uint16_t>(nick.size()));
    for (const auto c : nick) w.u16(static_cast<std::uint16_t>(c));
    w.u16(0);
    return w.take();
}

formats::WagonDefinition car_definition() {
    formats::WagonDefinition car;
    car.length = 16.0f;
    car.width = 3.0f;
    car.height = 4.5f;
    car.mass = 50000.0f;
    return car;
}

server::NewTrain new_train(const formats::WagonDefinition& car, const std::uint32_t node, const double offset, const std::uint32_t number) {
    server::NewTrain t;
    for (std::uint32_t i = 0; i < 2; ++i) {
        t.cars.push_back({.definition = &car});
        net::TrainCar record;
        record.file = u"test\\car.wag";
        record.uid = (number << 18) | i;
        record.mass = car.mass;
        record.width = car.width;
        record.length = car.length;
        record.height = car.height;
        record.handbrake = 0.0f;
        record.max_handbrake = 0.0f;
        t.records.push_back(record);
    }
    t.node = node;
    t.offset = offset;
    t.forward = true;
    t.number = number;
    t.top_speed = 30.0f;
    return t;
}

net::Bytes player_pos() {
    net::PlayerPosition p;
    p.train = 1;
    p.tile_x = 0;
    p.tile_z = 0;
    p.x = 10.0f;
    p.z = 50.0f;
    return net::encode_player_pos(p);
}

void game() {
    auto network = switch_track();
    const auto car = car_definition();
    Recorder rec;
    server::ServerOptions options;
    options.route_id = "testroute";
    options.description = server::game_description("TestRoute", 1);
    options.counts = {20, 0, 1, 2};
    {
        // Consoles are not taken by default (they come after the server).
        Recorder off;
        server::GameServer closed(off, options, *network, nullptr);
        closed.connected(1, "127.0.0.1:1");
        closed.received(1, P(Package::console_login), login(u"DSP"));
        check(off.out[1].size() == 2 && off.out[1][0] == 0x02 && off.closed.contains(1), "consoles off: a local console gets 0x4702");
    }
    options.consoles = true;
    server::GameServer game(rec, options, *network, nullptr);
    std::string error;
    double now = 0.0;
    game.tick(now);

    // Refusals: a wrong version, a dispatcher console (not yet), not a login.
    game.connected(9, "127.0.0.1:9");
    game.received(9, P(Package::login), login(u"Old", 100));
    check(rec.out[9].size() == 2 && rec.out[9][0] == 0x05 && rec.out[9][1] == 0x47 && rec.closed.contains(9), "protocol 100: 0x4705 and closed");
    game.connected(10, "10.0.0.5:10");
    game.received(10, P(Package::console_login), login(u"DSP"));
    check(rec.out[10].size() == 2 && rec.out[10][0] == 0x02 && rec.closed.contains(10), "a console from elsewhere, not a dispatcher: 0x4702");
    game.connected(11, "127.0.0.1:11");
    game.received(11, P(Package::time), net::Bytes(4));
    check(rec.out[11].size() == 2 && rec.out[11][0] == 0x04 && rec.closed.contains(11), "not a login: 0x4704");

    // Alice logs in: the reply as rts_server.exe writes it.
    game.connected(1, "127.0.0.1:1");
    game.received(1, P(Package::login), login(u"Alice"));
    const auto reply = take_reply(rec, 1);
    check(reply && reply->version == 101 && reply->route == u"testroute" && reply->season == 1 && reply->weather == 0,
          "the login reply: version, route, season, weather");
    check(reply && reply->counts[0] == 20 && reply->counts[1] == 0 && reply->counts[2] == 1 && reply->counts[3] == 2, "the route's four counts");
    check(reply && reply->description == u"Маршрут: TestRoute\r\nСезон: Лето", "the description");
    check(!rec.closed.contains(1), "Alice stays connected");

    // No train yet: no world.
    // (rts_server.exe answers with the code alone, no length field.)
    game.received(1, P(Package::snapshot), {});
    check(rec.out[1] == net::Bytes{0x03, 0x47}, "no train: 0x4703");
    rec.out[1].clear();

    // A train for Alice: placed, assigned; the world.
    const auto a = game.add_train(new_train(car, 1, 45.0, 101), error);
    check(a == 1u, "train 1 placed: " + error);
    check(!game.add_train(new_train(car, 1, 50.0, 199), error) && error.rfind("occupied by train 1", 0) == 0,
          "not put where train 1 stands: " + error);
    check(game.assign(1, "Alice", error), "train 1 assigned: " + error);
    auto got = take(rec, 1);
    check(got.size() == 1 && got[0].id == P(Package::train_assign) && got[0].body.size() == 13 && got[0].body[4] == 1, "Alice: 0x4601 mine");
    game.received(1, P(Package::snapshot), {});
    {
        const auto bytes = rec.out[1];
        rec.out[1].clear();
        const auto read = net::read_snapshot(bytes);
        check(read.ok() && read.snapshot.root && read.snapshot.complete, "the snapshot reads whole");
        check(read.snapshot.trains.size() == 1 && read.snapshot.trains[0].mine() && read.snapshot.trains[0].pos && read.snapshot.trains[0].stands,
              "the snapshot: Alice's standing train");
        check(read.snapshot.junctions.size() == 1 && read.snapshot.junctions[0].index == 19, "the snapshot: junction index 19 (node 20)");
        check(read.snapshot.time && std::abs(*read.snapshot.time - 12.0f * 3600.0f) < 1.0f, "the snapshot: the time");
    }
    game.received(1, P(Package::snapshot), {});
    check(rec.out[1] == net::Bytes{0x03, 0x47}, "a second request: 0x4703");
    rec.out[1].clear();

    // Bob, and a train for him: Alice (playing) sees it come.
    game.connected(2, "127.0.0.1:2");
    game.received(2, P(Package::login), login(u"Bob"));
    check(take_reply(rec, 2).has_value(), "Bob's reply");
    const auto b = game.add_train(new_train(car, 2, 60.0, 102), error);
    check(b == 2u, "train 2 placed");
    got = take(rec, 1);
    check(has(got, P(Package::train_add)) && has(got, P(Package::train_pos)) && has(got, P(Package::train_stands)), "Alice sees train 2 come");
    check(take(rec, 2).empty(), "Bob in the menu gets no world yet");
    check(game.assign(2, "Bob", error), "train 2 to Bob");
    check(ids(take(rec, 2)) == std::vector<std::uint16_t>{P(Package::train_assign)}, "Bob: 0x4601");
    check(ids(take(rec, 1)) == std::vector<std::uint16_t>{P(Package::train_info)}, "Alice: train 2 is a player's now (0x4605)");
    game.received(2, P(Package::snapshot), {});
    {
        const auto bytes = rec.out[2];
        rec.out[2].clear();
        const auto read = net::read_snapshot(bytes);
        check(read.ok() && read.snapshot.trains.size() == 2, "Bob's snapshot: two trains");
        bool mine = false, other = false;
        for (const auto& t : read.snapshot.trains) {
            mine = mine || (t.id == 2 && t.mine());
            other = other || (t.id == 1 && !t.assign && t.kind == 1);
        }
        check(mine && other, "Bob's own train assigned, Alice's a player's");
    }

    // Positions: relayed to the others only.
    {
        net::TrainPos pos;
        pos.node = 1;
        pos.offset = 18.0f;
        pos.direction = 0;
        pos.speed = 2.0f;
        pos.cars = {{24.0, 2.0f, 0.0f}, {8.0, 2.0f, 0.0f}};
        game.received(1, P(Package::train_pos), net::encode_train_pos(1, pos));
        got = take(rec, 2);
        check(got.size() == 1 && got[0].id == P(Package::train_pos), "Bob gets Alice's train_pos");
        check(take(rec, 1).empty(), "Alice does not get her own back");
        const auto trains = game.trains();
        check(!trains.empty() && std::abs(trains[0].speed - 2.0f) < 1e-3f && !trains[0].stands, "the server keeps the place and the speed");
        // A train_pos with a wrong car count is dropped.
        pos.cars.pop_back();
        game.received(1, P(Package::train_pos), net::encode_train_pos(1, pos));
        check(take(rec, 2).empty(), "a car count that does not match: dropped");
    }

    // player_pos: with the sender's id in front and its name after (84 bytes in play).
    game.received(1, P(Package::player_pos), player_pos());
    got = take(rec, 2);
    check(got.size() == 1 && got[0].id == P(Package::player_pos) && got[0].body.size() == 128 && got[0].body[0] == 1 && got[0].body[44] == 'A',
          "Bob gets Alice's place: her id, her name");

    // A car's light: to the players near the train (Bob has no place yet: he gets it).
    {
        net::CarLightState light{.train = 1, .car = 0, .light = 2, .lit = true};
        game.received(1, P(Package::car_light), net::encode_car_light(light));
        check(ids(take(rec, 2)) == std::vector<std::uint16_t>{P(Package::car_light)}, "Bob gets Alice's car light");
        light.train = 99;
        game.received(1, P(Package::car_light), net::encode_car_light(light));
        check(take(rec, 2).empty(), "a light of no known train goes nowhere");
    }

    // Junctions: a player's request, then the dispatcher's lock.
    {
        net::WireWriter w;
        w.u32(19);
        w.u32(1);
        w.u8(0);
        game.received(2, P(Package::junction), w.data());
        got = take(rec, 1);
        check(got.size() == 1 && got[0].id == P(Package::junction) && got[0].body[4] == 1, "Bob throws junction 20: Alice gets branch 1");
        check(network->junction(20)->selected == 1, "the server's junction is thrown");
        take(rec, 2);
        check(game.set_junction(20, 0, true, error), "the dispatcher locks it at branch 0: " + error);
        check(has(take(rec, 1), P(Package::junction)) && has(take(rec, 2), P(Package::junction)), "both get the locked junction");
        game.received(2, P(Package::junction), w.data());
        got = take(rec, 2);
        check(got.size() == 1 && got[0].body[4] == 0 && got[0].body[8] == 1, "a locked junction refused: Bob gets its state back");
        check(take(rec, 1).empty(), "and Alice gets nothing");
        check(game.set_junction(20, std::nullopt, false, error), "unlocked");
        take(rec, 1);
        take(rec, 2);
    }

    // Asked for a train's look: to its owner alone.
    game.received(2, P(Package::state_request), net::encode_state_request(2, 1));
    check(ids(take(rec, 1)) == std::vector<std::uint16_t>{P(Package::state_request)}, "Alice is asked for her train's look");

    // Coupling: Alice's train 1 runs into Bob's train 2 (her front, his rear).
    game.received(1, P(Package::couple), net::encode_couple({.a = 1, .b = 2, .end_a = 1, .end_b = 0}));
    got = take(rec, 2);
    check(ids(got) == std::vector<std::uint16_t>{P(Package::train_assign), P(Package::couple), P(Package::train_id), P(Package::train_pos)},
          "Bob: not yours, the coupling, the id change, the merged place (in that order)");
    check(!got.empty() && got[0].body[4] == 0, "Bob's 0x4601 says not his");
    check(take(rec, 1).empty(), "Alice gets nothing back");
    {
        const auto trains = game.trains();
        check(trains.size() == 1 && trains[0].id == 1 && trains[0].cars == 4 && trains[0].owner == "Alice", "one train of four cars, Alice's");
    }
    game.received(1, P(Package::couple), net::encode_couple({.a = 1, .b = 2, .end_a = 1, .end_b = 0}));
    check(take(rec, 2).empty(), "a second coupling of a train gone: nothing");

    // Cutting: the part keeps the owner; its sender gets the server's id.
    game.received(1, P(Package::uncouple), net::encode_uncouple({.train = 1, .car = 2, .part = 100244, .side = 0}));
    got = take(rec, 1);
    check(got.size() == 1 && got[0].id == P(Package::train_id), "Alice: her part's id from the server");
    if (const auto change = got.empty() ? std::nullopt : net::decode_train_id_change(got[0].body)) {
        check(change->first == 100244 && change->second == 2, "local 100244 -> 2");
    }
    got = take(rec, 2);
    check(ids(got) == std::vector<std::uint16_t>{P(Package::uncouple), P(Package::train_pos)}, "Bob: the cut with the new id, the part's place");
    {
        const auto trains = game.trains();
        check(trains.size() == 2 && trains[0].cars == 2 && trains[1].cars == 2 && trains[1].owner == "Alice", "two trains of two cars, both Alice's");
    }

    // Pause: announced, then nothing goes out; time every 300 s.
    game.set_paused(true);
    check(ids(take(rec, 1)) == std::vector<std::uint16_t>{P(Package::pause)} && ids(take(rec, 2)) == std::vector<std::uint16_t>{P(Package::pause)},
          "pause to both");
    now = 400.0;
    game.tick(now);
    check(take(rec, 1).empty(), "nothing while paused");
    game.set_paused(false);
    take(rec, 1);
    take(rec, 2);
    game.tick(now += 1.0);
    game.tick(now += 301.0);
    check(has(take(rec, 1), P(Package::time)), "the time every 300 s");
    take(rec, 2);

    // A message to Bob alone.
    check(game.message("Bob", u"Привет", error), "message sent");
    got = take(rec, 2);
    check(got.size() == 1 && got[0].id == P(Package::message), "Bob gets 0x4611");
    if (!got.empty()) {
        net::WireReader r(got[0].body);
        check(r.text_u32() == u"Привет", "with its text");
    }
    check(take(rec, 1).empty(), "Alice does not");

    // Alice leaves and comes back: her trains again, then the world.
    game.closed(1);
    take(rec, 2);
    game.connected(3, "127.0.0.1:3");
    game.received(3, P(Package::login), login(u"Alice"));
    check(take_reply(rec, 3).has_value(), "Alice again");
    got = take(rec, 3);
    check(got.size() == 2 && got[0].id == P(Package::train_assign) && got[1].id == P(Package::train_assign), "her two trains assigned again");
    game.received(3, P(Package::snapshot), {});
    check(net::read_snapshot(rec.out[3]).snapshot.trains.size() == 2, "and the world");
    rec.out[3].clear();

    // The same name while connected: the new connection takes over.
    game.connected(4, "127.0.0.1:4");
    game.received(4, P(Package::login), login(u"Alice"));
    check(rec.closed.contains(3) && take_reply(rec, 4).has_value(), "a second login of Alice closes the first");

    // Flooding.
    game.connected(5, "127.0.0.1:5");
    game.received(5, P(Package::login), login(u"Flood"));
    take_reply(rec, 5);
    for (int i = 0; i < 6000 && !rec.closed.contains(5); ++i) game.received(5, P(Package::object_state), net::Bytes(24));
    check(rec.closed.contains(5) && rec.out[5].size() >= 2 && rec.out[5][0] == 0x06 && rec.out[5][1] == 0x47, "a flood: 0x4706 and dropped");

    // The dispatcher's console: from this computer, the world with no train,
    // its commands and the game's state.
    game.set_console_commands([](const std::string_view line) { return "ran: " + std::string(line); });
    game.connected(6, "127.0.0.1:6");
    game.received(6, P(Package::console_login), login(u"DSP"));
    check(take_reply(rec, 6).has_value() && !rec.closed.contains(6), "a local console logs in");
    game.received(6, P(Package::snapshot), {});
    check(net::read_snapshot(rec.out[6]).snapshot.trains.size() == 2, "it gets the world with no train of its own");
    rec.out[6].clear();
    {
        net::WireWriter w;
        w.text_u32(u"trains");
        game.received(6, P(Package::console_command), w.data());
        got = take(rec, 6);
        check(got.size() == 1 && got[0].id == P(Package::console_reply), "a command answered");
        if (!got.empty()) {
            net::WireReader r(got[0].body);
            check(r.text_u32() == u"ran: trains", "with what it printed");
        }
        // From a player it is nothing.
        take(rec, 4);
        game.received(4, P(Package::console_command), w.data());
        check(take(rec, 4).empty(), "a player's command is dropped");
        game.tick(now += 1.5);
        got = take(rec, 6);
        bool state = false;
        for (const auto& g : got) {
            if (g.id != P(Package::console_state)) continue;
            const auto s = net::decode_console_state(g.body);
            state = s && s->trains.size() == 2 && s->users.size() >= 3;
            if (s) {
                for (const auto& t : s->trains) state = state && t.owner != 0; // both Alice's
            }
        }
        check(state, "the console gets the game's state: the trains with their owner");

        // A consist by its cars, from the console only.
        net::ConsolePlace place;
        place.consist = u"Мой состав";
        place.max_velocity = 33.3f;
        place.cars = {{.folder = u"mdd_ed9m", .name = u"ed9m_g", .engine = true, .cold = true, .handbrake = 50.0f},
                      {.folder = u"mdd_ed9m", .name = u"ed9m_p", .flipped = true}};
        place.node = 1;
        place.offset = 20.5;
        place.forward = false;
        const auto bytes = net::encode_console_place(place);
        const auto back = net::decode_console_place(bytes);
        check(back && back->consist == place.consist && back->cars.size() == 2 && back->cars[0].engine && back->cars[0].cold &&
                  back->cars[1].flipped && back->cars[0].handbrake == 50.0f && back->node == 1 && back->offset == 20.5 && !back->forward,
              "console_place reads back");
        std::size_t placed = 0;
        game.set_console_place([&](const net::ConsolePlace& p) {
            ++placed;
            return "placed " + std::to_string(p.cars.size());
        });
        game.received(4, P(Package::console_place), bytes);
        check(placed == 0, "a player's consist is dropped");
        game.received(6, P(Package::console_place), bytes);
        got = take(rec, 6);
        check(placed == 1 && got.size() == 1 && got[0].id == P(Package::console_reply), "the console's consist is placed and answered");
    }

    // The console.
    bool quit = false;
    check(game.kick("Bob", error) && rec.closed.contains(2), "Bob kicked");
    const auto words = server::command_words("add \"ED9M-0032 8 vag\" path 105");
    check(words.size() == 4 && words[1] == "ED9M-0032 8 vag", "quoted words keep their spaces");
    check(server::parse_time_of_day("06:30") == 6.5 * 3600.0 && !server::parse_time_of_day("25:00"), "times of day");
    static_cast<void>(quit);
}

// RTS's hand route from a console (0x4619): laid along the junctions as they
// stand, its junction held, the distance sent to everyone; -1 cancels.
void hand_route() {
    auto network = switch_track();
    const auto car = car_definition();
    Recorder rec;
    server::ServerOptions options;
    options.route_id = "testroute";
    options.consoles = true;
    server::GameServer game(rec, options, *network, nullptr);
    std::string error;
    game.tick(0.0);
    check(game.add_train(new_train(car, 1, 40.0, 101), error) == 1u, "a train for the route");
    game.connected(1, "127.0.0.1:1");
    game.received(1, P(Package::console_login), login(u"DSP"));
    take_reply(rec, 1);
    game.received(1, P(Package::snapshot), {});
    rec.out[1].clear();
    // The console throws junction 20 to branch 1 (as RTS's console), then the length.
    {
        net::WireWriter w;
        w.u32(19);
        w.u32(1);
        w.u8(0);
        game.received(1, P(Package::junction), w.data());
    }
    check(network->junction(20)->selected == 1, "the console's junction thrown");
    {
        net::WireWriter w;
        w.u32(1);
        w.f32(100.0f);
        w.u8(0);
        game.received(1, P(Package::route_left), w.data());
    }
    bool sent = false;
    for (const auto& g : take(rec, 1)) {
        if (g.id != P(Package::route_left)) continue;
        net::WireReader r(g.body);
        const auto train = r.u32();
        const auto left = r.f32();
        sent = train == 1 && std::abs(left - 100.0f) < 0.5f;
    }
    check(sent, "everyone gets the route's length (0x4619)");
    const auto held = game.route_junctions(1);
    check(held.size() == 1 && held[0] == 20, "its junction held: " + std::to_string(held.size()));
    // Along the junction as it stands: branch 1.
    {
        net::WireWriter w;
        w.u32(19);
        w.u32(0);
        w.u8(0);
        game.received(1, P(Package::junction), w.data()); // the dispatcher may still throw it
    }
    // Cancelled: -1 to everyone, the junction free.
    take(rec, 1);
    {
        net::WireWriter w;
        w.u32(1);
        w.f32(-1.0f);
        w.u8(0);
        game.received(1, P(Package::route_left), w.data());
    }
    bool cancelled = false;
    for (const auto& g : take(rec, 1)) {
        if (g.id != P(Package::route_left)) continue;
        net::WireReader r(g.body);
        r.u32();
        cancelled = r.f32() < 0.0f;
    }
    check(cancelled && game.route_junctions(1).empty(), "cancelled: -1, the junction free");
}

// Over TCP with openRTS's own client: the reply, the assignment, the world.
void over_tcp() {
    auto network = switch_track();
    const auto car = car_definition();
    server::TcpServer tcp;
    std::string error;
    if (!tcp.listen(0, error, true)) {
        check(false, "listen: " + error);
        return;
    }
    server::ServerOptions options;
    options.route_id = "testroute";
    options.description = server::game_description("TestRoute", 2);
    options.season = 2;
    options.counts = {20, 0, 0, 0};
    server::GameServer game(tcp, options, *network, nullptr);
    std::atomic<bool> done = false;
    std::thread pump([&] {
        const auto start = std::chrono::steady_clock::now();
        while (!done) {
            for (auto& e : tcp.take_events()) {
                if (e.kind == server::NetEvent::Kind::connected) game.connected(e.connection, e.address);
                if (e.kind == server::NetEvent::Kind::package) game.received(e.connection, e.id, e.body);
                if (e.kind == server::NetEvent::Kind::closed) game.closed(e.connection);
            }
            // The dispatcher gives the player a train once it has logged in.
            static bool given = false;
            if (!given) {
                for (const auto& u : game.users()) {
                    if (u.nick != "Looper" || !u.connected) continue;
                    std::string why;
                    if (const auto id = game.add_train(new_train(car, 1, 45.0, 101), why)) game.assign(*id, "Looper", why);
                    given = true;
                }
            }
            game.tick(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    net::RtsConnection client;
    const auto result = client.connect("127.0.0.1:" + std::to_string(tcp.port()), u"Looper", 5000);
    check(result.ok(), "openRTS's client logs in over TCP: " + result.error);
    if (result.ok()) {
        check(result.reply->route == u"testroute" && result.reply->season == 2 && result.reply->counts[0] == 20, "its reply");
        bool asked = false, ended = false;
        net::WireWriter world;
        bool collecting = false;
        const auto start = std::chrono::steady_clock::now();
        while (!ended && std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
            for (auto& package : client.receive()) {
                if (!asked) {
                    const auto assign = package.id == P(Package::train_assign) ? net::decode_train_assign(package.body) : std::nullopt;
                    if (assign && assign->assign.mine) {
                        client.send(P(Package::snapshot), {});
                        asked = true;
                    }
                    continue;
                }
                if (package.id == P(Package::login) && package.body.empty()) collecting = true;
                if (!collecting) continue;
                world.frame(package.id, package.body);
                if (package.id == P(Package::snapshot)) ended = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(asked, "the dispatcher's train reaches the player");
        const auto read = net::read_snapshot(world.data());
        check(ended && read.ok() && read.snapshot.trains.size() == 1 && read.snapshot.trains[0].mine(), "the world comes whole with its train");
    }
    client.close();
    done = true;
    pump.join();
    tcp.stop();
}

// The numbers a TDB writes (TrackNodes ( n, TrItemTable ( n) and its
// junction nodes, read from its text, not through formats.
struct WrittenCounts {
    std::uint32_t nodes{}, items{}, junctions{};
};
WrittenCounts written_counts(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(input)), {});
    std::string text; // UTF-16: its ASCII half is enough for keywords and digits
    const bool wide = bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF;
    for (std::size_t i = wide ? 2 : 0; i < bytes.size(); i += wide ? 2 : 1) text.push_back(bytes[i]);
    const auto number_after = [&](const std::string& key) -> std::uint32_t {
        const auto at = text.find(key);
        if (at == std::string::npos) return 0;
        const auto open = text.find('(', at);
        return open == std::string::npos ? 0u : static_cast<std::uint32_t>(std::strtoul(text.c_str() + open + 1, nullptr, 10));
    };
    WrittenCounts counts{.nodes = number_after("TrackNodes"), .items = number_after("TrItemTable"), .junctions = 0};
    for (auto at = text.find("TrJunctionNode"); at != std::string::npos; at = text.find("TrJunctionNode", at + 1)) ++counts.junctions;
    return counts;
}

// Kuban as rts_server.exe plays it.
void content(const std::filesystem::path& root) {
    std::string error;
    auto route = server::ServerRoute::load(root, "kuban_rts", error);
    if (!route) {
        std::cout << "server content: Kuban_RTS not installed (" << error << "), skipped\n";
        return;
    }
    const auto& c = route->counts();
    // rts_server.exe's numbers were taken on the 2025 build's Kuban (its
    // TDB 1653 nodes). The 2026-11 build redrew its track: there the
    // counts are checked against the numbers its own files write, and the
    // snapshot against the route as read.
    const auto tdb = written_counts(root / "ROUTES" / "Kuban_RTS" / "Kuban_RTS.tdb");
    const bool recorded = c.nodes == 1653;
    std::cout << "server content: Kuban " << c.nodes << " nodes, " << c.items << " items, " << c.substations << " substations, "
              << c.sections << " sections, " << (route->signals() ? route->signals()->heads().size() : 0) << " signal heads; "
              << (recorded ? "the Kuban rts_server.exe was recorded on" : "not the Kuban rts_server.exe was recorded on: checked against its files")
              << '\n';
    if (recorded) {
        check(c.items == 4074 && c.substations == 3 && c.sections == 3,
              "Kuban's counts as rts_server.exe sent them (1653, 4074, 3, 3): " + std::to_string(c.nodes) + ", " + std::to_string(c.items) +
                  ", " + std::to_string(c.substations) + ", " + std::to_string(c.sections));
        check(route->signals() && route->signals()->heads().size() == 1951, "1951 signal heads, as the live snapshot");
    } else {
        check(c.nodes == tdb.nodes && c.items == tdb.items && c.substations > 0 && c.sections > 0,
              "Kuban's counts as its TDB writes them (" + std::to_string(tdb.nodes) + ", " + std::to_string(tdb.items) + "): " +
                  std::to_string(c.nodes) + ", " + std::to_string(c.items));
    }
    Recorder rec;
    server::ServerOptions options;
    options.route_id = "kuban_rts";
    options.counts = c;
    server::GameServer game(rec, options, route->network(), route->signals());
    double now = 0.0;
    game.tick(now);
    game.connected(1, "127.0.0.1:1");
    game.received(1, P(Package::login), login(u"Alice"));
    take_reply(rec, 1);
    auto train = server::train_on_path(*route, "ED9M-0032 8 vag", "Краснодар-1 п.4 (платф) - Староминская п.3 (платф)", 101, error);
    check(train.has_value(), "an ED9M at a Kuban path's start: " + error);
    if (!train) return;
    check(train->records.size() == 8 && train->records[0].kind == 1 && train->records[0].mass > 10000.0f, "its cars' records");
    const auto id = game.add_train(std::move(*train), error);
    check(id.has_value() && game.assign(*id, "Alice", error), "placed and assigned: " + error);
    take(rec, 1);
    game.received(1, P(Package::snapshot), {});
    const auto read = net::read_snapshot(rec.out[1]);
    rec.out[1].clear();
    if (recorded) {
        check(read.ok() && read.snapshot.junctions.size() == 555 && read.snapshot.signals.size() == 1951, "the world: 555 junctions, 1951 heads");
    } else {
        check(read.ok() && read.snapshot.junctions.size() == tdb.junctions && route->signals()
                  && read.snapshot.signals.size() == route->signals()->heads().size(),
              "the world: every junction of the TDB (" + std::to_string(tdb.junctions) + ") and every signal head: " +
                  std::to_string(read.snapshot.junctions.size()) + ", " + std::to_string(read.snapshot.signals.size()));
    }
    for (int i = 0; i < 40; ++i) game.tick(now += 0.05);
    take(rec, 1);
    // A signal held at stop from the console.
    const auto& heads = route->signals()->heads();
    std::uint32_t item = 0;
    bool found = false;
    for (const auto& head : heads) {
        if (head.placed && head.state > 0.0f) {
            item = head.track_item;
            found = true;
            break;
        }
    }
    check(found, "a head showing more than stop");
    if (found) {
        check(game.set_signal(item, formats::SignalAspect::stop, error), "held at stop: " + error);
        for (int i = 0; i < 4; ++i) game.tick(now += 0.05);
        bool sent = false;
        for (const auto& g : take(rec, 1)) {
            if (g.id != P(Package::signal_head)) continue;
            const auto head = net::decode_signal_head(g.body);
            sent = sent || (head && head->item == item && head->state == 0.0f && head->mode == 0.0f);
        }
        check(sent, "the players get it: 0x460C stop, mode 0 (manual)");
    }
    // A route from the console: to a named signal ahead, its junctions held.
    {
        std::string result;
        std::string reached;
        std::size_t tried = 0;
        for (const auto& [name, items] : route->signal_names()) {
            static_cast<void>(items);
            if (++tried > 400) break;
            if (game.set_route(*id, {route->signal_items(name)}, result)) {
                reached = name;
                break;
            }
        }
        check(!reached.empty(), "a route to some named signal: " + result);
        std::cout << "server content: route to " << reached << ": " << result << '\n';
        const auto held = game.route_junctions(*id);
        check(!held.empty(), "its junctions are held");
        // A player cannot throw one of them.
        for (const auto n : held) {
            const auto* node = route->network().node(n);
            if (!node || node->branch_count() != 2) continue;
            const auto selected = route->network().junction(n)->selected;
            take(rec, 1);
            net::WireWriter w;
            w.u32(n - 1);
            w.u32(1 - selected);
            w.u8(0);
            game.received(1, P(Package::junction), w.data());
            check(route->network().junction(n)->selected == selected, "a junction on the route is refused to the player");
            const auto got = take(rec, 1);
            check(got.size() == 1 && got[0].id == P(Package::junction) && got[0].body[4] == selected, "who gets its state back");
            break;
        }
        check(game.cancel_route(*id, error) && game.route_junctions(*id).empty() && !game.cancel_route(*id, error), "the route freed once");
        take(rec, 1);
    }
    // The console's consist by its cars, at a place of the track.
    {
        net::ConsolePlace place;
        place.consist = u"ED9M (the console's)";
        for (const auto* name : {"mdd_ed9m-0032_g", "mdd_ed9m-0032_m", "mdd_ed9m-0032_p"}) {
            place.cars.push_back({.folder = u"mdd_ed9m-0032", .name = net::from_utf8(name), .engine = true}); // all .eng
        }
        // At a place well along a long node, away from the trains.
        std::uint32_t node = 0;
        for (auto n = static_cast<std::uint32_t>(route->network().node_count() - 1); n > 0 && !node; --n) {
            const auto* candidate = route->network().node(n);
            if (candidate && candidate->kind == formats::TrackNodeKind::vector && candidate->length > 400.0) node = n;
        }
        place.node = node;
        place.offset = 300.0;
        const auto answer = server::place_cars(game, *route, place);
        check(answer.rfind("train ", 0) == 0, "the cars placed: " + answer);
        bool standing = false;
        for (const auto& t : game.trains()) standing = standing || (t.cars == 3 && t.kind == 0 && t.owner.empty());
        check(standing, "a STATIC train of three cars, nobody's");
        const auto again = server::place_cars(game, *route, place);
        check(again.find("occupied") != std::string::npos, "not placed again where it stands: " + again);
        take(rec, 1);
    }
    // A junction thrown from the console.
    std::uint32_t junction = 0;
    for (std::uint32_t n = 1; n < route->network().node_count() && !junction; ++n) {
        const auto* node = route->network().node(n);
        if (node && node->kind == formats::TrackNodeKind::junction && node->branch_count() == 2 && !route->network().junction(n)->occupied) junction = n;
    }
    const auto before = route->network().junction(junction)->selected;
    check(junction && game.set_junction(junction, 1 - before, std::nullopt, error), "a junction thrown: " + error);
    bool thrown = false;
    for (const auto& g : take(rec, 1)) {
        if (g.id != P(Package::junction)) continue;
        const auto j = net::decode_junction(g.body);
        thrown = thrown || (j && j->node() == junction && j->branch == 1 - before);
    }
    check(thrown, "the players get it");
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1) {
        content(argv[1]);
        if (failures == 0) std::cout << "server content: all checks passed\n";
        return failures == 0 ? 0 : 1;
    }
    game();
    hand_route();
    over_tcp();
    if (failures == 0) std::cout << "server: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
