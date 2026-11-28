#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <openrts/simulation/train_route.hpp>

// A scenario train's timetable (its TrainSchedule; docs/activities.md, "The
// timetable"), as RTS keeps it (0x4751E4, 0x4755C8, 0x424488, 0x4FB1B4,
// 0x466F70). Each stop is found on the train's route after the stop before
// it: the next platform of its station the route passes (both ends
// platforms, NumSkipped such passes left out first). The stop is the
// platform's end the train reaches last, where its head stops, and the
// platform's length.
//
// Standing at a stop, the player starts the boarding (Ctrl+Num7). The
// arrival is noted, and the boarding takes the platform's
// PlatformMinWaitingTime for its PlatformNumPassengersWaiting, scaled to the
// stop's NumPassengers. It takes longer when only part of the train's
// passenger room stands at the platform. It ends when the train moves or no
// passenger room at the platform is open, and the departure is noted then.
// The stop is done only if the boarding was over.
namespace openrts::simulation {

class TrackNetwork;
struct TrainRoute;

// A platform the route passes: where its end met last is (the leg and its
// distance on it), and its length (timetable stops, traffic stopping at
// platforms).
struct PlatformPass {
    std::size_t leg{};
    double far{};    // leg distance of that end
    double length{};
    std::uint32_t far_item{};
};
[[nodiscard]] std::vector<PlatformPass> platform_passes(const TrackNetwork& network, const TrainRoute& route);

class TrackNetwork;

struct TimetableStop {
    std::string station;     // the scenario's name; once found, the platform's spelling
    std::int32_t skipped{};  // NumSkipped
    float passengers{};      // NumPassengers
    std::int32_t arrival{};  // s into the day
    std::int32_t departure{};
    // On the route.
    bool found = false;
    std::size_t leg{};
    double distance{};                    // leg distance of the platform's end the train reaches last
    double length{};                      // the platform's length, m
    std::uint32_t platform = 0xFFFFFFFFu; // that end's TDB item
    std::uint32_t min_wait{};             // its PlatformMinWaitingTime, s
    std::uint32_t waiting{};              // its PlatformNumPassengersWaiting
    // What happened (RTS's stop +121 / +124 and +120 / +128).
    bool arrived = false;  // the boarding started here
    bool departed = false; // left after the boarding was over
    std::optional<std::int32_t> arrived_at, departed_at; // s into the day
};

// Finds the stops on `route` in order (each after the one before it) and
// sets `found`, the place and the platform; the rest is kept as given.
void locate_stops(const TrackNetwork& network, const TrainRoute& route, std::vector<TimetableStop>& stops);

// The train as the timetable sees it.
struct TimetableTrain {
    std::size_t leg{};
    double head{};     // leg distance of the leading end
    double length{};   // end to end, m
    double lead_car{}; // the leading car's length, m
    float speed{};     // m/s
    // Every car with passenger room: its centre's distance behind the head
    // (m) and its PassengerCapacity.
    std::vector<std::pair<double, float>> cars;
    bool doors_open = true; // the doors are open (a train that checks them)
};

// Where a train's head is against a stop: F6's distance colours (RTS 0x466F70).
enum class StopPlace : std::uint8_t {
    ahead,    // the platform not reached yet (white)
    partly,   // the train partly at it (yellow)
    in_place, // the whole train at it (green): the boarding can start
    covering, // longer than the platform, its head at the platform's end (lime): the boarding can start
    passed,   // left behind (red)
};

enum class BoardingStart : std::uint8_t { started, already, moving, no_stop, doors_closed };

enum class TimetableEventKind : std::uint8_t {
    boarding_started, // RTS event 22
    boarding_over,    // the boarding time ran out: the train may leave
    left,             // the boarding ended: the train moved or the doors closed (RTS event 23)
};

struct TimetableEvent {
    TimetableEventKind kind{};
    std::size_t stop{};
    bool done = false; // left: after the boarding was over
};

class Timetable {
public:
    Timetable() = default;
    // `tolerance`: s late still on time; `check_doors`: boarding wants the
    // doors open (TrainCheckOpenedDoorsAtStop).
    Timetable(std::vector<TimetableStop> stops, const TrainRoute& route, float tolerance, bool check_doors);

    [[nodiscard]] bool empty() const noexcept { return stops_.empty(); }
    [[nodiscard]] const std::vector<TimetableStop>& stops() const noexcept { return stops_; }
    [[nodiscard]] float tolerance() const noexcept { return tolerance_; }
    [[nodiscard]] bool check_doors() const noexcept { return check_doors_; }
    // m from the train's head to the stop along the route (negative: past it).
    [[nodiscard]] double distance(std::size_t stop, const TimetableTrain& train) const;
    [[nodiscard]] StopPlace place(std::size_t stop, const TimetableTrain& train) const;
    // The stop the train is at: the first not left whose platform it is on
    // (RTS 0x46AE0C).
    [[nodiscard]] std::optional<std::size_t> stop_at(const TimetableTrain& train) const;
    // The arrival noted no later than the timetable's plus the tolerance; the
    // departure no earlier than the timetable's.
    [[nodiscard]] bool arrived_on_time(const TimetableStop& stop) const;
    [[nodiscard]] static bool left_on_time(const TimetableStop& stop);

    // Ctrl+Num7 (RTS 0x424488): standing (under 0.5 m/s) at a stop, the
    // boarding starts there.
    BoardingStart start_boarding(const TimetableTrain& train, std::int32_t time_of_day);
    // Every frame: the boarding counts down and ends when the train moves
    // or no passenger room at the platform is open.
    void step(const TimetableTrain& train, float elapsed, std::int32_t time_of_day);
    [[nodiscard]] std::optional<std::size_t> boarding() const noexcept { return boarding_; }
    [[nodiscard]] float boarding_left() const noexcept { return left_; } // s (RTS train +132)
    [[nodiscard]] std::vector<TimetableEvent> take_events();

private:
    // RTS 0x4FB1B4: the passenger room at the platform with its doors open
    // against the train's; none, or the train moving, ends the boarding.
    void check_room(const TimetableTrain& train, std::int32_t time_of_day);
    [[nodiscard]] double route_distance(std::size_t leg, double distance) const;

    std::vector<TimetableStop> stops_;
    std::vector<double> leg_start_; // route distance where each leg starts
    float tolerance_{};
    bool check_doors_ = false;
    std::optional<std::size_t> boarding_;
    float left_{}; // s of boarding left
    float room_{}; // the open room at the platform the last time (RTS train +152)
    bool over_ = false;
    std::vector<TimetableEvent> events_;
};

} // namespace openrts::simulation
