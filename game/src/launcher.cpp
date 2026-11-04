#include "launcher.hpp"

#include <algorithm>
#include <array>
#include <cctype>
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

#include "key_layout.hpp"
#include "ui_fonts.hpp"
#include "ui_text.hpp"
#include "viewer_state.hpp"

namespace openrts::game {
namespace {

namespace fs = std::filesystem;
using viewer::remember;
using viewer::remembered;
using viewer::tr;

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

constexpr const char* season_keys[] = {"spring", "summer", "autumn", "winter"};
constexpr const char* season_names[] = {"Spring", "Summer", "Autumn", "Winter"};
constexpr const char* weather_keys[] = {"clear", "snow", "rain"};
constexpr const char* weather_names[] = {"Clear", "Snow", "Rain"};
constexpr const char* start_names[] = {"Running", "Switched on", "Cold"};
// Where the player starts (--spawn); the last one: no train, a flight over the route.
constexpr const char* spawn_names[] = {"In the cab", "Beside the car", "Above the train", "No train (fly over the route)"};
constexpr const char* spawn_keys[] = {"cab", "car", "above", "none"};
constexpr int no_train = 3;

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
    std::string status;
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

    void set_root(const fs::path& folder) {
        root = folder;
        const auto text = utf8(root);
        root_text.fill(0);
        std::copy_n(text.begin(), (std::min)(text.size(), root_text.size() - 1), root_text.begin());
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

    [[nodiscard]] std::string command_line() const {
        if (!ready()) return {};
        const auto exe = game_exe();
        if (exe.empty()) return {};
        std::vector<std::string> args{utf8(exe), "--window", "--route", routes[static_cast<std::size_t>(route)], "--root", utf8(root)};
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

    void launch() {
        if (running() || !ready()) return;
        const auto command = command_line();
        if (command.empty()) {
            status = tr("openrts_viewer.exe is not found next to the menu.");
            return;
        }
        save_settings();
#ifdef _WIN32
        const int length = MultiByteToWideChar(CP_UTF8, 0, command.c_str(), -1, nullptr, 0);
        std::wstring wide(static_cast<std::size_t>((std::max)(length, 1)), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, command.c_str(), -1, wide.data(), length);
        // The game's console output goes to openrts.log beside the settings
        // (no console window).
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
            status = viewer::trf("The game did not start (error %lu).", static_cast<unsigned long>(GetLastError()));
            return;
        }
        CloseHandle(info.hThread);
        process = info.hProcess;
        status = tr("The game is running. The menu comes back when it is closed.");
#else
        status = tr("Starting the game is only done on Windows yet.");
#endif
    }

    void poll() {
#ifdef _WIN32
        if (!process || WaitForSingleObject(process, 0) != WAIT_OBJECT_0) return;
        DWORD code = 0;
        GetExitCodeProcess(process, &code);
        CloseHandle(process);
        process = nullptr;
        status = code == 0 ? std::string(tr("The game is over."))
                           : viewer::trf("The game ended with code %lu: see openrts.log in %s.", static_cast<unsigned long>(code),
                                         utf8(viewer::user_data_directory()).c_str());
#endif
    }
};

Launcher::Launcher(std::filesystem::path exe_dir) : state_(std::make_unique<State>()) {
    state_->exe_dir = std::move(exe_dir);
    state_->load_settings();
    state_->scan();
}

std::string Launcher::command() const { return state_->command_line(); }

std::string Launcher::summary() const {
    const auto& s = *state_;
    return "root " + utf8(s.root) + ": " + std::to_string(s.routes.size()) + " routes, " + std::to_string(s.consists.size()) +
           " consists, " + std::to_string(s.paths.size()) + " paths of the chosen route" + (s.status.empty() ? "" : " (" + s.status + ")");
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

#ifdef OPENRTS_HAS_IMGUI
namespace {

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
    }
    ~MenuStyle() {
        ImGui::PopStyleColor(colours_);
        ImGui::PopStyleVar(8);
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

} // namespace
#endif

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
    const float spacing = ImGui::GetStyle().ItemSpacing.x;

    // The name, then the RTS folder.
    if (auto* title = viewer::title_font()) {
        ImGui::PushFont(title);
        ImGui::TextUnformatted("openRTS");
        ImGui::PopFont();
    } else {
        ImGui::TextUnformatted("openRTS");
    }
    const bool busy = s.running();
    ImGui::BeginDisabled(busy);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(tr("RTS folder"));
    ImGui::SameLine();
    const char* read = tr("Read");
    const float read_width = ImGui::CalcTextSize(read).x + ImGui::GetStyle().FramePadding.x * 2.0f + 24.0f;
    ImGui::SetNextItemWidth(-(read_width + spacing));
    const bool entered = ImGui::InputText("##root", s.root_text.data(), s.root_text.size(), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button(read, {read_width, 0.0f}) || entered) {
        s.set_root(from_utf8(std::string(s.root_text.data())));
        s.scan();
        if (is_installation(s.root)) remember("root", utf8(s.root));
    }
    ImGui::Spacing();

    // The bottom bar keeps its height (the separator with its spacing, Start,
    // a status line or two); the lists and the options share the rest.
    constexpr float start_width = 240.0f, start_height = 46.0f;
    const float line = ImGui::GetTextLineHeightWithSpacing();
    const float bottom = start_height + ImGui::GetStyle().ItemSpacing.y * 4.0f + 6.0f + (s.status.empty() ? 0.0f : line * 2.0f);
    const float middle = (std::max)(200.0f, ImGui::GetContentRegionAvail().y - bottom);
    const float options_width = std::clamp(io.DisplaySize.x * 0.36f, 430.0f, 540.0f);
    const float lists_width = ImGui::GetContentRegionAvail().x - options_width - spacing;

    // Left: route, consist (with its search) and path.
    if (ImGui::BeginChild("##lists", {lists_width, middle})) {
        constexpr auto table_flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame;
        if (ImGui::BeginTable("##choice", 3, table_flags, ImGui::GetContentRegionAvail())) {
            const auto routes = std::string(tr("Route")) + "  " + std::to_string(s.routes.size()) + "###route";
            const auto consists = std::string(tr("Consist")) + "  " + std::to_string(s.consists.size()) + "###consist";
            const auto paths = std::string(tr("Train path")) + "  " + std::to_string(s.paths.size()) + "###path";
            ImGui::TableSetupColumn(routes.c_str());
            ImGui::TableSetupColumn(consists.c_str());
            ImGui::TableSetupColumn(paths.c_str());
            ImGui::TableHeadersRow();
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            if (ImGui::BeginListBox("##routes", {-FLT_MIN, -FLT_MIN})) {
                for (std::size_t i = 0; i < s.routes.size(); ++i) {
                    if (ImGui::Selectable(s.routes[i].c_str(), s.route == static_cast<int>(i)) && s.route != static_cast<int>(i)) {
                        s.route = static_cast<int>(i);
                        s.load_paths();
                    }
                }
                ImGui::EndListBox();
            }

            ImGui::TableNextColumn();
            ImGui::BeginDisabled(!s.consist_used());
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##filter", tr("search"), s.filter.data(), s.filter.size());
            const auto filter = lower(std::string(s.filter.data()));
            if (ImGui::BeginListBox("##consists", {-FLT_MIN, -FLT_MIN})) {
                for (std::size_t i = 0; i < s.consists.size(); ++i) {
                    if (!filter.empty() && lower(s.consists[i]).find(filter) == std::string::npos) continue;
                    if (ImGui::Selectable(s.consists[i].c_str(), s.consist == static_cast<int>(i))) s.consist = static_cast<int>(i);
                }
                ImGui::EndListBox();
            }

            ImGui::TableNextColumn();
            if (ImGui::BeginListBox("##paths", {-FLT_MIN, -FLT_MIN})) {
                if (ImGui::Selectable(tr("(the route's first path)"), s.path < 0)) s.path = -1;
                for (std::size_t i = 0; i < s.paths.size(); ++i) {
                    const auto& p = s.paths[i];
                    const auto label = (p.name.empty() ? p.file : p.name) + "##" + p.file;
                    if (ImGui::Selectable(label.c_str(), s.path == static_cast<int>(i))) s.path = static_cast<int>(i);
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

    // Right: the trip, then the settings, a line each.
    if (ImGui::BeginChild("##options", {options_width, middle}, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding)) {
        const float label_width = std::clamp(options_width * 0.48f, 190.0f, 240.0f);
        ImGui::SeparatorText(tr("Trip"));
        if (begin_form("##trip", label_width)) {
            char time[16];
            std::snprintf(time, sizeof time, "%02d:%02d", s.minutes / 60, s.minutes % 60);
            form_row(tr("Time"));
            ImGui::SliderInt("##time", &s.minutes, 0, 24 * 60 - 1, time);
            const char* seasons[4];
            for (int i = 0; i < 4; ++i) seasons[i] = tr(season_names[i]);
            form_row(tr("Season"));
            ImGui::Combo("##season", &s.season, seasons, 4);
            const char* weathers[3];
            for (int i = 0; i < 3; ++i) weathers[i] = tr(weather_names[i]);
            form_row(tr("Weather"));
            ImGui::Combo("##weather", &s.weather, weathers, 3);
            const char* starts[3];
            for (int i = 0; i < 3; ++i) starts[i] = tr(start_names[i]);
            form_row(tr("Train start"));
            ImGui::Combo("##start", &s.start, starts, 3);
            const char* spawns[4];
            for (int i = 0; i < 4; ++i) spawns[i] = tr(spawn_names[i]);
            form_row(tr("Start at"));
            ImGui::Combo("##spawn", &s.spawn, spawns, 4);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("Beside the car: on foot next to the cab car, as RTS starts an activity. No train: fly over the route; the first consist put on the track in Track control (Ctrl+Alt+F7) is yours."));
            form_row(tr("Situation"));
            const auto none = std::string(tr("none"));
            const auto& shown = s.situation >= 0 ? s.situations[static_cast<std::size_t>(s.situation)] : none;
            ImGui::BeginDisabled(s.situations.empty());
            if (ImGui::BeginCombo("##situation", shown.c_str())) {
                if (ImGui::Selectable(none.c_str(), s.situation < 0)) s.situation = -1;
                for (int i = 0; i < static_cast<int>(s.situations.size()); ++i) {
                    if (ImGui::Selectable(s.situations[static_cast<std::size_t>(i)].c_str(), s.situation == i)) s.situation = i;
                }
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", tr("The trains as they were saved in the game (Train window, Situations); they replace the consist chosen here."));
            ImGui::EndTable();
        }

        ImGui::Spacing();
        ImGui::SeparatorText(tr("Settings"));
        if (begin_form("##settings", label_width)) {
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
            // The graphics API of the game (and of this menu from its next start).
            int backend = 0;
            if (const auto key = remembered("backend"))
                backend = static_cast<int>(renderer::parse_graphics_backend(*key).value_or(renderer::GraphicsBackend::automatic));
            const char* backends[] = {tr("Automatic"), "Vulkan", "DirectX 12", "OpenGL"};
            form_row(tr("Graphics API"));
            if (ImGui::Combo("##backend", &backend, backends, 4))
                remember("backend", renderer::graphics_backend_key(static_cast<renderer::GraphicsBackend>(backend)));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("Automatic: Vulkan, else DirectX 12. Try DirectX 12 if the game does not start or the screen stays black; OpenGL is for old cards, the experimental renderer may not work there."));
            form_row(tr("Sound"));
            ImGui::Checkbox("##sound", &s.sound);
            form_row(tr("Sound volume"));
            ImGui::BeginDisabled(!s.sound);
            ImGui::SliderFloat("##volume", &s.volume, 0.0f, 1.0f, "%.2f");
            ImGui::EndDisabled();
            form_row(tr("Anti-aliasing (MSAA 4x)"));
            ImGui::Checkbox("##msaa", &s.msaa);
            form_row(tr("VSync"));
            ImGui::Checkbox("##vsync", &s.vsync);
            form_row(tr("Experimental renderer"));
            ImGui::Checkbox("##experimental", &s.experimental);
            form_row("HDR");
            ImGui::Checkbox("##hdr", &s.hdr);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("HDR (tone curve with the experimental renderer)"));
            ImGui::EndTable();
        }
#ifdef _WIN32
        ImGui::Spacing();
        // The settings and the game's log (openrts.log, for a bug report).
        if (ImGui::Button(tr("Open the settings and log folder"), {-FLT_MIN, 0.0f}))
            ShellExecuteW(nullptr, L"open", viewer::user_data_directory().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", utf8(viewer::user_data_directory()).c_str());
#endif
    }
    ImGui::EndChild();
    ImGui::EndDisabled();

    // Bottom: what was chosen, and Start.
    ImGui::Separator();
    // Start in the middle of the bar (above the status lines), the two
    // summary lines centred on it.
    const float status_height = s.status.empty() ? 0.0f : line * 2.0f;
    const float bar_height = ImGui::GetContentRegionAvail().y - status_height;
    const float bar_top = ImGui::GetCursorPosY() + (std::max)(0.0f, (bar_height - start_height) * 0.5f);
    ImGui::SetCursorPosY(bar_top + (std::max)(0.0f, (start_height - 2.0f * line) * 0.5f));
    ImGui::BeginGroup();
    if (!s.ready()) {
        ImGui::TextDisabled("%s", tr("Choose a route and a consist."));
    } else if (!s.consist_used()) {
        const auto what = s.situation >= 0 ? viewer::trf("situation %s", s.situations[static_cast<std::size_t>(s.situation)].c_str())
                                           : std::string(tr("no train"));
        ImGui::Text("%s  ·  %s", s.routes[static_cast<std::size_t>(s.route)].c_str(), what.c_str());
        ImGui::TextDisabled("%02d:%02d  ·  %s  ·  %s", s.minutes / 60, s.minutes % 60, tr(season_names[s.season]), tr(weather_names[s.weather]));
    } else {
        ImGui::Text("%s  ·  %s", s.routes[static_cast<std::size_t>(s.route)].c_str(), s.consists[static_cast<std::size_t>(s.consist)].c_str());
        const auto& path = s.path < 0 || s.path >= static_cast<int>(s.paths.size())
                               ? std::string(tr("(the route's first path)"))
                               : (s.paths[static_cast<std::size_t>(s.path)].name.empty() ? s.paths[static_cast<std::size_t>(s.path)].file
                                                                                         : s.paths[static_cast<std::size_t>(s.path)].name);
        ImGui::TextDisabled("%s  ·  %02d:%02d  ·  %s  ·  %s", path.c_str(), s.minutes / 60, s.minutes % 60, tr(season_names[s.season]),
                            tr(weather_names[s.weather]));
    }
    ImGui::EndGroup();
    ImGui::SetCursorPos({ImGui::GetWindowContentRegionMax().x - start_width, bar_top});
    ImGui::BeginDisabled(busy || !s.ready());
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.16f, 0.52f, 0.30f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.20f, 0.62f, 0.36f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{0.14f, 0.45f, 0.26f, 1.0f});
    if (auto* large = viewer::large_font()) ImGui::PushFont(large);
    if (ImGui::Button(busy ? tr("The game is running") : tr("Start"), {start_width, start_height})) s.launch();
    if (viewer::large_font()) ImGui::PopFont();
    ImGui::PopStyleColor(3);
    ImGui::EndDisabled();
    if (!s.status.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.95f, 0.80f, 0.45f, 1.0f});
        ImGui::TextWrapped("%s", s.status.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::End();
#endif
}

} // namespace openrts::game
