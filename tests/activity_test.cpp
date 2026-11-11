// RTS scenarios (formats/activity): the header, services and their trains,
// events with their conditions and outcomes, the player's service, times;
// with a content root, the route's scenarios as they are (Kuban's).
#include <filesystem>
#include <iostream>
#include <string>

#include <openrts/core/installation.hpp>
#include <openrts/core/vfs.hpp>
#include <openrts/formats/activity.hpp>

namespace {

int failures = 0;

void check(const bool condition, const std::string& what) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << what << '\n';
    }
}

constexpr const char* sample = R"(SIMISA@@@@@@@@@@JINX0a1t______

Tr_Activity_Header (
	RouteID ( Kuban_RTS )
	Name ( "Test run" )
	Description ( "Two lines
of text" )
	ActivityStartsIn ( A )
	ActivityEndsIn ( B )
	Duration ( 04:00:00 )
	Season ( 3 )
	Weather ( 1 )
	ActivityStarts ( 03:00:00 )
	PlayerStartsInTile ( -4495 14567 )
	PlayerStartPosition ( -828.0731 15.0181 -442.3131 )
)
TrainsDistanceOfNoReturn ( 100 )
TrafficLimitsTable ( TrafficLimit ( 6000 60.0 ) TrafficLimit ( 300 0 ) TrafficLimit ( 500 12.5 ) )
GlobalBlockPoints ( 1 BlockPoint ( ID ( 1 ) Enabled ( 0 ) Location ( -4495 14567 10 1 20 ) PointAffects ( 2 3 7 ) ) )
GlobalEvents ( 1
	ServiceEvent (
		ID ( 1 )
		ActivationLevel ( 1 )
		Enabled ( 1 )
		Action ( ActivityStarts ( FirstStartOnly ( 1 ) ) )
		Outcome (
			ShowMessage ( Text ( "Hello" ) Title ( "Word" ) ShowModal ( 1 ) Buttons ( MB_OK ) )
			Suspend ( 4.0 )
			SendSoundTrigger ( 0 901 )
		)
	)
)
Services (
	Service (
		ID ( 1 )
		Name ( "Shunting" )
		Type ( PLAYER )
		Path ( Shunt 1 )
		ServiceTrains ( 1
			Train ( ID ( 1 ) Consist ( "My loco" 1 ) TrainMaxSpeed ( 25 ) TrainStartTime ( 02:59:00 ) TrainFlags ( CRITICAL_TRAIN )
				WagonParameters ( 2
					WagonGroup ( PlaceDrag ( 3 ) )
					WagonGroup ( Range ( 2 5 ) HandbrakePercent ( 50 ) CloseEndValves ( 2 ) DisconnectTrainPipe ( 1 ) ConnectEPCable ( 3 ) RaisePanto ( 1 ) )
				) )
		)
		ServiceEvents ( 1
			ServiceEvent ( ID ( 2 ) Enabled ( 0 ) Action ( WagonState ( WagonID ( 0001-0001-0006 ) Equal ( CABIN SMSStream 0 24 1.0 ) ) ) Outcome ( EnableEvent ( 1 2 ) ) )
		)
	)
	Service (
		ID ( 2 )
		Name ( "Standing" )
		Type ( STATIC )
		ServiceTrains ( 2
			Train ( ID ( 1 ) Consist ( Wagons 1 ) TrainPlacement ( PointStart ( -4496 14566 848.4456 15.0181 233.1279 ) PointHeading ( -4495 14567 -931.7742 15.0181 -835.8328 ) ) )
			Train ( ID ( 2 ) Consist ( Tanks 1 ) )
		)
	)
	Service ( ID ( 3 ) Name ( "Later" ) Type ( PLAYER ) Path ( "Main train" 1 ) ServiceTrains ( 1 Train ( ID ( 1 ) Consist ( X 1 ) ) ) )
	Service ( ID ( 4 ) Name ( "AI" ) Type ( TRAFFIC ) Path ( AI 1 )
		ServiceStartup ( ServiceStarts ( SINGLE ) )
		ServiceFlags ( NO_REMOVE FORCE_REMOVAL )
		ServiceTrains ( 2
			Train ( ID ( 1 ) Consist ( Y 1 ) TrainMaxSpeed ( 38 ) TrainAcceleration ( 0.4 -0.3 ) TrainEfficiency ( 5 ) StartupSpeed ( 2 )
				FinishSpeed ( 3 ) TrainStartOffset ( 15 ) TrainDistanceOfNoReturn ( 100 ) TrainStartTime ( 03:10:00 ) TrainRemoveTime ( 04:00:00 )
				TrainFlags ( IGNORE_SIGNALS critical_train ) )
			Train ( ID ( 2 ) Consist ( Z 1 ) TrainAcceleration ( 0.5 ) TrainStartTime ( 05:00:00 ) TrainRemoveTime ( 04:00:00 ) )
		)
		ServiceBlockPoints ( 2
			BlockPoint ( ID ( 1 ) Location ( -4496 14562 -1014.8284 23.5266 -125.1841
NumPasses: 1 ) Enabled ( 1 ) ReleaseOnTime ( 09:50:00 ) )
			BlockPoint ( ID ( 2 ) Enabled ( 1 ) Location ( 1 2 3 4 5 ) NumPassed ( 1 ) ReleaseOnReach ( -2 1 ) ReleaseOnApproach ( 3 -1 ) ReleaseManual ( 1 ) SuppressErrors ( ) )
		)
	)
)
)";

} // namespace

int main(int argc, char* argv[]) {
    using namespace openrts::formats;

    check(parse_activity_time("03:00:00") == 3 * 3600, "h:m:s");
    check(parse_activity_time("1:00") == 3600, "h:m");
    check(!parse_activity_time("soon"), "not a time");

    {
        const auto a = parse_activity(sample);
        check(a.ok(), "the sample reads: " + a.error);
        check(a.name == "Test run" && a.route_id == "Kuban_RTS", "name and route");
        check(a.description == "Two lines\nof text", "a quoted text over two lines");
        check(a.start_time == 3 * 3600 && a.duration == 4 * 3600, "start and duration");
        check(a.season == 3 && a.weather == 1, "season and weather");
        check(!a.player_train && a.player_position && a.player_position->tile_x == -4495 && a.player_position->tile_z == 14567
                  && std::abs(a.player_position->x + 828.0731) < 1e-3,
              "the player starts on foot at the header's place");
        check(a.services.size() == 4, "four services");
        const auto* player = a.player_service();
        check(player && player->id == 1 && player->path == "Shunt", "the player's service: the PLAYER one with a start time");
        check(player && player->trains.size() == 1 && player->trains.front().consist == "My loco"
                  && player->trains.front().start_time == 2 * 3600 + 59 * 60 && player->trains.front().max_speed == 25.0f,
              "its train");
        if (player && player->trains.size() == 1) {
            const auto& train = player->trains.front();
            check(!train.state, "no TrainState: none read (the runtime takes RTS's default)");
            check(train.wagon_groups.size() == 2, "WagonParameters: two groups");
            if (train.wagon_groups.size() == 2) {
                const auto& all = train.wagon_groups[0];
                check(all.first == -1 && all.last == -1 && all.place_drag == 3 && all.ends_set == 0 && !all.handbrake,
                      "PlaceDrag ( 3 ): chocks at both ends of the whole train");
                const auto& some = train.wagon_groups[1];
                check(some.first == 2 && some.last == 5 && some.place_drag == 0 && some.handbrake && *some.handbrake == 0.5f,
                      "a range and a handbrake percentage");
                check(some.ends_set == (0x80 | 0x01) && some.ends_clear == 0x30, "the end bits: cock rear, hose front shut; EP cables joined");
                check(some.other.size() == 1 && some.other.front() == "raisepanto", "what is not run is named");
            }
        }
        const auto* standing = a.service(2);
        check(standing && standing->type == ActivityServiceType::standing && standing->trains.size() == 2, "the STATIC service");
        check(standing && standing->trains[0].start && standing->trains[0].heading && standing->trains[0].heading->tile_z == 14567
                  && !standing->trains[1].start,
              "its trains' placements");
        check(a.service(4) && a.service(4)->type == ActivityServiceType::traffic, "the TRAFFIC service");
        // Traffic (docs/traffic.md).
        check(a.no_return == 250.0f, "TrainsDistanceOfNoReturn: at least 250 m");
        check(a.traffic_limits && a.traffic_limits->size() == 2 && (*a.traffic_limits)[1].radius == 500.0f
                  && (*a.traffic_limits)[1].speed == 12.5f,
              "TrafficLimitsTable: a limit of 0 is left out (RTS 0x470A3C)");
        check(a.global_block_points.size() == 1 && !a.global_block_points[0].enabled && a.global_block_points[0].affects == std::vector<std::int32_t>{3, 7},
              "a global block point: off, for services 3 and 7");
        if (const auto* ai = a.service(4); ai && ai->trains.size() == 2 && ai->block_points.size() == 2) {
            const auto& t = ai->trains[0];
            check(ai->starts == "SINGLE" && ai->flag("no_remove") && ai->flag("FORCE_REMOVAL") && !ai->flag("CHECK_IF_BUSY"),
                  "ServiceStarts and ServiceFlags");
            check(t.max_speed == 38.0f && t.acceleration == 0.4f && t.deceleration == 0.3f, "TrainAcceleration's magnitudes");
            check(t.efficiency == 2.0f && t.startup_speed == 2.0f && t.finish_speed == 3.0f && t.start_offset == 15.0f && t.no_return == 250.0f,
                  "efficiency clamped to 2, startup and finish speeds, offset, no return at least 250 m");
            check(t.start_time == 3 * 3600 + 600 && t.remove_time == 4 * 3600 && t.flag("ignore_signals") && t.flag("CRITICAL_TRAIN"),
                  "start and remove times, flags in any case");
            const auto& late = ai->trains[1];
            check(!late.start_time && !late.remove_time && late.acceleration == 0.5f && !late.deceleration && late.efficiency == 1.0f,
                  "a start after the removal: neither time counts (RTS 0x471B9C)");
            const auto& b1 = ai->block_points[0];
            check(b1.id == 1 && b1.enabled && b1.location && b1.location->tile_z == 14562 && std::abs(b1.location->z + 125.1841) < 1e-3
                      && b1.passed == 0 && b1.release_time == 9 * 3600 + 50 * 60,
                  "a block point as the RTS editor writes it (\"NumPasses: 1\" in Location is not read)");
            const auto& b2 = ai->block_points[1];
            check(b2.passed == 1 && b2.release_on_reach == std::pair<std::int32_t, std::int32_t>{-2, 1}
                      && b2.release_on_approach == std::pair<std::int32_t, std::int32_t>{3, -1} && b2.release_manual && b2.suppress_errors,
                  "NumPassed, ReleaseOnReach / ReleaseOnApproach, ReleaseManual, SuppressErrors");
        } else {
            check(false, "the TRAFFIC service's two trains and two block points");
        }
        check(a.global_events.size() == 1 && a.event_count() == 2, "events counted");
        const auto& event = a.global_events.front();
        check(event.enabled && event.conditions.size() == 1 && event.conditions[0].name == "activitystarts", "the start's condition");
        check(event.outcomes.size() == 3 && event.outcomes[0].name == "showmessage" && event.outcomes[0].text("text") == "Hello"
                  && event.outcomes[0].text("showmodal") == "1" && event.outcomes[1].name == "suspend",
              "its outcomes in order");
        const auto& wagon = a.services[0].events.front();
        check(!wagon.enabled && wagon.conditions[0].child("equal") && wagon.conditions[0].child("equal")->values.size() == 5,
              "a wagon state condition kept as written");
        check(!parse_activity("SIMISA@@@@@@@@@@JINX0a1t______\nNothing ( )").ok(), "no header: an error");
    }

    // The installation's scenarios (Kuban's "Кубанский Выльник"), when there.
    if (argc > 1) {
        openrts::core::Vfs vfs;
        const std::filesystem::path root(std::u8string(reinterpret_cast<const char8_t*>(argv[1])));
        openrts::core::mount_installation(vfs, root, openrts::core::load_installation_config(root));
        vfs.build_index();
        const auto route = openrts::core::VirtualPath::parse("ROUTES/Kuban_RTS");
        const auto all = route ? load_activities(vfs, *route) : std::vector<ActivityFile>{};
        const ActivityFile* vylnik = nullptr;
        for (const auto& file : all) {
            std::cerr << "  scenario " << file.path.str() << ": " << (file.activity.ok() ? "read" : file.activity.error) << ", "
                      << file.activity.services.size() << " services, " << file.activity.event_count() << " events\n";
            check(file.activity.ok(), file.file + " reads");
            if (file.activity.event_count() == 42) vylnik = &file;
        }
        if (vylnik) {
            const auto& a = vylnik->activity;
            check(a.services.size() == 23 && a.start_time == 3 * 3600 && a.duration == 4 * 3600, "Vylnik: 23 services, 03:00, 4 h");
            const auto* player = a.player_service();
            check(player && player->id == 1 && player->trains.size() == 1, "Vylnik: the shunting service is the player's start");
            const auto* standing = a.service(2);
            check(standing && standing->trains.size() == 32, "Vylnik: 32 standing trains");
            std::size_t traffic = 0, points = 0;
            for (const auto& service : a.services) {
                points += service.block_points.size();
                if (service.type != ActivityServiceType::traffic) continue;
                ++traffic;
                for (const auto& train : service.trains)
                    check(train.max_speed > 0.0f && train.acceleration > 0.0f, "Vylnik: " + service.name + ": traffic can run (speed, acceleration)");
            }
            check(traffic == 20 && points == 24, "Vylnik: 20 traffic services, 24 block points");
            const auto folder = activity_folder(*route, vylnik->file);
            check(folder && vfs.exists(folder->resolve(player ? player->path + ".pat" : std::string("x")).value_or(*folder)),
                  "Vylnik: the player's path in the scenario's own folder");
            check(load_activity(vfs, *route, vylnik->file).has_value(), "found again by its name");
        } else {
            std::cerr << "  (no Kuban scenario with 42 events in this installation: its checks skipped)\n";
        }
    }
    if (failures == 0) std::cout << "activity: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
