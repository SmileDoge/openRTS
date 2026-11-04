#include "debug_view.hpp"

#include "viewer_state.hpp"

#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>

namespace openrts::viewer {
namespace {

[[nodiscard]] std::string trim(std::string text) {
    const auto first = text.find_first_not_of(" \t\r");
    const auto last = text.find_last_not_of(" \t\r");
    return first == std::string::npos ? std::string{} : text.substr(first, last - first + 1);
}

} // namespace

std::optional<DebugView> load_debug_view(const std::filesystem::path& file, std::string& error) {
    std::ifstream input(file, std::ios::binary);
    if (!input) {
        error = "cannot open " + file.string();
        return std::nullopt;
    }
    std::map<std::string, std::string> values;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto equals = line.find('=');
        if (equals == std::string::npos) continue;
        values[trim(line.substr(0, equals))] = trim(line.substr(equals + 1));
    }
    const auto text = [&](const char* key) { return values.contains(key) ? values[key] : std::string{}; };
    const auto number = [&](const char* key) {
        try {
            return values.contains(key) ? std::stod(values[key]) : 0.0;
        } catch (...) {
            return 0.0;
        }
    };
    DebugView view{
        .root = text("root"),
        .mode = text("mode"),
        .content = text("content"),
        .tile_x = static_cast<std::int32_t>(number("tile_x")),
        .tile_z = static_cast<std::int32_t>(number("tile_z")),
        .x = number("x"),
        .y = number("y"),
        .z = number("z"),
        .target_x = number("target_x"),
        .target_y = number("target_y"),
        .target_z = number("target_z"),
        .yaw = static_cast<float>(number("yaw")),
        .pitch = static_cast<float>(number("pitch")),
        .distance = static_cast<float>(number("distance")),
        .fly = number("fly") != 0.0,
        .time_of_day = static_cast<std::int32_t>(number("time_of_day")),
        .season = static_cast<std::int32_t>(number("season")),
        .weather = static_cast<std::int32_t>(number("weather")),
        .experimental = number("experimental") != 0.0,
    };
    if (view.mode.empty() || view.content.empty()) {
        error = file.string() + ": mode or content missing";
        return std::nullopt;
    }
    return view;
}

bool save_debug_view(const std::filesystem::path& file, const DebugView& view, std::string& error) {
    std::error_code ignored;
    std::filesystem::create_directories(file.parent_path(), ignored);
    std::ostringstream out;
    out << std::setprecision(10);
    out << "# openRTS viewer debug view: openrts_viewer --window --view " << file.filename().string() << '\n'
        << "root = " << view.root << '\n'
        << "mode = " << view.mode << '\n'
        << "content = " << view.content << '\n'
        << "tile_x = " << view.tile_x << '\n'
        << "tile_z = " << view.tile_z << '\n'
        << "x = " << view.x << '\n'
        << "y = " << view.y << '\n'
        << "z = " << view.z << '\n'
        << "target_x = " << view.target_x << '\n'
        << "target_y = " << view.target_y << '\n'
        << "target_z = " << view.target_z << '\n'
        << "yaw = " << view.yaw << '\n'
        << "pitch = " << view.pitch << '\n'
        << "distance = " << view.distance << '\n'
        << "fly = " << (view.fly ? 1 : 0) << '\n'
        << "time_of_day = " << view.time_of_day << '\n'
        << "season = " << view.season << '\n'
        << "weather = " << view.weather << '\n'
        << "experimental = " << (view.experimental ? 1 : 0) << '\n';
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    output << out.str();
    if (!output) {
        error = "cannot write " + file.string();
        return false;
    }
    return true;
}

std::filesystem::path debug_view_directory() {
    return user_data_directory() / "debug_views";
}

std::filesystem::path resolve_debug_view(const std::string& argument) {
    if (argument == "last") return debug_view_directory() / "last.view";
    std::filesystem::path path(std::u8string(argument.begin(), argument.end()));
    if (path.has_parent_path() || std::filesystem::exists(path)) return path;
    // A bare name refers to debug_views/<name>(.view).
    auto inside = debug_view_directory() / path;
    if (!inside.has_extension()) inside += ".view";
    return inside;
}

} // namespace openrts::viewer
