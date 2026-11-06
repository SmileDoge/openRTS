#include "window_viewer.hpp"

#ifdef OPENRTS_HAS_WGPU
#include <openrts/renderer/depth.hpp>
#include <openrts/renderer/tone_map.hpp>
#include <openrts/renderer/depth_export.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <glm/ext/matrix_clip_space.hpp>

#include <openrts/renderer/route_space.hpp>
#include <openrts/renderer/scene_effects.hpp>
#include <openrts/renderer/webgpu_context.hpp>

#include "frame_limiter.hpp"
#include "gpu_timer.hpp"
#include "key_layout.hpp"
#include "ui_fonts.hpp"
#include "ui_text.hpp"
#include "rts_config.hpp"
#include "viewer_state.hpp"

#include <webgpu/wgpu.h>

#include "exit_codes.hpp"

#if defined(OPENRTS_HAS_GLFW) && defined(_WIN32)
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#ifdef OPENRTS_HAS_IMGUI
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_wgpu.h>
#endif

namespace openrts::viewer {
namespace {

constexpr int initial_width = 1280;
constexpr int initial_height = 720;
// The vertical field of view, degrees, when neither the panel nor RTS's
// config (FOV) gives one.
constexpr float default_fov_degrees = 48.7f;
constexpr double max_frame_seconds = 0.1;

class GlfwSession {
public:
    GlfwSession() : initialized_(glfwInit() == GLFW_TRUE) {}
    ~GlfwSession() {
        if (initialized_) glfwTerminate();
    }
    GlfwSession(const GlfwSession&) = delete;
    GlfwSession& operator=(const GlfwSession&) = delete;

    [[nodiscard]] bool ok() const noexcept { return initialized_; }

private:
    bool initialized_;
};

// Where a window was left, as viewer_state.ini keeps it ("x y width height
// maximized"): its restored rectangle (the content area, screen
// coordinates) and whether it was maximized. The game and the start menu
// keep one each (window_game, window_menu).
struct WindowPlacement {
    int x = 0;
    int y = 0;
    int width = initial_width;
    int height = initial_height;
    bool positioned = false; // x and y are known
    bool maximized = false;
    bool operator==(const WindowPlacement&) const = default;
};

[[nodiscard]] std::optional<WindowPlacement> remembered_placement(const std::string_view key) {
    const auto value = remembered(key);
    if (!value) return std::nullopt;
    std::istringstream fields(*value);
    WindowPlacement placement;
    int maximized = 0;
    if (!(fields >> placement.x >> placement.y >> placement.width >> placement.height >> maximized)) return std::nullopt;
    placement.width = std::clamp(placement.width, 320, 16384);
    placement.height = std::clamp(placement.height, 200, 16384);
    placement.positioned = true;
    placement.maximized = maximized != 0;
    return placement;
}

// Whether the title bar of a window placed so stays on a screen: a monitor
// unplugged since leaves the window where the system puts it.
[[nodiscard]] bool on_a_monitor(const WindowPlacement& placement) {
    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    for (int i = 0; i < count; ++i) {
        int x = 0, y = 0, width = 0, height = 0;
        glfwGetMonitorWorkarea(monitors[i], &x, &y, &width, &height);
        const int left = (std::max)(placement.x, x);
        const int right = (std::min)(placement.x + placement.width, x + width);
        const int top = (std::max)(placement.y - 30, y);
        const int bottom = (std::min)(placement.y, y + height);
        if (right - left >= 100 && bottom - top >= 15) return true;
    }
    return false;
}

// Follows the window's place and size and writes them a second after they
// settle, and when the window goes (a crash later keeps them too).
class PlacementKeeper {
public:
    PlacementKeeper(std::string key, const WindowPlacement& start) : key_(std::move(key)), last_(start) {}
    ~PlacementKeeper() {
        if (dirty_) save();
    }
    PlacementKeeper(const PlacementKeeper&) = delete;
    PlacementKeeper& operator=(const PlacementKeeper&) = delete;

    void update(GLFWwindow* window, const double now) {
        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) return;
        auto placement = last_;
        placement.maximized = glfwGetWindowAttrib(window, GLFW_MAXIMIZED) != 0;
        // Maximized, the restored rectangle stays the one seen last.
        if (!placement.maximized) {
            glfwGetWindowPos(window, &placement.x, &placement.y);
            glfwGetWindowSize(window, &placement.width, &placement.height);
            placement.positioned = true;
        }
        if (placement.width <= 0 || placement.height <= 0) return;
        if (placement != last_) {
            last_ = placement;
            changed_at_ = now;
            dirty_ = true;
        }
        if (dirty_ && now - changed_at_ > 1.0) save();
    }

private:
    void save() {
        dirty_ = false;
        if (!last_.positioned) return;
        remember(key_, std::to_string(last_.x) + ' ' + std::to_string(last_.y) + ' ' + std::to_string(last_.width) + ' '
                           + std::to_string(last_.height) + ' ' + (last_.maximized ? '1' : '0'));
    }

    std::string key_;
    WindowPlacement last_;
    double changed_at_ = 0.0;
    bool dirty_ = false;
};

class Window {
public:
    Window(const int width, const int height, const char* title) {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        handle_ = glfwCreateWindow(width, height, title, nullptr, nullptr);
    }
    ~Window() {
        if (handle_) glfwDestroyWindow(handle_);
    }
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    [[nodiscard]] GLFWwindow* get() const noexcept { return handle_; }

private:
    GLFWwindow* handle_ = nullptr;
};

// Swap chain surface plus the matching depth buffer; both follow window size.
class PresentationSurface {
public:
    PresentationSurface(const renderer::WgpuDeviceContext& context, GLFWwindow* window, const bool copy_source = false)
        : device_(context.device()) {
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
        // A capture copies the frame out; where the surface cannot be a copy
        // source (OpenGL), the frame is drawn into a texture of its own.
        const bool copy_supported = (capabilities.usages & WGPUTextureUsage_CopySrc) != 0;
        offscreen_capture_ = copy_source && !copy_supported;
        // Fifo (vsync) is always available; without vsync prefer Immediate,
        // then Mailbox (no tearing, still uncapped).
        for (std::size_t i = 0; i < capabilities.presentModeCount; ++i) {
            const auto mode = capabilities.presentModes[i];
            if (mode == WGPUPresentMode_Immediate) uncapped_mode_ = mode;
            if (mode == WGPUPresentMode_Mailbox && uncapped_mode_ != WGPUPresentMode_Immediate) uncapped_mode_ = mode;
        }
        if (capabilities.formatCount > 0 && capabilities.alphaModeCount > 0) {
            // A plain (non-sRGB) target: the shaders write display values
            // as RTS's GL framebuffer took them; an sRGB surface (often the
            // first offered, e.g. Vulkan on NVIDIA) would encode them again
            // and brighten everything.
            WGPUTextureFormat format = capabilities.formats[0];
            for (const auto wanted : {WGPUTextureFormat_BGRA8Unorm, WGPUTextureFormat_RGBA8Unorm}) {
                bool offered = false;
                for (std::size_t i = 0; i < capabilities.formatCount; ++i) offered = offered || capabilities.formats[i] == wanted;
                if (offered) {
                    format = wanted;
                    break;
                }
            }
            std::cout << "surface format: " << static_cast<int>(format) << " (first offered "
                      << static_cast<int>(capabilities.formats[0]) << ")" << std::endl;
            config_ = {
                .device = device_,
                .format = format,
                .usage = WGPUTextureUsage_RenderAttachment | (copy_source && copy_supported ? WGPUTextureUsage_CopySrc : WGPUTextureUsage_None),
                .width = initial_width,
                .height = initial_height,
                .alphaMode = capabilities.alphaModes[0],
                .presentMode = WGPUPresentMode_Fifo,
            };
            configured_ = true;
        }
        wgpuSurfaceCapabilitiesFreeMembers(capabilities);

        if (configured_) {
            wgpuSurfaceConfigure(surface_, &config_);
            recreate_depth();
        }
    }

    ~PresentationSurface() {
        release_depth();
        if (configured_) wgpuSurfaceUnconfigure(surface_);
        if (surface_) wgpuSurfaceRelease(surface_);
    }

    PresentationSurface(const PresentationSurface&) = delete;
    PresentationSurface& operator=(const PresentationSurface&) = delete;

    [[nodiscard]] bool ok() const noexcept { return surface_ && configured_; }
    [[nodiscard]] WGPUTextureFormat format() const noexcept { return config_.format; }
    [[nodiscard]] WGPUTextureView depth_view() const noexcept { return depth_view_; }
    // The multisampled colour target (resolved into the frame), or nullptr
    // without multisampling.
    [[nodiscard]] WGPUTextureView msaa_view() const noexcept { return msaa_view_; }

    // False when the surface only offers Fifo, so vsync cannot be turned off.
    [[nodiscard]] bool vsync_optional() const noexcept { return uncapped_mode_ != WGPUPresentMode_Fifo; }
    [[nodiscard]] bool vsync() const noexcept { return config_.presentMode == WGPUPresentMode_Fifo; }
    [[nodiscard]] const char* present_mode_name() const noexcept {
        switch (config_.presentMode) {
        case WGPUPresentMode_Immediate:
            return "Immediate";
        case WGPUPresentMode_Mailbox:
            return "Mailbox";
        default:
            return "Fifo";
        }
    }

    void set_vsync(const bool enabled) {
        const auto mode = enabled ? WGPUPresentMode_Fifo : uncapped_mode_;
        if (!configured_ || config_.presentMode == mode) return;
        config_.presentMode = mode;
        wgpuSurfaceConfigure(surface_, &config_);
    }

    void resize(const std::uint32_t width, const std::uint32_t height) {
        if (config_.width == width && config_.height == height) return;
        config_.width = width;
        config_.height = height;
        wgpuSurfaceConfigure(surface_, &config_);
        recreate_depth();
    }

    // The scene depth stays after the frame and can be sampled (the copy for
    // ReShade, renderer::DepthExport); remakes the depth buffer when it changes.
    void set_depth_export(const bool on) {
        if (depth_export_ == on) return;
        depth_export_ = on;
        recreate_depth();
    }
    [[nodiscard]] bool depth_export() const noexcept { return depth_export_; }

    // Returns the frame texture, or nullptr when the surface has nothing to draw into.
    [[nodiscard]] WGPUTexture acquire() const {
        if (offscreen_) {
            wgpuTextureAddRef(offscreen_);
            return offscreen_;
        }
        WGPUSurfaceTexture frame{};
        wgpuSurfaceGetCurrentTexture(surface_, &frame);
        const bool usable = frame.status == WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal
            || frame.status == WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal;
        return usable ? frame.texture : nullptr;
    }

    void present() const {
        if (!offscreen_) wgpuSurfacePresent(surface_);
    }

private:
    void recreate_depth() {
        release_depth();
        const auto samples = renderer::render_sample_count();
        const WGPUTextureDescriptor descriptor{
            .usage = WGPUTextureUsage_RenderAttachment | (depth_export_ ? WGPUTextureUsage_TextureBinding : WGPUTextureUsage_None),
            .dimension = WGPUTextureDimension_2D,
            .size = {.width = config_.width, .height = config_.height, .depthOrArrayLayers = 1},
            .format = renderer::depth_format,
            .mipLevelCount = 1,
            .sampleCount = samples,
        };
        depth_ = wgpuDeviceCreateTexture(device_, &descriptor);
        if (offscreen_capture_) {
            const WGPUTextureDescriptor frame{
                .usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc,
                .dimension = WGPUTextureDimension_2D,
                .size = {.width = config_.width, .height = config_.height, .depthOrArrayLayers = 1},
                .format = config_.format,
                .mipLevelCount = 1,
                .sampleCount = 1,
            };
            offscreen_ = wgpuDeviceCreateTexture(device_, &frame);
        }
        if (depth_) depth_view_ = wgpuTextureCreateView(depth_, nullptr);
        if (samples > 1) {
            const WGPUTextureDescriptor colour{
                .usage = WGPUTextureUsage_RenderAttachment,
                .dimension = WGPUTextureDimension_2D,
                .size = {.width = config_.width, .height = config_.height, .depthOrArrayLayers = 1},
                .format = scene_format(),
                .mipLevelCount = 1,
                .sampleCount = samples,
            };
            msaa_ = wgpuDeviceCreateTexture(device_, &colour);
            if (msaa_) msaa_view_ = wgpuTextureCreateView(msaa_, nullptr);
        }
        if (renderer::scene_hdr()) {
            const WGPUTextureDescriptor hdr{
                .usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding,
                .dimension = WGPUTextureDimension_2D,
                .size = {.width = config_.width, .height = config_.height, .depthOrArrayLayers = 1},
                .format = renderer::hdr_format,
                .mipLevelCount = 1,
                .sampleCount = 1,
            };
            hdr_ = wgpuDeviceCreateTexture(device_, &hdr);
            if (hdr_) hdr_view_ = wgpuTextureCreateView(hdr_, nullptr);
        }
    }

public:
    // The format the scene is drawn in: the screen's, or HDR.
    [[nodiscard]] WGPUTextureFormat scene_format() const noexcept {
        return renderer::scene_hdr() ? renderer::hdr_format : config_.format;
    }
    // The HDR frame the scene resolves into (null without HDR).
    [[nodiscard]] WGPUTextureView hdr_view() const noexcept { return hdr_view_; }

private:

    void release_depth() {
        if (depth_view_) wgpuTextureViewRelease(depth_view_);
        if (depth_) wgpuTextureRelease(depth_);
        if (msaa_view_) wgpuTextureViewRelease(msaa_view_);
        if (msaa_) wgpuTextureRelease(msaa_);
        if (hdr_view_) wgpuTextureViewRelease(hdr_view_);
        if (hdr_) wgpuTextureRelease(hdr_);
        if (offscreen_) wgpuTextureRelease(offscreen_);
        offscreen_ = nullptr;
        depth_view_ = nullptr;
        depth_ = nullptr;
        msaa_view_ = nullptr;
        msaa_ = nullptr;
        hdr_view_ = nullptr;
        hdr_ = nullptr;
    }

    WGPUTexture hdr_ = nullptr;
    WGPUTextureView hdr_view_ = nullptr;
    WGPUTexture offscreen_ = nullptr; // the frame of a capture the surface cannot copy
    bool offscreen_capture_ = false;
    bool depth_export_ = false;
    WGPUDevice device_ = nullptr;
    WGPUSurface surface_ = nullptr;
    WGPUSurfaceConfiguration config_{};
    bool configured_ = false;
    WGPUPresentMode uncapped_mode_ = WGPUPresentMode_Fifo;
    WGPUTexture depth_ = nullptr;
    WGPUTextureView depth_view_ = nullptr;
    WGPUTexture msaa_ = nullptr;
    WGPUTextureView msaa_view_ = nullptr;
};

// CPU stages of one frame, in loop order.
enum class FrameStage { update, panel, wait, begin, background, cull, prepare, effects, models, foreground, finish, submit, count };
constexpr std::size_t frame_stage_count = static_cast<std::size_t>(FrameStage::count);
constexpr std::array<const char*, frame_stage_count> frame_stage_names{
    "update",  // input, camera, scene update (streaming, track, later simulation)
    "panel",   // ImGui panel
    "wait",    // waiting for the swap chain image (vsync and GPU back-pressure)
    "begin",   // frame view, encoder and main pass
    "backgr",  // sky and terrain
    "cull",    // scene culling and model submission
    "prepare", // model batching and the frame's uploads
    "effects", // recording the shadow cascades, the depth pre-pass, GTAO and the clusters
    "models",  // model batching and draw recording
    "foregr",  // scene overlays (track lines) and the panel
    "finish",  // end of pass and command buffer
    "submit",  // queue submit, present, retired resource collection
};
using StageTimes = std::array<double, frame_stage_count>;

// Splits the frame into stages on the CPU clock.
class StageClock {
public:
    void start() { last_ = std::chrono::steady_clock::now(); }
    void mark(const FrameStage stage, StageTimes& times) {
        const auto now = std::chrono::steady_clock::now();
        times[static_cast<std::size_t>(stage)] += std::chrono::duration<double>(now - last_).count();
        last_ = now;
    }

private:
    std::chrono::steady_clock::time_point last_;
};

// Frame rate over the last half second: average FPS and frame time, the
// slowest frame (stutter shows there first), the average of each CPU stage
// and the GPU time of the main pass.
class FrameStats {
public:
    void add(const double seconds, const StageTimes& stages, const double gpu_sum_ms, const int gpu_count) {
        window_ += seconds;
        worst_ = (std::max)(worst_, seconds);
        for (std::size_t i = 0; i < frame_stage_count; ++i) stage_sum_[i] += stages[i];
        gpu_sum_ += gpu_sum_ms;
        gpu_count_ += gpu_count;
        ++frames_;
        if (window_ < 0.5) return;
        fps_ = frames_ / window_;
        average_ms_ = window_ * 1000.0 / frames_;
        worst_ms_ = worst_ * 1000.0;
        for (std::size_t i = 0; i < frame_stage_count; ++i) stage_ms_[i] = stage_sum_[i] * 1000.0 / frames_;
        if (gpu_count_ > 0) gpu_ms_ = gpu_sum_ / gpu_count_;
        window_ = 0.0;
        worst_ = 0.0;
        stage_sum_ = {};
        gpu_sum_ = 0.0;
        gpu_count_ = 0;
        frames_ = 0;
    }

    [[nodiscard]] double fps() const noexcept { return fps_; }
    [[nodiscard]] double average_ms() const noexcept { return average_ms_; }
    [[nodiscard]] double worst_ms() const noexcept { return worst_ms_; }
    [[nodiscard]] const StageTimes& stage_ms() const noexcept { return stage_ms_; }
    [[nodiscard]] double gpu_ms() const noexcept { return gpu_ms_; }

private:
    double window_{};
    double worst_{};
    int frames_{};
    StageTimes stage_sum_{};
    double gpu_sum_{};
    int gpu_count_{};
    double fps_{};
    double average_ms_{};
    double worst_ms_{};
    StageTimes stage_ms_{};
    double gpu_ms_{};
};

// Panel controls that the main loop acts on after the panel is built.
struct PanelState {
    FrameStats stats;
    const char* present_mode = "Fifo";
    bool vsync = true;
    bool vsync_optional = false;
    int fps_limit = 0; // 0: none, else 30..500
    bool gpu_timing = false;
    audio::AudioEngine* sound = nullptr;
    bool save_view = false;
    std::string save_status;
    // RTS's Pause key (0x402FC3): the world stands still, the camera moves.
    bool paused = false;
    bool menu = false; // the scene draws the whole interface
    bool hdr = false;  // the frame is HDR: the tone curve and exposure apply
    std::string_view backend_now = "-"; // the graphics API this run draws through
    bool reshade_depth = false;         // keep the depth and copy it single-sample (INSTALL.md, ReShade)
    // False on OpenGL: the experimental path's compute shaders read depth
    // textures with textureLoad, which wgpu's GLSL output does not support.
    bool experimental_supported = true;
    int backend_choice = 0;             // the remembered one: auto, vulkan, dx12, opengl
    // 0 none (RTS look), 1 Khronos PBR Neutral, 2 AgX, 3 ACES, 4 Khronos by
    // day and AgX by night: in the experimental path.
    int tone_curve = 4;
    float exposure = 1.0f;
    float night_exposure = 2.0f;
    float night = 0.0f; // 0 day .. 1 night, from the sun (shown)
    float saturation = 1.0f;
    bool physical_light = false; // physical falloff of headlights and lamps (experimental renderer)
    float night_ambient = 1.0f;  // sky light at night, a factor (experimental renderer)
    float light_range = 0.0f;    // lamps lit within this distance, m; 0: RTS's MaxLightDistance (experimental renderer)
    float headlight_reach = 1.0f; // the trains' headlights reach this many times the .eng's (experimental renderer)
    bool light_fullbright = false; // FullBright surfaces lit as ordinary ones (experimental renderer)
    float fov = default_fov_degrees; // vertical field of view, degrees
    // The experimental path's effects (renderer::SceneEffects).
    renderer::SceneEffectSettings effects;
    // Shadow cascades and the depth pre-pass recorded on worker threads.
    bool parallel_recording = true;
};

// World clock: time of day advanced by real frame time times `speed`.
struct ClockControls {
    double time_of_day = 12.0 * 3600.0;
    float speed = 1.0f;
    int season = static_cast<int>(renderer::rts::Season::summer);
    int weather = static_cast<int>(renderer::rts::Weather::clear);

    [[nodiscard]] SceneClock advance(const float elapsed) {
        constexpr double day = renderer::rts::seconds_per_day;
        time_of_day = std::fmod(time_of_day + static_cast<double>(elapsed) * speed, day);
        if (time_of_day < 0.0) time_of_day += day;
        return {
            .time_of_day = static_cast<std::int32_t>(time_of_day),
            .elapsed = elapsed,
            .season = static_cast<renderer::rts::Season>(season),
            .weather = static_cast<renderer::rts::Weather>(weather),
        };
    }
};

#ifdef OPENRTS_HAS_IMGUI
class DebugOverlay {
public:
    DebugOverlay(GLFWwindow* window, const WGPUDevice device, const WGPUTextureFormat target_format, const bool menu) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        // Where the windows were left: imgui.ini beside the settings (UTF-8,
        // as ImGui opens files). The start menu keeps a file of its own:
        // it stays open while the game runs, and ImGui writes its whole
        // layout when it closes, which would put back the game's windows
        // as they were before the game moved them.
        layout_file_ = [menu] {
            const auto text = (user_data_directory() / (menu ? "imgui_menu.ini" : "imgui.ini")).u8string();
            return std::string(text.begin(), text.end());
        }();
        ImGui::GetIO().IniFilename = layout_file_.c_str();
        ImGui::StyleColorsDark();
        load_font();
        // Callbacks are forwarded by the viewer's own GLFW handlers.
        ImGui_ImplGlfw_InitForOther(window, false);

        ImGui_ImplWGPU_InitInfo info{};
        info.Device = device;
        info.NumFramesInFlight = 3;
        info.RenderTargetFormat = target_format;
        // In the main pass it shares the scene's depth; with HDR it draws in a pass of its own.
        info.DepthStencilFormat = renderer::scene_hdr() ? WGPUTextureFormat_Undefined : renderer::depth_format;
        info.PipelineMultisampleState.count = renderer::scene_hdr() ? 1u : renderer::render_sample_count();
        initialized_ = ImGui_ImplWGPU_Init(&info);
    }

    ~DebugOverlay() {
        if (initialized_) ImGui_ImplWGPU_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
    }

    DebugOverlay(const DebugOverlay&) = delete;
    DebugOverlay& operator=(const DebugOverlay&) = delete;
    DebugOverlay(DebugOverlay&&) = delete;
    DebugOverlay& operator=(DebugOverlay&&) = delete;

    [[nodiscard]] bool ok() const noexcept { return initialized_; }

    void build(const ViewerScene& scene,
               const ViewerCamera& camera,
               renderer::ModelRenderer& renderer,
               ClockControls& clock,
               const bool time_of_day_applies,
               PanelState& panel) const {
        ImGui_ImplWGPU_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        if (panel.menu) {
            scene.draw_overlay(camera);
            ImGui::Render();
            return;
        }
        if (panel.paused) {
            // As RTS writes it over the picture.
            const char* text = tr("PAUSE (Pause)");
            const auto display = ImGui::GetIO().DisplaySize;
            const auto size = ImGui::CalcTextSize(text);
            auto* list = ImGui::GetForegroundDrawList();
            const ImVec2 at{(display.x - size.x) * 0.5f, display.y * 0.12f};
            list->AddRectFilled({at.x - 10.0f, at.y - 6.0f}, {at.x + size.x + 10.0f, at.y + size.y + 6.0f}, 0xb0000000u, 4.0f);
            list->AddText(at, 0xff40e0ffu, text);
        }
        ImGui::Begin(tr("openRTS viewer###viewer"));
        // What is open and how fast it runs; then the settings, a section
        // each, one setting a line, top to bottom.
        // Scene titles are literals: their text is NUL-terminated.
        ImGui::TextUnformatted(tr(scene.title().data()));
        ImGui::Text(tr("FPS %.0f  |  %.2f ms avg, %.2f ms worst"), panel.stats.fps(), panel.stats.average_ms(),
                    panel.stats.worst_ms());

        if (ImGui::CollapsingHeader(tr("General"), ImGuiTreeNodeFlags_DefaultOpen)) {
            // Language of the interface (Russian by default), remembered.
            int language = ui_language() == UiLanguage::russian ? 0 : 1;
            const char* languages[] = {"Русский", "English"};
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::Combo("Язык / Language", &language, languages, 2)) {
                set_ui_language(language == 0 ? UiLanguage::russian : UiLanguage::english);
                remember("language", language == 0 ? "ru" : "en");
            }
            // The engineer's keys: RTS's (numeric keypad) or openRTS's.
            int layout = key_layout() == KeyLayout::rts ? 0 : 1;
            const char* layouts[] = {tr("RTS (Num0 / Num1)"), tr("openRTS (no numeric keypad)")};
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::Combo(tr("Keys"), &layout, layouts, 2)) {
                set_key_layout(layout == 0 ? KeyLayout::rts : KeyLayout::openrts);
                remember("key_layout", layout == 0 ? "rts" : "openrts");
            }
            ImGui::Checkbox(tr("Pause (Pause)"), &panel.paused);
        }

        if (ImGui::CollapsingHeader(tr("Graphics"), ImGuiTreeNodeFlags_DefaultOpen)) {
            // The lighting: RTS's or the experimental one (R); not on OpenGL.
            auto path = renderer.shading_path();
            int path_index = path == renderer::ShadingPath::experimental ? 1 : 0;
            const char* paths[] = {tr("RTS"), tr("Experimental")};
            ImGui::BeginDisabled(!panel.experimental_supported);
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::Combo(tr("Renderer (R)"), &path_index, paths, 2))
                path = path_index == 1 ? renderer::ShadingPath::experimental : renderer::ShadingPath::rts_compatible;
            ImGui::EndDisabled();
            if (!panel.experimental_supported && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", tr("Not available on OpenGL: choose Vulkan or DirectX 12 (Graphics API) and restart."));
            renderer.set_shading_path(path);
            // The graphics API, from the next start (the device is made once).
            const char* backends[] = {tr("Automatic"), "Vulkan", "DirectX 12", "OpenGL"};
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::Combo(tr("Graphics API"), &panel.backend_choice, backends, 4))
                remember("backend", renderer::graphics_backend_key(static_cast<renderer::GraphicsBackend>(panel.backend_choice)));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("Takes effect after a restart. Automatic: Vulkan, else DirectX 12. OpenGL is for old cards; the experimental renderer may not work there."));
            ImGui::TextDisabled("%s", trf("now: %s", std::string(panel.backend_now).c_str()).c_str());
            ImGui::BeginDisabled(!panel.vsync_optional);
            const auto vsync_label = std::string(tr("VSync")) + " (" + panel.present_mode + ")###vsync";
            ImGui::Checkbox(vsync_label.c_str(), &panel.vsync);
            ImGui::EndDisabled();
            {
                // One slider: 30..500, its right end "no limit".
                int shown = panel.fps_limit == 0 ? max_fps_limit + 1 : panel.fps_limit;
                const std::string format = shown > max_fps_limit ? std::string(tr("no limit")) : std::string(tr("%d frames/s"));
                ImGui::SetNextItemWidth(220.0f);
                if (ImGui::SliderInt(tr("FPS limit"), &shown, min_fps_limit, max_fps_limit + 1, format.c_str(), ImGuiSliderFlags_AlwaysClamp)) {
                    panel.fps_limit = shown > max_fps_limit ? 0 : shown;
                    remember("fps_limit", std::to_string(panel.fps_limit));
                }
            }
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::SliderFloat(tr("Field of view"), &panel.fov, 30.0f, 100.0f, tr("%.0f degrees")))
                remember("fov", std::to_string(panel.fov));
            ImGui::BeginDisabled(!panel.experimental_supported);
            if (ImGui::Checkbox(tr("Depth for ReShade"), &panel.reshade_depth))
                remember("reshade_depth", panel.reshade_depth ? "1" : "0");
            ImGui::EndDisabled();
            if (!panel.experimental_supported && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", tr("Not available on OpenGL: choose Vulkan or DirectX 12 (Graphics API) and restart."));
            else if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("Keeps the scene depth to the end of the frame and, with anti-aliasing, adds a copy ReShade can take. In ReShade: RESHADE_DEPTH_INPUT_IS_REVERSED 1, RESHADE_DEPTH_MULTIPLIER 20 (see INSTALL.md)."));
            bool batching = renderer.batching();
            if (ImGui::Checkbox(tr("Merge identical primitives (instancing)"), &batching)) renderer.set_batching(batching);
            bool culling = renderer.face_culling();
            if (ImGui::Checkbox(tr("Cull back faces (RTS)"), &culling)) renderer.set_face_culling(culling);
            if (panel.hdr) {
                ImGui::SeparatorText("HDR");
                const char* curves[] = {tr("none (as RTS)"), "Khronos PBR Neutral", "AgX (punchy)", "ACES",
                                        tr("auto: Khronos by day, AgX by night")};
                ImGui::SetNextItemWidth(220.0f);
                if (ImGui::Combo(tr("Tone curve (experimental renderer)"), &panel.tone_curve, curves, 5))
                    remember("tone_mode", std::to_string(panel.tone_curve));
                ImGui::SetNextItemWidth(220.0f);
                if (ImGui::SliderFloat(tr("Exposure"), &panel.exposure, 0.25f, 4.0f, "%.2f", ImGuiSliderFlags_Logarithmic))
                    remember("exposure", std::to_string(panel.exposure));
                if (panel.tone_curve == 4) {
                    ImGui::SetNextItemWidth(220.0f);
                    if (ImGui::SliderFloat(tr("Night exposure"), &panel.night_exposure, 0.5f, 6.0f, "%.2f", ImGuiSliderFlags_Logarithmic))
                        remember("night_exposure", std::to_string(panel.night_exposure));
                    ImGui::TextDisabled(tr("night %.0f %%"), panel.night * 100.0f);
                }
                ImGui::SetNextItemWidth(220.0f);
                if (ImGui::SliderFloat(tr("Saturation"), &panel.saturation, 0.5f, 1.8f, "%.2f"))
                    remember("saturation", std::to_string(panel.saturation));
            }
        }

        if (ImGui::CollapsingHeader(tr("Experimental renderer"), ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::BeginDisabled(!panel.experimental_supported);
            ImGui::SeparatorText(tr("Sun"));
            if (ImGui::Checkbox(tr("Sun shadows"), &panel.effects.shadows)) remember("effect_shadows", panel.effects.shadows ? "1" : "0");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("Cascaded shadow maps of the sun, as far from the camera as set below."));
            if (panel.effects.shadows) {
                ImGui::Indent();
                ImGui::SetNextItemWidth(200.0f);
                if (ImGui::SliderFloat(tr("Shadow distance"), &panel.effects.shadow_distance, 100.0f, 1000.0f, tr("%.0f m")))
                    remember("shadow_distance", std::to_string(panel.effects.shadow_distance));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", tr("How far from the camera things cast shadows; farther makes the far cascade coarser."));
                const char* sizes[] = {"1024", "2048", "4096"};
                int quality = panel.effects.shadow_map_size <= 1024 ? 0 : panel.effects.shadow_map_size >= 4096 ? 2 : 1;
                ImGui::SetNextItemWidth(200.0f);
                if (ImGui::Combo(tr("Shadow quality"), &quality, sizes, 3)) {
                    panel.effects.shadow_map_size = 1024u << quality;
                    remember("shadow_quality", std::to_string(panel.effects.shadow_map_size));
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", tr("Texels a side of each cascade's map: sharper shadows, more video memory (4096: 64 MB a cascade)."));
                int cascades = static_cast<int>(panel.effects.shadow_cascades);
                ImGui::SetNextItemWidth(200.0f);
                if (ImGui::SliderInt(tr("Shadow cascades"), &cascades, 2, 4)) {
                    panel.effects.shadow_cascades = static_cast<std::uint32_t>(cascades);
                    remember("shadow_cascades", std::to_string(cascades));
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", tr("Pieces the shadow distance is split into: more keeps the middle distance sharp, each one more pass."));
                ImGui::SetNextItemWidth(200.0f);
                if (ImGui::SliderFloat(tr("Shadow softness"), &panel.effects.shadow_softness, 0.5f, 4.0f, "%.1f"))
                    remember("shadow_softness", std::to_string(panel.effects.shadow_softness));
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("How blurred shadow edges are, in shadow map texels."));
                if (ImGui::Checkbox(tr("Keep the far cascades"), &panel.effects.cache_far_cascades))
                    remember("shadow_cache", panel.effects.cache_far_cascades ? "1" : "0");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", tr("The far cascades are drawn every other frame, and at once when the view leaves them."));
                if (ImGui::Checkbox(tr("Sharp shadows in the cab"), &panel.effects.cab_shadows))
                    remember("shadow_cab", panel.effects.cab_shadows ? "1" : "0");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", tr("A shadow map of its own around the cab: sharp edges on the desk, one more pass."));
                ImGui::Unindent();
            }

            ImGui::SeparatorText(tr("Lamps"));
            if (ImGui::Checkbox(tr("Every lamp, clustered"), &panel.effects.clustered_lights))
                remember("effect_clusters", panel.effects.clustered_lights ? "1" : "0");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("Every lamp and headlight near the camera lights the scene, not RTS's nine a ring."));
            {
                int lamp_shadows = static_cast<int>(panel.effects.lamp_shadows);
                ImGui::SetNextItemWidth(200.0f);
                if (ImGui::SliderInt(tr("Lamp shadows"), &lamp_shadows, 0, 4)) {
                    panel.effects.lamp_shadows = static_cast<std::uint32_t>(lamp_shadows);
                    remember("lamp_shadows", std::to_string(lamp_shadows));
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", tr("Lamps that throw shadows: those lighting the cab (or the view) most, by power, direction and reach, so a floodlight does not shine through the roof. Each is one more pass."));
            }
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::SliderFloat(tr("Light range"), &panel.light_range, 0.0f, 3000.0f, panel.light_range < 1.0f ? tr("as RTS") : tr("%.0f m")))
                remember("light_range", std::to_string(panel.light_range));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("How far the scenery's lamps and floodlights still light the scene; at 0 as RTS's MaxLightDistance."));
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::SliderFloat(tr("Headlight reach"), &panel.headlight_reach, 1.0f, 5.0f, "x%.1f"))
                remember("headlight_reach", std::to_string(panel.headlight_reach));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("How far the trains' headlights and searchlights reach, as a factor of their .eng; 1 as RTS."));
            if (ImGui::Checkbox(tr("Physical light falloff"), &panel.physical_light))
                remember("light_falloff", panel.physical_light ? "physical" : "rts");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("Headlights and lamps fall off as 1/distance squared with a soft cone edge; off: the RTS light shapes."));
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::SliderFloat(tr("Lamps in daylight"), &panel.effects.lamps_in_daylight, 0.0f, 1.0f, "%.2f"))
                remember("lamps_daylight", std::to_string(panel.effects.lamps_in_daylight));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("How strong lamps are in full daylight against the night: an eye used to the sun hardly sees a cab lamp. 1: as at night."));

            ImGui::SeparatorText(tr("Sky and surroundings"));
            if (ImGui::Checkbox(tr("Ambient occlusion, GTAO"), &panel.effects.ambient_occlusion))
                remember("effect_ao", panel.effects.ambient_occlusion ? "1" : "0");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Darkens the sky light in corners and creases, from the depth of the scene."));
            if (ImGui::Checkbox(tr("Aerial perspective"), &panel.effects.aerial_perspective))
                remember("effect_aerial", panel.effects.aerial_perspective ? "1" : "0");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("Distance haze thinning with height and brighter towards the sun, instead of RTS's linear fog."));
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::SliderFloat(tr("Night sky light"), &panel.night_ambient, 0.5f, 8.0f, "%.2f", ImGuiSliderFlags_Logarithmic))
                remember("night_ambient", std::to_string(panel.night_ambient));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("The environment's ambient light at night, as a factor: the unlit surroundings brighter, the lamps as they are. No effect by day."));
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::SliderFloat(tr("Ground bounce"), &panel.effects.ground_bounce, 0.0f, 0.5f, "%.2f"))
                remember("ground_bounce", std::to_string(panel.effects.ground_bounce));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("Light the sunlit ground throws back onto bogies and the underframe, as the ground's reflectance (ballast and grass about 0.2). 0: none."));
            if (ImGui::Checkbox(tr("Light FullBright as ordinary surfaces"), &panel.light_fullbright))
                remember("light_fullbright", panel.light_fullbright ? "1" : "0");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("MSTS FullBright surfaces (platforms, signs) take sun, sky and lamps; their darkened night textures are left out."));
            ImGui::EndDisabled();
        }

        if (ImGui::CollapsingHeader(tr("Sound"), ImGuiTreeNodeFlags_DefaultOpen)) {
            if (panel.sound) {
                float volume = panel.sound->volume();
                ImGui::SetNextItemWidth(220.0f);
                if (ImGui::SliderFloat(tr("Sound volume"), &volume, 0.0f, 1.0f, "%.2f")) {
                    panel.sound->set_volume(volume);
                    remember("volume", std::to_string(volume)); // the next run starts with it
                }
                ImGui::TextDisabled(tr("%zu voices"), panel.sound->active_voices());
            } else {
                ImGui::TextDisabled("%s", tr("Sound: off"));
            }
        }

        if (ImGui::CollapsingHeader(tr("Time and weather"), ImGuiTreeNodeFlags_DefaultOpen)) draw_clock(clock, time_of_day_applies);

        if (ImGui::CollapsingHeader(tr("Scene"), ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text(tr("Camera: %s"), camera.fly ? "fly" : "orbit");
            scene.draw_overlay(camera);
            if (ImGui::Button(tr("Save debug view (Ctrl+F6)"))) panel.save_view = true;
            if (!panel.save_status.empty()) ImGui::TextWrapped("%s", panel.save_status.c_str());
        }

        if (ImGui::CollapsingHeader(tr("Performance"))) {
            const auto& stages = panel.stats.stage_ms();
            double cpu = 0.0;
            for (std::size_t i = 0; i < frame_stage_count; ++i) {
                // "wait" is idle time, not work.
                if (static_cast<FrameStage>(i) != FrameStage::wait) cpu += stages[i];
                ImGui::Text("%-7s %6.2f ms", frame_stage_names[i], stages[i]);
            }
            ImGui::Text(tr("CPU work %5.2f ms (without wait)"), cpu);
            if (panel.gpu_timing) {
                ImGui::Text(tr("GPU pass %5.2f ms"), panel.stats.gpu_ms());
            } else {
                ImGui::TextDisabled("%s", tr("GPU pass: no timestamp queries on this device"));
            }
            const auto gpu = renderer.stats();
            ImGui::Text(tr("GPU: %zu textures, %zu retired waiting, frame %llu done"), gpu.cached_textures, gpu.retired_pending,
                        static_cast<unsigned long long>(gpu.completed_frame));
            ImGui::Text(tr("Models: %zu instances, %zu primitives, %zu draw calls"), gpu.instances, gpu.primitives, gpu.draw_calls);
            if (ImGui::Checkbox(tr("Parallel recording"), &panel.parallel_recording))
                remember("parallel_recording", panel.parallel_recording ? "1" : "0");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("The shadow cascades and the depth pre-pass are recorded on worker threads while the main pass is."));
        }

        ImGui::Separator();
        ImGui::TextUnformatted(tr("RMB: look  WASD: move  Q/E: vertical"));
        ImGui::TextUnformatted(tr("Shift: boost  F: fly/orbit"));
        ImGui::TextUnformatted(tr("T: track  C: train here  J: switch ahead  V: follow  K: EP brake"));
        ImGui::End();

        ImGui::Render();
    }

    void render(const WGPURenderPassEncoder pass) const { ImGui_ImplWGPU_RenderDrawData(ImGui::GetDrawData(), pass); }

private:
    static void draw_clock(ClockControls& clock, const bool time_of_day_applies) {
        auto hours = static_cast<float>(clock.time_of_day / 3600.0);
        const auto seconds = static_cast<int>(clock.time_of_day);
        ImGui::Text(tr("Time %02d:%02d:%02d"), seconds / 3600, seconds / 60 % 60, seconds % 60);
        if (!time_of_day_applies) {
            ImGui::SameLine();
            ImGui::TextUnformatted(tr("(no environment: fixed light)"));
        }
        if (ImGui::SliderFloat("##time", &hours, 0.0f, 24.0f, "%.2f h")) clock.time_of_day = hours * 3600.0;

        ImGui::TextUnformatted(tr("Speed:"));
        for (const float speed : {0.0f, 1.0f, 60.0f, 600.0f, 3600.0f}) {
            ImGui::SameLine();
            const auto label = speed == 0.0f ? std::string(tr("pause")) : "x" + std::to_string(static_cast<int>(speed));
            if (ImGui::RadioButton(label.c_str(), clock.speed == speed)) clock.speed = speed;
        }

        const char* seasons[] = {tr("Spring"), tr("Summer"), tr("Autumn"), tr("Winter")};
        ImGui::Combo(tr("Season"), &clock.season, seasons, 4);
        // Order of the RTS weather variable: clear, snow, rain.
        const char* weathers[] = {tr("Clear"), tr("Snow"), tr("Rain")};
        ImGui::Combo(tr("Weather"), &clock.weather, weathers, 3);
    }

    // A system monospace font with Cyrillic (content names are often
    // Russian); ImGui's built-in font, Latin only, when none is found.
    static void load_font() {
        auto& io = ImGui::GetIO();
        static const ImWchar ranges[] = {
            0x0020, 0x00FF, // Latin + Latin-1
            0x0400, 0x052F, // Cyrillic + supplement
            0x2010, 0x205E, // punctuation (dashes, quotes, ellipsis)
            0x2116, 0x2116, // numero sign
            0,
        };
        const std::vector<std::string> candidates{
            "C:/Windows/Fonts/consola.ttf",
            "C:/Windows/Fonts/lucon.ttf",
            "C:/Windows/Fonts/cour.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        };
        bool loaded = false;
        for (const auto& file : candidates) {
            if (!std::filesystem::is_regular_file(file)) continue;
            if (io.Fonts->AddFontFromFileTTF(file.c_str(), 15.0f, nullptr, ranges)) {
                loaded = true;
                break;
            }
        }
        if (!loaded) io.Fonts->AddFontDefault();
        // Map labels (Track control station names; RTS: Arial 16, here a
        // little larger for today's screens).
        for (const char* file : {"C:/Windows/Fonts/arial.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"}) {
            if (!std::filesystem::is_regular_file(file)) continue;
            if (auto* font = io.Fonts->AddFontFromFileTTF(file, 20.0f, nullptr, ranges)) {
                map_label_font() = font;
                break;
            }
        }
        // The start menu's title and large button (Segoe UI Semibold).
        for (const char* file : {"C:/Windows/Fonts/seguisb.ttf", "C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf",
                                 "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"}) {
            if (!std::filesystem::is_regular_file(file)) continue;
            title_font() = io.Fonts->AddFontFromFileTTF(file, 34.0f, nullptr, ranges);
            large_font() = io.Fonts->AddFontFromFileTTF(file, 21.0f, nullptr, ranges);
            break;
        }
    }

    std::string layout_file_; // ImGui keeps the pointer until the context goes
    bool initialized_ = false;
};
#endif

[[nodiscard]] bool overlay_wants_mouse() {
#ifdef OPENRTS_HAS_IMGUI
    return ImGui::GetIO().WantCaptureMouse;
#else
    return false;
#endif
}

// Mouse/keyboard state that drives the camera between GLFW callbacks.
struct CameraInput {
    ViewerCamera* camera = nullptr;
    bool orbit_dragging = false;
    bool fly_looking = false;
    bool toggle_key_down = false;
    bool shading_key_down = false;
    bool pause_key_down = false;
    // Scene keys, for press edges.
    bool junction_key_down = false;
    bool follow_key_down = false;
    bool track_key_down = false;
    bool labels_key_down = false;
    bool track_control_key_down = false;
    bool info_key_down = false;
    bool stations_key_down = false;
    bool save_key_down = false;
    bool enter_key_down = false;
    bool kp0_down = false;
    bool kp1_down = false;
    bool kp3_down = false;
    int clicks = 0;            // left clicks in fly mode since the last scene_input()
    int releases = 0;          // left button let go (anywhere, over a panel too)
    double scroll = 0.0;       // wheel since the last scene_input()
    bool scene_takes_scroll = false;
    std::array<bool, 14> train_keys_down{};
    std::vector<KeyEvent> key_events; // since the last scene_input()
    bool keyboard_captured = false;   // the scene takes the keyboard (cab keys)
    double cursor_x = 0.0;
    double cursor_y = 0.0;
};

[[nodiscard]] CameraInput* camera_input(GLFWwindow* window) {
    return static_cast<CameraInput*>(glfwGetWindowUserPointer(window));
}

void set_cursor_captured(GLFWwindow* window, const bool captured) {
    glfwSetInputMode(window, GLFW_CURSOR, captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
}

void on_mouse_button(GLFWwindow* window, const int button, const int action, [[maybe_unused]] const int mods) {
#ifdef OPENRTS_HAS_IMGUI
    ImGui_ImplGlfw_MouseButtonCallback(window, button, action, mods);
#endif
    // A held cab button comes back wherever the mouse is let go.
    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_RELEASE) {
        if (auto* held = camera_input(window)) ++held->releases;
    }
    if (overlay_wants_mouse()) return;
    auto* input = camera_input(window);
    if (!input) return;

    const bool pressed = action == GLFW_PRESS;
    if (button == GLFW_MOUSE_BUTTON_LEFT && !input->camera->fly) input->orbit_dragging = pressed;
    if (button == GLFW_MOUSE_BUTTON_LEFT && input->camera->fly && pressed) ++input->clicks;
    if (button == GLFW_MOUSE_BUTTON_RIGHT && input->camera->fly) {
        input->fly_looking = pressed;
        set_cursor_captured(window, pressed);
    }
    glfwGetCursorPos(window, &input->cursor_x, &input->cursor_y);
}

void on_cursor_moved(GLFWwindow* window, const double x, const double y) {
#ifdef OPENRTS_HAS_IMGUI
    ImGui_ImplGlfw_CursorPosCallback(window, x, y);
#endif
    if (overlay_wants_mouse()) return;
    auto* input = camera_input(window);
    if (!input) return;

    if (input->orbit_dragging || input->fly_looking) {
        input->camera->rotate(static_cast<float>(x - input->cursor_x), static_cast<float>(y - input->cursor_y));
    }
    input->cursor_x = x;
    input->cursor_y = y;
}

void on_scroll(GLFWwindow* window, [[maybe_unused]] const double x, const double y) {
#ifdef OPENRTS_HAS_IMGUI
    ImGui_ImplGlfw_ScrollCallback(window, x, y);
#endif
    if (overlay_wants_mouse()) return;
    if (auto* input = camera_input(window)) {
        input->scroll += y;
        if (!input->scene_takes_scroll) input->camera->zoom(static_cast<float>(y));
    }
}

void on_key([[maybe_unused]] GLFWwindow* window,
            [[maybe_unused]] const int key,
            [[maybe_unused]] const int scancode,
            [[maybe_unused]] const int action,
            [[maybe_unused]] const int mods) {
#ifdef OPENRTS_HAS_IMGUI
    ImGui_ImplGlfw_KeyCallback(window, key, scancode, action, mods);
    if (ImGui::GetIO().WantCaptureKeyboard) return;
#endif
    if (action == GLFW_REPEAT) return;
    if (auto* input = camera_input(window)) {
        input->key_events.push_back({.key = key,
                                     .down = action == GLFW_PRESS,
                                     .shift = (mods & GLFW_MOD_SHIFT) != 0,
                                     .ctrl = (mods & GLFW_MOD_CONTROL) != 0,
                                     .alt = (mods & GLFW_MOD_ALT) != 0});
    }
}

void on_char([[maybe_unused]] GLFWwindow* window, [[maybe_unused]] const unsigned codepoint) {
#ifdef OPENRTS_HAS_IMGUI
    ImGui_ImplGlfw_CharCallback(window, codepoint);
#endif
}

void install_input_callbacks(GLFWwindow* window, CameraInput& input) {
    glfwSetWindowUserPointer(window, &input);
    glfwSetMouseButtonCallback(window, on_mouse_button);
    glfwSetCursorPosCallback(window, on_cursor_moved);
    glfwSetScrollCallback(window, on_scroll);
    glfwSetKeyCallback(window, on_key);
    glfwSetCharCallback(window, on_char);
}

// Polled keyboard handling: F toggles fly/orbit on key press, WASD/QE move.
void update_camera_from_keyboard(GLFWwindow* window, CameraInput& input, const float seconds) {
    const auto down = [window](const int key) { return glfwGetKey(window, key) == GLFW_PRESS; };
    auto& camera = *input.camera;

    // F is a cab key when the cab has the keyboard.
    const bool toggle = down(GLFW_KEY_F) && !input.keyboard_captured;
    if (toggle && !input.toggle_key_down) {
        camera.toggle_fly();
        if (!camera.fly) {
            input.fly_looking = false;
            set_cursor_captured(window, false);
        }
    }
    input.toggle_key_down = toggle;
    if (input.keyboard_captured) return; // the cab has the letter keys

    camera.move(
        {
            .forward = down(GLFW_KEY_W),
            .backward = down(GLFW_KEY_S),
            .left = down(GLFW_KEY_A),
            .right = down(GLFW_KEY_D),
            .up = down(GLFW_KEY_E),
            .down = down(GLFW_KEY_Q),
            .boost = down(GLFW_KEY_LEFT_SHIFT) || down(GLFW_KEY_RIGHT_SHIFT),
        },
        seconds);
}

// Probe and track keys: P place, Up/Down drive, J throw a junction, V follow,
// T track overlay.
[[nodiscard]] SceneInput scene_input(GLFWwindow* window, CameraInput& input) {
    const auto down = [window](const int key) { return glfwGetKey(window, key) == GLFW_PRESS; };
    const auto pressed = [&](const int key, bool& was_down) {
        const bool now = down(key);
        const bool edge = now && !was_down;
        was_down = now;
        return edge;
    };
    const bool ctrl = down(GLFW_KEY_LEFT_CONTROL) || down(GLFW_KEY_RIGHT_CONTROL);
    const bool shift = down(GLFW_KEY_LEFT_SHIFT) || down(GLFW_KEY_RIGHT_SHIFT);
    const bool alt = down(GLFW_KEY_LEFT_ALT) || down(GLFW_KEY_RIGHT_ALT);
    const bool f5 = pressed(GLFW_KEY_F5, input.info_key_down);
    const bool f7 = pressed(GLFW_KEY_F7, input.track_control_key_down);
    const bool enter_now = down(GLFW_KEY_ENTER) || down(GLFW_KEY_KP_ENTER);
    const bool enter = enter_now && !input.enter_key_down;
    input.enter_key_down = enter_now;
    // The engineer's window and head out (key_layout.hpp): RTS's keypad
    // keys, or - and = on the top row.
    const bool rts_keys = key_layout() == KeyLayout::rts;
    const bool kp0 = pressed(rts_keys ? GLFW_KEY_KP_0 : GLFW_KEY_MINUS, input.kp0_down);
    const bool kp1 = pressed(rts_keys ? GLFW_KEY_KP_1 : GLFW_KEY_EQUAL, input.kp1_down);
    const bool kp3 = pressed(rts_keys ? GLFW_KEY_KP_3 : GLFW_KEY_GRAVE_ACCENT, input.kp3_down);
    int width = 0, height = 0;
    glfwGetWindowSize(window, &width, &height);
    const bool pointer = !input.fly_looking && width > 0 && height > 0 && !overlay_wants_mouse()
                         && input.cursor_x >= 0.0 && input.cursor_y >= 0.0 && input.cursor_x < width && input.cursor_y < height;
    const int clicks = std::exchange(input.clicks, 0);
    const int releases = std::exchange(input.releases, 0);
    const double scroll = std::exchange(input.scroll, 0.0);
    const auto engineer = [&](SceneInput& in) {
        in.cab_window = kp0;
        in.head_out = kp1;
        in.car_menu = kp3;
        in.pointer = pointer;
        in.pointer_x = static_cast<float>(input.cursor_x / width * 2.0 - 1.0);
        in.pointer_y = static_cast<float>(1.0 - input.cursor_y / height * 2.0);
        in.click = clicks > 0 && pointer;
        in.release = releases > 0;
        in.held = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        in.cursor_x = static_cast<float>(input.cursor_x);
        in.cursor_y = static_cast<float>(input.cursor_y);
        in.click_shift = shift;
        in.scroll = pointer ? static_cast<float>(scroll) : 0.0f;
        in.walk_forward = down(GLFW_KEY_UP);
        in.walk_back = down(GLFW_KEY_DOWN);
        in.walk_left = down(GLFW_KEY_LEFT);
        in.walk_right = down(GLFW_KEY_RIGHT);
        in.walk_run = shift;
        in.walk_slow = down(GLFW_KEY_END);
        in.engineer_action = enter;
        in.engineer_action_ctrl = enter && ctrl;
        in.cab_labels = alt;
    };
    if (input.keyboard_captured) {
        // Only the function keys keep their viewer meaning; the rest is the cab's.
        SceneInput captured{
            .toggle_track_control = f7 && ctrl && alt,
            .place_names = f7 && !ctrl && !alt,
            .car_info = f5 && !ctrl,
            .car_info_shift = shift,
            .limits_panel = f5 && ctrl,
            .stations_panel = pressed(GLFW_KEY_F6, input.stations_key_down) && !ctrl,
            .signal_names = f7 && ctrl && !alt,
        };
        engineer(captured);
        captured.keys = std::exchange(input.key_events, {});
        return captured;
    }
    SceneInput result{
        .step_up = down(GLFW_KEY_UP),
        .step_down = down(GLFW_KEY_DOWN),
        .throw_junction = pressed(GLFW_KEY_J, input.junction_key_down),
        .toggle_follow = pressed(GLFW_KEY_V, input.follow_key_down),
        .toggle_track = pressed(GLFW_KEY_T, input.track_key_down),
        .toggle_labels = pressed(GLFW_KEY_L, input.labels_key_down),
        .place_train = pressed(GLFW_KEY_C, input.train_keys_down[0]),
        .throttle_up = pressed(GLFW_KEY_HOME, input.train_keys_down[1]),
        .throttle_down = pressed(GLFW_KEY_END, input.train_keys_down[2]),
        .reverser_forward = pressed(GLFW_KEY_PAGE_UP, input.train_keys_down[3]),
        .reverser_back = pressed(GLFW_KEY_PAGE_DOWN, input.train_keys_down[4]),
        .brake_more = pressed(GLFW_KEY_RIGHT_BRACKET, input.train_keys_down[5]),
        .brake_less = pressed(GLFW_KEY_LEFT_BRACKET, input.train_keys_down[6]),
        .brake_less_held = down(GLFW_KEY_LEFT_BRACKET),
        .independent_more = pressed(GLFW_KEY_PERIOD, input.train_keys_down[7]),
        .independent_less = pressed(GLFW_KEY_COMMA, input.train_keys_down[8]),
        .toggle_ep = pressed(GLFW_KEY_K, input.train_keys_down[9]),
        .horn = down(GLFW_KEY_SPACE),
        .sander = down(GLFW_KEY_X),
        .bell = pressed(GLFW_KEY_B, input.train_keys_down[10]),
        .pantographs = pressed(GLFW_KEY_P, input.train_keys_down[11]),
        .doors_left = pressed(GLFW_KEY_Y, input.train_keys_down[12]),
        .doors_right = pressed(GLFW_KEY_U, input.train_keys_down[13]),
        .toggle_track_control = f7 && ctrl && alt,
        .place_names = f7 && !ctrl && !alt,
        .car_info = f5 && !ctrl,
        .car_info_shift = shift,
        .limits_panel = f5 && ctrl,
        .stations_panel = pressed(GLFW_KEY_F6, input.stations_key_down) && !ctrl,
        .signal_names = f7 && ctrl && !alt,
    };
    engineer(result);
    result.keys = std::exchange(input.key_events, {});
    return result;
}

void toggle_shading_from_keyboard(GLFWwindow* window, CameraInput& input, renderer::ModelRenderer& renderer) {
    const bool down = glfwGetKey(window, GLFW_KEY_R) == GLFW_PRESS;
    if (down && !input.shading_key_down) {
        renderer.set_shading_path(renderer.shading_path() == renderer::ShadingPath::rts_compatible
                                      ? renderer::ShadingPath::experimental
                                      : renderer::ShadingPath::rts_compatible);
    }
    input.shading_key_down = down;
}

// Route coordinates <-> scene-local coordinates (identity for shape scenes).
[[nodiscard]] glm::vec3 to_local(const std::optional<formats::RoutePosition>& origin, const std::int32_t tile_x,
                                 const std::int32_t tile_z, const double x, const double y, const double z) {
    if (!origin) return {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
    constexpr double tile = formats::route_tile_size;
    return {
        static_cast<float>((tile_x - origin->tile_x) * tile + x - origin->x),
        static_cast<float>(y - origin->y),
        static_cast<float>((tile_z - origin->tile_z) * tile + z - origin->z),
    };
}

void apply_view(const DebugView& view, const std::optional<formats::RoutePosition>& origin, ViewerCamera& camera) {
    camera.fly = view.fly;
    camera.yaw = view.yaw;
    camera.pitch = view.pitch;
    if (view.distance > 0.0f) camera.distance = view.distance;
    camera.position = to_local(origin, view.tile_x, view.tile_z, view.x, view.y, view.z);
    camera.target = to_local(origin, view.tile_x, view.tile_z, view.target_x, view.target_y, view.target_z);
}

// Writes debug_views/last.view and a timestamped copy; returns a status line.
[[nodiscard]] std::string save_view(DebugView view, const std::optional<formats::RoutePosition>& origin,
                                    const ViewerCamera& camera, const ClockControls& clock,
                                    const renderer::ShadingPath shading) {
    const auto absolute = [&](const glm::vec3& local) {
        if (!origin) return formats::RoutePosition{.x = local.x, .y = local.y, .z = local.z};
        return renderer::route_position(local, *origin);
    };
    const auto eye = absolute(camera.eye());
    // The target is stored in the eye's tile frame.
    const auto target_local = camera.fly ? camera.eye() + camera.forward() * camera.distance : camera.target;
    const auto target = to_local(origin, eye.tile_x, eye.tile_z, 0.0, 0.0, 0.0);
    view.tile_x = eye.tile_x;
    view.tile_z = eye.tile_z;
    view.x = eye.x;
    view.y = eye.y;
    view.z = eye.z;
    view.target_x = target_local.x - target.x;
    view.target_y = target_local.y - target.y;
    view.target_z = target_local.z - target.z;
    view.yaw = camera.yaw;
    view.pitch = camera.pitch;
    view.distance = camera.distance;
    view.fly = camera.fly;
    view.time_of_day = static_cast<std::int32_t>(clock.time_of_day);
    view.season = clock.season;
    view.weather = clock.weather;
    view.experimental = shading == renderer::ShadingPath::experimental;

    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
    localtime_s(&local, &now);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &local);
    auto name = view.content.substr(view.content.find_last_of('/') + 1);
    std::string error;
    const auto directory = debug_view_directory();
    const auto copy = directory / (name + "_" + stamp + ".view");
    if (!save_debug_view(directory / "last.view", view, error) || !save_debug_view(copy, view, error)) return error;
    std::cout << "debug view saved: " << copy.string() << std::endl;
    return "saved " + copy.filename().string() + " (and last.view)";
}

// Prints the adapter and the optional features GPU-driven rendering would
// use (compute culling with indirect draws).
void report_adapter(const WGPUAdapter adapter) {
    WGPUAdapterInfo info{};
    wgpuAdapterGetInfo(adapter, &info);
    const auto text = [](const WGPUStringView view) { return std::string(view.data ? view.data : "", view.data ? view.length : 0); };
    const char* backend = info.backendType == WGPUBackendType_Vulkan ? "Vulkan"
        : info.backendType == WGPUBackendType_D3D12                   ? "D3D12"
        : info.backendType == WGPUBackendType_Metal                   ? "Metal"
        : info.backendType == WGPUBackendType_OpenGL                  ? "OpenGL"
                                                                      : "other";
    std::cout << "GPU: " << text(info.device) << " (" << backend << ", " << text(info.description) << ")\n";
    wgpuAdapterInfoFreeMembers(info);
    const auto has = [&](const WGPUFeatureName feature) { return wgpuAdapterHasFeature(adapter, feature) ? "yes" : "no"; };
    std::cout << "  compute shaders: " << (backend[0] == 'O' ? "GLES 3.1+ only" : "yes")
              << ", indirect first instance: " << has(WGPUFeatureName_IndirectFirstInstance)
              << ", multi-draw indirect count: " << has(static_cast<WGPUFeatureName>(WGPUNativeFeature_MultiDrawIndirectCount))
              << ", texture binding arrays: " << has(static_cast<WGPUFeatureName>(WGPUNativeFeature_TextureBindingArray))
              << ", non-uniform indexing: "
              << has(static_cast<WGPUFeatureName>(WGPUNativeFeature_SampledTextureAndStorageBufferArrayNonUniformIndexing))
              << ", timestamps: " << has(WGPUFeatureName_TimestampQuery) << std::endl;
}

[[nodiscard]] glm::mat4 projection(const ViewerCamera& camera, const float aspect, const float vertical_fov) {
    // At most 5 cm: the route camera keeps distance 1000 (the far plane at
    // 20 km), which made the near plane 2 m and cut the cab's walls and floor
    // away. Reversed Z on Depth32Float keeps the far precision anyway.
    const auto near_plane = std::clamp(camera.distance * 0.002f, 0.01f, 0.05f);
    return renderer::reversed_z_perspective(vertical_fov, aspect, near_plane, camera.distance * 20.0f);
}

struct FrameTargets {
    WGPUTextureView color;   // the frame
    WGPUTextureView depth;
    WGPUTextureView msaa = nullptr; // multisampled colour, resolved into `color`
    bool keep_depth = false;        // the depth stays after the pass (for ReShade)
};

[[nodiscard]] WGPURenderPassEncoder begin_main_pass(const WGPUCommandEncoder encoder,
                                                    const FrameTargets& targets,
                                                    const glm::vec4& clear_colour,
                                                    const WGPURenderPassTimestampWrites* timestamps,
                                                    const WGPUQuerySet occlusion) {
    const WGPURenderPassColorAttachment color{
        .view = targets.msaa ? targets.msaa : targets.color,
        .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
        .resolveTarget = targets.msaa ? targets.color : nullptr,
        .loadOp = WGPULoadOp_Clear,
        .storeOp = targets.msaa ? WGPUStoreOp_Discard : WGPUStoreOp_Store,
        .clearValue = {.r = clear_colour.r, .g = clear_colour.g, .b = clear_colour.b, .a = clear_colour.a},
    };
    const WGPURenderPassDepthStencilAttachment depth{
        .view = targets.depth,
        .depthLoadOp = WGPULoadOp_Clear,
        .depthStoreOp = targets.keep_depth ? WGPUStoreOp_Store : WGPUStoreOp_Discard,
        .depthClearValue = renderer::depth_clear_value,
        .depthReadOnly = false,
        .stencilLoadOp = WGPULoadOp_Undefined,
        .stencilStoreOp = WGPUStoreOp_Undefined,
        .stencilReadOnly = true,
    };
    const WGPURenderPassDescriptor descriptor{
        .colorAttachmentCount = 1,
        .colorAttachments = &color,
        .depthStencilAttachment = &depth,
        .occlusionQuerySet = occlusion,
        .timestampWrites = timestamps,
    };
    return wgpuCommandEncoderBeginRenderPass(encoder, &descriptor);
}

// Releases a scene's GPU resources before the renderer and device are destroyed.
class SceneAttachment {
public:
    explicit SceneAttachment(ViewerScene& scene) : scene_(scene) {}
    ~SceneAttachment() { scene_.detach(); }
    SceneAttachment(const SceneAttachment&) = delete;
    SceneAttachment& operator=(const SceneAttachment&) = delete;

private:
    ViewerScene& scene_;
};

} // namespace

// One frame of the swap chain to a file (--capture).
struct FrameCapture {
    WGPUBuffer buffer = nullptr;
    std::uint32_t width = 0, height = 0, row_bytes = 0;
};

[[nodiscard]] FrameCapture begin_capture(const WGPUDevice device, const WGPUCommandEncoder encoder, const WGPUTexture texture,
                                         const std::uint32_t width, const std::uint32_t height) {
    FrameCapture shot{.width = width, .height = height, .row_bytes = (width * 4 + 255) / 256 * 256};
    const WGPUBufferDescriptor descriptor{.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst,
                                          .size = static_cast<std::uint64_t>(shot.row_bytes) * height};
    shot.buffer = wgpuDeviceCreateBuffer(device, &descriptor);
    if (!shot.buffer) return shot;
    const WGPUTexelCopyTextureInfo source{.texture = texture};
    const WGPUTexelCopyBufferInfo target{.layout = {.bytesPerRow = shot.row_bytes, .rowsPerImage = height}, .buffer = shot.buffer};
    const WGPUExtent3D size{.width = width, .height = height, .depthOrArrayLayers = 1};
    wgpuCommandEncoderCopyTextureToBuffer(encoder, &source, &target, &size);
    return shot;
}

// Waits for the copy and writes a binary PPM.
[[nodiscard]] bool finish_capture(const renderer::WgpuDeviceContext& context, const FrameCapture& shot, const WGPUTextureFormat format,
                                  const std::string& path) {
    bool mapped = false;
    bool done = false;
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
    wgpuBufferMapAsync(shot.buffer, WGPUMapMode_Read, 0, static_cast<std::size_t>(shot.row_bytes) * shot.height, callback);
    for (int i = 0; i < 1000 && !done; ++i) {
        wgpuDevicePoll(context.device(), true, nullptr);
        wgpuInstanceProcessEvents(context.instance());
    }
    bool written = false;
    if (mapped) {
        const auto* bytes = static_cast<const std::uint8_t*>(
            wgpuBufferGetConstMappedRange(shot.buffer, 0, static_cast<std::size_t>(shot.row_bytes) * shot.height));
        std::ofstream out(path, std::ios::binary);
        if (bytes && out) {
            out << "P6\n" << shot.width << ' ' << shot.height << "\n255\n";
            const bool bgra = format == WGPUTextureFormat_BGRA8Unorm || format == WGPUTextureFormat_BGRA8UnormSrgb;
            std::vector<char> row(static_cast<std::size_t>(shot.width) * 3);
            for (std::uint32_t y = 0; y < shot.height; ++y) {
                const auto* line = bytes + static_cast<std::size_t>(y) * shot.row_bytes;
                for (std::uint32_t x = 0; x < shot.width; ++x) {
                    row[x * 3 + 0] = static_cast<char>(line[x * 4 + (bgra ? 2 : 0)]);
                    row[x * 3 + 1] = static_cast<char>(line[x * 4 + 1]);
                    row[x * 3 + 2] = static_cast<char>(line[x * 4 + (bgra ? 0 : 2)]);
                }
                out.write(row.data(), static_cast<std::streamsize>(row.size()));
            }
            written = static_cast<bool>(out);
        }
        wgpuBufferUnmap(shot.buffer);
    }
    wgpuBufferRelease(shot.buffer);
    return written;
}

// What ReShade makes of the exported depth with RESHADE_DEPTH_INPUT_IS_REVERSED 1,
// RESHADE_DEPTH_MULTIPLIER 1/near and RESHADE_DEPTH_LINEARIZATION_FAR_PLANE
// 1000 (ReShade.fxh GetLinearizedDepth), against the true distance.
void report_reshade_depth(const renderer::WgpuDeviceContext& context, const FrameCapture& shot, const float near_plane,
                          const float far_plane) {
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
    const auto size = static_cast<std::size_t>(shot.row_bytes) * shot.height;
    wgpuBufferMapAsync(shot.buffer, WGPUMapMode_Read, 0, size, callback);
    for (int i = 0; i < 1000 && !done; ++i) {
        wgpuDevicePoll(context.device(), true, nullptr);
        wgpuInstanceProcessEvents(context.instance());
    }
    if (mapped) {
        const auto* bytes = static_cast<const std::uint8_t*>(wgpuBufferGetConstMappedRange(shot.buffer, 0, size));
        const auto at = [&](const std::uint32_t x, const std::uint32_t y) {
            float d = 0.0f;
            std::memcpy(&d, bytes + static_cast<std::size_t>(y) * shot.row_bytes + x * 4u, 4);
            return d;
        };
        constexpr float far_reshade = 1000.0f;
        const auto reshade_metres = [&](const float d) {
            const float x = 1.0f - d / near_plane;
            return x / (far_reshade - x * (far_reshade - 1.0f)) * far_reshade;
        };
        // Reversed Z (glm perspectiveRH_ZO with near and far swapped).
        const auto true_metres = [&](const float d) { return near_plane * far_plane / (d * (far_plane - near_plane) + near_plane); };
        std::size_t sky = 0;
        for (std::uint32_t y = 0; y < shot.height; ++y)
            for (std::uint32_t x = 0; x < shot.width; ++x) sky += at(x, y) <= 0.0f;
        std::cout << "reshade depth: " << shot.width << "x" << shot.height << ", sky "
                  << 100.0 * static_cast<double>(sky) / (static_cast<double>(shot.width) * shot.height) << " %";
        for (const auto& [name, fy] : {std::pair{"centre", 0.5f}, std::pair{"lower", 0.8f}, std::pair{"bottom", 0.97f}}) {
            const float d = at(shot.width / 2, static_cast<std::uint32_t>(fy * static_cast<float>(shot.height - 1)));
            std::cout << ", " << name << ' ';
            if (d <= 0.0f)
                std::cout << "sky";
            else
                std::cout << reshade_metres(d) << " m (true " << true_metres(d) << " m)";
        }
        std::cout << std::endl;
        wgpuBufferUnmap(shot.buffer);
    } else {
        std::cout << "reshade depth: readback failed" << std::endl;
    }
    wgpuBufferRelease(shot.buffer);
}

int run_window_viewer(ViewerScene& scene, const WindowOptions& options) {
    // The graphics API: --backend for this run, else the remembered choice.
    auto backend = options.backend;
    if (!backend) {
        if (const auto key = remembered("backend")) backend = renderer::parse_graphics_backend(*key);
    }
    auto context = renderer::create_wgpu_device(backend.value_or(renderer::GraphicsBackend::automatic));
    if (!context.ok()) {
        std::cerr << "WebGPU device failed: " << context.error << '\n';
        return exit_code::device_failed;
    }
    const auto device = context.context.device();
    report_adapter(context.context.adapter());
    // OpenGL: the experimental path's compute shaders read depth textures
    // with textureLoad, which wgpu's GLSL output cannot do.
    const bool experimental_supported = renderer::adapter_backend_name(context.context.adapter()).substr(0, 6) != "OpenGL";

    const GlfwSession glfw;
    if (!glfw.ok()) {
        std::cerr << "GLFW initialization failed.\n";
        return exit_code::platform_failed;
    }
    const bool capture = !options.capture_path.empty();
    if (capture) {
        // Off the screen and without focus: the user's desktop stays as it is.
        glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
        glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
    }
    // The size and place it was left at (not for a capture: its frame is 1280 x 720).
    const std::string placement_key = options.menu ? "window_menu" : "window_game";
    WindowPlacement placement;
    if (!capture) {
        if (const auto saved = remembered_placement(placement_key)) {
            placement = *saved;
            if (on_a_monitor(placement)) {
                glfwWindowHint(GLFW_POSITION_X, placement.x);
                glfwWindowHint(GLFW_POSITION_Y, placement.y);
            } else {
                placement.positioned = false;
            }
            glfwWindowHint(GLFW_MAXIMIZED, placement.maximized ? GLFW_TRUE : GLFW_FALSE);
        }
    }
    const Window window(placement.width, placement.height, options.title.c_str());
    if (!window.get()) return exit_code::window_failed;
    if (capture) glfwSetWindowPos(window.get(), -20000, -20000);
    std::optional<PlacementKeeper> placement_keeper;
    if (!capture) placement_keeper.emplace(placement_key, placement);

    ViewerCamera camera;
    CameraInput input{.camera = &camera};
    install_input_callbacks(window.get(), input);

    renderer::render_sample_count() = options.msaa;
    renderer::scene_hdr() = options.hdr && !options.menu;
    if (renderer::scene_hdr()) std::cout << "HDR frame: Rgba16Float, tone curve (AgX in the experimental path)" << std::endl;
    std::cout << "antialiasing: " << (options.msaa > 1 ? std::to_string(options.msaa) + "x MSAA, alpha-to-coverage" : "off")
              << ", anisotropy " << renderer::max_anisotropy << "x" << std::endl;
    PresentationSurface surface(context.context, window.get(), capture);
    if (!surface.ok()) return exit_code::surface_failed;

    renderer::ModelRenderer renderer(device, surface.scene_format());
    std::unique_ptr<renderer::ToneMapper> tone;
    if (renderer::scene_hdr()) tone = std::make_unique<renderer::ToneMapper>(device, surface.format());
    std::unique_ptr<renderer::DepthExport> depth_export;
    if (!renderer.ready()) {
        std::cerr << "Model renderer initialization failed.\n";
        return exit_code::renderer_failed;
    }
    renderer.set_shading_path(experimental_supported ? options.shading : renderer::ShadingPath::rts_compatible);
    renderer.set_batching(options.batching);
    // The experimental path's shadows, lamps, occlusion and haze.
    renderer::SceneEffects effects(device);
    // Workers for the parallel recording: a cascade or the pre-pass each.
    renderer::TaskPool recording_pool((std::min)((std::max)(std::thread::hardware_concurrency(), 2u) - 1u, 5u));
    renderer.set_task_pool(&recording_pool);
    if (!effects.ready()) {
        std::cerr << "Scene effects initialization failed.\n";
        return exit_code::renderer_failed;
    }

#ifdef OPENRTS_HAS_IMGUI
    const DebugOverlay overlay(window.get(), device, surface.format(), options.menu);
    if (!overlay.ok()) {
        std::cerr << "ImGui initialization failed.\n";
        return exit_code::renderer_failed;
    }
#endif

    // The interface language chosen last time (Russian by default).
    set_ui_language(remembered("language").value_or("ru") == "en" ? UiLanguage::english : UiLanguage::russian);
    // The keys chosen last time (openRTS's, without the keypad, by default).
    set_key_layout(remembered("key_layout").value_or("openrts") == "rts" ? KeyLayout::rts : KeyLayout::openrts);
    // Sound is optional: without a device the viewer runs silent.
    std::unique_ptr<audio::AudioEngine> sound;
    if (options.sound) {
        sound = std::make_unique<audio::AudioEngine>(audio::AudioEngineOptions{.device = !options.sound_offline});
        if (!sound->ok()) {
            std::cout << "sound: " << sound->error() << ", running silent\n";
            sound.reset();
        } else {
            sound->set_volume(options.sound_volume);
        }
    }

    const SceneAttachment attachment(scene);
    if (const auto error = scene.attach(
            {.device = device, .target_format = surface.scene_format(), .models = renderer, .audio = sound.get()}, camera);
        !error.empty()) {
        std::cerr << error << '\n';
        return exit_code::renderer_failed;
    }

    if (options.view) apply_view(*options.view, scene.origin(), camera);
    const double capture_start = glfwGetTime();
    // A capture from a saved view (--view, no --capture-camera) keeps that
    // camera: the scene's own (the engineer's cab) is put back after it,
    // from the view each frame, as the scene moves its origin along with a
    // camera far from it (rebase_near): a copy of the camera would be left
    // behind and the origin would run away.
    const auto capture_view = [&]() -> std::optional<ViewerCamera> {
        if (!capture || !options.view || options.capture_camera) return std::nullopt;
        ViewerCamera view_camera = camera;
        apply_view(*options.view, scene.origin(), view_camera);
        return view_camera;
    };
    bool capture_switched = false;
    std::size_t capture_switch_next = 0; // the next --capture-switch to make
    bool capture_menu_picked = false;
    std::size_t capture_menu_next = 0; // of capture_menu_more: the next to open and pick
    bool capture_menu_open = false;
    bool capture_uncoupled = false;
    bool capture_driven = false;
    int capture_board_step = 0; // --capture-board: 1 at the door, then the presses of Enter
    FrameLimiter limiter;

    ClockControls clock{
        .time_of_day = static_cast<double>(options.time_of_day),
        .speed = options.time_speed,
        .season = static_cast<int>(options.season),
        .weather = static_cast<int>(options.weather),
    };
    surface.set_vsync(options.vsync);
    const auto timer_queue = wgpuDeviceGetQueue(device);
    GpuFrameTimer gpu_timer(device, timer_queue);
    wgpuQueueRelease(timer_queue);
    PanelState panel{
        .vsync = surface.vsync(), .vsync_optional = surface.vsync_optional(), .gpu_timing = gpu_timer.available()};
    panel.menu = options.menu;
    panel.fps_limit = clamp_fps_limit(options.fps_limit);
    panel.hdr = renderer::scene_hdr();
    panel.backend_now = renderer::adapter_backend_name(context.context.adapter());
    panel.reshade_depth = options.reshade_depth.value_or(remembered("reshade_depth").value_or("0") == "1");
    panel.experimental_supported = experimental_supported;
    if (!panel.experimental_supported)
        std::cout << "OpenGL: the experimental renderer and the depth copy for ReShade are not available "
                     "(wgpu cannot read depth textures with textureLoad in GLSL); the RTS path only." << std::endl;
    if (const auto key = remembered("backend"))
        panel.backend_choice = static_cast<int>(renderer::parse_graphics_backend(*key).value_or(renderer::GraphicsBackend::automatic));
    if (const auto curve = remembered("tone_mode")) panel.tone_curve = std::clamp(std::atoi(curve->c_str()), 0, 4);
    if (const auto falloff = remembered("light_falloff")) panel.physical_light = *falloff == "physical";
    if (const auto fullbright = remembered("light_fullbright")) panel.light_fullbright = *fullbright == "1";
    if (const auto range = remembered("light_range"))
        panel.light_range = std::clamp(static_cast<float>(std::atof(range->c_str())), 0.0f, 3000.0f);
    if (options.light_range) panel.light_range = std::clamp(*options.light_range, 0.0f, 3000.0f);
    if (const auto reach = remembered("headlight_reach"))
        panel.headlight_reach = std::clamp(static_cast<float>(std::atof(reach->c_str())), 1.0f, 5.0f);
    if (options.headlight_reach) panel.headlight_reach = std::clamp(*options.headlight_reach, 1.0f, 5.0f);
    if (const auto ambient = remembered("night_ambient"))
        panel.night_ambient = std::clamp(static_cast<float>(std::atof(ambient->c_str())), 0.5f, 8.0f);
    if (const auto lamps = remembered("lamps_daylight"))
        panel.effects.lamps_in_daylight = std::clamp(static_cast<float>(std::atof(lamps->c_str())), 0.0f, 1.0f);
    if (options.lamps_daylight) panel.effects.lamps_in_daylight = std::clamp(*options.lamps_daylight, 0.0f, 1.0f);
    if (const auto bounce = remembered("ground_bounce"))
        panel.effects.ground_bounce = std::clamp(static_cast<float>(std::atof(bounce->c_str())), 0.0f, 0.5f);
    if (options.ground_bounce) panel.effects.ground_bounce = std::clamp(*options.ground_bounce, 0.0f, 0.5f);
    if (const auto count = remembered("lamp_shadows")) panel.effects.lamp_shadows = static_cast<std::uint32_t>(std::clamp(std::atoi(count->c_str()), 0, 4));
    if (options.lamp_shadows) panel.effects.lamp_shadows = static_cast<std::uint32_t>(std::clamp(*options.lamp_shadows, 0, 4));
    if (const auto saturation = remembered("saturation"))
        panel.saturation = std::clamp(static_cast<float>(std::atof(saturation->c_str())), 0.5f, 1.8f);
    if (const auto exposure = remembered("night_exposure"))
        panel.night_exposure = std::clamp(static_cast<float>(std::atof(exposure->c_str())), 0.5f, 6.0f);
    if (const auto exposure = remembered("exposure")) panel.exposure = std::clamp(static_cast<float>(std::atof(exposure->c_str())), 0.25f, 4.0f);
    if (const auto on = remembered("effect_shadows")) panel.effects.shadows = *on == "1";
    if (const auto on = remembered("effect_clusters")) panel.effects.clustered_lights = *on == "1";
    if (const auto on = remembered("effect_ao")) panel.effects.ambient_occlusion = *on == "1";
    if (const auto on = remembered("effect_aerial")) panel.effects.aerial_perspective = *on == "1";
    if (const auto on = remembered("parallel_recording")) panel.parallel_recording = *on == "1";
    if (const auto on = remembered("shadow_cache")) panel.effects.cache_far_cascades = *on == "1";
    if (options.shadow_cache) panel.effects.cache_far_cascades = *options.shadow_cache;
    if (const auto on = remembered("shadow_cab")) panel.effects.cab_shadows = *on == "1";
    if (options.cab_shadows) panel.effects.cab_shadows = *options.cab_shadows;
    if (options.parallel_recording) panel.parallel_recording = *options.parallel_recording;
    if (const auto value = remembered("shadow_distance"))
        panel.effects.shadow_distance = std::clamp(static_cast<float>(std::atof(value->c_str())), 100.0f, 1000.0f);
    if (const auto value = remembered("shadow_quality")) {
        const auto size = std::atoi(value->c_str());
        panel.effects.shadow_map_size = size <= 1024 ? 1024u : size >= 4096 ? 4096u : 2048u;
    }
    if (const auto value = remembered("shadow_cascades"))
        panel.effects.shadow_cascades = static_cast<std::uint32_t>(std::clamp(std::atoi(value->c_str()), 2, 4));
    if (const auto value = remembered("shadow_softness"))
        panel.effects.shadow_softness = std::clamp(static_cast<float>(std::atof(value->c_str())), 0.5f, 4.0f);
    if (options.shadow_settings) {
        // distance,size,cascades,softness
        std::array<double, 4> values{};
        const char* at = options.shadow_settings->c_str();
        std::size_t read = 0;
        for (; read < values.size() && *at; ++read) {
            char* end = nullptr;
            values[read] = std::strtod(at, &end);
            if (end == at) break;
            at = *end == ',' ? end + 1 : end;
        }
        const auto distance = static_cast<float>(values[0]);
        const auto size = static_cast<int>(values[1]);
        const auto cascades = static_cast<int>(values[2]);
        const auto softness = static_cast<float>(values[3]);
        if (read == values.size()) {
            panel.effects.shadow_distance = std::clamp(distance, 100.0f, 1000.0f);
            panel.effects.shadow_map_size = size <= 1024 ? 1024u : size >= 4096 ? 4096u : 2048u;
            panel.effects.shadow_cascades = static_cast<std::uint32_t>(std::clamp(cascades, 2, 4));
            panel.effects.shadow_softness = std::clamp(softness, 0.5f, 4.0f);
        }
    }
    if (options.effects) {
        const auto& list = *options.effects;
        const auto has = [&](const char* name) { return list.find(name) != std::string::npos; };
        panel.effects.shadows = has("shadows");
        panel.effects.clustered_lights = has("clusters");
        panel.effects.ambient_occlusion = has("ao");
        panel.effects.aerial_perspective = has("aerial");
        panel.effects.show_occlusion = has("show-ao");
    }
    // The field of view: the panel's, else RTS's (config.ini FOV, vertical, degrees).
    if (const auto fov = remembered("fov")) {
        panel.fov = std::clamp(static_cast<float>(std::atof(fov->c_str())), 30.0f, 100.0f);
    } else if (const auto rts = rts_config_value(read_rts_config(options.identity.root), "fov")) {
        panel.fov = std::clamp(*rts, 30.0f, 100.0f);
    }
    StageClock stage_clock;
    StageTimes stages{};
    double last_time = glfwGetTime();
    while (!glfwWindowShouldClose(window.get())) {
        // The FPS limit: the wait comes before the input is read.
        limiter.set(panel.fps_limit);
        limiter.wait();
        stage_clock.start();
        glfwPollEvents();
        const double now = glfwGetTime();
        if (placement_keeper) placement_keeper->update(window.get(), now);
        int gpu_count = 0;
        const double gpu_sum = gpu_timer.take_sum_ms(gpu_count);
        panel.stats.add(now - last_time, stages, gpu_sum, gpu_count);
        stages = {};
        const auto elapsed = static_cast<float>((std::min)(now - last_time, max_frame_seconds));
        input.keyboard_captured = scene.keyboard_captured();
        input.scene_takes_scroll = scene.takes_scroll();
        update_camera_from_keyboard(window.get(), input, elapsed);
        last_time = now;

        toggle_shading_from_keyboard(window.get(), input, renderer);
        if (!panel.experimental_supported) renderer.set_shading_path(renderer::ShadingPath::rts_compatible);
        {
            // Pause: trains, DLLs, the clock and the sound stop; the free camera still flies.
            const bool pause_key = glfwGetKey(window.get(), GLFW_KEY_PAUSE) == GLFW_PRESS;
            if (pause_key && !input.pause_key_down) panel.paused = !panel.paused;
            input.pause_key_down = pause_key;
        }
        if (sound) sound->set_paused(panel.paused);
        renderer::physical_light_falloff() = panel.physical_light;
        renderer::night_ambient_scale() = panel.night_ambient;
        renderer::light_range() = panel.light_range;
        renderer::headlight_reach() = panel.headlight_reach;
        renderer::light_fullbright() = panel.light_fullbright;
        auto frame_input = scene_input(window.get(), input);
        if (capture && options.capture_camera) {
            if (const auto car = scene.train_car_transform(options.capture_car)) {
                const auto& c = *options.capture_camera;
                frame_input.light_eye = glm::vec3{*car * glm::vec4{c[0], c[1], c[2], 1.0f}};
            }
        }
        if (const auto view_camera = capture_view()) frame_input.light_eye = view_camera->eye();
        if (capture && !capture_switched && glfwGetTime() - capture_start >= options.capture_at * 0.5) {
            if (options.capture_cab) scene.enter_cab();
            if (options.capture_menu) frame_input.car_menu = true;
            capture_switched = true;
        } else if (capture && capture_switched && options.capture_menu && *options.capture_menu > 0 && !capture_menu_picked
                   && glfwGetTime() - capture_start >= options.capture_at * 0.6) {
            scene.choose_car_menu_item(*options.capture_menu);
            capture_menu_picked = true;
        } else if (capture && capture_menu_picked && capture_menu_next < options.capture_menu_more.size()) {
            // The next items: opened and picked in turn over 0.65..0.95 of capture_at.
            const double slot = 0.3 / static_cast<double>(options.capture_menu_more.size());
            const double at = 0.65 + slot * static_cast<double>(capture_menu_next);
            const double since = glfwGetTime() - capture_start;
            if (!capture_menu_open && since >= options.capture_at * at) {
                frame_input.car_menu = true;
                capture_menu_open = true;
            } else if (capture_menu_open && since >= options.capture_at * (at + slot * 0.5)) {
                scene.choose_car_menu_item(options.capture_menu_more[capture_menu_next++]);
                capture_menu_open = false;
            }
        }
        if (capture && options.capture_uncouple && !capture_uncoupled && glfwGetTime() - capture_start >= options.capture_at * 0.6) {
            scene.uncouple_driven(*options.capture_uncouple);
            capture_uncoupled = true;
        }
        if (capture && !options.capture_board.empty()) {
            const double since = glfwGetTime() - capture_start;
            const auto boards = options.capture_board.size();
            const auto board = static_cast<std::size_t>(capture_board_step / 3);
            const int phase = capture_board_step % 3;
            const double slot = 0.6 / static_cast<double>(boards);
            if (board < boards && since >= options.capture_at * (0.3 + slot * (static_cast<double>(board) + phase / 3.0))) {
                if (phase == 0) {
                    scene.stand_at_cab_door(options.capture_board[board]);
                } else {
                    // Enter (to that cab, its door opened), then Ctrl+Enter (up the steps).
                    frame_input.engineer_action = true;
                    frame_input.engineer_action_ctrl = phase == 2;
                }
                ++capture_board_step;
            }
        }
        if (capture && options.capture_drive && !capture_driven && glfwGetTime() - capture_start >= options.capture_at * 0.8) {
            scene.drive_unit(*options.capture_drive);
            capture_driven = true;
        }
        // The switches one after another, half a second apart, as a hand
        // would (a DLL checks the others' State, which follows SetState
        // after the animation).
        if (capture_switched && capture_switch_next < options.capture_switches.size()
            && glfwGetTime() - capture_start >= options.capture_at * 0.5 + 0.5 * static_cast<double>(capture_switch_next)) {
            const auto [id, state] = options.capture_switches[capture_switch_next++];
            scene.set_cab_switch(id, state);
        }
        if (const auto time = scene.take_time_of_day()) clock.time_of_day = *time;
        scene.update(renderer, camera, clock.advance(panel.paused || scene.paused() ? 0.0f : elapsed), frame_input);
        if (capture && options.capture_camera) {
            if (const auto car = scene.train_car_transform(options.capture_car)) {
                const auto& c = *options.capture_camera;
                const glm::vec3 eye{*car * glm::vec4{c[0], c[1], c[2], 1.0f}};
                const glm::vec3 target{*car * glm::vec4{c[3], c[4], c[5], 1.0f}};
                const auto direction = glm::normalize(target - eye);
                camera.fly = true;
                camera.position = eye;
                camera.pitch = std::asin(std::clamp(-direction.y, -1.0f, 1.0f));
                camera.yaw = std::atan2(-direction.x, -direction.z);
            }
        }
        if (const auto view_camera = capture_view()) camera = *view_camera;
        if (sound) {
            sound->set_listener(camera.eye(), camera.forward());
            sound->update();
        }
        const auto environment = scene.environment();
        stage_clock.mark(FrameStage::update, stages);
#ifdef OPENRTS_HAS_IMGUI
        // Ctrl+F6 (F6 alone is RTS's stations-ahead panel).
        const bool save_keys = glfwGetKey(window.get(), GLFW_KEY_F6) == GLFW_PRESS
            && (glfwGetKey(window.get(), GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS || glfwGetKey(window.get(), GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS);
        panel.save_view = save_keys && !input.save_key_down;
        input.save_key_down = save_keys;
        panel.present_mode = surface.present_mode_name();
        panel.sound = sound.get();
        overlay.build(scene, camera, renderer, clock, environment.time_of_day_applies, panel);
        if (panel.save_view) {
            panel.save_status = save_view(options.identity, scene.origin(), camera, clock, renderer.shading_path());
        }
        surface.set_vsync(panel.vsync);
#endif
        stage_clock.mark(FrameStage::panel, stages);

        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window.get(), &width, &height);
        if (width <= 0 || height <= 0) continue;
        surface.resize(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));

        const bool capture_now = capture && glfwGetTime() - capture_start >= options.capture_at;
        // Not on OpenGL: no copy there, and a kept, sampled multisampled
        // depth makes wgpu's GL frame several times slower.
        const bool reshade_depth = panel.reshade_depth && panel.experimental_supported;
        surface.set_depth_export(reshade_depth);
        const auto frame_texture = surface.acquire();
        stage_clock.mark(FrameStage::wait, stages);
        if (!frame_texture) continue;
        const auto frame_view = wgpuTextureCreateView(frame_texture, nullptr);
        const auto encoder = wgpuDeviceCreateCommandEncoder(device, nullptr);
        const bool hdr = tone && surface.hdr_view();

        const auto aspect = static_cast<float>(width) / static_cast<float>(height);
        FrameView frame{
            .eye = camera.eye(),
            .view = camera.view(),
            .projection = projection(camera, aspect, glm::radians(panel.fov)),
            .vertical_fov = glm::radians(panel.fov),
            .aspect = aspect,
            .viewport_height = static_cast<float>(height),
        };
        // The experimental path's effects: shadow cascades, the depth
        // pre-pass with its occlusion and the lamp clusters, before the
        // main pass (renderer::SceneEffects).
        const bool experimental = renderer.shading_path() == renderer::ShadingPath::experimental;
        effects.set_settings(panel.effects);
        effects.begin_frame({.view = frame.view,
                             .projection = frame.projection,
                             .eye = frame.eye,
                             .vertical_fov = frame.vertical_fov,
                             .aspect = aspect,
                             .near_plane = std::clamp(camera.distance * 0.002f, 0.01f, 0.05f),
                             .far_plane = camera.distance * 20.0f,
                             .width = static_cast<std::uint32_t>(width),
                             .height = static_cast<std::uint32_t>(height)},
                            environment.lighting, experimental);
        frame.effects = effects.bind_group();
        frame.shadow_range = effects.shadow_range();
        frame.pool = &recording_pool;
        renderer.set_effects(frame.effects);
        renderer.set_depth_passes(effects.wants_depth());
        renderer.begin_frame({
            .view_projection = frame.projection * frame.view,
            .camera_position = frame.eye,
            .lighting = environment.lighting,
        });
        scene.submit(renderer, frame);
        effects.set_focus(scene.shadow_focus());
        stage_clock.mark(FrameStage::cull, stages);
        renderer.prepare();
        stage_clock.mark(FrameStage::prepare, stages);
        const renderer::SceneEffects::DepthDraw depth_draw = [&](const WGPURenderPassEncoder depth_pass, const std::uint32_t slot,
                                                                  const glm::mat4& view_projection, const renderer::DepthPassKind kind) {
            scene.draw_depth(depth_pass, slot, view_projection, kind);
            renderer.render_depth(depth_pass, slot, view_projection, kind);
        };
        scene.prepare_depth();
        // In parallel the workers record and finish a command buffer each
        // (the cascades, the pre-pass) while this thread records the main pass.
        const bool parallel = panel.parallel_recording && experimental;
        if (parallel) {
            effects.record_parallel(device, recording_pool, depth_draw);
        } else {
            effects.record(encoder, depth_draw);
        }
        stage_clock.mark(FrameStage::effects, stages);
        const auto pass =
            begin_main_pass(encoder,
                            {.color = hdr ? surface.hdr_view() : frame_view,
                             .depth = surface.depth_view(),
                             .msaa = surface.msaa_view(),
                             .keep_depth = reshade_depth},
                            environment.clear_colour, gpu_timer.begin_frame(), scene.occlusion_query_set());
        stage_clock.mark(FrameStage::begin, stages);
        scene.draw_background(pass, frame);
        stage_clock.mark(FrameStage::background, stages);
        renderer.render(pass, [&] { scene.draw_before_translucent(pass, frame); },
                        [&] { scene.draw_after_translucent(pass, frame); });
        stage_clock.mark(FrameStage::models, stages);
        scene.draw_foreground(pass, frame);
#ifdef OPENRTS_HAS_IMGUI
        if (!hdr && !capture) overlay.render(pass);
#endif
        stage_clock.mark(FrameStage::foreground, stages);
        wgpuRenderPassEncoderEnd(pass);
        // The depth for ReShade: a single-sample copy of a multisampled one
        // (a single-sample depth is kept as it is, keep_depth above).
        const bool depth_copy = reshade_depth && renderer::render_sample_count() > 1;
        if (depth_copy) {
            // Made on first use: its pipeline does not exist on OpenGL.
            if (!depth_export) depth_export = std::make_unique<renderer::DepthExport>(device);
            depth_export->record(encoder, surface.depth_view(), static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
        }
        if (hdr) {
            // The tone curve to the screen, then the panels over it.
            // The RTS path stays as RTS drew it; the curve is the experimental path's.
            // Night from the sun's direct light (display-encoded RTS colour):
            // full day from 0.55, full night at 0.15 and below.
            const auto direct = glm::vec3(environment.lighting.sun_diffuse);
            const auto luma = glm::dot(direct, glm::vec3(0.2126f, 0.7152f, 0.0722f));
            const auto day = std::clamp((luma - 0.15f) / 0.4f, 0.0f, 1.0f);
            panel.night = 1.0f - day * day * (3.0f - 2.0f * day);
            const renderer::ToneSettings settings{
                .curve = renderer.shading_path() == renderer::ShadingPath::experimental ? static_cast<renderer::ToneCurve>(panel.tone_curve)
                                                                                         : renderer::ToneCurve::none,
                .exposure = panel.exposure,
                .night = panel.night,
                .night_exposure = panel.night_exposure * panel.exposure,
                .saturation = panel.saturation,
            };
            tone->render(encoder, surface.hdr_view(), frame_view, settings);
#ifdef OPENRTS_HAS_IMGUI
            const WGPURenderPassColorAttachment ui_colour{
                .view = frame_view,
                .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
                .loadOp = WGPULoadOp_Load,
                .storeOp = WGPUStoreOp_Store,
            };
            const WGPURenderPassDescriptor ui_descriptor{.colorAttachmentCount = 1, .colorAttachments = &ui_colour};
            const auto ui = wgpuCommandEncoderBeginRenderPass(encoder, &ui_descriptor);
            if (!capture) overlay.render(ui);
            wgpuRenderPassEncoderEnd(ui);
            wgpuRenderPassEncoderRelease(ui);
#endif
        }
        scene.after_main_pass(encoder);
        gpu_timer.resolve(encoder);

        FrameCapture shot;
        FrameCapture depth_shot;
        if (capture_now) shot = begin_capture(device, encoder, frame_texture, static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
        if (capture_now && depth_copy && depth_export->texture())
            depth_shot = begin_capture(device, encoder, depth_export->texture(), static_cast<std::uint32_t>(width),
                                       static_cast<std::uint32_t>(height));
        const auto commands = wgpuCommandEncoderFinish(encoder, nullptr);
        // The workers' command buffers first: the shadows and the occlusion
        // the main pass reads.
        auto submitted = parallel ? effects.finish_parallel(recording_pool) : std::vector<WGPUCommandBuffer>{};
        stage_clock.mark(FrameStage::finish, stages);
        submitted.push_back(commands);
        const auto queue = wgpuDeviceGetQueue(device);
        wgpuQueueSubmit(queue, submitted.size(), submitted.data());
        for (std::size_t i = 0; i + 1 < submitted.size(); ++i) wgpuCommandBufferRelease(submitted[i]);
        gpu_timer.submitted();
        scene.frame_submitted();
        renderer.frame_submitted(queue);
        wgpuQueueRelease(queue);
        wgpuCommandBufferRelease(commands);
        wgpuRenderPassEncoderRelease(pass);
        wgpuCommandEncoderRelease(encoder);
        wgpuTextureViewRelease(frame_view);
        if (shot.buffer) {
            const bool written = finish_capture(context.context, shot, surface.format(), options.capture_path);
            std::cout << (written ? "captured: " : "capture failed: ") << options.capture_path << std::endl;
            // The frame times before it, for comparing settings (--no-vsync).
            std::cout << "frame times: " << panel.stats.average_ms() << " ms avg, " << panel.stats.worst_ms() << " ms worst" << std::endl;
            for (const auto& [id, asked] : options.capture_switches) {
                if (const auto where = scene.cab_switch_state(id)) {
                    std::cout << "cab switch " << id << ": asked " << asked << ", State " << where->first << ", SetState " << where->second << std::endl;
                }
            }
            const auto model_stats = renderer.stats();
            std::cout << "models: " << model_stats.instances << " instances, " << model_stats.draw_calls << " draw calls" << std::endl;
            if (const auto world = scene.world_report(); !world.empty()) std::cout << "world: " << world << std::endl;
            if (options.capture_menu) std::cout << "car menu: " << scene.car_menu_report() << std::endl;
            const auto effect_stats = effects.stats();
            std::cout << "effects: " << effect_stats.cascades << " cascades (" << effect_stats.cascades_drawn << " drawn, nearest texel "
                      << effect_stats.near_texel * 1000.0f << " mm), cab map " << (effect_stats.focus ? "on" : "off");
            if (effect_stats.focus) std::cout << " (texel " << effect_stats.focus_texel * 1000.0f << " mm)";
            std::cout << ", daylight " << effect_stats.daylight << ", lamps x" << effect_stats.lamp_strength << ", " << effect_stats.lamp_shadows
                      << " lamp shadows";
            std::cout << ", " << effect_stats.lights << " lights" << std::endl;
            std::cout << "cpu stages (ms):";
            for (std::size_t i = 0; i < frame_stage_count; ++i) std::cout << ' ' << frame_stage_names[i] << ' ' << panel.stats.stage_ms()[i];
            std::cout << "; gpu main pass " << panel.stats.gpu_ms() << " ms" << std::endl;
            if (depth_shot.buffer) {
                const auto near_plane = std::clamp(camera.distance * 0.002f, 0.01f, 0.05f);
                report_reshade_depth(context.context, depth_shot, near_plane, camera.distance * 20.0f);
            }
            wgpuTextureRelease(frame_texture);
            return written ? exit_code::success : exit_code::device_failed;
        }
        surface.present();
        wgpuTextureRelease(frame_texture);
        renderer.collect_retired();
        stage_clock.mark(FrameStage::submit, stages);
    }
    return exit_code::success;
}

} // namespace openrts::viewer

#else // windowed viewer unavailable

namespace openrts::viewer {

int run_window_viewer(ViewerScene&, const WindowOptions&) {
    auto context = renderer::create_headless_wgpu_device();
    if (!context.ok()) {
        std::cerr << "WebGPU device failed: " << context.error << '\n';
        return exit_code::device_failed;
    }
    std::cerr << "Windowed viewer requires GLFW on Windows.\n";
    return exit_code::platform_failed;
}

} // namespace openrts::viewer

#endif
#endif
