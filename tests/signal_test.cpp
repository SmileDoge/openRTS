// Signals: the RTS script compiler (text assembly as RTS writes it), the VM
// rules, and the signal system on a synthetic track with a train.
#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <openrts/formats/rolling_stock.hpp>
#include <openrts/formats/route_world.hpp>
#include <openrts/formats/signal_config.hpp>
#include <openrts/formats/signal_script.hpp>
#include <openrts/formats/track_database.hpp>
#include <openrts/formats/track_sections.hpp>
#include <openrts/simulation/signals.hpp>
#include <openrts/simulation/track_network.hpp>
#include <openrts/simulation/train.hpp>

namespace {

using namespace openrts;
using formats::SignalAspect;

int failures = 0;

void check(const bool condition, const std::string& what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

// T_Head_YG of TestRoute_RTS and the assembly RTS wrote for it.
const std::string yg_source = R"(  header line padding for the 32-character start of the search
SCRIPT T_Head_YG

	extern float	block_state ();
	extern float	next_sig_lr ();
	extern float	def_draw_state ();
	extern float	state;
	extern float	draw_state;
	extern float	enabled;
	float		next_state;

	state = SIGASP_STOP_AND_PROCEED;
	next_state = next_sig_lr (SIGFN_NORMAL);
	if ((next_state ==# SIGASP_STOP_AND_PROCEED) || (next_state ==# SIGASP_RESTRICTING))
	{
		state = SIGASP_APPROACH_1;
	}
	else
	{
		state = SIGASP_CLEAR_2;
	}
	draw_state = def_draw_state (state);
	if (state >=# SIGASP_APPROACH_1)
	{
		if (next_state ==# SIGASP_APPROACH_2)
		{
			draw_state = 3;
		}
		else if ((next_state ==# SIGASP_APPROACH_3) || (next_state ==# SIGASP_CLEAR_1))
		{
			draw_state = 4;
		}
	}
)";

const std::string yg_assembly =
    "MOV BX,1\r\nMOV state,BX\r\nMOV BX,1\r\nPUSH BX\r\nCALL next_sig_lr,BX\r\nMOV next_state,BX\r\nMOV AX,next_state\r\n"
    "MOV BX,1\r\nCMPE AX,BX\r\nMOV BX,next_state\r\nMOV CX,2\r\nCMPE BX,CX\r\nOR AX,BX\r\nJZ if0\r\nMOV BX,3\r\n"
    "MOV state,BX\r\nJMP else0\r\n:if0\r\nMOV BX,7\r\nMOV state,BX\r\n:else0\r\nMOV BX,state\r\nPUSH BX\r\n"
    "CALL def_draw_state,BX\r\nMOV draw_state,BX\r\nMOV AX,state\r\nMOV BX,3\r\nCMPGE AX,BX\r\nJZ if0\r\n"
    "MOV AX,next_state\r\nMOV BX,4\r\nCMPE AX,BX\r\nJZ if1\r\nMOV BX,3\r\nMOV draw_state,BX\r\nJMP else1\r\n:if1\r\n"
    "MOV AX,next_state\r\nMOV BX,5\r\nCMPE AX,BX\r\nMOV BX,next_state\r\nMOV CX,6\r\nCMPE BX,CX\r\nOR AX,BX\r\n"
    "JZ if2\r\nMOV BX,4\r\nMOV draw_state,BX\r\nJMP else2\r\n:if2\r\n:else2\r\n:else1\r\nJMP else0\r\n:if0\r\n:else0\r\n";

void test_compiler() {
    const auto file = formats::compile_signal_scripts(yg_source);
    check(file.scripts.size() == 1, "one script");
    if (file.scripts.empty()) return;
    const auto& script = file.scripts.front();
    check(script.ok(), "compiles: " + script.error);
    check(script.name == "t_head_yg", "script name lower-cased");
    check(script.assembly == yg_assembly, "assembly as RTS writes it");
    // Repeated labels: the second `JZ if0` jumps to the second `:if0`.
    std::vector<std::uint32_t> jz;
    for (std::size_t i = 0; i < script.code.size(); ++i) {
        if (script.code[i].op == formats::SignalOp::jz) jz.push_back(script.code[i].target);
    }
    // JZ if0 (the first :if0), JZ if0 (the second :if0, at the end), then
    // JZ if1 and JZ if2 in between.
    check(jz.size() == 4 && jz[0] < jz[2] && jz[2] < jz[3] && jz[3] < jz[1], "repeated labels resolve in order");

    // `a || b && c` is (a || b) && c: equal precedence, left to right.
    const auto precedence = formats::compile_signal_scripts(R"(  header line padding for the 32-character start of the search
SCRIPT P
	extern float state;
	float a;
	float b;
	float c;
	state = a || b && c;
)");
    check(!precedence.scripts.empty() && precedence.scripts.front().assembly
              == "MOV BX,a\r\nMOV CX,b\r\nOR BX,CX\r\nMOV CX,c\r\nAND BX,CX\r\nMOV state,BX\r\n",
          "|| and && share a precedence, applied left to right");
    // a, b and c are read before any write: the static check sees it.
    check(!precedence.scripts.empty() && precedence.scripts.front().reads_stale_local, "stale local detected");
}

// A straight 5 km track with three NORMAL signals facing forward.
std::unique_ptr<simulation::TrackNetwork> signal_track() {
    std::string placements;
    for (int i = 0; i < 10; ++i) placements += "1 0 0 0 0 1 1 00 0 0 0 0 " + std::to_string(i * 500.0) + " 0 0 0 ";
    const std::string database = "SIMISA@@@@@@@@@@JINX0T0t______\nTrackDB ( Serial ( 1 ) TrackNodes ( 3\n"
                                 "TrackNode ( 1 TrVectorNode ( TrVectorSections ( 10 " + placements
        + ") TrItemRefs ( 3 TrItemRef ( 0 ) TrItemRef ( 1 ) TrItemRef ( 2 ) ) ) TrPins ( 1 1 TrPin ( 2 1 ) TrPin ( 3 1 ) ) )\n"
          "TrackNode ( 2 TrEndNode ( 0 ) UiD ( 0 0 0 0 0 0 0 0 0 0 0 0 ) TrPins ( 1 0 TrPin ( 1 1 ) ) )\n"
          "TrackNode ( 3 TrEndNode ( 0 ) UiD ( 0 0 0 0 0 0 0 0 0 0 0 0 ) TrPins ( 1 0 TrPin ( 1 0 ) ) )\n"
          ") TrItemTable ( 3\n"
          "SignalItem ( TrItemId ( 0 ) TrItemSData ( 1000 00000002 ) TrSignalType ( 00000000 0 0 TestHead ) )\n"
          "SignalItem ( TrItemId ( 1 ) TrItemSData ( 2000 00000002 ) TrSignalType ( 00000000 0 0 TestHead ) )\n"
          "SignalItem ( TrItemId ( 2 ) TrItemSData ( 3000 00000002 ) TrSignalType ( 00000000 0 0 TestHead ) )\n"
          ") )\n";
    const std::string sections = "SIMISA@@@@@@@@@@JINX0T0t______\nTrackSections ( 1\n"
                                 "    TrackSection ( 1 SectionSize ( 1.5 500 ) )\n)\nTrackShapes ( 0 )\n";
    auto library = formats::parse_track_sections(sections);
    auto db = formats::parse_track_database(database);
    if (!library.ok() || !db.ok()) return nullptr;
    return std::make_unique<simulation::TrackNetwork>(std::move(db), std::move(library));
}

const std::string test_config = R"(SIMISA@@@@@@@@@@JINX0G0t______
LightsTab ( 1 LightsTabEntry ( "Red Light" colour ( 255 255 0 0 ) ) )
SignalTypes ( 1
	SignalType ( "TestHead"
		SignalFnType ( NORMAL )
		SignalLights ( 1 SignalLight ( 0 "Red Light" Position ( 0 3 0 ) Radius ( 0.1 ) ) )
		SignalDrawStates ( 3
			SignalDrawState ( 0 "Red" DrawLights ( 1 DrawLight ( 0 ) ) )
			SignalDrawState ( 1 "Yellow" )
			SignalDrawState ( 2 "Green" )
		)
		SignalAspects ( 3
			SignalAspect ( STOP "Red" )
			SignalAspect ( APPROACH_1 "Yellow" SpeedKPH ( 60 ) )
			SignalAspect ( CLEAR_2 "Green" )
		)
		SignalNumClearAhead ( 2 )
	)
)
SignalShapes ( 1
	SignalShape ( "test.s" "Test" SignalSubObjs ( 1 SignalSubObj ( 0 "HEAD" "Head" SigSubType ( SIGNAL_HEAD ) SigSubSType ( "TestHead" ) ) ) )
)
ScriptFiles ( ScriptFile ( sigscr.dat ) )
)";

const std::string test_scripts = R"(  header line padding for the 32-character start of the search
SCRIPT TestHead
	extern float block_state ();
	extern float next_sig_lr ();
	extern float def_draw_state ();
	extern float state;
	extern float draw_state;
	state = SIGASP_STOP;
	if (block_state () ==# BLOCK_CLEAR)
	{
		if (next_sig_lr (SIGFN_NORMAL) ==# SIGASP_STOP)
		{
			state = SIGASP_APPROACH_1;
		}
		else
		{
			state = SIGASP_CLEAR_2;
		}
	}
	draw_state = def_draw_state (state);
)";

const std::string engine_text = R"(Wagon ( loco Type ( Engine ) Mass ( 80t ) Size ( 3m 4m 20m ) WheelRadius ( 0.6m )
    Coupling ( Spring ( Stiffness ( 1e6 1e6 ) Damping ( 1e6 1e6 ) Break ( 1e7 1e7 ) r0 ( 0.1m 0.1m ) ) )
    Friction ( 1000 1 0 0 1 ) Adheasion ( 0.2 0.3 2 0 )
    BrakeEquipmentType ( "Triple_valve, Auxilary_reservoir" ) BrakeSystemType ( "Air_single_pipe" )
    MaxBrakeForce ( 100kN ) TripleValveRatio ( 2.5 ) MaxReleaseRate ( 20 ) MaxApplicationRate ( 30 )
    MaxAuxilaryChargingRate ( 2 ) BrakeCylinderPressureForMaxBrakeBrakeForce ( 55 ) )
Engine ( loco Type ( Electric ) MaxPower ( 2000kW ) MaxForce ( 200kN ) MaxVelocity ( 100km/h ) AirBrakesMainResVolume ( 30 ) ))";

void test_system() {
    auto network = signal_track();
    check(network != nullptr, "signal track builds");
    if (!network) return;
    auto config = std::make_shared<formats::SignalConfig>(formats::parse_signal_config(test_config));
    check(config->ok(), "test sigcfg: " + config->error);
    auto scripts = std::make_shared<formats::SignalScriptFile>(formats::compile_signal_scripts(test_scripts, config.get()));
    check(scripts->scripts.size() == 1 && scripts->scripts.front().ok(), "test script compiles");
    std::vector<formats::WorldObjectPlacement> signals;
    for (std::uint32_t i = 0; i < 3; ++i) {
        formats::WorldObjectPlacement placement{.kind = formats::WorldObjectKind::signal, .uid = i, .shape_file = "test.s"};
        placement.signal_units.push_back({.sub_object = 0, .track_item = i});
        signals.push_back(placement);
    }
    simulation::SignalSystem system(*network, config, scripts, signals);
    check(system.heads().size() == 3 && system.stats().unplaced == 0, "three placed heads");

    // No train: every block clear, no chain, so next_sig_lr gives STOP.
    system.update({}, simulation::train_step);
    for (const auto& head : system.heads()) check(head.aspect() == SignalAspect::approach_1, "free-running: APPROACH_1");
    check(system.heads()[0].speed > 16.0f && system.heads()[0].speed < 17.0f, "def_draw_state sets the speed (60 km/h)");

    // A train before the first signal: the chain clears from the far end.
    const auto engine = formats::parse_wagon(engine_text);
    simulation::Train train({{.definition = &engine}});
    std::string error;
    check(train.place(*network, 1, 500.0, true, &error), "train placed: " + error);
    const simulation::Train* trains[] = {&train};
    system.update(trains, simulation::train_step);
    check(system.heads()[2].aspect() == SignalAspect::approach_1, "last signal: APPROACH_1 (nothing beyond)");
    check(system.heads()[1].aspect() == SignalAspect::clear_2, "middle signal: CLEAR_2");
    check(system.heads()[0].aspect() == SignalAspect::clear_2, "first signal: CLEAR_2");
    const auto ahead = system.ahead(train);
    check(ahead.valid && ahead.object == 0 && std::abs(ahead.distance - 500.0) < 1.0 && ahead.aspect == SignalAspect::clear_2,
          "the train sees the first signal 500 m ahead at CLEAR_2");

    // Drive past the first signal: it goes to STOP behind the train (its
    // block is occupied) and the pass is reported.
    auto driver = simulation::attach_keyboard_driver(train);
    driver->reverser = 1;
    driver->throttle = driver->notches;
    std::vector<simulation::SignalPass> passes;
    for (int i = 0; i < 20 * 120 && train.front_end()->offset() < 1100.0; ++i) {
        train.step();
        system.update(trains, simulation::train_step);
        for (const auto& pass : system.take_passes()) passes.push_back(pass);
    }
    check(train.front_end()->offset() >= 1100.0, "the train passed the first signal");
    check(passes.size() == 1 && passes.front().object == 0 && !passes.front().at_danger(), "one pass, not at danger");
    check(system.heads()[0].aspect() == SignalAspect::stop, "the first signal is at STOP behind the train");
    check(system.block_state(0) == 1, "its block is occupied");
    check(system.ahead(train).object == 1, "the next signal is the middle one");

    // Backing off a little: the train last moved towards its rear. Another
    // train then leaves the middle signal to the free run (next_sig_lr gives
    // STOP: APPROACH_1); the player's train still runs the pass ahead of its
    // front (0x508520), so the signal keeps its aspect for the train
    // (CLEAR_2) - as RTS shows a shunting signal at ЗИП after backing off.
    driver->throttle = 0;
    driver->reverser = 0;
    train.add_velocity(-static_cast<float>(train.speed()) - 0.5f);
    for (int i = 0; i < 10; ++i) train.step();
    train.add_velocity(-static_cast<float>(train.speed()));
    system.update(trains, simulation::train_step);
    check(system.heads()[1].aspect() == SignalAspect::approach_1, "not the player's: the middle signal runs free (APPROACH_1)");
    system.set_player([&](const simulation::Train& t) { return t.id() == train.id(); });
    system.update(trains, simulation::train_step);
    check(system.heads()[1].aspect() == SignalAspect::clear_2, "the player's train keeps the signal ahead of its front (CLEAR_2)");
    check(system.ahead(train).object != 1, "what the train obeys still comes from the end it moves towards");
}

// Modes and overrides (0x499948, 0x508520): an override replaces the
// script's aspect; FAILED skips the script and lights nothing; a train in
// shunting mode gives its chain SHUNT, which turns proceed aspects into
// RESTRICTING.
void test_modes() {
    auto network = signal_track();
    if (!network) return;
    auto config = std::make_shared<formats::SignalConfig>(formats::parse_signal_config(test_config));
    auto scripts = std::make_shared<formats::SignalScriptFile>(formats::compile_signal_scripts(test_scripts, config.get()));
    std::vector<formats::WorldObjectPlacement> signals;
    for (std::uint32_t i = 0; i < 3; ++i) {
        formats::WorldObjectPlacement placement{.kind = formats::WorldObjectKind::signal, .uid = i, .shape_file = "test.s"};
        placement.signal_units.push_back({.sub_object = 0, .track_item = i});
        signals.push_back(placement);
    }
    simulation::SignalSystem system(*network, config, scripts, signals);
    system.update({}, simulation::train_step);
    check(system.heads()[0].aspect() == SignalAspect::approach_1, "modes: free-running APPROACH_1 to start with");

    system.set_override(0, {.aspect = SignalAspect::clear_2});
    system.update({}, simulation::train_step);
    check(system.heads()[0].aspect() == SignalAspect::clear_2, "an override's aspect replaces the script's");
    system.clear_override(0);
    system.update({}, simulation::train_step);
    check(system.heads()[0].aspect() == SignalAspect::approach_1, "cleared: the script decides again");

    system.set_override(1, {.mode = simulation::SignalMode::failed});
    system.update({}, simulation::train_step);
    check(static_cast<int>(system.heads()[1].state) == 9 && system.heads()[1].draw_state < 0.0f,
          "FAILED: no script, aspect FAILED, no lights");
    system.clear_override(1);
    system.update({}, simulation::train_step);
    check(system.heads()[1].aspect() == SignalAspect::approach_1 && system.heads()[1].mode == 0.0f, "FAILED cleared");

    const auto engine = formats::parse_wagon(engine_text);
    simulation::Train train({{.definition = &engine}});
    std::string error;
    check(train.place(*network, 1, 500.0, true, &error), "modes: train placed: " + error);
    const simulation::Train* trains[] = {&train};
    system.update(trains, simulation::train_step);
    check(system.heads()[0].aspect() == SignalAspect::clear_2, "a train's chain: CLEAR_2");
    system.set_train_settings(train.id(), {.mode = simulation::SignalMode::shunt});
    system.update(trains, simulation::train_step);
    check(system.heads()[0].mode == 1.0f && system.heads()[0].aspect() == SignalAspect::restricting,
          "a shunting train: SHUNT, CLEAR_2 becomes RESTRICTING");
    check(system.ahead(train).aspect == SignalAspect::restricting, "and the train obeys RESTRICTING");
    system.set_train_settings(train.id(), {.mode = simulation::SignalMode::normal});
    system.update(trains, simulation::train_step);
    check(system.heads()[0].mode == 0.0f && system.heads()[0].aspect() == SignalAspect::clear_2, "back to NORMAL: CLEAR_2");
}

} // namespace

int main() {
    test_compiler();
    test_system();
    test_modes();
    if (failures == 0) std::cout << "signal tests passed\n";
    return failures == 0 ? 0 : 1;
}
