// openrts-server: the openRTS server, Legacy on (docs/server.md). rts1.exe
// and openRTS clients log in to it as to rts_server.exe; its console is the
// dispatcher's (type "help").
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <openrts/core/core.hpp>
#include <openrts/core/virtual_path.hpp>
#include <openrts/server/console.hpp>
#include <openrts/server/game_server.hpp>
#include <openrts/server/server_route.hpp>
#include <openrts/server/transport.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

namespace fs = std::filesystem;
using namespace openrts;

std::atomic<bool> stop_requested = false;

void on_signal(int) { stop_requested = true; }

constexpr const char* usage =
    "usage: openrts-server --route <folder> [--root <RTS installation>] [--port 800]\n"
    "                      [--season spring|summer|autumn|winter] [--weather clear|snow|rain] [--time HH:MM]\n"
    "                      [--no-admit] [--run <seconds>] [--command \"<line>\"]...\n"
    "  --run       stop after so many seconds (tests); --command runs a console line at start\n"
    "  players get their trains from the dispatcher at the server's console\n";

#ifdef _WIN32
std::string narrow(const std::wstring_view text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}
#endif

// The command line as UTF-8 (Windows passes argv in the ANSI code page:
// Russian path names came through broken).
std::vector<std::string> arguments(int argc, char** argv) {
#ifdef _WIN32
    int count = 0;
    if (wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count)) {
        std::vector<std::string> out;
        for (int i = 1; i < count; ++i) out.push_back(narrow(wide[i]));
        LocalFree(wide);
        return out;
    }
#endif
    return {argv + 1, argv + argc};
}

// One console line as UTF-8; false at the end. A Windows console is read
// as UTF-16 (std::getline loses Russian there).
bool read_line(std::string& line) {
#ifdef _WIN32
    const HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (in != INVALID_HANDLE_VALUE && GetConsoleMode(in, &mode)) {
        std::wstring wide;
        wchar_t c = 0;
        DWORD got = 0;
        for (;;) {
            if (!ReadConsoleW(in, &c, 1, &got, nullptr) || got == 0) return !wide.empty();
            if (c == L'\n') break;
            if (c != L'\r') wide.push_back(c);
        }
        line = narrow(wide);
        return true;
    }
#endif
    return static_cast<bool>(std::getline(std::cin, line));
}

std::string stamp() {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char text[16];
    std::strftime(text, sizeof text, "%H:%M:%S", &tm);
    return text;
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    const auto args = arguments(argc, argv);
    fs::path root = fs::current_path();
    std::string route_name;
    std::uint16_t port = 800;
    std::uint8_t season = 1, weather = 0;
    double time_of_day = 12.0 * 3600.0;
    bool admit = true;
    double run_for = -1.0;
    std::vector<std::string> commands;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        const bool has_value = i + 1 < args.size();
        if (a == "--route" && has_value) {
            route_name = args[++i];
        } else if (a == "--root" && has_value) {
            const auto& text = args[++i];
            root = fs::path(std::u8string(text.begin(), text.end()));
        } else if (a == "--port" && has_value) {
            port = static_cast<std::uint16_t>(std::stoi(args[++i]));
        } else if (a == "--season" && has_value) {
            const auto s = server::parse_season(args[++i]);
            if (!s) {
                std::cerr << usage;
                return 2;
            }
            season = *s;
        } else if (a == "--weather" && has_value) {
            const auto w = server::parse_weather(args[++i]);
            if (!w) {
                std::cerr << usage;
                return 2;
            }
            weather = *w;
        } else if (a == "--time" && has_value) {
            const auto t = server::parse_time_of_day(args[++i]);
            if (!t) {
                std::cerr << usage;
                return 2;
            }
            time_of_day = *t;
        } else if (a == "--no-admit") {
            admit = false;
        } else if (a == "--run" && has_value) {
            run_for = std::stod(args[++i]);
        } else if (a == "--command" && has_value) {
            commands.push_back(args[++i]);
        } else {
            std::cerr << usage;
            return 2;
        }
    }
    if (route_name.empty()) {
        std::cerr << usage;
        return 2;
    }

    std::cout << "openRTS server " << core::version() << ": loading " << route_name << " from " << root.string() << std::endl;
    const auto loading = std::chrono::steady_clock::now();
    std::string error;
    auto route = server::ServerRoute::load(root, route_name, error);
    if (!route) {
        std::cerr << "error: " << error << std::endl;
        return 1;
    }
    for (const auto& note : route->notes()) std::cout << "note: " << note << std::endl;
    const auto& counts = route->counts();
    std::cout << "route " << route->folder() << " (RouteID " << route->route_id() << "): " << counts.nodes << " track nodes, "
              << counts.items << " track items, " << counts.substations << " substations, " << counts.sections << " electrosections; "
              << (route->signals() ? std::to_string(route->signals()->heads().size()) + " signal heads" : std::string("no signals")) << ", "
              << route->paths().size() << " paths; "
              << std::chrono::duration<double>(std::chrono::steady_clock::now() - loading).count() << " s" << std::endl;

    server::TcpServer tcp;
    if (!tcp.listen(port, error)) {
        std::cerr << "error: " << error << std::endl;
        return 1;
    }
    server::ServerOptions options;
    options.route_id = core::fold_case(route->route_id());
    options.description = server::game_description(route->route_id(), season);
    options.season = season;
    options.weather = weather;
    options.counts = counts;
    options.time_of_day = time_of_day;
    options.admit = admit;
    server::GameServer game(tcp, options, route->network(), route->signals());
    // A console's commands are the server's own, but "quit".
    game.set_console_commands([&game, &route](const std::string_view line) {
        const auto words = server::command_words(line);
        if (!words.empty() && (core::fold_case(words[0]) == "quit" || core::fold_case(words[0]) == "exit")) return std::string("not from a console");
        bool ignored = false;
        return server::run_command(game, *route, line, ignored);
    });
    game.set_console_place([&game, &route](const net::ConsolePlace& place) { return server::place_cars(game, *route, place); });
    std::cout << "listening on port " << tcp.port() << "; type \"help\" for the dispatcher's commands" << std::endl;

    std::signal(SIGINT, on_signal);
    std::mutex lines_mutex;
    std::vector<std::string> lines(commands.begin(), commands.end());
    // The console: lines typed at the server. Detached: std::getline blocks.
    std::thread([&lines_mutex, &lines] {
        std::string line;
        while (read_line(line)) {
            std::lock_guard lock(lines_mutex);
            lines.push_back(line);
        }
    }).detach();

    const auto start = std::chrono::steady_clock::now();
    bool quit = false;
    while (!quit && !stop_requested) {
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (run_for >= 0.0 && now >= run_for) break;
        for (auto& event : tcp.take_events()) {
            switch (event.kind) {
            case server::NetEvent::Kind::connected: game.connected(event.connection, std::move(event.address)); break;
            case server::NetEvent::Kind::package: game.received(event.connection, event.id, event.body); break;
            case server::NetEvent::Kind::closed: game.closed(event.connection); break;
            }
        }
        std::vector<std::string> typed;
        {
            std::lock_guard lock(lines_mutex);
            typed.swap(lines);
        }
        for (const auto& line : typed) {
            const auto answer = server::run_command(game, *route, line, quit);
            if (!answer.empty()) std::cout << answer << (answer.back() == '\n' ? "" : "\n") << std::flush;
        }
        game.tick(now);
        for (const auto& line : game.take_log()) std::cout << stamp() << ' ' << line << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::cout << "server stopped" << std::endl;
    tcp.stop();
    return 0;
}
