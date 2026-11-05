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

// 0x4611 message: the dispatcher's text.
[[nodiscard]] std::optional<std::u16string> decode_message(ByteView body);
// 0x4616 pause: suspended or not.
[[nodiscard]] std::optional<bool> decode_pause(ByteView body);

} // namespace openrts::net
