// Trains with legacy DLLs under the realistic air (docs/train.md, "Realistic
// air with DLL locomotives"): the DLL controller on outputs made here, no DLL
// run. As RTS the train keeps RTS's air; with the DLL's valve the pipe
// follows its УР and its fast vent is an emergency; with openRTS's valve over
// Brakes.dll the cab's 395 handle (BR_395ID1) works the pipe. Brakes.dll's
// parameters are read from the .eng.
#include <cmath>
#include <iostream>
#include <memory>
#include <string>

#include <openrts/formats/rolling_stock.hpp>
#include <openrts/simulation/train.hpp>
#include <openrts/trainplugin/legacy_consist.hpp>

namespace {

using namespace openrts;

int failures = 0;

void check(const bool condition, const std::string& what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

const std::string common = R"(
    Size ( 3m 4m 20m )
    WheelRadius ( 0.6m )
    NumWheels ( 4 )
    Coupling ( Type ( Automatic ) Spring ( Stiffness ( 1e6 1e6 ) Damping ( 1e6 1e6 ) Break ( 1e7 1e7 ) r0 ( 0.1m 0.1m ) ) )
    Friction ( 1000 1 0 0 1 )
    Adheasion ( 0.2 0.3 2 0 )
    BrakeEquipmentType ( "Triple_valve, Auxilary_reservoir" )
    BrakeSystemType ( "Air_single_pipe" )
    MaxBrakeForce ( 100kN )
    TripleValveRatio ( 2.5 )
    MaxReleaseRate ( 20 )
    MaxApplicationRate ( 30 )
    MaxAuxilaryChargingRate ( 2 )
    BrakeCylinderPressureForMaxBrakeBrakeForce ( 55 )
)";

const std::string engine_text = "Wagon ( loco\n Type ( Engine )\n Mass ( 100t )\n" + common + R"()
Engine ( loco
    Type ( Electric )
    MaxPower ( 3000kW )
    MaxForce ( 300kN )
    AirBrakesMainResVolume ( 1000 )
)
EngExtended (
    AuxLibraries (
        AuxLibrary (
            AuxLibraryName ( "BRAKES" )
            AuxLibraryModule ( "../mdd_Libraries/Brakes/Brakes.dll" )
            AuxLibraryParameters ( 10
                BR_395ID1 ( 2 )
                BR_MANU1 ( 3 )
                BR_ZARPRESS ( 5.0 )
            )
        )
    )
))";

const std::string wagon_text = "Wagon ( car\n Type ( Carriage )\n Mass ( 50t )\n" + common + ")";

struct Rig {
    formats::WagonDefinition engine = formats::parse_wagon(engine_text);
    formats::WagonDefinition wagon = formats::parse_wagon(wagon_text);
    std::shared_ptr<trainplugin::LegacyConsist::Outputs> outputs = std::make_shared<trainplugin::LegacyConsist::Outputs>();
    std::shared_ptr<trainplugin::DllAirSetting> air = std::make_shared<trainplugin::DllAirSetting>();
    std::unique_ptr<simulation::TrackNetwork> network = simulation::straight_test_track(2000.0);
    std::unique_ptr<simulation::Train> train;

    explicit Rig(const trainplugin::DllAir mode) {
        std::vector<simulation::CarSetup> setups{{.definition = &engine}};
        for (int i = 0; i < 6; ++i) setups.push_back({.definition = &wagon});
        simulation::TrainSettings settings;
        settings.brake_model = simulation::BrakeModel::realistic;
        train = std::make_unique<simulation::Train>(setups, settings);
        std::string error;
        check(train->place(*network, 1, 1000.0, true, &error), "placed: " + error);
        outputs->valid = true;
        outputs->cars.resize(setups.size());
        auto& cab = outputs->cars[0];
        cab.has_engine = true;
        cab.loco_flags = 1; // the cab driven
        cab.switches.resize(8);
        cab.switches[2].state = 1; // the 395 in II
        cab.engine.equalising = 5.0f;
        air->mode = mode;
        train->set_controller(0, trainplugin::make_legacy_controller(outputs, 0, air));
    }
    auto& cab() { return outputs->cars[0]; }
    void run(const double seconds) {
        for (int k = 0; k < static_cast<int>(seconds / simulation::train_step); ++k) train->step();
    }
    [[nodiscard]] float least_wagon_cylinder() const {
        float least = 99.0f;
        for (std::size_t k = 1; k < train->cars().size(); ++k) least = (std::min)(least, train->cars()[k].cylinder);
        return least;
    }
    [[nodiscard]] float most_cylinder() const {
        float most = 0.0f;
        for (const auto& c : train->cars()) most = (std::max)(most, c.cylinder);
        return most;
    }
};

} // namespace

int main() {
    // Brakes.dll's parameters.
    {
        const auto engine = formats::parse_wagon(engine_text);
        check(engine.ok() && engine.engine, "the engine parses");
        if (engine.engine) {
            const auto& lib = engine.engine->brakes_library;
            check(lib.present && lib.valve == 2 && lib.equalising_gauge == 3 && std::abs(lib.charging - 5.0f) < 1e-4f,
                  "Brakes.dll's BR_395ID1, BR_MANU1, BR_ZARPRESS");
        }
    }

    // As RTS: the DLL's valve is made for RTS's triple valve.
    {
        Rig rig(trainplugin::DllAir::rts);
        check(rig.train->brake_model() == simulation::BrakeModel::rts, "rts: the train keeps RTS's air");
    }

    // The DLL's valve: the pipe follows its УР; its fast vent is an emergency.
    {
        Rig rig(trainplugin::DllAir::dll_valve);
        check(rig.train->brake_model() == simulation::BrakeModel::realistic, "dll: the realistic air runs");
        rig.run(240.0);
        check(std::abs(rig.train->cars().back().pipe - 5.0f) < 0.15f, "dll: the pipe charged to the DLL's УР (" +
                                                                         std::to_string(rig.train->cars().back().pipe) + ")");
        check(rig.most_cylinder() < 0.05f, "dll: released");
        rig.cab().engine.equalising = 4.5f; // a step
        rig.run(25.0);
        check(rig.least_wagon_cylinder() > 0.3f, "dll: a 0.5 step of its УР brakes every wagon (" +
                                                     std::to_string(rig.least_wagon_cylinder()) + ")");
        rig.cab().engine.equalising = 5.0f;
        rig.run(120.0);
        check(rig.most_cylinder() < 0.05f, "dll: released after its УР is back");
        rig.cab().engine.train_pipe_rate = -1.2f; // its VI or ЭПК
        rig.run(10.0);
        check(rig.train->cars().back().cylinder > 2.0f, "dll: its fast vent brakes the whole train in emergency");
        check(rig.train->cars().front().main_reservoir > 6.0f, "dll: the compressor keeps the main reservoir");
    }

    // openRTS's valve on Brakes.dll's handle (switch 2: states I..VI).
    {
        Rig rig(trainplugin::DllAir::own_valve);
        rig.cab().engine.equalising = 4.2f; // what the DLL keeps for itself: not used
        rig.run(240.0);
        const auto* asked = rig.train->demands_of(0);
        check(asked && std::abs(asked->equalising - 5.0f) < 0.05f, "own: II charges the УР to BR_ZARPRESS");
        check(std::abs(rig.train->cars().back().pipe - 5.0f) < 0.15f, "own: the pipe follows it");
        rig.cab().switches[2].state = 5; // V
        rig.run(3.0);
        rig.cab().switches[2].state = 3; // IV
        rig.run(25.0);
        check(rig.least_wagon_cylinder() > 0.3f, "own: V then IV brakes every wagon (" + std::to_string(rig.least_wagon_cylinder()) + ")");
        rig.cab().switches[2].state = 1; // II
        rig.run(120.0);
        check(rig.most_cylinder() < 0.05f, "own: II releases");
        rig.cab().switches[2].state = 6; // VI
        rig.run(10.0);
        check(rig.train->cars().back().cylinder > 2.0f, "own: VI brakes the whole train in emergency");
        rig.cab().switches[2].state = 1; // II: the pipe charges again and every car releases
        rig.run(240.0);
        check(rig.train->cars().back().pipe > 4.5f, "own: after VI, II charges the pipe again (" +
                                                       std::to_string(rig.train->cars().back().pipe) + ")");
        check(rig.most_cylinder() < 0.05f, "own: and every car releases (" + std::to_string(rig.most_cylinder()) + ")");
        // Its own valve venting towards its own УР below the pipe (after an
        // overcharge by openRTS's valve) is no emergency.
        rig.cab().engine.equalising = 4.2f;
        rig.cab().engine.train_pipe_rate = -0.8f;
        rig.run(10.0);
        check(rig.most_cylinder() < 0.05f, "own: the DLL's valve venting to its own УР is not an ЭПК (" +
                                               std::to_string(rig.most_cylinder()) + ")");
        // Its ЭПК: fast, its УР standing at the pipe.
        rig.cab().engine.equalising = rig.train->cars().front().pipe;
        rig.cab().engine.train_pipe_rate = -0.9f;
        rig.run(10.0);
        check(rig.train->cars().back().cylinder > 2.0f, "own: the DLL's ЭПК still brakes in emergency");
    }

    // The cabs' 395 states by their hints: the ВЛ80С's VI..I, words.
    {
        formats::CabSwitch control;
        control.num_states = 7;
        control.state_hints = {"[VI]", "[V]", "[Vа]", "[IV]", "[III]", "[II]", "[I]"};
        const auto states = trainplugin::valve_states(control);
        check(states.size() == 7 && states[0] == 6 && states[1] == 5 && states[2] == 4 && states[5] == 1 && states[6] == 0,
              "the ВЛ80С's 395: states VI..I");
        control.state_hints = {"Отпуск и зарядка", "Поездное", "Перекрыша без питания", "Перекрыша с питанием", "VЭ", "Служебное",
                               "Экстренное"};
        const auto words = trainplugin::valve_states(control);
        std::string got;
        for (const auto w : words) got += std::to_string(w) + " ";
        check(words.size() == 7 && words[0] == 0 && words[1] == 1 && words[2] == 2 && words[3] == 3 && words[4] == 4 && words[5] == 5 &&
                  words[6] == 6,
              "states by words: " + got);
        control.state_hints.clear();
        check(trainplugin::valve_states(control).size() == 7 && trainplugin::valve_states(control)[3] == 3, "no hints: I..VI");
    }

    // The ВЛ80С's cab: the 395 (VI..I), the reducer (BR_REDID1 32, "[4.0]" ..
    // "[5.9]"), the combined valve (BR_KKID1 33).
    {
        formats::CabDescription cab;
        formats::CabSwitch valve;
        valve.id = 0;
        valve.num_states = 7;
        valve.state_hints = {"[VI]", "[V]", "[Vа]", "[IV]", "[III]", "[II]", "[I]"};
        formats::CabSwitch reducer;
        reducer.id = 32;
        reducer.num_states = 20;
        for (int k = 0; k < 20; ++k) reducer.state_hints.push_back("[" + std::to_string(40 + k).insert(1, ".") + "]");
        formats::CabSwitch combined;
        combined.id = 33;
        combined.num_states = 3;
        combined.state_hints = {"[Двойная тяга]", "[Нормальное]", "[Экстренное]"};
        cab.switches = {valve, reducer, combined};
        formats::EngineDefinition::BrakesLibrary ids{.present = true, .valve = 0, .reducer = 32, .combined = 33};
        const auto controls = trainplugin::brake_cab_controls(ids, cab);
        std::vector<dllhost::SwitchState> switches(34);
        switches[0].state = 5;   // II
        switches[32].state = 12; // 5.2
        switches[33].state = 1;  // normal
        auto read = controls.read(ids, switches);
        check(read.position == 1 && std::abs(read.charging - 5.2f) < 1e-4f && read.combined == 0, "the ВЛ80С: II, the reducer at 5.2, КК normal");
        switches[32].state = 8; // 4.8
        switches[33].state = 0; // double traction
        read = controls.read(ids, switches, read);
        check(std::abs(read.charging - 4.8f) < 1e-4f && read.combined == 1, "the reducer at 4.8, КК double traction");
        switches[33].state = 2;
        check(controls.read(ids, switches, read).combined == 2, "КК emergency");
        // The 367 (BR_367ID1 23, operating in state 2, BR_367OPPOL).
        ids.block = 23;
        ids.block_operating = 2;
        switches[23].state = 0;
        check(controls.read(ids, switches).blocked, "the 367 off cuts the valve off");
        switches[23].state = 2;
        check(!controls.read(ids, switches).blocked, "the 367 in its operating state");
    }

    if (failures == 0) std::cout << "dll air: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
