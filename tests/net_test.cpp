// The RTS wire format, package names, the startup snapshot, the packages of
// a game in play and a connection to a fake RTS server on the loopback
// (docs/net.md). With a game folder: RTS/multdata, the snapshot the client
// saved at its last login, reads without a problem and writes back byte for
// byte.
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>

#include <openrts/net/connection.hpp>
#include <openrts/net/packages.hpp>
#include <openrts/net/play.hpp>
#include <openrts/net/snapshot.hpp>
#include <openrts/net/wire.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using TestSocket = SOCKET;
#define close_test_socket closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using TestSocket = int;
#define close_test_socket ::close
#endif

namespace {

using namespace openrts::net;

int failures = 0;

void check(const bool condition, const std::string& what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

void wire_fields() {
    WireWriter w;
    w.u8(0xAB);
    w.u16(0x1234);
    w.i16(-2);
    w.u32(0xDEADBEEF);
    w.f32(41.26f);
    w.f64(166.9);
    w.text_u32(u"mdd_ed9m-0062\\mdd_ed9m-0062_g.eng");
    w.text_u32(u"");
    w.text_u16(u"Краснодар-2");
    const auto bytes = w.take();
    // An empty string is its length alone: no zero unit (0x461A's notes).
    check(bytes.size() == 1 + 2 + 2 + 4 + 4 + 8 + (4 + 33 * 2 + 2) + 4 + (2 + 11 * 2 + 2), "field sizes");
    WireReader r(bytes);
    check(r.u8() == 0xAB, "u8");
    check(r.u16() == 0x1234, "u16");
    check(r.i16() == -2, "i16");
    check(r.u32() == 0xDEADBEEF, "u32");
    check(r.f32() == 41.26f, "f32");
    check(r.f64() == 166.9, "f64");
    check(r.text_u32() == u"mdd_ed9m-0062\\mdd_ed9m-0062_g.eng", "text_u32");
    check(r.text_u32().empty(), "empty text_u32");
    check(r.text_u16() == u"Краснодар-2", "text_u16");
    check(r.ok() && r.at_end(), "read to the end");
    r.u32();
    check(!r.ok(), "reading past the end fails");
    check(to_utf8(u"Краснодар") == "Краснодар" && from_utf8("Голова сзади") == u"Голова сзади", "UTF-8 both ways");
}

void frames() {
    WireWriter w;
    w.begin(id(Package::junctions));
    for (std::uint32_t i = 0; i < 2; ++i) {
        w.begin(id(Package::junction));
        w.u32(3 + i);
        w.u32(1);
        w.u8(0);
        w.end();
    }
    w.end();
    w.begin(id(Package::snapshot));
    w.end();
    const auto bytes = w.take();
    // As RTS/multdata starts its junctions: 09 46 len, then 0a 46 0f 00 00 00 03 00 00 00 01 00 00 00 00.
    check(bytes.size() == 6 + 2 * 15 + 6, "two junctions and the end");
    check(bytes[6] == 0x0A && bytes[7] == 0x46 && bytes[8] == 15 && bytes[12] == 3, "a junction entry as RTS writes it");
    const auto list = split_frames(bytes);
    check(list.error.empty() && list.frames.size() == 2, "split into two packages");
    check(list.frames[0].id == 0x4609 && list.frames[0].body.size() == 30, "the junction list");
    WireReader inner(list.frames[0].body);
    const auto first = inner.frame();
    check(first.id == 0x460A && first.body.size() == 9, "a nested junction");

    auto broken = bytes;
    broken[2] = 3; // a length under the header
    check(!split_frames(broken).error.empty(), "a bad length stops the split");
    broken = bytes;
    broken.resize(bytes.size() - 2);
    check(!split_frames(broken).error.empty(), "a cut header stops the split");
}

void names() {
    check(package_name(0x4603) == "train_pos" && package_name(0x462F) == "train_pos_forced", "position names");
    check(package_name(0x460E) == "uncouple" && package_name(0x460F) == "couple", "top-level coupling names");
    check(package_name(0x460E, 0x4607) == "train_cars" && package_name(0x460F, 0x460E) == "train_car",
          "the car list inside a train");
    check(package_name(0x4645) == "piece_supply", "piece supply");
    check(package_name(0x1234).empty(), "an unknown id has no name");
}

Snapshot sample() {
    Snapshot s;
    s.root = true;
    s.complete = true;
    s.time = 57041.73f;
    s.junctions = {{3, 1, 0}, {5, 0, 1}};
    s.signals = {{656, 0, 7.0f, 0.0f, 0.0f, 0.0f, -1.0f}, {658, 3, 2.0f, 4.0f, 0.0f, 1.0f, 11.9f}};
    s.substations = {{1, 0, 27500.0f, 0}, {3, 4, 10500.0f, 0}};
    s.phases = {{1, {}}, {3, {27500.0f, 27500.0f}}};
    s.sections = {{3, -2}};
    s.pieces = {{1592, 0, -1, 1}};
    SnapshotTrain t;
    t.id = 4;
    t.kind = 1;
    t.subtype = 1;
    t.top_speed = 29.06f;
    t.state = 2;
    TrainCar a;
    a.kind = 1;
    a.file = u"tsrloco_vl80k-125\\vl80k-125a.eng";
    a.uid = (266u << 18) | 0;
    a.mass = 92000.0f;
    a.width = 3.2f;
    a.length = 16.0f;
    a.height = 5.25f;
    a.handbrake = 0.0f;
    a.max_handbrake = 50000.0f;
    TrainCar b = a;
    b.file = u"tsrloco_vl80k-125\\vl80k-125b.eng";
    b.uid = (266u << 18) | 1;
    b.flags = 1;
    b.handbrake.reset();
    b.max_handbrake.reset();
    t.cars = {a, b};
    t.value_88 = 2.0f;
    TrainPos pos;
    pos.node = 1482;
    pos.offset = 41.26f;
    pos.direction = 1;
    pos.cars = {{24.35, 0.0f, 0.0f}, {8.15, 0.0f, 0.0f}};
    t.pos = pos;
    t.stands = true;
    t.assign = TrainAssign{1, 266, 1, 1};
    t.names = std::array<std::u16string, 2>{u"ВЛ80к", u""};
    t.brakes = std::array<float, 4>{5.0f, 9.0f, 5.0f, 0.0f};
    t.route_left = std::pair{128.11f, std::uint8_t{0}};
    t.cargo.push_back(Bytes{4, 0, 0, 0, 1, 0, 0, 0});
    s.trains.push_back(t);
    PlayerRecord p;
    p.user = 3;
    p.position = {-917.9f, 1.2f, 569.6f};
    const char16_t name[] = u"Player";
    for (std::size_t i = 0; i < 6; ++i) {
        p.name[2 * i] = static_cast<std::uint8_t>(name[i] & 0xFF);
        p.name[2 * i + 1] = static_cast<std::uint8_t>(name[i] >> 8);
    }
    s.players.push_back(p);
    s.other.push_back({0x464C, Bytes{7, 0, 0, 0, 1}});
    return s;
}

void snapshot_round_trip() {
    const auto original = sample();
    const auto bytes = write_snapshot(original);
    const auto read = read_snapshot(bytes);
    for (const auto& problem : read.problems) std::cerr << "  " << problem << '\n';
    check(read.ok(), "the written snapshot reads without problems");
    const auto& s = read.snapshot;
    check(s.root && s.complete, "begins with the root and ends with 0x4608");
    check(s.time && *s.time == 57041.73f, "time");
    check(s.junctions.size() == 2 && s.junctions[1].node() == 6 && s.junctions[1].lock == 1, "junctions");
    check(s.signals.size() == 2 && s.signals[1].item == 658 && s.signals[1].semaphore == 3 && s.signals[1].speed == 11.9f,
          "signal heads");
    check(s.substations.size() == 2 && s.substations[1].voltage == 10500.0f, "substations");
    check(s.phases.size() == 2 && s.phases[0].voltages.empty() && s.phases[1].voltages.size() == 2, "phase voltages");
    check(s.sections.size() == 1 && s.sections[0].source == -2, "section sources");
    check(s.pieces.size() == 1 && s.pieces[0].node == 1592 && s.pieces[0].source == -1, "piece supplies");
    check(s.trains.size() == 1, "one train");
    if (!s.trains.empty()) {
        const auto& t = s.trains.front();
        check(t.cars.size() == 2 && t.cars[0].file == u"tsrloco_vl80k-125\\vl80k-125a.eng", "car file names");
        check(t.cars[0].handbrake && t.cars[0].max_handbrake && !t.cars[1].handbrake, "optional handbrake fields");
        check(t.cars[1].flipped() && !t.cars[0].flipped(), "flipped flag");
        check(t.pos && t.pos->node == 1482 && !t.pos->forward() && t.pos->cars.size() == 2, "position");
        check(t.stands && t.mine() && t.assign->number == 266, "standing, assigned");
        check(t.names && (*t.names)[0] == u"ВЛ80к" && (*t.names)[1].empty(), "names");
        check(t.brakes && (*t.brakes)[1] == 9.0f, "brakes");
        check(t.route_left && t.route_left->first == 128.11f, "route left");
        check(t.cargo.size() == 1, "cargo kept");
        // The rear end of the snapshot to the front of car 0: 24.35 + 16 / 2.
        check(std::abs(front_distance(t) - 32.35) < 1e-4, "front distance from the rear end");
    }
    check(s.players.size() == 1 && s.players[0].name_utf8() == "Player", "player record");
    check(s.other.size() == 1 && s.other[0].id == 0x464C, "unknown packages kept");
    check(write_snapshot(s) == bytes, "writes back byte for byte");
}

void broken_snapshot() {
    const auto bytes = write_snapshot(sample());
    // A position for a train the snapshot has not got.
    WireWriter w;
    w.bytes(bytes);
    w.begin(id(Package::train_pos));
    w.u32(99);
    w.f32(0.0f);
    w.f32(0.0f);
    w.u32(1);
    w.f32(0.0f);
    w.u8(0);
    w.u32(0);
    w.end();
    const auto read = read_snapshot(w.data());
    check(read.problems.size() == 1 && read.snapshot.other.size() == 2, "a stray position is a problem and kept");
}

void play_packages() {
    WireWriter w;
    w.u32(4);
    w.f32(1.5f);
    w.f32(-0.2f);
    w.u32(1482);
    w.f32(41.26f);
    w.u8(1);
    w.u32(1);
    w.begin(id(Package::car_pos));
    w.f64(8.15);
    w.f32(1.5f);
    w.f32(-0.2f);
    w.end();
    const auto pos = decode_train_pos(w.data());
    check(pos && pos->train == 4 && pos->pos.node == 1482 && !pos->pos.forward() && pos->pos.cars.size() == 1,
          "a train position in play");
    // 0x4605 as the live capture had it: train 4, PLAYER, passenger, 266, "Включен".
    const Bytes info_bytes{4, 0, 0, 0, 1, 0, 1, 0, 0x0a, 1, 0, 0, 1, 0, 0, 0, 0};
    const auto info = decode_train_info(info_bytes);
    check(info && info->train == 4 && info->kind == 1 && info->subtype == 1 && info->number == 266 && info->state == 1,
          "train info");
    const Bytes remove_bytes{4, 0, 0, 0, 0};
    const auto remove = decode_train_remove(remove_bytes);
    check(remove && remove->train == 4 && remove->remove == 0, "taken off the track");
    WireWriter m;
    m.u32(6);
    for (const auto unit : std::u16string(u"Привет")) m.u16(static_cast<std::uint16_t>(unit));
    m.u16(0);
    const auto message = decode_message(m.data());
    check(message && *message == u"Привет", "a dispatcher's message");
    const Bytes pause_bytes{1};
    check(decode_pause(pause_bytes) == true, "pause");
    check(!decode_time(Bytes{1, 2, 3}), "a short time does not parse");

    // A light and a car's look as an rts1.exe sent them (live capture).
    const Bytes light_bytes{0x02, 0, 0, 0, 0, 0, 0, 0, 0x01, 0, 0, 0, 0x01, 0, 0, 0xc8, 0x42, 0xff, 0xff, 0xff, 0x50, 0, 0, 0,
                            0x80, 0xcd, 0xcc, 0x8c, 0x40, 0xcd, 0xcc, 0xbe, 0x41};
    const auto light = decode_car_light(light_bytes);
    check(light && light->train == 2 && light->car == 0 && light->light == 1 && light->lit, "a car light");
    const Bytes look_bytes{0x02, 0, 0, 0, 0, 0, 0, 0, 0x01, 0x0e, 0xff, 0x03, 0, 0, 0, 0, 0, 0, 0x01, 0};
    const auto look = decode_car_state(look_bytes);
    check(look && look->train == 2 && look->kind == 1 && look->flags == 0x0003ff0e && look->pantographs == 1 &&
              !look->running,
          "a car's look: pantograph 1 up");
    const auto request = encode_state_request(2, 3);
    check(request == Bytes({2, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}), "the look request as rts1.exe sends it");
    PlayerPosition at;
    at.tile_x = -4549;
    at.tile_z = 14591;
    at.x = -765.08f;
    const auto place = encode_player_pos(at);
    check(place.size() == 44 && place[12] == 0x3b && place[13] == 0xee && place[14] == 0xff && place[15] == 0x38,
          "a player's place: 44 bytes, the tile at 12");
    // In a train: its id second, the first field 0 (rts1.exe driving: 0, 2, 0).
    at.train = 2;
    const auto inside = encode_player_pos(at);
    check(inside[0] == 0 && inside[4] == 2, "a player's place: the train he is in is the second field");

    // Our own train out and back in.
    TrainPos own;
    own.speed = 12.5f;
    own.node = 1585;
    own.offset = 750.44f;
    own.direction = 0;
    own.cars = {{9.85, 12.5f, 0.2f}};
    const auto own_bytes = encode_train_pos(2, own);
    const auto own_back = decode_train_pos(own_bytes);
    check(own_back && own_back->train == 2 && own_back->pos.node == 1585 && own_back->pos.cars.size() == 1 &&
              own_back->pos.cars[0].offset == 9.85,
          "our train's position reads back");
    CarLightState lamp;
    lamp.train = 2;
    lamp.light = 7;
    lamp.lit = true;
    lamp.radius = 0.95f;
    lamp.colour = 0xa0ffffffu;
    const auto lamp_bytes = encode_car_light(lamp);
    const auto lamp_back = decode_car_light(lamp_bytes);
    check(lamp_bytes.size() == 33 && lamp_back && lamp_back->light == 7 && lamp_back->lit, "our light: 33 bytes as RTS's");
    CarLook ours;
    ours.train = 2;
    ours.kind = 1;
    ours.pantographs = 2;
    const auto ours_bytes = encode_car_state(ours);
    const auto ours_back = decode_car_state(ours_bytes);
    check(ours_bytes.size() == 20 && ours_back && ours_back->pantographs == 2 && ours_back->kind == 1,
          "our car's look: 20 bytes as RTS's");
    check(encode_train_id(2) == Bytes({2, 0, 0, 0}), "train_stands");
    // A sound trigger as rts1.exe sent it (the ED9M, car 1, code 45).
    const Bytes trigger_bytes{3, 0, 0, 0, 1, 0, 0, 0, 3, 0, 1, 0, 45, 0, 0, 0, 0, 0, 0, 0};
    const auto trigger = decode_sound_trigger(trigger_bytes);
    check(trigger && trigger->train == 3 && trigger->car == 1 && trigger->slot == 3 && trigger->code == 45,
          "sound_trigger: train, car, slot 3, code");
    check(trigger && encode_sound_trigger(*trigger) == trigger_bytes, "sound_trigger: written back the same");
    SoundValues values;
    values.train = 2;
    values.car = 0;
    values.vars[2] = 0.03f;
    values.travelled = 12.5f;
    values.volume = 0.7f;
    const auto values_bytes = encode_sound_values(values);
    const auto values_back = decode_sound_values(values_bytes);
    // A substation's load as rts1.exe sent it (substation 3, phase 1).
    SubstationLoadReport load;
    load.substation = 3;
    load.phase = 1;
    std::memcpy(&load.energy, "\x7e\x34\x27\x42", 4);
    std::memcpy(&load.window, "\x25\x06\x01\x40", 4);
    check(encode_substation_load(load) == Bytes({3, 0, 0, 0, 1, 0, 0x7e, 0x34, 0x27, 0x42, 0x25, 0x06, 0x01, 0x40}),
          "substation_load: 14 bytes as rts1.exe sends them");
    // A car sound's whole state as the capture has it: a looping stream
    // (its first trigger, + 1) and a quiet one.
    SoundState sound;
    sound.values.train = 2;
    sound.values.volume = 0.7f;
    SoundStreamState looping;
    looping.current = 1;
    looping.triggers = {1, 0};
    SoundStreamState quiet;
    quiet.volume = 0.7f;
    quiet.triggers = {0, 0, 0};
    sound.streams = {looping, quiet};
    const auto sound_bytes = encode_sound_state(sound);
    const auto sound_back = decode_sound_state(sound_bytes);
    check(sound_bytes.size() == 54 + 46 + 50 && sound_bytes[50] == 2 && sound_bytes[54] == 0x3B && sound_bytes[55] == 0x46,
          "sound_state: the values, the stream count at 50, a 0x463B per stream from 54 (as rts1.exe)");
    check(sound_back && sound_back->streams.size() == 2 && sound_back->streams[0].current == 1 &&
              sound_back->streams[1].triggers.size() == 3 && sound_back->values.volume == 0.7f,
          "sound_state: read back");

    // Couplings and cuts as the live capture has them.
    check(encode_couple({2, 4, 0, 1}) == Bytes({2, 0, 0, 0, 4, 0, 0, 0, 0, 1}), "couple: A, B, the two ends");
    const auto cut_back = decode_uncouple(Bytes({2, 0, 0, 0, 1, 0, 0, 0, 0x94, 0x87, 0x01, 0x00, 0}));
    check(cut_back && cut_back->train == 2 && cut_back->car == 1 && cut_back->part == 100244 && cut_back->side == 0,
          "uncouple: train, car, the local id of the part, side");
    const auto renamed = decode_train_id_change(Bytes({0x94, 0x87, 0x01, 0x00, 1, 0, 0, 0}));
    check(renamed && renamed->first == 100244 && renamed->second == 1, "train_id: the local id becomes the server's");
    const auto car = [](const std::uint32_t uid, const std::uint32_t flags = 0) {
        TrainCar c;
        c.uid = uid;
        c.flags = flags;
        return c;
    };
    const std::vector<TrainCar> engine{car(1)}, wagons{car(10), car(11), car(12)};
    const auto uids = [](const std::vector<TrainCar>& cars) {
        std::vector<std::uint32_t> out;
        for (const auto& c : cars) out.push_back(c.uid);
        return out;
    };
    check(uids(coupled_cars(engine, wagons, 0, 1)) == std::vector<std::uint32_t>{1, 10, 11, 12},
          "A's rear to B's front: A, then B in order (the capture's 9 and 19 cars)");
    const auto turned = coupled_cars(engine, wagons, 0, 0);
    check(uids(turned) == std::vector<std::uint32_t>{1, 12, 11, 10} && turned[1].flipped() && !turned[0].flipped(),
          "rear to rear: B turned, its cars flipped");
    check(uids(coupled_cars(engine, wagons, 1, 0)) == std::vector<std::uint32_t>{10, 11, 12, 1}, "A's front to B's rear: B ahead");
    const auto [stay, part] = cut_cars(coupled_cars(engine, wagons, 0, 1), 1, 0);
    check(uids(stay) == std::vector<std::uint32_t>{1} && uids(part) == std::vector<std::uint32_t>{10, 11, 12},
          "cut before car 1, side 0: the part is the cars from it on (the capture's 18-car part)");
    check(values_bytes.size() == 50 && values_back && values_back->slot == 3 && values_back->vars[2] == 0.03f &&
              values_back->travelled == 12.5f && values_back->volume == 0.7f,
          "sound_values: 50 bytes as RTS's");
}

// A fake RTS server on the loopback: the login and its reply, an
// assignment, the snapshot on request, then it hangs up.
void connection_loopback() {
#ifdef _WIN32
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    struct Cleanup {
        ~Cleanup() { WSACleanup(); }
    } cleanup;
#endif
    TestSocket listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in where{};
    where.sin_family = AF_INET;
    where.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    where.sin_port = 0;
    if (::bind(listener, reinterpret_cast<sockaddr*>(&where), sizeof where) != 0 || ::listen(listener, 1) != 0) {
        check(false, "a loopback listener");
        close_test_socket(listener);
        return;
    }
    socklen_t size = sizeof where;
    ::getsockname(listener, reinterpret_cast<sockaddr*>(&where), &size);
    const auto port = ntohs(where.sin_port);

    bool login_ok = false;
    bool asked = false;
    std::thread server([&] {
        const auto peer = ::accept(listener, nullptr, nullptr);
        const auto read = [&](std::uint8_t* out, int n) {
            while (n > 0) {
                const auto got = ::recv(peer, reinterpret_cast<char*>(out), n, 0);
                if (got <= 0) return false;
                out += got;
                n -= got;
            }
            return true;
        };
        const auto write = [&](const Bytes& bytes) {
            ::send(peer, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), 0);
        };
        std::uint8_t head[6];
        if (read(head, 6)) {
            const auto length = head[2] | (head[3] << 8);
            Bytes body(static_cast<std::size_t>(length - 6));
            read(body.data(), static_cast<int>(body.size()));
            WireReader r(body);
            login_ok = head[0] == 0x00 && head[1] == 0x46 && length == 12 + 2 * 4 && r.u16() == 101 && r.text_u16() == u"Test";
        }
        // The reply has no length field.
        WireWriter reply;
        reply.u16(0x4600);
        reply.u16(101);
        reply.text_u16(u"Маршрут: Test");
        reply.u8(1);
        reply.u8(0);
        reply.text_u16(u"testroute");
        for (std::uint32_t i = 0; i < 4; ++i) reply.u32(i + 1);
        write(reply.data());
        WireWriter assign;
        assign.begin(id(Package::train_assign));
        assign.u32(3);
        assign.u8(1);
        assign.u32(101);
        assign.u16(0);
        assign.u16(1);
        assign.end();
        write(assign.data());
        if (read(head, 6)) asked = head[0] == 0x08 && head[1] == 0x46;
        Snapshot s;
        s.root = true;
        s.complete = true;
        s.time = 3600.0f;
        s.junctions = {{3, 1, 0}};
        write(write_snapshot(s));
        close_test_socket(peer);
    });

    RtsConnection connection;
    const auto login = connection.connect("127.0.0.1:" + std::to_string(port), u"Test", 5000);
    check(login.ok() && login.reply->route == u"testroute" && login.reply->description == u"Маршрут: Test" &&
              login.reply->season == 1 && login.reply->counts[3] == 4,
          "the login reply without a length field");
    std::vector<ReceivedPackage> got;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool sent = false;
    while (std::chrono::steady_clock::now() < until) {
        for (auto& p : connection.receive()) {
            if (p.id == id(Package::train_assign) && !sent) sent = connection.send(id(Package::snapshot), {});
            got.push_back(std::move(p));
        }
        if (!got.empty() && got.back().id == id(Package::server_down)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    server.join();
    close_test_socket(listener);
    check(login_ok, "the login as RTS sends it: version 101, the nick");
    check(asked, "the snapshot asked for after the assignment");
    // assign, root, time, junctions, end, then the hang-up.
    check(got.size() == 6 && got[0].id == 0x4601 && got[1].id == 0x4600 && got[2].id == 0x460D && got[4].id == 0x4608 &&
              got[5].id == id(Package::server_down),
          "packages in order, then server_down when the server hangs up");
    check(!connection.connected(), "not connected after the hang-up");
    const auto bad = RtsConnection().connect("127.0.0.1:1", u"Test", 1000);
    check(!bad.ok() && !bad.error.empty(), "no server: an error");
    check(parse_address("host") && parse_address("host")->port == 800 && parse_address("1.2.3.4:7800")->port == 7800 &&
              !parse_address("host:x"),
          "addresses");
}

void content(const std::filesystem::path& root) {
    const auto file = root / "RTS" / "multdata";
    if (!std::filesystem::exists(file)) {
        std::cout << "no " << file.string() << ": skipped\n";
        return;
    }
    std::ifstream in(file, std::ios::binary);
    const Bytes bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto read = read_snapshot(bytes);
    for (const auto& problem : read.problems) std::cerr << "  " << problem << '\n';
    check(read.ok(), "multdata reads without problems");
    check(read.snapshot.complete && read.snapshot.time.has_value(), "multdata is a whole snapshot");
    check(!read.snapshot.junctions.empty() && !read.snapshot.signals.empty(), "multdata has junctions and signals");
    check(write_snapshot(read.snapshot) == bytes, "multdata writes back byte for byte");
    std::cout << "multdata: " << read.snapshot.junctions.size() << " junctions, " << read.snapshot.signals.size()
              << " signal heads, " << read.snapshot.trains.size() << " trains\n";
}

} // namespace

int main(const int argc, char** argv) {
    if (argc > 1) {
        content(std::filesystem::path(argv[1]));
    } else {
        wire_fields();
        frames();
        names();
        snapshot_round_trip();
        broken_snapshot();
        play_packages();
        connection_loopback();
    }
    if (failures == 0) std::cout << "net tests passed\n";
    return failures == 0 ? 0 : 1;
}
