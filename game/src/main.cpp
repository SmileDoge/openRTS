// openrts_game: the start menu. It chooses the route, consist, path, time and
// settings and runs the game (openrts_viewer) as a process of its own.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(OPENRTS_HAS_IMGUI) && defined(OPENRTS_HAS_WGPU)
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <openrts/core/core.hpp>

#include "launcher.hpp"
#include "update.hpp"
#include "window_viewer.hpp"

namespace {

std::filesystem::path executable_folder(const char* argv0) {
#ifdef _WIN32
    wchar_t buffer[MAX_PATH * 4];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (length > 0 && length < std::size(buffer)) return std::filesystem::path(buffer).parent_path();
#endif
    std::error_code error;
    return std::filesystem::absolute(argv0, error).parent_path();
}

} // namespace

int main(int argc, char* argv[]) {
    const auto folder = executable_folder(argv[0]);
    openrts::game::Launcher menu(folder);
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    // --check-update: the latest release against this build, printed;
    // --install-latest <folder>: that release laid over <folder> the way the
    // menu's window does it (no window either way).
    if (!args.empty() && (args[0] == "--check-update" || args[0] == "--install-latest")) {
        namespace update = openrts::update;
        using namespace std::chrono_literals;
        update::Updater updater;
        updater.check(std::string(openrts::core::version()));
        while (updater.busy()) std::this_thread::sleep_for(100ms);
        auto status = updater.status();
        if (!status.release) {
            std::cout << "check failed: " << status.error << ": " << status.detail << '\n';
            return 1;
        }
        const auto& release = *status.release;
        std::cout << "latest " << release.tag << " (" << release.archive_name << ", " << release.archive_size << " bytes, sha256 "
                  << (!release.sha256.empty() ? release.sha256 : release.sha256_url.empty() ? std::string("none") : "from the .sha256 file")
                  << "); this build " << openrts::core::version()
                  << (status.stage == update::Updater::Stage::available ? ": newer, offered" : ": not newer") << '\n'
                  << "summary: " << release.summary << '\n';
        if (args[0] == "--check-update") return 0;
        if (args.size() < 2) {
            std::cout << "--install-latest <folder>\n";
            return 1;
        }
        const std::filesystem::path target(std::u8string(args[1].begin(), args[1].end()));
        const auto work = std::filesystem::temp_directory_path() / "openrts_update";
        updater.install(target, work, work / "old_files.txt");
        std::uint64_t shown = 0;
        while (updater.busy()) {
            std::this_thread::sleep_for(100ms);
            status = updater.status();
            if (status.done >= shown + (8u << 20)) {
                shown = status.done;
                std::cout << "downloaded " << status.done / 1048576 << " of " << status.total / 1048576 << " MB\n";
            }
        }
        status = updater.status();
        if (status.stage != update::Updater::Stage::installed) {
            std::cout << "update failed: " << status.error << ": " << status.detail << '\n';
            return 1;
        }
        std::cout << "installed into " << target.string() << "; old files listed in " << (work / "old_files.txt").string() << '\n';
        return 0;
    }
    // --print-command: what Start and Connect would run, without a window.
    if (!args.empty() && args[0] == "--print-command") {
        std::cout << menu.summary() << '\n' << menu.command() << '\n' << menu.network_command() << '\n';
        return 0;
    }
    openrts::viewer::WindowOptions options;
    options.menu = true;
    options.title = "openRTS " + std::string(openrts::core::version());
    options.sound = false;
    options.msaa = 1;
    // --tab single|network|settings|controls opens that tab; --capture <file> draws
    // the menu into a file after a second and exits (a smoke test, no check
    // for a new version); --no-update-check; --updated <version>: started
    // again by an update from that version.
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--no-update-check") {
            menu.set_update_check(false);
        } else if (i + 1 >= args.size()) {
            break;
        } else if (args[i] == "--tab") {
            menu.show_tab(args[++i]);
        } else if (args[i] == "--capture") {
            options.capture_path = std::string(args[++i]);
            options.capture_at = 1.0f;
            menu.set_update_check(false);
        } else if (args[i] == "--updated") {
            menu.set_updated_from(std::string(args[++i]));
        }
    }
    const int result = openrts::viewer::run_window_viewer(menu, options);
    // Updated: the new menu in this one's place.
    if (menu.wants_close())
        openrts::update::start_process(folder / "openrts_game.exe", "--updated " + std::string(openrts::core::version()));
    return result;
}

#else
#include <openrts/simulation/simulation.hpp>

int main() {
    std::cout << "openRTS game skeleton; simulation: " << openrts::simulation::module_name() << '\n';
    return 0;
}
#endif
