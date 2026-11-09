#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <openrts/assets/assets.hpp>
#include <openrts/renderer/renderer.hpp>

#include "exit_codes.hpp"
#include "frame_limiter.hpp"

#ifdef OPENRTS_HAS_WGPU
#include <openrts/core/core.hpp>
#include <openrts/core/installation.hpp>
#include <openrts/core/vfs.hpp>
#include <openrts/core/virtual_path.hpp>
#include <openrts/formats/ace_file.hpp>
#include <openrts/formats/environment.hpp>
#include <openrts/net/connection.hpp>
#include <openrts/net/packages.hpp>
#include <openrts/net/play.hpp>
#include <openrts/renderer/webgpu_context.hpp>
#include <openrts/renderer/webgpu_texture.hpp>

#include "crash_report.hpp"

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
#include "debug_view.hpp"
#include "viewer_scene.hpp"
#include "viewer_state.hpp"
#include "window_viewer.hpp"
#endif

namespace {

void print_banner() {
    std::cout << "openRTS viewer skeleton\n"
              << "assets: " << openrts::assets::module_name() << '\n'
              << "renderer backend: " << openrts::renderer::backend_name() << '\n';
}

#ifdef OPENRTS_HAS_WGPU
namespace exit_code = openrts::viewer::exit_code;

// `--ace <file>`: decodes the texture and uploads it to a headless device.
int run_ace_upload_check(const std::string_view file) {
    const auto image = openrts::formats::decode_structured_ace_rgba_path(file);
    if (!image.ok()) {
        std::cerr << "ACE decode failed: " << image.error << '\n';
        return exit_code::content_failed;
    }

    auto context = openrts::renderer::create_headless_wgpu_device();
    if (!context.ok()) {
        std::cerr << "WebGPU device failed: " << context.error << '\n';
        return exit_code::device_failed;
    }

    const auto texture = openrts::renderer::upload_rgba8_texture(
        context.context.device(),
        {.width = image.image->width, .height = image.image->height, .pixels = &image.image->pixels});
    if (!texture.ok()) {
        std::cerr << "WebGPU texture upload failed: " << texture.error << '\n';
        return exit_code::upload_failed;
    }

    std::cout << "uploaded RGBA8 texture: " << image.image->width << 'x' << image.image->height << '\n';
    return exit_code::success;
}

namespace fs = std::filesystem;
using openrts::core::VirtualPath;

// The installation and the content path a command-line path refers to:
// an existing file or folder inside an installation, or (with --root) a
// virtual path such as `ROUTES/Kuban_RTS` that may live only in a pack.
struct ContentTarget {
    fs::path root;
    VirtualPath path;
    bool installation = true;
};

[[nodiscard]] std::optional<ContentTarget> locate(const std::string_view argument, const std::optional<fs::path>& root,
                                                  const std::string_view default_folder) {
    const fs::path given{std::u8string(argument.begin(), argument.end())};
    std::error_code error;
    if (fs::exists(given, error)) {
        const auto absolute = fs::weakly_canonical(fs::absolute(given, error), error);
        const auto installation = root ? std::optional<fs::path>(fs::weakly_canonical(*root, error))
                                       : openrts::core::find_installation_root(absolute);
        if (installation) {
            const auto relative = absolute.lexically_relative(*installation);
            if (auto path = VirtualPath::from_filesystem(relative); path && !relative.empty() && *relative.begin() != "..") {
                return ContentTarget{*installation, std::move(*path)};
            }
        }
        // A loose file outside any installation: its grandparent becomes the
        // root, so `..\TEXTURES` references still resolve.
        const auto base = absolute.parent_path().parent_path();
        const auto path = VirtualPath::from_filesystem(absolute.lexically_relative(base));
        if (!path) return std::nullopt;
        return ContentTarget{base, *path, false};
    }
    if (!root) return std::nullopt;
    auto path = VirtualPath::parse(argument);
    if (!path) return std::nullopt;
    if (path->str().find('/') == std::string::npos) path = VirtualPath::parse(std::string(default_folder) + "/" + path->str());
    return ContentTarget{*root, *path};
}

// `--window --shape <file.s>` or `--window --route <route folder | name>`,
// followed by optional `--root <installation>`, `--mount <zip|folder>` (an
// extra mod-layer source), `--no-hot-reload`, `--consist <name>` and `--path <name>` (a train to drive, on a
// route path start; C puts it on the track nearest to the camera), `--track-control` (open the
// Track control window at start), (
// the route start), `--experimental`, `--backend auto|vulkan|dx12|opengl`, `--reshade-depth on|off`, `--situation <name|file>`, `--spawn cab|car|above`, `--no-vsync`, `--no-batching`, `--msaa 1|4` (default 4), `--no-sound`, `--sound-offline`, `--volume <0..1>` (default 0.5),
// `--time HH:MM[:SS]`, `--speed <multiplier>`, `--paused` (start paused),
// `--window-mode windowed|borderless|fullscreen`,
// `--season spring|summer|autumn|winter` and `--weather clear|snow|rain`.
// A step of joining a server for the start menu (game/src/launcher.cpp reads
// openrts.log): one line "status: <word> [detail]".
void report_status(const std::string_view what) { std::cout << "status: " << what << std::endl; }

// Logs in to an RTS server as a player (docs/net.md, "Observer"). The reply
// names the route, its season and weather. Nothing on failure, said on the
// console and in a status line.
std::optional<openrts::net::LoginReply> login_server(openrts::net::RtsConnection& connection, const std::string& address,
                                                     const std::string& nick, const bool console) {
    namespace net = openrts::net;
    report_status("connecting " + address);
    const auto login = connection.connect(address, net::from_utf8(nick), 10000, console);
    if (!login.ok()) {
        std::cerr << "Server " << address << ": " << login.error;
        std::string why = "unreachable";
        switch (login.refused) {
        case 0: break;
        case 0x4701: std::cerr << " (the nickname is in use)"; why = "refused nick"; break;
        case 0x4702:
            std::cerr << (console ? " (not a dispatcher on this server: --dispatcher <nick> there)" : " (the server takes no new players)");
            why = console ? "refused console" : "refused full";
            break;
        case 0x4705: std::cerr << " (not protocol 101)"; why = "refused version"; break;
        default: {
            std::cerr << " (code 0x" << std::hex << login.refused << std::dec << ")";
            char code[16];
            std::snprintf(code, sizeof code, "0x%04X", static_cast<unsigned>(login.refused));
            why = std::string("refused ") + code;
            break;
        }
        }
        // The line ends before the status line (one file for both).
        std::cerr << std::endl;
        report_status(why);
        return std::nullopt;
    }
    const auto& reply = *login.reply;
    std::cout << "net: logged in to " << address << " as " << nick << "; route " << net::to_utf8(reply.route) << "; season "
              << int(reply.season) << ", weather " << int(reply.weather) << "; " << net::to_utf8(reply.description) << std::endl;
    return reply;
}

// After the login: the wait for a train the dispatcher assigns (the server
// sends the world only then: 0x44ECAC), then the startup snapshot. Packages
// after its end go to `early`. Nothing on failure, said on the console and
// in a status line.
std::optional<openrts::net::Bytes> wait_for_world(openrts::net::RtsConnection& connection, const std::string& address,
                                                  const std::string& nick, const std::string& route,
                                                  std::vector<openrts::net::ReceivedPackage>& early, const bool console) {
    namespace net = openrts::net;
    bool asked = false;
    if (console) {
        // The dispatcher's console needs no train: the world at once.
        std::cout << "net: the dispatcher's console; asking for the world" << std::endl;
        report_status("loading");
        connection.send(net::id(net::Package::snapshot), {});
        asked = true;
    } else {
        std::cout << "net: waiting for a train: assign one to " << nick << " on the dispatcher console" << std::endl;
        report_status("waiting " + route);
    }

    bool collecting = false;
    bool done = false;
    net::WireWriter world;
    const auto started = std::chrono::steady_clock::now();
    while (!done) {
        for (auto& package : connection.receive()) {
            if (done) {
                early.push_back(std::move(package));
                continue;
            }
            if (package.id == net::id(net::Package::server_down)) {
                std::cerr << "Server " << address << " closed the connection" << std::endl;
                report_status("closed");
                return std::nullopt;
            }
            if (!asked) {
                const auto assign = package.id == net::id(net::Package::train_assign)
                                        ? net::decode_train_assign(package.body)
                                        : std::nullopt;
                if (assign && assign->assign.mine) {
                    std::cout << "net: train " << assign->train << " assigned; asking for the world" << std::endl;
                    report_status("loading");
                    connection.send(net::id(net::Package::snapshot), {});
                    asked = true;
                }
                continue;
            }
            if (package.id == net::id(net::Package::snapshot_again)) {
                std::cerr << "Server " << address << " will not send the world (0x4703)" << std::endl;
                report_status("no-world");
                return std::nullopt;
            }
            if (package.id == net::id(net::Package::login) && package.body.empty()) collecting = true;
            if (!collecting) continue;
            world.frame(package.id, package.body);
            if (package.id == net::id(net::Package::snapshot)) done = true;
        }
        if (done) break;
        if (std::chrono::steady_clock::now() - started > std::chrono::minutes(30)) {
            std::cerr << "Server " << address << ": no train assigned in 30 minutes" << std::endl;
            report_status("timeout");
            return std::nullopt;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return world.take();
}

int run_window(std::string_view mode, std::string_view path, const std::vector<std::string_view>& flags) {
    openrts::viewer::WindowOptions options;
    // The volume last set (viewer panel or the start menu); --volume overrides it.
    if (const auto volume = openrts::viewer::remembered("volume")) {
        try {
            options.sound_volume = std::clamp(std::stof(*volume), 0.0f, 1.0f);
        } catch (...) {
        }
    }
    options.title = "openRTS " + std::string(openrts::core::version());
    // --view first: it sets what to open and the conditions; later flags win.
    std::string view_mode;
    std::string view_content;
    for (std::size_t i = 0; i + 1 < flags.size(); ++i) {
        if (flags[i] != "--view") continue;
        std::string error;
        const auto file = openrts::viewer::resolve_debug_view(std::string(flags[i + 1]));
        auto view = openrts::viewer::load_debug_view(file, error);
        if (!view) {
            std::cerr << "Debug view: " << error << '\n';
            return exit_code::content_failed;
        }
        std::cout << "debug view: " << file.string() << '\n';
        options.time_of_day = view->time_of_day;
        options.time_speed = 0.0f;
        options.season = static_cast<openrts::renderer::rts::Season>(view->season);
        options.weather = static_cast<openrts::renderer::rts::Weather>(view->weather);
        if (view->experimental) options.shading = openrts::renderer::ShadingPath::experimental;
        if (mode == "--view") {
            view_mode = "--" + view->mode;
            view_content = view->content;
        }
        options.view = std::move(view);
    }
    if (mode == "--view") {
        if (!options.view) return exit_code::content_failed;
        mode = view_mode;
        path = view_content;
    }
    std::optional<fs::path> root;
    std::vector<fs::path> mounts;
    bool hot_reload = true;
    std::string consist;
    std::string train_path;
    bool track_control = false;
    int start_state = -1;
    std::filesystem::path situation; // --situation
    int start_view = 0;              // --spawn
    std::string save_situation;      // --save-situation
    std::filesystem::path mp_snapshot; // --mp-snapshot
    std::string mp_connect;          // --mp-connect host:port
    std::string mp_nick = "openRTS"; // --mp-nick
    bool mp_console = false;         // --mp-console: as the dispatcher's console
    std::filesystem::path mp_save;   // --mp-save-snapshot: the snapshot the server sent
    bool time_given = false;         // --time wins over a snapshot's time
    bool fps_given = false;          // --fps-limit wins over the remembered one (not remembered)
    bool season_given = false;       // --season / --weather win over the server's
    bool weather_given = false;
    for (std::size_t i = 0; i < flags.size(); ++i) {
        const bool has_value = i + 1 < flags.size();
        if (flags[i] == "--view" && has_value) {
            ++i;
        } else if (flags[i] == "--root" && has_value) {
            const auto value = flags[++i];
            root = fs::path(std::u8string(value.begin(), value.end()));
        } else if (flags[i] == "--mount" && has_value) {
            const auto value = flags[++i];
            mounts.emplace_back(std::u8string(value.begin(), value.end()));
        } else if (flags[i] == "--no-hot-reload") {
            hot_reload = false;
        } else if (flags[i] == "--consist" && has_value) {
            consist = std::string(flags[++i]);
        } else if (flags[i] == "--path" && has_value) {
            train_path = flags[++i];
        } else if (flags[i] == "--start" && has_value) {
            // The DLLs' start state, as an activity's TrainState.
            const auto& value = flags[++i];
            start_state = value == "cold" ? 0 : value == "on" ? 1 : -1;
        } else if (flags[i] == "--situation" && has_value) {
            const auto value = flags[++i];
            situation = std::filesystem::path(std::u8string(value.begin(), value.end()));
        } else if (flags[i] == "--mp-connect" && has_value) {
            mp_connect = std::string(flags[++i]);
        } else if (flags[i] == "--mp-save-snapshot" && has_value) {
            const auto value = flags[++i];
            mp_save = std::filesystem::path(std::u8string(value.begin(), value.end()));
        } else if (flags[i] == "--mp-nick" && has_value) {
            mp_nick = std::string(flags[++i]);
        } else if (flags[i] == "--mp-console") {
            mp_console = true;
        } else if (flags[i] == "--mp-snapshot" && has_value) {
            const auto value = flags[++i];
            mp_snapshot = std::filesystem::path(std::u8string(value.begin(), value.end()));
        } else if (flags[i] == "--save-situation" && has_value) {
            save_situation = std::string(flags[++i]);
        } else if (flags[i] == "--spawn" && has_value) {
            const auto value = std::string(flags[++i]);
            start_view = value == "cab" ? 1 : value == "car" ? 2 : value == "above" ? 3 : 0;
        } else if (flags[i] == "--track-control") {
            track_control = true;
        } else if (flags[i] == "--no-sound") {
            options.sound = false;
        } else if (flags[i] == "--sound-offline") {
            options.sound_offline = true;
        } else if (flags[i] == "--capture" && has_value) {
            options.capture_path = std::string(flags[++i]);
        } else if (flags[i] == "--capture-at" && has_value) {
            options.capture_at = static_cast<float>(std::atof(std::string(flags[++i]).c_str()));
        } else if (flags[i] == "--capture-switch" && i + 2 < flags.size()) {
            const auto id = static_cast<std::uint32_t>(std::atoi(std::string(flags[++i]).c_str()));
            const auto state = static_cast<std::uint32_t>(std::atoi(std::string(flags[++i]).c_str()));
            options.capture_switches.emplace_back(id, state);
        } else if (flags[i] == "--capture-menu" && has_value) {
            const int item = std::atoi(std::string(flags[++i]).c_str());
            if (options.capture_menu) {
                options.capture_menu_more.push_back(item);
            } else {
                options.capture_menu = item;
            }
        } else if (flags[i] == "--capture-inspect") {
            options.capture_inspect = true;
        } else if (flags[i] == "--capture-uncouple" && has_value) {
            options.capture_uncouple = static_cast<std::size_t>(std::atoi(std::string(flags[++i]).c_str()));
        } else if (flags[i] == "--capture-board" && has_value) {
            options.capture_board.push_back(static_cast<std::size_t>(std::atoi(std::string(flags[++i]).c_str())));
        } else if (flags[i] == "--fps-limit" && has_value) {
            options.fps_limit = openrts::viewer::clamp_fps_limit(std::atoi(std::string(flags[++i]).c_str()));
            fps_given = true;
        } else if (flags[i] == "--capture-drive" && has_value) {
            options.capture_drive = static_cast<std::size_t>(std::atoi(std::string(flags[++i]).c_str()));
        } else if (flags[i] == "--capture-cab") {
            options.capture_cab = true;
        } else if (flags[i] == "--capture-car" && has_value) {
            options.capture_car = static_cast<std::size_t>(std::atoi(std::string(flags[++i]).c_str()));
        } else if (flags[i] == "--capture-camera" && i + 6 < flags.size()) {
            std::array<float, 6> c{};
            for (auto& v : c) v = static_cast<float>(std::atof(std::string(flags[++i]).c_str()));
            options.capture_camera = c;
        } else if (flags[i] == "--hdr") {
            options.hdr = true;
        } else if (flags[i] == "--reshade-depth" && has_value) {
            options.reshade_depth = std::string(flags[++i]) != "off";
        } else if (flags[i] == "--backend" && has_value) {
            const auto value = std::string(flags[++i]);
            options.backend = openrts::renderer::parse_graphics_backend(value);
            if (!options.backend) std::cerr << "Unknown --backend " << value << " (auto, vulkan, dx12, opengl)." << std::endl;
        } else if (flags[i] == "--volume" && has_value) {
            options.sound_volume = std::clamp(std::stof(std::string(flags[++i])), 0.0f, 1.0f);
        } else if (flags[i] == "--msaa" && has_value) {
            options.msaa = std::stoi(std::string(flags[++i])) > 1 ? 4u : 1u;
        } else if (flags[i] == "--no-batching") {
            options.batching = false;
        } else if (flags[i] == "--no-vsync") {
            options.vsync = false;
        } else if (flags[i] == "--shadow-cache" && has_value) {
            options.shadow_cache = std::string(flags[++i]) != "off";
        } else if (flags[i] == "--cab-shadows" && has_value) {
            options.cab_shadows = std::string(flags[++i]) != "off";
        } else if (flags[i] == "--window-mode" && has_value) {
            const auto value = std::string(flags[++i]);
            options.window_mode = openrts::viewer::parse_window_mode(value);
            if (!options.window_mode) std::cerr << "Unknown --window-mode " << value << " (windowed, borderless, fullscreen)." << std::endl;
        } else if (flags[i] == "--parallel-recording" && has_value) {
            options.parallel_recording = std::string(flags[++i]) != "off";
        } else if (flags[i] == "--paused") {
            options.start_paused = true;
        } else if (flags[i] == "--multi-draw" && has_value) {
            options.multi_draw = std::string(flags[++i]) != "off";
        } else if (flags[i] == "--shadow-settings" && has_value) {
            options.shadow_settings = std::string(flags[++i]);
        } else if (flags[i] == "--headlight-reach" && has_value) {
            options.headlight_reach = static_cast<float>(std::atof(std::string(flags[++i]).c_str()));
        } else if (flags[i] == "--lamp-shadows" && has_value) {
            options.lamp_shadows = std::atoi(std::string(flags[++i]).c_str());
        } else if (flags[i] == "--ground-bounce" && has_value) {
            options.ground_bounce = static_cast<float>(std::atof(std::string(flags[++i]).c_str()));
        } else if (flags[i] == "--lamps-daylight" && has_value) {
            options.lamps_daylight = static_cast<float>(std::atof(std::string(flags[++i]).c_str()));
        } else if (flags[i] == "--light-range" && has_value) {
            options.light_range = static_cast<float>(std::atof(std::string(flags[++i]).c_str()));
        } else if (flags[i] == "--effects" && has_value) {
            options.effects = std::string(flags[++i]);
        } else if (flags[i] == "--experimental") {
            options.shading = openrts::renderer::ShadingPath::experimental;
        } else if (flags[i] == "--time" && has_value) {
            options.time_of_day = openrts::formats::parse_time_of_day(std::string(flags[++i]));
            time_given = true;
        } else if (flags[i] == "--speed" && has_value) {
            options.time_speed = std::stof(std::string(flags[++i]));
        } else if (flags[i] == "--season" && has_value) {
            const auto value = flags[++i];
            constexpr std::string_view seasons[] = {"spring", "summer", "autumn", "winter"};
            const auto it = std::find(std::begin(seasons), std::end(seasons), value);
            if (it == std::end(seasons)) {
                std::cerr << "Unknown season: " << value << '\n';
                return exit_code::content_failed;
            }
            options.season = static_cast<openrts::renderer::rts::Season>(it - std::begin(seasons));
            season_given = true;
        } else if (flags[i] == "--weather" && has_value) {
            const auto value = flags[++i];
            constexpr std::string_view weathers[] = {"clear", "snow", "rain"};
            const auto it = std::find(std::begin(weathers), std::end(weathers), value);
            if (it == std::end(weathers)) {
                std::cerr << "Unknown weather: " << value << '\n';
                return exit_code::content_failed;
            }
            options.weather = static_cast<openrts::renderer::rts::Weather>(it - std::begin(weathers));
            weather_given = true;
        } else {
            std::cerr << "Unknown viewer option: " << flags[i] << '\n';
            return exit_code::content_failed;
        }
    }

    // A server (--mp-connect) names the route, its season and weather at the
    // login: the route it plays is opened (a --route given is only checked).
    std::shared_ptr<openrts::net::RtsConnection> connection;
    std::string server_route;
    if (!mp_connect.empty()) {
        connection = std::make_shared<openrts::net::RtsConnection>();
        const auto reply = login_server(*connection, mp_connect, mp_nick, mp_console);
        if (!reply) return exit_code::network_failed;
        server_route = openrts::net::to_utf8(reply->route);
        if (!season_given && reply->season < 4) options.season = static_cast<openrts::renderer::rts::Season>(reply->season);
        if (!weather_given && reply->weather < 3) options.weather = static_cast<openrts::renderer::rts::Weather>(reply->weather);
        const auto given = fs::path(std::u8string(path.begin(), path.end())).filename().u8string();
        if (path.empty()) {
            mode = "--route";
        } else if (openrts::core::fold_case(std::string(given.begin(), given.end())) != openrts::core::fold_case(server_route)) {
            std::cout << "net: the server plays route " << server_route << ", not " << path << ": " << server_route
                      << " is opened" << std::endl;
        }
        path = server_route;
    }

    if (!fps_given) {
        if (const auto fps = openrts::viewer::remembered("fps_limit")) options.fps_limit = openrts::viewer::clamp_fps_limit(std::atoi(fps->c_str()));
    }
    if (!options.window_mode) {
        if (const auto remembered_mode = openrts::viewer::remembered("window_mode"))
            options.window_mode = openrts::viewer::parse_window_mode(*remembered_mode);
    }

    if (mode != "--shape" && mode != "--route") return exit_code::success;
    if (!root && options.view && !options.view->root.empty()) root = fs::path(std::u8string(options.view->root.begin(), options.view->root.end()));
    auto target = locate(path, root, mode == "--route" ? "ROUTES" : "GLOBAL/SHAPES");
    if (!target) {
        std::cerr << "Cannot locate " << path << " (give an existing path, or --root with a route name).\n";
        return exit_code::content_failed;
    }

    // Content VFS: packs, folders, patches, mods (openrts.ini), plus --mount.
    auto vfs = std::make_shared<openrts::core::Vfs>();
    const auto config = target->installation ? openrts::core::load_installation_config(target->root)
                                             : openrts::core::InstallationConfig{};
    if (target->installation) {
        openrts::core::mount_installation(*vfs, target->root, config);
    } else {
        vfs->mount_directory(target->root, {.layer = openrts::core::VfsLayer::folder});
    }
    int mount_order = 1000;
    for (const auto& extra : mounts) {
        const openrts::core::VfsMountOptions mount{.layer = openrts::core::VfsLayer::mod, .priority = mount_order++};
        if (fs::is_directory(extra)) {
            vfs->mount_directory(extra, mount);
        } else {
            vfs->mount_zip(extra, mount);
        }
    }
    vfs->build_index();
    // A server names its route in lower case: the folder's own spelling.
    if (connection) {
        if (const auto folder = openrts::core::VirtualPath::parse("ROUTES")) {
            for (const auto& entry : vfs->list(*folder))
                if (entry.directory && entry.path.key() == target->path.key()) target->path = entry.path;
        }
    }
    const auto stats = vfs->stats();
    std::cout << "content: " << target->root.string() << " -> " << target->path.str() << " (" << stats.sources
              << " sources, " << stats.files << " files)\n";
    for (const auto& source : vfs->sources()) {
        std::cout << "  " << openrts::core::to_string(source.layer) << ": " << source.name << " (" << source.files
                  << " files)" << (source.error.empty() ? "" : " error: " + source.error) << '\n';
    }

    const bool watching = hot_reload && config.hot_reload.value_or(true);
    if (watching) {
        vfs->start_watching({.reconcile_interval = std::chrono::seconds(config.reconcile_seconds)});
    }

    // An RTS startup snapshot (docs/net.md): from a file (--mp-snapshot) or
    // from a live server (--mp-connect, then the scene watches its game). Its
    // time of day here, the rest when the route scene is up.
    std::shared_ptr<const openrts::net::Snapshot> snapshot;
    std::vector<openrts::net::ReceivedPackage> early;
    std::optional<openrts::net::Bytes> snapshot_bytes;
    std::string snapshot_source;
    if (connection) {
        if (!vfs->is_directory(target->path)) {
            std::cerr << "The server plays route " << server_route << ": it is not in " << target->root.string() << std::endl;
            report_status("no-route " + server_route);
            return exit_code::content_failed;
        }
        snapshot_bytes = wait_for_world(*connection, mp_connect, mp_nick, server_route, early, mp_console);
        if (!snapshot_bytes) return exit_code::network_failed;
        snapshot_source = mp_connect;
        // The world as the server sent it, kept for opening it again without
        // the network (--mp-snapshot): the last one in the settings folder
        // always, and where --mp-save-snapshot says.
        const auto last = openrts::viewer::user_data_directory() / "last_server_snapshot.bin";
        for (const auto& file : {last, mp_save}) {
            if (file.empty()) continue;
            std::ofstream(file, std::ios::binary)
                .write(reinterpret_cast<const char*>(snapshot_bytes->data()), static_cast<std::streamsize>(snapshot_bytes->size()));
            std::cout << "net: snapshot saved to " << file.string() << std::endl;
        }
    } else if (!mp_snapshot.empty()) {
        std::ifstream in(mp_snapshot, std::ios::binary);
        if (!in) {
            std::cerr << "Cannot read " << mp_snapshot.string() << '\n';
            return exit_code::content_failed;
        }
        snapshot_bytes = openrts::net::Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        snapshot_source = mp_snapshot.string();
    }
    if (snapshot_bytes) {
        auto read = openrts::net::read_snapshot(*snapshot_bytes);
        for (const auto& problem : read.problems) std::cout << "snapshot: " << problem << '\n';
        std::cout << "snapshot: " << snapshot_source << ": " << read.snapshot.junctions.size() << " junctions, "
                  << read.snapshot.signals.size() << " signal heads, " << read.snapshot.trains.size() << " trains"
                  << std::endl;
        if (read.snapshot.time && !time_given) {
            options.time_of_day = static_cast<std::int32_t>(*read.snapshot.time);
            std::cout << "snapshot: time of day " << options.time_of_day / 3600 << ':' << options.time_of_day / 60 % 60 / 10
                      << options.time_of_day / 60 % 10 << std::endl;
        }
        snapshot = std::make_shared<const openrts::net::Snapshot>(std::move(read.snapshot));
    }

    const openrts::viewer::SceneContent content{
        .vfs = vfs, .watching = watching, .consist = consist, .path = train_path, .track_control = track_control,
        .start_state = start_state, .situation = situation, .start_view = start_view,
        .save_situation = save_situation, .root = target->root, .snapshot = snapshot, .connection = connection,
        .early = std::move(early), .console = mp_console};
    auto loaded = mode == "--shape" ? openrts::viewer::load_shape_scene(content, target->path)
                                    : openrts::viewer::load_route_scene(content, target->path);

    const auto root_text = target->root.u8string();
    options.identity = {
        .root = std::string(root_text.begin(), root_text.end()),
        .mode = std::string(mode.substr(2)),
        .content = target->path.str(),
    };
    if (!loaded.scene) {
        std::cerr << loaded.error << '\n';
        return exit_code::content_failed;
    }
    if (connection) report_status("playing");
    return openrts::viewer::run_window_viewer(*loaded.scene, options);
}
#endif

} // namespace

int main(const int argc, char* argv[]) {
    openrts::viewer::install_crash_report();
    print_banner();

#ifdef OPENRTS_HAS_WGPU
#ifdef _WIN32
    // Every argument as UTF-8, as the rest of the program takes text. The C
    // runtime's argv is in the system code page: a Cyrillic path name, consist,
    // nick or folder did not match the content (only --consist and --mp-nick
    // were converted before 2026-11-06; the menu's paths of Kuban_RTS were
    // not found).
    std::vector<std::string> utf8_args;
    {
        int count = 0;
        if (wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count)) {
            for (int i = 1; i < count; ++i) {
                const int length = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
                std::string text(static_cast<std::size_t>((std::max)(length, 1)), '\0');
                WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, text.data(), length, nullptr, nullptr);
                text.resize(text.empty() ? 0 : text.size() - 1);
                utf8_args.push_back(std::move(text));
            }
            LocalFree(wide);
        }
    }
    static_cast<void>(argc);
    static_cast<void>(argv);
    const std::vector<std::string_view> args(utf8_args.begin(), utf8_args.end());
#else
    const std::vector<std::string_view> args(argv + 1, argv + argc);
#endif
    if (args.size() == 2 && args[0] == "--ace") return run_ace_upload_check(args[1]);
    // `--window --view <file|name|last>` reopens a saved debug view.
    if (args.size() >= 3 && args[0] == "--window" && args[1] == "--view") {
        return run_window("--view", {}, {args.begin() + 1, args.end()});
    }
    // `--window ... --mp-connect <address> ...` without --route: the route is
    // the server's (the flags in any order).
    if (args.size() >= 3 && args[0] == "--window" && args[1] != "--route" && args[1] != "--shape" &&
        std::find(args.begin(), args.end(), "--mp-connect") != args.end()) {
        return run_window("--route", {}, {args.begin() + 1, args.end()});
    }
    if (args.size() >= 3 && args[0] == "--window") {
        return run_window(args[1], args[2], {args.begin() + 3, args.end()});
    }
#else
    static_cast<void>(argc);
    static_cast<void>(argv);
#endif
    return 0;
}
