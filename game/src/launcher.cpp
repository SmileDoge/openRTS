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
#endif

#ifdef OPENRTS_HAS_IMGUI
#include <imgui.h>
#endif

#include <openrts/core/installation.hpp>
#include <openrts/core/vfs.hpp>
#include <openrts/formats/train_path.hpp>

#include "key_layout.hpp"
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

    void load_paths() {
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
        if (route < 0 || consist < 0) return {};
        const auto exe = game_exe();
        if (exe.empty()) return {};
        std::vector<std::string> args{utf8(exe), "--window", "--route", routes[static_cast<std::size_t>(route)],
                                      "--root", utf8(root), "--consist", consists[static_cast<std::size_t>(consist)]};
        if (path >= 0) {
            args.push_back("--path");
            args.push_back(paths[static_cast<std::size_t>(path)].file);
        }
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
        if (running() || route < 0 || consist < 0) return;
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
        // The game's console output goes to openrts.log (no console window).
        SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        const HANDLE log = CreateFileW(L"openrts.log", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
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
                           : viewer::trf("The game ended with code %lu: see openrts.log.", static_cast<unsigned long>(code));
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

void Launcher::draw_overlay(const viewer::ViewerCamera&) const {
#ifdef OPENRTS_HAS_IMGUI
    auto& s = *state_;
    const auto& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({0.0f, 0.0f});
    ImGui::SetNextWindowSize(io.DisplaySize);
    constexpr auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                           ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                           ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("##launcher", nullptr, flags);
    ImGui::SetWindowFontScale(1.6f);
    ImGui::TextUnformatted("openRTS");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();

    const bool busy = s.running();
    ImGui::BeginDisabled(busy);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(tr("RTS folder"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-160.0f);
    const bool entered = ImGui::InputText("##root", s.root_text.data(), s.root_text.size(), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button(tr("Read")) || entered) {
        s.set_root(from_utf8(std::string(s.root_text.data())));
        s.scan();
        if (is_installation(s.root)) remember("root", utf8(s.root));
    }

    const float list_height = (std::max)(160.0f, io.DisplaySize.y - 430.0f);
    if (ImGui::BeginTable("##choice", 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(tr("Route"));
        if (ImGui::BeginListBox("##routes", {-FLT_MIN, list_height})) {
            for (std::size_t i = 0; i < s.routes.size(); ++i) {
                if (ImGui::Selectable(s.routes[i].c_str(), s.route == static_cast<int>(i)) && s.route != static_cast<int>(i)) {
                    s.route = static_cast<int>(i);
                    s.load_paths();
                }
            }
            ImGui::EndListBox();
        }

        ImGui::TableNextColumn();
        ImGui::TextUnformatted(tr("Consist"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##filter", tr("search"), s.filter.data(), s.filter.size());
        const auto filter = lower(std::string(s.filter.data()));
        if (ImGui::BeginListBox("##consists", {-FLT_MIN, list_height})) {
            for (std::size_t i = 0; i < s.consists.size(); ++i) {
                if (!filter.empty() && lower(s.consists[i]).find(filter) == std::string::npos) continue;
                if (ImGui::Selectable(s.consists[i].c_str(), s.consist == static_cast<int>(i))) s.consist = static_cast<int>(i);
            }
            ImGui::EndListBox();
        }

        ImGui::TableNextColumn();
        ImGui::TextUnformatted(tr("Train path"));
        if (ImGui::BeginListBox("##paths", {-FLT_MIN, list_height})) {
            if (ImGui::Selectable(tr("(the route's first path)"), s.path < 0)) s.path = -1;
            for (std::size_t i = 0; i < s.paths.size(); ++i) {
                const auto& p = s.paths[i];
                const auto label = (p.name.empty() ? p.file : p.name) + "##" + p.file;
                if (ImGui::Selectable(label.c_str(), s.path == static_cast<int>(i))) s.path = static_cast<int>(i);
                if (ImGui::IsItemHovered() && !p.start.empty()) ImGui::SetTooltip("%s -> %s (%s)", p.start.c_str(), p.end.c_str(), p.file.c_str());
            }
            ImGui::EndListBox();
        }
        ImGui::EndTable();
    }

    ImGui::Separator();
    char time[16];
    std::snprintf(time, sizeof time, "%02d:%02d", s.minutes / 60, s.minutes % 60);
    ImGui::SetNextItemWidth(260.0f);
    ImGui::SliderInt(tr("Time"), &s.minutes, 0, 24 * 60 - 1, time);
    ImGui::SameLine();
    const char* seasons[4];
    for (int i = 0; i < 4; ++i) seasons[i] = tr(season_names[i]);
    ImGui::SetNextItemWidth(120.0f);
    ImGui::Combo(tr("Season"), &s.season, seasons, 4);
    ImGui::SameLine();
    const char* weathers[3];
    for (int i = 0; i < 3; ++i) weathers[i] = tr(weather_names[i]);
    ImGui::SetNextItemWidth(120.0f);
    ImGui::Combo(tr("Weather"), &s.weather, weathers, 3);
    ImGui::SameLine();
    const char* starts[3];
    for (int i = 0; i < 3; ++i) starts[i] = tr(start_names[i]);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::Combo(tr("Train start"), &s.start, starts, 3);

    if (ImGui::CollapsingHeader(tr("Settings"), ImGuiTreeNodeFlags_DefaultOpen)) {
        int language = viewer::ui_language() == viewer::UiLanguage::russian ? 0 : 1;
        const char* languages[] = {"Русский", "English"};
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::Combo("Язык / Language", &language, languages, 2)) {
            viewer::set_ui_language(language == 0 ? viewer::UiLanguage::russian : viewer::UiLanguage::english);
            remember("language", language == 0 ? "ru" : "en");
        }
        ImGui::SameLine();
        int layout = viewer::key_layout() == viewer::KeyLayout::rts ? 0 : 1;
        const char* layouts[] = {tr("RTS (Num0 / Num1)"), tr("openRTS (no numeric keypad)")};
        ImGui::SetNextItemWidth(240.0f);
        if (ImGui::Combo(tr("Keys"), &layout, layouts, 2)) {
            viewer::set_key_layout(layout == 0 ? viewer::KeyLayout::rts : viewer::KeyLayout::openrts);
            remember("key_layout", layout == 0 ? "rts" : "openrts");
        }
        ImGui::Checkbox(tr("Sound"), &s.sound);
        ImGui::SameLine();
        ImGui::BeginDisabled(!s.sound);
        ImGui::SetNextItemWidth(220.0f);
        ImGui::SliderFloat(tr("Sound volume"), &s.volume, 0.0f, 1.0f, "%.2f");
        ImGui::EndDisabled();
        ImGui::Checkbox(tr("Anti-aliasing (MSAA 4x)"), &s.msaa);
        ImGui::SameLine();
        ImGui::Checkbox(tr("VSync"), &s.vsync);
        ImGui::Checkbox(tr("Experimental lighting"), &s.experimental);
        ImGui::SameLine();
        ImGui::Checkbox(tr("HDR (tone curve with experimental lighting)"), &s.hdr);
    }
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::BeginDisabled(busy || s.route < 0 || s.consist < 0);
    if (ImGui::Button(tr("Start"), {220.0f, 44.0f})) s.launch();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (s.route < 0 || s.consist < 0) {
        ImGui::TextDisabled("%s", tr("Choose a route and a consist."));
    } else {
        ImGui::Text("%s  ·  %s", s.routes[static_cast<std::size_t>(s.route)].c_str(), s.consists[static_cast<std::size_t>(s.consist)].c_str());
    }
    if (!s.status.empty()) ImGui::TextWrapped("%s", s.status.c_str());
    ImGui::End();
#endif
}

} // namespace openrts::game
