#pragma once

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

// 0x4623 state_request: mode 2 asks a train's owner for its whole look
// (lights, sound, car state), as rts1.exe does when it loads another
// player's train; mode 1 a helper's whole cab.
[[nodiscard]] Bytes encode_state_request(std::uint32_t mode, std::uint32_t train);

// 0x462D player_pos as a client sends it (44 bytes): the server keeps it
// and draws the person on the consoles' map; it relays a train's lights,
// look and sounds only to players within 3 km of it.
struct PlayerPosition {
    std::uint32_t train{}; // the train the player is in, 0 none
    std::uint32_t value_4{};
    std::uint32_t value_8{};
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
