#include "viewer_state.hpp"

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

[[nodiscard]] fs::path program_directory() {
#ifdef _WIN32
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) break;
        if (length < buffer.size()) return fs::path(buffer.substr(0, length)).parent_path();
        buffer.resize(buffer.size() * 2);
    }
#else
    std::error_code error;
    if (const auto self = fs::read_symlink("/proc/self/exe", error); !error) return self.parent_path();
#endif
    std::error_code error;
    return fs::current_path(error);
}

// %LOCALAPPDATA%\openRTS, or $XDG_DATA_HOME/openRTS (~/.local/share/openRTS).
[[nodiscard]] fs::path per_user_directory() {
#ifdef _WIN32
    std::wstring local(32768, L'\0');
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local.data(), static_cast<DWORD>(local.size()));
    if (length > 0 && length < local.size()) return fs::path(local.substr(0, length)) / L"openRTS";
#else
    if (const char* data = std::getenv("XDG_DATA_HOME"); data && *data) return fs::path(data) / "openRTS";
    if (const char* home = std::getenv("HOME"); home && *home) return fs::path(home) / ".local" / "share" / "openRTS";
#endif
    return {};
}

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
