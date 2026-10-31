// openrts-train-check <installation root> [--run]
//
// Reads every consist of TRAINS/CONSISTS_RTS and TRAINS/CONSISTS (as RTS
// does), resolves and parses every engine and wagon they use, and reports
// failures. With --run every consist that loads also drives on a straight
// test track (full throttle, then full brake) to check the simulation.
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <openrts/core/installation.hpp>
#include <openrts/core/vfs.hpp>
#include <openrts/formats/rolling_stock.hpp>
#include <openrts/simulation/train.hpp>

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

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: openrts-train-check <installation root> [--run]\n");
        return 2;
    }
    const bool run = argc > 2 && std::string(argv[2]) == "--run";
    const std::filesystem::path root = argv[1];
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
