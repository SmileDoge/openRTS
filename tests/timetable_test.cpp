// A scenario train's timetable (simulation/timetable.hpp): its stops found
// on the route as RTS finds them (the next pass of the station's platform,
// NumSkipped passes left out, a train starting on a platform), the boarding
// (Ctrl+Num7) and its time, a train partly at the platform boarding longer,
// leaving before the boarding is over, F6's places and colours.
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <openrts/core/installation.hpp>
#include <openrts/core/vfs.hpp>
#include <openrts/core/virtual_path.hpp>
#include <openrts/formats/activity.hpp>
#include <openrts/formats/track_database.hpp>
#include <openrts/formats/train_path.hpp>
#include <openrts/formats/track_sections.hpp>
#include <openrts/simulation/timetable.hpp>
#include <openrts/simulation/track_network.hpp>
#include <openrts/simulation/train_route.hpp>

namespace {

using namespace openrts;
using simulation::BoardingStart;
using simulation::StopPlace;
using simulation::TimetableEventKind;

int failures = 0;

void check(const bool condition, const std::string& what) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << what << '\n';
    }
}

bool near(const double a, const double b, const double tolerance = 1e-3) { return std::abs(a - b) <= tolerance; }

const char* sections_text = R"(SIMISA@@@@@@@@@@JINX0T0t______
TrackSections ( 1
    TrackSection ( 1 SectionSize ( 1.5 100 ) )
)
TrackShapes ( 0 )
)";

std::string straight(const int count) {
    std::string text;
    for (int i = 0; i < count; ++i) text += "1 0 0 0 0 1 1 00 0 0 0 0 " + std::to_string(100.0 * i) + " 0 0 0 ";
    return text;
}

std::string platform(const int id, const double offset, const int other, const std::string& station, const std::string& name) {
    return "PlatformItem ( TrItemId ( " + std::to_string(id) + " ) TrItemSData ( " + std::to_string(offset) + " 00000000 ) PlatformName ( \""
        + name + "\" ) Station ( \"" + station + "\" ) PlatformTrItemData ( 00000000 " + std::to_string(other)
        + " ) PlatformMinWaitingTime ( 180 ) PlatformNumPassengersWaiting ( 20 ) )\n";
}

// end 10 - [1: 3000 m along +z] - end 11, with platforms: "Тест" 100-300,
// "Каневская" 500-700, "Краснодар-1" 1200-1400 and 2000-2250.
std::unique_ptr<simulation::TrackNetwork> line() {
    const std::string nodes = "TrackNode ( 1 TrVectorNode ( TrVectorSections ( 30 " + straight(30)
        + ") TrItemRefs ( 8 TrItemRef ( 0 ) TrItemRef ( 1 ) TrItemRef ( 2 ) TrItemRef ( 3 ) TrItemRef ( 4 ) TrItemRef ( 5 ) TrItemRef ( 6 ) "
          "TrItemRef ( 7 ) ) ) TrPins ( 1 1 TrPin ( 10 1 ) TrPin ( 11 1 ) ) )\n"
          "TrackNode ( 10 TrEndNode ( 0 ) UiD ( 0 0 0 0 0 0 0 0 0 0 0 0 ) TrPins ( 1 0 TrPin ( 1 1 ) ) )\n"
          "TrackNode ( 11 TrEndNode ( 0 ) UiD ( 0 0 0 0 0 0 0 0 0 0 0 0 ) TrPins ( 1 0 TrPin ( 1 0 ) ) )\n";
    const std::string items = platform(0, 100, 1, "Тест", "Тест п.1") + platform(1, 300, 0, "Тест", "Тест п.1")
        + platform(2, 500, 3, "Каневская", "Каневская п.I") + platform(3, 700, 2, "Каневская", "Каневская п.I")
        + platform(4, 1200, 5, "Краснодар-1", "Краснодар-1 п.1") + platform(5, 1400, 4, "Краснодар-1", "Краснодар-1 п.1")
        + platform(6, 2000, 7, "Краснодар-1", "Краснодар-1 п.2") + platform(7, 2250, 6, "Краснодар-1", "Краснодар-1 п.2");
    const std::string tdb = "SIMISA@@@@@@@@@@JINX0T0t______\nTrackDB ( Serial ( 1 ) TrackNodes ( 11\n" + nodes + ") TrItemTable ( 8\n"
        + items + ") )\n";
    auto sections = formats::parse_track_sections(sections_text);
    auto db = formats::parse_track_database(tdb);
    check(sections.ok() && db.ok(), "the line parses: " + db.error);
    return std::make_unique<simulation::TrackNetwork>(std::move(db), std::move(sections));
}

simulation::TimetableStop stop(const std::string& station, const std::int32_t arrival, const std::int32_t departure,
                               const float passengers, const std::int32_t skipped = 0) {
    return {.station = station, .skipped = skipped, .passengers = passengers, .arrival = arrival, .departure = departure};
}

// Five cars of 20 m: an engine, then four carriages of 200 seats.
simulation::TimetableTrain train_at(const double head, const float speed = 0.0f) {
    simulation::TimetableTrain t;
    t.head = head;
    t.length = 100.0;
    t.lead_car = 20.0;
    t.speed = speed;
    t.cars = {{30.0, 200.0f}, {50.0, 200.0f}, {70.0, 200.0f}, {90.0, 200.0f}};
    return t;
}

// A scenario's timetable on its route: every stop found on its service's
// path (with the content: Kuban's 310-1, Novokuznetsk's express).
void test_content(const std::filesystem::path& root, const std::string& route_name, const std::string& scenario,
                  const std::uint32_t service_id, const std::size_t expected) {
    core::Vfs vfs;
    core::mount_installation(vfs, root, core::load_installation_config(root));
    vfs.build_index();
    const auto route = core::VirtualPath::parse("ROUTES/" + route_name);
    auto file = route ? formats::load_activity(vfs, *route, scenario) : std::nullopt;
    if (!file) {
        std::cout << "  no \"" << scenario << "\" in " << root.string() << ": skipped\n";
        return;
    }
    const auto* service = file->activity.service(service_id);
    check(service && !service->trains.empty() && service->trains.front().schedule.size() == expected,
          scenario + ": the service's schedule");
    if (!service || service->trains.empty()) return;
    std::string error;
    const auto network = simulation::TrackNetwork::load(vfs, *route, &error);
    check(network != nullptr, scenario + ": the route's track: " + error);
    if (!network) return;
    // The path from the scenario's folder, else the route's.
    auto paths = formats::load_train_paths(vfs, *route);
    if (const auto folder = formats::activity_folder(*route, file->file)) {
        auto own = formats::load_train_paths_in(vfs, *folder);
        paths.insert(paths.begin(), own.begin(), own.end());
    }
    const auto wanted = core::fold_case(service->path);
    const formats::TrainPath* path = nullptr;
    for (const auto& candidate : paths) {
        if (!path && candidate.ok()
            && (core::fold_case(candidate.file) == wanted || core::fold_case(candidate.id) == wanted || core::fold_case(candidate.name) == wanted))
            path = &candidate;
    }
    check(path != nullptr, scenario + ": the path " + service->path);
    if (!path) return;
    const auto plan = simulation::plan_from_path(*network, *path, &error);
    check(plan.has_value(), scenario + ": the plan: " + error);
    if (!plan) return;
    const auto built = simulation::build_route(*network, *plan);
    check(built.ok(), scenario + ": the route: " + built.error);
    if (!built.ok()) return;
    std::vector<simulation::TimetableStop> stops;
    for (const auto& s : service->trains.front().schedule)
        stops.push_back({.station = s.station, .skipped = s.skipped, .passengers = s.passengers, .arrival = s.arrival, .departure = s.departure});
    simulation::locate_stops(*network, built, stops);
    double last = -1.0;
    std::size_t last_leg = 0;
    for (const auto& stop : stops) {
        std::cout << "  " << scenario << ": " << stop.station << (stop.found ? "" : " NOT FOUND") << ", leg " << stop.leg + 1 << " at "
                  << static_cast<long>(stop.distance) << " m, platform " << static_cast<long>(stop.length) << " m, wait " << stop.min_wait
                  << " s for " << stop.waiting << '\n';
        check(stop.found, scenario + ": " + stop.station + " on the path");
        check(stop.length > 20.0 && stop.length < 1000.0, scenario + ": " + stop.station + "'s platform length");
        check(stop.leg > last_leg || stop.distance > last, scenario + ": " + stop.station + " after the stop before it");
        last = stop.distance;
        last_leg = stop.leg;
    }
}

} // namespace

int main(int argc, char* argv[]) {
    const auto network = line();
    simulation::PathPlan plan;
    plan.name = "line";
    plan.start = {1, 150.0, true};
    simulation::PlanPoint end;
    end.kind = simulation::PlanPointKind::end;
    end.node = 1;
    end.offset = 2900.0;
    plan.points = {end};
    const auto route = simulation::build_route(*network, plan);
    check(route.ok() && route.legs.size() == 1, "the route builds: " + route.error);
    if (!route.ok()) return 1;

    const std::int32_t ten = 10 * 3600;
    std::vector<simulation::TimetableStop> stops{stop("Тест", ten, ten + 300, 10.0f), stop("каневская", ten + 900, ten + 1020, 10.0f),
                                                 stop("Краснодар-1", ten + 1800, 0, 100.0f, 1), stop("Нигде", ten + 2400, ten + 2400, 1.0f)};
    simulation::locate_stops(*network, route, stops);
    // The train starts on Тест's platform: its far end ahead, all of it counted.
    check(stops[0].found && near(stops[0].distance, 150.0) && near(stops[0].length, 200.0) && stops[0].platform == 1,
          "a train starting on a platform: the end ahead, the whole length (" + std::to_string(stops[0].distance) + ", "
              + std::to_string(stops[0].length) + ")");
    check(stops[1].found && near(stops[1].distance, 550.0) && near(stops[1].length, 200.0) && stops[1].station == "Каневская",
          "a station in another case: found, spelled as the platform");
    check(stops[1].min_wait == 180 && stops[1].waiting == 20, "the platform's wait and waiting passengers");
    check(stops[2].found && near(stops[2].distance, 2100.0) && near(stops[2].length, 250.0),
          "NumSkipped ( 1 ): the second pass of the station (" + std::to_string(stops[2].distance) + ")");
    check(!stops[3].found, "a station not on the path");

    {
        // After a stop the next is looked for further on only.
        std::vector<simulation::TimetableStop> again{stop("Краснодар-1", 0, 0, 1.0f), stop("Каневская", 0, 0, 1.0f),
                                                     stop("Краснодар-1", 0, 0, 1.0f)};
        simulation::locate_stops(*network, route, again);
        check(again[0].found && near(again[0].distance, 1250.0) && !again[1].found && again[2].found && near(again[2].distance, 2100.0),
              "each stop after the one before it");
    }

    simulation::Timetable timetable(stops, route, 60.0f, false);
    // At Тест: the whole train on the platform.
    auto t = train_at(140.0, 1.0f);
    check(timetable.place(0, t) == StopPlace::in_place, "the whole train at the platform: in place");
    check(timetable.stop_at(t) == 0, "the stop under the train");
    check(timetable.start_boarding(t, ten) == BoardingStart::moving, "moving: no boarding");
    t.speed = 0.0f;
    check(timetable.start_boarding(t, ten + 10) == BoardingStart::started && timetable.boarding() == 0, "standing: the boarding starts");
    check(near(timetable.boarding_left(), 90.0f), "the platform's 180 s for 20 passengers, for 10: 90 s");
    check(timetable.start_boarding(t, ten + 11) == BoardingStart::already, "once");
    check(timetable.stops()[0].arrived && timetable.stops()[0].arrived_at == ten + 10, "the arrival noted");
    auto events = timetable.take_events();
    check(events.size() == 1 && events[0].kind == TimetableEventKind::boarding_started, "its event");
    timetable.step(t, 60.0f, ten + 70);
    check(near(timetable.boarding_left(), 30.0f) && timetable.take_events().empty(), "counting down");
    timetable.step(t, 31.0f, ten + 101);
    events = timetable.take_events();
    check(timetable.boarding_left() == 0.0f && events.size() == 1 && events[0].kind == TimetableEventKind::boarding_over, "over");
    check(timetable.boarding() == 0, "the train still at the platform: boarding till it moves");
    t.speed = 1.0f;
    timetable.step(t, 0.1f, ten + 400);
    events = timetable.take_events();
    check(!timetable.boarding() && events.size() == 1 && events[0].kind == TimetableEventKind::left && events[0].done,
          "moving off ends it");
    check(timetable.stops()[0].departed && timetable.stops()[0].departed_at == ten + 400, "the departure noted, the stop done");
    check(timetable.arrived_on_time(timetable.stops()[0]) && simulation::Timetable::left_on_time(timetable.stops()[0]),
          "arrived before the time, left after it");

    // Каневская: the head 40 m past the platform's end, three carriages of
    // four on it: the boarding takes 4/3 as long.
    t = train_at(590.0);
    check(timetable.place(1, t) == StopPlace::partly, "partly at the platform");
    check(timetable.stop_at(t) == 1, "the stop done is not under it any more: the next one");
    check(timetable.start_boarding(t, ten + 950) == BoardingStart::started && near(timetable.boarding_left(), 120.0f),
          "three of four carriages at the platform: 120 s (" + std::to_string(timetable.boarding_left()) + ")");
    check(timetable.arrived_on_time(timetable.stops()[1]), "50 s late within the 60 s tolerance: on time");
    {
        auto late = timetable.stops()[1];
        late.arrived_at = ten + 961;
        check(!timetable.arrived_on_time(late), "61 s late: late");
        late.departed_at = ten + 1000;
        check(!simulation::Timetable::left_on_time(late), "left before the timetable's departure: early");
    }
    (void)timetable.take_events();
    t.speed = 2.0f;
    timetable.step(t, 1.0f, ten + 960);
    events = timetable.take_events();
    check(events.size() == 1 && events[0].kind == TimetableEventKind::left && !events[0].done, "left before the boarding was over");
    check(!timetable.stops()[1].departed && timetable.stops()[1].departed_at == ten + 960 && timetable.boarding_left() == 0.0f,
          "the stop not done");
    t.speed = 0.0f;
    check(timetable.stop_at(t) == 1, "and still open: it can be boarded again");

    // The places F6 colours.
    check(timetable.place(1, train_at(300.0)) == StopPlace::ahead, "ahead");
    check(timetable.place(1, train_at(800.0)) == StopPlace::passed, "passed: its rear past the platform's end");
    check(near(timetable.distance(2, train_at(300.0)), 1800.0), "the distance to a stop ahead");
    {
        auto long_train = train_at(555.0);
        long_train.length = 300.0;
        check(timetable.place(1, long_train) == StopPlace::covering, "a train longer than the platform, its head at the end");
        long_train.head = 530.0;
        check(timetable.place(1, long_train) == StopPlace::partly, "short of the end by more than half a car");
    }

    // A train checking its doors; one without passenger cars.
    {
        simulation::Timetable doors(stops, route, 0.0f, true);
        auto shut = train_at(140.0);
        shut.doors_open = false;
        check(doors.start_boarding(shut, ten) == BoardingStart::doors_closed, "doors shut: no boarding");
        simulation::Timetable empty_train(stops, route, 0.0f, false);
        auto engine = train_at(140.0);
        engine.cars.clear();
        check(empty_train.start_boarding(engine, ten) == BoardingStart::started && !empty_train.boarding(),
              "no passenger room: over at once");
        events = empty_train.take_events();
        check(events.size() == 2 && events[1].kind == TimetableEventKind::left && !events[1].done && !empty_train.stops()[0].departed,
              "not done");
        check(empty_train.stop_at(train_at(1000.0)) == std::nullopt, "no stop between platforms");
    }

    if (argc > 1) {
        test_content(argv[1], "Kuban_RTS", "Пассажирский поезд №310-1", 3, 5);
        test_content(argv[1], "Novokuznetsk", "Скорый поезд 77", 4, 4);
    }

    if (failures == 0) std::cout << "timetable: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
