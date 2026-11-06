// openrts_game: the start menu. It chooses the route, consist, path, time and
// settings and runs the game (openrts_viewer) as a process of its own.

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
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

#include "launcher.hpp"
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
    openrts::game::Launcher menu(executable_folder(argv[0]));
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    // --print-command: what Start and Connect would run, without a window.
    if (!args.empty() && args[0] == "--print-command") {
        std::cout << menu.summary() << '\n' << menu.command() << '\n' << menu.network_command() << '\n';
        return 0;
    }
    openrts::viewer::WindowOptions options;
    options.menu = true;
    options.title = "openRTS";
    options.sound = false;
    options.msaa = 1;
    // --tab single|network|settings opens that tab; --capture <file> draws
    // the menu into a file after a second and exits (a smoke test).
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == "--tab") {
            menu.show_tab(args[++i]);
        } else if (args[i] == "--capture") {
            options.capture_path = std::string(args[++i]);
            options.capture_at = 1.0f;
        }
    }
    return openrts::viewer::run_window_viewer(menu, options);
}

#else
#include <openrts/simulation/simulation.hpp>

int main() {
    std::cout << "openRTS game skeleton; simulation: " << openrts::simulation::module_name() << '\n';
    return 0;
}
#endif
