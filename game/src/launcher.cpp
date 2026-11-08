#include "launcher.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
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
#include <shellapi.h>
#endif

#ifdef OPENRTS_HAS_IMGUI
#include <imgui.h>
#endif

#include <openrts/core/installation.hpp>
#include <openrts/core/vfs.hpp>
#include <openrts/formats/train_path.hpp>
#include <openrts/renderer/webgpu_context.hpp>

#include "controls_help.hpp"
#include "frame_limiter.hpp"
#include "key_layout.hpp"
#include "ui_fonts.hpp"
#include "ui_text.hpp"
#include "viewer_state.hpp"
#include "window_mode.hpp"

namespace openrts::game {
namespace {

namespace fs = std::filesystem;
using viewer::remember;
using viewer::remembered;
using viewer::tr;
using viewer::trf;

[[nodiscard]] std::string utf8(const fs::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

[[nodiscard]] fs::path from_utf8(const std::string& text) { return fs::path(std::u8string(text.begin(), text.end())); }

[[nodiscard]] bool is_installation(const fs::path& path) {
    std::error_code error;
    return !path.empty() && fs::is_directory(path / "ROUTES", error);
}

[[nodiscard]] std::string lower(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

[[nodiscard]] int remembered_int(const char* key, const int fallback) {
    const auto value = remembered(key);
    if (!value) return fallback;
    try {
        return std::stoi(*value);
    } catch (...) {
        return fallback;
    }
}

[[nodiscard]] float remembered_float(const char* key, const float fallback) {
    const auto value = remembered(key);
    if (!value) return fallback;
    try {
        return std::stof(*value);
    } catch (...) {
        return fallback;
    }
}

// A command-line argument for CreateProcess (the C runtime's parsing).
[[nodiscard]] std::string quoted(const std::string& argument) {
    if (!argument.empty() && argument.find_first_of(" \t\"") == std::string::npos) return argument;
    std::string out = "\"";
    std::size_t slashes = 0;
    for (const char c : argument) {
        if (c == '\\') {
            ++slashes;
            continue;
        }
        if (c == '"') out.append(slashes * 2 + 1, '\\');
        else out.append(slashes, '\\');
        slashes = 0;
        out += c;
    }
    out.append(slashes * 2, '\\');
    out += '"';
    return out;
}

// Text without the spaces around it.
[[nodiscard]] std::string trimmed(const std::string& text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

// The characters (not the bytes) of UTF-8 text.
[[nodiscard]] std::size_t characters(const std::string& text) {
    return static_cast<std::size_t>(
        std::count_if(text.begin(), text.end(), [](const char c) { return (static_cast<unsigned char>(c) & 0xC0u) != 0x80u; }));
}

// A list kept in viewer_state.ini as one value: "a;b;c".
[[nodiscard]] std::vector<std::string> split_list(const std::string& text) {
    std::vector<std::string> list;
    std::size_t from = 0;
    while (from <= text.size()) {
        const auto end = (std::min)(text.find(';', from), text.size());
        if (auto item = trimmed(text.substr(from, end - from)); !item.empty()) list.push_back(std::move(item));
        from = end + 1;
    }
    return list;
}

[[nodiscard]] std::string join_list(const std::vector<std::string>& list) {
    std::string text;
    for (const auto& item : list) text += (text.empty() ? "" : ";") + item;
    return text;
}

template <std::size_t N>
void set_text(std::array<char, N>& buffer, const std::string& text) {
    buffer.fill(0);
    std::copy_n(text.begin(), (std::min)(text.size(), N - 1), buffer.begin());
}

constexpr const char* season_keys[] = {"spring", "summer", "autumn", "winter"};
constexpr const char* season_names[] = {"Spring", "Summer", "Autumn", "Winter"};
constexpr const char* weather_keys[] = {"clear", "snow", "rain"};
constexpr const char* weather_names[] = {"Clear", "Snow", "Rain"};
constexpr const char* start_names[] = {"Running", "Switched on", "Cold"};
// Where the player starts (--spawn); the last one: no train, a flight over the route.
constexpr const char* spawn_names[] = {"In the cab", "Beside the car", "Above the train", "No train (fly over the route)"};
constexpr const char* spawn_keys[] = {"cab", "car", "above", "none"};
constexpr int beside_the_car = 1;
constexpr int no_train = 3;

// The menu's tabs, remembered by their keys.
enum class Tab { single, network, settings, controls };
constexpr const char* tab_keys[] = {"single", "network", "settings", "controls"};
constexpr int tab_count = static_cast<int>(std::size(tab_keys));

// How far the running game got in joining a server: its "status:" lines in
// openrts.log (viewer/src/main.cpp, report_status).
enum class NetStep { none, connecting, waiting, loading, playing, failed };

// Where the player is when the dispatcher assigns his train: in the cab
// (--spawn cab), or as before (the camera follows the train from above).
constexpr const char* net_spawn_names[] = {"In the cab", "Above the train"};
constexpr const char* net_spawn_keys[] = {"cab", "above"};
constexpr int net_spawn_cab = 0;

constexpr std::size_t nick_limit = 15;  // RTS takes a nick of 1 to 15 characters (docs/net.md)
constexpr std::size_t servers_kept = 8; // the servers joined before

#ifdef OPENRTS_HAS_IMGUI
// The menu's look, pushed around its window only: rounded frames, more air,
// a calm dark background, blue for what is chosen and green for Start.
class MenuStyle {
public:
    MenuStyle() {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {22.0f, 16.0f});
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {9.0f, 5.0f});
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {9.0f, 7.0f});
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 4.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 4.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {6.0f, 4.0f});
        ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, 5.0f);
        colour(ImGuiCol_WindowBg, {0.085f, 0.095f, 0.115f, 1.0f});
        colour(ImGuiCol_ChildBg, {0.115f, 0.125f, 0.150f, 1.0f});
        colour(ImGuiCol_PopupBg, {0.115f, 0.125f, 0.150f, 0.98f});
        colour(ImGuiCol_Border, {0.22f, 0.24f, 0.29f, 1.0f});
        colour(ImGuiCol_FrameBg, {0.17f, 0.19f, 0.23f, 1.0f});
        colour(ImGuiCol_FrameBgHovered, {0.21f, 0.24f, 0.29f, 1.0f});
        colour(ImGuiCol_FrameBgActive, {0.24f, 0.28f, 0.34f, 1.0f});
        colour(ImGuiCol_Header, {0.20f, 0.40f, 0.68f, 0.85f});
        colour(ImGuiCol_HeaderHovered, {0.24f, 0.46f, 0.76f, 0.70f});
        colour(ImGuiCol_HeaderActive, {0.26f, 0.50f, 0.82f, 1.0f});
        colour(ImGuiCol_Button, {0.19f, 0.22f, 0.27f, 1.0f});
        colour(ImGuiCol_ButtonHovered, {0.24f, 0.29f, 0.36f, 1.0f});
        colour(ImGuiCol_ButtonActive, {0.28f, 0.34f, 0.42f, 1.0f});
        colour(ImGuiCol_CheckMark, {0.42f, 0.66f, 0.98f, 1.0f});
        colour(ImGuiCol_SliderGrab, {0.36f, 0.58f, 0.90f, 1.0f});
        colour(ImGuiCol_SliderGrabActive, {0.46f, 0.68f, 1.0f, 1.0f});
        colour(ImGuiCol_TableHeaderBg, {0.15f, 0.17f, 0.21f, 1.0f});
        colour(ImGuiCol_Separator, {0.24f, 0.27f, 0.32f, 1.0f});
        colour(ImGuiCol_Text, {0.90f, 0.92f, 0.95f, 1.0f});
        colour(ImGuiCol_TextDisabled, {0.55f, 0.58f, 0.64f, 1.0f});
        colour(ImGuiCol_Tab, {0.13f, 0.145f, 0.175f, 1.0f});
        colour(ImGuiCol_TabHovered, {0.24f, 0.46f, 0.76f, 0.80f});
        colour(ImGuiCol_TabSelected, {0.20f, 0.40f, 0.68f, 1.0f});
        colour(ImGuiCol_TabSelectedOverline, {0.46f, 0.68f, 1.0f, 1.0f});
        colour(ImGuiCol_TabDimmed, {0.13f, 0.145f, 0.175f, 1.0f});
        colour(ImGuiCol_TabDimmedSelected, {0.20f, 0.40f, 0.68f, 1.0f});
        colour(ImGuiCol_TabDimmedSelectedOverline, {0.46f, 0.68f, 1.0f, 1.0f});
    }
    ~MenuStyle() {
        ImGui::PopStyleColor(colours_);
        ImGui::PopStyleVar(9);
    }
    MenuStyle(const MenuStyle&) = delete;
    MenuStyle& operator=(const MenuStyle&) = delete;

private:
    void colour(const ImGuiCol which, const ImVec4& value) {
        ImGui::PushStyleColor(which, value);
        ++colours_;
    }
    int colours_ = 0;
};

// A row of a form: the label on the left, the control filling the rest.
void form_row(const char* label) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
}

[[nodiscard]] bool begin_form(const char* id, const float label_width) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit)) return false;
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, label_width);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

// What a choice means, on the screen under it (dim, wrapped), not only in a
// tooltip.
void hint(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

// Something to fix before going on (amber, wrapped).
void warning(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.95f, 0.80f, 0.45f, 1.0f});
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

constexpr auto panel_flags = ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding;
#endif

} // namespace

struct Launcher::State {
    fs::path exe_dir;
    fs::path root;
    std::array<char, 1024> root_text{};
    std::shared_ptr<core::Vfs> vfs;
    std::vector<std::string> routes;
    std::vector<std::string> consists;
    std::vector<formats::TrainPath> paths;
    int route = -1, consist = -1, path = -1; // -1 path: the route's first
    std::array<char, 128> filter{};
    int minutes = 12 * 60;
    int season = 1, weather = 0, start = 0; // start: running, on, cold
    int spawn = 0;                          // spawn_names
    std::vector<std::string> situations;    // saved for the chosen route (the game's Train window)
    int situation = -1;                     // -1: none
    bool sound = true;
    float volume = 0.5f;
    bool msaa = true, vsync = true;
    bool experimental = false, hdr = false;
    int fps_limit = 0; // the game's FPS limit, shared with its panel ("fps_limit"): 0 none, 30..500
    viewer::WindowMode window_mode = viewer::WindowMode::windowed; // the game's window ("window_mode"), shared with its panel
    std::string status;
    Tab tab = Tab::single;
    bool select_tab = true; // the tab bar is told to show `tab` on the next frame
    // Network play: the server ("host[:port]", port 800 when none is given),
    // the nick, the servers joined before (newest first).
    std::array<char, 256> address{};
    std::array<char, 64> nick{};
    std::vector<std::string> servers;
    int net_spawn = 1; // net_spawn_names
    // An own server: only its settings' place, the server is not there yet
    // (docs/roadmap.md, stage 3).
    int host_port = 800;
    int host_players = 0; // 0: RTS and openRTS (Legacy), 1: openRTS only
    // The running game; for a network game, how far it got in joining.
    bool network_game = false;
    bool cancelled = false;
    NetStep net_step = NetStep::none;
    std::string net_reason;             // the failed status's word
    std::string net_detail;             // the route while waiting; the failure's detail
    std::string net_address, net_nick;  // what the game was started with
    std::uint64_t log_read = 0;         // bytes of openrts.log read
    std::string log_line;               // its unfinished last line
    std::chrono::steady_clock::time_point next_log_check{};
#ifdef _WIN32
    HANDLE process = nullptr;
#endif

    [[nodiscard]] bool running() const {
#ifdef _WIN32
        return process != nullptr;
#else
        return false;
#endif
    }

    // A network game still joining: it can be cancelled.
    [[nodiscard]] bool joining() const noexcept {
        return network_game && (net_step == NetStep::connecting || net_step == NetStep::waiting || net_step == NetStep::loading);
    }

    void set_root(const fs::path& folder) {
        root = folder;
        set_text(root_text, utf8(root));
    }

    void load_settings() {
        fs::path folder;
        if (const auto value = remembered("root"); value && is_installation(from_utf8(*value))) folder = from_utf8(*value);
        if (folder.empty()) {
            // The openRTS folder sits in the RTS installation; or a build folder below it.
            std::error_code error;
            std::vector<fs::path> starts{fs::current_path(error), exe_dir};
            for (auto from : starts) {
                for (int up = 0; up < 6 && !from.empty() && folder.empty(); ++up) {
                    if (is_installation(from)) folder = from;
                    from = from.parent_path();
                }
            }
        }
        set_root(folder);
        minutes = std::clamp(remembered_int("launcher_time", 12 * 60), 0, 24 * 60 - 1);
        season = std::clamp(remembered_int("launcher_season", 1), 0, 3);
        weather = std::clamp(remembered_int("launcher_weather", 0), 0, 2);
        start = std::clamp(remembered_int("launcher_start", 0), 0, 2);
        sound = remembered_int("launcher_sound", 1) != 0;
        volume = std::clamp(remembered_float("volume", 0.5f), 0.0f, 1.0f);
        msaa = remembered_int("launcher_msaa", 1) != 0;
        vsync = remembered_int("launcher_vsync", 1) != 0;
        experimental = remembered_int("launcher_experimental", 0) != 0;
        hdr = remembered_int("launcher_hdr", 0) != 0;
        spawn = std::clamp(remembered_int("launcher_spawn", 0), 0, no_train);
        fps_limit = viewer::clamp_fps_limit(remembered_int("fps_limit", 0));
        window_mode = viewer::parse_window_mode(remembered("window_mode").value_or("")).value_or(viewer::WindowMode::windowed);
        if (const auto value = remembered("launcher_tab")) {
            for (int i = 0; i < tab_count; ++i)
                if (*value == tab_keys[i]) tab = static_cast<Tab>(i);
        }
        set_text(address, remembered("mp_address").value_or("127.0.0.1"));
        set_text(nick, remembered("mp_nick").value_or(""));
        servers = split_list(remembered("mp_servers").value_or(""));
        if (const auto value = remembered("mp_spawn")) {
            for (int i = 0; i < 2; ++i)
                if (*value == net_spawn_keys[i]) net_spawn = i;
        }
    }

    void save_settings() const {
        remember("root", utf8(root));
        if (route >= 0) remember("launcher_route", routes[static_cast<std::size_t>(route)]);
        if (consist >= 0) remember("launcher_consist", consists[static_cast<std::size_t>(consist)]);
        remember("launcher_path", path >= 0 ? paths[static_cast<std::size_t>(path)].file : std::string());
        remember("launcher_time", std::to_string(minutes));
        remember("launcher_season", std::to_string(season));
        remember("launcher_weather", std::to_string(weather));
        remember("launcher_start", std::to_string(start));
        remember("launcher_sound", sound ? "1" : "0");
        remember("volume", std::to_string(volume));
        remember("launcher_msaa", msaa ? "1" : "0");
        remember("launcher_vsync", vsync ? "1" : "0");
        remember("launcher_experimental", experimental ? "1" : "0");
        remember("launcher_hdr", hdr ? "1" : "0");
        remember("launcher_spawn", std::to_string(spawn));
        remember("launcher_situation", situation >= 0 ? situations[static_cast<std::size_t>(situation)] : std::string());
        remember("launcher_tab", tab_keys[static_cast<int>(tab)]);
        remember("mp_address", address_text());
        remember("mp_nick", nick_text());
        remember("mp_servers", join_list(servers));
        remember("mp_spawn", net_spawn_keys[net_spawn]);
    }

    void scan() {
        routes.clear();
        consists.clear();
        paths.clear();
        route = consist = path = -1;
        vfs.reset();
        status.clear();
        if (!is_installation(root)) {
            status = tr("There is no ROUTES folder here: give the RTS folder.");
            return;
        }
        vfs = std::make_shared<core::Vfs>();
        core::mount_installation(*vfs, root, core::load_installation_config(root));
        vfs->build_index();
        if (const auto folder = core::VirtualPath::parse("ROUTES")) {
            for (const auto& entry : vfs->list(*folder)) {
                if (!entry.directory) continue;
                bool track = false;
                for (const auto& file : vfs->list(entry.path))
                    if (!file.directory && lower(std::string(file.path.filename())).ends_with(".trk")) track = true;
                if (track) routes.emplace_back(entry.path.filename());
            }
        }
        std::set<std::string> seen;
        std::vector<core::VirtualPath> stack;
        for (const auto* name : {"TRAINS/CONSISTS_RTS", "TRAINS/CONSISTS"})
            if (const auto folder = core::VirtualPath::parse(name)) stack.push_back(*folder);
        while (!stack.empty()) {
            const auto folder = stack.back();
            stack.pop_back();
            for (const auto& entry : vfs->list(folder)) {
                if (entry.directory) {
                    stack.push_back(entry.path);
                    continue;
                }
                std::string name(entry.path.filename());
                if (lower(name).ends_with(".con")) {
                    name.resize(name.size() - 4);
                    if (seen.insert(lower(name)).second) consists.push_back(name);
                }
            }
        }
        const auto by_name = [](const std::string& a, const std::string& b) { return lower(a) < lower(b); };
        std::sort(routes.begin(), routes.end(), by_name);
        std::sort(consists.begin(), consists.end(), by_name);
        const auto find = [](const std::vector<std::string>& list, const std::optional<std::string>& name) {
            if (!name) return -1;
            for (std::size_t i = 0; i < list.size(); ++i)
                if (lower(list[i]) == lower(*name)) return static_cast<int>(i);
            return -1;
        };
        route = find(routes, remembered("launcher_route"));
        consist = find(consists, remembered("launcher_consist"));
        load_paths();
        if (const auto wanted = remembered("launcher_path"); wanted && !wanted->empty()) {
            for (std::size_t i = 0; i < paths.size(); ++i)
                if (lower(paths[i].file) == lower(*wanted)) path = static_cast<int>(i);
        }
        if (routes.empty()) status = tr("No routes in this folder.");
    }

    [[nodiscard]] fs::path situation_folder() const {
        return route < 0 ? fs::path{} : viewer::user_data_directory() / "situations" / routes[static_cast<std::size_t>(route)];
    }

    // The route's saved situations, the remembered one chosen again.
    void load_situations() {
        situations.clear();
        situation = -1;
        if (route < 0) return;
        std::error_code error;
        for (const auto& entry : fs::directory_iterator(situation_folder(), error)) {
            if (entry.path().extension() == ".situation") situations.push_back(utf8(entry.path().stem()));
        }
        std::sort(situations.begin(), situations.end());
        if (const auto wanted = remembered("launcher_situation"); wanted && !wanted->empty()) {
            for (std::size_t i = 0; i < situations.size(); ++i)
                if (situations[i] == *wanted) situation = static_cast<int>(i);
        }
    }

    // A consist is wanted: not when the trains come from a situation or there is no train.
    [[nodiscard]] bool consist_used() const noexcept { return spawn != no_train && situation < 0; }
    [[nodiscard]] bool ready() const noexcept { return route >= 0 && (!consist_used() || consist >= 0); }

    [[nodiscard]] std::string address_text() const { return trimmed(address.data()); }
    [[nodiscard]] std::string nick_text() const { return trimmed(nick.data()); }
    [[nodiscard]] bool nick_fits() const {
        const auto length = characters(nick_text());
        return length >= 1 && length <= nick_limit;
    }
    // A server can be joined: the RTS folder (the server's route is opened
    // from it), an address and a nick.
    [[nodiscard]] bool network_ready() const { return vfs != nullptr && !address_text().empty() && nick_fits(); }
    [[nodiscard]] bool ready_for(const Tab which) const { return which == Tab::network ? network_ready() : ready(); }

    void load_paths() {
        load_situations();
        paths.clear();
        path = -1;
        if (!vfs || route < 0) return;
        if (const auto folder = core::VirtualPath::parse("ROUTES/" + routes[static_cast<std::size_t>(route)])) {
            paths = formats::load_train_paths(*vfs, *folder);
            std::erase_if(paths, [](const formats::TrainPath& p) { return !p.ok(); });
            std::sort(paths.begin(), paths.end(), [](const formats::TrainPath& a, const formats::TrainPath& b) {
                return lower(a.name.empty() ? a.file : a.name) < lower(b.name.empty() ? b.file : b.name);
            });
        }
    }

    [[nodiscard]] fs::path game_exe() const {
        const std::vector<fs::path> candidates{
            exe_dir / "openrts_viewer.exe",
            exe_dir.parent_path().parent_path() / "viewer" / exe_dir.filename() / "openrts_viewer.exe",
        };
        std::error_code error;
        for (const auto& candidate : candidates)
            if (fs::is_regular_file(candidate, error)) return candidate;
        return {};
    }

    // The game's command line for a tab's choice (empty: not ready, or no game).
    [[nodiscard]] std::string command_line(const Tab which) const {
        if (!ready_for(which)) return {};
        const auto exe = game_exe();
        if (exe.empty()) return {};
        std::vector<std::string> args{utf8(exe), "--window"};
        if (which == Tab::network) {
            // The route, its season and weather, the time and the train are the server's.
            args.insert(args.end(), {"--mp-connect", address_text(), "--mp-nick", nick_text(), "--root", utf8(root)});
            if (net_spawn == net_spawn_cab) args.insert(args.end(), {"--spawn", "cab"});
        } else {
            args.insert(args.end(), {"--route", routes[static_cast<std::size_t>(route)], "--root", utf8(root)});
            if (situation >= 0) {
                args.insert(args.end(), {"--situation", utf8(situation_folder() / (situations[static_cast<std::size_t>(situation)] + ".situation"))});
            } else if (consist_used()) {
                args.insert(args.end(), {"--consist", consists[static_cast<std::size_t>(consist)]});
                if (path >= 0) {
                    args.push_back("--path");
                    args.push_back(paths[static_cast<std::size_t>(path)].file);
                }
            }
            if (spawn != no_train) args.insert(args.end(), {"--spawn", spawn_keys[spawn]});
            char time[16];
            std::snprintf(time, sizeof time, "%02d:%02d", minutes / 60, minutes % 60);
            args.insert(args.end(), {"--time", time, "--season", season_keys[season], "--weather", weather_keys[weather]});
            if (start == 1) args.insert(args.end(), {"--start", "on"});
            if (start == 2) args.insert(args.end(), {"--start", "cold"});
        }
        if (sound) {
            args.insert(args.end(), {"--volume", std::to_string(volume)});
        } else {
            args.push_back("--no-sound");
        }
        if (!msaa) args.insert(args.end(), {"--msaa", "1"});
        if (!vsync) args.push_back("--no-vsync");
        if (experimental) args.push_back("--experimental");
        if (hdr) args.push_back("--hdr");
        std::string command;
        for (const auto& a : args) command += (command.empty() ? "" : " ") + quoted(a);
        return command;
    }

    // What the running network game is doing, or why it could not join.
    [[nodiscard]] std::string net_message() const {
        switch (net_step) {
        case NetStep::none: return {};
        case NetStep::connecting: return trf("Connecting to %s...", net_address.c_str());
        case NetStep::waiting:
            return trf("On the server, route %s. Waiting for a train: the dispatcher assigns one to %s on the console.", net_detail.c_str(),
                       net_nick.c_str());
        case NetStep::loading: return tr("A train is assigned: loading the world and the route...");
        case NetStep::playing: return trf("In the game on %s. The menu comes back when it is closed.", net_address.c_str());
        case NetStep::failed: break;
        }
        if (net_reason == "unreachable") return trf("The server %s does not answer: check the address and the port.", net_address.c_str());
        if (net_reason == "refused") {
            if (net_detail == "nick") return trf("The nick %s is in use on the server: choose another.", net_nick.c_str());
            if (net_detail == "full") return tr("The server takes no new players.");
            if (net_detail == "version") return tr("The server speaks another protocol version (RTS's is 101).");
            return trf("The server refused the login (code %s).", net_detail.c_str());
        }
        if (net_reason == "closed") return tr("The server closed the connection.");
        if (net_reason == "no-world") return tr("The server did not send the world.");
        if (net_reason == "timeout") return tr("No train was assigned in 30 minutes.");
        if (net_reason == "no-route") return trf("The server plays route %s, and it is not in the RTS folder.", net_detail.c_str());
        return trf("The game could not join the server: see openrts.log in %s.", utf8(viewer::user_data_directory()).c_str());
    }

    // A "status: <word> [detail]" line of the game (viewer/src/main.cpp).
    void take_status(const std::string& line) {
        constexpr std::string_view prefix = "status: ";
        if (!line.starts_with(prefix)) return;
        const auto rest = line.substr(prefix.size());
        const auto space = rest.find(' ');
        const auto word = rest.substr(0, space);
        const auto detail = space == std::string::npos ? std::string() : rest.substr(space + 1);
        if (word == "connecting") {
            net_step = NetStep::connecting;
        } else if (word == "waiting") {
            net_step = NetStep::waiting;
            net_detail = detail;
        } else if (word == "loading") {
            net_step = NetStep::loading;
        } else if (word == "playing") {
            net_step = NetStep::playing;
        } else {
            net_step = NetStep::failed;
            net_reason = word;
            net_detail = detail;
        }
        status = net_message();
    }

    // The game's new lines in openrts.log, for its status lines.
    void read_log() {
#ifdef _WIN32
        const auto log_path = viewer::user_data_directory() / L"openrts.log";
        const HANDLE file = CreateFileW(log_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        LARGE_INTEGER at{};
        at.QuadPart = static_cast<LONGLONG>(log_read);
        if (SetFilePointerEx(file, at, nullptr, FILE_BEGIN)) {
            char buffer[16384];
            DWORD got = 0;
            while (ReadFile(file, buffer, sizeof buffer, &got, nullptr) && got > 0) {
                log_read += got;
                log_line.append(buffer, got);
                for (auto end = log_line.find('\n'); end != std::string::npos; end = log_line.find('\n')) {
                    auto line = log_line.substr(0, end);
                    log_line.erase(0, end + 1);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    take_status(line);
                }
            }
        }
        CloseHandle(file);
#endif
    }

    void launch(const Tab which) {
        if (running() || !ready_for(which)) return;
        const auto command = command_line(which);
        if (command.empty()) {
            status = tr("openrts_viewer.exe is not found next to the menu.");
            return;
        }
        network_game = which == Tab::network;
        cancelled = false;
        net_step = network_game ? NetStep::connecting : NetStep::none;
        net_reason.clear();
        net_detail.clear();
        net_address = address_text();
        net_nick = nick_text();
        log_read = 0;
        log_line.clear();
        if (network_game) {
            std::erase(servers, net_address);
            servers.insert(servers.begin(), net_address);
            if (servers.size() > servers_kept) servers.resize(servers_kept);
        }
        save_settings();
#ifdef _WIN32
        const int length = MultiByteToWideChar(CP_UTF8, 0, command.c_str(), -1, nullptr, 0);
        std::wstring wide(static_cast<std::size_t>((std::max)(length, 1)), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, command.c_str(), -1, wide.data(), length);
        // The game's console output goes to openrts.log beside the settings
        // (no console window); a network game's steps are read back from it.
        SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        const auto log_path = viewer::user_data_directory() / L"openrts.log";
        const HANDLE log = CreateFileW(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        STARTUPINFOW startup{};
        startup.cb = sizeof startup;
        if (log != INVALID_HANDLE_VALUE) {
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdOutput = log;
            startup.hStdError = log;
            startup.hStdInput = nullptr;
        }
        PROCESS_INFORMATION info{};
        const BOOL ok = CreateProcessW(nullptr, wide.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                                       &startup, &info);
        if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
        if (!ok) {
            net_step = NetStep::none;
            status = viewer::trf("The game did not start (error %lu).", static_cast<unsigned long>(GetLastError()));
            return;
        }
        CloseHandle(info.hThread);
        process = info.hProcess;
        status = network_game ? net_message() : std::string(tr("The game is running. The menu comes back when it is closed."));
#else
        status = tr("Starting the game is only done on Windows yet.");
#endif
    }

    // Stops a network game that is still joining (the server sees the
    // connection close).
    void cancel() {
#ifdef _WIN32
        if (!process || !joining()) return;
        TerminateProcess(process, 1);
        cancelled = true;
#endif
    }

    void poll() {
#ifdef _WIN32
        if (!process) return;
        const bool ended = WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
        const auto now = std::chrono::steady_clock::now();
        if (joining() && !cancelled && (ended || now >= next_log_check)) {
            next_log_check = now + std::chrono::milliseconds(250);
            read_log();
        }
        if (!ended) return;
        DWORD code = 0;
        GetExitCodeProcess(process, &code);
        CloseHandle(process);
        process = nullptr;
        // The game's panel may have changed it.
        fps_limit = viewer::clamp_fps_limit(remembered_int("fps_limit", 0));
        window_mode = viewer::parse_window_mode(remembered("window_mode").value_or("")).value_or(viewer::WindowMode::windowed);
        if (cancelled) {
            net_step = NetStep::none;
            status = tr("The connection was cancelled.");
        } else if (!network_game || net_step != NetStep::failed) {
            // A failed join keeps its status: it says why.
            net_step = NetStep::none;
            status = code == 0 ? std::string(tr("The game is over."))
                               : viewer::trf("The game ended with code %lu: see openrts.log in %s.", static_cast<unsigned long>(code),
                                             utf8(viewer::user_data_directory()).c_str());
        }
#endif
    }

#ifdef OPENRTS_HAS_IMGUI
    void draw_tabs();
    void draw_single(float height);
    void draw_network(float height);
    void draw_settings(float height);
    void draw_controls(float height);
    void draw_bar(float button_width, float button_height, float status_height);
#endif
};

#ifdef OPENRTS_HAS_IMGUI
// The tabs. Their items close at once; the chosen tab is drawn below them.
void Launcher::State::draw_tabs() {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {20.0f, 8.0f});
    auto* large = viewer::large_font();
    if (large) ImGui::PushFont(large);
    // A tab chosen in code shows on the next frame; till then the bar's
    // own choice does not count.
    const bool forcing = select_tab;
    select_tab = false;
    if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_NoTooltip)) {
        const std::string labels[] = {std::string(tr("Single player")) + "###single", std::string(tr("Network game")) + "###network",
                                      std::string(tr("Settings")) + "###settings", std::string(tr("Controls")) + "###controls"};
        for (int i = 0; i < tab_count; ++i) {
            const auto which = static_cast<Tab>(i);
            const ImGuiTabItemFlags flags = forcing && tab == which ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (!ImGui::BeginTabItem(labels[i].c_str(), nullptr, flags)) continue;
            ImGui::EndTabItem();
            if (!forcing && tab != which) {
                tab = which;
                remember("launcher_tab", tab_keys[i]);
            }
        }
        ImGui::EndTabBar();
    }
    if (large) ImGui::PopFont();
    ImGui::PopStyleVar();
}

// Single player: route, consist (with its search) and path on the left, the
// trip on the right.
void Launcher::State::draw_single(const float height) {
    if (!vfs) {
        if (ImGui::BeginChild("##single_empty", {0.0f, height}, panel_flags)) {
            ImGui::TextWrapped("%s", tr("The routes and consists are read from the RTS folder: give it in Settings."));
            ImGui::Spacing();
            if (ImGui::Button(tr("Go to Settings"))) {
                tab = Tab::settings;
                select_tab = true;
            }
        }
        ImGui::EndChild();
        return;
    }
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float options_width = std::clamp(ImGui::GetIO().DisplaySize.x * 0.36f, 430.0f, 540.0f);
    const float lists_width = ImGui::GetContentRegionAvail().x - options_width - spacing;

    if (ImGui::BeginChild("##lists", {lists_width, height})) {
        constexpr auto table_flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame;
        if (ImGui::BeginTable("##choice", 3, table_flags, ImGui::GetContentRegionAvail())) {
            const auto route_header = std::string(tr("Route")) + "  " + std::to_string(routes.size()) + "###route";
            const auto consist_header = std::string(tr("Consist")) + "  " + std::to_string(consists.size()) + "###consist";
            const auto path_header = std::string(tr("Train path")) + "  " + std::to_string(paths.size()) + "###path";
            ImGui::TableSetupColumn(route_header.c_str());
            ImGui::TableSetupColumn(consist_header.c_str());
            ImGui::TableSetupColumn(path_header.c_str());
            ImGui::TableHeadersRow();
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            if (ImGui::BeginListBox("##routes", {-FLT_MIN, -FLT_MIN})) {
                for (std::size_t i = 0; i < routes.size(); ++i) {
                    if (ImGui::Selectable(routes[i].c_str(), route == static_cast<int>(i)) && route != static_cast<int>(i)) {
                        route = static_cast<int>(i);
                        load_paths();
                    }
                }
                ImGui::EndListBox();
            }

            ImGui::TableNextColumn();
            ImGui::BeginDisabled(!consist_used());
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##filter", tr("search"), filter.data(), filter.size());
            const auto wanted = lower(std::string(filter.data()));
            if (ImGui::BeginListBox("##consists", {-FLT_MIN, -FLT_MIN})) {
                for (std::size_t i = 0; i < consists.size(); ++i) {
                    if (!wanted.empty() && lower(consists[i]).find(wanted) == std::string::npos) continue;
                    if (ImGui::Selectable(consists[i].c_str(), consist == static_cast<int>(i))) consist = static_cast<int>(i);
                }
                ImGui::EndListBox();
            }

            ImGui::TableNextColumn();
            if (ImGui::BeginListBox("##paths", {-FLT_MIN, -FLT_MIN})) {
                if (ImGui::Selectable(tr("(the route's first path)"), path < 0)) path = -1;
                for (std::size_t i = 0; i < paths.size(); ++i) {
                    const auto& p = paths[i];
                    const auto label = (p.name.empty() ? p.file : p.name) + "##" + p.file;
                    if (ImGui::Selectable(label.c_str(), path == static_cast<int>(i))) path = static_cast<int>(i);
                    if (ImGui::IsItemHovered() && !p.start.empty()) ImGui::SetTooltip("%s -> %s (%s)", p.start.c_str(), p.end.c_str(), p.file.c_str());
                }
                ImGui::EndListBox();
            }
            ImGui::EndDisabled();
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();

    if (ImGui::BeginChild("##trip_panel", {options_width, height}, panel_flags)) {
        const float label_width = std::clamp(options_width * 0.48f, 190.0f, 240.0f);
        ImGui::SeparatorText(tr("Trip"));
        if (begin_form("##trip", label_width)) {
            char time[16];
            std::snprintf(time, sizeof time, "%02d:%02d", minutes / 60, minutes % 60);
            form_row(tr("Time"));
            ImGui::SliderInt("##time", &minutes, 0, 24 * 60 - 1, time);
            const char* seasons[4];
            for (int i = 0; i < 4; ++i) seasons[i] = tr(season_names[i]);
            form_row(tr("Season"));
            ImGui::Combo("##season", &season, seasons, 4);
            const char* weathers[3];
            for (int i = 0; i < 3; ++i) weathers[i] = tr(weather_names[i]);
            form_row(tr("Weather"));
            ImGui::Combo("##weather", &weather, weathers, 3);
            const char* starts[3];
            for (int i = 0; i < 3; ++i) starts[i] = tr(start_names[i]);
            form_row(tr("Train start"));
            ImGui::Combo("##start", &start, starts, 3);
            const char* spawns[4];
            for (int i = 0; i < 4; ++i) spawns[i] = tr(spawn_names[i]);
            form_row(tr("Start at"));
            ImGui::Combo("##spawn", &spawn, spawns, 4);
            form_row(tr("Situation"));
            const auto none = std::string(tr("none"));
            const auto& shown = situation >= 0 ? situations[static_cast<std::size_t>(situation)] : none;
            ImGui::BeginDisabled(situations.empty());
            if (ImGui::BeginCombo("##situation", shown.c_str())) {
                if (ImGui::Selectable(none.c_str(), situation < 0)) situation = -1;
                for (int i = 0; i < static_cast<int>(situations.size()); ++i) {
                    if (ImGui::Selectable(situations[static_cast<std::size_t>(i)].c_str(), situation == i)) situation = i;
                }
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
            ImGui::EndTable();
        }
        ImGui::Spacing();
        if (spawn == beside_the_car) hint(tr("On foot next to the cab car, as RTS starts an activity."));
        if (spawn == no_train) hint(tr("A flight over the route; the first consist put on the track in Track control (Ctrl+Alt+F7) is yours."));
        if (situation >= 0) {
            hint(tr("The situation's trains replace the consist chosen here."));
        } else if (situations.empty()) {
            hint(tr("Situations are saved in the game: Train window, Situations."));
        }
    }
    ImGui::EndChild();
}

// Network play: joining a server on the left; an own server on the right,
// its place kept for when there is one.
void Launcher::State::draw_network(const float height) {
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float host_width = std::clamp(ImGui::GetIO().DisplaySize.x * 0.36f, 430.0f, 540.0f);
    const float join_width = ImGui::GetContentRegionAvail().x - host_width - spacing;

    if (ImGui::BeginChild("##join", {join_width, height}, panel_flags)) {
        const float label_width = std::clamp(join_width * 0.30f, 170.0f, 240.0f);
        ImGui::SeparatorText(tr("Join a server"));
        if (begin_form("##join_form", label_width)) {
            form_row(tr("Server address"));
            ImGui::InputTextWithHint("##address", "127.0.0.1:800", address.data(), address.size());
            form_row(tr("Nick"));
            ImGui::InputText("##nick", nick.data(), nick.size());
            const char* spawns[2];
            for (int i = 0; i < 2; ++i) spawns[i] = tr(net_spawn_names[i]);
            form_row(tr("Start at"));
            ImGui::Combo("##net_spawn", &net_spawn, spawns, 2);
            ImGui::EndTable();
        }
        hint(net_spawn == net_spawn_cab ? tr("When the dispatcher assigns your train, you sit at its desk.")
                                        : tr("When the dispatcher assigns your train, the camera follows it from above."));
        hint(tr("The port is 800 when none is given. The nick is how the other players and the dispatcher see you."));
        const auto length = characters(nick_text());
        if (length == 0) {
            warning(tr("Give a nick: 1 to 15 characters."));
        } else if (length > nick_limit) {
            warning(trf("The nick has %zu characters: RTS takes 15 at most.", length).c_str());
        }
        if (!vfs) warning(tr("Give the RTS folder in Settings: the server's route is opened from it."));
        ImGui::Spacing();
        ImGui::SeparatorText(tr("Servers joined before"));
        if (servers.empty()) {
            ImGui::TextDisabled("%s", tr("none yet"));
        } else {
            const auto rows = static_cast<float>((std::min)(servers.size(), servers_kept));
            const float box = rows * ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().FramePadding.y * 2.0f;
            if (ImGui::BeginListBox("##servers", {-FLT_MIN, box})) {
                const auto current = address_text();
                for (const auto& server : servers) {
                    if (ImGui::Selectable(server.c_str(), server == current)) set_text(address, server);
                }
                ImGui::EndListBox();
            }
        }
        ImGui::Spacing();
        ImGui::SeparatorText(tr("In the game"));
        hint(tr("The server gives the route, the season, the weather and the time. Your train is assigned by the dispatcher on the console; until then the game waits, and this menu says so."));
        hint(tr("An RTS server (rts_server.exe) takes openRTS and RTS (rts1.exe) players together."));
    }
    ImGui::EndChild();
    ImGui::SameLine();

    if (ImGui::BeginChild("##host", {host_width, height}, panel_flags)) {
        const float label_width = std::clamp(host_width * 0.40f, 150.0f, 210.0f);
        ImGui::SeparatorText(tr("Own server"));
        hint(tr("Not there yet: an openRTS server is the next stage. For now start an RTS server (rts_server.exe) and join it."));
        ImGui::Spacing();
        ImGui::BeginDisabled(true);
        if (begin_form("##host_form", label_width)) {
            form_row(tr("Port"));
            ImGui::InputInt("##host_port", &host_port, 0, 0);
            form_row(tr("Players"));
            const char* kinds[] = {tr("RTS and openRTS (Legacy)"), tr("openRTS only")};
            ImGui::Combo("##host_players", &host_players, kinds, 2);
            ImGui::EndTable();
        }
        hint(host_players == 0 ? tr("Legacy: rts1.exe joins too; the simulation follows RTS for everyone.")
                               : tr("openRTS only: its own protocol, and the server's simulation settings."));
        ImGui::Spacing();
        ImGui::Button(tr("Start the server"), {-FLT_MIN, 0.0f});
        ImGui::EndDisabled();
    }
    ImGui::EndChild();
}

// Settings: the game (RTS folder, language, keys) and the sound on the left,
// the graphics on the right.
void Launcher::State::draw_settings(const float height) {
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float half = (ImGui::GetContentRegionAvail().x - spacing) * 0.5f;
    const float label_width = std::clamp(half * 0.38f, 170.0f, 260.0f);

    if (ImGui::BeginChild("##settings_game", {half, height}, panel_flags)) {
        ImGui::SeparatorText(tr("Game"));
        if (begin_form("##game_form", label_width)) {
            form_row(tr("RTS folder"));
            const char* read = tr("Read");
            const float read_width = ImGui::CalcTextSize(read).x + ImGui::GetStyle().FramePadding.x * 2.0f + 24.0f;
            ImGui::SetNextItemWidth(-(read_width + spacing));
            const bool entered = ImGui::InputText("##root", root_text.data(), root_text.size(), ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if (ImGui::Button(read, {read_width, 0.0f}) || entered) {
                set_root(from_utf8(std::string(root_text.data())));
                scan();
                if (is_installation(root)) remember("root", utf8(root));
            }
            int language = viewer::ui_language() == viewer::UiLanguage::russian ? 0 : 1;
            const char* languages[] = {"Русский", "English"};
            form_row("Язык / Language");
            if (ImGui::Combo("##language", &language, languages, 2)) {
                viewer::set_ui_language(language == 0 ? viewer::UiLanguage::russian : viewer::UiLanguage::english);
                remember("language", language == 0 ? "ru" : "en");
            }
            int layout = viewer::key_layout() == viewer::KeyLayout::rts ? 0 : 1;
            const char* layouts[] = {tr("RTS (Num0 / Num1)"), tr("openRTS (no numeric keypad)")};
            form_row(tr("Keys"));
            if (ImGui::Combo("##keys", &layout, layouts, 2)) {
                viewer::set_key_layout(layout == 0 ? viewer::KeyLayout::rts : viewer::KeyLayout::openrts);
                remember("key_layout", layout == 0 ? "rts" : "openrts");
            }
            form_row(tr("Key hints at the bottom of the screen"));
            bool hints = viewer::key_hints();
            if (ImGui::Checkbox("##key_hints", &hints)) {
                viewer::set_key_hints(hints);
                remember("key_hints", hints ? "1" : "0");
            }
            ImGui::EndTable();
        }
        if (vfs) {
            hint(trf("Found: %zu routes, %zu consists.", routes.size(), consists.size()).c_str());
        } else {
            warning(tr("There is no ROUTES folder here: give the RTS folder."));
        }
        ImGui::Spacing();
        ImGui::SeparatorText(tr("Sound"));
        if (begin_form("##sound_form", label_width)) {
            form_row(tr("Sound"));
            ImGui::Checkbox("##sound", &sound);
            form_row(tr("Sound volume"));
            ImGui::BeginDisabled(!sound);
            ImGui::SliderFloat("##volume", &volume, 0.0f, 1.0f, "%.2f");
            ImGui::EndDisabled();
            ImGui::EndTable();
        }
#ifdef _WIN32
        ImGui::Spacing();
        // The settings and the game's log (openrts.log, for a bug report).
        ImGui::SeparatorText(tr("Settings and log"));
        if (ImGui::Button(tr("Open the settings and log folder"), {-FLT_MIN, 0.0f}))
            ShellExecuteW(nullptr, L"open", viewer::user_data_directory().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        hint(utf8(viewer::user_data_directory()).c_str());
#endif
    }
    ImGui::EndChild();
    ImGui::SameLine();

    if (ImGui::BeginChild("##settings_graphics", {0.0f, height}, panel_flags)) {
        ImGui::SeparatorText(tr("Graphics"));
        if (begin_form("##graphics_form", label_width)) {
            // The graphics API of the game (and of this menu from its next start).
            int backend = 0;
            if (const auto key = remembered("backend"))
                backend = static_cast<int>(renderer::parse_graphics_backend(*key).value_or(renderer::GraphicsBackend::automatic));
            const char* backends[] = {tr("Automatic"), "Vulkan", "DirectX 12", "OpenGL"};
            form_row(tr("Graphics API"));
            if (ImGui::Combo("##backend", &backend, backends, 4))
                remember("backend", renderer::graphics_backend_key(static_cast<renderer::GraphicsBackend>(backend)));
            form_row(tr("Anti-aliasing (MSAA 4x)"));
            ImGui::Checkbox("##msaa", &msaa);
            form_row(tr("VSync"));
            ImGui::Checkbox("##vsync", &vsync);
            {
                // One slider: 30..500, its right end "no limit".
                int shown = fps_limit == 0 ? viewer::max_fps_limit + 1 : fps_limit;
                const std::string format = shown > viewer::max_fps_limit ? std::string(tr("no limit")) : std::string(tr("%d frames/s"));
                form_row(tr("FPS limit"));
                if (ImGui::SliderInt("##fps_limit", &shown, viewer::min_fps_limit, viewer::max_fps_limit + 1, format.c_str(),
                                     ImGuiSliderFlags_AlwaysClamp)) {
                    fps_limit = shown > viewer::max_fps_limit ? 0 : shown;
                    remember("fps_limit", std::to_string(fps_limit));
                }
            }
            {
                // The game's window (this menu stays a window).
                int mode = static_cast<int>(window_mode);
                const char* modes[] = {viewer::window_mode_name(viewer::WindowMode::windowed),
                                       viewer::window_mode_name(viewer::WindowMode::borderless),
                                       viewer::window_mode_name(viewer::WindowMode::fullscreen)};
                form_row(tr("Window mode"));
                if (ImGui::Combo("##window_mode", &mode, modes, viewer::window_mode_count)) {
                    window_mode = static_cast<viewer::WindowMode>(mode);
                    remember("window_mode", viewer::window_mode_key(window_mode));
                }
            }
            form_row(tr("Experimental renderer"));
            ImGui::Checkbox("##experimental", &experimental);
            form_row("HDR");
            ImGui::Checkbox("##hdr", &hdr);
            ImGui::SameLine();
            ImGui::TextDisabled("%s", tr("tone curve with the experimental renderer"));
            ImGui::EndTable();
        }
        ImGui::Spacing();
        hint(tr("Automatic: Vulkan, else DirectX 12. Try DirectX 12 if the game does not start or the screen stays black; OpenGL is for old cards, the experimental renderer may not work there."));
        hint(tr("The menu itself takes the graphics API from its next start."));
        hint(tr("Window mode: full screen takes the monitor in its own mode; a borderless window covers it and Alt+Tab is quicker. In the game Alt+Enter switches between the window and full screen."));
    }
    ImGui::EndChild();
}

// The simulator's keys and mouse (the game's panel has the same tab).
void Launcher::State::draw_controls(const float height) {
    if (ImGui::BeginChild("##controls", {0.0f, height}, panel_flags)) viewer::draw_controls_help();
    ImGui::EndChild();
}

// The bottom bar of a game tab: what was chosen, and the button that starts
// it (Cancel while a network game is joining).
void Launcher::State::draw_bar(const float button_width, const float button_height, const float status_height) {
    const float line = ImGui::GetTextLineHeightWithSpacing();
    const float bar_height = ImGui::GetContentRegionAvail().y - status_height;
    const float bar_top = ImGui::GetCursorPosY() + (std::max)(0.0f, (bar_height - button_height) * 0.5f);
    ImGui::SetCursorPosY(bar_top + (std::max)(0.0f, (button_height - 2.0f * line) * 0.5f));
    ImGui::BeginGroup();
    if (tab == Tab::network) {
        const auto server = address_text();
        const auto who = nick_text();
        ImGui::Text("%s  ·  %s", server.empty() ? tr("no server address") : server.c_str(), who.empty() ? tr("no nick") : who.c_str());
        ImGui::TextDisabled("%s", tr("The route, the time and the train come from the server."));
    } else if (!ready()) {
        ImGui::TextDisabled("%s", tr("Choose a route and a consist."));
    } else if (!consist_used()) {
        const auto what = situation >= 0 ? viewer::trf("situation %s", situations[static_cast<std::size_t>(situation)].c_str())
                                         : std::string(tr("no train"));
        ImGui::Text("%s  ·  %s", routes[static_cast<std::size_t>(route)].c_str(), what.c_str());
        ImGui::TextDisabled("%02d:%02d  ·  %s  ·  %s", minutes / 60, minutes % 60, tr(season_names[season]), tr(weather_names[weather]));
    } else {
        ImGui::Text("%s  ·  %s", routes[static_cast<std::size_t>(route)].c_str(), consists[static_cast<std::size_t>(consist)].c_str());
        const auto& shown = path < 0 || path >= static_cast<int>(paths.size())
                                ? std::string(tr("(the route's first path)"))
                                : (paths[static_cast<std::size_t>(path)].name.empty() ? paths[static_cast<std::size_t>(path)].file
                                                                                      : paths[static_cast<std::size_t>(path)].name);
        ImGui::TextDisabled("%s  ·  %02d:%02d  ·  %s  ·  %s", shown.c_str(), minutes / 60, minutes % 60, tr(season_names[season]),
                            tr(weather_names[weather]));
    }
    ImGui::EndGroup();

    ImGui::SetCursorPos({ImGui::GetWindowContentRegionMax().x - button_width, bar_top});
    auto* large = viewer::large_font();
    if (large) ImGui::PushFont(large);
    if (running() && joining()) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.55f, 0.22f, 0.20f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.66f, 0.27f, 0.24f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{0.48f, 0.18f, 0.16f, 1.0f});
        if (ImGui::Button(tr("Cancel"), {button_width, button_height})) cancel();
        ImGui::PopStyleColor(3);
    } else {
        const bool busy = running();
        ImGui::BeginDisabled(busy || !ready_for(tab));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.16f, 0.52f, 0.30f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.20f, 0.62f, 0.36f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{0.14f, 0.45f, 0.26f, 1.0f});
        const char* label = busy ? tr("The game is running") : tab == Tab::network ? tr("Connect") : tr("Start");
        if (ImGui::Button(label, {button_width, button_height})) launch(tab);
        ImGui::PopStyleColor(3);
        ImGui::EndDisabled();
    }
    if (large) ImGui::PopFont();
}
#endif

Launcher::Launcher(std::filesystem::path exe_dir) : state_(std::make_unique<State>()) {
    state_->exe_dir = std::move(exe_dir);
    state_->load_settings();
    state_->scan();
    // No RTS folder yet: the settings first, where it is given.
    if (!state_->vfs) state_->tab = Tab::settings;
}

std::string Launcher::command() const { return state_->command_line(Tab::single); }

std::string Launcher::network_command() const { return state_->command_line(Tab::network); }

void Launcher::show_tab(const std::string_view key) {
    for (int i = 0; i < tab_count; ++i) {
        if (key != tab_keys[i]) continue;
        state_->tab = static_cast<Tab>(i);
        state_->select_tab = true;
    }
}

std::string Launcher::summary() const {
    const auto& s = *state_;
    return "root " + utf8(s.root) + ": " + std::to_string(s.routes.size()) + " routes, " + std::to_string(s.consists.size()) +
           " consists, " + std::to_string(s.paths.size()) + " paths of the chosen route; tab " + tab_keys[static_cast<int>(s.tab)] +
           "; server " + s.address_text() + " as " + s.nick_text() + (s.status.empty() ? "" : " (" + s.status + ")");
}

Launcher::~Launcher() {
#ifdef _WIN32
    if (state_->process) CloseHandle(state_->process);
#endif
}

void Launcher::update(renderer::ModelRenderer&, viewer::ViewerCamera&, const viewer::SceneClock&, const viewer::SceneInput&) {
    state_->poll();
}

viewer::SceneEnvironment Launcher::environment() const {
    viewer::SceneEnvironment environment;
    environment.clear_colour = {0.10f, 0.11f, 0.13f, 1.0f};
    return environment;
}

void Launcher::draw_overlay(const viewer::ViewerCamera&) const {
#ifdef OPENRTS_HAS_IMGUI
    auto& s = *state_;
    const auto& io = ImGui::GetIO();
    const MenuStyle style;
    ImGui::SetNextWindowPos({0.0f, 0.0f});
    ImGui::SetNextWindowSize(io.DisplaySize);
    constexpr auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                           ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                           ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar;
    ImGui::Begin("##launcher", nullptr, flags);

    // The name, then the tabs.
    if (auto* title = viewer::title_font()) {
        ImGui::PushFont(title);
        ImGui::TextUnformatted("openRTS");
        ImGui::PopFont();
    } else {
        ImGui::TextUnformatted("openRTS");
    }
    s.draw_tabs();
    ImGui::Spacing();

    // The bottom keeps its height (the separator with its spacing, on a game
    // tab the button, a status line or two); the tab's panels take the rest.
    constexpr float button_width = 240.0f, button_height = 46.0f;
    const bool game_tab = s.tab == Tab::single || s.tab == Tab::network;
    const auto& style_now = ImGui::GetStyle();
    const float status_height = s.status.empty() ? 0.0f : ImGui::GetTextLineHeightWithSpacing() * 2.0f;
    const float bar = (game_tab ? button_height + style_now.ItemSpacing.y * 4.0f : style_now.ItemSpacing.y * 2.0f) + 6.0f;
    const float middle = (std::max)(200.0f, ImGui::GetContentRegionAvail().y - bar - status_height);

    ImGui::BeginDisabled(s.running());
    switch (s.tab) {
    case Tab::single: s.draw_single(middle); break;
    case Tab::network: s.draw_network(middle); break;
    case Tab::settings: s.draw_settings(middle); break;
    case Tab::controls: s.draw_controls(middle); break;
    }
    ImGui::EndDisabled();

    ImGui::Separator();
    if (game_tab) s.draw_bar(button_width, button_height, status_height);
    if (!s.status.empty()) {
        const bool failed = s.net_step == NetStep::failed;
        ImGui::PushStyleColor(ImGuiCol_Text, failed ? ImVec4{1.0f, 0.55f, 0.45f, 1.0f} : ImVec4{0.95f, 0.80f, 0.45f, 1.0f});
        ImGui::TextWrapped("%s", s.status.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::End();
#endif
}

} // namespace openrts::game
