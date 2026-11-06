#pragma once

#include <array>
#include <cstdint>
#include <optional>
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

// 0x4611 message: the dispatcher's text.
[[nodiscard]] std::optional<std::u16string> decode_message(ByteView body);
// 0x4616 pause: suspended or not.
[[nodiscard]] std::optional<bool> decode_pause(ByteView body);

} // namespace openrts::net
