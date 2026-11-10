// An RTS scenario's events run (viewer/src/activity_view) on a made-up
// world: the start's messages with a pause between, a modal message holding
// the rest, boarding and the report menu (F8), a signal overridden and let
// go, a coupling handing the train to another service (its OnStart), a place
// reached, a car's brake cylinder, an event enabled by another, outcomes not
// run named; with an installation, Novokuznetsk's "Скорый поезд 77" played
// through from the first report to the end of the shift.
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <openrts/core/installation.hpp>
#include <openrts/core/vfs.hpp>
#include <openrts/formats/activity.hpp>

#include "../viewer/src/activity_view.hpp"

namespace {

int failures = 0;

void check(const bool condition, const std::string& what) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << what << '\n';
    }
}

using namespace openrts;
using viewer::ActivityTrainId;
using viewer::ActivityView;

// The world: who is in the cab, the places the trains have been to, cars'
// values, couplings to come, the signals overridden.
struct FakeWorld final : viewer::ActivityWorld {
    std::optional<ActivityTrainId> cab;
    std::vector<std::pair<ActivityTrainId, formats::ActivityPoint>> visited;
    double cylinder = 0.0;
    double speed = 0.0;
    double radio = 0.0; // the cab's stream 24 (the radio: 1 while the call to the station plays)
    bool all_cylinders = false; // every car's cylinder the same (else car 74's only)
    std::vector<std::pair<std::int32_t, std::uint32_t>> sounds;
    std::vector<bool> valves;
    std::vector<std::pair<ActivityTrainId, ActivityTrainId>> couplings;
    std::vector<std::pair<ActivityTrainId, ActivityTrainId>> reassigned;
    int overrides = 0, cancels = 0;
    std::optional<simulation::SignalOverride> last;
    struct Block {
        std::uint32_t service{};
        std::int32_t point{};
        bool enabled = false;
    };
    std::vector<Block> blocks; // ReleaseBlockPoint / EnableBlockPoint
    [[nodiscard]] bool released(const std::uint32_t service, const std::int32_t point) const {
        return std::any_of(blocks.begin(), blocks.end(), [&](const Block& b) { return b.service == service && b.point == point && !b.enabled; });
    }
    void set_block_point(const std::uint32_t service, const std::int32_t point, const bool enabled, double) override {
        blocks.push_back({service, point, enabled});
    }

    [[nodiscard]] bool in_cab(const ActivityTrainId train) const override { return cab && *cab == train; }
    [[nodiscard]] bool near(const ActivityTrainId train, const formats::ActivityPoint& at, const double radius, std::string_view) const override {
        for (const auto& [who, p] : visited) {
            if (!(who == train)) continue;
            const double dx = (p.tile_x - at.tile_x) * 2048.0 + p.x - at.x;
            const double dz = (p.tile_z - at.tile_z) * 2048.0 + p.z - at.z;
            if (std::hypot(dx, dz) <= radius) return true;
        }
        return false;
    }
    [[nodiscard]] std::optional<double> car_value(const viewer::ActivityCarRef& car, std::string_view, const std::string_view name, int,
                                                  int) const override {
        if (name == "BrakeCylinder" && (car.number == 74 || all_cylinders)) return cylinder;
        if (name == "Velocity") return speed;
        if (name == "SMSStream") return radio;
        return std::nullopt;
    }
    void sound_trigger(const std::int32_t object, const std::uint32_t code, const viewer::ActivityCarRef&) override {
        sounds.emplace_back(object, code);
    }
    void end_valves(ActivityTrainId, std::int32_t, std::int32_t, const bool open) override { valves.push_back(open); }
    [[nodiscard]] std::vector<std::pair<ActivityTrainId, ActivityTrainId>> take_couplings() override { return std::exchange(couplings, {}); }
    void reassign(const ActivityTrainId from, const ActivityTrainId to) override {
        reassigned.emplace_back(from, to);
        if (cab && *cab == from) cab = to;
        for (auto& [who, p] : visited)
            if (who == from) who = to;
    }
    bool override_signal(const formats::ActivityPoint&, const std::optional<simulation::SignalOverride>& value) override {
        (value ? overrides : cancels) += 1;
        last = value;
        return true;
    }
};

// Runs `seconds` of game time in frames, closing modal messages as they come
// when `ok` (the player's OK).
void run(ActivityView& view, viewer::ActivityWorld& world, double& clock, const double seconds, const bool ok = true) {
    for (double t = 0.0; t < seconds; t += 0.1) {
        if (ok && view.holds()) view.close_message();
        const float elapsed = view.holds() ? 0.0f : 0.1f; // the game waits for a modal message
        clock += elapsed;
        view.update(elapsed, clock, world);
    }
}

constexpr const char* sample = R"(SIMISA@@@@@@@@@@JINX0a1t______
Tr_Activity_Header ( RouteID ( R ) Name ( "Run" ) ActivityStarts ( 09:20:00 ) PlayerStartsInTile ( 1 2 ) PlayerStartPosition ( 0 0 0 ) )
ActivityMenus ( 2
	ActivityMenu ( MenuID ( 1 ) Name ( "Station" ) MenuItems ( 1 MenuItem ( ItemLabel ( "Report" ) MenuItemID ( 1 ) Enabled ( 1 ) Visible ( 1 ) ) ) )
	ActivityMenu ( MenuID ( 3 ) Name ( "Brakes" ) MenuItems ( 1 MenuItem ( ItemLabel ( "Ready" ) MenuItemID ( 3 ) Enabled ( 1 ) Visible ( 1 ) ) ) )
)
GlobalEvents ( 1
	ServiceEvent ( ID ( 1 ) ActivationLevel ( 1 ) Enabled ( 1 ) Action ( ActivityStarts ( FirstStartOnly ( 1 ) ) )
		Outcome ( ShowMessage ( Text ( "first" ) ShowModal ( 1 ) ) Suspend ( 1.0 ) ShowMessage ( Text ( "second" ) ShowModal ( 1 ) ) ) )
)
Services (
	Service ( ID ( 1 ) Name ( "Shunting" ) Type ( PLAYER ) Path ( A 1 )
		ServiceTrains ( 1 Train ( ID ( 1 ) Consist ( Loco 1 ) TrainStartTime ( 09:19:00 ) ) )
		ServiceEvents ( 4
			ServiceEvent ( ID ( 1 ) Enabled ( 1 ) Action ( PlayerBoardsTrain ( Area ( CABIN ) ) ) Outcome ( SetMainMenu ( 1 ) ) )
			ServiceEvent ( ID ( 2 ) Enabled ( 1 ) Action ( OnMenuSelect ( MenuID ( 1 ) MenuItemID ( 1 ) ) )
				Outcome ( ShowMessage ( Text ( "wait" ) ShowModal ( 1 ) ) Suspend ( 10.0 ) ShowMessage ( Text ( "go" ) ShowModal ( 1 ) ) SetMainMenu ( -1 )
					SignalOverride ( Position ( 1 2 10 0 20 ) SignalMode ( SHUNTING ) SetAspect ( -1 RESTRICTING ) SetDrawState ( -1 -2 ) )
					ReleaseBlockPoint ( 1 1 ) EnableEvent ( 1 4 ) ) )
			ServiceEvent ( ID ( 3 ) Enabled ( 1 ) Action ( CoupleCars ( ConjTrainID ( 2 4 ) ) ) Outcome ( AssignService ( NewID ( 4 1 ) NewPathLoad (  ) ) ) )
			ServiceEvent ( ID ( 4 ) Enabled ( 0 ) Action ( ReachLocation ( Location ( 1 2 100 0 0 ) Radius ( 5 ) ) ) Outcome ( PostMessage ( Text ( "enabled one" ) ) ) )
		)
	)
	Service ( ID ( 2 ) Name ( "Standing" ) Type ( STATIC ) ServiceTrains ( 1 Train ( ID ( 4 ) Consist ( Cars 1 ) ) ) )
	Service ( ID ( 4 ) Name ( "Express" ) Type ( PLAYER ) Path ( B 1 ) ServiceTrains ( 1 Train ( ID ( 1 ) Consist ( Loco 1 ) ) )
		ServiceEvents ( 4
			ServiceEvent ( ID ( 1 ) Enabled ( 1 ) Action ( OnStart (  ) )
				Outcome ( SetMainMenu ( 3 ) SignalOverride ( Position ( 1 2 10 0 20 ) Cancel (  ) ) StartWagonStateTracking ( Enable ( 1 ) ) ) )
			ServiceEvent ( ID ( 2 ) Enabled ( 1 ) Action ( OnMenuSelect ( MenuID ( 3 ) MenuItemID ( 3 ) ) )
				Outcome ( ShowHint ( Text ( "brake" ) ) ) )
			ServiceEvent ( ID ( 3 ) Enabled ( 1 ) Action ( WagonState ( TrainID ( 4 1 ) AboveOrEqual ( WAGON BrakeCylinder 0 -1 1.4 ) WagonNum ( 74 ) ) )
				Outcome ( PostMessage ( Text ( "braked" ) ) ) )
			ServiceEvent ( ID ( 4 ) Enabled ( 1 ) Action ( ReachLocation ( Location ( 1 3 0 0 0 ) Radius ( 15 ) ) )
				Outcome ( Suspend ( 2 ) SuccessfullyCompleteActivity (  ) ) )
		)
	)
)
)";

// What RTS's activity.pdf says of the events: a message's buttons and
// OnCloseMessageBox, the activation level, Suspend ( 0 ) till ActivateEvent,
// WagonState only while tracked, a Permanent event's dynamic relation each
// time it comes true, sounds and end valves.
constexpr const char* semantics = R"(SIMISA@@@@@@@@@@JINX0a1t______
Tr_Activity_Header ( RouteID ( R ) Name ( "Rules" ) ActivityStarts ( 10:00:00 ) )
GlobalEvents ( 4
	ServiceEvent ( ID ( 1 ) Enabled ( 1 ) Action ( ActivityStarts (  ) )
		Outcome ( ShowMessage ( MsgID ( 7 ) Text ( "ask" ) Buttons ( MB_YESNO ) ) Suspend ( 0 ) PostMessage ( Text ( "resumed" ) ) ) )
	ServiceEvent ( ID ( 2 ) Enabled ( 1 ) Action ( OnCloseMessageBox ( MsgID ( 7 ) ButtonID ( ID_NO ) ) )
		Outcome ( IncreaseActivationLevel ( -1 3 1 ) ActivateEvent ( -1 1 ) StartWagonStateTracking ( Enable ( 1 ) TrainID ( 1 1 ) ) ) )
	ServiceEvent ( ID ( 3 ) ActivationLevel ( 0 ) Enabled ( 1 ) Action ( ActivateByTimer ( 0 ) )
		Outcome ( PostMessage ( Text ( "levelled" ) ) SendSoundTrigger ( 0 901 ) ) )
	ServiceEvent ( ID ( 4 ) Enabled ( 1 ) Permanent ( 1 ) Action ( WagonState ( TrainID ( 1 1 ) WagonNum ( 0 ) IncPast ( WAGON Velocity 0 0 5 ) ) )
		Outcome ( PostMessage ( Text ( "fast" ) ) ModifyWagons ( WagonParameters ( 1 WagonGroup ( OpenEndValves ( 1 ) ) ) ) ) )
)
Services ( Service ( ID ( 1 ) Name ( "Me" ) Type ( PLAYER ) Path ( A 1 ) ServiceTrains ( 1 Train ( ID ( 1 ) Consist ( A 1 ) TrainStartTime ( 10:00:00 ) ) ) ) )
)";

std::size_t journal_count(const ActivityView& view, const std::string& text);

void test_semantics() {
    auto activity = std::make_shared<const formats::Activity>(formats::parse_activity(semantics));
    check(activity->ok() && activity->global_events.size() == 4 && activity->global_events[3].permanent, "the rules read");
    ActivityView view(activity);
    FakeWorld world;
    double clock = 10 * 3600;
    world.speed = 3.0;
    view.start(clock, world);
    run(view, world, clock, 1.0, false);
    check(view.open_messages() == 1 && !view.holds(), "a message that is not modal: the game goes on");
    world.speed = 6.0;
    run(view, world, clock, 0.5, false);
    check(journal_count(view, "fast") == 0, "WagonState waits while its train is not tracked");
    check(journal_count(view, "levelled") == 0, "an event at level 0 waits");
    check(journal_count(view, "resumed") == 0, "Suspend ( 0 ): till ActivateEvent");
    view.close_message("ID_NO");
    run(view, world, clock, 0.5, false);
    check(journal_count(view, "resumed") == 1, "OnCloseMessageBox ( 7 ID_NO ): its ActivateEvent resumes the suspended event");
    check(journal_count(view, "levelled") == 1 && world.sounds.size() == 1 && world.sounds[0] == std::pair<std::int32_t, std::uint32_t>{0, 901},
          "its IncreaseActivationLevel arms the timer, whose sound trigger goes to object 0");
    world.speed = 3.0;
    run(view, world, clock, 0.3, false);
    world.speed = 6.0;
    run(view, world, clock, 0.3, false);
    check(journal_count(view, "fast") == 1 && world.valves.size() == 1 && world.valves[0], "tracked: IncPast fires as 5 m/s is passed, the end valves open");
    run(view, world, clock, 0.5, false);
    check(journal_count(view, "fast") == 1, "not again while it stays above");
    world.speed = 4.0;
    run(view, world, clock, 0.3, false);
    world.speed = 7.0;
    run(view, world, clock, 0.3, false);
    check(journal_count(view, "fast") == 2, "Permanent: again when it is passed again");
}

std::size_t journal_count(const ActivityView& view, const std::string& text) {
    std::size_t n = 0;
    for (const auto& entry : view.journal()) n += entry.text == text ? 1 : 0;
    return n;
}

bool journal_has(const ActivityView& view, const std::string& text) {
    for (const auto& entry : view.journal())
        if (entry.text == text) return true;
    return false;
}

void test_sample() {
    auto activity = std::make_shared<const formats::Activity>(formats::parse_activity(sample));
    check(activity->ok() && activity->menus.size() == 2 && activity->menu(3) && activity->menu(3)->items.front().label == "Ready",
          "the menus read");
    ActivityView view(activity);
    FakeWorld world;
    double clock = 9 * 3600 + 20 * 60;
    view.start(clock, world);
    check(view.player() == ActivityTrainId{1, 1}, "the player's train: the shunting service's");
    check(view.open_messages() == 1 && view.holds(), "the start's first message, modal");
    run(view, world, clock, 0.5, false);
    check(view.open_messages() == 1 && journal_has(view, "first") && !journal_has(view, "second"), "a modal message holds the rest");
    view.close_message();
    run(view, world, clock, 0.5, false);
    check(!journal_has(view, "second"), "then the pause (1 s) runs");
    run(view, world, clock, 1.0, false);
    check(journal_has(view, "second"), "and the second message comes");
    view.close_message();

    check(view.main_menu() == -1, "no report before boarding");
    world.cab = ActivityTrainId{1, 1};
    run(view, world, clock, 0.2);
    check(view.main_menu() == 1, "in the cab: the station's menu (SetMainMenu)");
    view.choose(1, 1);
    run(view, world, clock, 0.2, false);
    check(journal_has(view, "wait") && view.holds(), "the report's answer");
    view.close_message();
    run(view, world, clock, 5.0, false);
    check(!journal_has(view, "go"), "10 s to wait");
    run(view, world, clock, 6.0, false);
    check(journal_has(view, "go"), "then the next message");
    check(world.overrides == 0, "the signal waits for the message's OK");
    view.close_message();
    run(view, world, clock, 0.2);
    check(world.overrides == 1 && world.last && world.last->mode == simulation::SignalMode::shunt
              && world.last->aspect == formats::SignalAspect::restricting && world.last->draw_state == -2,
          "the shunting signal opened (SignalOverride)");
    check(view.main_menu() == -1, "the menu taken away");
    check(world.released(1, 1) && !view.not_run().contains("releaseblockpoint"), "ReleaseBlockPoint ( 1 1 ): service 1's point 1 released");

    world.visited.emplace_back(ActivityTrainId{1, 1}, formats::ActivityPoint{.tile_x = 1, .tile_z = 2, .x = 102.0});
    run(view, world, clock, 0.2);
    check(journal_has(view, "enabled one"), "an event enabled by another fires at its place");

    world.couplings.emplace_back(ActivityTrainId{1, 1}, ActivityTrainId{2, 4});
    run(view, world, clock, 0.2);
    check(world.reassigned.size() == 1 && world.reassigned[0].second == ActivityTrainId{4, 1} && view.player() == ActivityTrainId{4, 1},
          "coupled: the train is the express service's (AssignService)");
    check(view.main_menu() == 3 && world.cancels == 1, "its OnStart: the brakes' menu, the signal let go");
    view.choose(3, 3);
    run(view, world, clock, 0.2);
    check(journal_has(view, "brake"), "the menu item's hint");
    world.cylinder = 1.0;
    run(view, world, clock, 0.2);
    check(!journal_has(view, "braked"), "the cylinder under 1.4");
    world.cylinder = 1.5;
    run(view, world, clock, 0.2);
    check(journal_has(view, "braked"), "car 74's cylinder at 1.5 (WagonState)");
    world.visited.emplace_back(ActivityTrainId{4, 1}, formats::ActivityPoint{.tile_x = 1, .tile_z = 3, .x = 10.0});
    run(view, world, clock, 1.0);
    check(view.outcome() == ActivityView::Outcome::running, "the end after its 2 s pause");
    run(view, world, clock, 1.5);
    check(view.outcome() == ActivityView::Outcome::succeeded, "the scenario completed");
}

// Novokuznetsk's "Скорый поезд 77" through: every report its menus offer,
// the coupling, the places of the way, the shift handed over.
void test_express(const std::filesystem::path& root) {
    core::Vfs vfs;
    core::mount_installation(vfs, root, core::load_installation_config(root));
    vfs.build_index();
    const auto route = core::VirtualPath::parse("ROUTES/Novokuznetsk");
    auto file = route ? formats::load_activity(vfs, *route, "Скорый поезд 77") : std::nullopt;
    if (!file) {
        std::cout << "  no \"Скорый поезд 77\" in " << root.string() << ": its run skipped\n";
        return;
    }
    auto activity = std::make_shared<const formats::Activity>(std::move(file->activity));
    ActivityView view(activity);
    FakeWorld world;
    double clock = activity->start_time;
    view.start(clock, world);
    run(view, world, clock, 3.0);
    world.cab = view.player();
    run(view, world, clock, 1.0);
    // Reports, as the menus come; the coupling and the places as the
    // service's events ask for them.
    bool coupled = false;
    std::vector<std::pair<std::int32_t, std::int32_t>> reported; // a report once: a menu may stay (menu 2 till the coupling)
    for (int round = 0; round < 60 && view.outcome() == ActivityView::Outcome::running; ++round) {
        if (const auto* menu = activity->menu(view.main_menu())) {
            const std::pair choice{menu->id, menu->items.front().id};
            if (std::find(reported.begin(), reported.end(), choice) == reported.end()) {
                reported.push_back(choice);
                view.choose(choice.first, choice.second);
                run(view, world, clock, 200.0);
                continue;
            }
        }
        if (!coupled && view.player().service == 1) {
            world.couplings.emplace_back(view.player(), ActivityTrainId{2, 4});
            coupled = true;
            run(view, world, clock, 1.0);
            continue;
        }
        // The way: every place the player's service waits for.
        const auto* service = activity->service(view.player().service);
        if (!service) break;
        for (const auto& event : service->events)
            for (const auto& c : event.conditions)
                if (c.name == "reachlocation")
                    if (const auto* at = c.child("location"); at && at->values.size() >= 5)
                        world.visited.emplace_back(view.player(), formats::ActivityPoint{.tile_x = std::stoi(at->values[0]),
                                                                                         .tile_z = std::stoi(at->values[1]),
                                                                                         .x = std::stod(at->values[2]),
                                                                                         .y = std::stod(at->values[3]),
                                                                                         .z = std::stod(at->values[4])});
        run(view, world, clock, 5.0);
    }
    std::cout << "  \"Скорый поезд 77\": " << view.journal().size() << " messages, " << world.overrides << " signals held, "
              << world.cancels << " let go, the train now " << view.player().service << ' ' << view.player().train << "; not run:";
    for (const auto& name : view.not_run()) std::cout << ' ' << name;
    std::cout << '\n';
    check(coupled && world.reassigned.size() == 2 && view.player() == ActivityTrainId{13, 1},
          "Скорый поезд 77: shunting, the express, then Prokopyevsk's shunting");
    check(world.overrides >= 10 && world.cancels >= 10, "Скорый поезд 77: the shunting signals opened and let go");
    std::cout << "  \"Скорый поезд 77\": " << world.blocks.size() << " block points released\n";
    check(world.released(7, 1) && world.released(15, 1) && world.blocks.size() >= 10, "Скорый поезд 77: the traffic let go at its block points");
    check(view.outcome() == ActivityView::Outcome::succeeded, "Скорый поезд 77: the shift handed over, the scenario completed");
}

// Kuban's "Кубанский выльник" through: its reports are the radio's call to
// the station (the cab's sound stream 24 sounding, WagonState SMSStream),
// the brake test the last car's cylinder; the coupling and the places as
// the events ask.
void test_vylnik(const std::filesystem::path& root) {
    core::Vfs vfs;
    core::mount_installation(vfs, root, core::load_installation_config(root));
    vfs.build_index();
    const auto route = core::VirtualPath::parse("ROUTES/Kuban_RTS");
    auto file = route ? formats::load_activity(vfs, *route, "Кубанский Выльник") : std::nullopt;
    if (!file) {
        std::cout << "  no \"Кубанский Выльник\" in " << root.string() << ": its run skipped\n";
        return;
    }
    auto activity = std::make_shared<const formats::Activity>(std::move(file->activity));
    ActivityView view(activity);
    FakeWorld world;
    world.all_cylinders = true;
    double clock = activity->start_time;
    view.start(clock, world);
    run(view, world, clock, 3.0);
    world.cab = view.player();
    run(view, world, clock, 1.0);
    bool coupled = false;
    for (int round = 0; round < 120 && view.outcome() == ActivityView::Outcome::running; ++round) {
        // A call on the radio (the sound plays some seconds), then the answers.
        world.radio = 1.0;
        run(view, world, clock, 2.0);
        world.radio = 0.0;
        run(view, world, clock, 150.0);
        // The brake test: the last car's cylinder filled, then let off.
        world.cylinder = 2.0;
        run(view, world, clock, 2.0);
        world.cylinder = 0.0;
        run(view, world, clock, 60.0);
        if (!coupled && view.player().service == 1) {
            world.couplings.emplace_back(view.player(), ActivityTrainId{2, 1});
            coupled = true;
            run(view, world, clock, 1.0);
        }
        // The way: the next place the player's service waits for, one a round.
        const auto* service = activity->service(view.player().service);
        if (!service) break;
        bool moved = false;
        for (const auto& event : service->events) {
            for (const auto& c : event.conditions) {
                if (moved || c.name != "reachlocation") continue;
                const auto* at = c.child("location");
                if (!at || at->values.size() < 5) continue;
                const formats::ActivityPoint p{.tile_x = std::stoi(at->values[0]),
                                               .tile_z = std::stoi(at->values[1]),
                                               .x = std::stod(at->values[2]),
                                               .y = std::stod(at->values[3]),
                                               .z = std::stod(at->values[4])};
                const bool seen = std::any_of(world.visited.begin(), world.visited.end(), [&](const auto& v) {
                    return v.second.tile_x == p.tile_x && v.second.tile_z == p.tile_z && v.second.x == p.x && v.second.z == p.z;
                });
                if (seen) continue;
                world.visited.emplace_back(view.player(), p);
                moved = true;
            }
        }
    }
    std::cout << "  \"Кубанский выльник\": " << view.journal().size() << " messages, " << world.sounds.size() << " voice lines, "
              << world.overrides << " signals held, " << world.valves.size() << " end valve moves, the train now " << view.player().service
              << ' ' << view.player().train << "; not run:";
    for (const auto& name : view.not_run()) std::cout << ' ' << name;
    std::cout << '\n';
    check(coupled && view.player() == ActivityTrainId{4, 1}, "Кубанский выльник: shunting, then the player's train");
    check(world.sounds.size() >= 15, "Кубанский выльник: the station's voice lines (SendSoundTrigger)");
    std::cout << "  \"Кубанский выльник\": " << world.blocks.size() << " block points released\n";
    check(world.released(1, 1) && world.released(5, 1) && world.released(24, 1) && world.blocks.size() >= 18,
          "Кубанский выльник: the shunting's point and the traffic's let go");
    check(view.outcome() == ActivityView::Outcome::succeeded, "Кубанский выльник: completed");
}

} // namespace

int main(int argc, char* argv[]) {
    test_sample();
    test_semantics();
    if (argc > 1) {
        const std::filesystem::path root(std::u8string(reinterpret_cast<const char8_t*>(argv[1])));
        test_express(root);
        test_vylnik(root);
    }
    if (failures == 0) std::cout << "activity run: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
