#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>
#include <string>

#include <openrts/net/snapshot.hpp>
#include <openrts/net/wire.hpp>

// Packages of an RTS game in progress, one body at a time (docs/net.md;
// docs/reverse/multiplayer-protocol.md). The snapshot's packages come again
// in play; these decode the ones an observing client needs. Each gives
// nothing when the body does not parse whole.
namespace openrts::net {

[[nodiscard]] std::optional<float> decode_time(ByteView body);
[[nodiscard]] std::optional<JunctionState> decode_junction(ByteView body);
[[nodiscard]] std::optional<SignalHeadState> decode_signal_head(ByteView body);

// 0x4618 signal_control, console <-> server (rts_server.exe 0x451F3C, read
// by 0x40FB90): a signal unit (the server's signal object, by its table
// index), its aspect (+0), its mode (+4: 0 manual, 1 open, 2 automatic
// block) and whether the dispatcher picked the aspect (+17).
struct SignalControl {
    std::uint32_t unit{};
    std::uint16_t aspect{};
    float mode{};
    std::uint8_t pinned{};
};
[[nodiscard]] std::optional<SignalControl> decode_signal_control(ByteView body);
[[nodiscard]] Bytes encode_signal_control(const SignalControl& value);

// 0x450B route_segment, console <-> server (rts_server.exe 0x453F80 writes,
// 0x454194 reads): a segment of a train's route, a station route of the
// route database. `tile_x` / `tile_z`: the tile of the station route's
// start point (Point1), a check; `reverse`: it is run from its end; `hold`:
// the train's next segment waits; `reversed`: the train runs it rear
// first; `path`: its TrainPath (index + 1, 0 none); `thrown`: how many of
// its junctions are set so far.
struct RouteSegment {
    std::uint32_t id{};
    std::uint32_t train{};
    std::uint32_t route{};
    std::u16string station;
    std::int16_t tile_x{};
    std::int16_t tile_z{};
    std::uint8_t reverse{};
    std::uint8_t hold{};
    std::uint8_t reversed{};
    float length{};
    std::uint32_t priority{};
    std::uint8_t state{}; // 0 waiting, 1 set, 2 set part of the way, 3 the train on it, 4 left
    std::uint16_t path{};
    std::uint16_t thrown{};
};
[[nodiscard]] std::optional<RouteSegment> decode_route_segment(ByteView body);
[[nodiscard]] Bytes encode_route_segment(const RouteSegment& value);
// 0x450C route_segment_reply (0x454484): the request's id, the segment's
// (0 none), ok, the reason when not.
struct RouteSegmentReply {
    std::uint32_t request{};
    std::uint32_t id{};
    std::uint8_t ok{};
    std::u16string text;
};
[[nodiscard]] std::optional<RouteSegmentReply> decode_route_segment_reply(ByteView body);
[[nodiscard]] Bytes encode_route_segment_reply(const RouteSegmentReply& value);
// 0x450E route_segment_state (0x454600): id, state, path, priority,
// length, junctions set.
[[nodiscard]] std::optional<RouteSegment> decode_route_segment_state(ByteView body);
[[nodiscard]] Bytes encode_route_segment_state(const RouteSegment& value);
[[nodiscard]] std::optional<SnapshotTrain> decode_train_add(ByteView body);

struct TrainPosUpdate {
    std::uint32_t train{};
    TrainPos pos;
};
// 0x4603 train_pos and 0x462F train_pos_forced.
[[nodiscard]] std::optional<TrainPosUpdate> decode_train_pos(ByteView body);
// 0x4606 train_stands: the train id alone.
[[nodiscard]] std::optional<std::uint32_t> decode_train_id(ByteView body);

// What a client sends of its own train (docs/net.md, "Own train").
// 0x4603 train_pos: the rear end, the cars (0x4604 each).
[[nodiscard]] Bytes encode_train_pos(std::uint32_t train, const TrainPos& pos);
// A body of the train id alone: 0x4606 train_stands.
[[nodiscard]] Bytes encode_train_id(std::uint32_t train);
// 0x4636 derail, as rts1.exe sends it of its own train: a u8 per car (1
// derailed), then a 0x4638 per derailed car. The block's layout is the live
// capture's of 2026-11-05 byte for byte (reverse/multiplayer-protocol.md;
// the meanings PROBABLE). Every client runs the falling itself from it.
struct DerailedCar {
    std::uint32_t car{};
    std::array<float, 3> heading{};  // a unit vector along the car, level
    std::array<float, 3> position{}; // in its tile (x, y, z)
    float tip{};                     // the roll now, rad
    float tip_rest{};                // the roll it comes to rest at
    float tip_third{};               // UNKNOWN: about 0.7 of the rest in the capture
    float weight{};                  // m * g, N
    std::uint32_t state{};           // 0 as it comes off, 4 once it rolls (the capture)
};
struct Derail {
    std::uint32_t train{};
    std::vector<std::uint8_t> derailed; // by car
    std::vector<DerailedCar> cars;
};
[[nodiscard]] Bytes encode_derail(const Derail& derail);
[[nodiscard]] std::optional<Derail> decode_derail(ByteView body);
// 0x45FB openrts_train_state (openRTS's own, 2026-11-21): a train's state as
// its owner's openRTS keeps it (the cars' air and switches, its DLLs'
// saved state: TrainView::own_state), compressed and opaque to the server,
// which keeps it with the train (and in its placements) and gives it back
// to the owner who gets the train again. In parts: a package holds 0x3C00
// bytes at most. u32 train, u16 part (0-based), u16 parts, the bytes.
struct TrainStatePart {
    std::uint32_t train{};
    std::uint16_t part{}, parts{};
    Bytes bytes;
};
[[nodiscard]] std::vector<Bytes> encode_train_state(std::uint32_t train, ByteView state);
// 0x45FC openrts_doors (openRTS's own, 2026-11-21): a train's passenger
// doors as its owner shows them, a byte per car (bit n: the doors of
// GlobalID n + 1 open; 1 the right ones, 2 the left). RTS shares none
// (reverse/multiplayer-protocol.md): sent when they change.
struct OpenrtsDoors {
    std::uint32_t train{};
    std::vector<std::uint8_t> groups; // by car
};
[[nodiscard]] Bytes encode_openrts_doors(const OpenrtsDoors& doors);
[[nodiscard]] std::optional<OpenrtsDoors> decode_openrts_doors(ByteView body);
// 0x45FD openrts_cab_doors (openRTS's own, 2026-11-21): the doors of one
// car's cab or salon (its .sd's doors: to the vestibule, the passageway,
// the passengers' from inside) as whoever is in it opened or shut them, a
// byte each (1 open), for the engineer and his helpers alike.
struct OpenrtsCabDoors {
    std::uint32_t train{}, car{};
    std::vector<std::uint8_t> open;
};
[[nodiscard]] Bytes encode_openrts_cab_doors(const OpenrtsCabDoors& doors);
[[nodiscard]] std::optional<OpenrtsCabDoors> decode_openrts_cab_doors(ByteView body);
// 0x45FE openrts_car_action (openRTS's own, 2026-11-21): a car menu item
// done on a car of any train (a chock, the handbrake, a valve, a hose, a
// cock, a cable), carried out on every openRTS computer, its owner's too:
// RTS keeps them on the one that did it (reverse/multiplayer-protocol.md).
// u32 train, u32 car, u8 coupling (0 a bogie's menu), u8 front, u32 item.
struct OpenrtsCarAction {
    std::uint32_t train{}, car{};
    bool coupling = false, front = true;
    std::uint32_t item{};
};
[[nodiscard]] Bytes encode_openrts_car_action(const OpenrtsCarAction& action);
[[nodiscard]] std::optional<OpenrtsCarAction> decode_openrts_car_action(ByteView body);
// 0x461C car_cargo with openRTS's own body (2026-11-26): the server reads
// its first two fields (u32 train, u32 car), keeps the last of each car and
// gives them to those who join (snapshot.hpp); then u8 'o', u8 version 1,
// u32 n and n loads: u16 cargo (formats::CargoKind's code), text_u16
// object id, i32 area, f32 volume m³, f32 mass kg, i32 count, f32 x 3 its
// place, f32 x 3 its size (simulation::CarLoad). All the car's loads: they
// replace what it had.
struct OpenrtsCarCargo {
    std::uint32_t train{}, car{};
    struct Load {
        std::uint16_t cargo{};
        std::string object;
        std::int32_t area{};
        float volume{}, mass{};
        std::int32_t count = 1;
        std::array<float, 3> position{}, extent{};
    };
    std::vector<Load> loads;
};
[[nodiscard]] Bytes encode_openrts_car_cargo(const OpenrtsCarCargo& cargo);
// nullopt: not openRTS's (RTS's own body) or broken.
[[nodiscard]] std::optional<OpenrtsCarCargo> decode_openrts_car_cargo(ByteView body);
[[nodiscard]] std::optional<TrainStatePart> decode_train_state(ByteView body);
// 0x4617 car_light, as rts1.exe sends it: the receiver takes the radius,
// colour and position too (0x4C3DE0).
struct CarLightState {
    std::uint32_t train{};
    std::uint32_t car{};
    std::uint32_t light{};
    bool lit = false;
    float radius{};
    std::uint32_t colour{}; // the .eng's LightColour, AARRGGBB
    std::array<float, 3> position{};
};
[[nodiscard]] Bytes encode_car_light(const CarLightState& light);

// 0x4602 train_remove: 0 off the track (kept), 1 deleted.
struct TrainRemove {
    std::uint32_t train{};
    std::uint8_t remove{};
};
[[nodiscard]] std::optional<TrainRemove> decode_train_remove(ByteView body);

// 0x4605 train_info.
struct TrainInfo {
    std::uint32_t train{};
    std::uint16_t kind{};
    std::uint16_t subtype{};
    std::uint32_t number{};
    std::uint8_t state{};
    std::uint32_t path{};
};
[[nodiscard]] std::optional<TrainInfo> decode_train_info(ByteView body);

// 0x4601 train_assign / 0x461E helper_assign.
struct TrainAssignUpdate {
    std::uint32_t train{};
    TrainAssign assign;
};
[[nodiscard]] std::optional<TrainAssignUpdate> decode_train_assign(ByteView body);

// 0x4617 car_light: a light of a car another client drives (33 bytes; the
// radius, colour and position after `lit` are left).
struct CarLight {
    std::uint32_t train{};
    std::uint32_t car{};
    std::uint32_t light{}; // the car's Lights entry, in file order
    bool lit = false;
};
[[nodiscard]] std::optional<CarLight> decode_car_light(ByteView body);

// 0x463C car_state: how a car another client drives looks (20 bytes).
struct CarLook {
    std::uint32_t train{};
    std::uint32_t car{};
    std::uint8_t kind{};        // 0 wagon, 1 electric, 2 diesel
    std::uint32_t flags{};      // the car's Flags (RTS skips them)
    std::uint32_t value_32{};
    std::uint8_t value_14{};
    std::uint8_t pantographs{}; // bit n-1: pantograph n raised
    bool running = false;       // a diesel's engine runs (else it is cold)
};
[[nodiscard]] std::optional<CarLook> decode_car_state(ByteView body);
[[nodiscard]] Bytes encode_car_state(const CarLook& look);

// A car's sound as its owner sends it (docs/reverse/multiplayer.md, "Who
// sends"). The slot is 3, the exterior .sms, as rts1.exe sends it.
// 0x4639 sound_trigger (20 bytes): one discrete trigger.
struct SoundTrigger {
    std::uint32_t train{};
    std::uint32_t car{};
    std::uint16_t slot = 3;
    std::uint32_t code{};
};
[[nodiscard]] std::optional<SoundTrigger> decode_sound_trigger(ByteView body);
[[nodiscard]] Bytes encode_sound_trigger(const SoundTrigger& trigger);
// 0x463D sound_values (50 bytes): the sound's Var1..Var8, the distance
// travelled and its volume (SMSObject +284, +308).
struct SoundValues {
    std::uint32_t train{};
    std::uint32_t car{};
    std::uint16_t slot = 3;
    std::array<float, 8> vars{};
    float travelled{};
    float volume = 1.0f;
};
[[nodiscard]] std::optional<SoundValues> decode_sound_values(ByteView body);
[[nodiscard]] Bytes encode_sound_values(const SoundValues& values);
// 0x463A sound_state: a car sound's whole state, the owner's answer to
// state_request 2 (rts1.exe 0x4CE290): the 0x463D fields, the number of
// streams, then a 0x463B per stream (0x4CEA38). Read from 3 232 packages of
// the live capture of 2026-11-05.
struct SoundStreamState {
    std::uint32_t flags{};
    std::uint32_t current{};      // the trigger that plays (its loop) + 1; 0 none
    std::uint32_t start{};        // UNKNOWN, 0 in the capture
    std::uint32_t stream_flags{}; // UNKNOWN, 0 in the capture
    std::uint32_t position{};     // PROBABLE the loop's play position (samples)
    float volume = 1.0f;          // the stream's Volume
    float value = 1.0f;           // UNKNOWN, 1.0 in the capture
    std::vector<std::uint32_t> triggers; // per trigger: 1 the current one
};
struct SoundState {
    SoundValues values;
    std::vector<SoundStreamState> streams;
};
[[nodiscard]] std::optional<SoundState> decode_sound_state(ByteView body);
[[nodiscard]] Bytes encode_sound_state(const SoundState& state);

// The line from the server (the console's "Электроснабжение"), as in the
// snapshot: 0x4646 a substation's state, 0x4648 its phases' voltages,
// 0x4647 a section's source, 0x4645 a piece of track's own supply.
[[nodiscard]] std::optional<Substation> decode_substation(ByteView body);
[[nodiscard]] std::optional<std::vector<SubstationPhases>> decode_phase_voltages(ByteView body);
[[nodiscard]] std::optional<SectionSource> decode_section_source(ByteView body);
[[nodiscard]] std::optional<PieceSupply> decode_piece_supply(ByteView body);

// 0x4649 substation_load (14 bytes), to the dispatchers: what a train drew
// from a substation phase over a window of about 2 s (rts1.exe 0x5B6030).
struct SubstationLoadReport {
    std::uint32_t substation{};
    std::uint16_t phase{};
    float energy{}; // the power (PowerConsuming) summed over the window x s
    float window{}; // s
};
[[nodiscard]] Bytes encode_substation_load(const SubstationLoadReport& load);

// Couplings in play (docs/reverse/multiplayer.md, "Coupling in play").
// 0x460F couple: train A took train B in; the ends, 0 the rear (the last
// car's), 1 the front: A's rear to B's front makes A's cars then B's in their
// order (PROBABLE, both live couplings of 2026-11-05).
struct Coupling {
    std::uint32_t a{}, b{};
    std::uint8_t end_a{}, end_b{};
};
[[nodiscard]] std::optional<Coupling> decode_couple(ByteView body);
[[nodiscard]] Bytes encode_couple(const Coupling& coupling);
// 0x460E uncouple: train cut before car `car`; the part gets `part` (the
// sender's local id, the server's id to the others); side 0: the part is
// the cars from `car` on (the live capture), PROBABLE 1: the cars before.
struct Uncoupling {
    std::uint32_t train{}, car{}, part{};
    std::uint8_t side{};
};
[[nodiscard]] std::optional<Uncoupling> decode_uncouple(ByteView body);
[[nodiscard]] Bytes encode_uncouple(const Uncoupling& cut);
// 0x4610 train_id: a train's id changed (old, new).
[[nodiscard]] std::optional<std::pair<std::uint32_t, std::uint32_t>> decode_train_id_change(ByteView body);
// The record's cars after a coupling or a cut (the snapshot's car flags,
// bit 0 flipped).
[[nodiscard]] std::vector<TrainCar> coupled_cars(const std::vector<TrainCar>& a, const std::vector<TrainCar>& b,
                                                 std::uint8_t end_a, std::uint8_t end_b);
// {the cars that stay, the part's}.
[[nodiscard]] std::pair<std::vector<TrainCar>, std::vector<TrainCar>> cut_cars(const std::vector<TrainCar>& cars,
                                                                             std::size_t car, std::uint8_t side);

// 0x4623 state_request: mode 2 asks a train's owner for its whole look
// (lights, sound, car state), as rts1.exe does when it loads another
// player's train; mode 1 a helper's whole cab.
[[nodiscard]] Bytes encode_state_request(std::uint32_t mode, std::uint32_t train);

// 0x462D player_pos as a client sends it (44 bytes): the server keeps it
// and draws the person on the consoles' map; it relays a train's lights,
// look and sounds only to players within 3 km of it.
struct PlayerPosition {
    // The user record's train field (0x462E +4, the train the client takes
    // as its own): rts1.exe always sends 0 here (live capture 2026-11-05);
    // openRTS sent its train in it until 2026-11-06.
    std::uint32_t value_0{};
    std::uint32_t train{};   // the train the player is in, 0 none (rts1.exe: 2, 3, 4 driving)
    std::uint32_t value_8{}; // UNKNOWN: 0, sometimes 1
    std::int16_t tile_x{}, tile_z{};
    float x{}, y{}, z{};             // in the tile
    std::array<float, 4> view{};     // UNKNOWN: the camera's (rts1.exe sends headings and the like)
};
[[nodiscard]] Bytes encode_player_pos(const PlayerPosition& position);

// openRTS's dispatcher console (console_state, server -> consoles): the
// game as the server holds it, which RTS's packages do not carry whole
// (owners, routes).
struct ConsoleUser {
    std::uint32_t id{};
    std::u16string nick;
    bool connected = false;
    bool playing = false;
    bool console = false;
    std::vector<std::uint32_t> trains;
    float camera_x{}, camera_z{}; // route metres (tile * 2048 + x), when `placed`
    bool placed = false;
};
struct ConsoleTrain {
    std::uint32_t id{}, number{};
    std::uint16_t kind{}, subtype{};
    std::uint8_t state{};
    std::uint32_t owner{};           // user id, 0 none
    std::uint32_t route_junctions{}; // held for its route
    float speed{};                   // m/s
    bool stands = false;
    std::uint32_t cars{};
    std::u16string first_car;        // its first car's file
};
// A segment of a train's route (a station route; GameServer::SegmentSummary).
struct ConsoleSegment {
    std::uint32_t id{}, train{};
    std::uint32_t route{}; // the station route's ID
    std::u16string station, name;
    bool reverse = false;  // from its end
    bool hold = false;     // the train's next segment waits
    bool reversed = false; // the train runs it rear first
    std::uint32_t priority{};
    std::uint8_t state{};  // 0 waiting, 1 set, 2 set part of the way, 3 the train on it, 4 left
    std::uint16_t thrown{};
    std::uint32_t junctions{};
};
struct ConsoleState {
    float time{}; // s of the day
    bool paused = false;
    bool admits = true;
    std::vector<ConsoleUser> users;
    std::vector<ConsoleTrain> trains;
    // Since 2026-11-21, after the trains (a reader without them stops there).
    std::vector<ConsoleSegment> segments;
};
[[nodiscard]] Bytes encode_console_state(const ConsoleState& state);
[[nodiscard]] std::optional<ConsoleState> decode_console_state(ByteView body);

// console_place: a consist the console puts on the track, by its cars, not
// its name: the dispatcher's own consists need not be on the server, only
// the cars' files. It stands there, nobody's, until given to a player.
struct ConsoleCar {
    std::u16string folder; // under TRAINS/TRAINSET
    std::u16string name;   // the file's stem
    bool engine = false;   // .eng, else .wag
    bool flipped = false;
    bool cold = false;
    float handbrake = -1.0f; // percent; below 0 none given
    std::uint8_t chocks{};   // PlaceDrag: 1 a chock at its end towards the train's front, 2 the other (2026-11-21)
};
struct ConsolePlace {
    std::u16string consist;  // its name, for the log
    float max_velocity{};    // the consist's MaxVelocity (m/s), 0 none
    std::vector<ConsoleCar> cars;
    std::uint32_t number{};  // 0: the server's lowest free
    // Where: at a path's start (`path` not empty), else the front of its
    // first car at (node, offset), facing along the node when `forward`.
    std::u16string path;
    std::uint32_t node{};
    double offset{};
    bool forward = true;
    // Since 2026-11-21, after the cars (an older console's place has none):
    // the train's name (0x461A), its kind (0 STATIC, 1 PLAYER, 2 TRAFFIC,
    // 3 LOOSE) and state (0 "Холодный", 1 "Включен", 2 "Движется").
    std::u16string name;
    std::uint16_t kind{};
    std::uint8_t state{};
    std::u16string notes; // after the state (0x461A's second text)
};
[[nodiscard]] Bytes encode_console_place(const ConsolePlace& place);

// 0x45FA openrts_weather (openRTS's own, 2026-11-21): the game's weather, as
// the server sets it while it runs. `dynamic` 0: `weather` stands (0 clear,
// 1 snow, 2 rain, 3 thunderstorm); 1: openRTS's dynamic weather
// (docs/weather.md), the same for everyone by its seed and clock (the
// game seconds its cycle has run when this was sent; the server sends it
// again every 30 s), its climate by the season or the sliders'.
struct OpenrtsWeather {
    std::uint8_t dynamic{};
    std::uint8_t weather{};
    std::uint64_t seed = 1;
    double clock{};
    std::uint8_t by_season = 1;
    float cloudiness = 0.5f, wet = 0.3f, storms = 0.2f;
    float spell_minutes = 45.0f, change_minutes = 12.0f;
};
[[nodiscard]] Bytes encode_openrts_weather(const OpenrtsWeather& weather);
// openrts_clock (0x45EF): how fast the server's clock runs (1 as real time,
// up to 3600; its pause is RTS's own package) and the season, 0 spring ..
// 3 winter. A package of its own: openrts_weather's length is fixed, and
// an older openRTS drops one it does not know.
struct OpenrtsClock {
    float speed = 1.0f;
    std::uint8_t season = 1;
};
[[nodiscard]] Bytes encode_openrts_clock(const OpenrtsClock& clock);
[[nodiscard]] std::optional<OpenrtsClock> decode_openrts_clock(ByteView body);
[[nodiscard]] std::optional<OpenrtsWeather> decode_openrts_weather(ByteView body);
[[nodiscard]] std::optional<ConsolePlace> decode_console_place(ByteView body);

// console_next: the station routes a train can take next, as the server's
// "Собрать маршрут" lists them (docs/server.md, "Station routes"): from the
// end of its last segment, else its head; `back` the other way.
struct ConsoleNextAsk {
    std::uint32_t train{};
    bool back = false;
};
[[nodiscard]] Bytes encode_console_next_ask(const ConsoleNextAsk& ask);
[[nodiscard]] std::optional<ConsoleNextAsk> decode_console_next_ask(ByteView body);
struct ConsoleRouteChoice {
    std::uint32_t route{};  // the station route's ID
    bool reverse = false;   // run from its end
    bool main_track = false;
    bool wrong_way = false; // onto a wrong track
    bool platform = false;  // ends at a platform ("[П]")
    float track_length{};   // m of the track it ends on, 0 not known
    float distance{};       // m along the track to its start
    std::u16string station, name;
};
struct ConsoleNext {
    std::uint32_t train{};
    bool back = false;
    bool found = false;      // the train is on the server's track
    bool rear_first = false; // a segment set from here is run rear first
    std::uint32_t after{};   // the segment it follows, 0: from the train
    std::u16string after_name;
    std::vector<ConsoleRouteChoice> routes; // main tracks first, onto a wrong track last
};
[[nodiscard]] Bytes encode_console_next(const ConsoleNext& next);
[[nodiscard]] std::optional<ConsoleNext> decode_console_next(ByteView body);

// 0x462D player_pos as the server relays it in play: the sender's id in
// front, the 40 bytes after its first field, its name after (rts_server.exe
// and openRTS's server alike). Who is where, for the players' list and
// their names over the world.
struct OtherPlayer {
    std::uint32_t user{};
    std::uint32_t train{}; // the train he is in, 0 none
    std::int16_t tile_x{}, tile_z{};
    float x{}, y{}, z{};
    std::u16string nick;
};
[[nodiscard]] std::optional<OtherPlayer> decode_other_player(ByteView body);

// openrts_hello: u16 the openRTS features the sender has (1: players and
// helpers), then (since 2026-11-16) u8 1 when the game is a thunderstorm
// (the server's answer; its login says rain, all RTS knows). Older readers
// stop after the u16.
constexpr std::uint16_t openrts_features = 1;
// The server's answer since 2026-11-21 then, when its game may be joined
// from Discord: u8 1, the address for the others (text_u16; empty: the one
// the player connected to), u16 the players in it, u16 its places (0: no
// limit). The server sends it again when the players' number changes.
struct OpenrtsDiscord {
    std::u16string address;
    std::uint16_t players = 0;
    std::uint16_t places = 0;
    bool operator==(const OpenrtsDiscord&) const = default;
};
[[nodiscard]] Bytes encode_openrts_hello(std::uint16_t features = openrts_features, bool storm = false,
                                         std::optional<OpenrtsDiscord> discord = std::nullopt);
[[nodiscard]] std::optional<OpenrtsDiscord> openrts_hello_discord(ByteView body);
[[nodiscard]] std::optional<std::uint16_t> decode_openrts_hello(ByteView body);
// The answer says the game is a thunderstorm (no byte for it: no).
[[nodiscard]] bool openrts_hello_storm(ByteView body);

// helper (openRTS's own): a player becomes another's helper (помощник
// машиниста), as RTS's 0x461F does through the dispatcher; here a player
// asks and the engineer answers, or the dispatcher assigns. One layout for
// every kind.
enum class HelperKind : std::uint8_t {
    ask = 1,     // player -> server: help on `train` (0: stop helping)
    asked = 2,   // server -> engineer: `user` (`nick`) asks to help on `train`
    answer = 3,  // engineer -> server: `user` taken (`value` 1) or not (0)
    helping = 4, // server -> helper: on `train` of `user` (`nick`), `value` a HelperState
    helper = 5,  // server -> engineer: `user` (`nick`) helps on `train` now (`value` 1) or no longer (0)
    drop = 6,    // engineer -> server: `user` helps no more
};
enum class HelperState : std::uint8_t {
    ended = 0,   // not (or no longer) a helper
    waiting = 1, // asked, the engineer has not answered
    helping = 2,
    refused = 3, // the engineer said no
    cannot = 4,  // no such train, nobody drives it, or not an openRTS player
};
struct HelperNote {
    HelperKind kind = HelperKind::ask;
    std::uint32_t train{};
    std::uint32_t user{};
    std::uint8_t value{};
    std::u16string nick;
};
[[nodiscard]] Bytes encode_helper_note(const HelperNote& note);
[[nodiscard]] std::optional<HelperNote> decode_helper_note(ByteView body);

// helper_cab: what the engineer's DLLs show in car `car` of `train` (the
// cab a helper is in), as the viewer packs it (zlib over the DLL host's
// CarOutput).
struct HelperCab {
    std::uint32_t train{};
    std::uint32_t car{};
    Bytes packed;
};
[[nodiscard]] Bytes encode_helper_cab(const HelperCab& cab);
[[nodiscard]] std::optional<HelperCab> decode_helper_cab(ByteView body);

// helper_input: a helper's hand on the engineer's cab (car `car` of
// `train`). The engineer's DLLs do it as if his own hand did.
enum class HelperInputKind : std::uint8_t {
    watch = 1,   // the helper is in car `car` (SIZE_MAX as 0xFFFFFFFF: in none): its cab is sent to him
    operate = 2, // switch `id` to `state` (CanSwitch / Switched)
    drag = 3,    // switch `id` dragged `delta` states, `release`: let go
};
struct HelperInput {
    HelperInputKind kind = HelperInputKind::watch;
    std::uint32_t train{};
    std::uint32_t car{};
    std::uint32_t id{};
    std::uint32_t state{};
    float delta{};
    bool release = false;
};
[[nodiscard]] Bytes encode_helper_input(const HelperInput& input);
[[nodiscard]] std::optional<HelperInput> decode_helper_input(ByteView body);

// 0x4611 message: the dispatcher's text.
[[nodiscard]] std::optional<std::u16string> decode_message(ByteView body);
// 0x4616 pause: suspended or not.
[[nodiscard]] std::optional<bool> decode_pause(ByteView body);

} // namespace openrts::net
