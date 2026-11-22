// openrts_game: the game's one window. The menu, the route setup and the
// simulation itself live in this process; openrts_viewer stays the headless
// capture and CI viewer.

#include <chrono>
#include <cstdint>
#include <cstdlib>
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
#include <openrts/ui/fonts.hpp>

#include "log_console.hpp"
#include "shell.hpp"
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
    // The console's lines (F2) from the first one on.
    openrts::viewer::start_log_capture();
    const auto folder = executable_folder(argv[0]);
    // The menu's own faces (Montserrat and Onest) live beside the program.
    openrts::ui::set_font_folder(folder / "menu");
    openrts::game::GameShell shell(folder);
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    // --print-command: what Start and Connect would run, without a window.
    if (!args.empty() && args[0] == "--print-command") {
        std::cout << shell.summary() << '\n' << shell.command() << '\n' << shell.network_command() << '\n';
        return 0;
    }
    openrts::viewer::WindowOptions options;
    options.menu = true;
    options.title = "DTRE " + openrts::core::version_text();
    options.sound = true;
    options.msaa = 1;
    std::string play_route;
    std::string play_consist;
    bool pause_after_load = false;
    // --tab single|network|settings|controls opens that tab; --capture <file>
    // draws the menu into a file after a second and exits.
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--pause") {
            pause_after_load = true;
        } else if (i + 1 >= args.size()) {
            break;
        } else if (args[i] == "--tab") {
            shell.show_tab(args[++i]);
        } else if (args[i] == "--capture") {
            options.capture_path = std::string(args[++i]);
            options.capture_at = 1.0f;
            // The menu is the interface here: the capture shows it, not the
            // empty scene behind it.
            options.capture_ui = true;
        } else if (args[i] == "--capture-at") {
            options.capture_at = std::strtof(std::string(args[++i]).c_str(), nullptr);
        } else if (args[i] == "--play") {
            play_route = std::string(args[++i]);
        } else if (args[i] == "--consist") {
            play_consist = std::string(args[++i]);
        }
    }
    if (!play_route.empty() && !shell.play(play_route, play_consist)) {
        std::cerr << "--play: no game folder is set (the menu's Settings, Game folder)\n";
        return 1;
    }
    if (pause_after_load) shell.pause_when_loaded();
    shell.apply_window_options(options);
    // A capture is a check: one sample, as before.
    if (!options.capture_path.empty()) options.msaa = 1;
    const int result = openrts::viewer::run_window_viewer(shell, options);
    return result;
}

#else
#include <openrts/simulation/simulation.hpp>

int main() {
    std::cout << "openRTS game skeleton; simulation: " << openrts::simulation::module_name() << '\n';
    return 0;
}
#endif
