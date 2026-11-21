#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <openrts/net/play.hpp>

// A placement (docs/placements.md): trains as they stand on a route's track,
// one file for the single player, the server and the dispatcher's console
// alike (a player's placement sent to the organiser is put as it is).
// UTF-8 lines:
//   openRTS placement 1
//   route <the route's folder name>       (optional)
//   include <another placement's name>    (optional, any number; put with it)
//   train <number> <kind> <subtype> <state> <top speed m/s> <node> <offset> <forward 0|1>
//   name <text>      (optional)
//   notes <text>     (optional; "\n" a new line, "\\" a backslash)
//   from <name>      (optional: the included placement it came from)
//   car <flags> <handbrake 0..1, "-" none> <folder\name.eng|.wag>
//   shoes <front>,<rear>   (optional, the car before: its brake shoes)
//   end
// The kind is RTS's (0 STATIC, 1 PLAYER, 2 TRAFFIC, 3 LOOSE), the subtype
// the console's category (0 other, 1 passenger, 2 freight), the state
// 0 "Холодный", 1 "Включен", 2 "Движется"; the number 0: the server's
// lowest free; the place the front of the first car on TDB vector node
// `node`, facing along it when `forward`; a car's flags as net::TrainCar's
// (1 flipped, 2 cold, 0x100 a chock at its end towards the train's front,
// 0x200 at the other: RTS's PlaceDrag 1, 2). Read as well: the header
// "openRTS server placement 1" (the first days of the format) and the
// single player's older files ("# openRTS placement", [train] blocks).
namespace openrts::net {

inline constexpr std::string_view placement_header = "openRTS placement 1";

struct PlacementCar {
    std::uint32_t flags{};
    std::optional<float> handbrake;
    std::u16string file; // "folder\\name.eng", under TRAINS/TRAINSET
    std::string shoes;   // "front,rear" by formats::brake_shoe_name, empty: the car's own
    [[nodiscard]] std::uint8_t chocks() const noexcept { return static_cast<std::uint8_t>((flags >> 8) & 3u); }
};

struct PlacementTrain {
    std::uint32_t number{};
    std::u16string name, notes;
    std::uint16_t kind{}, subtype{};
    std::uint8_t state{};
    float top_speed{};
    std::uint32_t node{};
    double offset{};
    bool forward = true;
    std::vector<PlacementCar> cars;
    std::string from; // the included placement it came from, empty: its own
};

struct Placement {
    std::string route;                 // its route's folder name, empty: not given
    std::vector<std::string> includes; // other placements of its folder, put with it
    std::vector<PlacementTrain> trains;
};

[[nodiscard]] std::string write_placement(const Placement& placement);
[[nodiscard]] std::string write_placement(const std::vector<PlacementTrain>& trains);
// The file's placement; `error` set when the text is not a placement.
[[nodiscard]] Placement parse_placement(std::string_view text, std::string& error);
// Its trains only.
[[nodiscard]] std::vector<PlacementTrain> read_placement(std::string_view text, std::string& error);
// Its trains with those of the placements it includes, and theirs in turn:
// each read once by `open` (its folder's file by name; none: missing), a
// placement already in it (`self`, or trains `from` it) not again. The
// included trains' `from` is their placement's name; `problems` the missing
// ones, the loops and the unreadable.
using PlacementOpener = std::function<std::optional<std::string>(const std::string& name)>;
[[nodiscard]] std::vector<PlacementTrain> resolve_placement(const Placement& placement, std::string_view self, const PlacementOpener& open,
                                                            std::vector<std::string>& problems);
// The route a placement is for against the one here (folder names, any
// case); one not given fits any.
[[nodiscard]] bool placement_route_fits(std::string_view placement_route, std::string_view route);
// A train of it as a console puts it on the track (console_place).
[[nodiscard]] ConsolePlace place_of(const PlacementTrain& train);
// A placement's name for a file: no path characters, not empty.
[[nodiscard]] bool placement_name_ok(std::string_view name);
// A placement's text as one line of a console command and back
// ("placement upload": "\\" and "\n" escaped).
[[nodiscard]] std::string placement_to_line(std::string_view text);
[[nodiscard]] std::string placement_from_line(std::string_view line);
// The command lines that upload a placement to the server under `name`
// (quoted): "placement upload <name> <part> <parts> <text>", each within
// a console command's package.
[[nodiscard]] std::vector<std::string> placement_upload_commands(std::string_view name, std::string_view text);

} // namespace openrts::net
