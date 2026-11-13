// openrts-train-check <installation root> [--run]
//
// Reads every consist of TRAINS/CONSISTS_RTS and TRAINS/CONSISTS (as RTS
// does), resolves and parses every engine and wagon they use, and reports
// failures. With --run every consist that loads also drives on a straight
// test track (full throttle, then full brake) to check the simulation.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#include <openrts/core/installation.hpp>
#include <openrts/core/vfs.hpp>
#include <openrts/formats/ace_file.hpp>
#include <openrts/formats/rolling_stock.hpp>
#include <openrts/simulation/adhesion.hpp>
#include <openrts/simulation/brake_shoes.hpp>
#include <openrts/simulation/train.hpp>

namespace openrts {
int dll_check(const core::Vfs& vfs, const std::filesystem::path& root, const std::vector<std::string>& args);
int cab_check(const core::Vfs& vfs, const std::vector<std::string>& args);
int shape_check(const core::Vfs& vfs, const std::vector<std::string>& args);
int prim_flags_check(const core::Vfs& vfs, const std::vector<std::string>& args);
}

namespace {

using namespace openrts;

void walk(const core::Vfs& vfs, const core::VirtualPath& directory,
          const std::function<void(const core::VirtualPath&)>& visit) {
    for (const auto& entry : vfs.list(directory)) {
        if (entry.directory) {
            walk(vfs, entry.path, visit);
        } else {
            visit(entry.path);
        }
    }
}

} // namespace

// --brake: one train made of the named consists (stems, searched in every
// subfolder), accelerated with full throttle to `speed_kmh` on a straight
// level track, then the brake valve to emergency (VI). Prints the stopping
// distance and time, how long the brakes take to reach the last car, the
// largest coupler tension and compression, breaks and derailments.
// --brake-ratio <folder> ...: every .eng in the TRAINSET folders, read as
// RTS reads it (with its .rtseng): its brake force at the full cylinder
// against its weight (at 40 km/h, the content's), what its shoes make of it
// at a stop in an emergency (3.8 atm), and whether that outgrows the
// adhesion of dry (0.33) and wet (0.215) rails, as is and with the
// limit to real stock's (docs/train.md). A force marked * is the engine's
// ORTSBrakeForce, which the limit leaves as it is.
int brake_ratio(const core::Vfs& vfs, const std::vector<std::string>& folders) {
    std::printf("%-34s %6s %8s %6s %-9s %6s %6s  %-15s %s\n", "engine", "t", "F kN", "F/W", "shoes", "stop", "limit", "as is: dry/wet",
                "limited: dry/wet");
    for (const auto& folder : folders) {
        const auto dir = core::VirtualPath::parse("TRAINS/TRAINSET/" + folder);
        if (!dir) continue;
        for (const auto& entry : vfs.list(*dir)) {
            const auto name = std::string(entry.path.filename());
            if (entry.directory || name.size() < 5 || core::fold_case(name.substr(name.size() - 4)) != ".eng") continue;
            const auto stem = name.substr(0, name.size() - 4);
            const auto wagon = formats::load_wagon(vfs, folder, stem, true);
            if (!wagon.ok() || wagon.mass <= 0.0f) continue;
            const auto& brakes = wagon.brakes;
            const float weight = wagon.mass * 10.0f;
            // ORTSBrakeForce, when given, is the realistic air's (marked *).
            const bool own = brakes.realistic_brake_force > 0.0f;
            const float force = own ? brakes.realistic_brake_force : brakes.max_brake_force;
            const float ratio = force / weight;
            const auto shoe = simulation::shoe_of(wagon);
            const float share = brakes.cylinder_pressure_for_max_force > 0.0f ? (std::min)(3.8f / brakes.cylinder_pressure_for_max_force, 1.5f) : 1.0f;
            const float stop = ratio * share * simulation::shoe_factor(shoe, 0.0f, share);
            const float limited = (own ? ratio : (std::min)(ratio, simulation::shoe_force_limit(shoe))) * share * simulation::shoe_factor(shoe, 0.0f, share);
            const float dry = simulation::wheel_rail_adhesion(0.0f);
            const float wet = dry * simulation::rail_factor(simulation::RailCondition::wet);
            const auto verdict = [&](const float at_stop) {
                return std::string(at_stop > dry ? "slides" : "holds") + "/" + (at_stop > wet ? "slides" : "holds");
            };
            std::printf("%-34s %6.1f %7.1f%s %6.3f %-9s %6.3f %6.3f  %-15s %s\n", (folder + "/" + stem).c_str(), wagon.mass / 1000.0f,
                        force / 1000.0f, own ? "*" : " ", ratio, formats::brake_shoe_name(shoe), stop, limited, verdict(stop).c_str(),
                        verdict(limited).c_str());
        }
    }
    return 0;
}

// --charge <wagons> <folder/eng>[!] ... <folder/wag> [--realistic]: the
// engines (the first is the master, keyboard driven, no DLL) and `wagons`
// of the wagon, standing; how long the air takes (docs/train.md): from
// empty, and released after VI with the pipe empty. The cars' times: the
// head and tail pipe to 4.5 and 4.8, the tail's auxiliary reservoir to 4.5,
// every cylinder below 0.4 (released).
int charge_test(const core::Vfs& vfs, std::vector<std::string> args) {
    const auto realistic_flag = std::find(args.begin(), args.end(), "--realistic");
    const bool realistic = realistic_flag != args.end();
    if (realistic) args.erase(realistic_flag);
    if (args.size() < 3) {
        std::fprintf(stderr, "--charge <wagons> <folder/eng>[!] ... <folder/wag> [--realistic]\n");
        return 2;
    }
    const auto count = static_cast<std::size_t>(std::atoi(args[0].c_str()));
    std::vector<std::unique_ptr<formats::WagonDefinition>> stock;
    const auto load = [&](std::string spec, const bool engine) -> const formats::WagonDefinition* {
        bool flipped = false;
        if (!spec.empty() && spec.back() == '!') {
            flipped = true;
            spec.pop_back();
        }
        (void)flipped;
        const auto slash = spec.find('/');
        if (slash == std::string::npos) return nullptr;
        auto wagon = std::make_unique<formats::WagonDefinition>(formats::load_wagon(vfs, spec.substr(0, slash), spec.substr(slash + 1), engine));
        if (!wagon->ok()) {
            std::fprintf(stderr, "%s: %s\n", spec.c_str(), wagon->error.c_str());
            return nullptr;
        }
        stock.push_back(std::move(wagon));
        return stock.back().get();
    };
    std::vector<simulation::CarSetup> cars;
    for (std::size_t k = 1; k + 1 < args.size(); ++k) {
        const auto* engine = load(args[k], true);
        if (!engine) return 2;
        cars.push_back({.definition = engine, .flipped = !args[k].empty() && args[k].back() == '!'});
    }
    const auto* wagon = load(args.back(), false);
    if (!wagon) return 2;
    for (std::size_t k = 0; k < count; ++k) cars.push_back({.definition = wagon});
    double length = 0.0, mass = 0.0;
    for (const auto& car : cars) {
        length += car.definition->length;
        mass += car.definition->mass;
    }
    auto network = simulation::straight_test_track(length + 2000.0);
    simulation::Train train(cars);
    if (realistic) train.set_brake_model(simulation::BrakeModel::realistic);
    std::string error;
    if (!network || !train.place(*network, 1, length + 100.0, true, &error)) {
        std::fprintf(stderr, "cannot place the train: %s\n", error.c_str());
        return 2;
    }
    const auto driver = simulation::attach_keyboard_driver(train);
    if (!driver) {
        std::fprintf(stderr, "no engine to drive\n");
        return 2;
    }
    std::printf("train: %zu cars (%zu wagons), %.0f t, %.0f m; %s air\n", cars.size(), count, mass / 1000.0, train.length(),
                realistic ? "realistic" : "RTS");
    const auto n = train.cars().size();
    // The run: until every figure came or 30 min; a line every 30 s.
    const auto run = [&](const char* what) {
        double time = 0.0, head45 = -1, tail45 = -1, tail48 = -1, zr45 = -1, released = -1;
        float least_mr = 99.0f;
        std::printf("%s\n     s  head pipe  tail pipe  tail aux  most cyl   MR\n", what);
        while (time < 1800.0) {
            train.step();
            time += simulation::train_step;
            const auto& c = train.cars();
            float most = 0.0f;
            for (const auto& car : c) most = (std::max)(most, (std::max)(car.cylinder, car.independent));
            least_mr = (std::min)(least_mr, c.front().main_reservoir);
            if (head45 < 0 && c.front().pipe >= 4.5f) head45 = time;
            if (tail45 < 0 && c.back().pipe >= 4.5f) tail45 = time;
            if (tail48 < 0 && c.back().pipe >= 4.8f) tail48 = time;
            if (zr45 < 0 && c.back().auxiliary >= 4.5f) zr45 = time;
            if (released < 0 && most < 0.4f) released = time;
            if (std::fmod(time + 1e-6, 30.0) < simulation::train_step) {
                std::printf("  %4.0f  %9.2f  %9.2f  %8.2f  %8.2f  %4.2f\n", time, c.front().pipe, c.back().pipe, c.back().auxiliary, most,
                            c.front().main_reservoir);
            }
            if (head45 >= 0 && tail48 >= 0 && zr45 >= 0 && released >= 0) break;
        }
        const auto when = [](const double t) { return t < 0 ? std::string("not in 30 min") : std::to_string(static_cast<int>(t + 0.5)) + " s"; };
        std::printf("  head pipe 4.5: %s; tail pipe 4.5: %s, 4.8: %s; tail aux 4.5: %s; released (every cylinder < 0.4): %s; MR at least %.2f\n",
                    when(head45).c_str(), when(tail45).c_str(), when(tail48).c_str(), when(zr45).c_str(), when(released).c_str(),
                    least_mr);
    };
    // From empty: every car's pipe, reservoirs and cylinder at 0.
    for (std::size_t k = 0; k < n; ++k) {
        const auto& c = train.cars()[k];
        train.set_air(k, {.pipe = 0.0f, .charging_pipe = 0.0f, .cylinder = 0.0f, .auxiliary = 0.0f, .control = 0.0f,
                          .main_reservoir = c.main_reservoir});
    }
    driver->brake = simulation::DriverHandles::BrakeHandle::running;
    run("from empty, the valve in II:");
    // Charged for 10 min more, then VI for 40 s, then II.
    for (int k = 0; k < static_cast<int>(600.0 / simulation::train_step); ++k) train.step();
    driver->brake = simulation::DriverHandles::BrakeHandle::emergency;
    for (int k = 0; k < static_cast<int>(40.0 / simulation::train_step); ++k) train.step();
    std::printf("after 40 s of VI: head pipe %.2f, tail pipe %.2f, tail aux %.2f, tail cylinder %.2f\n", train.cars().front().pipe,
                train.cars().back().pipe, train.cars().back().auxiliary, train.cars().back().cylinder);
    driver->brake = simulation::DriverHandles::BrakeHandle::running;
    run("released after VI, the valve in II:");
    return 0;
}

int brake_test(const core::Vfs& vfs, const std::vector<core::VirtualPath>& consists, const double speed_kmh,
               std::vector<std::string> names) {
    // --realistic among the names: openRTS realistic air instead of RTS's model.
    const auto realistic_flag = std::find(names.begin(), names.end(), "--realistic");
    const bool realistic = realistic_flag != names.end();
    if (realistic) names.erase(realistic_flag);
    // --limit-brakes: TrainSettings::limit_brake_force.
    const auto limit_flag = std::find(names.begin(), names.end(), "--limit-brakes");
    const bool limit_brakes = limit_flag != names.end();
    if (limit_brakes) names.erase(limit_flag);
    // --rails dry|wet|snow|ice|leaves: the rails' state (adhesion.hpp); RTS's by default.
    auto rails = simulation::RailCondition::rts;
    if (const auto flag = std::find(names.begin(), names.end(), "--rails"); flag != names.end() && flag + 1 != names.end()) {
        const auto value = *(flag + 1);
        rails = value == "dry"    ? simulation::RailCondition::dry
              : value == "wet"    ? simulation::RailCondition::wet
              : value == "snow"   ? simulation::RailCondition::snow
              : value == "ice"    ? simulation::RailCondition::ice
              : value == "leaves" ? simulation::RailCondition::leaves
                                  : simulation::RailCondition::rts;
        names.erase(flag, flag + 2);
    }
    std::vector<std::unique_ptr<formats::WagonDefinition>> stock;
    std::vector<simulation::CarSetup> cars;
    for (const auto& name : names) {
        const core::VirtualPath* found = nullptr;
        for (const auto& path : consists) {
            // The file name without ".con".
            auto stem = std::string(path.filename());
            if (stem.size() > 4) stem.resize(stem.size() - 4);
            if (core::fold_case(stem) == core::fold_case(name)) found = &path;
        }
        if (!found) {
            std::fprintf(stderr, "consist %s not found\n", name.c_str());
            return 2;
        }
        std::string error;
        const auto bytes = vfs.read(*found, &error);
        const auto consist = bytes ? formats::parse_consist(*bytes) : formats::ConsistDefinition{.error = error};
        if (!consist.ok()) {
            std::fprintf(stderr, "%s: %s\n", name.c_str(), consist.error.c_str());
            return 2;
        }
        for (const auto& entry : consist.entries) {
            auto wagon = std::make_unique<formats::WagonDefinition>(formats::load_wagon(vfs, entry.folder, entry.name, entry.engine));
            if (!wagon->ok()) {
                std::fprintf(stderr, "%s/%s: %s\n", entry.folder.c_str(), entry.name.c_str(), wagon->error.c_str());
                return 2;
            }
            cars.push_back({.definition = wagon.get(), .flipped = entry.flipped});
            stock.push_back(std::move(wagon));
        }
    }
    double length = 0.0, mass = 0.0;
    for (const auto& car : cars) {
        length += car.definition->length;
        mass += car.definition->mass;
    }
    auto network = simulation::straight_test_track(length + 80000.0);
    simulation::Train train(cars);
    if (realistic) train.set_brake_model(simulation::BrakeModel::realistic);
    train.set_rails(rails);
    train.set_limit_brake_force(limit_brakes);
    std::string error;
    if (!network || !train.place(*network, 1, length + 100.0, true, &error)) {
        std::fprintf(stderr, "cannot place the train: %s\n", error.c_str());
        return 2;
    }
    const auto driver = simulation::attach_keyboard_driver(train);
    if (!driver) {
        std::fprintf(stderr, "no engine to drive\n");
        return 2;
    }
    std::printf("train: %zu cars, %.0f t, %.0f m (%s)\n", cars.size(), mass / 1000.0, train.length(), names.front().c_str());

    struct Extreme {
        double force = 0.0;
        std::size_t coupler = 0;
        double time = 0.0;
    };
    Extreme tension, compression;
    std::size_t breaks = 0, derailments = 0;
    std::string first_derailment;
    double time = 0.0;
    const auto watch = [&] {
        const auto& states = train.cars();
        for (std::size_t j = 0; j + 1 < states.size(); ++j) {
            const double force = states[j].coupler_force;
            if (force > tension.force) tension = {force, j + 1, time};
            if (-force > compression.force) compression = {-force, j + 1, time};
        }
        for (const auto& event : train.take_events()) {
            if (event.kind == simulation::TrainEventKind::coupler_break) ++breaks;
            if (event.kind == simulation::TrainEventKind::derailment && derailments++ == 0) {
                char text[64];
                std::snprintf(text, sizeof text, "car %u at %.1f s", event.car + 1, time);
                first_derailment = text;
            }
        }
    };

    // Accelerate: full throttle until the speed is reached.
    driver->reverser = 1;
    driver->throttle = driver->notches;
    const double target = speed_kmh / 3.6;
    while (train.speed() < target && time < 1800.0) {
        train.step();
        time += simulation::train_step;
        watch();
    }
    if (train.speed() < target) {
        std::printf("did not reach %.0f km/h in 30 min (top %.1f km/h)\n", speed_kmh, train.speed() * 3.6);
        return 1;
    }
    std::printf("start: %.0f km/h after %.0f s; on the way the largest tension %.0f kN (coupler %zu), compression %.0f kN (coupler %zu)\n",
                train.speed() * 3.6, time, tension.force / 1000.0, tension.coupler, compression.force / 1000.0, compression.coupler);

    // Emergency.
    tension = compression = {};
    breaks = derailments = 0;
    first_derailment.clear();
    driver->throttle = 0;
    driver->brake = simulation::KeyboardController::BrakeHandle::emergency;
    const double brake_at = time;
    const double from = train.front_end()->offset();
    const double speed_at_brake = train.speed();
    double head_brakes = -1.0, tail_brakes = -1.0;
    std::string profile;
    double next_mark = 0.0;
    const auto fastest = [&] {
        double fastest_speed = 0.0;
        for (const auto& car : train.cars()) fastest_speed = (std::max)(fastest_speed, std::abs(static_cast<double>(car.velocity)));
        return fastest_speed;
    };
    while (fastest() > 0.01 && time - brake_at < 900.0) {
        const double since = time - brake_at;
        if (since >= next_mark) {
            char mark[48];
            std::snprintf(mark, sizeof mark, "%s%.0f s %.0f km/h", profile.empty() ? "" : ", ", since, train.speed() * 3.6);
            profile += mark;
            next_mark += 10.0;
        }
        train.step();
        time += simulation::train_step;
        watch();
        if (head_brakes < 0.0 && train.cars().front().cylinder > 0.1f) head_brakes = time - brake_at;
        // Where the first car's wheels start sliding (the realistic air).
        if (static bool told = false; !told && train.cars().front().sliding()) {
            told = true;
            const auto& c = train.cars().front();
            std::printf("  car 1 slides from %.1f km/h: cylinder %.2f, independent %.2f, brake %.0f kN, shoes wet %.2f\n",
                        std::abs(c.velocity) * 3.6, c.cylinder, c.independent, c.brake_force / 1000.0, c.shoe_state[0].wet);
        }
        if (tail_brakes < 0.0 && train.cars().back().cylinder > 0.1f) tail_brakes = time - brake_at;
    }
    const double distance = std::abs(train.front_end()->offset() - from);
    std::printf("emergency at %.1f km/h: stopped after %.0f m in %.0f s\n", speed_at_brake * 3.6, distance, time - brake_at);
    std::printf("  brakes: the first car after %.1f s, the last after %.1f s (cylinder above 0.1 atm)\n", head_brakes, tail_brakes);
    {
        // Wheel slide (the realistic air): the cars that slid and how far.
        std::size_t slid = 0;
        float most = 0.0f;
        for (const auto& car : train.cars()) {
            if (car.slid > 0.0f) ++slid;
            most = (std::max)(most, car.slid);
        }
        std::printf("  wheel slide: %zu cars slid, the most %.0f m\n", slid, most);
        for (std::size_t k = 0; k < train.cars().size(); ++k) {
            const auto& car = train.cars()[k];
            if (car.slid > 0.0f) std::printf("    car %zu (%s): %.0f m\n", k + 1, car.definition->name.c_str(), car.slid);
        }
    }
    std::printf("  couplers: the largest compression %.0f kN (coupler %zu, %.1f s), tension %.0f kN (coupler %zu, %.1f s)\n",
                compression.force / 1000.0, compression.coupler, compression.time - brake_at, tension.force / 1000.0, tension.coupler,
                tension.time - brake_at);
    std::printf("  coupler breaks %zu, derailments %zu%s%s\n", breaks, derailments, first_derailment.empty() ? "" : ", first: ",
                first_derailment.c_str());
    std::printf("  speed: %s\n", profile.c_str());

    // Release: the valve to II (running), as after a stop; how long the
    // pipe, the cylinders and the auxiliary reservoirs take.
    driver->brake = simulation::KeyboardController::BrakeHandle::running;
    const double release_at = time;
    struct Mark {
        const char* what;
        std::function<bool()> reached;
        double at = -1.0;
    };
    const auto& states = train.cars();
    const std::size_t middle = states.size() / 2;
    std::vector<Mark> marks{
        {"head pipe 4.8 atm", [&] { return train.cars().front().pipe >= 4.8f; }},
        {"middle pipe 4.5 atm", [&] { return train.cars()[middle].pipe >= 4.5f; }},
        {"tail pipe 4.5 atm", [&] { return train.cars().back().pipe >= 4.5f; }},
        {"tail pipe 4.9 atm", [&] { return train.cars().back().pipe >= 4.9f; }},
        {"head cylinder below 0.4 atm", [&] { return train.cars()[2].cylinder < 0.4f; }},
        {"tail cylinder below 0.4 atm", [&] { return train.cars().back().cylinder < 0.4f; }},
        {"tail auxiliary 4.8 atm", [&] { return train.cars().back().auxiliary >= 4.8f; }},
    };
    float lowest_main = train.cars().front().main_reservoir;
    const float start_pipe = states.front().pipe;
    double next_row = 0.0;
    std::printf("release (II): t, pipe head / middle / tail, tail cylinder, tail auxiliary, main reservoir, equalising (atm)\n");
    while (time - release_at < 900.0) {
        if (time - release_at >= next_row && next_row <= 300.0) {
            const auto& s = train.cars();
            std::printf("  %4.0f s  pipe %.2f / %.2f / %.2f  cyl %.2f  aux %.2f  main %.2f  eq %.2f\n", time - release_at, s.front().pipe,
                        s[middle].pipe, s.back().pipe, s.back().cylinder, s.back().auxiliary, s.front().main_reservoir,
                        driver->equalising);
            next_row += next_row < 60.0 ? 10.0 : 60.0;
        }
        train.step();
        time += simulation::train_step;
        lowest_main = (std::min)(lowest_main, train.cars().front().main_reservoir);
        bool all = true;
        for (auto& mark : marks) {
            if (mark.at < 0.0 && mark.reached()) mark.at = time - release_at;
            all = all && mark.at >= 0.0;
        }
        if (all) break;
    }
    std::printf("release (II) after the stop, the pipe from %.2f atm at the head:\n", start_pipe);
    for (const auto& mark : marks) {
        if (mark.at >= 0.0) std::printf("  %-28s after %5.1f s\n", mark.what, mark.at);
        else std::printf("  %-28s not within 900 s\n", mark.what);
    }
    std::printf("  main reservoir at its lowest %.2f atm; equalising reservoir now %.2f atm\n", lowest_main,
                driver->equalising);
    return 0;
}

// The command line as UTF-8 (Windows hands main() the ANSI code page, which
// loses consist names such as "ВЛ85-001").
std::vector<std::string> utf8_arguments(int argc, char** argv) {
#ifdef _WIN32
    int count = 0;
    if (wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count)) {
        std::vector<std::string> out;
        for (int i = 0; i < count; ++i) {
            const int n = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
            std::string a(static_cast<std::size_t>((std::max)(n - 1, 0)), '\0');
            if (n > 1) WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, a.data(), n, nullptr, nullptr);
            out.push_back(std::move(a));
        }
        LocalFree(wide);
        return out;
    }
#endif
    return std::vector<std::string>(argv, argv + argc);
}

int main(int argc, char** argv) {
    const auto args = utf8_arguments(argc, argv);
    std::vector<char*> pointers;
    for (const auto& a : args) pointers.push_back(const_cast<char*>(a.c_str()));
    argc = static_cast<int>(pointers.size());
    argv = pointers.data();
    if (argc < 2) {
        std::fprintf(stderr, "usage: openrts-train-check <installation root> [--run]\n"
                             "       openrts-train-check <installation root> --brake <km/h> <consist> [consist...]\n"
                             "       openrts-train-check <installation root> --dll <TRAINSET folder> <eng|wag>[!]... [--script <file>] [--seconds <s>]\n");
        return 2;
    }
    const bool run = argc > 2 && std::string(argv[2]) == "--run";
    const std::filesystem::path root = std::filesystem::path(std::u8string(args[1].begin(), args[1].end()));
    core::Vfs vfs;
    core::mount_installation(vfs, root, core::load_installation_config(root));

    std::vector<core::VirtualPath> consists;
    for (const char* top : {"TRAINS/CONSISTS_RTS", "TRAINS/CONSISTS"}) {
        if (const auto directory = core::VirtualPath::parse(top)) {
            walk(vfs, *directory, [&](const core::VirtualPath& path) {
                if (core::fold_case(path.extension()) == ".con") consists.push_back(path);
            });
        }
    }
    if (argc > 4 && std::string(argv[2]) == "--ace-raw") {
        // A texture as raw RGBA8 (for viewing): --ace-raw <virtual path> <out file>.
        const auto path = core::VirtualPath::parse(argv[3]);
        const auto decoded = path ? formats::decode_ace_rgba(vfs, *path) : formats::AceDecodeResult{.error = "bad path"};
        if (!decoded.ok()) {
            std::fprintf(stderr, "%s: %s\n", argv[3], decoded.error.c_str());
            return 1;
        }
        std::FILE* out = nullptr;
        if (fopen_s(&out, argv[4], "wb") == 0 && out) {
            std::fwrite(decoded.image->pixels.data(), 1, decoded.image->pixels.size(), out);
            std::fclose(out);
        }
        std::printf("%u %u\n", decoded.image->width, decoded.image->height);
        return 0;
    }
    if (argc > 2 && std::string(argv[2]) == "--prim-flags") {
        return prim_flags_check(vfs, std::vector<std::string>(argv + 3, argv + argc));
    }
    if (argc > 2 && std::string(argv[2]) == "--shape") {
        return shape_check(vfs, std::vector<std::string>(argv + 3, argv + argc));
    }
    if (argc > 2 && std::string(argv[2]) == "--cab") {
        return cab_check(vfs, std::vector<std::string>(argv + 3, argv + argc));
    }
    if (argc > 2 && std::string(argv[2]) == "--dll") {
        return dll_check(vfs, root, std::vector<std::string>(argv + 3, argv + argc));
    }
    if (argc > 3 && std::string(argv[2]) == "--brake-ratio") {
        return brake_ratio(vfs, std::vector<std::string>(argv + 3, argv + argc));
    }
    if (argc > 4 && std::string(argv[2]) == "--charge") {
        return charge_test(vfs, std::vector<std::string>(argv + 3, argv + argc));
    }
    if (argc > 4 && std::string(argv[2]) == "--brake") {
        return brake_test(vfs, consists, std::atof(argv[3]), std::vector<std::string>(argv + 4, argv + argc));
    }

    std::size_t consist_failed = 0;
    std::size_t consists_complete = 0;
    std::map<std::string, formats::WagonDefinition> stock; // by path key
    std::map<std::string, std::size_t> missing;             // file -> uses
    std::map<std::string, std::string> broken;              // file -> error
    std::vector<std::string> consist_errors;
    std::vector<std::pair<std::string, formats::ConsistDefinition>> loaded;
    for (const auto& path : consists) {
        std::string error;
        const auto bytes = vfs.read(path, &error);
        auto consist = bytes ? formats::parse_consist(*bytes) : formats::ConsistDefinition{.error = error};
        if (!consist.ok()) {
            ++consist_failed;
            if (consist_errors.size() < 10) consist_errors.push_back(path.str() + ": " + consist.error);
            continue;
        }
        bool complete = true;
        for (const auto& entry : consist.entries) {
            const auto file = formats::wagon_path(entry.folder, entry.name, entry.engine);
            if (!vfs.exists(file)) {
                ++missing[file.str()];
                complete = false;
                continue;
            }
            auto [it, inserted] = stock.try_emplace(file.key());
            if (inserted) it->second = formats::load_wagon(vfs, entry.folder, entry.name, entry.engine);
            if (!it->second.ok()) {
                broken[file.str()] = it->second.error;
                complete = false;
            }
        }
        if (complete) {
            ++consists_complete;
            loaded.emplace_back(path.str(), std::move(consist));
        }
    }

    std::size_t engines = 0;
    std::size_t with_dll = 0;
    std::size_t no_shape = 0;
    for (const auto& [_, wagon] : stock) {
        if (wagon.engine) {
            ++engines;
            with_dll += wagon.engine->library_module.empty() ? 0 : 1;
        }
        no_shape += wagon.shape.empty() ? 1 : 0;
    }
    std::printf("consists: %zu read, %zu failed, %zu with all stock loadable\n", consists.size() - consist_failed,
                consist_failed, consists_complete);
    std::printf("stock: %zu files (%zu engines, %zu with a DLL), %zu broken, %zu missing files, %zu without shape\n",
                stock.size(), engines, with_dll, broken.size(), missing.size(), no_shape);
    std::size_t shown = 0;
    for (const auto& [file, error] : broken) {
        if (shown++ >= 12) break;
        std::printf("  BROKEN %s: %s\n", file.c_str(), error.c_str());
    }
    shown = 0;
    for (const auto& [file, uses] : missing) {
        if (shown++ >= 8) break;
        std::printf("  missing %s (%zu uses)\n", file.c_str(), uses);
    }
    for (const auto& line : consist_errors) std::printf("  FAIL %s\n", line.c_str());

    int status = consist_failed > 0 ? 1 : 0;
    if (run) {
        std::size_t ran = 0;
        std::size_t bad = 0;
        double worst_stop = 0.0;
        for (const auto& [path, consist] : loaded) {
            std::vector<simulation::CarSetup> cars;
            for (const auto& entry : consist.entries) {
                const auto& wagon = stock.at(formats::wagon_path(entry.folder, entry.name, entry.engine).key());
                cars.push_back({.definition = &wagon, .flipped = entry.flipped});
            }
            const auto result = simulation::test_run(cars);
            ++ran;
            if (!result.ok) {
                if (++bad <= 10) std::printf("  RUN %s: %s\n", path.c_str(), result.message.c_str());
                continue;
            }
            worst_stop = (std::max)(worst_stop, result.stopping_time);
        }
        std::printf("runs: %zu consists, %zu failed, longest stop %.0f s\n", ran, bad, worst_stop);
        if (bad > 0) status = 1;
    }
    return status;
}
