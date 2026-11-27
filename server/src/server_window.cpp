#include "server_window.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include <webgpu/wgpu.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
#include <GLFW/glfw3.h>
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_wgpu.h>

#include <openrts/net/placement.hpp>
#include <openrts/core/core.hpp>
#include <openrts/core/user_data.hpp>
#include <openrts/core/virtual_path.hpp>
#include <openrts/net/wire.hpp>
#include <openrts/renderer/webgpu_context.hpp>
#include <openrts/server/console.hpp>
#include <openrts/simulation/track_traveller.hpp>
#include <openrts/trackmap/track_map.hpp>
#include <openrts/ui/fonts.hpp>
#include <openrts/ui/text.hpp>

namespace openrts::server {
namespace {

namespace fs = std::filesystem;
using ui::tr;
using ui::trf;

constexpr std::uint16_t kind_static = 0, kind_player = 1, kind_traffic = 2;

[[nodiscard]] std::string clock_text(const double seconds) {
    const auto s = static_cast<long long>(seconds);
    char text[16];
    std::snprintf(text, sizeof text, "%02lld:%02lld:%02lld", s / 3600 % 24, s / 60 % 60, s % 60);
    return text;
}

[[nodiscard]] std::string stamp() {
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

// The distance from (px, py) to the segment a-b; `t` where along it (0..1).
[[nodiscard]] double segment_distance(const double px, const double py, const double ax, const double ay, const double bx, const double by,
                                      double& t) {
    const double dx = bx - ax, dy = by - ay;
    const double span = dx * dx + dy * dy;
    t = span > 1e-12 ? std::clamp(((px - ax) * dx + (py - ay) * dy) / span, 0.0, 1.0) : 0.0;
    return std::hypot(px - (ax + dx * t), py - (ay + dy * t));
}

[[nodiscard]] std::string utf8_path(const fs::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

[[nodiscard]] fs::path path_of(const char* utf8) {
    const std::string text(utf8);
    return fs::path(std::u8string(text.begin(), text.end()));
}

// ---- the window's picture -------------------------------------------------------

#ifdef _WIN32
// The swap chain of the window (a plain BGRA8 / RGBA8 target, Fifo).
class Surface {
public:
    Surface(const renderer::WgpuDeviceContext& context, GLFWwindow* window) : device_(context.device()) {
        const WGPUSurfaceSourceWindowsHWND source{
            .chain = {.sType = WGPUSType_SurfaceSourceWindowsHWND},
            .hinstance = GetModuleHandle(nullptr),
            .hwnd = glfwGetWin32Window(window),
        };
        const WGPUSurfaceDescriptor descriptor{.nextInChain = &source.chain};
        surface_ = wgpuInstanceCreateSurface(context.instance(), &descriptor);
        if (!surface_) return;
        WGPUSurfaceCapabilities capabilities{};
        wgpuSurfaceGetCapabilities(surface_, context.adapter(), &capabilities);
        if (capabilities.formatCount > 0 && capabilities.alphaModeCount > 0) {
            WGPUTextureFormat format = capabilities.formats[0];
            for (const auto wanted : {WGPUTextureFormat_BGRA8Unorm, WGPUTextureFormat_RGBA8Unorm}) {
                for (std::size_t i = 0; i < capabilities.formatCount; ++i) {
                    if (capabilities.formats[i] == wanted) format = wanted;
                }
                if (format == wanted) break;
            }
            int width = 0, height = 0;
            glfwGetFramebufferSize(window, &width, &height);
            config_ = {.device = device_,
                       .format = format,
                       .usage = WGPUTextureUsage_RenderAttachment,
                       .width = static_cast<std::uint32_t>((std::max)(width, 1)),
                       .height = static_cast<std::uint32_t>((std::max)(height, 1)),
                       .alphaMode = capabilities.alphaModes[0],
                       .presentMode = WGPUPresentMode_Fifo};
            wgpuSurfaceConfigure(surface_, &config_);
            configured_ = true;
        }
        wgpuSurfaceCapabilitiesFreeMembers(capabilities);
    }
    ~Surface() {
        if (configured_) wgpuSurfaceUnconfigure(surface_);
        if (surface_) wgpuSurfaceRelease(surface_);
    }
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;

    [[nodiscard]] bool ok() const noexcept { return configured_; }
    [[nodiscard]] WGPUTextureFormat format() const noexcept { return config_.format; }
    [[nodiscard]] std::uint32_t width() const noexcept { return config_.width; }
    [[nodiscard]] std::uint32_t height() const noexcept { return config_.height; }
    void resize(const int width, const int height) {
        if (width <= 0 || height <= 0) return;
        if (config_.width == static_cast<std::uint32_t>(width) && config_.height == static_cast<std::uint32_t>(height)) return;
        config_.width = static_cast<std::uint32_t>(width);
        config_.height = static_cast<std::uint32_t>(height);
        wgpuSurfaceConfigure(surface_, &config_);
    }
    [[nodiscard]] WGPUTexture acquire() const {
        WGPUSurfaceTexture frame{};
        wgpuSurfaceGetCurrentTexture(surface_, &frame);
        const bool usable = frame.status == WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal ||
                            frame.status == WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal;
        return usable ? frame.texture : nullptr;
    }
    void present() const { wgpuSurfacePresent(surface_); }

private:
    WGPUDevice device_ = nullptr;
    WGPUSurface surface_ = nullptr;
    WGPUSurfaceConfiguration config_{};
    bool configured_ = false;
};

// ImGui's frame into `target`, cleared first.
void render_frame(const WGPUDevice device, const WGPUTexture target, const WGPUCommandEncoder encoder) {
    const auto view = wgpuTextureCreateView(target, nullptr);
    const WGPURenderPassColorAttachment colour{
        .view = view,
        .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
        .loadOp = WGPULoadOp_Clear,
        .storeOp = WGPUStoreOp_Store,
        .clearValue = {.r = 0.09, .g = 0.1, .b = 0.12, .a = 1.0},
    };
    const WGPURenderPassDescriptor descriptor{.colorAttachmentCount = 1, .colorAttachments = &colour};
    const auto pass = wgpuCommandEncoderBeginRenderPass(encoder, &descriptor);
    ImGui_ImplWGPU_RenderDrawData(ImGui::GetDrawData(), pass);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
    wgpuTextureViewRelease(view);
    (void)device;
}

// The window's picture to a binary PPM (--capture): drawn into a texture of
// its own and read back.
bool capture_frame(const renderer::WgpuDeviceContext& context, const WGPUTextureFormat format, const std::uint32_t width,
                   const std::uint32_t height, const fs::path& file) {
    const auto device = context.device();
    const WGPUTextureDescriptor texture_descriptor{
        .usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc,
        .dimension = WGPUTextureDimension_2D,
        .size = {.width = width, .height = height, .depthOrArrayLayers = 1},
        .format = format,
        .mipLevelCount = 1,
        .sampleCount = 1,
    };
    const auto texture = wgpuDeviceCreateTexture(device, &texture_descriptor);
    if (!texture) return false;
    const std::uint32_t row_bytes = (width * 4 + 255) / 256 * 256;
    const WGPUBufferDescriptor buffer_descriptor{.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst,
                                                 .size = static_cast<std::uint64_t>(row_bytes) * height};
    const auto buffer = wgpuDeviceCreateBuffer(device, &buffer_descriptor);
    const auto encoder = wgpuDeviceCreateCommandEncoder(device, nullptr);
    render_frame(device, texture, encoder);
    const WGPUTexelCopyTextureInfo source{.texture = texture};
    const WGPUTexelCopyBufferInfo target{.layout = {.bytesPerRow = row_bytes, .rowsPerImage = height}, .buffer = buffer};
    const WGPUExtent3D size{.width = width, .height = height, .depthOrArrayLayers = 1};
    wgpuCommandEncoderCopyTextureToBuffer(encoder, &source, &target, &size);
    const auto commands = wgpuCommandEncoderFinish(encoder, nullptr);
    const auto queue = wgpuDeviceGetQueue(device);
    wgpuQueueSubmit(queue, 1, &commands);
    wgpuCommandBufferRelease(commands);
    wgpuCommandEncoderRelease(encoder);
    bool mapped = false, done = false;
    struct State {
        bool* mapped;
        bool* done;
    } state{&mapped, &done};
    const WGPUBufferMapCallbackInfo callback{
        .mode = WGPUCallbackMode_AllowProcessEvents,
        .callback = [](const WGPUMapAsyncStatus status, WGPUStringView, void* data, void*) {
            auto* s = static_cast<State*>(data);
            *s->mapped = status == WGPUMapAsyncStatus_Success;
            *s->done = true;
        },
        .userdata1 = &state,
    };
    wgpuBufferMapAsync(buffer, WGPUMapMode_Read, 0, static_cast<std::size_t>(row_bytes) * height, callback);
    for (int i = 0; i < 1000 && !done; ++i) {
        wgpuDevicePoll(device, true, nullptr);
        wgpuInstanceProcessEvents(context.instance());
    }
    bool written = false;
    if (mapped) {
        const auto* bytes = static_cast<const std::uint8_t*>(wgpuBufferGetConstMappedRange(buffer, 0, static_cast<std::size_t>(row_bytes) * height));
        std::ofstream out(file, std::ios::binary);
        if (bytes && out) {
            out << "P6\n" << width << ' ' << height << "\n255\n";
            const bool bgra = format == WGPUTextureFormat_BGRA8Unorm || format == WGPUTextureFormat_BGRA8UnormSrgb;
            std::vector<char> row(static_cast<std::size_t>(width) * 3);
            for (std::uint32_t y = 0; y < height; ++y) {
                const auto* line = bytes + static_cast<std::size_t>(y) * row_bytes;
                for (std::uint32_t x = 0; x < width; ++x) {
                    row[x * 3 + 0] = static_cast<char>(line[x * 4 + (bgra ? 2 : 0)]);
                    row[x * 3 + 1] = static_cast<char>(line[x * 4 + 1]);
                    row[x * 3 + 2] = static_cast<char>(line[x * 4 + (bgra ? 0 : 2)]);
                }
                out.write(row.data(), static_cast<std::streamsize>(row.size()));
            }
            written = static_cast<bool>(out);
        }
        wgpuBufferUnmap(buffer);
    }
    wgpuQueueRelease(queue);
    wgpuBufferRelease(buffer);
    wgpuTextureRelease(texture);
    return written;
}
#endif

// ---- the start form (rts_server.exe's: route, season, weather, time, port) ----------

struct StartForm {
    char root[512]{};
    char data[512]{};
    std::vector<std::string> routes;
    int route = -1;
    int season = 1;
    int weather = 0;
    char time[16] = "12:00";
    int port = 800;
    bool admit = true;
    char password[64]{}; // the dispatcher's, for a console from another computer
    int places = 16;      // ServerOptions::max_players, 0 no limit
    bool weather_dynamic = false; // openRTS's dynamic weather from the start
    bool discord = false; // the players' Discord "Join"
    char address[128]{};  // the address it hands over (empty: the one each connected to)
    bool fast_positions = true; // ServerOptions::fast_positions
    std::string error;
    int loading = 0; // frames since "Start": the load runs after "Loading" is shown

    void list_routes() {
        routes.clear();
        std::error_code failed;
        for (const auto& entry : fs::directory_iterator(path_of(root) / "ROUTES", failed)) {
            if (entry.is_directory(failed)) routes.push_back(utf8_path(entry.path().filename()));
        }
        std::sort(routes.begin(), routes.end());
    }
    void pick_route(const std::string& name) {
        for (std::size_t i = 0; i < routes.size(); ++i) {
            if (core::fold_case(routes[i]) == core::fold_case(name)) route = static_cast<int>(i);
        }
    }

    // The form as it was last started (server_window.ini in the server's
    // folder): key=value lines.
    void load(const fs::path& file) {
        std::ifstream in(file);
        std::string line, route_name;
        while (std::getline(in, line)) {
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            const auto key = line.substr(0, eq);
            const auto value = line.substr(eq + 1);
            try {
                if (key == "root") std::snprintf(root, sizeof root, "%s", value.c_str());
                else if (key == "route") route_name = value;
                else if (key == "season") season = std::clamp(std::stoi(value), 0, 3);
                else if (key == "weather") weather = std::clamp(std::stoi(value), 0, 3);
                else if (key == "time") std::snprintf(time, sizeof time, "%s", value.c_str());
                else if (key == "port") port = std::clamp(std::stoi(value), 0, 65535);
                else if (key == "admit") admit = value == "1";
                else if (key == "dispatcher_password") std::snprintf(password, sizeof password, "%s", value.c_str());
                else if (key == "discord_join") discord = value == "1";
                else if (key == "places") places = std::clamp(std::stoi(value), 0, 999);
                else if (key == "weather_dynamic") weather_dynamic = value == "1";
                else if (key == "public_address") std::snprintf(address, sizeof address, "%s", value.c_str());
                else if (key == "fast_positions") fast_positions = value == "1";
            } catch (const std::exception&) {
                // a number spoilt by hand: the default kept
            }
        }
        list_routes();
        pick_route(route_name);
    }
    void save(const fs::path& file) const {
        std::ofstream out(file);
        out << "root=" << root << "\nroute=" << (route >= 0 ? routes[static_cast<std::size_t>(route)] : std::string()) << "\nseason=" << season
            << "\nweather=" << weather << "\ntime=" << time << "\nport=" << port << "\nadmit=" << (admit ? 1 : 0)
            << "\ndispatcher_password=" << password << "\nplaces=" << places << "\ndiscord_join=" << (discord ? 1 : 0) << "\npublic_address=" << address
            << "\nweather_dynamic=" << (weather_dynamic ? 1 : 0) << "\nfast_positions=" << (fast_positions ? 1 : 0) << '\n';
    }
};

// ---- the console: the map and the dispatcher's tabs ------------------------------------

// A station route as picked in a list: the route (in StationRoutes::routes())
// and whether it is run from its end.
struct RouteChoice {
    std::size_t route{};
    bool reverse = false;
    bool operator==(const RouteChoice&) const = default;
};

class Console {
public:
    explicit Console(ServerSession& session) : s_(session), map_(session.route().network(), origin_of(session.route().network())) {
        auto& route = s_.route();
        // The signals' names by signal object (the route database's, by TDB item).
        std::map<std::uint32_t, std::string> by_item;
        for (const auto& [name, item] : route.signal_names()) by_item.emplace(item, name);
        if (const auto* signals = route.signals()) {
            const auto& objects = signals->objects();
            for (std::uint32_t o = 0; o < objects.size(); ++o) {
                for (const auto h : objects[o].heads) {
                    if (const auto it = by_item.find(signals->heads()[h].track_item); it != by_item.end()) {
                        names_[o] = it->second;
                        break;
                    }
                }
            }
            std::set<std::string> types;
            for (const auto& head : signals->heads()) {
                if (head.type) types.insert(head.type->name);
            }
            types_.assign(types.begin(), types.end());
        }
        map_.signal_name = [this](const std::uint32_t object) {
            const auto it = names_.find(object);
            return it == names_.end() ? std::string() : it->second;
        };
        map_.signal_notes = [this](const std::uint32_t object) {
            std::vector<std::string> notes;
            if (const auto item = first_item(object)) notes.push_back(s_.game().signal_report(*item));
            return notes;
        };
        consists_ = route.consist_names();
        const auto stations = route.station_routes().stations();
        for (const auto& [name, count] : stations) stations_.push_back(name);
        // The map on the first path's start, else the middle of the first track.
        std::optional<simulation::TrackPoint> at;
        if (!route.paths().empty()) {
            if (const auto front = route.path_front(route.paths().front(), 0.0)) at = front->pose();
        }
        if (!at) {
            if (const auto* node = route.network().node(1)) at = route.network().point(1, node->length * 0.5);
        }
        if (at) map_.centre_on(*at, 0.5);
        std::snprintf(time_, sizeof time_, "%s", clock_text(s_.game().time_of_day()).substr(0, 5).c_str());
    }

    void log(std::string line) {
        log_.push_back(stamp() + ' ' + std::move(line));
        std::cout << log_.back() << std::endl; // the console behind the window too
        while (log_.size() > 3000) log_.pop_front();
        log_scroll_ = true;
    }

    // A console line, its answer into the log line by line. "quit" stops
    // the server as its button does.
    void run(const std::string& line) {
        log("> " + line);
        bool quit = false;
        const auto answer = s_.command(line, quit);
        std::size_t start = 0;
        while (start < answer.size()) {
            auto end = answer.find('\n', start);
            if (end == std::string::npos) end = answer.size();
            if (end > start) log(answer.substr(start, end - start));
            start = end + 1;
        }
        if (quit) quit_ = true;
    }

    // The whole window; false when the operator stops the server.
    bool draw() {
        for (auto& line : s_.game().take_log()) log(std::move(line));
        routes_tab_open_ = false;
        hovered_.reset();
        // A train gone (removed here, by a command or a console) is no
        // longer the selected one.
        if (selected_train_) {
            const auto trains = s_.game().trains();
            if (std::none_of(trains.begin(), trains.end(), [&](const TrainSummary& t) { return t.id == selected_train_; })) selected_train_ = 0;
        }
        const auto& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(io.DisplaySize);
        bool running = !quit_;
        ImGui::Begin("##server", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
        top_bar(running);
        ImGui::BeginChild("panel", ImVec2(460.0f, 0.0f), ImGuiChildFlags_Borders);
        // Setting a route: the panel greyed under what is being asked.
        const bool routing = routing_.active;
        if (routing) {
            routing_banner();
            ImGui::BeginDisabled();
        }
        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem(tr("Trains"))) {
                trains_tab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(tr("Station routes"))) {
                routes_tab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(tr("Signals"))) {
                signals_tab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(tr("Players"))) {
                players_tab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(tr("Time and weather"))) {
                clock_tab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(tr("Log"))) {
                log_tab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        if (routing) ImGui::EndDisabled();
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("map", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        draw_map();
        ImGui::EndChild();
        ImGui::End();
        return running;
    }

private:
    [[nodiscard]] static formats::RoutePosition origin_of(const simulation::TrackNetwork& network) {
        for (std::uint32_t n = 1; n < network.node_count(); ++n) {
            if (const auto at = network.point(n, 0.0)) return {.tile_x = at->tile_x, .tile_z = at->tile_z, .x = at->x, .y = at->y, .z = at->z};
        }
        return {};
    }

    // A signal object's first TDB item (what the game's signal commands take).
    [[nodiscard]] std::optional<std::uint32_t> first_item(const std::uint32_t object) const {
        const auto* signals = s_.route().signals();
        if (!signals || object >= signals->objects().size() || signals->objects()[object].heads.empty()) return std::nullopt;
        return signals->heads()[signals->objects()[object].heads.front()].track_item;
    }

    void report(const bool ok, const std::string& what, const std::string& error) {
        set_status(ok ? what : tr("error: ") + error, !ok);
        log(status_);
    }

    // The line under the top bar: what was last done, for a few seconds
    // (an error longer).
    void set_status(std::string text, const bool error = false) {
        status_ = std::move(text);
        status_error_ = error;
        status_until_ = ImGui::GetTime() + (error ? 10.0 : 5.0);
    }

    void top_bar(bool& running) {
        auto& game = s_.game();
        ImGui::Text("%s", trf("Route %s, port %u", s_.route().folder().c_str(), static_cast<unsigned>(s_.port())).c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::Text("%s", trf("Time %s", clock_text(game.time_of_day()).c_str()).c_str());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f);
        ImGui::InputText("##time", time_, sizeof time_);
        ImGui::SameLine();
        if (ImGui::Button(tr("Set the time"))) {
            if (const auto t = parse_time_of_day(time_)) {
                game.set_time(*t);
                report(true, trf("time set to %s", clock_text(*t).c_str()), {});
            } else {
                report(false, {}, tr("the time is HH:MM or HH:MM:SS"));
            }
        }
        ImGui::SameLine();
        bool paused = game.paused();
        if (ImGui::Checkbox(tr("Pause"), &paused)) game.set_paused(paused);
        // The weather while the game runs (openRTS's players and consoles):
        // one that stands, or openRTS's dynamic one.
        {
            auto weather = game.weather();
            ImGui::SameLine();
            const char* weathers[] = {tr("clear"), tr("snow"), tr("rain"), tr("thunderstorm")};
            int kind = weather.weather < 4 ? weather.weather : 0;
            ImGui::SetNextItemWidth(110.0f);
            ImGui::BeginDisabled(weather.dynamic != 0);
            if (ImGui::Combo("##weather", &kind, weathers, 4)) {
                weather.weather = static_cast<std::uint8_t>(kind);
                game.set_weather(weather);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            bool dynamic = weather.dynamic != 0;
            if (ImGui::Checkbox(tr("Dynamic weather"), &dynamic)) {
                weather.dynamic = dynamic ? 1 : 0;
                game.set_weather(weather);
            }
            if (dynamic) {
                ImGui::SameLine();
                if (ImGui::SmallButton(tr("Another"))) {
                    weather.seed = weather.seed * 6364136223846793005ull + 1442695040888963407ull;
                    weather.clock = 0.0;
                    game.set_weather(weather);
                }
            }
        }
        ImGui::SameLine();
        bool admit = game.admits();
        if (ImGui::Checkbox(tr("Take new players"), &admit)) game.set_admit(admit);
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        std::size_t playing = 0;
        for (const auto& user : game.users()) playing += user.connected ? 1 : 0;
        const auto places = s_.settings().max_players;
        ImGui::Text("%s", places > 0 ? trf("players: %zu of %u", playing, places).c_str() : trf("players: %zu", playing).c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::TextUnformatted(s_.settings().dispatcher_password.empty() ? tr("dispatcher: this computer only")
                                                                         : tr("dispatcher: from anywhere, by password"));
        // At the right edge, after the line's text; on a line of its own when
        // the window is too narrow for both (it overlapped the text).
        {
            const char* stop = tr("Stop the server");
            const float width = ImGui::CalcTextSize(stop).x + ImGui::GetStyle().FramePadding.x * 2.0f;
            ImGui::SameLine();
            const float after = ImGui::GetCursorPosX();
            const float right = ImGui::GetWindowContentRegionMax().x - width;
            if (right >= after) ImGui::SetCursorPosX(right);
            else ImGui::NewLine();
            if (ImGui::Button(stop)) running = false;
        }
        // The line stays, empty when nothing is shown: the panels below keep their place.
        if (ImGui::GetTime() < status_until_) {
            ImGui::TextColored(status_error_ ? ImVec4(1.0f, 0.4f, 0.4f, 1.0f) : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s", status_.c_str());
        } else {
            ImGui::TextUnformatted("");
        }
        ImGui::Separator();
    }

    // ---- the map ----
    void draw_map() {
        auto& game = s_.game();
        auto& route = s_.route();
        map_.trains.clear();
        map_.lines.clear();
        map_.marks.clear();
        map_.selected_train = selected_train_;
        // The segments' ways: waiting grey, set green, the train on it dark green.
        const auto& station_routes = route.station_routes();
        for (const auto& segment : game.segments()) {
            const auto index = station_routes.route_index(segment.route);
            if (!index) continue;
            const auto* path = station_routes.path_of(station_routes.routes()[*index], segment.reverse);
            const auto& leg = path ? (segment.reverse ? path->backward : path->forward) : std::nullopt;
            if (!leg) continue;
            const ImU32 colour = segment.state == 0 ? IM_COL32(120, 120, 120, 200)
                               : segment.state == 3 ? IM_COL32(0, 120, 0, 220)
                                                    : IM_COL32(0, 200, 60, 200);
            map_.lines.push_back({.points = map_.along(*leg, 0.0, leg->length()), .colour = colour, .width = 6.0f});
        }
        for (const auto& t : game.trains()) {
            if (const auto leg = game.route_leg(t.id)) {
                map_.lines.push_back({.points = map_.along(*leg, 0.0, leg->length()), .colour = IM_COL32(0, 190, 0, 160), .width = 4.0f});
            }
            const auto cars = game.train_cars(t.id);
            if (cars.empty()) continue;
            // The first train on the track brings the map to it once.
            if (!seen_train_) {
                seen_train_ = true;
                map_.centre_on(cars.front().front, 1.0);
            }
            trackmap::MapTrain train{.id = t.id};
            train.colour = t.kind == kind_player    ? IM_COL32(200, 30, 200, 255)
                         : t.kind == kind_traffic   ? IM_COL32(40, 60, 220, 255)
                         : t.kind == kind_static    ? IM_COL32(140, 120, 20, 255)
                                                    : IM_COL32(120, 120, 120, 255);
            for (const auto& car : cars) train.cars.emplace_back(car.front, car.rear);
            train.label = std::to_string(t.number) + (t.owner.empty() ? std::string() : ' ' + t.owner) + ' ' + trf("%.0f km/h", std::abs(t.speed) * 3.6);
            map_.trains.push_back(std::move(train));
        }
        route_lines();
        if (placing_.active) placement_bar();
        if (routing_.active) {
            refresh_routing();
            if (routing_.preview.ok)
                map_.lines.push_back({.points = map_.along(routing_.preview.leg, 0.0, routing_.preview.leg.length()),
                                      .colour = routing_.request.shunting ? IM_COL32(230, 230, 230, 230) : IM_COL32(0, 120, 255, 220),
                                      .width = 7.0f});
        }
        map_.overlay = [this](ImDrawList* draw) {
            draw_route_marks(draw);
            if (placing_.active) draw_placement(draw);
            if (routing_.active) draw_routing(draw);
        };
        const bool menu_open = ImGui::IsPopupOpen("map_menu");
        const auto frame = map_.draw(route.signals(), menu_open);
        map_.overlay = nullptr;
        if (placing_.active) {
            // The consist being placed: drag a car along the track; a click
            // on another track moves its front there.
            const auto& io = ImGui::GetIO();
            if (frame.left_click) {
                placing_.grabbed = placement_car_at(frame.mouse);
                if (placing_.grabbed < 0) {
                    if (const auto where = map_.nearest_track(frame.mouse, 14.0)) move_placement(*where);
                }
            }
            if (placing_.grabbed >= 0) {
                if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    placing_.grabbed = -1;
                } else if (!placing_.cars.empty() && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)) {
                    // Along the grabbed car on the screen, its front the positive way.
                    const auto index = (std::min)(static_cast<std::size_t>(placing_.grabbed), placing_.cars.size() - 1);
                    const auto a = map_.screen(placing_.cars[index].first), b = map_.screen(placing_.cars[index].second);
                    const double dx = a.x - b.x, dy = a.y - b.y;
                    const double span = std::hypot(dx, dy);
                    if (span > 1e-6) slide_placement((io.MouseDelta.x * dx + io.MouseDelta.y * dy) / span / map_.scale());
                }
            }
            if (!io.WantTextInput && !menu_open) {
                const double step = io.KeyShift ? 10.0 : 1.0;
                if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) finish_placement();
                else if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) turn_placement();
                else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) cancel_placement();
                else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) slide_placement(step);
                else if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) slide_placement(-step);
            }
        } else if (routing_.active) {
            const auto& io = ImGui::GetIO();
            if (!io.WantTextInput && !menu_open && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) routing_.active = false;
        } else if (frame.left_click && frame.left_click->kind == trackmap::PickKind::train) {
            selected_train_ = static_cast<std::uint32_t>(frame.left_click->train);
        }
        if (frame.right_click) {
            menu_ = *frame.right_click;
            std::memset(menu_filter_, 0, sizeof menu_filter_);
            ImGui::OpenPopup("map_menu");
        }
        map_menu();
    }

    // ---- placing a consist: seen on the map first, put when confirmed ----
    void start_placement(const std::string& consist, simulation::TrackLocation where, const bool exact) {
        auto made = s_.route().consist(consist, 0);
        if (!made.ok()) {
            report(false, {}, made.error.empty() ? "consist " + consist + " has no cars" : made.error);
            return;
        }
        Placement p;
        p.active = true;
        p.consist = consist;
        for (std::size_t i = 0; i < made.cars.size(); ++i) {
            const double length = made.cars[i].definition ? made.cars[i].definition->length : 0.0;
            p.fronts.push_back(p.length);
            p.lengths.push_back(length);
            p.engines.push_back(i < made.records.size() && made.records[i].kind != 0);
            p.length += length;
        }
        placing_ = std::move(p);
        // The front where it was asked, the cars behind it: exactly so at a
        // path's start, else whichever way they fit, along the node first.
        if (exact) {
            placing_.head = where;
            lay_placement();
        } else {
            for (const bool forward : {true, false}) {
                where.forward = forward;
                placing_.head = where;
                lay_placement();
                if (placing_.fits) break;
            }
            if (!placing_.fits) {
                where.forward = true;
                placing_.head = where;
                lay_placement();
            }
        }
        if (!placing_.cars.empty()) map_.centre_on(placing_.cars.front().first);
        seen_train_ = true; // the map stays on it, not on the first train
        set_status(trf("placing %s: check where it stands, then put it", consist.c_str()));
    }

    // The train menu's "Move it": the preview from the train's cars, its
    // front where the train's is; put, the server puts the train again there.
    void start_moving(const std::uint32_t id) {
        const auto standing = s_.game().standing_trains();
        const auto it = std::find_if(standing.begin(), standing.end(), [&](const StandingTrain& t) { return t.id == id; });
        if (it == standing.end() || it->cars.empty()) {
            report(false, {}, trf("train %u is not on the track", id));
            return;
        }
        Placement p;
        p.active = true;
        p.moving = id;
        p.consist = trf("train %u (№%u)", id, it->number);
        for (const auto& car : it->cars) {
            p.fronts.push_back(p.length);
            p.lengths.push_back(car.length);
            p.engines.push_back(car.kind != 0);
            p.length += car.length;
        }
        p.head = {it->node, it->offset, it->forward};
        placing_ = std::move(p);
        lay_placement();
        set_status(trf("moving %s: drag it along the track, click another track to move it there", placing_.consist.c_str()));
    }

    void lay_placement() {
        auto& p = placing_;
        auto& network = s_.route().network();
        p.cars.clear();
        p.fits = true;
        p.overlaps.clear();
        for (std::size_t i = 0; i < p.lengths.size(); ++i) {
            simulation::TrackTraveller front(network, p.head.node, p.head.offset, p.head.forward);
            const auto to_front = front.move(-p.fronts[i]);
            simulation::TrackTraveller rear = front;
            const auto to_rear = rear.move(-p.lengths[i]);
            if (!to_front.ok() || !to_rear.ok()) {
                p.fits = false;
                if (to_front.ok()) p.cars.emplace_back(front.pose(), rear.pose());
                break;
            }
            p.cars.emplace_back(front.pose(), rear.pose());
        }
        // Another train in the way: a car end or middle of the consist within
        // 1.6 m of one of its cars (tracks lie 4 m and more apart). The
        // server decides when it is put.
        double t = 0.0;
        for (const auto& train : s_.game().trains()) {
            if (train.id == p.moving) continue;
            for (const auto& car : s_.game().train_cars(train.id)) {
                const double ax = map_.world_x(car.front), az = map_.world_z(car.front);
                const double bx = map_.world_x(car.rear), bz = map_.world_z(car.rear);
                for (const auto& [front, rear] : p.cars) {
                    const double fx = map_.world_x(front), fz = map_.world_z(front), rx = map_.world_x(rear), rz = map_.world_z(rear);
                    const double points[3][2] = {{fx, fz}, {(fx + rx) * 0.5, (fz + rz) * 0.5}, {rx, rz}};
                    for (const auto& point : points) {
                        if (segment_distance(point[0], point[1], ax, az, bx, bz, t) < 1.6) {
                            p.overlaps = trf("train %u", train.number);
                            return;
                        }
                    }
                }
            }
        }
    }

    void slide_placement(const double metres) {
        if (!placing_.active || metres == 0.0) return;
        const auto previous = placing_.head;
        const bool fitted = placing_.fits;
        simulation::TrackTraveller head(s_.route().network(), previous.node, previous.offset, previous.forward);
        head.move(metres);
        placing_.head = {head.node(), head.offset(), head.forward()};
        lay_placement();
        if (fitted && !placing_.fits) {
            // The front ran off the track or the rear would: it stays.
            placing_.head = previous;
            lay_placement();
        }
    }

    void move_placement(simulation::TrackLocation where) {
        if (!placing_.active) return;
        // Facing the way it faced on the screen as far as the new track allows.
        auto& network = s_.route().network();
        simulation::TrackTraveller old(network, placing_.head.node, placing_.head.offset, placing_.head.forward);
        const float facing = old.pose().heading;
        simulation::TrackTraveller along(network, where.node, where.offset, true);
        where.forward = std::cos(along.pose().heading - facing) >= 0.0f;
        placing_.head = where;
        lay_placement();
    }

    void turn_placement() {
        if (!placing_.active) return;
        simulation::TrackTraveller tail(s_.route().network(), placing_.head.node, placing_.head.offset, placing_.head.forward);
        if (!tail.move(-placing_.length).ok()) {
            report(false, {}, tr("turning needs the whole consist on the track"));
            return;
        }
        tail.reverse();
        placing_.head = {tail.node(), tail.offset(), tail.forward()};
        lay_placement();
    }

    void cancel_placement() {
        placing_ = {};
        set_status(tr("placing dropped"));
    }

    void finish_placement() {
        if (!placing_.active) return;
        if (!placing_.fits) {
            report(false, {}, tr("does not fit: not enough track"));
            return;
        }
        auto& game = s_.game();
        std::string error;
        if (placing_.moving != 0) {
            std::uint32_t placed = 0;
            const auto answer = replace_train(game, s_.route(), placing_.moving, placing_.head, std::nullopt, &placed);
            const bool ok = answer.rfind("error", 0) != 0;
            report(ok, answer, answer);
            if (placed) selected_train_ = placed;
            if (ok) placing_ = {};
            return;
        }
        const auto number = new_number_[0] ? static_cast<std::uint32_t>(std::strtoul(new_number_, nullptr, 10)) : game.free_number();
        auto made = train_at(s_.route(), placing_.consist, placing_.head, number, error);
        if (made) {
            static constexpr std::uint16_t kinds[] = {kind_static, kind_player, 3, kind_traffic}; // STATIC, PLAYER, LOOSE, TRAFFIC
            made->name = net::from_utf8(new_name_);
            made->kind = kinds[std::clamp(new_kind_, 0, 3)];
            made->state = static_cast<std::uint8_t>(std::clamp(new_state_, 0, 2));
            // Chocks under its head and tail (the records' 0x100 / 0x200).
            if (!made->records.empty()) {
                if (new_chocks_ & 1) made->records.front().flags |= 0x100u;
                if (new_chocks_ & 2) made->records.back().flags |= 0x200u;
            }
        }
        const auto id = made ? game.add_train(std::move(*made), error) : std::nullopt;
        if (id) new_number_[0] = 0;
        report(id.has_value(), trf("train %u (№%u) placed", id.value_or(0), number), error);
        if (!id) return;
        selected_train_ = *id;
        placing_ = {};
    }

    // The car of the consist being placed under the mouse, -1 none.
    [[nodiscard]] int placement_car_at(const ImVec2 mouse) const {
        const double reach = (std::max)(8.0, map_.scale() * 3.2 * 0.5 + 4.0);
        double best = reach, t = 0.0;
        int found = -1;
        for (std::size_t i = 0; i < placing_.cars.size(); ++i) {
            const auto a = map_.screen(placing_.cars[i].first), b = map_.screen(placing_.cars[i].second);
            const double d = segment_distance(mouse.x, mouse.y, a.x, a.y, b.x, b.y, t);
            if (d < best) {
                best = d;
                found = static_cast<int>(i);
            }
        }
        return found;
    }

    // Over the map while placing: what it is, whether it fits, the buttons.
    void placement_bar() {
        const bool trouble = !placing_.fits || !placing_.overlaps.empty();
        ImGui::TextUnformatted(trf("Placing %s: %zu cars, %.0f m", placing_.consist.c_str(), placing_.lengths.size(), placing_.length).c_str());
        if (!placing_.fits) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", tr("does not fit: not enough track"));
        } else if (!placing_.overlaps.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", trf("runs into %s", placing_.overlaps.c_str()).c_str());
        }
        ImGui::BeginDisabled(!placing_.fits);
        if (ImGui::Button(tr("Put it here (Enter)"))) finish_placement();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(tr("Turn it around (Space)"))) turn_placement();
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel (Esc)"))) cancel_placement();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", tr("drag it along the track; a click on another track moves it there; Left / Right: 1 m, with Shift 10 m"));
        (void)trouble;
    }

    // The consist being placed: outlined cars, the front one orange with an
    // arrow the way it faces; a red outline where it does not fit or runs
    // into a train.
    void draw_placement(ImDrawList* draw) const {
        if (placing_.cars.empty()) return;
        const float width = std::clamp(static_cast<float>(map_.scale() * 3.2), 5.0f, 14.0f);
        const bool trouble = !placing_.fits || !placing_.overlaps.empty();
        const ImU32 outline = trouble ? IM_COL32(220, 0, 0, 255) : IM_COL32(20, 20, 20, 255);
        for (std::size_t i = 0; i < placing_.cars.size(); ++i) {
            auto a = map_.screen(placing_.cars[i].first), b = map_.screen(placing_.cars[i].second);
            const float dx = b.x - a.x, dy = b.y - a.y;
            const float span = std::sqrt(dx * dx + dy * dy);
            if (span > 8.0f) {
                const float gap = 1.5f / span;
                a = ImVec2(a.x + dx * gap, a.y + dy * gap);
                b = ImVec2(b.x - dx * gap, b.y - dy * gap);
            }
            const ImU32 fill = i == 0 ? IM_COL32(255, 120, 0, 255) : placing_.engines[i] ? IM_COL32(255, 200, 120, 235) : IM_COL32(250, 250, 250, 235);
            draw->AddLine(a, b, outline, width + 3.0f);
            draw->AddLine(a, b, fill, width);
        }
        const auto front = map_.screen(placing_.cars.front().first);
        auto rear = map_.screen(placing_.cars.front().second);
        if (std::hypot(front.x - rear.x, front.y - rear.y) < 3.0f) rear = map_.screen(placing_.cars.back().second);
        trackmap::draw_direction_arrow(draw, front, rear, IM_COL32(255, 120, 0, 255), outline, std::clamp(width * 2.2f, 18.0f, 30.0f));
        draw->AddText(ImVec2(front.x + 10.0f, front.y + 8.0f), trouble ? IM_COL32(200, 0, 0, 255) : IM_COL32(0, 0, 0, 255), placing_.consist.c_str());
    }

    // The consists to put at a place, in the map's menu: the one picked in
    // "Поезда" first, then any found by part of its name.
    void consist_menu(const simulation::TrackLocation& location) {
        if (consist_ >= 0 && consist_ < static_cast<int>(consists_.size())) {
            const auto& picked = consists_[static_cast<std::size_t>(consist_)];
            if (ImGui::MenuItem(trf("%s (picked in Trains)", picked.c_str()).c_str())) start_placement(picked, location, false);
            ImGui::Separator();
        }
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(320.0f);
        ImGui::InputTextWithHint("##menu_filter", tr("consist: part of its name"), menu_filter_, sizeof menu_filter_);
        const auto filter = ui::search_form(menu_filter_);
        std::vector<std::size_t> found;
        for (std::size_t i = 0; i < consists_.size(); ++i) {
            if (filter.empty() || ui::search_form(consists_[i]).find(filter) != std::string::npos) found.push_back(i);
        }
        ImGui::BeginChild("menu_consists", ImVec2(320.0f, 280.0f), ImGuiChildFlags_Borders);
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(found.size()));
        while (clipper.Step()) {
            for (int k = clipper.DisplayStart; k < clipper.DisplayEnd; ++k) {
                const auto& name = consists_[found[static_cast<std::size_t>(k)]];
                if (ImGui::Selectable(name.c_str())) {
                    start_placement(name, location, false);
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        ImGui::EndChild();
    }

    // ---- setting a route on the map ----
    void start_routing(const std::uint32_t train, const bool edit) {
        routing_ = {};
        routing_.active = true;
        routing_.train = train;
        selected_train_ = train;
        if (edit) {
            if (const auto request = s_.game().route_request(train)) routing_.request = *request;
        }
    }
    void refresh_routing() {
        const double now = ImGui::GetTime();
        if (!routing_.dirty && now - routing_.at < 1.0) return;
        routing_.preview = s_.game().preview_route(routing_.train, routing_.request);
        routing_.dirty = false;
        routing_.at = now;
    }
    void confirm_routing(const bool shunting) {
        routing_.request.shunting = shunting;
        std::string result;
        const bool ok = s_.game().set_requested_route(routing_.train, routing_.request, result);
        report(ok, result, result);
        if (ok) routing_.active = false;
    }
    // Over the greyed panel: what is asked, the way's state, the actions.
    void routing_banner() {
        auto& r = routing_;
        ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.1f, 1.0f), "%s", trf("Setting a route for train %u", r.train).c_str());
        ImGui::TextWrapped("%s", tr("Right-click the track: the end, then points to pass; a junction: the branch to take; anywhere: confirm."));
        ImGui::TextUnformatted(trf("End: %s, points to pass: %zu, junctions chosen: %zu, from: %s", r.request.end ? tr("set") : tr("none"),
                                   r.request.via.size(), r.request.branches.size(), r.request.rear_first ? tr("the rear") : tr("the head"))
                                   .c_str());
        refresh_routing();
        if (r.preview.ok) ImGui::TextUnformatted(trf("The way: %.0f m, %zu junctions", r.preview.leg.length(), r.preview.leg.junctions.size()).c_str());
        else if (r.request.end) ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "%s", r.preview.error.c_str());
        if (ImGui::Button(tr("Turn the direction"))) {
            r.request.rear_first = !r.request.rear_first;
            r.dirty = true;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!r.preview.ok);
        if (ImGui::Button(tr("Train order"))) confirm_routing(false);
        ImGui::SameLine();
        if (ImGui::Button(tr("Shunting order"))) confirm_routing(true);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel"))) r.active = false;
        ImGui::Separator();
    }
    // The map's menu while a route is set: the end, points to pass, a
    // junction's branch, confirming.
    void routing_menu() {
        auto& r = routing_;
        auto& network = s_.route().network();
        ImGui::TextDisabled("%s", trf("Route for train %u", r.train).c_str());
        if (menu_.kind == trackmap::PickKind::track) {
            const auto at = menu_.location;
            if (!r.request.end) {
                if (ImGui::MenuItem(tr("The route's end here"))) {
                    r.request.end = at;
                    r.dirty = true;
                }
            } else {
                if (ImGui::MenuItem(tr("A point to pass here"))) {
                    r.request.via.push_back(at);
                    r.dirty = true;
                }
                if (ImGui::MenuItem(tr("Move the end here"))) {
                    r.request.end = at;
                    r.dirty = true;
                }
            }
        } else if (menu_.kind == trackmap::PickKind::junction) {
            const auto node = menu_.junction;
            const auto* info = network.node(node);
            const auto chosen = r.request.branches.find(node);
            for (std::uint32_t b = 0; info && b < info->branch_count(); ++b) {
                if (ImGui::MenuItem(trf("Take branch %u", b).c_str(), nullptr, chosen != r.request.branches.end() && chosen->second == b)) {
                    r.request.branches[node] = b;
                    r.dirty = true;
                }
            }
            if (chosen != r.request.branches.end() && ImGui::MenuItem(tr("Any branch (the shortest way)"))) {
                r.request.branches.erase(node);
                r.dirty = true;
            }
        }
        ImGui::Separator();
        if (!r.request.via.empty() && ImGui::MenuItem(tr("Remove the last point to pass"))) {
            r.request.via.pop_back();
            r.dirty = true;
        }
        if (ImGui::MenuItem(tr("Turn the direction"))) {
            r.request.rear_first = !r.request.rear_first;
            r.dirty = true;
        }
        refresh_routing();
        if (ImGui::MenuItem(tr("Confirm and set: train order"), nullptr, false, r.preview.ok)) confirm_routing(false);
        if (ImGui::MenuItem(tr("Confirm and set: shunting order"), nullptr, false, r.preview.ok)) confirm_routing(true);
        if (ImGui::MenuItem(tr("Cancel setting the route"))) r.active = false;
    }
    // The end, the points to pass, the chosen junctions and where it starts.
    void draw_routing(ImDrawList* draw) const {
        const auto& r = routing_;
        auto& network = s_.route().network();
        const auto dot = [&](const simulation::TrackLocation& at, const ImU32 colour, const char* text) {
            const auto pose = network.point(at.node, at.offset);
            if (!pose) return;
            const auto p = map_.screen(*pose);
            draw->AddCircleFilled(p, 9.0f, colour);
            draw->AddCircle(p, 9.0f, IM_COL32(255, 255, 255, 255), 0, 2.0f);
            const auto size = ImGui::CalcTextSize(text);
            draw->AddText(ImVec2(p.x - size.x * 0.5f, p.y - size.y * 0.5f), IM_COL32(255, 255, 255, 255), text);
        };
        for (std::size_t i = 0; i < r.request.via.size(); ++i) dot(r.request.via[i], IM_COL32(0, 110, 255, 255), std::to_string(i + 1).c_str());
        if (r.request.end) dot(*r.request.end, IM_COL32(220, 30, 30, 255), "K");
        for (const auto& [node, branch] : r.request.branches) {
            if (const auto pose = network.point(node, 0.0)) {
                const auto p = map_.screen(*pose);
                draw->AddRectFilled(ImVec2(p.x - 9.0f, p.y - 9.0f), ImVec2(p.x + 9.0f, p.y + 9.0f), IM_COL32(255, 150, 0, 255), 3.0f);
                const auto text = std::to_string(branch);
                draw->AddText(ImVec2(p.x - 4.0f, p.y - 7.0f), IM_COL32(0, 0, 0, 255), text.c_str());
            }
        }
        // Where the way starts and which way.
        if (r.preview.ok && r.preview.leg.length() > 1.0) {
            const auto points = map_.along(r.preview.leg, 0.0, (std::min)(12.0, r.preview.leg.length()));
            if (points.size() >= 2)
                trackmap::draw_direction_arrow(draw, map_.screen(points.back()), map_.screen(points.front()), IM_COL32(0, 110, 255, 255),
                                               IM_COL32(255, 255, 255, 255), 20.0f);
        }
    }

    void map_menu() {
        if (!ImGui::BeginPopup("map_menu")) return;
        if (routing_.active) {
            routing_menu();
            ImGui::EndPopup();
            return;
        }
        auto& game = s_.game();
        auto& route = s_.route();
        std::string error;
        switch (menu_.kind) {
        case trackmap::PickKind::train: {
            const auto id = static_cast<std::uint32_t>(menu_.train);
            selected_train_ = id;
            const auto trains = game.trains();
            const auto it = std::find_if(trains.begin(), trains.end(), [&](const auto& t) { return t.id == id; });
            if (it == trains.end()) {
                ImGui::TextDisabled("%s", tr("nothing here"));
                break;
            }
            ImGui::TextDisabled("%s", trf("Train %u (№%u), %zu cars", id, it->number, it->cars).c_str());
            // Its number and name (0x4605, 0x461A to everyone).
            if (ImGui::BeginMenu(tr("Number and name"))) {
                if (form_train_ != id || ImGui::IsWindowAppearing()) {
                    form_train_ = id;
                    std::snprintf(form_number_, sizeof form_number_, "%u", it->number);
                    std::snprintf(form_name_, sizeof form_name_, "%s", net::to_utf8(game.names(id).first).c_str());
                }
                ImGui::SetNextItemWidth(90.0f);
                ImGui::InputTextWithHint("##form_number", tr("number"), form_number_, sizeof form_number_, ImGuiInputTextFlags_CharsDecimal);
                ImGui::SetNextItemWidth(220.0f);
                ImGui::InputTextWithHint("##form_name", tr("name (the map shows it)"), form_name_, sizeof form_name_);
                if (ImGui::Button(tr("Apply"))) {
                    const auto number = static_cast<std::uint32_t>(std::strtoul(form_number_, nullptr, 10));
                    bool ok = true;
                    if (number && number != it->number) ok = game.set_number(id, number, error);
                    auto [old_name, notes] = game.names(id);
                    if (ok && net::from_utf8(form_name_) != old_name) ok = game.set_names(id, net::from_utf8(form_name_), std::move(notes), error);
                    report(ok, trf("train %u: №%s, %s", id, form_number_, form_name_), error);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndMenu();
            }
            if (it->owner.empty()) {
                if (ImGui::BeginMenu(tr("Give it to a player"))) {
                    for (const auto& user : game.users()) {
                        if (user.console || !user.connected) continue;
                        if (ImGui::MenuItem(user.nick.c_str())) report(game.assign(id, user.nick, error), trf("train %u given to %s", id, user.nick.c_str()), error);
                    }
                    ImGui::EndMenu();
                }
            } else if (ImGui::MenuItem(trf("Take it back from %s", it->owner.c_str()).c_str())) {
                report(game.unassign(id, error), trf("train %u taken back", id), error);
            }
            if (ImGui::MenuItem(tr("Set a route"))) start_routing(id, false);
            if (game.route_request(id) && ImGui::MenuItem(tr("Edit the route"))) start_routing(id, true);
            if (ImGui::MenuItem(tr("Cancel the route"), nullptr, false, !game.route_junctions(id).empty() || game.route_leg(id).has_value())) {
                report(game.cancel_route(id, error), trf("train %u's route freed", id), error);
            }
            // Put again (a new id): elsewhere, or with chocks; nobody's train.
            if (ImGui::MenuItem(tr("Move it (on the map)"), nullptr, false, it->owner.empty())) start_moving(id);
            if (ImGui::BeginMenu(tr("Chocks"), it->owner.empty())) {
                const auto now = train_chocks(game, id);
                const char* labels[] = {tr("none"), tr("under the head"), tr("under the tail"), tr("under both ends")};
                for (std::uint8_t mask = 0; mask < 4; ++mask) {
                    if (!ImGui::MenuItem(labels[mask], nullptr, now == mask)) continue;
                    std::uint32_t placed = 0;
                    const auto answer = replace_train(game, route, id, std::nullopt, mask, &placed);
                    report(answer.rfind("error", 0) != 0, answer, answer);
                    if (placed) selected_train_ = placed;
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem(tr("Remove train"))) report(game.remove_train(id, error), trf("train %u removed", id), error);
            break;
        }
        case trackmap::PickKind::junction: {
            const auto node = menu_.junction;
            const auto* info = route.network().node(node);
            const auto* state = route.network().junction(node);
            ImGui::TextDisabled("%s", trf("Junction %u", node).c_str());
            for (std::uint32_t b = 0; info && b < info->branch_count(); ++b) {
                if (ImGui::MenuItem(trf("Branch %u", b).c_str(), nullptr, state && state->selected == b)) {
                    report(game.set_junction(node, b, std::nullopt, error), trf("junction %u to branch %u", node, b), error);
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem(tr("Lock against players"))) report(game.set_junction(node, std::nullopt, true, error), trf("junction %u locked", node), error);
            if (ImGui::MenuItem(tr("Unlock"))) report(game.set_junction(node, std::nullopt, false, error), trf("junction %u unlocked", node), error);
            break;
        }
        case trackmap::PickKind::signal: {
            const auto item = first_item(menu_.signal);
            if (!item) break;
            const auto name = map_.signal_name(menu_.signal);
            const auto heading = (name.empty() ? std::string() : name + ": ") + game.signal_report(*item);
            ImGui::TextDisabled("%s", heading.c_str());
            if (ImGui::BeginMenu(tr("Set the aspect"))) {
                for (int a = 0; a <= 7; ++a) {
                    const auto aspect = static_cast<formats::SignalAspect>(a);
                    if (ImGui::MenuItem(formats::to_string(aspect))) {
                        const bool ok = game.set_signal(*item, aspect, error);
                        report(ok, game.signal_report(*item), error);
                    }
                }
                ImGui::EndMenu();
            }
            const auto act = [&](const char* label, const GameServer::SignalAction action) {
                if (!ImGui::MenuItem(label)) return;
                const bool ok = game.control_signal(*item, action, error);
                report(ok, game.signal_report(*item), error);
            };
            act(tr("Open"), GameServer::SignalAction::open);
            act(tr("Automatic block"), GameServer::SignalAction::automatic);
            act(tr("Close"), GameServer::SignalAction::close);
            act(tr("Back to its default"), GameServer::SignalAction::defaults);
            break;
        }
        case trackmap::PickKind::track: {
            const auto location = menu_.location;
            if (placing_.active && ImGui::MenuItem(trf("Move %s here", placing_.consist.c_str()).c_str())) move_placement(location);
            if (ImGui::BeginMenu(tr("Put a train"))) {
                consist_menu(location);
                ImGui::EndMenu();
            }
            if (selected_train_ && ImGui::MenuItem(trf("Route for train %u to here", selected_train_).c_str())) {
                route_to(selected_train_, location);
            }
            break;
        }
        default:
            ImGui::TextDisabled("%s", tr("nothing here"));
            break;
        }
        ImGui::EndPopup();
    }

    // The selected train's route from its head to a place; from its rear
    // when the place is behind it.
    void route_to(const std::uint32_t train, const simulation::TrackLocation& to) {
        auto& game = s_.game();
        std::string result;
        for (const bool facing : {to.forward, !to.forward}) {
            const simulation::TrackLocation target{to.node, to.offset, facing};
            for (const bool rear_first : {false, true}) {
                if (game.lay_free_route_to(train, target, rear_first, result)) {
                    report(true, result, {});
                    return;
                }
            }
        }
        report(false, {}, result);
    }

    // ---- the tabs ----
    void trains_tab() {
        auto& game = s_.game();
        auto& route = s_.route();
        std::string error;
        static constexpr const char* kinds[] = {"STATIC", "PLAYER", "TRAFFIC", "LOOSE"};
        if (ImGui::BeginTable("trains", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0.0f, 220.0f))) {
            ImGui::TableSetupColumn(tr("No."));
            ImGui::TableSetupColumn(tr("id"));
            ImGui::TableSetupColumn(tr("kind"));
            ImGui::TableSetupColumn(tr("driver"));
            ImGui::TableSetupColumn(tr("cars"));
            ImGui::TableSetupColumn(tr("km/h"));
            ImGui::TableHeadersRow();
            for (const auto& t : game.trains()) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                char label[32];
                std::snprintf(label, sizeof label, "%u##train%u", t.number, t.id);
                if (ImGui::Selectable(label, selected_train_ == t.id, ImGuiSelectableFlags_SpanAllColumns)) {
                    selected_train_ = t.id;
                    if (const auto cars = game.train_cars(t.id); !cars.empty()) map_.centre_on(cars.front().front, 1.0);
                }
                ImGui::TableNextColumn();
                ImGui::Text("%u", t.id);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(t.kind < 4 ? kinds[t.kind] : "?");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(t.owner.empty() ? "-" : t.owner.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%zu", t.cars);
                ImGui::TableNextColumn();
                ImGui::Text("%.0f", std::abs(t.speed) * 3.6);
            }
            ImGui::EndTable();
        }
        if (selected_train_) {
            ImGui::SeparatorText(trf("Train %u", selected_train_).c_str());
            if (ImGui::Button(tr("Take it back"))) report(game.unassign(selected_train_, error), trf("train %u taken back", selected_train_), error);
            ImGui::SameLine();
            if (ImGui::Button(tr("Cancel the route"))) report(game.cancel_route(selected_train_, error), trf("train %u's route freed", selected_train_), error);
            ImGui::SameLine();
            if (ImGui::Button(tr("Remove train"))) {
                report(game.remove_train(selected_train_, error), trf("train %u removed", selected_train_), error);
                selected_train_ = 0;
            }
            if (selected_train_ && ImGui::BeginCombo(tr("Give it to"), tr("a player..."))) {
                for (const auto& user : game.users()) {
                    if (user.console || !user.connected) continue;
                    if (ImGui::Selectable(user.nick.c_str())) {
                        report(game.assign(selected_train_, user.nick, error), trf("train %u given to %s", selected_train_, user.nick.c_str()), error);
                    }
                }
                ImGui::EndCombo();
            }
        }
        ImGui::SeparatorText(tr("A new train"));
        ImGui::InputTextWithHint("##consist_filter", tr("consist: part of its name"), consist_filter_, sizeof consist_filter_);
        if (ImGui::BeginListBox("##consists", ImVec2(-1.0f, 150.0f))) {
            const auto filter = ui::search_form(consist_filter_);
            for (std::size_t i = 0; i < consists_.size(); ++i) {
                if (!filter.empty() && ui::search_form(consists_[i]).find(filter) == std::string::npos) continue;
                // A click on the picked one unpicks it.
                const bool picked = consist_ == static_cast<int>(i);
                if (ImGui::Selectable(consists_[i].c_str(), picked)) consist_ = picked ? -1 : static_cast<int>(i);
            }
            ImGui::EndListBox();
        }
        if (consist_ >= 0 && consist_ < static_cast<int>(consists_.size())) {
            ImGui::TextUnformatted(trf("Picked: %s", consists_[static_cast<std::size_t>(consist_)].c_str()).c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("Unpick"))) consist_ = -1;
        }
        const auto& paths = route.paths();
        const char* path_name = path_ >= 0 && path_ < static_cast<int>(paths.size()) ? paths[static_cast<std::size_t>(path_)].name.c_str() : tr("no path");
        if (ImGui::BeginCombo(tr("Path"), path_name)) {
            if (ImGui::Selectable(tr("no path"), path_ < 0)) path_ = -1;
            for (std::size_t i = 0; i < paths.size(); ++i) {
                const auto& name = paths[i].name.empty() ? paths[i].file : paths[i].name;
                if (ImGui::Selectable((name + "##path" + std::to_string(i)).c_str(), path_ == static_cast<int>(i))) path_ = static_cast<int>(i);
            }
            ImGui::EndCombo();
        }
        // The new train's number, name, kind and state (here and on the map).
        ImGui::SetNextItemWidth(90.0f);
        ImGui::InputTextWithHint("##new_number", tr("number"), new_number_, sizeof new_number_, ImGuiInputTextFlags_CharsDecimal);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##new_name", tr("name (the map shows it)"), new_name_, sizeof new_name_);
        {
            static constexpr const char* kind_names[] = {"STATIC", "PLAYER", "LOOSE", "TRAFFIC"};
            const char* states[] = {tr("Cold"), tr("Switched on"), tr("Running")};
            ImGui::SetNextItemWidth(110.0f);
            ImGui::Combo("##new_kind", &new_kind_, kind_names, 4);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(130.0f);
            ImGui::Combo("##new_state", &new_state_, states, 3);
            const char* chocks[] = {tr("no chocks"), tr("chocks under the head"), tr("chocks under the tail"), tr("chocks under both ends")};
            ImGui::SetNextItemWidth(190.0f);
            ImGui::Combo("##new_chocks", &new_chocks_, chocks, 4);
        }
        const bool ready = consist_ >= 0 && path_ >= 0;
        ImGui::BeginDisabled(!ready);
        if (ImGui::Button(tr("At the path's start (preview)"))) {
            const auto& consist = consists_[static_cast<std::size_t>(consist_)];
            const auto& path = paths[static_cast<std::size_t>(path_)];
            auto made = route.consist(consist, 0);
            double length = 0.0;
            for (const auto& car : made.cars) length += car.definition ? car.definition->length : 0.0;
            if (const auto front = made.ok() ? route.path_front(path, length) : std::nullopt) {
                start_placement(consist, {front->node(), front->offset(), front->forward()}, true);
            } else {
                report(false, {}, made.ok() ? tr("the path's start has no track") : made.error);
            }
        }
        ImGui::EndDisabled();
        ImGui::TextWrapped("%s", tr("Or right-click the track on the map: \"Put a train\". The consist shows where it will stand; it is put when "
                                    "confirmed. Right-click a train, a junction or a signal for what can be done with it; the wheel zooms, a "
                                    "right drag moves the map."));
        placements_ui();
    }

    // Placements: every train on the track saved under a name (the route's
    // placements folder, the dispatchers' consoles take the same files),
    // put back the trains there removed first, or added to them.
    // A placement opened: as saved, its trains with those it includes.
    void open_placement(const std::string& name) {
        auto& route = s_.route();
        opened_ = name;
        opened_trains_.clear();
        opened_problems_.clear();
        std::string error;
        auto placement = placement_file_read(route, name, error);
        if (!placement) {
            report(false, {}, error);
            opened_.clear();
            return;
        }
        opened_placement_ = std::move(*placement);
        opened_trains_ = placement_resolved(route, opened_placement_, name, opened_problems_);
        opened_picked_.assign(opened_trains_.size(), 1);
    }

    void placements_ui() {
        auto& game = s_.game();
        auto& route = s_.route();
        ImGui::SeparatorText(tr("Placements"));
        ImGui::SetNextItemWidth(220.0f);
        ImGui::InputTextWithHint("##placement_name", tr("placement's name"), placement_name_, sizeof placement_name_);
        ImGui::SameLine();
        ImGui::BeginDisabled(!net::placement_name_ok(placement_name_));
        if (ImGui::Button(tr("Save every train"))) {
            const auto answer = save_placement(game, route, placement_name_);
            report(answer.rfind("error", 0) != 0, answer, answer);
            placements_listed_ = false;
        }
        ImGui::EndDisabled();
        if (s_.settings().autosave_minutes > 0.0) {
            ImGui::TextDisabled("%s", trf("autosave: every %.0f min and at the stop; autosave-previous: the last run's (after a crash or a restart)",
                                          s_.settings().autosave_minutes).c_str());
        }
        if (!placements_listed_) {
            placements_ = placement_names(route);
            placements_listed_ = true;
        }
        if (ImGui::SmallButton(tr("Refresh##placements"))) placements_listed_ = false;
        if (placements_.empty()) {
            ImGui::TextDisabled("%s", tr("none saved yet"));
            return;
        }
        if (ImGui::BeginTable("placements", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            for (const auto& name : placements_) {
                ImGui::PushID(name.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const bool autosave = name == autosave_name || name == autosave_previous_name;
                ImGui::TextUnformatted(autosave ? (name + " " + tr(name == autosave_name ? "(autosave)" : "(the last run's autosave)")).c_str() : name.c_str());
                ImGui::TableNextColumn();
                if (ImGui::SmallButton(tr("Put (the trains now removed)"))) {
                    const auto answer = load_placement(game, route, name, false);
                    report(answer.rfind("error", 0) != 0, answer, answer);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(tr("Add"))) {
                    const auto answer = load_placement(game, route, name, true);
                    report(answer.rfind("error", 0) != 0, answer, answer);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(tr(opened_ == name ? "Close##open" : "Open: includes, trains"))) {
                    if (opened_ == name) opened_.clear();
                    else open_placement(name);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(tr("Delete"))) {
                    std::error_code error;
                    std::filesystem::remove(route.placements_folder() / std::filesystem::path(std::u8string(name.begin(), name.end()) + u8".placement"), error);
                    if (opened_ == name) opened_.clear();
                    placements_listed_ = false;
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (opened_.empty()) return;
        // The one opened: the placements it includes, its trains.
        ImGui::SeparatorText(opened_.c_str());
        ImGui::TextUnformatted(trf("Includes (%zu):", opened_placement_.includes.size()).c_str());
        for (std::size_t i = 0; i < opened_placement_.includes.size(); ++i) {
            ImGui::SameLine();
            const auto other = opened_placement_.includes[i];
            if (ImGui::SmallButton(trf("%s ×##inc%zu", other.c_str(), i).c_str())) {
                const auto answer = placement_include(route, opened_, other, false);
                report(answer.rfind("error", 0) != 0, answer, answer);
                open_placement(opened_);
                break;
            }
        }
        ImGui::SetNextItemWidth(240.0f);
        if (ImGui::BeginCombo("##include_other", tr("include another placement"))) {
            for (const auto& other : placements_) {
                const auto& includes = opened_placement_.includes;
                if (other == opened_ || std::find(includes.begin(), includes.end(), other) != includes.end()) continue;
                if (ImGui::Selectable(other.c_str())) {
                    const auto answer = placement_include(route, opened_, other, true);
                    report(answer.rfind("error", 0) != 0, answer, answer);
                    open_placement(opened_);
                    break;
                }
            }
            ImGui::EndCombo();
        }
        for (const auto& problem : opened_problems_) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "%s", problem.c_str());
        if (opened_.empty()) return;
        if (ImGui::SmallButton(tr("All"))) std::fill(opened_picked_.begin(), opened_picked_.end(), 1);
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("None"))) std::fill(opened_picked_.begin(), opened_picked_.end(), 0);
        static constexpr const char* kinds[] = {"STATIC", "PLAYER", "TRAFFIC", "LOOSE"};
        const char* states[] = {tr("Cold"), tr("Switched on"), tr("Running")};
        if (ImGui::BeginTable("##opened_trains", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 180.0f))) {
            ImGui::TableSetupColumn("");
            ImGui::TableSetupColumn(tr("No."));
            ImGui::TableSetupColumn(tr("Name"));
            ImGui::TableSetupColumn(tr("Kind"));
            ImGui::TableSetupColumn(tr("Cars"));
            ImGui::TableSetupColumn(tr("From"));
            ImGui::TableHeadersRow();
            for (std::size_t i = 0; i < opened_trains_.size(); ++i) {
                const auto& t = opened_trains_[i];
                ImGui::PushID(static_cast<int>(i));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                bool on = opened_picked_[i] != 0;
                if (ImGui::Checkbox("##pick", &on)) opened_picked_[i] = on ? 1 : 0;
                ImGui::TableNextColumn();
                ImGui::Text("%u", t.number);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(net::to_utf8(t.name).c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%s, %s", t.kind < 4 ? kinds[t.kind] : "?", t.state < 3 ? states[t.state] : "?");
                ImGui::TableNextColumn();
                const bool chocked = std::any_of(t.cars.begin(), t.cars.end(), [](const net::PlacementCar& c) { return c.chocks() != 0; });
                ImGui::Text("%zu%s", t.cars.size(), chocked ? tr(", chocks") : "");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(t.from.empty() ? tr("its own") : t.from.c_str());
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        const auto count = static_cast<std::size_t>(std::count(opened_picked_.begin(), opened_picked_.end(), 1));
        ImGui::BeginDisabled(count == 0);
        if (ImGui::Button(trf("Put the %zu picked", count).c_str())) {
            std::size_t put = 0;
            std::string problems;
            for (std::size_t i = 0; i < opened_trains_.size(); ++i) {
                if (!opened_picked_[i]) continue;
                const auto answer = place_cars(game, route, net::place_of(opened_trains_[i]));
                if (answer.rfind("error", 0) == 0) problems += (problems.empty() ? "" : "; ") + answer;
                else ++put;
            }
            report(problems.empty(), trf("%zu of %zu trains put", put, count), problems);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200.0f);
        ImGui::InputTextWithHint("##subset_name", tr("another placement's name"), subset_name_, sizeof subset_name_);
        ImGui::SameLine();
        ImGui::BeginDisabled(!net::placement_name_ok(subset_name_));
        if (ImGui::Button(tr("Save the picked as it"))) {
            net::Placement subset{.route = route.folder()};
            for (std::size_t i = 0; i < opened_trains_.size(); ++i) {
                if (!opened_picked_[i]) continue;
                auto t = opened_trains_[i];
                t.from.clear();
                subset.trains.push_back(std::move(t));
            }
            const auto written = placement_file_write(route, subset_name_, subset);
            report(written.empty(), trf("placement %s: %zu trains saved", subset_name_, subset.trains.size()), written);
            placements_listed_ = false;
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
    }

    // The point a train's next station route is looked for from (rts_server.exe's
    // "Собрать маршрут": the last route of its queue, else the train itself),
    // the way chosen with "Направление".
    void update_current_point() {
        auto& game = s_.game();
        const auto& station_routes = s_.route().station_routes();
        current_at_.reset();
        current_rear_first_ = false;
        current_text_.clear();
        if (!selected_train_) return;
        std::optional<GameServer::SegmentSummary> last;
        for (const auto& segment : game.segments()) {
            if (segment.train == selected_train_) last = segment;
        }
        // A new train or a new end of its queue: the direction as it goes.
        const auto key = std::to_string(selected_train_) + "/" + (last ? std::to_string(last->id) : std::string("-"));
        if (key != flip_key_) {
            flip_key_ = key;
            flip_ = false;
            picked_.reset();
        }
        if (const auto from = game.next_from(selected_train_, flip_)) {
            current_at_ = from->at;
            current_rear_first_ = from->rear_first;
            behind_ = from->behind;
        }
        if (last) {
            current_text_ = flip_ ? trf("From the end of %s, back (it reverses there)", last->name.c_str()) : trf("From the end of %s", last->name.c_str());
        } else {
            current_text_ = flip_ ? tr("From the train's rear (it goes rear first)") : tr("From the train's head");
        }
        // The routes from there, looked for again when the point changes.
        char text[96] = "";
        if (current_at_) std::snprintf(text, sizeof text, "%u %.1f %d %.0f", current_at_->node, current_at_->offset, current_at_->forward ? 1 : 0, behind_);
        if (text != avail_key_) {
            avail_key_ = text;
            avail_ = current_at_ ? station_routes.available(s_.route().network(), *current_at_, 60000.0, behind_) : std::vector<StationRoutes::Choice>{};
            // Main tracks first, onto a wrong track last (rts_server.exe's
            // list: green, lilac), the nearest first between.
            const auto rank = [&](const StationRoutes::Choice& c) {
                const auto& r = station_routes.routes()[c.route];
                const auto last_point = c.reverse ? r.start : r.end;
                if (last_point && (station_routes.points()[*last_point].data->flags & point_flag::wrong_way)) return 2;
                return (r.data->flags & route_flag::main_track) ? 0 : 1;
            };
            std::stable_sort(avail_.begin(), avail_.end(), [&](const auto& a, const auto& b) { return rank(a) < rank(b); });
        }
    }

    // A station route's line in the list: its station and name, from its
    // end, onto a platform track and its length, how far its start is.
    [[nodiscard]] std::string route_label(const StationRoutes::Route& r, const bool reverse, const std::optional<double> distance) const {
        const auto& station_routes = s_.route().station_routes();
        std::string label = r.data->station + ": " + r.data->name;
        if (reverse) label += tr(" (from its end)");
        const auto last_point = reverse ? r.start : r.end;
        if (last_point) {
            const auto& point = station_routes.points()[*last_point];
            if (point.data->flags & point_flag::platform) label += " [П]";
            if (point.length > 0.0) label += ' ' + trf("%.0f m", point.length);
        }
        if (distance && *distance > 50.0) label += "  " + trf("in %.1f km", *distance / 1000.0);
        return label;
    }

    void add_picked() {
        if (!picked_ || !selected_train_) return;
        auto& game = s_.game();
        const auto& r = s_.route().station_routes().routes()[picked_->route];
        std::string error;
        const auto made = game.add_segment({.train = selected_train_,
                                            .route = r.data->id,
                                            .reverse = picked_->reverse,
                                            .hold = hold_,
                                            .reversed = current_rear_first_,
                                            .priority = static_cast<std::uint32_t>(priority_)},
                                           error);
        report(made.has_value(), trf("segment %u queued: %s", made.value_or(0), r.data->name.c_str()), error);
        if (made) picked_.reset();
    }

    // A route picked in a list: the map shows all of it and where the
    // routes are looked for from (unchanged when they are on it already).
    void pick_route(const RouteChoice choice) {
        picked_ = choice;
        const auto& station_routes = s_.route().station_routes();
        const auto* leg = station_routes.leg_of(station_routes.routes()[choice.route], choice.reverse);
        if (!leg) return;
        auto points = map_.along(*leg, 0.0, leg->length());
        if (current_at_) {
            if (const auto here = s_.route().network().point(current_at_->node, current_at_->offset)) points.push_back(*here);
        }
        map_.fit(points);
    }

    void routes_tab() {
        auto& game = s_.game();
        const auto& station_routes = s_.route().station_routes();
        std::string error;
        if (station_routes.routes().empty()) {
            ImGui::TextWrapped("%s", tr("The route database has no station routes."));
            return;
        }
        routes_tab_open_ = true;
        // The train: picked here, in "Поезда" or on the map.
        const auto trains = game.trains();
        std::string train_name = tr("a train...");
        for (const auto& t : trains) {
            if (t.id == selected_train_) train_name = trf("%u (id %u)", t.number, t.id) + (t.owner.empty() ? "" : ", " + t.owner);
        }
        if (ImGui::BeginCombo(tr("Train"), train_name.c_str())) {
            for (const auto& t : trains) {
                const auto label = trf("%u (id %u)", t.number, t.id) + (t.owner.empty() ? "" : ", " + t.owner) + "##pick" + std::to_string(t.id);
                if (ImGui::Selectable(label.c_str(), t.id == selected_train_)) selected_train_ = t.id;
            }
            ImGui::EndCombo();
        }
        update_current_point();
        static constexpr const char* states[] = {"waiting", "set", "set part of the way", "the train on it", "left"};
        if (selected_train_) {
            // Its queue (rts_server.exe's upper list).
            ImGui::SeparatorText(tr("Its queue"));
            bool any = false;
            for (const auto& segment : game.segments()) {
                if (segment.train != selected_train_) continue;
                any = true;
                char label[32];
                std::snprintf(label, sizeof label, "%s##cancel%u", tr("Cancel"), segment.id);
                if (ImGui::SmallButton(label)) report(game.cancel_segment(segment.id, error), trf("segment %u cancelled", segment.id), error);
                ImGui::SameLine();
                ImGui::Text("%s: %s%s, %s %u/%zu", segment.station.c_str(), segment.name.c_str(), segment.reverse ? tr(" (from its end)") : "",
                            tr(segment.state < 5 ? states[segment.state] : "?"), static_cast<unsigned>(segment.thrown), segment.junctions);
            }
            if (!any) ImGui::TextDisabled("%s", tr("nothing queued"));
            // What can be set next, from there, the way chosen.
            ImGui::SeparatorText(tr("Routes it can take"));
            ImGui::TextUnformatted(current_text_.c_str());
            if (ImGui::Button(flip_ ? tr("Direction: back (change)") : tr("Direction: ahead (change)"))) {
                flip_ = !flip_;
                picked_.reset();
            }
            ImGui::TextDisabled("%s", tr("green: main track; lilac: onto a wrong track; [П] a platform, then the track's length"));
            if (!current_at_) {
                ImGui::TextDisabled("%s", tr("the train is not on the server's track"));
            } else if (ImGui::BeginListBox("##available", ImVec2(-1.0f, 190.0f))) {
                if (avail_.empty()) ImGui::TextDisabled("%s", tr("none from here this way"));
                for (const auto& choice : avail_) {
                    const auto& r = station_routes.routes()[choice.route];
                    const auto last_point = choice.reverse ? r.start : r.end;
                    const bool wrong = last_point && (station_routes.points()[*last_point].data->flags & point_flag::wrong_way);
                    const bool main = (r.data->flags & route_flag::main_track) != 0;
                    const RouteChoice here{choice.route, choice.reverse};
                    if (wrong) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.55f, 1.0f, 1.0f));
                    else if (main) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.95f, 0.45f, 1.0f));
                    const auto label = route_label(r, choice.reverse, choice.distance) + "##avail" + std::to_string(choice.route) + (choice.reverse ? "r" : "");
                    if (ImGui::Selectable(label.c_str(), picked_ == here, ImGuiSelectableFlags_AllowDoubleClick)) {
                        pick_route(here);
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) add_picked();
                    }
                    if (ImGui::IsItemHovered()) hovered_ = here;
                    if (wrong || main) ImGui::PopStyleColor();
                }
                ImGui::EndListBox();
            }
        } else {
            ImGui::TextDisabled("%s", tr("pick a train here, in Trains or on the map"));
        }
        ImGui::Checkbox(tr("hold the next"), &hold_);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        ImGui::InputInt(tr("priority"), &priority_);
        priority_ = (std::max)(priority_, 0);
        ImGui::BeginDisabled(!picked_ || !selected_train_);
        const auto set_label = picked_ ? trf("Set: %s", station_routes.routes()[picked_->route].data->name.c_str()) : std::string(tr("Set the route picked"));
        if (ImGui::Button(set_label.c_str())) add_picked();
        ImGui::EndDisabled();
        if (!picked_) ImGui::TextDisabled("%s", tr("pick a route in the list (the map shows it; a double click sets it)"));
        // Any route of a station, wherever the train is (a check then decides).
        if (ImGui::CollapsingHeader(tr("Every route of a station"))) {
            const char* station = station_ >= 0 && station_ < static_cast<int>(stations_.size()) ? stations_[static_cast<std::size_t>(station_)].c_str()
                                                                                              : tr("a station...");
            if (ImGui::BeginCombo(tr("Station"), station)) {
                for (std::size_t i = 0; i < stations_.size(); ++i) {
                    if (ImGui::Selectable(stations_[i].c_str(), station_ == static_cast<int>(i))) station_ = static_cast<int>(i);
                }
                ImGui::EndCombo();
            }
            ImGui::InputTextWithHint("##route_filter", tr("part of its name"), route_filter_, sizeof route_filter_);
            ImGui::Checkbox(tr("from its end"), &back_);
            if (ImGui::BeginListBox("##routes", ImVec2(-1.0f, 160.0f))) {
                const auto filter = ui::search_form(route_filter_);
                for (std::size_t i = 0; i < station_routes.routes().size(); ++i) {
                    const auto& r = station_routes.routes()[i];
                    if (station_ < 0 || r.data->station != stations_[static_cast<std::size_t>(station_)]) continue;
                    if (!filter.empty() && ui::search_form(r.data->name).find(filter) == std::string::npos) continue;
                    const bool laid = station_routes.leg_of(r, back_) != nullptr;
                    const RouteChoice here{i, back_};
                    const auto label = std::to_string(r.data->id) + "  " + route_label(r, back_, std::nullopt) + (laid ? "" : tr(" (not laid)")) + "##all" +
                                       std::to_string(i);
                    if (ImGui::Selectable(label.c_str(), picked_ == here)) pick_route(here);
                    if (ImGui::IsItemHovered()) hovered_ = here;
                }
                ImGui::EndListBox();
            }
        }
        // Every train's queue (rts_server.exe's lower list).
        ImGui::SeparatorText(tr("Every train's queue"));
        if (ImGui::BeginTable("segments", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0.0f, 0.0f))) {
            ImGui::TableSetupColumn(tr("train"));
            ImGui::TableSetupColumn(tr("station route"), ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn(tr("state"));
            ImGui::TableSetupColumn("");
            ImGui::TableHeadersRow();
            for (const auto& segment : game.segments()) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%u", segment.train);
                ImGui::TableNextColumn();
                ImGui::Text("%s: %s", segment.station.c_str(), segment.name.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%s, %u/%zu", tr(segment.state < 5 ? states[segment.state] : "?"), static_cast<unsigned>(segment.thrown), segment.junctions);
                ImGui::TableNextColumn();
                char label[32];
                std::snprintf(label, sizeof label, "%s##cancel_all%u", tr("Cancel"), segment.id);
                if (ImGui::SmallButton(label)) report(game.cancel_segment(segment.id, error), trf("segment %u cancelled", segment.id), error);
            }
            ImGui::EndTable();
        }
    }

    // On the map while "Станционные маршруты" is open: the route picked and
    // the one under the mouse in its lists, each with an arrow at its end
    // and its name, and the point the routes are looked for from.
    void route_lines() {
        if (!routes_tab_open_) return;
        const auto& station_routes = s_.route().station_routes();
        const auto add = [&](const std::optional<RouteChoice>& choice, const ImU32 colour) {
            if (!choice) return;
            const auto* leg = station_routes.leg_of(station_routes.routes()[choice->route], choice->reverse);
            if (!leg) return;
            map_.lines.push_back({.points = map_.along(*leg, 0.0, leg->length()), .colour = colour, .width = 8.0f});
            // And the way to it from where the routes are looked for from, thin.
            if (const auto* way = way_to(*choice)) {
                map_.lines.push_back({.points = map_.along(*way, 0.0, way->length()), .colour = (colour & 0x00FFFFFFu) | 0x90000000u, .width = 4.0f});
            }
        };
        add(picked_, IM_COL32(230, 0, 200, 230));
        if (hovered_ != picked_) add(hovered_, IM_COL32(255, 140, 0, 230));
    }

    // The way from the point the routes are looked for from to a route's
    // start (as the walk found it, junctions any way); kept while the point
    // stays.
    const simulation::RouteLeg* way_to(const RouteChoice& choice) {
        if (!current_at_) return nullptr;
        if (ways_key_ != avail_key_) {
            ways_key_ = avail_key_;
            ways_.clear();
        }
        const auto key = std::pair{choice.route, choice.reverse};
        auto found = ways_.find(key);
        if (found == ways_.end()) {
            std::optional<simulation::RouteLeg> way;
            const auto& station_routes = s_.route().station_routes();
            const auto* leg = station_routes.leg_of(station_routes.routes()[choice.route], choice.reverse);
            const auto start = leg ? leg->at(0.0) : std::nullopt;
            if (start) {
                simulation::PathPlan plan;
                plan.name = "way to a station route";
                plan.start = *current_at_;
                plan.points = {{.kind = simulation::PlanPointKind::end, .node = start->node, .offset = start->offset}};
                auto built = simulation::build_route(s_.route().network(), plan);
                if (built.ok() && built.legs.size() == 1 && built.legs[0].length() > 1.0) way = std::move(built.legs[0]);
            }
            found = ways_.emplace(key, std::move(way)).first;
        }
        return found->second ? &*found->second : nullptr;
    }

    void draw_route_marks(ImDrawList* draw) const {
        if (!routes_tab_open_) return;
        const auto& station_routes = s_.route().station_routes();
        auto& network = s_.route().network();
        const auto mark = [&](const std::optional<RouteChoice>& choice, const ImU32 colour) {
            if (!choice) return;
            const auto& r = station_routes.routes()[choice->route];
            const auto* leg = station_routes.leg_of(r, choice->reverse);
            if (!leg || leg->length() < 1.0) return;
            const auto points = map_.along(*leg, (std::max)(0.0, leg->length() - 8.0), leg->length());
            if (points.size() < 2) return;
            const auto front = map_.screen(points.back()), rear = map_.screen(points.front());
            trackmap::draw_direction_arrow(draw, front, rear, colour, IM_COL32(20, 20, 20, 255), 20.0f);
            const auto name = r.data->station + ": " + r.data->name;
            draw->AddText(ImVec2(front.x + 14.0f, front.y - 8.0f), IM_COL32(20, 20, 20, 255), name.c_str());
        };
        mark(picked_, IM_COL32(230, 0, 200, 255));
        if (hovered_ != picked_) mark(hovered_, IM_COL32(255, 140, 0, 255));
        // Where the routes are looked for from, and which way.
        if (current_at_) {
            if (const auto pose = network.point(current_at_->node, current_at_->offset)) {
                const auto p = map_.screen(*pose);
                const float heading = pose->heading + (current_at_->forward ? 0.0f : 3.14159265f);
                const ImVec2 along(std::sin(heading), -std::cos(heading));
                draw->AddCircleFilled(p, 7.0f, IM_COL32(0, 110, 255, 255));
                draw->AddCircle(p, 7.0f, IM_COL32(255, 255, 255, 255), 0, 2.0f);
                trackmap::draw_direction_arrow(draw, ImVec2(p.x + along.x * 8.0f, p.y + along.y * 8.0f), p, IM_COL32(0, 110, 255, 255),
                                               IM_COL32(255, 255, 255, 255), 18.0f);
            }
        }
    }

    void signals_tab() {
        auto& game = s_.game();
        std::string error;
        ImGui::TextWrapped("%s", tr("A signal's own actions: right-click it on the map. Here: every signal of a type, and the defaults."));
        const char* type = type_ >= 0 && type_ < static_cast<int>(types_.size()) ? types_[static_cast<std::size_t>(type_)].c_str() : tr("a type...");
        if (ImGui::BeginCombo(tr("Signal type"), type)) {
            for (std::size_t i = 0; i < types_.size(); ++i) {
                if (ImGui::Selectable(types_[i].c_str(), type_ == static_cast<int>(i))) type_ = static_cast<int>(i);
            }
            ImGui::EndCombo();
        }
        ImGui::BeginDisabled(type_ < 0);
        std::size_t count = 0;
        const auto by_type = [&](const char* label, const simulation::UnitMode mode) {
            if (!ImGui::Button(label)) return;
            const bool ok = game.set_signal_type_mode(types_[static_cast<std::size_t>(type_)], mode, count, error);
            report(ok, trf("%zu signals of type %s: %s", count, types_[static_cast<std::size_t>(type_)].c_str(), label), error);
        };
        by_type(tr("Manual"), simulation::UnitMode::manual);
        ImGui::SameLine();
        by_type(tr("Open"), simulation::UnitMode::open);
        ImGui::SameLine();
        by_type(tr("Automatic block"), simulation::UnitMode::automatic);
        if (type_ >= 0) {
            const auto* signals = s_.route().signals();
            const formats::SignalType* found = nullptr;
            if (signals) {
                for (const auto& head : signals->heads()) {
                    if (head.type && head.type->name == types_[static_cast<std::size_t>(type_)]) found = head.type;
                }
            }
            bool shown = found && signals->shown_as_normal(*found);
            if (ImGui::Checkbox(tr("Shown as a train signal"), &shown)) {
                const bool ok = game.set_signal_type_shown(types_[static_cast<std::size_t>(type_)], shown, count, error);
                report(ok, trf(shown ? "type %s is shown as a train signal" : "type %s is not shown as a train signal", types_[static_cast<std::size_t>(type_)].c_str()), error);
            }
        }
        ImGui::EndDisabled();
        ImGui::SeparatorText(tr("Defaults"));
        if (ImGui::Button(tr("Restore the defaults"))) report(game.restore_signal_defaults(error), tr("every signal back to its defaults"), error);
        if (ImGui::Button(tr("Set as the defaults"))) report(game.set_signal_defaults(error), tr("every signal's defaults are its mode and aspect now"), error);
        if (ImGui::Button(tr("Save the route database"))) {
            std::string result;
            const bool ok = s_.route().save_signal_defaults(result);
            report(ok, result, result);
        }
        ImGui::TextWrapped("%s", trf("Saved into %s (the installation is not touched).", utf8_path(s_.route().saved_database_path()).c_str()).c_str());
    }

    void players_tab() {
        auto& game = s_.game();
        std::string error;
        if (ImGui::BeginTable("users", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0.0f, 240.0f))) {
            ImGui::TableSetupColumn(tr("nick"));
            ImGui::TableSetupColumn(tr("state"));
            ImGui::TableSetupColumn(tr("trains"));
            ImGui::TableSetupColumn(tr("address"), ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            for (const auto& user : game.users()) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (ImGui::Selectable((user.nick + "##user").c_str(), player_ == user.nick, ImGuiSelectableFlags_SpanAllColumns)) player_ = user.nick;
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(user.console ? tr("dispatcher") : !user.connected ? tr("away") : user.playing ? tr("playing") : tr("in the menu"));
                ImGui::TableNextColumn();
                std::string trains;
                for (const auto t : user.trains) trains += (trains.empty() ? "" : ", ") + std::to_string(t);
                ImGui::TextUnformatted(trains.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(user.address.c_str());
            }
            ImGui::EndTable();
        }
        ImGui::InputTextWithHint("##message", tr("a message"), message_, sizeof message_);
        const auto send = [&](const std::string& to) {
            const std::string text(message_);
            if (text.empty()) return;
            const bool ok = game.message(to, net::from_utf8(text), error);
            report(ok, trf("message to %s: %s", to == "*" ? tr("everyone") : to.c_str(), text.c_str()), error);
            if (ok) message_[0] = 0;
        };
        ImGui::BeginDisabled(player_.empty());
        if (ImGui::Button(tr("To the player"))) send(player_);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(tr("To everyone"))) send("*");
        ImGui::BeginDisabled(player_.empty());
        if (ImGui::Button(tr("Drop the player"))) report(game.kick(player_, error), trf("%s dropped", player_.c_str()), error);
        ImGui::EndDisabled();
    }

    // The game's clock and weather for everyone (openRTS's players and
    // consoles at once; RTS's: the time, the weather of their login).
    void clock_tab() {
        auto& game = s_.game();
        const int seconds = static_cast<int>(game.time_of_day());
        ImGui::Text(tr("Time %02d:%02d:%02d"), seconds / 3600, seconds / 60 % 60, seconds % 60);
        // The slider while dragged shows where it goes; let go, the time is set.
        if (!time_dragged_) time_hours_ = static_cast<float>(game.time_of_day() / 3600.0);
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##clock_time", &time_hours_, 0.0f, 24.0f, "%.2f h");
        time_dragged_ = ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            game.set_time(static_cast<double>(time_hours_) * 3600.0);
            report(true, trf("time set to %s", clock_text(game.time_of_day()).c_str()), {});
        }
        ImGui::TextUnformatted(tr("Speed:"));
        for (const float speed : {0.0f, 1.0f, 60.0f, 600.0f, 3600.0f}) {
            ImGui::SameLine();
            const auto label = speed == 0.0f ? std::string(tr("pause")) : "x" + std::to_string(static_cast<int>(speed));
            const bool on = speed == 0.0f ? game.paused() : !game.paused() && game.clock_speed() == speed;
            if (ImGui::RadioButton((label + "##clock_speed").c_str(), on)) {
                if (speed == 0.0f) {
                    game.set_paused(true);
                } else {
                    game.set_paused(false);
                    game.set_clock(speed, game.season());
                }
            }
        }
        const char* seasons[] = {tr("Spring"), tr("Summer"), tr("Autumn"), tr("Winter")};
        int season = game.season();
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::Combo(tr("Season"), &season, seasons, 4)) game.set_clock(game.clock_speed(), static_cast<std::uint8_t>(season));
        ImGui::TextWrapped("%s", tr("RTS players see the time jump at each sync when the clock runs faster, and take a new season when they log in again."));

        ImGui::SeparatorText(tr("Weather"));
        auto weather = game.weather();
        bool changed = false;
        const char* weathers[] = {tr("Clear"), tr("Snow"), tr("Rain"), tr("Thunderstorm")};
        int kind = weather.weather < 4 ? weather.weather : 0;
        ImGui::BeginDisabled(weather.dynamic != 0);
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::Combo("##clock_weather", &kind, weathers, 4)) {
            weather.weather = static_cast<std::uint8_t>(kind);
            changed = true;
        }
        ImGui::EndDisabled();
        bool dynamic = weather.dynamic != 0;
        if (ImGui::Checkbox(tr("Dynamic weather (openRTS)"), &dynamic)) {
            weather.dynamic = dynamic ? 1 : 0;
            changed = true;
        }
        ImGui::TextDisabled("%s", tr(dynamic ? "(clouds, rain or snow and thunderstorms change by themselves, alike for every openRTS player)"
                                             : "(off: the weather chosen stays, as in RTS)"));
        if (dynamic) {
            if (ImGui::Button(tr("Another weather"))) {
                weather.seed = weather.seed * 6364136223846793005ull + 1442695040888963407ull;
                weather.clock = 0.0;
                changed = true;
            }
            bool by_season = weather.by_season != 0;
            if (ImGui::Checkbox(tr("By the season##weather"), &by_season)) {
                weather.by_season = by_season ? 1 : 0;
                changed = true;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", tr(by_season ? "(winter overcast with snow; spring and autumn rainy; summer clearer, with thunderstorms)"
                                                   : "(the climate below)"));
            const auto slider = [&](const char* label, float& value, const float scale, const float least, const float most, const char* format) {
                float shown = value * scale;
                ImGui::SetNextItemWidth(220.0f);
                if (ImGui::SliderFloat(label, &shown, least, most, format)) {
                    value = shown / scale;
                    changed = true;
                }
            };
            ImGui::BeginDisabled(by_season);
            slider(tr("Usual cloud cover"), weather.cloudiness, 100.0f, 0.0f, 100.0f, "%.0f %%");
            slider(tr("Spells with rain or snow"), weather.wet, 100.0f, 0.0f, 100.0f, "%.0f %%");
            slider(tr("Thunderstorms among the rains"), weather.storms, 100.0f, 0.0f, 100.0f, "%.0f %%");
            ImGui::EndDisabled();
            slider(tr("A weather lasts, game minutes"), weather.spell_minutes, 1.0f, 10.0f, 240.0f, "%.0f");
            slider(tr("Turns into the next over, game minutes"), weather.change_minutes, 1.0f, 1.0f, 60.0f, "%.0f");
        }
        if (changed) game.set_weather(weather);
        ImGui::TextWrapped("%s", tr("The weather reaches openRTS players at once; RTS players keep the weather of their login."));
    }

    void log_tab() {
        ImGui::BeginChild("log", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing() * 1.2f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        for (const auto& line : log_) ImGui::TextUnformatted(line.c_str());
        if (log_scroll_) {
            ImGui::SetScrollHereY(1.0f);
            log_scroll_ = false;
        }
        ImGui::EndChild();
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputTextWithHint("##command", tr("a console command (help)"), command_, sizeof command_, ImGuiInputTextFlags_EnterReturnsTrue)) {
            const std::string line(command_);
            command_[0] = 0;
            if (!line.empty()) run(line);
            ImGui::SetKeyboardFocusHere(-1);
        }
    }

    ServerSession& s_;
    trackmap::TrackMap map_;
    std::map<std::uint32_t, std::string> names_; // signal object -> its name
    std::deque<std::string> log_;
    bool log_scroll_ = false;
    char command_[512]{};
    char time_[16] = "12:00";
    float time_hours_ = 12.0f; // the Time and weather tab's slider
    bool time_dragged_ = false;
    std::string status_;
    bool status_error_ = false;
    double status_until_ = 0.0; // ImGui time it is shown until
    std::uint32_t selected_train_ = 0;
    trackmap::Pick menu_;
    // The map's "Задать маршрут" mode: the train, what is asked so far, the
    // way it would take (renewed when asked or every second).
    struct RouteEdit {
        bool active = false;
        std::uint32_t train{};
        GameServer::RouteRequest request;
        GameServer::RoutePreview preview;
        bool dirty = true;
        double at = -10.0;
    };
    RouteEdit routing_;
    std::vector<std::string> consists_;
    char consist_filter_[64]{};
    int consist_ = -1;
    int path_ = -1;
    // A new train's number (empty: the lowest free), name, kind (STATIC,
    // PLAYER, LOOSE, TRAFFIC) and state (cold, on, running): STATIC and cold
    // by default (the organiser, 2026-11-21).
    char new_number_[16]{};
    char new_name_[128]{};
    int new_kind_ = 0;
    int new_state_ = 0;
    int new_chocks_ = 0; // 0 none, 1 head, 2 tail, 3 both
    // Placements (placements_ui); the one opened: its includes and trains
    // (with the included ones'), the picked put or saved as another.
    char placement_name_[80]{};
    std::vector<std::string> placements_;
    bool placements_listed_ = false;
    std::string opened_;
    net::Placement opened_placement_;
    std::vector<net::PlacementTrain> opened_trains_;
    std::vector<std::string> opened_problems_;
    std::vector<char> opened_picked_;
    char subset_name_[80]{};
    // The train menu's number and name, for that train.
    std::uint32_t form_train_ = 0;
    char form_number_[16]{};
    char form_name_[128]{};
    std::vector<std::string> stations_;
    int station_ = -1;
    char route_filter_[64]{};
    bool back_ = false, hold_ = false;
    // "Собрать маршрут": the route picked, the one under the mouse, the
    // point the routes are looked for from and what is found there.
    std::optional<RouteChoice> picked_, hovered_;
    bool routes_tab_open_ = false;
    bool flip_ = false;
    std::string flip_key_, avail_key_, ways_key_;
    std::map<std::pair<std::size_t, bool>, std::optional<simulation::RouteLeg>> ways_;
    std::vector<StationRoutes::Choice> avail_;
    std::optional<simulation::TrackLocation> current_at_;
    bool current_rear_first_ = false;
    double behind_ = 0.0;
    std::string current_text_;
    int priority_ = 0;
    std::vector<std::string> types_;
    int type_ = -1;
    char message_[256]{};
    std::string player_;
    bool quit_ = false;
    bool seen_train_ = false;
    // The consist being placed (seen on the map, put when confirmed).
    struct Placement {
        bool active = false;
        std::string consist;
        std::vector<double> lengths, fronts; // each car's length and how far its front is behind the train's
        std::vector<bool> engines;
        double length{};
        simulation::TrackLocation head; // the front of car 0, facing the train's way
        std::vector<std::pair<simulation::TrackPoint, simulation::TrackPoint>> cars;
        bool fits = true;
        std::string overlaps; // the train it runs into, empty none
        int grabbed = -1;
        std::uint32_t moving = 0; // a standing train put again (replace_train), 0: a consist
    } placing_;
    char menu_filter_[64]{};
};

// The start form; true when "Start" has been pressed and the load may run.
bool draw_form(StartForm& form) {
    const auto& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f));
    ImGui::Begin(tr("openRTS server###start"), nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
    if (form.loading > 0) {
        ImGui::TextUnformatted(tr("Loading the route..."));
        ImGui::End();
        return ++form.loading > 2;
    }
    ImGui::TextUnformatted(tr("The RTS installation"));
    ImGui::SetNextItemWidth(-90.0f);
    ImGui::InputText("##root", form.root, sizeof form.root);
    ImGui::SameLine();
    if (ImGui::Button(tr("Look"))) form.list_routes();
    const char* route = form.route >= 0 && form.route < static_cast<int>(form.routes.size()) ? form.routes[static_cast<std::size_t>(form.route)].c_str()
                                                                                          : tr("a route...");
    if (ImGui::BeginCombo(tr("Route"), route)) {
        for (std::size_t i = 0; i < form.routes.size(); ++i) {
            if (ImGui::Selectable(form.routes[i].c_str(), form.route == static_cast<int>(i))) form.route = static_cast<int>(i);
        }
        ImGui::EndCombo();
    }
    const char* seasons[] = {tr("spring"), tr("summer"), tr("autumn"), tr("winter")};
    ImGui::Combo(tr("Season"), &form.season, seasons, 4);
    const char* weathers[] = {tr("clear"), tr("snow"), tr("rain"), tr("thunderstorm")};
    ImGui::Combo(tr("Weather"), &form.weather, weathers, 4);
    ImGui::SameLine();
    ImGui::Checkbox(tr("Dynamic (openRTS)"), &form.weather_dynamic);
    ImGui::InputText(tr("Time (HH:MM)"), form.time, sizeof form.time);
    ImGui::InputInt(tr("Port"), &form.port);
    form.port = std::clamp(form.port, 0, 65535); // 0: any free port
    ImGui::Checkbox(tr("Take new players"), &form.admit);
    ImGui::InputInt(tr("Places (0: no limit)"), &form.places);
    form.places = std::clamp(form.places, 0, 999);
    ImGui::InputText(tr("Dispatcher password"), form.password, sizeof form.password);
    ImGui::TextDisabled("%s", form.password[0] ? tr("The dispatcher's console from another computer logs in with this password.")
                                               : tr("No password: the dispatcher's console only from this computer."));
    ImGui::Checkbox(tr("Join from Discord"), &form.discord);
    ImGui::BeginDisabled(!form.discord);
    ImGui::InputTextWithHint(tr("Address for the players"), tr("as the players connect, e.g. 26.1.2.3:800"), form.address, sizeof form.address);
    ImGui::EndDisabled();
    ImGui::TextDisabled("%s", form.discord ? tr("openRTS players' Discord profiles get \"Join\": it hands over this address (empty: the one each player connected to).")
                                           : tr("Without it the players' Discord profiles show no \"Join\"."));
    ImGui::Checkbox(tr("Fast train positions (10 times a second)"), &form.fast_positions);
    ImGui::TextDisabled("%s", form.fast_positions ? tr("The others' trains are seen 0.2 s behind (off: 1.75 s).")
                                                  : tr("As in RTS: the others' trains are seen 1.75 s behind."));
    ImGui::TextUnformatted(tr("The server's folder (the route database it saves)"));
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##data", form.data, sizeof form.data);
    if (!form.error.empty()) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", form.error.c_str());
    bool start = false;
    ImGui::BeginDisabled(form.route < 0);
    if (ImGui::Button(tr("Start"), ImVec2(120.0f, 0.0f))) start = true;
    ImGui::EndDisabled();
    ImGui::End();
    if (start) {
        if (!parse_time_of_day(form.time)) {
            form.error = tr("the time is HH:MM or HH:MM:SS");
        } else {
            form.error.clear();
            form.loading = 1;
        }
    }
    return false;
}

} // namespace

int run_server_window(SessionSettings settings, const std::vector<std::string>& commands, const fs::path& capture, const double capture_at,
                      const WindowGiven given) {
#ifndef _WIN32
    (void)settings;
    (void)commands;
    (void)capture;
    (void)capture_at;
    std::cerr << "the server's window is Windows-only for now: --no-window --route <folder>" << std::endl;
    return 2;
#else
    if (!glfwInit()) {
        std::cerr << "error: no window system (glfwInit); run with --no-window" << std::endl;
        return 1;
    }
    auto device = renderer::create_wgpu_device(renderer::GraphicsBackend::automatic);
    if (!device.ok()) {
        std::cerr << "error: no GPU device: " << device.error << "; run with --no-window" << std::endl;
        glfwTerminate();
        return 1;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(1500, 900, "openRTS server", nullptr, nullptr);
    if (!window) {
        std::cerr << "error: the window could not be made; run with --no-window" << std::endl;
        glfwTerminate();
        return 1;
    }
    int exit_code = 0;
    {
        Surface surface(device.context, window);
        if (!surface.ok()) {
            std::cerr << "error: the window's surface could not be made; run with --no-window" << std::endl;
            glfwDestroyWindow(window);
            glfwTerminate();
            return 1;
        }
        if (settings.data.empty()) settings.data = core::find_user_data_directory();
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        const auto layout = utf8_path(settings.data / "server_imgui.ini");
        ImGui::GetIO().IniFilename = layout.c_str();
        ImGui::StyleColorsDark();
        ui::load_fonts();
        ImGui_ImplGlfw_InitForOther(window, true);
        ImGui_ImplWGPU_InitInfo info{};
        info.Device = device.context.device();
        info.NumFramesInFlight = 3;
        info.RenderTargetFormat = surface.format();
        info.DepthStencilFormat = WGPUTextureFormat_Undefined;
        ImGui_ImplWGPU_Init(&info);

        StartForm form;
        std::snprintf(form.root, sizeof form.root, "%s", utf8_path(settings.root).c_str());
        std::snprintf(form.data, sizeof form.data, "%s", utf8_path(settings.data).c_str());
        form.season = settings.season;
        form.weather = settings.weather;
        std::snprintf(form.time, sizeof form.time, "%s", clock_text(settings.time_of_day).substr(0, 5).c_str());
        form.port = settings.port;
        form.admit = settings.admit;
        std::snprintf(form.password, sizeof form.password, "%s", settings.dispatcher_password.c_str());
        form.discord = settings.discord_join;
        form.fast_positions = settings.fast_positions;
        if (settings.max_players > 0) form.places = static_cast<int>(settings.max_players);
        std::snprintf(form.address, sizeof form.address, "%s", settings.public_address.c_str());
        form.list_routes();
        form.pick_route(settings.route);
        // Started without a route: the form as it was last started, but for
        // what the command line gave (the start menu: the folder, the port).
        const auto remembered = settings.data / "server_window.ini";
        if (settings.route.empty()) {
            form.load(remembered);
            if (given.root) {
                std::snprintf(form.root, sizeof form.root, "%s", utf8_path(settings.root).c_str());
                const auto kept = form.route >= 0 ? form.routes[static_cast<std::size_t>(form.route)] : std::string();
                form.list_routes();
                form.route = -1;
                form.pick_route(kept);
            }
            if (given.port) form.port = settings.port;
            if (given.password) std::snprintf(form.password, sizeof form.password, "%s", settings.dispatcher_password.c_str());
        }
        // A check (--capture) starts the route given at once.
        if (!capture.empty() && form.route >= 0) form.loading = 1;

        std::unique_ptr<ServerSession> session;
        std::unique_ptr<Console> console;
        const auto opened = std::chrono::steady_clock::now();
        std::optional<std::chrono::steady_clock::time_point> started;
        const auto queue = wgpuDeviceGetQueue(device.context.device());
        // The game steps on a thread of its own, every 20 ms as without the
        // window: a slow window (a computer without a video card draws it
        // in software) no longer slows the game (2026-11-21: on such a
        // server the players' world came in seconds apart). The console
        // reads and changes the game under the same lock; the drawing and
        // the present are outside it.
        std::mutex game_lock;
        std::atomic<bool> stepping = false;
        std::thread stepper;
        const auto stop_stepping = [&] {
            stepping = false;
            if (stepper.joinable()) stepper.join();
        };
        bool running = true;
        while (running && !glfwWindowShouldClose(window)) {
            glfwPollEvents();
            int width = 0, height = 0;
            glfwGetFramebufferSize(window, &width, &height);
            surface.resize(width, height);
            const auto now = std::chrono::steady_clock::now();

            ImGui_ImplWGPU_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            if (console) {
                const std::lock_guard lock(game_lock);
                running = console->draw();
            } else if (draw_form(form)) {
                SessionSettings chosen = settings;
                chosen.root = path_of(form.root);
                chosen.data = path_of(form.data);
                chosen.route = form.routes[static_cast<std::size_t>(form.route)];
                chosen.season = static_cast<std::uint8_t>(form.season);
                chosen.weather = static_cast<std::uint8_t>(form.weather);
                chosen.weather_dynamic = form.weather_dynamic;
                chosen.time_of_day = parse_time_of_day(form.time).value_or(12.0 * 3600.0);
                chosen.port = static_cast<std::uint16_t>(form.port);
                chosen.admit = form.admit;
                chosen.dispatcher_password = form.password;
                chosen.discord_join = form.discord;
                chosen.fast_positions = form.fast_positions;
                chosen.max_players = static_cast<std::uint32_t>(form.places);
                chosen.public_address = form.address;
                std::string error;
                std::vector<std::string> lines;
                std::error_code made;
                fs::create_directories(remembered.parent_path(), made);
                form.save(remembered);
                session = ServerSession::start(chosen, error, lines);
                form.loading = 0;
                if (!session) {
                    form.error = error;
                } else {
                    started = std::chrono::steady_clock::now();
                    console = std::make_unique<Console>(*session);
                    for (const auto& line : lines) console->log(line);
                    for (const auto& line : commands) console->run(line);
                    glfwSetWindowTitle(window, ("openRTS server: " + chosen.route).c_str());
                    stepping = true;
                    stepper = std::thread([&session, &game_lock, &stepping, start = *started] {
                        while (stepping) {
                            {
                                const std::lock_guard lock(game_lock);
                                session->step(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
                            }
                            std::this_thread::sleep_for(std::chrono::milliseconds(20));
                        }
                    });
                }
            }
            ImGui::Render();
            // A check: the window's picture after `capture_at` s, then it closes.
            const auto since = std::chrono::duration<double>(now - (started ? *started : opened)).count();
            if (!capture.empty() && (started || form.loading == 0) && since >= capture_at) {
                exit_code = capture_frame(device.context, surface.format(), surface.width(), surface.height(), capture) ? 0 : 1;
                std::cout << (exit_code == 0 ? "window captured to " : "the capture failed: ") << utf8_path(capture) << std::endl;
                break;
            }
            if (const auto frame = surface.acquire()) {
                const auto encoder = wgpuDeviceCreateCommandEncoder(device.context.device(), nullptr);
                render_frame(device.context.device(), frame, encoder);
                const auto commands_buffer = wgpuCommandEncoderFinish(encoder, nullptr);
                wgpuQueueSubmit(queue, 1, &commands_buffer);
                wgpuCommandBufferRelease(commands_buffer);
                wgpuCommandEncoderRelease(encoder);
                surface.present();
                wgpuTextureRelease(frame);
            }
        }
        stop_stepping();
        wgpuQueueRelease(queue);
        console.reset();
        session.reset();
        ImGui_ImplWGPU_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
    }
    glfwDestroyWindow(window);
    glfwTerminate();
    return exit_code;
#endif
}

} // namespace openrts::server
