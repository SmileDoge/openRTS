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

#include <openrts/core/user_data.hpp>

#include <openrts/core/core.hpp>
#include <openrts/core/virtual_path.hpp>
#include <openrts/server/console.hpp>
#include <openrts/server/game_server.hpp>
#include <openrts/server/server_route.hpp>
#include <openrts/server/session.hpp>
#include <openrts/server/transport.hpp>
#ifdef OPENRTS_SERVER_WINDOW
#include "server_window.hpp"
#endif

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
    "usage: openrts-server [--route <folder>] [--root <RTS installation>] [--port 800]\n"
    "                      [--season spring|summer|autumn|winter] [--weather clear|snow|rain|storm] [--time HH:MM]\n"
    "                      [--no-admit] [--data <folder>] [--dispatcher-password <text>]\n"
    "                      [--discord-join] [--public-address <host:port>] [--max-players <n>] [--autosave <minutes>]\n"
    "                      [--run <seconds>] [--command \"<line>\"]... [--no-window]\n"
    "  without --no-window (or --run) the server's window opens: its start form, then its map console;\n"
    "  --no-window runs it at the console (--route needed)\n"
    "  --data      where the server keeps its files (openRTS's user data folder by default)\n"
    "  --dispatcher-password  the dispatcher's console from another computer logs in with it\n"
    "              (none: only from this one)\n"
    "  --discord-join  openRTS players may be joined from their Discord profiles (\"Join\"),\n"
    "              at --public-address (else the address each one connected to)\n"
    "  --max-players  places: a new player beyond so many is refused (0, as by default: no limit)\n"
    "  --autosave  every train saved as the placement autosave so often (2 minutes by default, 0 never)\n"
    "              and at the stop; the last run's kept as autosave-previous\n"
    "  --run      stop after so many seconds (tests); --command runs a console line at start\n"
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
    fs::path data;
    std::string dispatcher_password;
    bool root_given = false, port_given = false, password_given = false;
    bool discord_join = false;
    std::string public_address;
    std::uint32_t max_players = 0;
    double autosave_minutes = 2.0;
    double run_for = -1.0;
    std::vector<std::string> commands;
    bool no_window = false;
    fs::path capture;          // the window's picture to this file (a check), then it closes
    double capture_at = 3.0;   // s after the start
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        const bool has_value = i + 1 < args.size();
        if (a == "--route" && has_value) {
            route_name = args[++i];
        } else if (a == "--root" && has_value) {
            const auto& text = args[++i];
            root = fs::path(std::u8string(text.begin(), text.end()));
            root_given = true;
        } else if (a == "--discord-join") {
            discord_join = true;
        } else if (a == "--public-address" && has_value) {
            public_address = args[++i];
        } else if (a == "--autosave" && has_value) {
            autosave_minutes = std::clamp(std::stod(args[++i]), 0.0, 24.0 * 60.0);
        } else if (a == "--max-players" && has_value) {
            max_players = static_cast<std::uint32_t>(std::clamp(std::stoi(args[++i]), 0, 65535));
        } else if (a == "--dispatcher-password" && has_value) {
            dispatcher_password = args[++i];
            password_given = true;
        } else if (a == "--data" && has_value) {
            const auto& text = args[++i];
            data = fs::path(std::u8string(text.begin(), text.end()));
        } else if (a == "--port" && has_value) {
            port = static_cast<std::uint16_t>(std::stoi(args[++i]));
            port_given = true;
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
        } else if (a == "--no-window") {
            no_window = true;
        } else if (a == "--capture" && has_value) {
            const auto& text = args[++i];
            capture = fs::path(std::u8string(text.begin(), text.end()));
        } else if (a == "--capture-at" && has_value) {
            capture_at = std::stod(args[++i]);
        } else {
            std::cerr << usage;
            return 2;
        }
    }
    server::SessionSettings settings{.root = root, .route = route_name, .data = data, .port = port, .season = season,
                                     .weather = weather, .time_of_day = time_of_day, .admit = admit,
                                     .dispatcher_password = dispatcher_password, .discord_join = discord_join,
                                     .public_address = public_address, .max_players = max_players,
                                     .autosave_minutes = autosave_minutes};
#ifdef OPENRTS_SERVER_WINDOW
    // The window (as rts_server.exe: its start form, then its map console),
    // unless the console is asked for or a test runs it.
    if (!no_window && run_for < 0.0) return server::run_server_window(settings, commands, capture, capture_at, {root_given, port_given, password_given});
#endif
    if (route_name.empty()) {
        std::cerr << usage;
        return 2;
    }

    std::cout << "openRTS server " << core::version_text() << ": loading " << route_name << " from " << root.string() << std::endl;
    std::string error;
    std::vector<std::string> lines;
    auto session = server::ServerSession::start(settings, error, lines);
    for (const auto& line : lines) std::cout << line << std::endl;
    if (!session) {
        std::cerr << "error: " << error << std::endl;
        return 1;
    }
    std::cout << "type \"help\" for the dispatcher's commands" << std::endl;

    std::signal(SIGINT, on_signal);
    std::mutex lines_mutex;
    std::vector<std::string> typed_lines(commands.begin(), commands.end());
    // The console: lines typed at the server. Detached: std::getline blocks.
    std::thread([&lines_mutex, &typed_lines] {
        std::string line;
        while (read_line(line)) {
            std::lock_guard lock(lines_mutex);
            typed_lines.push_back(line);
        }
    }).detach();

    const auto start = std::chrono::steady_clock::now();
    bool quit = false;
    while (!quit && !stop_requested) {
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (run_for >= 0.0 && now >= run_for) break;
        std::vector<std::string> typed;
        {
            std::lock_guard lock(lines_mutex);
            typed.swap(typed_lines);
        }
        for (const auto& line : typed) {
            const auto answer = session->command(line, quit);
            if (!answer.empty()) std::cout << answer << (answer.back() == '\n' ? "" : "\n") << std::flush;
        }
        session->step(now);
        for (const auto& line : session->game().take_log()) std::cout << stamp() << ' ' << line << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    session.reset();
    std::cout << "server stopped" << std::endl;
    return 0;
}
