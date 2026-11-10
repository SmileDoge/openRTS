#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <openrts/core/vfs.hpp>

// RTS scenarios (activities): ROUTES/<route>/RTS_ACT/<name>.act, the
// scenario's own consists, paths and sounds in RTS_ACT/<name>/. The format
// is RTS's own (docs/RTSFileReference.html, "Activity"); see
// docs/activities.md for what openRTS reads and runs.
namespace openrts::formats {

// A place in the world: its tile and the metres in it.
struct ActivityPoint {
    std::int32_t tile_x{};
    std::int32_t tile_z{};
    double x{};
    double y{};
    double z{};
};

// A block of an event's condition (Action) or outcome, as written: its name
// (lower case), values and inner blocks; the runtime reads what it knows.
struct ActivityNode {
    std::string name;
    std::vector<std::string> values;
    std::vector<ActivityNode> children;

    [[nodiscard]] const ActivityNode* child(std::string_view key) const;
    // The first value of child `key` ("" when there is none).
    [[nodiscard]] std::string text(std::string_view key) const;
};

struct ActivityEvent {
    std::uint32_t id{};
    // It fires while its level is above 0 (IncreaseActivationLevel,
    // SetActivationLevel change it).
    std::int32_t activation_level = 1;
    bool enabled = true;
    bool permanent = false; // Permanent: it may fire again and again (else once)
    std::vector<ActivityNode> conditions; // Action ( ... )
    std::vector<ActivityNode> outcomes;   // Outcome ( ... ), in order
};

enum class ActivityServiceType : std::uint8_t {
    player,   // PLAYER: a train of the player's
    standing, // STATIC: trains standing where TrainPlacement says
    traffic,  // TRAFFIC: AI trains along their paths
    other,
};

struct ActivityTrain {
    std::uint32_t id{};
    std::string consist;                     // the .con file's stem
    std::optional<ActivityPoint> start;      // TrainPlacement: where the train starts
    std::optional<ActivityPoint> heading;    // a point towards which it stands
    std::optional<std::int32_t> start_time;  // TrainStartTime, s into the day
    std::optional<std::int32_t> remove_time; // TrainRemoveTime
    std::optional<float> max_speed;          // TrainMaxSpeed as written, m/s
    // TrainAcceleration ( MaxAcc MaxDec ), m/s^2 (RTS takes the magnitudes).
    std::optional<float> acceleration;
    std::optional<float> deceleration;
    float efficiency = 1.0f;                 // TrainEfficiency, clamped to [0.03, 2] (RTS 0x471B9C)
    float startup_speed{};                   // StartupSpeed, m/s
    float finish_speed{};                    // FinishSpeed: non-zero, the train is removed at its path's end
    float start_offset{};                    // TrainStartOffset, m along the path
    std::optional<float> no_return;          // TrainDistanceOfNoReturn (at least 250 m)
    std::string service_type;                // TrainServiceType: FREIGHT, PASSENGER...
    std::vector<std::string> flags;          // TrainFlags
    std::optional<int> state;                // TrainState

    [[nodiscard]] bool flag(std::string_view key) const; // TrainFlags has it (any case)
};

// A block point (activity.pdf ch. 3, RTS 0x475B1C): while enabled no train
// it holds gets its way reserved past it.
struct ActivityBlockPoint {
    std::int32_t id{};
    bool enabled = true;
    std::optional<ActivityPoint> location;
    int passed{};                         // NumPassed: crossings of the place by the path before the point
    std::vector<std::int32_t> affects;    // PointAffects: services (global) or trains (a service's); empty: all
    std::optional<std::int32_t> release_time; // ReleaseOnTime, s into the day
    // ReleaseOnReach / ReleaseOnApproach ( ServiceID TrainID ): released when
    // such a train reaches it / gets its way reserved through it.
    std::optional<std::pair<std::int32_t, std::int32_t>> release_on_reach;
    std::optional<std::pair<std::int32_t, std::int32_t>> release_on_approach;
    bool release_manual = false;          // ReleaseManual: the player may release it (Tab)
    bool suppress_errors = false;         // SuppressErrors
};

// TrafficLimitsTable's TrafficLimit ( Radius Limit ): traffic takes curves
// tighter than Radius m at Limit m/s.
struct ActivityTrafficLimit {
    float radius{};
    float speed{};
};

struct ActivityService {
    std::uint32_t id{};
    std::string name;
    ActivityServiceType type = ActivityServiceType::other;
    std::string type_name; // as written
    std::string path;      // Path: the .pat file's stem
    std::string starts;    // ServiceStarts: NONE, SINGLE, SERIAL
    std::vector<std::string> flags; // ServiceFlags: FORCE_REMOVAL, CHECK_IF_BUSY, NO_REMOVE, CHECK_PLAYER_APPROACH
    std::vector<ActivityTrain> trains;
    std::vector<ActivityEvent> events;
    std::vector<ActivityBlockPoint> block_points; // ServiceBlockPoints

    [[nodiscard]] const ActivityTrain* train(std::uint32_t train_id) const;
    [[nodiscard]] bool flag(std::string_view key) const; // ServiceFlags has it (any case)
};

// StaticObjects: a scenario's objects; SOUND ones are a .sms on a car
// (SoundType WAGON, SoundWagonID) that SendSoundTrigger plays.
struct ActivityObject {
    std::string type;       // SOUND, ...
    std::string file;       // Shape ( file location )
    int location = 1;       // 1: the scenario's folder
    bool visible = true;
    std::string sound_type; // WAGON
    std::string wagon_id;   // ssss-tttt-nnnn: the service and train it starts in, its consist UiD
};

// ActivityMenus: what the player reports (RTS: F8), set by SetMainMenu.
struct ActivityMenuItem {
    std::string label;
    std::int32_t id{};
    bool enabled = true;
    bool visible = true;
};
struct ActivityMenu {
    std::int32_t id{};
    std::string name;
    std::vector<ActivityMenuItem> items;
    bool main = false; // MainActivityMenu
};

struct Activity {
    std::string route_id;
    std::string name;
    std::string description;
    std::string starts_in; // ActivityStartsIn: the place it starts in
    std::string ends_in;
    std::int32_t start_time = 12 * 3600; // ActivityStarts, s into the day
    std::int32_t duration{};              // s
    int season = 1;                       // 0 spring, 1 summer, 2 autumn, 3 winter
    int weather = 0;                      // 0 clear, 1 snow, 2 rain
    // PlayerStartsInTrain: the player's service, train, car (from 0) and
    // area (CABIN...). Without it the player starts on foot at
    // player_position (PlayerStartsInTile and PlayerStartPosition).
    struct PlayerTrain {
        std::uint32_t service{};
        std::uint32_t train{};
        std::uint32_t wagon{};
        std::string area;
    };
    std::optional<PlayerTrain> player_train;
    std::optional<ActivityPoint> player_position;
    // TrainsDistanceOfNoReturn: from a junction this near the train its way
    // is not cancelled (default 5000 m, at least 250; RTS 0x470A3C).
    float no_return = 5000.0f;
    // TrafficLimitsTable as written (up to 10); none: RTS's defaults
    // (simulation::rts_traffic_limits).
    std::optional<std::vector<ActivityTrafficLimit>> traffic_limits;
    std::vector<ActivityBlockPoint> global_block_points; // GlobalBlockPoints
    std::vector<ActivityService> services;
    std::vector<ActivityEvent> global_events; // GlobalEvents
    std::vector<ActivityMenu> menus;          // ActivityMenus
    std::vector<ActivityObject> objects;      // StaticObjects
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
    [[nodiscard]] const ActivityService* service(std::uint32_t id) const;
    [[nodiscard]] const ActivityMenu* menu(std::int32_t id) const;
    // The service the player starts with: PlayerStartsInTrain's, else the
    // first PLAYER service whose train has a start time, else the first
    // PLAYER service (nullptr: none).
    [[nodiscard]] const ActivityService* player_service() const;
    [[nodiscard]] std::size_t event_count() const;
};

[[nodiscard]] Activity parse_activity(std::string_view bytes);

// "h:m[:s]" as seconds (nullopt: not a time).
[[nodiscard]] std::optional<std::int32_t> parse_activity_time(std::string_view text);

// A route's scenarios (RTS_ACT/*.act), each read whole, by file name.
struct ActivityFile {
    std::string file; // the file's stem
    core::VirtualPath path;
    Activity activity;
};
[[nodiscard]] std::vector<ActivityFile> load_activities(const core::Vfs& vfs, const core::VirtualPath& route);
// One of them by its stem (or a virtual path to an .act file).
[[nodiscard]] std::optional<ActivityFile> load_activity(const core::Vfs& vfs, const core::VirtualPath& route, std::string_view name);
// The folder of a scenario's own files: RTS_ACT/<stem>.
[[nodiscard]] std::optional<core::VirtualPath> activity_folder(const core::VirtualPath& route, std::string_view file);

} // namespace openrts::formats
