#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <openrts/net/wire.hpp>

// The startup snapshot an RTS server sends a player after its login, which
// the client also saves as RTS/multdata and replays from (docs/net.md;
// docs/reverse/multiplayer-protocol.md, "The snapshot"). Fields keep the
// wire's values; the helpers below turn them into openRTS terms.
namespace openrts::net {

// 0x460A junction: RTS's node table index, which is the TDB node number - 1
// (client 0x4C3DE0 reads table[index]; 0x4603's node reads table[node - 1]).
struct JunctionState {
    std::uint32_t index{};
    std::uint32_t branch{};
    std::uint8_t lock{};
    [[nodiscard]] std::uint32_t node() const noexcept { return index + 1; }
};

// 0x460C signal_head: the head's TDB SignalItem (TrItemId).
struct SignalHeadState {
    std::uint32_t item{};
    std::uint16_t semaphore{}; // +78: the semaphore arm's position
    float state{};             // +84: the aspect (SIGASP_*)
    float draw_state{};        // +88
    float value_92{};          // +92, the client skips it
    float mode{};              // +96, the client skips it
    float speed{};             // +100: m/s, -1 none
};

// 0x4646 substation.
struct Substation {
    std::uint32_t id{}; // the .electrotype's ID
    std::uint32_t flags{}; // bit 0 switched off
    float voltage{};    // V
    std::uint32_t tail{};
};

// 0x4648 phase_voltages, per substation.
struct SubstationPhases {
    std::uint32_t id{};
    std::vector<float> voltages;
};

// 0x4647 section_source.
struct SectionSource {
    std::uint32_t section{};
    std::int16_t source{}; // -1 its own, -2 off, else a source
};

// 0x4645 piece_supply.
struct PieceSupply {
    std::uint32_t node{};  // TDB node, 1-based
    std::uint32_t piece{}; // index in the node's Electricity list
    std::int16_t source{};
    std::uint8_t keep{};
};

// 0x460F train_car, inside 0x4607 train_add.
struct TrainCar {
    std::uint16_t kind{};        // 0 wagon, 1 electric, 2 diesel
    std::u16string file;         // "folder\\name.eng", relative to TRAINS/TRAINSET
    std::uint32_t uid{};         // number << 18 | car index
    std::uint32_t flags{};       // 1 flipped, 2 cold, 0x100 / 0x200 chocks at the head / tail
    float mass{};                // kg
    float width{}, length{}, height{};
    std::optional<float> handbrake;     // 0..1
    std::optional<float> max_handbrake; // N (the client skips it)
    Bytes extra;                 // anything after them
    // openRTS's own, never on the wire: the car's key in the game, kept
    // through couplings and cuts so its locomotive DLL stays with it.
    std::uint32_t key{};
    [[nodiscard]] bool flipped() const noexcept { return (flags & 1u) != 0; }
};

// 0x4604 car_pos inside 0x4603 train_pos.
struct CarPos {
    double offset{}; // the car's middle, metres ahead of the train's rear end
    float speed{}, acceleration{};
};

// 0x4603 train_pos / 0x462F train_pos_forced: the REAR end of the train
// (RTS train +232: car offsets count from it) and the way the train faces.
struct TrainPos {
    float speed{}, acceleration{};
    std::uint32_t node{};   // TDB vector node, 1-based
    float offset{};         // along the node
    std::uint8_t direction{}; // 0 facing along the node (offset grows ahead), 1 against it
    std::vector<CarPos> cars;
    [[nodiscard]] bool forward() const noexcept { return direction == 0; }
};

// 0x4601 train_assign / 0x461E helper_assign.
struct TrainAssign {
    std::uint8_t mine{};
    std::uint32_t number{};
    std::uint16_t subtype{};
    std::uint16_t kind{};
};

// One train with everything the snapshot says about it.
struct SnapshotTrain {
    std::uint32_t id{};
    std::uint16_t kind{};    // 0 STATIC, 1 PLAYER, 2 TRAFFIC, 3 LOOSE
    std::uint16_t subtype{}; // 0 "Прочий", 1 "Пассажирский", 2 "Грузовой"
    float top_speed{};       // m/s
    std::uint8_t state{};    // 0 "Холодный", 1 "Включен", 2 "Движется"
    std::uint32_t value_4607{}; // the four bytes after the state
    std::vector<TrainCar> cars;
    float value_84 = 1.0f, value_88{}; // +84, +88 after the cars
    std::optional<TrainPos> pos;
    bool forced = false;     // the position came as 0x462F
    bool stands = false;     // 0x4606
    std::optional<TrainAssign> assign;
    std::optional<TrainAssign> helper;
    std::optional<std::array<std::u16string, 2>> names; // name, notes
    std::optional<std::array<float, 4>> brakes;          // brake pipe, main reservoir pipe, auxiliary, cylinders
    std::optional<std::pair<float, std::uint8_t>> route_left;
    std::vector<Bytes> cargo;  // 0x461C bodies
    std::optional<Bytes> derail; // the 0x4636 body
    [[nodiscard]] bool mine() const noexcept { return assign && assign->mine != 0; }
};

// 0x462E user and the snapshot's 0x462D player_pos share this layout.
struct PlayerRecord {
    std::uint32_t user{};
    std::uint32_t train{};
    std::uint32_t value_8{};
    std::uint32_t value_12{};
    std::array<float, 3> position{};
    std::uint32_t value_28{};
    std::array<float, 3> direction{};
    std::array<std::uint8_t, 32> name{}; // UTF-16, up to the first zero
    [[nodiscard]] std::string name_utf8() const;
};

// A package the snapshot model keeps as it came.
struct RawPackage {
    std::uint16_t id{};
    Bytes body;
};

struct Snapshot {
    bool root = false;   // began with an empty 0x4600 (the network stream; multdata has none)
    bool complete = false; // ended with 0x4608
    std::optional<float> time; // seconds of the day
    std::vector<JunctionState> junctions;
    std::vector<SignalHeadState> signals;
    std::vector<Substation> substations;
    std::vector<SubstationPhases> phases;
    std::vector<SectionSource> sections;
    std::vector<PieceSupply> pieces;
    std::vector<SnapshotTrain> trains;
    std::vector<PlayerRecord> users;   // 0x462E
    std::vector<PlayerRecord> players; // 0x462D
    std::optional<std::uint8_t> pause;
    std::optional<std::uint8_t> host_mark;
    std::vector<RawPackage> other; // track objects, weather, anything else, in order
};

struct SnapshotRead {
    Snapshot snapshot;
    std::vector<std::string> problems; // packages that did not parse, leftovers
    [[nodiscard]] bool ok() const noexcept { return problems.empty(); }
};

// Reads a snapshot: multdata as saved, or the packages of the network
// stream from the empty 0x4600 to 0x4608.
[[nodiscard]] SnapshotRead read_snapshot(ByteView data);
// Writes it in the order the RTS server sends it (0x41733C, as seen in the
// live capture of 2026-11-05).
[[nodiscard]] Bytes write_snapshot(const Snapshot& snapshot);

// The front of a train for placing it (openRTS places by the front end):
// metres from the rear end the snapshot gives to the front of car 0.
[[nodiscard]] double front_distance(const SnapshotTrain& train);

} // namespace openrts::net
