#include "viewer_state.hpp"

#include <openrts/core/installation.hpp>
#include <openrts/core/user_data.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace openrts::viewer {
namespace {

namespace fs = std::filesystem;

using core::per_user_directory;
using core::program_directory;

// What an older version kept next to the program or in the working
// directory, copied once into `to` (nothing is deleted there).
void move_in_older_files(const fs::path& to) {
    std::error_code error;
    if (fs::exists(to / "viewer_state.ini", error)) return;
    // The newest of the program's folder and the working directory (a
    // shortcut or a terminal may have started it elsewhere before).
    std::vector<fs::path> sources;
    for (const auto& from : {program_directory(), fs::current_path(error)}) {
        if (from.empty() || fs::equivalent(from, to, error) || !fs::exists(from / "viewer_state.ini", error)) continue;
        sources.push_back(from);
    }
    std::sort(sources.begin(), sources.end(), [](const fs::path& a, const fs::path& b) {
        std::error_code e;
        return fs::last_write_time(a / "viewer_state.ini", e) > fs::last_write_time(b / "viewer_state.ini", e);
    });
    for (const auto& from : sources) {
        for (const char* file : {"viewer_state.ini", "imgui.ini"}) {
            if (fs::exists(from / file, error)) fs::copy_file(from / file, to / file, fs::copy_options::skip_existing, error);
        }
        for (const char* folder : {"debug_views", "user_paths"}) {
            if (fs::is_directory(from / folder, error))
                fs::copy(from / folder, to / folder, fs::copy_options::recursive | fs::copy_options::skip_existing, error);
        }
        std::cout << "settings: copied from " << from.string() << " to " << to.string() << std::endl;
        return;
    }
}

[[nodiscard]] fs::path find_user_data_directory() {
    std::error_code error;
    const auto program = program_directory();
    // Portable: everything next to the program, as before.
    if (!program.empty() && fs::exists(program / "portable.txt", error)) return program;
    if (const auto user = per_user_directory(); !user.empty()) {
        fs::create_directories(user, error);
        if (fs::is_directory(user, error)) {
            move_in_older_files(user);
            return user;
        }
    }
    return program.empty() ? fs::current_path(error) : program;
}

[[nodiscard]] fs::path state_file() { return user_data_directory() / "viewer_state.ini"; }

[[nodiscard]] std::map<std::string, std::string> read_state() {
    std::map<std::string, std::string> values;
    std::ifstream input(state_file(), std::ios::binary);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto equals = line.find('=');
        if (equals == std::string::npos) continue;
        values[line.substr(0, equals)] = line.substr(equals + 1);
    }
    return values;
}

} // namespace

const std::filesystem::path& user_data_directory() {
    static const fs::path directory = find_user_data_directory();
    return directory;
}

std::optional<std::string> remembered(const std::string_view key) {
    const auto values = read_state();
    const auto it = values.find(std::string(key));
    if (it == values.end()) return std::nullopt;
    return it->second;
}

void remember(const std::string_view key, const std::string_view value) {
    auto values = read_state();
    values[std::string(key)] = std::string(value);
    std::ofstream output(state_file(), std::ios::binary | std::ios::trunc);
    for (const auto& [name, text] : values) output << name << '=' << text << '\n';
}

namespace {
int capture_panel_tab = -1;
} // namespace

void set_capture_panel(const int tab) noexcept { capture_panel_tab = tab; }

namespace {
std::optional<bool> developer_mode_value;
} // namespace

bool developer_mode() {
    if (!developer_mode_value) developer_mode_value = remembered("developer_mode").value_or("0") == "1";
    return *developer_mode_value;
}

void set_developer_mode(const bool on, const bool keep) {
    developer_mode_value = on;
    if (keep) remember("developer_mode", on ? "1" : "0");
}
int capture_panel() noexcept { return capture_panel_tab; }

void apply_settings_defaults() {
    constexpr std::string_view mark = "defaults_openrts_brakes";
    auto values = read_state();
    if (values[std::string(mark)] == "1") return;
    // The Train window's "realistic (openRTS)" preset and openRTS's devices.
    const std::pair<const char*, const char*> settings[] = {
        {"realistic_air", "1"}, {"dll_air", "2"},     {"gauges_openrts_air", "1"}, {"rails", "1"},
        {"limit_brake_force", "1"}, {"distributor_stability", "1"}, {"brake_condition", "6"}, {"brake_pressing", "100"},
        {"own_brakes", "1"},    {"own_klub", "1"},    {"no_brakes_dll", "1"},
    };
    for (const auto& [key, value] : settings) values[key] = value;
    values[std::string(mark)] = "1";
    std::ofstream output(state_file(), std::ios::binary | std::ios::trunc);
    for (const auto& [name, text] : values) output << name << '=' << text << '\n';
}

std::filesystem::path remembered_folder(const std::string_view key) {
    const auto value = remembered(key);
    if (!value) return {};
    return std::filesystem::path(std::u8string(value->begin(), value->end()));
}

void remember_folder(const std::string_view key, const std::filesystem::path& folder) {
    const auto text = folder.u8string();
    remember(key, std::string(text.begin(), text.end()));
}

} // namespace openrts::viewer
