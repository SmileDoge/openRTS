#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <openrts/core/vfs.hpp>

namespace openrts::formats {

// Edge length of an MSTS world tile, in metres.
inline constexpr double route_tile_size = 2048.0;

// Route-local coordinates stay separate from the tile address. This avoids
// precision loss while a renderer streams a large MSTS/RTS route.
struct RoutePosition {
    std::int32_t tile_x{};
    std::int32_t tile_z{};
    double x{};
    double y{};
    double z{};
};

// One entry of the .trk Environment block, e.g. SummerClear -> sun.env.
struct RouteEnvironment {
    std::string condition;
    std::string file;
};

struct RouteDefinition {
    std::string route_id;
    std::filesystem::path root;
    std::optional<RoutePosition> start;
    std::vector<RouteEnvironment> environments;
    // SpeedLimit / TempRestrictedSpeed, m/s (0: not given). RTS uses the
    // speed limit for speed posts that end a restriction (0x482680).
    float speed_limit{};
    float temp_restricted_speed{};
    // Electrified ( 1 ): RTS gives an electric engine its own voltage on a
    // route without an .electrotype (docs/electrification.md).
    bool electrified = false;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

// World-file object kinds that place a shape file.
enum class WorldObjectKind {
    static_object,  // Static
    track,          // TrackObj: track section shape (usually GLOBAL/SHAPES)
    gantry,         // Gantry: catenary structures
    collide_object, // CollideObject
    signal,         // Signal
    speed_post,     // Speedpost
    transfer,       // Transfer: a texture laid on the ground (FileName is the texture)
};

[[nodiscard]] const char* to_string(WorldObjectKind kind) noexcept;

// One head of a world `Signal`: the shape's sub-object and its TDB item.
struct WorldSignalUnit {
    std::uint32_t sub_object{};
    std::uint32_t track_item{}; // TrItemId (the second value; the first is the database, 0 = TDB)
};

struct WorldObjectPlacement {
    WorldObjectKind kind = WorldObjectKind::static_object;
    std::uint32_t uid{};
    std::filesystem::path shape_file;
    RoutePosition position;
    std::array<float, 4> rotation{0.f, 0.f, 0.f, 1.f};
    // Signals: SignalSubObj (bit n = sub-object n shown) and the heads.
    std::uint32_t signal_sub_objects{};
    std::vector<WorldSignalUnit> signal_units;
    // CollideFlags (RTS 0x41B6D0, object +60): non-zero without bit 2 makes
    // the object an obstacle for derailed cars (RTS 0x58A630).
    std::uint32_t collide_flags{};
    // StaticFlags (hex, object +56): 0x40000 Terrain and 0x200000 Global
    // decide the object's draw pass (rts1.exe 0x4E0BFC).
    std::uint32_t static_flags{};
    // A transfer's Width and Height in metres (RTS's default 100).
    float transfer_width = 100.0f;
    float transfer_height = 100.0f;
};

struct WorldTile {
    std::int32_t tile_x{};
    std::int32_t tile_z{};
    // Every object with a shape file, in file order.
    std::vector<WorldObjectPlacement> objects;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

[[nodiscard]] std::optional<std::pair<std::int32_t, std::int32_t>>
parse_world_tile_name(std::string_view filename);
[[nodiscard]] RouteDefinition load_route_definition(const std::filesystem::path& route_root);
[[nodiscard]] WorldTile load_world_tile(const std::filesystem::path& file);

// Parsing from decoded file contents; `filename` gives the tile address.
[[nodiscard]] RouteDefinition parse_route_definition(std::string_view trk_bytes);
[[nodiscard]] WorldTile parse_world_tile(std::string_view bytes, std::string_view filename);

// Through the content VFS: `route` is the route folder (ROUTES/<name>); the
// .trk is `<name>.trk`, or the only .trk in the folder.
[[nodiscard]] RouteDefinition load_route_definition(const core::Vfs& vfs, const core::VirtualPath& route);
[[nodiscard]] WorldTile load_world_tile(const core::Vfs& vfs, const core::VirtualPath& file);

} // namespace openrts::formats
