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

#include <openrts/formats/route_database.hpp>
#include <openrts/formats/track_database.hpp>
#include <openrts/formats/track_sections.hpp>
#include <openrts/net/connection.hpp>
#include <openrts/net/packages.hpp>
#include <openrts/net/play.hpp>
#include <openrts/net/snapshot.hpp>
#include <openrts/net/world.hpp>
#include <openrts/server/console.hpp>
#include <openrts/server/game_server.hpp>
#include <openrts/server/server_route.hpp>
#include <openrts/server/session.hpp>
#include <openrts/server/station_routes.hpp>
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
    check(server::parse_weather("storm") == 3 && server::parse_weather("rain") == 2 && !server::parse_weather("hail"), "weather names");
    check(net::openrts_hello_storm(net::encode_openrts_hello(net::openrts_features, true)) &&
              !net::openrts_hello_storm(net::encode_openrts_hello()) && net::decode_openrts_hello(net::encode_openrts_hello(1, true)) == 1,
          "the hello's thunderstorm byte");
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
        // A hand switch thrown on foot: 0x4642, the junction and the branch.
        net::WireWriter hand;
        hand.u32(19);
        hand.u32(1);
        game.received(2, P(Package::junction_request), hand.data());
        got = take(rec, 1);
        check(got.size() == 1 && got[0].id == P(Package::junction) && got[0].body[4] == 1, "Bob's hand switch (0x4642): Alice gets branch 1");
        check(network->junction(20)->selected == 1, "the hand switch is thrown on the server");
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

    // The server's own cut and coupling (the dispatcher's, as rts_server.exe
    // couples a LOOSE train run into another, 0x4501E0): to everyone, the
    // owner too, who must do the same with the train or its positions are
    // dropped (the car count).
    {
        check(game.cut(1, 1, error), "the server cuts train 1 before car 1: " + error);
        got = take(rec, 1);
        check(ids(got) == std::vector<std::uint16_t>{P(Package::uncouple), P(Package::train_pos)}, "Alice, the owner, gets the server's cut");
        const auto cut = got.empty() ? std::nullopt : net::decode_uncouple(got[0].body);
        check(cut && cut->train == 1 && cut->car == 1 && cut->side == 0 && cut->part != 0, "the cut names her train and the part");
        check(ids(take(rec, 2)) == std::vector<std::uint16_t>{P(Package::uncouple), P(Package::train_pos)}, "Bob gets it too");
        const auto part = cut ? cut->part : 0u;
        check(game.trains().size() == 3, "three trains");
        check(game.couple(1, part, error), "the server joins them again: " + error);
        got = take(rec, 1);
        check(ids(got) == std::vector<std::uint16_t>{P(Package::couple), P(Package::train_pos)}, "Alice, the owner of A, gets the server's coupling");
        const auto c = got.empty() ? std::nullopt : net::decode_couple(got[0].body);
        check(c && c->a == 1 && c->b == part, "the coupling names her train and the part");
        take(rec, 2);
        const auto trains = game.trains();
        check(trains.size() == 2 && trains[0].cars == 2, "two trains of two cars again");
        check(!game.couple(1, 1, error) && !game.cut(1, 0, error), "no coupling with itself, no cut before car 0");
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
// Helpers (помощник машиниста) and openRTS players (docs/server.md, "Helpers").
void helpers() {
    auto network = switch_track();
    const auto car = car_definition();
    Recorder rec;
    server::ServerOptions options;
    options.route_id = "testroute";
    options.description = server::game_description("TestRoute", 1);
    server::GameServer game(rec, options, *network, nullptr);
    std::string error;
    double now = 0.0;
    game.tick(now);
    const auto hello = net::encode_openrts_hello();
    const auto notes = [&](const server::ConnectionId c) {
        std::vector<net::HelperNote> out;
        for (const auto& g : take(rec, c)) {
            if (g.id != P(Package::helper)) continue;
            if (const auto n = net::decode_helper_note(g.body)) out.push_back(*n);
        }
        return out;
    };
    // Alice (openRTS) drives train 1.
    game.connected(1, "127.0.0.1:1");
    game.received(1, P(Package::login), login(u"Alice"));
    take_reply(rec, 1);
    game.received(1, P(Package::openrts_hello), hello);
    {
        const auto got = ids(take(rec, 1));
        std::string list;
        for (const auto id : got) list += " " + std::to_string(id);
        check(got == std::vector<std::uint16_t>{P(Package::openrts_hello)}, "an openRTS player's hello is answered:" + list);
    }
    check(game.add_train(new_train(car, 1, 45.0, 101), error) == 1u, "train 1: " + error);
    check(game.assign(1, "Alice", error), "train 1 to Alice: " + error);
    game.received(1, P(Package::snapshot), {});
    rec.out[1].clear();
    // Bob (openRTS) walks in without a train; Carl (rts1.exe, no hello) waits.
    game.connected(2, "127.0.0.1:2");
    game.received(2, P(Package::login), login(u"Bob"));
    take_reply(rec, 2);
    game.received(2, P(Package::openrts_hello), hello);
    rec.out[2].clear();
    game.received(2, P(Package::snapshot), {});
    check(net::read_snapshot(rec.out[2]).ok(), "an openRTS player gets the world without a train");
    rec.out[2].clear();
    game.connected(3, "127.0.0.1:3");
    game.received(3, P(Package::login), login(u"Carl"));
    take_reply(rec, 3);
    game.received(3, P(Package::snapshot), {});
    check(rec.out[3] == net::Bytes{0x03, 0x47}, "rts1.exe without a train waits (0x4703): " + std::to_string(rec.out[3].size()) + " bytes");
    rec.out[3].clear();
    // Bob asks; Alice is asked; she takes him.
    game.received(2, P(Package::helper), net::encode_helper_note({.kind = net::HelperKind::ask, .train = 1}));
    auto alice = notes(1);
    auto bob = notes(2);
    check(alice.size() == 1 && alice[0].kind == net::HelperKind::asked && alice[0].nick == u"Bob" && alice[0].train == 1,
          "Alice: Bob asks to help on train 1");
    check(bob.size() == 1 && bob[0].kind == net::HelperKind::helping && bob[0].value == static_cast<std::uint8_t>(net::HelperState::waiting) &&
              bob[0].nick == u"Alice",
          "Bob: waiting for Alice");
    const auto bob_id = alice.empty() ? 0u : alice[0].user;
    game.received(1, P(Package::helper), net::encode_helper_note({.kind = net::HelperKind::answer, .train = 1, .user = bob_id, .value = 1}));
    alice = notes(1);
    bob = notes(2);
    check(bob.size() == 1 && bob[0].value == static_cast<std::uint8_t>(net::HelperState::helping), "Bob helps");
    check(alice.size() == 1 && alice[0].kind == net::HelperKind::helper && alice[0].value == 1 && alice[0].user == bob_id, "Alice: Bob is her helper");
    // Her cab goes to him, his hand to her; nobody else's.
    const auto cab = net::encode_helper_cab({.train = 1, .car = 0, .packed = {9, 9}});
    game.received(1, P(Package::helper_cab), cab);
    check(ids(take(rec, 2)) == std::vector<std::uint16_t>{P(Package::helper_cab)}, "Alice's cab reaches Bob");
    game.received(2, P(Package::helper_cab), cab);
    check(take(rec, 1).empty() && take(rec, 2).empty(), "Bob's cab of a train not his goes nowhere");
    const auto hand = net::encode_helper_input({.kind = net::HelperInputKind::operate, .train = 1, .car = 0, .id = 3, .state = 1});
    game.received(2, P(Package::helper_input), hand);
    check(ids(take(rec, 1)) == std::vector<std::uint16_t>{P(Package::helper_input)}, "Bob's hand reaches Alice");
    game.received(3, P(Package::helper_input), hand);
    check(take(rec, 1).empty(), "a player helping nobody works no cab");
    check(game.users().size() == 3 && game.users()[1].helping == 1, "the users: Bob helps on train 1");
    // Train 1 taken from Alice: the help ends.
    check(game.unassign(1, error), "train 1 back: " + error);
    game.tick(now += 0.1);
    bob = notes(2);
    alice = notes(1);
    check(bob.size() == 1 && bob[0].kind == net::HelperKind::helping && bob[0].value == static_cast<std::uint8_t>(net::HelperState::ended),
          "Bob's help ends with the train taken away");
    check(alice.size() == 1 && alice[0].kind == net::HelperKind::helper && alice[0].value == 0, "Alice: Bob helps no more");
    // Alice again; Bob asks; she refuses.
    check(game.assign(1, "Alice", error), "train 1 to Alice again");
    rec.out[1].clear();
    game.received(2, P(Package::helper), net::encode_helper_note({.kind = net::HelperKind::ask, .train = 1}));
    notes(1);
    notes(2);
    game.received(1, P(Package::helper), net::encode_helper_note({.kind = net::HelperKind::answer, .train = 1, .user = bob_id, .value = 0}));
    bob = notes(2);
    check(bob.size() == 1 && bob[0].value == static_cast<std::uint8_t>(net::HelperState::refused), "Bob refused");
    // Carl (no hello) cannot: helpers are openRTS's.
    check(!game.set_helper("Carl", 1, error), "rts1.exe is no helper yet: " + error);
    // The dispatcher makes Bob the helper; Alice leaves: it ends.
    check(game.set_helper("Bob", 1, error), "the dispatcher's helper: " + error);
    notes(1);
    bob = notes(2);
    check(bob.size() == 1 && bob[0].value == static_cast<std::uint8_t>(net::HelperState::helping), "Bob helps (the dispatcher)");
    game.closed(1);
    bob = notes(2);
    check(bob.size() == 1 && bob[0].value == static_cast<std::uint8_t>(net::HelperState::ended), "Alice gone: Bob's help ends");
}

// A station route's segment on the switch track: set, the train on it,
// the train gone past it (rts_server.exe 0x46A348's states 1, 3, 4).
void station_segments() {
    auto network = switch_track();
    const auto car = car_definition();
    // Point A on node 1 (60 m), track B on node 3 (20 m to 50 m); the route
    // A -> B through junction 20 at branch 1.
    formats::RouteDatabase db;
    formats::RoutePoint a{.name = "A", .type = 1, .station = "S"};
    a.point1.position = {.x = 0.0, .z = 60.0};
    formats::RoutePoint b{.name = "B", .type = 2, .station = "S"};
    b.point1.position = {.x = 1.5, .z = 120.0};
    b.point2 = formats::RouteTrackPlace{.position = {.x = 1.5, .z = 150.0}};
    db.points = {a, b};
    db.routes = {{.id = 7, .name = "A => B", .station = "S", .start = "A", .end = "B"}};
    db.paths = {{.route = 7, .priority = 1, .junctions = {{.position = {}, .value = 1}}}};
    const auto routes = server::StationRoutes::build(*network, db);
    check(routes.paths().size() == 1 && routes.paths()[0].forward && routes.paths()[0].junctions.size() == 1,
          "the station route laid through junction 20: " + routes.summary() + " " +
              (routes.paths().empty() ? std::string() : routes.paths()[0].problem));
    if (routes.paths().empty() || !routes.paths()[0].forward) return;

    Recorder rec;
    server::ServerOptions options;
    options.route_id = "testroute";
    options.consoles = true;
    server::GameServer game(rec, options, *network, nullptr);
    game.set_station_routes(&routes);
    std::string error;
    double now = 0.0;
    game.tick(now);
    check(game.add_train(new_train(car, 1, 40.0, 101), error) == 1u, "a train before the route");
    game.connected(1, "127.0.0.1:1");
    game.received(1, P(Package::login), login(u"Alice"));
    take_reply(rec, 1);
    check(game.assign(1, "Alice", error), "Alice drives it: " + error);
    game.received(1, P(Package::snapshot), {});
    rec.out[1].clear();
    game.connected(2, "127.0.0.1:2");
    game.received(2, P(Package::console_login), login(u"DSP"));
    take_reply(rec, 2);
    game.received(2, P(Package::snapshot), {});
    rec.out[2].clear();

    check(network->junction(20)->selected == 0, "junction 20 at its main branch");
    const auto id = game.add_segment({.train = 1, .route = 7}, error);
    check(id.has_value(), "the segment queued: " + error);
    game.tick(now += 0.05);
    auto list = game.segments();
    check(list.size() == 1 && list[0].state == 1 && list[0].thrown == 1, "set at the next step: its junction thrown");
    check(network->junction(20)->selected == 1 && game.route_junctions(1) == std::vector<std::uint32_t>{20},
          "junction 20 at branch 1, held for the train's route");
    // Where Alice says her train is (0x4603), from a train put there.
    const auto move_to = [&](const std::uint32_t node, const double front) {
        simulation::Train ghost({{.definition = &car}, {.definition = &car}});
        std::string why;
        check(ghost.place(*network, node, front, true, &why), "a place for the train: " + why);
        auto pos = net::position_of(ghost);
        if (!pos) return;
        pos->speed = 0.0f;
        game.received(1, P(Package::train_pos), net::encode_train_pos(1, *pos));
        game.tick(now += 0.05);
    };
    move_to(3, 40.0);
    list = game.segments();
    check(list.size() == 1 && list[0].state == 3, "the train on it: state 3");
    move_to(3, 95.0);
    list = game.segments();
    check(list.size() == 1 && list[0].state == 4, "the train past it: state 4");
    game.tick(now += 0.05);
    check(game.segments().empty(), "and it goes at the next step");
    bool states[5]{}, gone = false;
    for (const auto& g : take(rec, 2)) {
        if (g.id == P(Package::route_segment_state)) {
            if (const auto s = net::decode_route_segment_state(g.body); s && s->state < 5) states[s->state] = true;
        }
        gone = gone || g.id == P(Package::route_segment_cancel);
    }
    check(states[1] && states[3] && states[4] && gone, "the console got states 1, 3, 4 (0x450E), then 0x450D");
    // Refused: no such station route; a train that is not there.
    check(!game.add_segment({.train = 1, .route = 8}, error) && error == "no station route 8", "an unknown station route refused");
    check(!game.add_segment({.train = 9, .route = 7}, error) && error == "no train 9", "an unknown train refused");
}

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

// UTF-8 text as a UTF-16LE file with a BOM, and back.
std::string utf16_bytes(const std::string_view text) {
    std::string out("\xFF\xFE", 2);
    for (const auto c : net::from_utf8(text)) {
        out.push_back(static_cast<char>(c & 0xFF));
        out.push_back(static_cast<char>(c >> 8));
    }
    return out;
}

std::string utf8_of(const std::string_view bytes) {
    std::u16string text;
    for (std::size_t i = 2; i + 1 < bytes.size(); i += 2) {
        text.push_back(static_cast<char16_t>(static_cast<unsigned char>(bytes[i]) | (static_cast<unsigned char>(bytes[i + 1]) << 8)));
    }
    return net::to_utf8(text);
}

// The route database written as rts_server.exe's "save all" writes it: the
// units and the types replaced, the rest as it was.
void route_database() {
    const std::string text =
        "SIMISA@@@@@@@@@@JINX1R0t______\r\n\r\n"
        "RoutePoints ( 1 \r\n\tRoutePoint ( \r\n\t\tName ( \"1\" )\r\n\t\tPointType ( 2 )\r\n\t)\r\n)\r\n"
        "SignalTypeShownAsNormal ( \"t_old\" )\r\n"
        "SignalUnits ( 2 \r\n"
        "\tSignalUnit ( \r\n\t\tPosition ( -4497 14555 902.8710 15.3226 -891.6260 )\r\n\t\tNumSigItems ( 1 )\r\n"
        "\t\tName ( \"Ч1\")\r\n\t\tUnitFlags ( 00000001 )\r\n\t\tDefaultAspect ( STOP )\r\n\t)\r\n"
        "\tSignalUnit ( \r\n\t\tPosition ( -4497 14554 865.8020 14.8220 964.5350 )\r\n\t\tNumSigItems ( 2 )\r\n"
        "\t\tUnitFlags ( 00000004 )\r\n\t\tDefaultAspect ( STOP )\r\n\t)\r\n"
        ")";
    const auto read = formats::parse_route_database(utf16_bytes(text));
    check(read.ok() && read.signal_units.size() == 2 && read.signal_units[0].name == "Ч1" && read.signal_units[0].flags == 1 &&
              read.signal_units[1].flags == 4 && read.signal_units[1].items == 2 && read.shown_as_normal == std::vector<std::string>{"t_old"},
          "a route database read: units, names, flags, the type shown as normal");
    auto changed = read;
    changed.signal_units[0].flags = 0x5;
    changed.signal_units[0].default_aspect = "CLEAR_2";
    changed.shown_as_normal = {"T_Stop_R"};
    std::string error;
    const auto bytes = formats::write_route_database(utf16_bytes(text), changed, error);
    check(bytes.size() > 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE,
          "written as UTF-16LE with a BOM: " + error);
    const auto again = formats::parse_route_database(bytes);
    check(again.ok() && again.signal_units.size() == 2 && again.signal_units[0].flags == 5 && again.signal_units[0].default_aspect == "CLEAR_2" &&
              again.signal_units[0].name == "Ч1" && again.signal_units[1].flags == 4 &&
              std::abs(again.signal_units[1].position.z - 964.535) < 1e-3 && again.shown_as_normal == std::vector<std::string>{"T_Stop_R"},
          "read back: the units and the types as written");
    const auto written = utf8_of(bytes);
    check(written.find("RoutePoint ( \r\n\t\tName ( \"1\" )") != std::string::npos && written.find("t_old") == std::string::npos &&
              written.find("\t\tName ( \"Ч1\")\r\n\t\tUnitFlags ( 00000005 )\r\n\t\tDefaultAspect ( CLEAR_2 )") != std::string::npos &&
              written.rfind("SIMISA@@@@@@@@@@JINX1R0t______\r\n\r\nSignalTypeShownAsNormal ( \"T_Stop_R\" )\r\n", 0) == 0,
          "the rest as it was, the type after the header, the units as RTS writes them");
    const auto fresh = formats::write_route_database({}, changed, error);
    check(formats::parse_route_database(fresh).signal_units.size() == 2, "and a new one when the route had none");

    // The station routes' part, as Kuban's database writes it.
    const std::string routes_text =
        "SIMISA@@@@@@@@@@JINX1R0t______\r\n\r\n"
        "RoutePoints ( 2 \r\n"
        "\tRoutePoint ( \r\n\t\tName ( \"4БП\" )\r\n\t\tPointType ( 2 )\r\n\t\tFlags ( 00000008 )\r\n"
        "\t\tStationName ( \"Краснодар-1\" )\r\n\t\tPoint1 ( -4549 14591 919.0742 0.9338 751.3620 0 )\r\n"
        "\t\tPoint2 ( -4549 14591 801.5150 0.9338 1011.0000 1 )\r\n\t)\r\n"
        "\tRoutePoint ( \r\n\t\tName ( \"Н Краснодар-2\" )\r\n\t\tPointType ( 1 )\r\n\t\tFlags ( 00000002 )\r\n"
        "\t\tStationName ( \"Краснодар-1\" )\r\n\t\tPoint1 ( -4547 14591 -1096.8704 0.9338 -825.6983 1 )\r\n\t)\r\n"
        ")\r\n"
        "RouteTrainRoutes ( 1 \r\n\tTrainRoute ( \r\n\t\tID ( 558 )\r\n\t\tName ( \"НМ4БП => Н Краснодар-2\" )\r\n"
        "\t\tFlags ( 0000000a )\r\n\t\tStationName ( \"Краснодар-1\" )\r\n\t\tStart ( \"4БП\" )\r\n"
        "\t\tEnd ( \"Н Краснодар-2\" )\r\n\t)\r\n)\r\n"
        "RouteTrainPaths ( 1 \r\n\tTrainPath ( \r\n\t\tRouteID ( 558 )\r\n\t\tPriority ( 1 )\r\n\t\tStart ( 00000002 )\r\n"
        "\t\tPoints ( 2 \r\n\t\t\tPoint ( -4548 14591 734.0760 0.9338 -826.0130 0 )\r\n"
        "\t\t\tPoint ( -4548 14591 545.0100 0.9338 -807.3250 1 )\r\n\t\t)\r\n\t)\r\n)\r\n"
        "SignalUnits ( 0 \r\n)";
    const auto routes = formats::parse_route_database(utf16_bytes(routes_text));
    check(routes.points.size() == 2 && routes.points[0].name == "4БП" && routes.points[0].type == 2 && routes.points[0].flags == 8 &&
              routes.points[0].point2 && routes.points[0].point2->value == 1 && std::abs(routes.points[0].point2->position.z - 1011.0) < 1e-6 &&
              routes.points[1].type == 1 && routes.points[1].flags == 2 && !routes.points[1].point2,
          "route points: name, type, flags, station, Point1 / Point2 with their flags");
    check(routes.routes.size() == 1 && routes.routes[0].id == 558 && routes.routes[0].flags == 0xA && routes.routes[0].start == "4БП" &&
              routes.routes[0].end == "Н Краснодар-2" && routes.routes[0].station == "Краснодар-1",
          "a station route: ID, flags, from, to");
    check(routes.paths.size() == 1 && routes.paths[0].route == 558 && routes.paths[0].priority == 1 && routes.paths[0].start == 2 &&
              routes.paths[0].junctions.size() == 2 && routes.paths[0].junctions[1].value == 1,
          "its path: the junctions with their branches");
    const auto kept = formats::parse_route_database(formats::write_route_database(utf16_bytes(routes_text), routes, error));
    check(kept.points.size() == 2 && kept.routes.size() == 1 && kept.paths.size() == 1, "and the save keeps them");
}

// Kuban as rts_server.exe plays it.
void content(const std::filesystem::path& root) {
    std::string error;
    // The server's own folder for the route database it saves.
    const auto data = std::filesystem::temp_directory_path() / "openrts_server_test";
    std::error_code fs_error;
    std::filesystem::remove_all(data, fs_error);
    auto route = server::ServerRoute::load(root, "kuban_rts", error, data);
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
    options.consoles = true;
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
    // The signals as rts_server.exe runs them: its units, their modes from
    // the route database; none open before a route.
    auto* signals = route->signals();
    std::size_t by_mode[3]{};
    std::size_t with_defaults = 0;
    for (std::uint32_t u = 0; u < signals->unit_count(); ++u) {
        ++by_mode[static_cast<int>(signals->unit_mode(u))];
        if (signals->defaults(u)) ++with_defaults;
    }
    std::cout << "server content: " << route->signal_modes() << '\n';
    {
        std::map<std::string, std::size_t> shown;
        for (std::uint32_t u = 0; u < signals->unit_count(); ++u) {
            if (signals->unit_mode(u) == simulation::UnitMode::automatic) ++shown[formats::to_string(signals->unit_aspect(u))];
        }
        std::cout << "server content: the automatic block units show";
        for (const auto& [aspect, count] : shown) std::cout << ' ' << aspect << " " << count;
        std::cout << '\n';
    }
    check(signals->dispatched() && signals->unit_count() > 1000 && by_mode[2] > 200 && by_mode[0] > 500 && by_mode[1] == 0,
          "Kuban's signal units: automatic block and manual from its route database, none open");
    check(with_defaults * 20 >= signals->unit_count() * 19, "nearly every unit has its SignalUnit");
    // At the load a manual unit shows what its scripts give (rts_server.exe's
    // hold of DefaultAspect acts on the console's selected unit, none then):
    // no head dark, as the live capture's first snapshot; Kuban's
    // "-also" heads red at STOP_AND_PROCEED, not "No" at STOP.
    {
        std::size_t dark = 0, red_also = 0, also = 0;
        for (const auto& head : signals->heads()) {
            dark += head.draw_state < 0.0f ? 1 : 0;
            if (!head.type || head.type->name.find("-also") == std::string::npos) continue;
            const auto unit = signals->unit_of_item(head.track_item);
            if (!unit || signals->unit_mode(*unit) != simulation::UnitMode::manual) continue;
            ++also;
            red_also += head.aspect() == formats::SignalAspect::stop_and_proceed ? 1 : 0;
        }
        std::cout << "server content: at the load " << dark << " heads dark, " << red_also << " of " << also << " manual -also heads red\n";
        check(dark == 0, "no signal head dark at the load");
        check(also > 0 && red_also * 2 > also, "manual -also heads red at the load");
    }
    // A console: every unit's 0x4618 after its world (rts_server.exe 0x41733C).
    game.connected(2, "127.0.0.1:2");
    game.received(2, P(Package::console_login), login(u"DSP"));
    take_reply(rec, 2);
    game.received(2, P(Package::snapshot), {});
    {
        // The snapshot's frames nest; the 0x4618s follow it: 17-byte frames.
        std::size_t controls = 0;
        bool right = true;
        const auto& bytes = rec.out[2];
        const std::uint8_t header[] = {0x18, 0x46, 17, 0, 0, 0};
        for (std::size_t at = 0; at + 17 <= bytes.size(); ++at) {
            if (!std::equal(std::begin(header), std::end(header), bytes.begin() + static_cast<std::ptrdiff_t>(at))) continue;
            const auto c4618 = net::decode_signal_control(net::ByteView(bytes).subspan(at + 6, 11));
            right = right && c4618 && c4618->unit == controls &&
                    c4618->mode == static_cast<float>(static_cast<int>(signals->unit_mode(c4618->unit)));
            ++controls;
            at += 16;
        }
        rec.out[2].clear();
        check(controls == signals->unit_count() && right, "the console gets every unit's 0x4618 in order: " + std::to_string(controls));
    }
    // Its 0x4618: an automatic block unit held at STOP, its flag set; then
    // automatic block again.
    {
        std::uint32_t unit = 0;
        while (unit < signals->unit_count() && signals->unit_mode(unit) != simulation::UnitMode::automatic) ++unit;
        game.received(2, P(Package::signal_control), net::encode_signal_control({.unit = unit, .aspect = 0, .mode = 0.0f, .pinned = 1}));
        check(signals->unit_mode(unit) == simulation::UnitMode::manual && signals->unit_pinned(unit) &&
                  signals->unit_aspect(unit) == formats::SignalAspect::stop,
              "the console's 0x4618 holds a unit at STOP, the dispatcher's");
        for (int i = 0; i < 4; ++i) game.tick(now += 0.05);
        bool back = false;
        for (const auto& g : take(rec, 2)) {
            if (g.id != P(Package::signal_control)) continue;
            const auto c4618 = net::decode_signal_control(g.body);
            back = back || (c4618 && c4618->unit == unit && c4618->mode == 0.0f && c4618->pinned == 1 && c4618->aspect == 0);
        }
        check(back, "the consoles get it back (0x4618, mode 0, the flag)");
        game.received(2, P(Package::signal_control), net::encode_signal_control({.unit = unit, .aspect = 0, .mode = 2.0f, .pinned = 0}));
        check(signals->unit_mode(unit) == simulation::UnitMode::automatic, "0x4618 with mode 2: automatic block again");
        for (int i = 0; i < 4; ++i) game.tick(now += 0.05);
        game.received(2, P(Package::signal_control), net::encode_signal_control({.unit = 99999, .mode = 2.0f}));
        bool refused = false;
        for (const auto& g : take(rec, 2)) refused = refused || g.id == P(Package::console_reply);
        check(refused, "a unit the server has not: the console told");
        take(rec, 1);
    }
    // A signal under automatic block held at stop from the console.
    std::uint32_t item = 0;
    bool found = false;
    for (std::uint32_t u = 0; u < signals->unit_count() && !found; ++u) {
        if (signals->unit_mode(u) != simulation::UnitMode::automatic || signals->unit_heads(u).empty()) continue;
        item = signals->unit_items(u).front();
        found = true;
    }
    check(found, "a signal under automatic block");
    if (found) {
        check(game.set_signal(item, formats::SignalAspect::stop, error), "held at stop: " + error);
        for (int i = 0; i < 4; ++i) game.tick(now += 0.05);
        // The unit's first head of its lowest function with a STOP shows it,
        // its other heads go dark (0x40C714).
        const auto unit = signals->unit_of_item(item);
        check(unit && signals->unit_aspect(*unit) == formats::SignalAspect::stop && signals->unit_pinned(*unit),
              "the signal's unit at STOP, the dispatcher's");
        const auto& items = unit ? signals->unit_items(*unit) : std::vector<std::uint32_t>{};
        bool sent = false;
        for (const auto& g : take(rec, 1)) {
            if (g.id != P(Package::signal_head)) continue;
            const auto head = net::decode_signal_head(g.body);
            sent = sent || (head && std::find(items.begin(), items.end(), head->item) != items.end() && head->state == 0.0f && head->mode == 0.0f);
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
        // Its manual signals facing the train are open now (0x40918C).
        std::vector<std::uint32_t> opened;
        for (std::uint32_t u = 0; u < signals->unit_count(); ++u) {
            if (signals->unit_mode(u) == simulation::UnitMode::open) opened.push_back(u);
        }
        check(!opened.empty() && result.find(std::to_string(opened.size()) + " signals opened") != std::string::npos,
              "the route opened its signals (" + std::to_string(opened.size()) + ")");
        for (int i = 0; i < 20; ++i) game.tick(now += 0.05);
        std::size_t proceed = 0;
        for (const auto u : opened) {
            const auto aspect = signals->unit_aspect(u);
            if (aspect > formats::SignalAspect::stop_and_proceed) ++proceed;
            std::cout << "server content: opened " << game.signal_report(signals->unit_items(u).front()) << '\n';
        }
        check(proceed > 0, "an opened signal shows a proceed aspect");
        bool sent_open = false;
        for (int i = 0; i < 4; ++i) game.tick(now += 0.05);
        for (const auto& g : take(rec, 1)) {
            if (g.id != P(Package::signal_head)) continue;
            const auto head = net::decode_signal_head(g.body);
            sent_open = sent_open || (head && head->mode == 1.0f);
        }
        check(sent_open, "the players get them: 0x460C with mode 1 (open)");
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
        bool closed = true;
        for (const auto u : opened) closed = closed && signals->unit_mode(u) == simulation::UnitMode::manual;
        check(closed, "cancelled: its signals closed (manual again)");
        take(rec, 1);
    }
    // Station routes (RoutePoints, TrainRoutes, TrainPaths) and the
    // dispatcher's segments of a train's route.
    {
        const auto& station_routes = route->station_routes();
        std::size_t both = 0, laid = 0;
        for (const auto& p : station_routes.paths()) {
            laid += (p.forward || p.backward) ? 1 : 0;
            both += (p.forward && p.backward) ? 1 : 0;
        }
        std::cout << "server content: " << station_routes.summary() << ", " << both << " both ways\n";
        check(station_routes.points().size() > 150 && station_routes.routes().size() > 700 && laid * 100 >= station_routes.paths().size() * 95,
              "Kuban's station routes laid on the track");
        game.set_station_routes(&station_routes);
        take(rec, 2);
        // The routes the ED9M can take ("Собрать маршрут"): from its head
        // the way it faces, then on from the end of one of them, and on.
        {
            const auto head = game.train_end(*id, false);
            check(head.has_value(), "the ED9M's head on the track");
            auto from = head;
            std::string chain;
            for (int step = 0; step < 4 && from; ++step) {
                const auto choices = station_routes.available(route->network(), *from, 60000.0, 300.0);
                std::cout << "server content: available " << step << ":";
                for (std::size_t k = 0; k < choices.size() && k < 8; ++k) {
                    const auto& r = station_routes.routes()[choices[k].route];
                    std::cout << " [" << r.data->id << " " << r.data->station << ": " << r.data->name << (choices[k].reverse ? " back" : "") << " @"
                              << static_cast<int>(choices[k].distance) << "]";
                }
                std::cout << " (" << choices.size() << ")\n";
                if (step == 0) {
                    check(std::any_of(choices.begin(), choices.end(),
                                      [&](const auto& c) { return station_routes.routes()[c.route].data->start == "4БП" && !c.reverse; }),
                          "routes from the ED9M's track (4БП) available from its head");
                }
                if (choices.empty()) break;
                // On along the one leading onto a stretch when there is one.
                const server::StationRoutes::Choice* pick = &choices.front();
                for (const auto& choice : choices) {
                    const auto& candidate = station_routes.routes()[choice.route];
                    const auto last = choice.reverse ? candidate.start : candidate.end;
                    if (last && (station_routes.points()[*last].data->flags & server::point_flag::stretch)) pick = &choice;
                }
                const auto& r = station_routes.routes()[pick->route];
                chain += (chain.empty() ? "" : " -> ") + r.data->station + ": " + r.data->name;
                const auto* leg = station_routes.leg_of(r, pick->reverse);
                from = leg ? leg->at(leg->length()) : std::nullopt;
            }
            std::cout << "server content: chain " << chain << "\n";
            check(chain.find(" -> ") != std::string::npos, "routes chained on from one's end");
        }
        // The first of Краснодар-1's routes from the ED9M's track that is set.
        std::uint32_t first = 0, first_route = 0;
        for (const auto& r : station_routes.routes()) {
            if (r.data->station != "Краснодар-1" || r.data->start != "4БП") continue;
            const auto made = game.add_segment({.train = *id, .route = r.data->id}, error);
            if (!made) continue;
            for (int i = 0; i < 6; ++i) game.tick(now += 0.05);
            const auto list = game.segments();
            if (!list.empty() && list.back().state == 1) {
                first = *made;
                first_route = r.data->id;
                break;
            }
            check(game.cancel_segment(*made, error), "a segment not set cancelled: " + error);
        }
        check(first != 0, "a segment from the ED9M's track is set");
        if (first) {
            const auto list = game.segments();
            const auto& s = list.back();
            std::cout << "server content: segment " << s.id << ": " << s.route << " " << s.name << ", " << s.thrown << " of " << s.junctions
                      << " junctions, " << static_cast<int>(s.length) << " m\n";
            check(s.thrown == s.junctions && !game.route_junctions(*id).empty(), "its junctions set and the train's route holding them");
            const auto* r = station_routes.route(first_route);
            const auto* path = r ? station_routes.path_of(*r, false) : nullptr;
            bool at_branches = path != nullptr;
            for (const auto& [node, branch] : path ? path->junctions : std::vector<std::pair<std::uint32_t, std::uint32_t>>{}) {
                at_branches = at_branches && route->network().junction(node)->selected == branch;
            }
            check(at_branches, "every junction of its path at its branch");
            std::size_t open = 0;
            for (std::uint32_t u = 0; u < signals->unit_count(); ++u) open += signals->unit_mode(u) == simulation::UnitMode::open ? 1 : 0;
            check(open > 0, "the route opened its signals: " + std::to_string(open));
            bool told = false, state = false;
            for (const auto& g : take(rec, 2)) {
                told = told || g.id == P(Package::route_segment);
                if (g.id == P(Package::route_segment_state)) {
                    const auto st = net::decode_route_segment_state(g.body);
                    state = state || (st && st->id == first && st->state == 1);
                }
            }
            check(told && state, "the console gets its 0x450B and 0x450E (state 1)");
            // The next one from where it ends; one from elsewhere refused.
            const auto& end_name = r->data->end;
            std::optional<std::uint32_t> next;
            for (const auto& other : station_routes.routes()) {
                if (other.data->station != r->data->station || other.data->start != end_name) continue;
                next = game.add_segment({.train = *id, .route = other.data->id}, error);
                if (next) break;
            }
            std::cout << "server content: the next segment from " << end_name << ": " << (next ? "queued" : error) << '\n';
            std::optional<std::uint32_t> wrong;
            for (const auto& other : station_routes.routes()) {
                if (other.data->station == r->data->station) continue;
                wrong = game.add_segment({.train = *id, .route = other.data->id}, error);
                break;
            }
            check(!wrong && error.find("no way") != std::string::npos, "a segment elsewhere refused: " + error);
            // A console's own (0x450B), then its cancel (0x450D).
            const auto& any = station_routes.routes().front();
            net::RouteSegment asked{.id = 0xFF000001u, .train = *id, .route = any.data->id, .station = net::from_utf8(any.data->station)};
            const auto& start_place = station_routes.points()[*any.start].data->point1.position;
            asked.tile_x = static_cast<std::int16_t>(start_place.tile_x);
            asked.tile_z = static_cast<std::int16_t>(start_place.tile_z);
            game.received(2, P(Package::route_segment), net::encode_route_segment(asked));
            bool replied = false;
            for (const auto& g : take(rec, 2)) {
                if (g.id != P(Package::route_segment_reply)) continue;
                const auto reply = net::decode_route_segment_reply(g.body);
                replied = reply && reply->request == 0xFF000001u;
                if (replied && reply->ok) game.cancel_segment(reply->id, error);
            }
            check(replied, "the console's 0x450B answered with 0x450C");
            asked.tile_x = static_cast<std::int16_t>(asked.tile_x + 1);
            game.received(2, P(Package::route_segment), net::encode_route_segment(asked));
            bool refused = false;
            for (const auto& g : take(rec, 2)) {
                if (g.id != P(Package::route_segment_reply)) continue;
                const auto reply = net::decode_route_segment_reply(g.body);
                refused = reply && !reply->ok && !reply->text.empty();
            }
            check(refused, "one of another route database (its start's tile) refused");
            // Cancelled: the train's route goes, its signals close.
            if (next) check(game.cancel_segment(*next, error), "the next one cancelled: " + error);
            {
                net::WireWriter w;
                w.u32(first);
                game.received(2, P(Package::route_segment_cancel), w.data());
            }
            check(game.segments().empty() && game.route_junctions(*id).empty(), "the console's 0x450D: the segment and the train's route gone");
            std::size_t still_open = 0;
            for (std::uint32_t u = 0; u < signals->unit_count(); ++u) still_open += signals->unit_mode(u) == simulation::UnitMode::open ? 1 : 0;
            check(still_open == 0, "its signals closed");
        }
        take(rec, 1);
        take(rec, 2);
    }
    // The console's signal actions on a manual unit: open, automatic block,
    // close (STOP_AND_PROCEED, as rts_server.exe's menu), its defaults.
    {
        std::uint32_t manual_item = 0;
        for (std::uint32_t u = 0; u < signals->unit_count() && !manual_item; ++u) {
            const auto value = signals->defaults(u);
            if (value && value->mode == simulation::UnitMode::manual && !signals->unit_heads(u).empty()) manual_item = signals->unit_items(u).front();
        }
        check(manual_item != 0, "a manual signal");
        const auto unit = *signals->unit_of_item(manual_item);
        check(game.control_signal(manual_item, server::GameServer::SignalAction::open, error) && signals->unit_mode(unit) == simulation::UnitMode::open,
              "opened: " + game.signal_report(manual_item));
        check(game.control_signal(manual_item, server::GameServer::SignalAction::automatic, error) &&
                  signals->unit_mode(unit) == simulation::UnitMode::automatic,
              "automatic block: " + game.signal_report(manual_item));
        check(game.control_signal(manual_item, server::GameServer::SignalAction::close, error) && signals->unit_mode(unit) == simulation::UnitMode::manual &&
                  signals->unit_aspect(unit) == formats::SignalAspect::stop_and_proceed,
              "closed: " + game.signal_report(manual_item));
        check(game.control_signal(manual_item, server::GameServer::SignalAction::defaults, error) &&
                  signals->unit_mode(unit) == simulation::UnitMode::manual && !signals->unit_pinned(unit),
              "back to its defaults: " + game.signal_report(manual_item));
        std::cout << "server content: " << game.signal_report(manual_item) << '\n';
        std::size_t count = 0;
        const auto* type = signals->heads()[signals->unit_heads(unit).front()].type;
        check(type && game.set_signal_type_mode(type->name, simulation::UnitMode::automatic, count, error) && count > 0 &&
                  signals->unit_mode(unit) == simulation::UnitMode::automatic,
              "a whole type under automatic block: " + std::to_string(count) + " " + error);
        check(game.restore_signal_defaults(error) && signals->unit_mode(unit) == simulation::UnitMode::manual, "every signal back to its defaults");
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

    // "Задать как умолчания", a type shown as normal, "save all" into the
    // server's folder, and a server loading it again.
    {
        std::uint32_t unit = 0;
        while (unit < signals->unit_count() && signals->unit_mode(unit) != simulation::UnitMode::manual) ++unit;
        const auto unit_item = signals->unit_items(unit).front();
        const auto* type = signals->heads()[signals->unit_heads(unit).front()].type;
        std::size_t count = 0;
        check(type && game.set_signal_type_shown(type->name, true, count, error) && count > 0 && signals->shown_as_normal(*type),
              "a type shown as normal: " + std::to_string(count) + " signals");
        check(game.control_signal(unit_item, server::GameServer::SignalAction::automatic, error) && game.set_signal_defaults(error) &&
                  signals->defaults(unit)->mode == simulation::UnitMode::automatic,
              "set as defaults: the unit's default is automatic block now");
        std::string result;
        check(route->save_signal_defaults(result) && std::filesystem::is_regular_file(route->saved_database_path()), "saved: " + result);
        std::cout << "server content: " << result << '\n';
        check(route->saved_database_path().string().find(data.string()) == 0, "into the server's folder, not the installation");
        std::ifstream input(route->saved_database_path(), std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        const auto text = utf8_of(bytes);
        check(text.find("RoutePoints ( 182") != std::string::npos || text.find("RoutePoints") != std::string::npos,
              "its RoutePoints kept");
        auto again = server::ServerRoute::load(root, "kuban_rts", error, data);
        check(again && again->signals() && again->signals()->unit_count() == signals->unit_count(), "the route loaded again: " + error);
        if (again && again->signals()) {
            const auto* s2 = again->signals();
            check(s2->unit_mode(unit) == simulation::UnitMode::automatic && s2->defaults(unit)->mode == simulation::UnitMode::automatic,
                  "the saved default read back: automatic block");
            check(s2->shown_as_normal(*type), "the type still shown as normal");
            if (again->signal_names() != route->signal_names()) {
                std::cout << "server content: names " << route->signal_names().size() << " then " << again->signal_names().size() << ':';
                std::size_t shown = 0;
                for (const auto& entry : route->signal_names()) {
                    if (std::find(again->signal_names().begin(), again->signal_names().end(), entry) != again->signal_names().end()) continue;
                    if (++shown > 6) break;
                    std::cout << " lost " << entry.first << "@" << entry.second;
                }
                shown = 0;
                for (const auto& entry : again->signal_names()) {
                    if (std::find(route->signal_names().begin(), route->signal_names().end(), entry) != route->signal_names().end()) continue;
                    if (++shown > 6) break;
                    std::cout << " new " << entry.first << "@" << entry.second;
                }
                std::cout << std::endl;
            }
            // One name per unit, as rts_server.exe writes them: a unit two
            // old SignalUnits named keeps one.
            check(again->signal_names().size() * 100 >= route->signal_names().size() * 99, "the names kept (one per unit)");
            std::size_t same = 0;
            for (std::uint32_t u = 0; u < s2->unit_count(); ++u) same += s2->unit_mode(u) == signals->unit_mode(u) ? 1 : 0;
            std::cout << "server content: loaded again, " << same << " of " << s2->unit_count() << " units in the same mode\n";
        }
        std::filesystem::remove_all(data, fs_error);
    }
}

// The running server as the program and its window have it: the route
// loaded, the port open, console lines, trains and segments on its map.
void session(const std::filesystem::path& root) {
    std::string error;
    std::vector<std::string> lines;
    const auto data = std::filesystem::temp_directory_path() / "openrts_server_session_test";
    check(!server::ServerSession::start({.root = root, .route = "no such route", .data = data, .port = 0, .local = true}, error, lines) &&
              !error.empty(),
          "a session refuses a route that is not there");
    error.clear();
    lines.clear();
    auto s = server::ServerSession::start({.root = root, .route = "Kuban_RTS", .data = data, .port = 0, .local = true}, error, lines);
    if (!s) {
        std::cout << "server session: Kuban_RTS not started (" << error << "), skipped\n";
        return;
    }
    check(s->port() != 0, "a session listens on a free port");
    check(!lines.empty() && lines.back().starts_with("listening on port"), "a session tells its port last");
    bool quit = false;
    const auto added = s->command("add \"ED9M-0032 8 vag\" \"Краснодар-1 п.4 (платф) - Староминская п.3 (платф)\"", quit);
    check(added.find("stands at the start") != std::string::npos && !quit, "a session takes console lines");
    double now = 0.0;
    for (int i = 0; i < 4; ++i) s->step(now += 0.05);
    const auto trains = s->game().trains();
    check(trains.size() == 1 && !s->game().train_cars(trains.front().id).empty(), "a session's train has its cars on the map");
    check(s->game().train_cars(99).empty() && !s->game().route_leg(99), "no cars or route for a train that is not there");
    // "Собрать маршрут" at the console: what the train can take next.
    const auto next = s->command("next " + std::to_string(trains.front().id), quit);
    check(next.find("552  Краснодар-1: НМ4БП => Н1") != std::string::npos && next.find("from the train") != std::string::npos,
          "next lists the routes from the train's head");
    const auto segment = s->command("segment " + std::to_string(trains.front().id) + " 552", quit);
    const auto after = s->command("next " + std::to_string(trains.front().id), quit);
    check(after.find("from the end of segment") != std::string::npos && after.find("Краснодар-1: ") != std::string::npos,
          "next goes on from the end of the train's last segment");
    for (int i = 0; i < 10; ++i) s->step(now += 0.05);
    const auto leg = s->game().route_leg(trains.front().id);
    check(leg && leg->length() > 900.0, "a segment's route leg is there for the map (" + segment + ")");
    // A route laid to a place: ahead of the train's head, not behind it.
    std::string result;
    const auto cars = s->game().train_cars(trains.front().id);
    if (const auto front = s->route().network().locate(cars.front().front, 1.0)) {
        (void)s->command("segment cancel 1", quit);
        for (int i = 0; i < 4; ++i) s->step(now += 0.05);
        const simulation::TrackLocation behind{front->node, front->offset, true};
        const bool one = s->game().lay_route_to(trains.front().id, behind, false, result);
        const bool two = s->game().lay_route_to(trains.front().id, simulation::TrackLocation{front->node, front->offset, false}, false, result);
        check(!one && !two, "no route to the place the head stands on (" + result + ")");
    }
    (void)s->command("quit", quit);
    check(quit, "quit stops a session's console");
    s.reset();
    std::error_code fs_error;
    std::filesystem::remove_all(data, fs_error);
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1) {
        content(argv[1]);
        session(argv[1]);
        if (failures == 0) std::cout << "server content: all checks passed\n";
        return failures == 0 ? 0 : 1;
    }
    route_database();
    game();
    helpers();
    station_segments();
    hand_route();
    over_tcp();
    if (failures == 0) std::cout << "server: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
