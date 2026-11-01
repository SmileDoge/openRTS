#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <openrts/core/vfs.hpp>

// Track topology (.tdb): nodes, their links (pins) and the items placed along
// them. Section shapes come from tsection.dat (track_sections.hpp).
namespace openrts::formats {

// Placement of one section of a vector node: which section, where it starts
// (tile + local position) and its start angles (pitch, heading, roll).
struct TrackVectorSection {
    std::uint32_t section{};
    std::uint32_t shape{};
    std::int32_t world_tile_x{};
    std::int32_t world_tile_z{};
    std::uint32_t world_uid{};
    std::uint32_t flags1{};
    std::uint32_t flags2{};
    std::int32_t tile_x{};
    std::int32_t tile_z{};
    float x{}, y{}, z{};
    float pitch{}, heading{}, roll{}; // radians
};

// World object a junction or end node stands for, and its placement.
struct TrackNodeUid {
    std::int32_t world_tile_x{};
    std::int32_t world_tile_z{};
    std::uint32_t world_uid{};
    std::int32_t tile_x{};
    std::int32_t tile_z{};
    float x{}, y{}, z{};
    float pitch{}, heading{}, roll{};
};

// A link to another node. `direction` names the end of the linked node:
// for a linked vector node 1 = its start, 0 = its end; for a linked junction
// 1 = its trunk side, 0 = a branch side (checked on Kuban_RTS).
struct TrackPin {
    std::uint32_t node{};
    std::uint32_t direction{};
};

enum class TrackNodeKind : std::uint8_t { vector, junction, end };

struct TrackNode {
    std::uint32_t index{};
    TrackNodeKind kind = TrackNodeKind::end;
    // Vector nodes.
    std::vector<TrackVectorSection> sections;
    std::vector<std::uint32_t> items; // TrItemRefs, in file order
    // Junction nodes: the track shape, plus the two other numbers of
    // TrJunctionNode as read.
    std::uint32_t junction_shape{};
    std::uint32_t junction_first{};
    std::uint32_t junction_last{};
    std::optional<TrackNodeUid> uid;
    // TrPins: `in_pins` first, then `out_pins`. Vector: one each (start, end);
    // junction: one trunk, then the branches; end: one.
    std::uint32_t in_pin_count{};
    std::uint32_t out_pin_count{};
    std::vector<TrackPin> pins;
};

enum class TrackItemKind : std::uint8_t {
    empty,
    signal,
    speed_post,
    platform,
    siding,
    level_crossing,
    crossover,
    pickup,
    car_spawner,
    sound_region,
    hazard,
    other,
};

[[nodiscard]] const char* to_string(TrackItemKind kind) noexcept;

struct TrackSignalDirection {
    std::uint32_t junction{};
    std::uint32_t values[3]{};
};

struct TrackItem {
    std::uint32_t id{};
    TrackItemKind kind = TrackItemKind::empty;
    std::string keyword; // e.g. "SignalItem"
    float distance{};    // TrItemSData: offset along its vector node
    std::uint32_t data_flags{};
    bool has_position = false;
    std::int32_t tile_x{}, tile_z{};
    float x{}, y{}, z{};
    std::string name;    // platform / siding name, signal type
    std::string station; // platforms
    // Platforms and sidings: the item at the other end (PlatformTrItemData /
    // SidingTrItemData), 0xFFFFFFFF when none.
    std::uint32_t linked = 0xFFFFFFFFu;
    // Signals.
    std::uint32_t signal_flags{};
    std::uint32_t signal_direction{};
    float signal_angle{};
    std::vector<TrackSignalDirection> signal_directions;
    // Speed posts: raw SpeedpostTrItemData values.
    std::vector<float> speed_post_data;
};

// What a speed post means, read as RTS does (0x482680) from the
// SpeedpostTrItemData values: flags first; with flag 2 (a limit) the speed
// follows unless flag 1 is also set (the limit ends: back to the route's
// SpeedLimit), then a display number with flag 0x10; without flag 2 an
// optional speed (flag 8) and the post's number (kilometre or mile post,
// warning boards); the sign's rotation last. Speeds are km/h, mph with flag
// 0x100. Flags 0x20 / 0x40 / 0x80: passenger / freight / both (none: both).
struct SpeedPost {
    std::uint32_t flags{};
    bool limit = false;     // flag 2
    bool resume = false;    // flags 2 + 1: the restriction ends
    bool warning = false;   // flag 1 without 2: an advance warning board
    bool passenger = true;
    bool freight = true;
    float speed{};          // m/s (limits that do not resume)
    float number{};         // posts without flag 2: the number shown
    float angle{};          // radians, the sign's rotation
};

struct TrackItem;
[[nodiscard]] std::optional<SpeedPost> speed_post(const TrackItem& item);

struct TrackDatabase {
    std::uint32_t serial{};
    // Indexed by node number; entry 0 is unused (numbering starts at 1).
    std::vector<TrackNode> nodes;
    // Indexed by TrItemId.
    std::vector<TrackItem> items;
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
    [[nodiscard]] const TrackNode* node(std::uint32_t index) const;
};

[[nodiscard]] TrackDatabase parse_track_database(std::string_view bytes);
// `<route>/<name>.tdb`, or the only .tdb in the route folder.
[[nodiscard]] TrackDatabase load_track_database(const core::Vfs& vfs, const core::VirtualPath& route);

} // namespace openrts::formats
