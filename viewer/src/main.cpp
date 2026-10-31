#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <openrts/assets/assets.hpp>
#include <openrts/renderer/renderer.hpp>

#include "exit_codes.hpp"

#ifdef OPENRTS_HAS_WGPU
#include <openrts/core/installation.hpp>
#include <openrts/core/vfs.hpp>
#include <openrts/formats/ace_file.hpp>
#include <openrts/formats/environment.hpp>
#include <openrts/renderer/webgpu_context.hpp>
#include <openrts/renderer/webgpu_texture.hpp>

#include "debug_view.hpp"
#include "viewer_scene.hpp"
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
// extra mod-layer source), `--no-hot-reload`, `--probe` (ride the track from
// the route start), `--experimental`, `--no-vsync`, `--no-batching`, `--no-sound`, `--volume <0..1>` (default 0.5),
// `--time HH:MM[:SS]`, `--speed <multiplier>`,
// `--season spring|summer|autumn|winter` and `--weather clear|snow|rain`.
int run_window(std::string_view mode, std::string_view path, const std::vector<std::string_view>& flags) {
    openrts::viewer::WindowOptions options;
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
    bool probe = false;
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
        } else if (flags[i] == "--probe") {
            probe = true;
        } else if (flags[i] == "--no-sound") {
            options.sound = false;
        } else if (flags[i] == "--volume" && has_value) {
            options.sound_volume = std::clamp(std::stof(std::string(flags[++i])), 0.0f, 1.0f);
        } else if (flags[i] == "--no-batching") {
            options.batching = false;
        } else if (flags[i] == "--no-vsync") {
            options.vsync = false;
        } else if (flags[i] == "--experimental") {
            options.shading = openrts::renderer::ShadingPath::experimental;
        } else if (flags[i] == "--time" && has_value) {
            options.time_of_day = openrts::formats::parse_time_of_day(std::string(flags[++i]));
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
        } else if (flags[i] == "--weather" && has_value) {
            const auto value = flags[++i];
            constexpr std::string_view weathers[] = {"clear", "snow", "rain"};
            const auto it = std::find(std::begin(weathers), std::end(weathers), value);
            if (it == std::end(weathers)) {
                std::cerr << "Unknown weather: " << value << '\n';
                return exit_code::content_failed;
            }
            options.weather = static_cast<openrts::renderer::rts::Weather>(it - std::begin(weathers));
        } else {
            std::cerr << "Unknown viewer option: " << flags[i] << '\n';
            return exit_code::content_failed;
        }
    }

    if (mode != "--shape" && mode != "--route") return exit_code::success;
    if (!root && options.view && !options.view->root.empty()) root = fs::path(std::u8string(options.view->root.begin(), options.view->root.end()));
    const auto target = locate(path, root, mode == "--route" ? "ROUTES" : "GLOBAL/SHAPES");
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

    const openrts::viewer::SceneContent content{.vfs = vfs, .watching = watching, .probe_at_start = probe};
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
    return openrts::viewer::run_window_viewer(*loaded.scene, options);
}
#endif

} // namespace

int main(const int argc, char* argv[]) {
    print_banner();

#ifdef OPENRTS_HAS_WGPU
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    if (args.size() == 2 && args[0] == "--ace") return run_ace_upload_check(args[1]);
    // `--window --view <file|name|last>` reopens a saved debug view.
    if (args.size() >= 3 && args[0] == "--window" && args[1] == "--view") {
        return run_window("--view", {}, {args.begin() + 1, args.end()});
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
