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
int brake_test(const core::Vfs& vfs, const std::vector<core::VirtualPath>& consists, const double speed_kmh,
               std::vector<std::string> names) {
    // --realistic among the names: openRTS realistic air instead of RTS's model.
    const auto realistic_flag = std::find(names.begin(), names.end(), "--realistic");
    const bool realistic = realistic_flag != names.end();
    if (realistic) names.erase(realistic_flag);
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
        if (tail_brakes < 0.0 && train.cars().back().cylinder > 0.1f) tail_brakes = time - brake_at;
    }
    const double distance = std::abs(train.front_end()->offset() - from);
    std::printf("emergency at %.1f km/h: stopped after %.0f m in %.0f s\n", speed_at_brake * 3.6, distance, time - brake_at);
    std::printf("  brakes: the first car after %.1f s, the last after %.1f s (cylinder above 0.1 atm)\n", head_brakes, tail_brakes);
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
