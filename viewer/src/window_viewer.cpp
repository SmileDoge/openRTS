#include "window_viewer.hpp"

#include <openrts/core/profile.hpp>

#ifdef OPENRTS_HAS_WGPU
#include <openrts/renderer/depth.hpp>
#include <openrts/renderer/bc7.hpp>
#include <openrts/renderer/forest_clearance.hpp>
#include <openrts/renderer/glow_sprites.hpp>
#include <openrts/renderer/native_sky.hpp>
#include <openrts/renderer/lightning.hpp>
#include <openrts/renderer/terrain_renderer.hpp>
#include <openrts/renderer/webgpu_texture.hpp>
#include <openrts/renderer/screen_quads.hpp>
#include <openrts/renderer/auto_exposure.hpp>
#include <openrts/renderer/tone_map.hpp>
#include <openrts/renderer/bloom.hpp>
#include <openrts/renderer/light_shafts.hpp>
#include <openrts/renderer/depth_export.hpp>
#include <algorithm>
#include <array>
#include <cfloat>
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

#include "controls_help.hpp"
#include "discord_status.hpp"
#include "frame_limiter.hpp"
#include "graphics_presets_ui.hpp"
#include "graphics_settings.hpp"
#include <openrts/renderer/gpu_pass_timer.hpp>
#include <openrts/renderer/video_memory.hpp>
#include "key_layout.hpp"
#include "ui_fonts.hpp"
#include "ui_slider.hpp"
#include "ui_text.hpp"
#include "log_console.hpp"
#include "rts_config.hpp"
#include "thunder_view.hpp"
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

// The monitor the window is on: its own when full screen, else the one that
// holds the window's centre, else the primary.
[[nodiscard]] GLFWmonitor* monitor_of(GLFWwindow* window) {
    if (auto* own = glfwGetWindowMonitor(window)) return own;
    int x = 0, y = 0, width = 0, height = 0;
    glfwGetWindowPos(window, &x, &y);
    glfwGetWindowSize(window, &width, &height);
    const int cx = x + width / 2;
    const int cy = y + height / 2;
    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    for (int i = 0; i < count; ++i) {
        const GLFWvidmode* video = glfwGetVideoMode(monitors[i]);
        int mx = 0, my = 0;
        glfwGetMonitorPos(monitors[i], &mx, &my);
        if (video && cx >= mx && cx < mx + video->width && cy >= my && cy < my + video->height) return monitors[i];
    }
    return glfwGetPrimaryMonitor();
}

// Shows the window as the window mode asks (window_mode.hpp): full screen in
// the monitor's own mode, borderless over the monitor, or back in the window
// at the place it had.
class WindowModeKeeper {
public:
    [[nodiscard]] WindowMode applied() const noexcept { return applied_; }

    void apply(GLFWwindow* window, const WindowMode mode) {
        if (mode == applied_) return;
        if (applied_ == WindowMode::windowed) {
            maximized_ = glfwGetWindowAttrib(window, GLFW_MAXIMIZED) != 0;
            if (maximized_) glfwRestoreWindow(window); // the place to come back to
            glfwGetWindowPos(window, &x_, &y_);
            glfwGetWindowSize(window, &width_, &height_);
        }
        GLFWmonitor* monitor = monitor_of(window);
        const GLFWvidmode* video = monitor ? glfwGetVideoMode(monitor) : nullptr;
        if (mode != WindowMode::windowed && !video) return;
        int mx = 0, my = 0;
        if (monitor) glfwGetMonitorPos(monitor, &mx, &my);
        switch (mode) {
        case WindowMode::fullscreen:
            glfwSetWindowMonitor(window, monitor, 0, 0, video->width, video->height, video->refreshRate);
            break;
        case WindowMode::borderless:
            // Out of full screen first (if it was), then without its frame
            // over the whole monitor.
            glfwSetWindowMonitor(window, nullptr, mx, my, video->width, video->height, GLFW_DONT_CARE);
            glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_FALSE);
            glfwSetWindowPos(window, mx, my);
            glfwSetWindowSize(window, video->width, video->height);
            break;
        case WindowMode::windowed:
            glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_TRUE);
            glfwSetWindowMonitor(window, nullptr, x_, y_, width_, height_, GLFW_DONT_CARE);
            if (maximized_) glfwMaximizeWindow(window);
            break;
        }
        applied_ = mode;
    }

private:
    WindowMode applied_ = WindowMode::windowed;
    int x_ = 100, y_ = 100, width_ = 1280, height_ = 720;
    bool maximized_ = false;
};

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
                .usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopySrc,
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
    [[nodiscard]] WGPUTexture hdr_texture() const noexcept { return hdr_; }

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
enum class FrameStage { update, panel, wait, begin, background, cull, prepare, effects, models, foreground, end, post, encode, join, submit, count };
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
    "end",     // the main pass ended (wgpu takes its commands)
    "post",    // after it: the depth copy for ReShade, the HDR tone pass and the panel, the GPU timer
    "encode",  // the command buffer finished
    "join",    // waiting for the workers' passes (the shadows, the depth pre-pass)
    "submit",  // queue submit, present, retired resource collection
};
using StageTimes = std::array<double, frame_stage_count>;

// Splits the frame into stages on the CPU clock.
// Each stage is a Tracy zone too (core/profile.hpp), opened under the
// stage that came next the frame before; between frames: the FPS limit's
// wait (and the perf line).
class StageClock {
public:
    StageClock() { next_.fill(between); }
    void start() {
        zone(between);
        last_ = std::chrono::steady_clock::now();
    }
    void mark(const FrameStage stage, StageTimes& times) {
        const auto now = std::chrono::steady_clock::now();
        times[static_cast<std::size_t>(stage)] += std::chrono::duration<double>(now - last_).count();
        last_ = now;
        zone(static_cast<std::size_t>(stage));
    }

private:
    static constexpr std::size_t between = frame_stage_count;
    static const char* name(const std::size_t stage) { return stage < frame_stage_count ? frame_stage_names[stage] : "FPS limit"; }
    // The open zone was `done`: closed under it, and the next is opened.
    void zone(const std::size_t done) {
        zone_.close(name(done));
        next_[previous_] = done;
        previous_ = done;
        zone_.open(name(next_[done]));
    }

    std::chrono::steady_clock::time_point last_;
    core::SequentialZone zone_;
    std::array<std::size_t, frame_stage_count + 1> next_{};
    std::size_t previous_ = between;
};

// Frame rate over the last half second: average FPS and frame time, the
// slowest frame (stutter shows there first), the average of each CPU stage
// and the GPU time of each pass (renderer::GpuPassTimer; a pass drawn every
// other frame or fourth counts at its share of a frame).
class FrameStats {
public:
    void add(const double seconds, const StageTimes& stages, const renderer::GpuPassTimes& gpu) {
        window_ += seconds;
        worst_ = (std::max)(worst_, seconds);
        for (std::size_t i = 0; i < frame_stage_count; ++i) stage_sum_[i] += stages[i];
        for (std::size_t i = 0; i < renderer::gpu_pass_count; ++i) {
            gpu_sum_.ms[i] += gpu.ms[i];
            gpu_sum_.seen[i] = gpu_sum_.seen[i] || gpu.seen[i];
        }
        gpu_sum_.frame_ms += gpu.frame_ms;
        gpu_sum_.frames += gpu.frames;
        ++frames_;
        if (window_ < 0.5) return;
        fps_ = frames_ / window_;
        average_ms_ = window_ * 1000.0 / frames_;
        worst_ms_ = worst_ * 1000.0;
        for (std::size_t i = 0; i < frame_stage_count; ++i) stage_ms_[i] = stage_sum_[i] * 1000.0 / frames_;
        if (gpu_sum_.frames > 0) {
            for (std::size_t i = 0; i < renderer::gpu_pass_count; ++i) gpu_pass_ms_[i] = gpu_sum_.ms[i] / gpu_sum_.frames;
            gpu_seen_ = gpu_sum_.seen;
            gpu_frame_ms_ = gpu_sum_.frame_ms / gpu_sum_.frames;
        }
        window_ = 0.0;
        worst_ = 0.0;
        stage_sum_ = {};
        gpu_sum_ = {};
        frames_ = 0;
    }

    [[nodiscard]] double fps() const noexcept { return fps_; }
    [[nodiscard]] double average_ms() const noexcept { return average_ms_; }
    [[nodiscard]] double worst_ms() const noexcept { return worst_ms_; }
    [[nodiscard]] const StageTimes& stage_ms() const noexcept { return stage_ms_; }
    // The main pass's GPU time (the log's "gpu main pass").
    [[nodiscard]] double gpu_ms() const noexcept { return gpu_pass_ms_[static_cast<std::size_t>(renderer::GpuPass::main)]; }
    [[nodiscard]] double gpu_frame_ms() const noexcept { return gpu_frame_ms_; }
    [[nodiscard]] const std::array<double, renderer::gpu_pass_count>& gpu_pass_ms() const noexcept { return gpu_pass_ms_; }
    [[nodiscard]] const std::array<bool, renderer::gpu_pass_count>& gpu_seen() const noexcept { return gpu_seen_; }
    // "name ms, name ms ..." of the passes seen (the log).
    [[nodiscard]] std::string gpu_report() const {
        std::string text;
        char number[32];
        for (std::size_t i = 0; i < renderer::gpu_pass_count; ++i) {
            if (!gpu_seen_[i]) continue;
            std::snprintf(number, sizeof number, " %.3f", gpu_pass_ms_[i]);
            text += (text.empty() ? "" : ", ") + std::string(renderer::gpu_pass_name(static_cast<renderer::GpuPass>(i))) + number;
        }
        return text;
    }

private:
    double window_{};
    double worst_{};
    int frames_{};
    StageTimes stage_sum_{};
    renderer::GpuPassTimes gpu_sum_{};
    double fps_{};
    double average_ms_{};
    double worst_ms_{};
    StageTimes stage_ms_{};
    std::array<double, renderer::gpu_pass_count> gpu_pass_ms_{};
    std::array<bool, renderer::gpu_pass_count> gpu_seen_{};
    double gpu_frame_ms_{};
};

// Panel controls that the main loop acts on after the panel is built.
struct PanelState {
    FrameStats stats;
    const char* present_mode = "Fifo";
    bool vsync = true;
    bool vsync_optional = false;
    int fps_limit = 0; // 0: none, else 30..500
    bool gpu_timing = false;
    renderer::VideoMemory video_memory; // the card's memory, read twice a second
    audio::AudioEngine* sound = nullptr;
    bool save_view = false;
    std::string save_status;
    // RTS's Pause key (0x402FC3): the world stands still, the camera moves.
    bool paused = false;
    // The panel's tab (testers, 2026-11-14: the sections one under another
    // lost what one looked for): general, graphics, train, world,
    // performance, the keys.
    enum class Tab : std::uint8_t { general, graphics, train, world, performance, controls };
    Tab tab = Tab::general;
    LogConsole console; // F2: the game's messages (log_console.hpp)
    // The frame rate in a corner of the screen, the panel shown or not
    // (remembered as fps_counter).
    bool fps_counter = false;
    // RTS's clock (Num4 / Shift+-, 0x402C88 byte_61D55C): the world's time
    // in the top right corner; shown too while time runs faster (0x408574).
    bool show_clock = false;
    // Until when the hint "Esc: the panels" shows while they are hidden
    // (set when a scene has loaded and when Esc hides them; ImGui time).
    double panels_hint_until = -1.0;
    // The window (window_mode.hpp): the mode wanted, which the main loop
    // applies; the full screen kind Alt+Enter goes to; fixed: always a
    // window (a capture, the start menu).
    WindowMode window_mode = WindowMode::windowed;
    WindowMode full_mode = WindowMode::fullscreen;
    bool window_mode_fixed = false;
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
    float night_exposure = 1.5f;
    renderer::AutoExposureSettings auto_exposure; // PBR stage 3
    float auto_exposure_factor = 1.0f;            // its factor now, for the panel
    float night = 0.0f; // 0 day .. 1 night, from the sun (shown)
    float saturation = 1.0f;
    bool physical_light = false; // physical falloff of headlights and lamps (experimental renderer)
    bool pbr = true;             // physically based light (experimental renderer, HDR; renderer::pbr_shading)
    float night_ambient = 1.0f;  // sky light at night, a factor (experimental renderer)
    float light_range = 0.0f;    // lamps lit within this distance, m; 0: RTS's MaxLightDistance (experimental renderer)
    float headlight_reach = 1.0f; // the trains' headlights reach this many times the .eng's (experimental renderer)
    bool light_fullbright = false; // FullBright surfaces lit as ordinary ones (experimental renderer)
    bool lamp_own_model = true;    // a scenery lamp throws no shadow of its own model (experimental)
    double loaded_at = -1.0;       // ImGui time the loading screen went (its fade); -1 while it shows
    // Forests keep their trees off tracks and roads (openRTS; RTS plants them
    // over the whole area): the crown this far from the way's edge, m.
    bool forest_clearance = true;
    float forest_clearance_distance = 3.0f;
    bool native_water = true; // the openRTS water instead of the environment's layers
    bool native_sky = true;   // openRTS's own sky (atmosphere, volumetric clouds) instead of RTS's dome
    bool signal_lights_real_size = true; // signal lights at the lens's size however far (not RTS's far angle)
    bool night_textures = true;          // the models' NIGHT textures at night (as RTS)
    // The trains' DXT1 textures without the blocks' squares, kept in BC7
    // (renderer/bc7.hpp; openRTS's own; remembered as smooth_train_textures).
    bool smooth_train_textures = false;
    int compress_textures = 2; // renderer/bc7.hpp: 0 not, 1 in memory, 2 kept on disk
    int texture_size = 0;      // renderer::texture_size_limit: 0 as the files, 1 1024, 2 512, 3 256
    bool screen_supersampling = true;    // the cab screens supersampled (renderer::cab_screens_supersampled)
    float cab_near_plane = 0.0f;         // m, seated in a cab (experiment); 0: as anywhere else
    bool cloud_cover_weather = true; // its clouds' cover by the weather, else cloud_cover
    float cloud_cover = 42.0f;       // %
    bool cloud_shadows = true;       // its clouds' shadows on the ground (the experimental path)
    float cloud_shadow_darkness = 85.0f; // % of the sun a thick cloud's shadow takes
    float cloud_shadow_sharpness = 3.0f; // their edges: the clouds' density in them times this
    float fov = default_fov_degrees; // vertical field of view, degrees
    // The experimental path's effects (renderer::SceneEffects).
    renderer::SceneEffectSettings effects;
    // Shadow cascades and the depth pre-pass recorded on worker threads.
    bool parallel_recording = true;
    bool multi_draw = true;
    // The graphics settings as kept when the game started (MSAA, the
    // renderer and HDR are the start's), for the presets.
    GraphicsSettings kept_graphics;
    OwnPresetsUi own_presets; // the presets of one's own (graphics_presets_ui.hpp)
    // The MSAA samples this start asked for (the device may draw with
    // fewer: renderer::render_sample_count()).
    std::uint32_t msaa_asked = 4;
};

// The panel's graphics settings from and to the start menu's
// (graphics_settings.hpp): at the start, and for a preset.
void set_panel_graphics(PanelState& panel, const GraphicsSettings& g) {
    panel.tone_curve = g.tone_curve;
    panel.exposure = g.exposure;
    panel.night_exposure = g.night_exposure;
    panel.auto_exposure = g.auto_exposure;
    panel.saturation = g.saturation;
    const bool show_occlusion = panel.effects.show_occlusion;
    const int material_view = panel.effects.material_view;
    panel.effects = g.effects;
    panel.effects.show_occlusion = show_occlusion;
    panel.effects.material_view = material_view;
    panel.physical_light = g.physical_light;
    panel.pbr = g.pbr;
    panel.light_range = g.light_range;
    panel.headlight_reach = g.headlight_reach;
    panel.night_ambient = g.night_ambient;
    panel.light_fullbright = g.light_fullbright;
    panel.lamp_own_model = g.lamp_own_model;
    panel.native_sky = g.native_sky;
    panel.cloud_cover_weather = g.cloud_cover_weather;
    panel.cloud_cover = g.cloud_cover;
    panel.cloud_shadows = g.cloud_shadows;
    panel.cloud_shadow_darkness = g.cloud_shadow_darkness;
    panel.cloud_shadow_sharpness = g.cloud_shadow_sharpness;
    panel.native_water = g.native_water;
    panel.forest_clearance = g.forest_clearance;
    panel.forest_clearance_distance = g.forest_clearance_distance;
    panel.signal_lights_real_size = g.signal_lights_real_size;
    panel.night_textures = g.night_textures;
    panel.screen_supersampling = g.screen_supersampling;
    panel.reshade_depth = g.reshade_depth;
    panel.smooth_train_textures = g.smooth_train_textures;
    panel.compress_textures = g.compress_textures;
    panel.texture_size = g.texture_size;
}

[[nodiscard]] GraphicsSettings panel_graphics(const PanelState& panel) {
    auto g = panel.kept_graphics;
    g.tone_curve = panel.tone_curve;
    g.exposure = panel.exposure;
    g.night_exposure = panel.night_exposure;
    g.auto_exposure = panel.auto_exposure;
    g.saturation = panel.saturation;
    g.effects = panel.effects;
    g.effects.show_occlusion = false;
    g.effects.material_view = 0;
    g.physical_light = panel.physical_light;
    g.pbr = panel.pbr;
    g.light_range = panel.light_range;
    g.headlight_reach = panel.headlight_reach;
    g.night_ambient = panel.night_ambient;
    g.light_fullbright = panel.light_fullbright;
    g.lamp_own_model = panel.lamp_own_model;
    g.native_sky = panel.native_sky;
    g.cloud_cover_weather = panel.cloud_cover_weather;
    g.cloud_cover = panel.cloud_cover;
    g.cloud_shadows = panel.cloud_shadows;
    g.cloud_shadow_darkness = panel.cloud_shadow_darkness;
    g.cloud_shadow_sharpness = panel.cloud_shadow_sharpness;
    g.native_water = panel.native_water;
    g.forest_clearance = panel.forest_clearance;
    g.forest_clearance_distance = panel.forest_clearance_distance;
    g.signal_lights_real_size = panel.signal_lights_real_size;
    g.night_textures = panel.night_textures;
    g.screen_supersampling = panel.screen_supersampling;
    g.reshade_depth = panel.reshade_depth;
    g.smooth_train_textures = panel.smooth_train_textures;
    g.compress_textures = panel.compress_textures;
    g.texture_size = panel.texture_size;
    return g;
}

// World clock: time of day advanced by real frame time times `speed`.
struct ClockControls {
    double time_of_day = 12.0 * 3600.0;
    float speed = 1.0f;
    int season = static_cast<int>(renderer::rts::Season::summer);
    // The RTS weather's order, then 3: a thunderstorm (rain with lightning).
    int weather = static_cast<int>(renderer::rts::Weather::clear);

    [[nodiscard]] SceneClock advance(const float elapsed) {
        constexpr double day = renderer::rts::seconds_per_day;
        time_of_day = std::fmod(time_of_day + static_cast<double>(elapsed) * speed, day);
        if (time_of_day < 0.0) time_of_day += day;
        return {
            .time_of_day = static_cast<std::int32_t>(time_of_day),
            .time_of_day_exact = time_of_day,
            .elapsed = elapsed,
            .time_speed = speed,
            .season = static_cast<renderer::rts::Season>(season),
            .weather = weather == 3 ? renderer::rts::Weather::rain : static_cast<renderer::rts::Weather>(weather),
            .storm = weather == 3,
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
        // The console (F2): the game's messages, on every screen (the
        // loading's too).
        draw_log_console(panel.console);

        if (panel.menu) {
            scene.draw_overlay(camera);
            ImGui::Render();
            return;
        }
        if (const auto loading = scene.loading(); !loading.done) {
            draw_loading_screen(loading);
            panel.loaded_at = -1.0;
            panel.panels_hint_until = -1.0;
            ImGui::Render();
            return;
        }
        {
            // After the loading screen its colour fades away (0.5 s,
            // smoothly) instead of the scene cutting in.
            constexpr double fade_time = 0.5;
            if (panel.loaded_at < 0.0) panel.loaded_at = ImGui::GetTime();
            const float left = 1.0f - static_cast<float>(std::clamp((ImGui::GetTime() - panel.loaded_at) / fade_time, 0.0, 1.0));
            if (left > 0.0f) {
                const float alpha = left * left * (3.0f - 2.0f * left);
                ImGui::GetForegroundDrawList()->AddRectFilled({0.0f, 0.0f}, ImGui::GetIO().DisplaySize,
                                                              IM_COL32(22, 24, 29, static_cast<int>(alpha * 255.0f)));
            }
        }
        if (panel.paused) {
            // As RTS writes it over the picture.
            const char* text = tr("PAUSE (Pause or F1)");
            const auto display = ImGui::GetIO().DisplaySize;
            const auto size = ImGui::CalcTextSize(text);
            auto* list = ImGui::GetForegroundDrawList();
            const ImVec2 at{(display.x - size.x) * 0.5f, display.y * 0.12f};
            list->AddRectFilled({at.x - 10.0f, at.y - 6.0f}, {at.x + size.x + 10.0f, at.y + size.y + 6.0f}, 0xb0000000u, 4.0f);
            list->AddText(at, 0xff40e0ffu, text);
        }
        // Esc shows and hides this panel and the DLL cab window, hidden at
        // the start (testers: they cluttered the picture); not while Esc
        // closes something of the scene's, nor in a text field.
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !scene.escape_taken() && !ImGui::GetIO().WantTextInput) {
            set_panels_shown(!panels_shown());
            if (!panels_shown()) panel.panels_hint_until = ImGui::GetTime() + 8.0;
        }
        if (!panels_shown()) {
            // The scene's own windows and HUD as ever; its lines for the
            // panel go nowhere.
            // Wholly off the screen (ImGui makes it at least WindowMinSize).
            ImGui::SetNextWindowPos({-200.0f, -200.0f});
            ImGui::SetNextWindowSize({1.0f, 1.0f});
            ImGui::Begin("##viewer_hidden", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground
                             | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
                             | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove);
            draw_scene_unseen(scene, camera);
            ImGui::End();
            if (panel.panels_hint_until < 0.0) panel.panels_hint_until = ImGui::GetTime() + 10.0;
            draw_fps_counter(panel);
            draw_world_clock(panel, clock);
            if (key_hints() && ImGui::GetTime() < panel.panels_hint_until) {
                const char* text = tr("Esc: the openRTS panel and the DLL cab window");
                auto* list = ImGui::GetForegroundDrawList();
                const auto size = ImGui::CalcTextSize(text);
                list->AddRectFilled({6.0f, 6.0f}, {14.0f + size.x, 14.0f + size.y}, 0x90000000u, 3.0f);
                list->AddText({10.0f, 10.0f}, 0xffd0d0d0u, text);
            }
            ImGui::Render();
            return;
        }
        draw_fps_counter(panel);
        draw_world_clock(panel, clock);
        ImGui::Begin(tr("openRTS viewer###viewer"));
        // What is open and how fast it runs; then a tab each: general,
        // graphics, the train, the world, performance, the keys.
        // Scene titles are literals: their text is NUL-terminated.
        ImGui::TextUnformatted(tr(scene.title().data()));
        ImGui::Text(tr("FPS %.0f  |  %.2f ms avg, %.2f ms worst"), panel.stats.fps(), panel.stats.average_ms(),
                    panel.stats.worst_ms());
        ImGui::SameLine();
        if (ImGui::SmallButton(panel.fps_counter ? tr("FPS on screen: on") : tr("FPS on screen: off"))) {
            panel.fps_counter = !panel.fps_counter;
            remember("fps_counter", panel.fps_counter ? "1" : "0");
        }
        if (ImGui::BeginTabBar("##panel_tabs")) {
            const auto tab = [&](const char* name, const PanelState::Tab which) {
                if (ImGui::BeginTabItem(name)) {
                    panel.tab = which;
                    ImGui::EndTabItem();
                }
            };
            tab(tr("General"), PanelState::Tab::general);
            tab(tr("Graphics"), PanelState::Tab::graphics);
            if (scene.has_train_panel()) tab(tr("Train"), PanelState::Tab::train);
            tab(tr("World"), PanelState::Tab::world);
            tab(tr("Performance"), PanelState::Tab::performance);
            tab(tr("Controls"), PanelState::Tab::controls);
            ImGui::EndTabBar();
        }
        if (panel.tab == PanelState::Tab::train && !scene.has_train_panel()) panel.tab = PanelState::Tab::general;
        if (panel.tab == PanelState::Tab::controls) {
            draw_controls_help();
            draw_scene_unseen(scene, camera);
            ImGui::End();
            ImGui::Render();
            return;
        }
        if (panel.tab == PanelState::Tab::train) {
            // The buttons and switches on top, the train's state below.
            scene.draw_train_panel();
            draw_scene_unseen(scene, camera);
            ImGui::End();
            ImGui::Render();
            return;
        }

        if (panel.tab == PanelState::Tab::general) {
            // The console: the game's messages (also F2).
            if (ImGui::Button(panel.console.open ? tr("Console (F2): close") : tr("Console (F2): the game's messages, warnings, errors")))
                panel.console.open = !panel.console.open;
            {
                const auto counts = log_counts();
                if (counts.errors > 0 || counts.warnings > 0) {
                    ImGui::SameLine();
                    ImGui::TextColored(counts.errors > 0 ? ImVec4{1.0f, 0.45f, 0.40f, 1.0f} : ImVec4{1.0f, 0.82f, 0.35f, 1.0f},
                                       tr("%llu errors, %llu warnings"), static_cast<unsigned long long>(counts.errors),
                                       static_cast<unsigned long long>(counts.warnings));
                }
            }
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
            bool hints = key_hints();
            if (ImGui::Checkbox(tr("Key hints at the bottom of the screen"), &hints)) {
                set_key_hints(hints);
                remember("key_hints", hints ? "1" : "0");
            }
            bool wasd = wasd_walking();
            if (ImGui::Checkbox(tr("Walk on W A S D while standing (seated: the cab's keys)"), &wasd)) {
                set_wasd_walking(wasd);
                remember("wasd_walk", wasd ? "1" : "0");
            }
            float sensitivity = mouse_sensitivity();
            ImGui::SetNextItemWidth(220.0f);
            if (slider_with_input(tr("Mouse sensitivity"), &sensitivity, min_mouse_sensitivity, max_mouse_sensitivity, "%.2f", "%.2f")) {
                set_mouse_sensitivity(sensitivity);
                remember("mouse_sensitivity", std::to_string(mouse_sensitivity()));
            }
            float head = head_motion();
            ImGui::SetNextItemWidth(220.0f);
            if (slider_with_input(tr("Head motion in the cab"), &head, 0.0f, max_head_motion, "%.2f", "%.2f")) {
                set_head_motion(head);
                remember("head_motion", std::to_string(head_motion()));
            }
            ImGui::TextDisabled("%s", tr("(the head nods with speeding up, braking and rail joints and leans in curves; 0: as RTS, the cab only sways)"));
            int joints = rail_joint_layout() == simulation::RailJointLayout::rts ? 1 : 0;
            const char* joint_layouts[] = {"openRTS", "RTS"};
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::Combo(tr("Rail joints"), &joints, joint_layouts, 2)) {
                set_rail_joint_layout(joints == 1 ? simulation::RailJointLayout::rts : simulation::RailJointLayout::openrts);
                remember("rail_joints", joints == 1 ? "rts" : "openrts");
            }
            ImGui::TextDisabled("%s", joints == 1 ? tr("(as RTS: a joint every 50 m; on stretches over 2 km four joints 12.5 m apart every 600 m)")
                                                  : tr("(25 m rails; on stretches of 1 km or more between switches welded strings of about 800 m with 3 or 4 joints 12.5 m apart between them)"));
            int bodies = body_motion_model() == simulation::BodyMotionModel::rts ? 1 : 0;
            const char* body_models[] = {"openRTS", "RTS"};
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::Combo(tr("Body motion"), &bodies, body_models, 2)) {
                set_body_motion_model(bodies == 1 ? simulation::BodyMotionModel::rts : simulation::BodyMotionModel::openrts);
                remember("body_motion", bodies == 1 ? "rts" : "openrts");
            }
            ImGui::TextDisabled("%s", bodies == 1 ? tr("(as RTS: one swing that grows with the speed, sideways, up and down)")
                                                  : tr("(each body on its own springs over its bogies: the track's errors under each bogie, rail joints and switches, hunting, curves; bounce, pitch, sway, yaw and roll)"));
            if (bodies == 0) {
                float throws = switch_throws();
                ImGui::SetNextItemWidth(220.0f);
                if (slider_with_input(tr("Throws aside at switches"), &throws, 0.0f, 2.0f, "%.2f", "%.2f")) {
                    set_switch_throws(throws);
                    remember("switch_throws", std::to_string(switch_throws()));
                }
                ImGui::TextDisabled("%s", tr("(1: a smooth push of the bogie, harder with the speed and on the diverging route; 0: none)"));
            }
            ImGui::Checkbox(tr("Pause (Pause or F1)"), &panel.paused);
        }

        if (panel.tab == PanelState::Tab::graphics) {
            {
                // A preset: the costly settings together, as in the start menu.
                const auto current = panel_graphics(panel);
                const auto preset = matching_graphics_preset(current);
                ImGui::SetNextItemWidth(220.0f);
                if (ImGui::BeginCombo(tr("Graphics preset"), preset ? tr(graphics_preset_name(*preset)) : tr("Custom"))) {
                    for (int i = 0; i < graphics_preset_count; ++i) {
                        const auto which = static_cast<GraphicsPreset>(i);
                        if (ImGui::Selectable(tr(graphics_preset_name(which)), preset && *preset == which)) {
                            auto now = current;
                            apply_graphics_preset(now, which);
                            save_graphics_settings(now, current);
                            set_panel_graphics(panel, now);
                            panel.kept_graphics = now;
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextWrapped("%s", preset ? tr(graphics_preset_note(*preset)) : tr("Custom (changed by hand)"));
                ImGui::PopStyleColor();
            }
            {
                // The presets of one's own, to pass to other players: the
                // renderer as it draws now (F10), the rest as the panel has it.
                ImGui::SeparatorText(tr("Your presets (to share with other players)"));
                auto current = panel_graphics(panel);
                current.experimental = renderer.shading_path() == renderer::ShadingPath::experimental;
                auto now = current;
                if (draw_own_graphics_presets(panel.own_presets, now, 220.0f)) {
                    save_graphics_settings(now, load_graphics_settings());
                    set_panel_graphics(panel, now);
                    panel.kept_graphics = now;
                    if (panel.experimental_supported)
                        renderer.set_shading_path(now.experimental ? renderer::ShadingPath::experimental : renderer::ShadingPath::rts_compatible);
                }
            }
            {
                // MSAA: its own choice, from the next start (the scene's
                // targets and pipelines are made with the samples).
                constexpr std::uint32_t counts[] = {1, 2, 4, 8};
                const char* names[] = {tr("no anti-aliasing"), "2x", "4x", "8x"};
                int index = 2;
                for (int i = 0; i < 4; ++i) {
                    if (counts[i] == panel.kept_graphics.msaa) index = i;
                }
                ImGui::SetNextItemWidth(220.0f);
                if (ImGui::Combo(tr("Anti-aliasing (MSAA)"), &index, names, 4)) {
                    auto now = panel_graphics(panel);
                    now.msaa = counts[index];
                    save_graphics_settings(now, panel_graphics(panel));
                    panel.kept_graphics.msaa = now.msaa;
                }
                const auto drawn = renderer::render_sample_count();
                if (panel.kept_graphics.msaa != panel.msaa_asked) {
                    ImGui::TextDisabled("%s", tr("(from the next start)"));
                } else if (drawn != panel.msaa_asked) {
                    ImGui::TextDisabled(tr("(%ux: this video card does not take it here, drawn with %ux)"), panel.msaa_asked, drawn);
                } else if (drawn <= 1) {
                    ImGui::TextDisabled("%s", tr("(off: RTS's content loses its alpha to coverage, thin wires break up)"));
                }
            }
            // The lighting: RTS's or the experimental one (R); not on OpenGL.
            auto path = renderer.shading_path();
            int path_index = path == renderer::ShadingPath::experimental ? 1 : 0;
            const char* paths[] = {tr("RTS"), tr("Experimental")};
            ImGui::BeginDisabled(!panel.experimental_supported);
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::Combo(tr("Renderer (F10)"), &path_index, paths, 2))
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
                ImGui::TextDisabled("%s", tr("In the background (another window in front, or minimised): at most 30 frames/s."));
            }
            if (!panel.window_mode_fixed) {
                // In a window, borderless or full screen (Alt+Enter: the window
                // or the full screen kind chosen last).
                int mode = static_cast<int>(panel.window_mode);
                const char* modes[] = {window_mode_name(WindowMode::windowed), window_mode_name(WindowMode::borderless),
                                       window_mode_name(WindowMode::fullscreen)};
                ImGui::SetNextItemWidth(220.0f);
                if (ImGui::Combo(tr("Window (Alt+Enter)"), &mode, modes, window_mode_count)) {
                    panel.window_mode = static_cast<WindowMode>(mode);
                    if (panel.window_mode != WindowMode::windowed) panel.full_mode = panel.window_mode;
                    remember("window_mode", window_mode_key(panel.window_mode));
                }
            }
            ImGui::SetNextItemWidth(220.0f);
            if (slider_with_input(tr("Field of view (Num+ / Num-)"), &panel.fov, 15.0f, 100.0f, tr("%.1f degrees"), "%.1f"))
                remember("fov", std::to_string(panel.fov));
            ImGui::BeginDisabled(!panel.experimental_supported);
            if (ImGui::Checkbox(tr("Depth for ReShade"), &panel.reshade_depth))
                remember("reshade_depth", panel.reshade_depth ? "1" : "0");
            ImGui::EndDisabled();
            if (!panel.experimental_supported && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", tr("Not available on OpenGL: choose Vulkan or DirectX 12 (Graphics API) and restart."));
            else if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("Keeps the scene depth to the end of the frame and, with anti-aliasing, adds a copy ReShade can take. In ReShade: RESHADE_DEPTH_INPUT_IS_REVERSED 1, RESHADE_DEPTH_MULTIPLIER 20 (see INSTALL.md)."));
            if (ImGui::Checkbox(tr("Keep trees off tracks and roads (not as in RTS)"), &panel.forest_clearance))
                remember("forest_clearance", panel.forest_clearance ? "1" : "0");
            ImGui::BeginDisabled(!panel.forest_clearance);
            ImGui::SetNextItemWidth(220.0f);
            if (slider_with_input(tr("Trees from the track's or road's edge"), &panel.forest_clearance_distance, 0.5f, 20.0f, tr("%.1f m"), "%.1f"))
                remember("forest_clearance_distance", std::to_string(panel.forest_clearance_distance));
            ImGui::EndDisabled();
            if (ImGui::Checkbox(tr("openRTS water: waves, sky reflection, soft shores (not as in RTS)"), &panel.native_water))
                remember("native_water", panel.native_water ? "1" : "0");
            if (ImGui::Checkbox(tr("Signal lights at the lens's size, far off a dot (not as in RTS)"), &panel.signal_lights_real_size))
                remember("signal_lights_real_size", panel.signal_lights_real_size ? "1" : "0");
            if (ImGui::Checkbox(tr("Night textures of the models (as in RTS)"), &panel.night_textures))
                remember("night_textures", panel.night_textures ? "1" : "0");
            ImGui::TextDisabled("%s", tr("(TEXTURES\\NIGHT: lit windows and lamps painted in; off: the day's textures at night, lit by the lamps)"));
            if (ImGui::Checkbox(tr("Smooth the compression blocks of the trains' textures (BC7, not as in RTS)"), &panel.smooth_train_textures))
                remember("smooth_train_textures", panel.smooth_train_textures ? "1" : "0");
            ImGui::TextDisabled("%s", tr("(the DXT1 of cars and cabs: the squares of its 4x4 blocks smoothed where they meet, kept in BC7, twice the video memory of the trains; a train loaded after: the first time some seconds longer, then from the cache)"));
            {
                const char* modes[] = {tr("Don't compress (as in RTS)"), tr("Compress, not kept on disk"), tr("Compress and keep on disk")};
                ImGui::SetNextItemWidth(260.0f);
                if (ImGui::Combo(tr("Uncompressed textures and PBR maps (BC7, normals BC5)"), &panel.compress_textures, modes, 3))
                    remember("compress_textures", std::to_string(panel.compress_textures));
                ImGui::TextDisabled("%s", tr("(compressed: a quarter of their video memory, barely seen; on disk: the first load longer, then read from the cache; not kept: each load longer, nothing written; textures loaded after the change)"));
                const char* sizes[] = {tr("as the files give them (as in RTS)"), "1024", "512", "256"};
                ImGui::SetNextItemWidth(260.0f);
                if (ImGui::Combo(tr("Textures' largest side##texture_size"), &panel.texture_size, sizes, 4))
                    remember("texture_size", std::to_string(panel.texture_size));
                ImGui::TextDisabled("%s", tr(panel.texture_size == 0   ? "(full size: as much video memory as the content takes)"
                                             : panel.texture_size == 1 ? "(2048 ones at 1024: a quarter of their memory, near objects a little softer)"
                                             : panel.texture_size == 2 ? "(at 512: a quarter of 1024's; cabs and near cars blurred)"
                                                                       : "(at 256: the least memory; everything near blurred)"));
                // The disk cache: counted every 2 s while shown, cleared on the button.
                static renderer::TextureCacheSize cache_size;
                static double counted_at = -100.0;
                if (ImGui::GetTime() - counted_at > 2.0) {
                    cache_size = renderer::texture_cache_size(renderer::texture_cache_directory());
                    counted_at = ImGui::GetTime();
                }
                ImGui::Text(tr("The compressed textures' cache on disk: %.0f MB, %llu files"), static_cast<double>(cache_size.bytes) / 1048576.0,
                            static_cast<unsigned long long>(cache_size.files));
                ImGui::SameLine();
                if (ImGui::Button(tr("Clear the cache"))) {
                    const auto removed = renderer::clear_texture_cache(renderer::texture_cache_directory());
                    std::cout << "texture cache: removed " << removed.files << " files, " << removed.bytes / 1048576 << " MB" << std::endl;
                    counted_at = -100.0;
                }
                ImGui::TextDisabled("%s", tr("(the textures already loaded stay; the next loads compress theirs again)"));
            }
            // The materials' height maps (SceneEffectSettings::parallax).
            if (ImGui::Checkbox(tr("Relief by the materials' height maps (parallax, not as in RTS)"), &panel.effects.parallax))
                remember("parallax", panel.effects.parallax ? "1" : "0");
            if (panel.effects.parallax) {
                ImGui::SetNextItemWidth(220.0f);
                if (slider_with_input(tr("Relief out to"), &panel.effects.parallax_distance, 5.0f, 100.0f, tr("%.0f m"), "%.0f"))
                    remember("parallax_distance", std::to_string(panel.effects.parallax_distance));
            }
            ImGui::TextDisabled("%s", tr("(only materials with a height map: the image shifts where the eye's ray meets the relief; the outline stays flat)"));
            if (ImGui::Checkbox(tr("Sharp cab screens: drawn at a higher resolution (not as in RTS)"), &panel.screen_supersampling))
                remember("screen_supersampling", panel.screen_supersampling ? "1" : "0");
            ImGui::TextDisabled("%s", tr("(each screen drawn into an image of its own and blended in linear light: thin lines keep their brightness, no steps on edges and needles)"));
            ImGui::SetNextItemWidth(220.0f);
            if (slider_with_input(tr("Near plane seated in the cab (experiment)"), &panel.cab_near_plane, 0.0f, 0.3f, tr("%.2f m"), "%.2f"))
                remember("cab_near_plane", std::to_string(panel.cab_near_plane));
            ImGui::TextDisabled("%s", tr("(0: as anywhere else, 1 cm; RTS draws with 0.2 m: what is nearer the eye is cut away)"));
            if (ImGui::Checkbox(tr("openRTS sky: atmosphere, volumetric clouds (not as in RTS)"), &panel.native_sky))
                remember("native_sky", panel.native_sky ? "1" : "0");
            if (panel.native_sky) {
                ImGui::Indent();
                if (ImGui::Checkbox(tr("Cloud cover by the weather (clear 42 %, snow 85 %, rain 92 %, thunderstorm 97 %)"), &panel.cloud_cover_weather))
                    remember("cloud_cover_weather", panel.cloud_cover_weather ? "1" : "0");
                ImGui::BeginDisabled(panel.cloud_cover_weather);
                ImGui::SetNextItemWidth(220.0f);
                if (slider_with_input(tr("Cloud cover"), &panel.cloud_cover, 0.0f, 100.0f, "%.0f %%", "%.0f"))
                    remember("cloud_cover", std::to_string(panel.cloud_cover));
                ImGui::EndDisabled();
                if (ImGui::Checkbox(tr("Cloud shadows on the ground (experimental renderer)"), &panel.cloud_shadows))
                    remember("cloud_shadows", panel.cloud_shadows ? "1" : "0");
                ImGui::BeginDisabled(!panel.cloud_shadows);
                ImGui::SetNextItemWidth(220.0f);
                if (slider_with_input(tr("Cloud shadow darkness (the sun a thick cloud takes)"), &panel.cloud_shadow_darkness, 0.0f, 100.0f, "%.0f %%", "%.0f"))
                    remember("cloud_shadow_darkness", std::to_string(panel.cloud_shadow_darkness));
                ImGui::SetNextItemWidth(220.0f);
                if (slider_with_input(tr("Cloud shadow sharpness (1: soft, as the clouds)"), &panel.cloud_shadow_sharpness, 1.0f, 10.0f, "%.1f", "%.1f"))
                    remember("cloud_shadow_sharpness", std::to_string(panel.cloud_shadow_sharpness));
                ImGui::EndDisabled();
                ImGui::Unindent();
            }
            bool batching = renderer.batching();
            if (ImGui::Checkbox(tr("Merge identical primitives (instancing)"), &batching)) renderer.set_batching(batching);
            bool culling = renderer.face_culling();
            if (ImGui::Checkbox(tr("Cull back faces (RTS)"), &culling)) renderer.set_face_culling(culling);
            if (panel.hdr) {
                ImGui::SeparatorText("HDR");
                const char* curves[] = {tr("none (as RTS)"), "Khronos PBR Neutral", "AgX (punchy)", "ACES",
                                        tr("auto: Khronos by day, AgX by night"), tr("auto: ACES by day, AgX by night")};
                ImGui::SetNextItemWidth(220.0f);
                if (ImGui::Combo(tr("Tone curve (experimental renderer)"), &panel.tone_curve, curves, 6))
                    remember("tone_mode", std::to_string(panel.tone_curve));
                // Stage 0 of PBR (docs/experimental-effects.md, "The frame's
                // encoding"): the experimental HDR frame linear or as before.
                ImGui::Checkbox(tr("HDR frame in linear light (off: gamma 2.2, as before; a check)"), &renderer::linear_hdr());
                ImGui::TextDisabled(renderer::frame_linear() ? tr("this frame: linear light") : tr("this frame: gamma 2.2"));
                ImGui::SetNextItemWidth(220.0f);
                if (slider_with_input(panel.auto_exposure.on ? tr("Exposure correction (with the auto exposure)") : tr("Exposure"), &panel.exposure,
                                      0.25f, 4.0f, "%.2f", "%.2f", ImGuiSliderFlags_Logarithmic))
                    remember("exposure", std::to_string(panel.exposure));
                // PBR stage 3 (docs/experimental-effects.md, "Auto exposure").
                if (ImGui::Checkbox(tr("Auto exposure (the picture gets used to the light, as an eye)"), &panel.auto_exposure.on))
                    remember("auto_exposure", panel.auto_exposure.on ? "1" : "0");
                if (panel.auto_exposure.on) {
                    auto& a = panel.auto_exposure;
                    ImGui::Indent();
                    ImGui::SetNextItemWidth(200.0f);
                    if (slider_with_input(tr("How far it follows the light (1: all the way)"), &a.strength, 0.0f, 1.0f, "%.2f", "%.2f"))
                        remember("auto_exposure_strength", std::to_string(a.strength));
                    ImGui::SetNextItemWidth(200.0f);
                    if (slider_with_input(tr("Darkens a bright scene by at most, stops"), &a.down, 0.0f, 3.0f, "%.1f", "%.1f"))
                        remember("auto_exposure_down", std::to_string(a.down));
                    ImGui::SetNextItemWidth(200.0f);
                    if (slider_with_input(tr("Brightens a dark scene by at most, stops"), &a.up, 0.0f, 3.0f, "%.1f", "%.1f"))
                        remember("auto_exposure_up", std::to_string(a.up));
                    ImGui::SetNextItemWidth(200.0f);
                    if (slider_with_input(tr("How fast it gets used to the light"), &a.speed, 0.1f, 5.0f, "%.1f", "%.1f", ImGuiSliderFlags_Logarithmic))
                        remember("auto_exposure_speed", std::to_string(a.speed));
                    if (renderer.shading_path() != renderer::ShadingPath::experimental) {
                        ImGui::TextDisabled("%s", tr("(in effect in the experimental renderer)"));
                    } else {
                        const float stops = std::log2((std::max)(panel.auto_exposure_factor, 1e-3f));
                        ImGui::TextDisabled(tr("now: x %.2f (%+.1f stops)"), panel.auto_exposure_factor, stops);
                        // At a limit: the setting holding it, named (it reads as "not changing" else).
                        const auto& ae = panel.auto_exposure;
                        if (ae.up > 0.0f && stops >= ae.up - 0.02f) {
                            ImGui::SameLine();
                            ImGui::TextColored({1.0f, 0.82f, 0.35f, 1.0f}, "%s", tr("at the limit of \"brightens a dark scene\""));
                        } else if (ae.down > 0.0f && stops <= -ae.down + 0.02f) {
                            ImGui::SameLine();
                            ImGui::TextColored({1.0f, 0.82f, 0.35f, 1.0f}, "%s", tr("at the limit of \"darkens a bright scene\""));
                        }
                    }
                    ImGui::Unindent();
                }
                if (panel.tone_curve == 4 || panel.tone_curve == 5) {
                    ImGui::SetNextItemWidth(220.0f);
                    if (slider_with_input(tr("Night exposure"), &panel.night_exposure, 0.5f, 6.0f, "%.2f", "%.2f", ImGuiSliderFlags_Logarithmic))
                        remember("night_exposure", std::to_string(panel.night_exposure));
                    ImGui::TextDisabled(tr("night %.0f %%"), panel.night * 100.0f);
                }
                ImGui::SetNextItemWidth(220.0f);
                if (slider_with_input(tr("Saturation"), &panel.saturation, 0.5f, 1.8f, "%.2f", "%.2f"))
                    remember("saturation", std::to_string(panel.saturation));
            }
        }

        if (panel.tab == PanelState::Tab::graphics) {
            ImGui::SeparatorText(tr("Experimental renderer"));
            ImGui::BeginDisabled(!panel.experimental_supported);
            // PBR stage 1 (docs/experimental-effects.md, "Physically based
            // light"): it needs the HDR frame in linear light, so without it
            // the choice is kept but greyed, the reason beside it.
            {
                const bool can_pbr = panel.hdr && renderer::linear_hdr();
                ImGui::BeginDisabled(!can_pbr);
                if (ImGui::Checkbox(tr("Physically based light (PBR: GGX reflections; off: as before)"), &panel.pbr))
                    remember("pbr", panel.pbr ? "1" : "0");
                ImGui::EndDisabled();
                if (can_pbr && panel.pbr) {
                    // PBR stage 2: the sky's light from its probe, or the old
                    // one (a check, not kept).
                    ImGui::Indent();
                    ImGui::Checkbox(tr("Sky light from the sky itself (IBL; off: the old sky light, a check)"), &renderer::sky_light_from_probe());
                    if (renderer::sky_light_from_probe()) {
                        // Its tuning (SceneEffectSettings), kept.
                        auto& e = panel.effects;
                        ImGui::SetNextItemWidth(200.0f);
                        if (slider_with_input(tr("Sky light strength (1: as the sky gives it)"), &e.sky_light_strength, 0.0f, 2.0f, "%.2f", "%.2f"))
                            remember("sky_light_strength", std::to_string(e.sky_light_strength));
                        ImGui::SetNextItemWidth(200.0f);
                        if (slider_with_input(tr("Sky reflections strength (1: as the sky gives them)"), &e.sky_reflection_strength, 0.0f, 2.0f,
                                              "%.2f", "%.2f"))
                            remember("sky_reflection_strength", std::to_string(e.sky_reflection_strength));
                        ImGui::SetNextItemWidth(200.0f);
                        if (slider_with_input(tr("Sky reflections in the cab and salon (their share)"), &e.sky_reflection_indoors, 0.0f, 1.0f,
                                              "%.2f", "%.2f"))
                            remember("sky_reflection_indoors", std::to_string(e.sky_reflection_indoors));
                        ImGui::SetNextItemWidth(200.0f);
                        if (slider_with_input(tr("Sky light in a tunnel, at least (its share)"), &e.sky_light_tunnel, 0.0f, 0.5f, "%.2f", "%.2f"))
                            remember("sky_light_tunnel", std::to_string(e.sky_light_tunnel));
                        if (ImGui::Button(tr("Sky light: defaults"))) {
                            const renderer::SceneEffectSettings defaults;
                            e.sky_light_strength = defaults.sky_light_strength;
                            e.sky_reflection_strength = defaults.sky_reflection_strength;
                            e.sky_reflection_indoors = defaults.sky_reflection_indoors;
                            e.sky_light_tunnel = defaults.sky_light_tunnel;
                            remember("sky_light_strength", std::to_string(e.sky_light_strength));
                            remember("sky_reflection_strength", std::to_string(e.sky_reflection_strength));
                            remember("sky_reflection_indoors", std::to_string(e.sky_reflection_indoors));
                            remember("sky_light_tunnel", std::to_string(e.sky_light_tunnel));
                        }
                    }
                    // openRTS's materials (PBR stage 4, docs/pbr-materials.md):
                    // a part of the models' material as the colour, a check.
                    const char* material_views[] = {tr("the picture"), tr("base colour"), tr("roughness"), tr("metallic"),
                                                    tr("normals"), tr("occlusion (the map's and GTAO's)"), tr("emission")};
                    ImGui::SetNextItemWidth(200.0f);
                    ImGui::Combo(tr("Show the material (a check)"), &panel.effects.material_view, material_views, 7);
                    ImGui::Unindent();
                }
                if (!panel.hdr) {
                    ImGui::TextDisabled("%s", tr("(needs HDR: turn it on in the start menu)"));
                } else if (!renderer::linear_hdr()) {
                    ImGui::TextDisabled("%s", tr("(needs the HDR frame in linear light, below)"));
                } else if (renderer.shading_path() != renderer::ShadingPath::experimental) {
                    ImGui::TextDisabled("%s", tr("(in effect in the experimental renderer)"));
                }
            }
            ImGui::SeparatorText(tr("Sun"));
            if (ImGui::Checkbox(tr("Sun shadows"), &panel.effects.shadows)) remember("effect_shadows", panel.effects.shadows ? "1" : "0");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("Cascaded shadow maps of the sun, as far from the camera as set below."));
            if (panel.effects.shadows) {
                ImGui::Indent();
                ImGui::SetNextItemWidth(200.0f);
                if (slider_with_input(tr("Shadow distance"), &panel.effects.shadow_distance, 100.0f, 1000.0f, tr("%.0f m"), "%.0f"))
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
                if (slider_with_input(tr("Shadow softness"), &panel.effects.shadow_softness, 0.5f, 4.0f, "%.1f", "%.1f"))
                    remember("shadow_softness", std::to_string(panel.effects.shadow_softness));
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("How blurred shadow edges are, in shadow map texels."));
                if (ImGui::Checkbox(tr("Shadow cache (what stays still)"), &panel.effects.cache_far_cascades))
                    remember("shadow_cache", panel.effects.cache_far_cascades ? "1" : "0");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", tr("The terrain, trees and buildings are drawn into the sun's shadows once and kept, only the strips the view moves into again; trains, cars and moving parts every frame. Video memory: a map a cascade more (64 MB at 4096)."));
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
                if (ImGui::SliderInt(tr("Lamp shadows"), &lamp_shadows, 0, static_cast<int>(renderer::SceneEffects::max_lamp_shadows))) {
                    panel.effects.lamp_shadows = static_cast<std::uint32_t>(lamp_shadows);
                    remember("lamp_shadows", std::to_string(lamp_shadows));
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", tr("Lamps that throw shadows: those lighting the cab (or the view) most, by power, direction and reach, so a floodlight does not shine through the roof. Each is one more pass."));
            }
            ImGui::SetNextItemWidth(200.0f);
            if (slider_with_input(tr("Light range"), &panel.light_range, 0.0f, 3000.0f, panel.light_range < 1.0f ? tr("as RTS") : tr("%.0f m"), "%.0f"))
                remember("light_range", std::to_string(panel.light_range));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("How far the scenery's lamps and floodlights still light the scene; at 0 as RTS's MaxLightDistance."));
            ImGui::SetNextItemWidth(200.0f);
            if (slider_with_input(tr("Headlight reach"), &panel.headlight_reach, 1.0f, 5.0f, "x%.1f", "%.1f"))
                remember("headlight_reach", std::to_string(panel.headlight_reach));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("How far the trains' headlights and searchlights reach, as a factor of their .eng; 1 as RTS."));
            if (ImGui::Checkbox(tr("Physical light falloff"), &panel.physical_light))
                remember("light_falloff", panel.physical_light ? "physical" : "rts");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("Headlights and lamps fall off as 1/distance squared with a soft cone edge; off: the RTS light shapes."));
            ImGui::SetNextItemWidth(200.0f);
            if (slider_with_input(tr("Lamps in daylight"), &panel.effects.lamps_in_daylight, 0.0f, 1.0f, "%.2f", "%.2f"))
                remember("lamps_daylight", std::to_string(panel.effects.lamps_in_daylight));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("How strong lamps are in full daylight against the night: an eye used to the sun hardly sees a cab lamp. 1: as at night."));

            ImGui::SeparatorText(tr("Sky and surroundings"));
            if (ImGui::Checkbox(tr("Ambient occlusion, GTAO"), &panel.effects.ambient_occlusion))
                remember("effect_ao", panel.effects.ambient_occlusion ? "1" : "0");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Darkens the sky light in corners and creases, from the depth of the scene."));
            if (panel.effects.ambient_occlusion) {
                ImGui::Indent();
                ImGui::TextUnformatted(tr("Occlusion quality:"));
                ImGui::SameLine();
                if (ImGui::RadioButton(tr("low (half resolution, several times faster)"), panel.effects.occlusion_low)) {
                    panel.effects.occlusion_low = true;
                    remember("effect_ao_quality", "low");
                }
                ImGui::SameLine();
                if (ImGui::RadioButton(tr("high##occlusion"), !panel.effects.occlusion_low)) {
                    panel.effects.occlusion_low = false;
                    remember("effect_ao_quality", "high");
                }
                ImGui::Unindent();
            }
            if (ImGui::Checkbox(tr("Aerial perspective"), &panel.effects.aerial_perspective))
                remember("effect_aerial", panel.effects.aerial_perspective ? "1" : "0");
            if (ImGui::Checkbox(tr("Bloom: light spreads round lamps, signals and the sun (HDR)"), &panel.effects.bloom))
                remember("effect_bloom", panel.effects.bloom ? "1" : "0");
            if (panel.effects.bloom) {
                ImGui::Indent();
                ImGui::SetNextItemWidth(200.0f);
                if (slider_with_input(tr("Bloom strength"), &panel.effects.bloom_strength, 0.0f, 3.0f, "%.2f", "%.2f"))
                    remember("bloom_strength", std::to_string(panel.effects.bloom_strength));
                if (ImGui::Checkbox(tr("At night from the textures' brightest too (lamps' bulbs, lit windows, signs)"),
                                    &panel.effects.bloom_textures))
                    remember("bloom_textures", panel.effects.bloom_textures ? "1" : "0");
                ImGui::Unindent();
            }
            if (ImGui::Checkbox(tr("Light shafts: the sun's beams in the air through trees, windows and clouds (HDR, not as in RTS)"),
                                &panel.effects.light_shafts))
                remember("effect_shafts", panel.effects.light_shafts ? "1" : "0");
            if (panel.effects.light_shafts) {
                ImGui::Indent();
                ImGui::SetNextItemWidth(200.0f);
                if (slider_with_input(tr("Light shafts' strength"), &panel.effects.light_shafts_strength, 0.0f, 4.0f, "%.2f", "%.2f"))
                    remember("shafts_strength", std::to_string(panel.effects.light_shafts_strength));
                ImGui::TextDisabled("%s", tr("(the dust in the air: stronger in haze and fog, and in the cab, where a beam through a window shows)"));
                if (!panel.effects.shadows) ImGui::TextDisabled("%s", tr("(they need the sun shadows on)"));
                ImGui::Unindent();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("Distance haze thinning with height and brighter towards the sun, instead of RTS's linear fog."));
            ImGui::SetNextItemWidth(200.0f);
            if (slider_with_input(tr("Night sky light"), &panel.night_ambient, 0.5f, 8.0f, "%.2f", "%.2f", ImGuiSliderFlags_Logarithmic))
                remember("night_ambient", std::to_string(panel.night_ambient));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("The environment's ambient light at night, as a factor: the unlit surroundings brighter, the lamps as they are. No effect by day."));
            ImGui::SetNextItemWidth(200.0f);
            if (slider_with_input(tr("Ground bounce"), &panel.effects.ground_bounce, 0.0f, 0.5f, "%.2f", "%.2f"))
                remember("ground_bounce", std::to_string(panel.effects.ground_bounce));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("Light the sunlit ground throws back onto bogies and the underframe, as the ground's reflectance (ballast and grass about 0.2). 0: none."));
            ImGui::SetNextItemWidth(200.0f);
            if (slider_with_input(tr("Light in the cab by day"), &panel.effects.indoor_light, 0.0f, 1.0f, "%.2f", "%.2f"))
                remember("indoor_light", std::to_string(panel.effects.indoor_light));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The sun through the windows and the sky's light thrown about the cab or salon you are in, which the shadows alone leave dark. 0: none."));
            if (ImGui::Checkbox(tr("Light FullBright as ordinary surfaces"), &panel.light_fullbright))
                remember("light_fullbright", panel.light_fullbright ? "1" : "0");
            if (ImGui::Checkbox(tr("A lamp throws no shadow of its own model"), &panel.lamp_own_model))
                remember("lamp_own_model", panel.lamp_own_model ? "1" : "0");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("MSTS FullBright surfaces (platforms, signs) take sun, sky and lamps; their darkened night textures are left out."));
            ImGui::EndDisabled();
        }

        if (panel.tab == PanelState::Tab::general) {
            ImGui::SeparatorText(tr("Sound"));
            if (panel.sound) {
                float volume = panel.sound->volume();
                ImGui::SetNextItemWidth(220.0f);
                if (slider_with_input(tr("Sound volume"), &volume, 0.0f, 1.0f, "%.2f", "%.2f")) {
                    panel.sound->set_volume(volume);
                    remember("volume", std::to_string(volume)); // the next run starts with it
                }
                ImGui::TextDisabled(tr("%zu voices, %zu heard"), panel.sound->active_voices(), panel.sound->audible_voices());
                int quiet = static_cast<int>(background_sound());
                const char* choices[] = {tr("plays as in front"), tr("quieter"), tr("silent")};
                ImGui::SetNextItemWidth(220.0f);
                if (ImGui::Combo(tr("Sound in the background (minimised or behind another window)"), &quiet, choices, 3)) {
                    set_background_sound(static_cast<BackgroundSound>(quiet));
                    remember("background_sound", background_sound_key(background_sound()));
                }
            } else {
                ImGui::TextDisabled("%s", tr("Sound: off"));
            }
        }

        if (panel.tab == PanelState::Tab::general) {
            ImGui::SeparatorText(tr("Time and weather"));
            draw_clock(clock, time_of_day_applies);
        }

        if (panel.tab == PanelState::Tab::world) {
            ImGui::Text(tr("Camera: %s"), camera.fly ? "fly" : "orbit");
            scene.draw_overlay(camera);
            if (ImGui::Button(tr("Save debug view (Ctrl+F6)"))) panel.save_view = true;
            if (!panel.save_status.empty()) ImGui::TextWrapped("%s", panel.save_status.c_str());
        } else {
            draw_scene_unseen(scene, camera);
        }

        if (panel.tab == PanelState::Tab::performance) {
            const auto& stages = panel.stats.stage_ms();
            double cpu = 0.0;
            for (std::size_t i = 0; i < frame_stage_count; ++i) {
                // "wait" is idle time, not work.
                if (static_cast<FrameStage>(i) != FrameStage::wait) cpu += stages[i];
                ImGui::Text("%-7s %6.2f ms", frame_stage_names[i], stages[i]);
            }
            ImGui::Text(tr("CPU work %5.2f ms (without wait)"), cpu);
            if (panel.gpu_timing) {
                // Each pass's GPU time, coloured by its share of the GPU's frame.
                const double frame = panel.stats.gpu_frame_ms();
                ImGui::Text(tr("GPU frame %5.2f ms (the first pass's start to the last one's end)"), frame);
                if (ImGui::BeginTable("##gpu_passes", 2, ImGuiTableFlags_SizingFixedFit)) {
                    for (std::size_t i = 0; i < renderer::gpu_pass_count; ++i) {
                        if (!panel.stats.gpu_seen()[i]) continue;
                        const double ms = panel.stats.gpu_pass_ms()[i];
                        const double share = frame > 0.0 ? ms / frame : 0.0;
                        const ImVec4 colour = share >= 0.4   ? ImVec4{1.00f, 0.40f, 0.32f, 1.0f}
                                              : share >= 0.2 ? ImVec4{1.00f, 0.64f, 0.28f, 1.0f}
                                              : share >= 0.1 ? ImVec4{0.92f, 0.84f, 0.36f, 1.0f}
                                                             : ImGui::GetStyleColorVec4(ImGuiCol_Text);
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(tr(renderer::gpu_pass_name(static_cast<renderer::GpuPass>(i))));
                        ImGui::TableNextColumn();
                        ImGui::TextColored(colour, "%6.2f ms  %3.0f %%", ms, share * 100.0);
                    }
                    ImGui::EndTable();
                }
            } else {
                ImGui::TextDisabled("%s", tr("GPU pass: no timestamp queries on this device"));
            }
            const auto gpu = renderer.stats();
            ImGui::Text(tr("GPU: %zu textures, %zu retired waiting, frame %llu done"), gpu.cached_textures, gpu.retired_pending,
                        static_cast<unsigned long long>(gpu.completed_frame));
            ImGui::Text(tr("Textures on the GPU: %.0f MB, the trains' %.0f MB, uncompressed %.0f MB"),
                        static_cast<double>(gpu.texture_bytes) / 1048576.0, static_cast<double>(gpu.train_texture_bytes) / 1048576.0,
                        static_cast<double>(gpu.rgba8_texture_bytes) / 1048576.0);
            // The card's memory as Windows counts it (renderer/video_memory.hpp).
            if (const auto memory = panel.video_memory; memory.known) {
                ImGui::Text(tr("Video memory: %.0f MB of the %.0f MB Windows lets the game keep (the card has %.0f MB), %.0f MB in shared memory"),
                            static_cast<double>(memory.usage) / 1048576.0, static_cast<double>(memory.budget) / 1048576.0,
                            static_cast<double>(memory.total) / 1048576.0, static_cast<double>(memory.shared_usage) / 1048576.0);
                if (memory.over_budget()) {
                    ImGui::TextColored(ImVec4{1.0f, 0.45f, 0.35f, 1.0f}, "%s",
                                       tr("Video memory over its budget: part of it is moved to system memory and the frame slows down many "
                                          "times. Lower the shadows' quality, the lamp shadows or the anti-aliasing."));
                }
            }
            ImGui::Text(tr("Models: %zu instances, %zu primitives, %zu draw calls"), gpu.instances, gpu.primitives, gpu.draw_calls);
            ImGui::Text(tr("Commands in the main pass: %zu"), gpu.draw_commands);
            ImGui::BeginDisabled(!renderer.multi_draw_supported());
            const auto multi_label = std::string(tr("Multi-draw indirect: a model's parts with one texture in one call"))
                + (renderer.multi_draw_supported() ? "" : std::string(tr(" (not on this device)")));
            if (ImGui::Checkbox(multi_label.c_str(), &panel.multi_draw)) remember("multi_draw", panel.multi_draw ? "1" : "0");
            ImGui::EndDisabled();
            if (ImGui::Checkbox(tr("Parallel recording"), &panel.parallel_recording))
                remember("parallel_recording", panel.parallel_recording ? "1" : "0");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("The shadow cascades and the depth pre-pass are recorded on worker threads while the main pass is."));
        }

        ImGui::End();

        ImGui::Render();
    }

    void render(const WGPURenderPassEncoder pass) const { ImGui_ImplWGPU_RenderDrawData(ImGui::GetDrawData(), pass); }

private:
    // The scene's own windows (the train, Track control) and its HUD stay
    // whatever the panel shows; its lines for the panel go to a region no
    // one sees (the Controls tab, the Scene section folded).
    // The start's loading screen over the whole window. The scene is drawn
    // under it all the same: its models and textures go to the GPU and its
    // pipelines are made meanwhile.
    static void draw_loading_screen(const SceneLoading& loading) {
        const auto display = ImGui::GetIO().DisplaySize;
        auto* list = ImGui::GetForegroundDrawList();
        list->AddRectFilled({0.0f, 0.0f}, display, IM_COL32(22, 24, 29, 255));
        const auto centred = [&](ImFont* font, const float size, const float y, const ImU32 colour, const char* text) {
            const auto extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
            list->AddText(font, size, {(display.x - extent.x) * 0.5f, y}, colour, text);
        };
        ImFont* normal = ImGui::GetFont();
        const float normal_size = ImGui::GetFontSize();
        ImFont* big = title_font() ? title_font() : normal;
        const float big_size = title_font() ? title_font()->FontSize : normal_size * 2.0f;
        ImFont* medium = large_font() ? large_font() : normal;
        const float medium_size = large_font() ? large_font()->FontSize : normal_size * 1.3f;
        const float middle = display.y * 0.5f;
        centred(big, big_size, middle - big_size - 48.0f, IM_COL32(230, 235, 242, 255), tr("Loading"));
        if (!loading.title.empty()) centred(medium, medium_size, middle - 36.0f, IM_COL32(200, 206, 216, 255), loading.title.c_str());
        // The bar.
        const float width = (std::min)(display.x * 0.5f, 640.0f);
        const ImVec2 from{(display.x - width) * 0.5f, middle + 4.0f};
        const ImVec2 to{from.x + width, from.y + 10.0f};
        list->AddRectFilled(from, to, IM_COL32(43, 48, 59, 255), 5.0f);
        const float share = std::clamp(loading.progress, 0.0f, 1.0f);
        if (share > 0.0f) list->AddRectFilled(from, {from.x + width * share, to.y}, IM_COL32(92, 148, 230, 255), 5.0f);
        if (loading.stage && *loading.stage) centred(normal, normal_size, to.y + 14.0f, IM_COL32(140, 148, 163, 255), tr(loading.stage));
    }

    // The frame rate in the screen's top right corner (the panel's "FPS on
    // screen"), whether the panel shows or not.
    static void draw_fps_counter(const PanelState& panel) {
        if (!panel.fps_counter) return;
        char text[64];
        std::snprintf(text, sizeof text, "%.0f FPS  %.1f ms", panel.stats.fps(), panel.stats.average_ms());
        auto* list = ImGui::GetForegroundDrawList();
        const auto size = ImGui::CalcTextSize(text);
        const auto display = ImGui::GetIO().DisplaySize;
        const ImVec2 at{display.x - size.x - 12.0f, 8.0f};
        list->AddRectFilled({at.x - 6.0f, at.y - 3.0f}, {at.x + size.x + 6.0f, at.y + size.y + 3.0f}, 0x90000000u, 3.0f);
        list->AddText(at, 0xff80ff80u, text);
    }

    // RTS's clock (0x408574): the world's time as H:MM:SS (0x45CA30) in the
    // top right corner, white, with " x60" while time runs faster, which
    // shows it as well; below the FPS counter when that is on.
    static void draw_world_clock(const PanelState& panel, const ClockControls& clock) {
        if (!panel.show_clock && clock.speed <= 1.0f) return;
        const auto seconds = static_cast<long long>(clock.time_of_day) % 86400;
        char text[48];
        int length = std::snprintf(text, sizeof text, "%lld:%02lld:%02lld", seconds / 3600, seconds % 3600 / 60, seconds % 60);
        if (clock.speed > 1.0f && length > 0) std::snprintf(text + length, sizeof text - static_cast<std::size_t>(length), " x%.0f", clock.speed);
        auto* list = ImGui::GetForegroundDrawList();
        const auto size = ImGui::CalcTextSize(text);
        const auto display = ImGui::GetIO().DisplaySize;
        const float top = panel.fps_counter ? 8.0f + size.y + 12.0f : 8.0f;
        const ImVec2 at{display.x - size.x - 12.0f, top};
        list->AddRectFilled({at.x - 6.0f, at.y - 3.0f}, {at.x + size.x + 6.0f, at.y + size.y + 3.0f}, 0x90000000u, 3.0f);
        list->AddText(at, 0xffffffffu, text);
    }

    static void draw_scene_unseen(const ViewerScene& scene, const ViewerCamera& camera) {
        // The scene's lines for the panel go nowhere (its own windows and
        // HUD as ever): clipped to an empty rectangle off the screen. Drawn
        // into the 1 x 1 child alone, its first pixel showed: a white dot
        // at the panel's end, or with the panel hidden at the screen's
        // top-left corner (testers, 2026-11-14).
        ImGui::BeginChild("##scene_unseen", {1.0f, 1.0f}, ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground);
        ImGui::PushClipRect({-16.0f, -16.0f}, {-16.0f, -16.0f}, false);
        scene.draw_overlay(camera);
        ImGui::PopClipRect();
        ImGui::EndChild();
    }

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
        // Order of the RTS weather variable: clear, snow, rain; then openRTS's thunderstorm.
        const char* weathers[] = {tr("Clear"), tr("Snow"), tr("Rain"), tr("Thunderstorm")};
        ImGui::Combo(tr("Weather"), &clock.weather, weathers, 4);
        if (clock.weather == 3) {
            if (ImGui::SliderFloat(tr("Lightning strikes a minute"), &renderer::lightning_rate(), 0.0f, 30.0f, "%.0f"))
                remember("lightning_rate", std::to_string(renderer::lightning_rate()));
            if (ImGui::SliderFloat(tr("Lightning brightness"), &renderer::lightning_brightness(), 0.0f, 3.0f, "%.1f"))
                remember("lightning_brightness", std::to_string(renderer::lightning_brightness()));
            if (ImGui::SliderFloat(tr("Thunder volume"), &thunder_volume(), 0.0f, 2.0f, "%.1f"))
                remember("thunder_volume", std::to_string(thunder_volume()));
            // A strike to try the light and the thunder at a distance.
            static float test_distance = 2000.0f;
            ImGui::SliderFloat(tr("Test lightning distance, m"), &test_distance, 100.0f, 12000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
            if (ImGui::Button(tr("Strike lightning ahead"))) renderer::lightning_test_request() = test_distance;
            if (clock.speed <= 0.0f || clock.speed > 16.0f) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", tr("(no lightning while the clock stands or runs faster than x16)"));
            }
        }
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
    // Which buttons turn the orbit: orbit_dragging is either.
    bool orbit_left = false;
    bool orbit_right = false;
    bool toggle_key_down = false;
    bool shading_key_down = false;
    bool pause_key_down = false;
    // Scene keys, for press edges.
    bool follow_key_down = false;
    bool track_key_down = false;
    bool labels_key_down = false;
    bool track_control_key_down = false;
    bool info_key_down = false;
    bool stations_key_down = false;
    bool drive_key_down = false;
    bool journal_key_down = false; // F11
    bool menu_key_down = false;    // F8
    bool car_view_key_down = false;
    bool save_key_down = false;
    bool window_mode_key_down = false; // Alt+Enter
    bool enter_key_down = false;
    bool kp0_down = false;
    bool kp1_down = false;
    bool kp3_down = false;
    bool kp2_down = false;
    bool kp5_down = false;
    bool kp4_down = false;
    bool kp7_down = false;
    bool inspector_key_down = false;
    bool tape_window_key_down = false; // F9
    bool free_camera_key_down = false; // Ctrl+F
    int clicks = 0;            // left clicks in fly mode since the last scene_input()
    int releases = 0;          // left button let go (anywhere, over a panel too)
    double scroll = 0.0;       // wheel since the last scene_input()
    bool scene_takes_scroll = false;
    std::array<bool, 14> train_keys_down{};
    std::vector<KeyEvent> key_events; // since the last scene_input()
    bool keyboard_captured = false;   // the scene takes the keyboard (cab keys)
    bool on_foot = false;             // the engineer stands or walks (not in a chair): W A S D walk
    double cursor_x = 0.0;
    double cursor_y = 0.0;
    // The cursor was captured or let go in this poll: its moves do not turn
    // the camera till the next (set_cursor_captured).
    bool skip_motion = false;
};

[[nodiscard]] CameraInput* camera_input(GLFWwindow* window) {
    return static_cast<CameraInput*>(glfwGetWindowUserPointer(window));
}

// The right button's look: the cursor hidden and held in the window, the
// mouse's raw motion where the system gives it. Without it GLFW turns the
// cursor's moves into steps from the window's middle, where it puts the
// cursor on capturing: a move still queued from before (the mouse moving
// as the button went down) became a step from the middle to where the
// button was pressed, and the camera turned at random.
void set_cursor_captured(GLFWwindow* window, const bool captured) {
    if (captured && glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    glfwSetInputMode(window, GLFW_CURSOR, captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    if (auto* input = static_cast<CameraInput*>(glfwGetWindowUserPointer(window))) input->skip_motion = true;
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
    // A press starts the look of the camera as it is; a release ends
    // whatever its press started, though the camera changed meanwhile (F3 or
    // F with the button held left the look on for good).
    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        if (pressed && input->camera->fly) ++input->clicks;
        input->orbit_left = pressed && !input->camera->fly;
    }
    if (button == GLFW_MOUSE_BUTTON_RIGHT) {
        if (pressed) {
            // The orbit (the car view too) turns with the right button as
            // well, as the look does elsewhere.
            input->fly_looking = input->camera->fly;
            input->orbit_right = !input->camera->fly;
            set_cursor_captured(window, true);
        } else if (action == GLFW_RELEASE) {
            const bool held = input->fly_looking || input->orbit_right;
            input->fly_looking = false;
            input->orbit_right = false;
            if (held) set_cursor_captured(window, false);
        }
    }
    input->orbit_dragging = input->orbit_left || input->orbit_right;
    glfwGetCursorPos(window, &input->cursor_x, &input->cursor_y);
}

void on_cursor_moved(GLFWwindow* window, const double x, const double y) {
#ifdef OPENRTS_HAS_IMGUI
    ImGui_ImplGlfw_CursorPosCallback(window, x, y);
#endif
    if (overlay_wants_mouse()) return;
    auto* input = camera_input(window);
    if (!input) return;

    if ((input->orbit_dragging || input->fly_looking) && !input->skip_motion) {
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
        // Ctrl and the wheel zoom the view (the field of view), in the cab
        // too (its controls then keep still); else outside the cab the wheel
        // brings the camera nearer, in it the cab has it.
        const bool ctrl = glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
        if (ctrl) {
            input->camera->zoom_view(static_cast<float>(y));
        } else {
            input->scroll += y;
            if (!input->scene_takes_scroll) input->camera->zoom(static_cast<float>(y));
        }
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

// RTS's field of view keys (rts1.exe 0x403715, WM_KEYDOWN): Num+ narrows it
// by half a degree while above 15, Num- widens it while below 90, on the
// press and on each of the key's repeats (here: after 0.5 s, 30 a second);
// with Ctrl RTS does something else with them, so they do nothing here.
struct FovKeys {
    int key = 0;        // 1 narrower, -1 wider, 0 none
    double held = 0.0;  // s since the press
    double next = 0.0;  // s at the next repeat
    bool changed = false;
};

// True when the key that changed the field of view was let go (remember it).
[[nodiscard]] bool update_fov_keys(GLFWwindow* window, FovKeys& state, float& fov, const float seconds, const bool keyboard_free) {
    const auto down = [window](const int key) { return glfwGetKey(window, key) == GLFW_PRESS; };
    const bool ctrl = down(GLFW_KEY_LEFT_CONTROL) || down(GLFW_KEY_RIGHT_CONTROL);
    int key = 0;
    if (keyboard_free && !ctrl) key = down(GLFW_KEY_KP_ADD) ? 1 : down(GLFW_KEY_KP_SUBTRACT) ? -1 : 0;
    if (key == 0) {
        const bool let_go = state.changed;
        state = {};
        return let_go;
    }
    int steps = 0;
    if (key != state.key) {
        state.key = key;
        state.held = 0.0;
        state.next = 0.5;
        steps = 1;
    } else {
        state.held += seconds;
        for (; state.held >= state.next && steps < 64; state.next += 1.0 / 30.0) ++steps;
    }
    for (int k = 0; k < steps; ++k) {
        if (key > 0 && fov > 15.0f) fov -= 0.5f;
        if (key < 0 && fov < 90.0f) fov += 0.5f;
        state.changed = true;
    }
    return false;
}

// Polled keyboard handling: F toggles fly/orbit on key press, WASD/QE move.
// The keyboard is ImGui's while one of its text boxes takes it (the console's
// message, a filter): the keys polled here stay out of the game then, as the
// key callback's do (on_key).
[[nodiscard]] bool imgui_has_keyboard() {
#ifdef OPENRTS_HAS_IMGUI
    return ImGui::GetIO().WantCaptureKeyboard;
#else
    return false;
#endif
}

void update_camera_from_keyboard(GLFWwindow* window, CameraInput& input, const float seconds) {
    const bool typing = imgui_has_keyboard();
    const auto down = [window, typing](const int key) { return !typing && glfwGetKey(window, key) == GLFW_PRESS; };
    auto& camera = *input.camera;

    // F is a cab key when the cab has the keyboard; Ctrl+F is the scene's
    // (SceneInput::free_camera).
    const bool ctrl = down(GLFW_KEY_LEFT_CONTROL) || down(GLFW_KEY_RIGHT_CONTROL);
    const bool toggle = down(GLFW_KEY_F) && !ctrl && !input.keyboard_captured;
    if (toggle && !input.toggle_key_down) {
        camera.toggle_fly();
        if (!camera.fly) {
            input.fly_looking = false;
            set_cursor_captured(window, false);
        }
    }
    input.toggle_key_down = toggle;
    if (input.keyboard_captured) return; // the cab has the letter keys
    if (input.on_foot && wasd_walking()) return; // W A S D walk the engineer

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
    const bool typing = imgui_has_keyboard();
    const auto down = [window, typing](const int key) { return !typing && glfwGetKey(window, key) == GLFW_PRESS; };
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
    const bool f4 = pressed(GLFW_KEY_F4, input.drive_key_down);
    const bool f7 = pressed(GLFW_KEY_F7, input.track_control_key_down);
    const bool f12 = pressed(GLFW_KEY_F12, input.inspector_key_down);
    // Ctrl+F9 the tape window (F9 alone is RTS's objectives, 0x402C88).
    const bool f9 = pressed(GLFW_KEY_F9, input.tape_window_key_down);
    // Ctrl+F: no cab of the installed content assigns it (their letters,
    // F among them, may all be the cab's; testers 2026-11-18).
    const bool free_camera = pressed(GLFW_KEY_F, input.free_camera_key_down) && ctrl && !alt;
    const bool enter_now = down(GLFW_KEY_ENTER) || down(GLFW_KEY_KP_ENTER);
    const bool enter = enter_now && !input.enter_key_down;
    input.enter_key_down = enter_now;
    // The engineer's window and head out (key_layout.hpp): RTS's keypad
    // keys, or - and = on the top row.
    const bool rts_keys = key_layout() == KeyLayout::rts;
    // - alone the window, Shift+- the clock (RTS: Num0 and Num4).
    const bool minus = pressed(rts_keys ? GLFW_KEY_KP_0 : GLFW_KEY_MINUS, input.kp0_down);
    const bool kp0 = minus && (rts_keys || !shift);
    const bool kp4 = rts_keys ? pressed(GLFW_KEY_KP_4, input.kp4_down) : minus && shift;
    // Ctrl+= is the boarding's in openRTS's layout, not the head out.
    const bool equal = pressed(rts_keys ? GLFW_KEY_KP_1 : GLFW_KEY_EQUAL, input.kp1_down);
    const bool kp1 = equal && (rts_keys || !ctrl);
    // ` alone the car menu, Shift+` the flashlight, Ctrl+` the tape (RTS:
    // Num3, Num2 and Ctrl+Num5).
    const bool grave = !rts_keys && pressed(GLFW_KEY_GRAVE_ACCENT, input.kp3_down);
    const bool kp3 = rts_keys ? pressed(GLFW_KEY_KP_3, input.kp3_down) : grave && !shift && !ctrl;
    const bool kp2 = rts_keys ? pressed(GLFW_KEY_KP_2, input.kp2_down) : grave && shift && !ctrl;
    const bool kp5 = rts_keys && pressed(GLFW_KEY_KP_5, input.kp5_down);
    const bool tape = rts_keys ? kp5 && ctrl : grave && ctrl;
    // The boarding at a scenario's stop (RTS: Ctrl+Num7, 0x403208).
    const bool board = rts_keys ? pressed(GLFW_KEY_KP_7, input.kp7_down) && ctrl : equal && ctrl;
    int width = 0, height = 0;
    glfwGetWindowSize(window, &width, &height);
    const bool pointer = !input.fly_looking && width > 0 && height > 0 && !overlay_wants_mouse()
                         && input.cursor_x >= 0.0 && input.cursor_y >= 0.0 && input.cursor_x < width && input.cursor_y < height;
    const int clicks = std::exchange(input.clicks, 0);
    const int releases = std::exchange(input.releases, 0);
    const double scroll = std::exchange(input.scroll, 0.0);
    // W A S D walk the engineer on his feet (key_layout.hpp): not the cab's
    // then, nor the free camera's.
    const bool wasd = wasd_walking() && input.on_foot && !ctrl && !alt;
    const auto walk_keys_out = [&](std::vector<KeyEvent>& keys) {
        if (!wasd_walking() || !input.on_foot) return;
        std::erase_if(keys, [](const KeyEvent& e) {
            return !e.ctrl && !e.alt && (e.key == GLFW_KEY_W || e.key == GLFW_KEY_A || e.key == GLFW_KEY_S || e.key == GLFW_KEY_D);
        });
    };
    const auto engineer = [&](SceneInput& in) {
        in.cab_window = kp0;
        in.head_out = kp1;
        in.car_menu = kp3;
        in.flashlight = kp2;
        in.clock = kp4;
        in.tape = tape;
        in.tape_which = kp5 && !ctrl;
        in.board = board;
        in.inspector = f12;
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
        in.walk_forward = down(GLFW_KEY_UP) || (wasd && down(GLFW_KEY_W));
        in.walk_back = down(GLFW_KEY_DOWN) || (wasd && down(GLFW_KEY_S));
        in.walk_left = down(GLFW_KEY_LEFT) || (wasd && down(GLFW_KEY_A));
        in.walk_right = down(GLFW_KEY_RIGHT) || (wasd && down(GLFW_KEY_D));
        in.walk_run = shift;
        in.walk_slow = down(GLFW_KEY_END);
        // Alt+Enter is the window's (full screen / window), not a door.
        in.engineer_action = enter && !alt;
        in.engineer_action_ctrl = enter && ctrl && !alt;
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
            .drive_panel = f4,
            .car_view = pressed(GLFW_KEY_F3, input.car_view_key_down),
            .signal_names = f7 && ctrl && !alt,
            .activity_journal = pressed(GLFW_KEY_F11, input.journal_key_down),
            .activity_menu = pressed(GLFW_KEY_F8, input.menu_key_down),
            .tape_window = f9 && ctrl,
            .free_camera = free_camera,
        };
        engineer(captured);
        captured.keys = std::exchange(input.key_events, {});
        walk_keys_out(captured.keys);
        return captured;
    }
    SceneInput result{
        .step_up = down(GLFW_KEY_UP),
        .step_down = down(GLFW_KEY_DOWN),
        .toggle_follow = pressed(GLFW_KEY_V, input.follow_key_down),
        .toggle_track = pressed(GLFW_KEY_T, input.track_key_down),
        .toggle_labels = pressed(GLFW_KEY_L, input.labels_key_down),
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
        .drive_panel = f4,
        .car_view = pressed(GLFW_KEY_F3, input.car_view_key_down),
        .signal_names = f7 && ctrl && !alt,
        .activity_journal = pressed(GLFW_KEY_F11, input.journal_key_down),
        .activity_menu = pressed(GLFW_KEY_F8, input.menu_key_down),
        .tape_window = f9 && ctrl,
        .free_camera = free_camera,
    };
    engineer(result);
    result.keys = std::exchange(input.key_events, {});
    walk_keys_out(result.keys);
    return result;
}

// F10 (until 2026-11-14 R, which cabs take too: both went at once).
void toggle_shading_from_keyboard(GLFWwindow* window, CameraInput& input, renderer::ModelRenderer& renderer) {
    const bool down = glfwGetKey(window, GLFW_KEY_F10) == GLFW_PRESS;
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

// The experiment (the panel's "Near plane seated in the cab"): the near
// plane while the engineer sits in a cab, 0: as anywhere else. Set each frame.
float seated_cab_near = 0.0f;

[[nodiscard]] float near_plane_of(const ViewerCamera& camera) {
    if (seated_cab_near > 0.0f) return seated_cab_near;
    // At most 5 cm: the route camera keeps distance 1000 (the far plane at
    // 20 km), which made the near plane 2 m and cut the cab's walls and floor
    // away. Reversed Z on Depth32Float keeps the far precision anyway.
    return std::clamp(camera.distance * 0.002f, 0.01f, 0.05f);
}

[[nodiscard]] glm::mat4 projection(const ViewerCamera& camera, const float aspect, const float vertical_fov) {
    return renderer::reversed_z_perspective(vertical_fov, aspect, near_plane_of(camera), camera.far_plane());
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
                                                    const std::optional<WGPURenderPassTimestampWrites>& timestamps,
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
        .timestampWrites = timestamps ? &*timestamps : nullptr,
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
                                         const std::uint32_t width, const std::uint32_t height, const std::uint32_t texel_bytes = 4) {
    FrameCapture shot{.width = width, .height = height, .row_bytes = (width * texel_bytes + 255) / 256 * 256};
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

// The light shafts' image of a captured frame (RGBA16Float, scene-linear):
// how bright they are, for tuning them by numbers.
// The HDR frame's light (--capture with --hdr): its luminance in linear
// light (a gamma 2.2 frame decoded, so the two encodings compare), over the
// whole frame and its regions: the mean, the median, the 10th, 90th and 99th
// percentiles and the most.
void report_hdr_frame(const renderer::WgpuDeviceContext& context, const FrameCapture& shot, const bool linear) {
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
        const auto half = [](const std::uint16_t h) {
            const std::uint32_t sign = (h >> 15) & 1u, exponent = (h >> 10) & 31u, mantissa = h & 1023u;
            float v = 0.0f;
            if (exponent == 0) v = std::ldexp(static_cast<float>(mantissa), -24);
            else if (exponent < 31) v = std::ldexp(static_cast<float>(mantissa | 1024u), static_cast<int>(exponent) - 25);
            else v = 65504.0f;
            return sign ? -v : v;
        };
        // Regions: the frame, its top, middle and bottom thirds, its middle ninth.
        constexpr const char* names[] = {"frame", "top", "middle", "bottom", "centre"};
        std::array<std::vector<float>, 5> values;
        for (auto& v : values) v.reserve(static_cast<std::size_t>(shot.width) * shot.height / 3);
        for (std::uint32_t y = 0; y < shot.height; ++y) {
            const auto* line = bytes + static_cast<std::size_t>(y) * shot.row_bytes;
            const std::size_t third = (std::min)(static_cast<std::size_t>(y) * 3 / shot.height, std::size_t{2});
            for (std::uint32_t x = 0; x < shot.width; ++x) {
                std::uint16_t c[3];
                std::memcpy(c, line + static_cast<std::size_t>(x) * 8u, sizeof c);
                float rgb[3];
                for (int k = 0; k < 3; ++k) {
                    const float stored = (std::max)(half(c[k]), 0.0f);
                    rgb[k] = linear ? stored : std::pow(stored, 2.2f);
                }
                const float luma = 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
                values[0].push_back(luma);
                values[1 + third].push_back(luma);
                if (x * 3 / shot.width == 1 && third == 1) values[4].push_back(luma);
            }
        }
        std::cout << "hdr frame (" << (linear ? "linear" : "gamma 2.2, decoded") << ", linear luminance):";
        for (std::size_t r = 0; r < values.size(); ++r) {
            auto& v = values[r];
            if (v.empty()) continue;
            double sum = 0.0;
            for (const float l : v) sum += l;
            const auto at = [&v](const double share) {
                const auto k = static_cast<std::size_t>(share * static_cast<double>(v.size() - 1));
                std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
                return v[k];
            };
            const double mean = sum / static_cast<double>(v.size());
            const float p10 = at(0.10), median = at(0.50), p90 = at(0.90), p99 = at(0.99);
            const float most = *std::max_element(v.begin(), v.end());
            std::printf(" %s mean %.4f median %.4f p10 %.4f p90 %.4f p99 %.4f max %.3f;", names[r], mean, median, p10, p90, p99, most);
        }
        std::cout << std::endl;
        wgpuBufferUnmap(shot.buffer);
    }
    wgpuBufferRelease(shot.buffer);
}

void report_light_shafts(const renderer::WgpuDeviceContext& context, const FrameCapture& shot) {
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
        const auto half = [](const std::uint16_t h) {
            const std::uint32_t sign = (h >> 15) & 1u, exponent = (h >> 10) & 31u, mantissa = h & 1023u;
            float v = 0.0f;
            if (exponent == 0) v = std::ldexp(static_cast<float>(mantissa), -24);
            else if (exponent < 31) v = std::ldexp(static_cast<float>(mantissa | 1024u), static_cast<int>(exponent) - 25);
            else v = 65504.0f;
            return sign ? -v : v;
        };
        double sum = 0.0, top = 0.0;
        std::size_t over = 0, strong = 0;
        std::array<double, 3> rows{}; // the top, middle and bottom thirds
        for (std::uint32_t y = 0; y < shot.height; ++y) {
            const auto* line = bytes + static_cast<std::size_t>(y) * shot.row_bytes;
            for (std::uint32_t x = 0; x < shot.width; ++x) {
                std::uint16_t c[3];
                std::memcpy(c, line + static_cast<std::size_t>(x) * 8u, sizeof c);
                const double luma = 0.2126 * half(c[0]) + 0.7152 * half(c[1]) + 0.0722 * half(c[2]);
                sum += luma;
                top = (std::max)(top, luma);
                over += luma > 0.01;
                strong += luma > 0.05;
                rows[(std::min)(static_cast<std::size_t>(y) * 3 / shot.height, std::size_t{2})] += luma;
            }
        }
        const double count = static_cast<double>(shot.width) * shot.height;
        std::cout << "light shafts: mean " << sum / count << ", max " << top << ", over 0.01 " << 100.0 * static_cast<double>(over) / count
                  << " %, over 0.05 " << 100.0 * static_cast<double>(strong) / count << " %; thirds top / middle / bottom "
                  << rows[0] / (count / 3) << " / " << rows[1] / (count / 3) << " / " << rows[2] / (count / 3) << std::endl;
        wgpuBufferUnmap(shot.buffer);
    }
    wgpuBufferRelease(shot.buffer);
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
    // The card's memory budget (DXGI; Windows only).
    std::unique_ptr<renderer::VideoMemoryProbe> video_memory_probe;
    {
        WGPUAdapterInfo info{};
        wgpuAdapterGetInfo(context.context.adapter(), &info);
        video_memory_probe = std::make_unique<renderer::VideoMemoryProbe>(info.vendorID, info.deviceID);
        wgpuAdapterInfoFreeMembers(info);
    }
    double video_memory_at = 0.0;
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

    renderer::render_sample_count() = renderer::supported_sample_count(context.context, options.msaa);
    renderer::scene_hdr() = options.hdr && !options.menu;
    if (options.hdr_linear) renderer::linear_hdr() = *options.hdr_linear;
    if (renderer::scene_hdr()) std::cout << "HDR frame: Rgba16Float, tone curve (AgX in the experimental path)" << std::endl;
    const auto samples = renderer::render_sample_count();
    std::cout << "antialiasing: " << (samples > 1 ? std::to_string(samples) + "x MSAA, alpha-to-coverage" : "off");
    if (samples != options.msaa) std::cout << " (" << options.msaa << "x asked: not offered by this device)";
    std::cout << ", anisotropy " << renderer::max_anisotropy << "x" << std::endl;
    PresentationSurface surface(context.context, window.get(), capture);
    if (!surface.ok()) return exit_code::surface_failed;

    renderer::ModelRenderer renderer(device, surface.scene_format());
    std::unique_ptr<renderer::ToneMapper> tone;
    std::unique_ptr<renderer::AutoExposure> auto_exposure; // made on first use
    bool auto_exposure_was_on = false;
    if (renderer::scene_hdr()) tone = std::make_unique<renderer::ToneMapper>(device, surface.format());
    // The HDR frame's bloom (the experimental path; made on first use).
    std::unique_ptr<renderer::BloomRenderer> bloom;
    std::unique_ptr<renderer::LightShaftRenderer> shafts;
    std::unique_ptr<renderer::DepthExport> depth_export;
    if (!renderer.ready()) {
        std::cerr << "Model renderer initialization failed.\n";
        return exit_code::renderer_failed;
    }
    renderer.set_shading_path(experimental_supported ? options.shading : renderer::ShadingPath::rts_compatible);
    renderer.set_batching(options.batching);
    // The experimental path's shadows, lamps, occlusion and haze.
    renderer::SceneEffects effects(device);
    effects.set_shadow_cache_check(options.shadow_cache_check);
    // The sky's light from its probe (PBR stage 2).
    renderer::SkyLight sky_light(device);
    if (!sky_light.ready()) std::cerr << "The sky's light could not be made: the physically based light keeps the old sky light.\n";
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
    // The keys chosen last time (RTS's, with the keypad, by default since
    // 2026-11-14: most players come from RTS; openRTS's for laptops).
    set_key_layout(remembered("key_layout").value_or("rts") == "openrts" ? KeyLayout::openrts : KeyLayout::rts);
    if (const auto throws = remembered("switch_throws")) set_switch_throws(std::strtof(throws->c_str(), nullptr));
    // A thunderstorm's lightning as last set.
    if (const auto rate = remembered("lightning_rate")) renderer::lightning_rate() = std::clamp(std::strtof(rate->c_str(), nullptr), 0.0f, 30.0f);
    if (const auto brightness = remembered("lightning_brightness"))
        renderer::lightning_brightness() = std::clamp(std::strtof(brightness->c_str(), nullptr), 0.0f, 3.0f);
    if (const auto volume = remembered("thunder_volume")) thunder_volume() = std::clamp(std::strtof(volume->c_str(), nullptr), 0.0f, 2.0f);
    set_key_hints(remembered("key_hints").value_or("1") != "0");
    set_wasd_walking(remembered("wasd_walk").value_or("1") != "0");
    set_mouse_sensitivity(parse_mouse_sensitivity(remembered("mouse_sensitivity").value_or("1").c_str()));
    set_head_motion(parse_head_motion(remembered("head_motion").value_or("").c_str()));
    set_background_sound(parse_background_sound(remembered("background_sound").value_or("").c_str()));
    set_rail_joint_layout(parse_rail_joint_layout(remembered("rail_joints").value_or("openrts").c_str()));
    set_body_motion_model(parse_body_motion_model(remembered("body_motion").value_or("openrts").c_str()));
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
    // From the end of the start's loading (moved on while it lasts).
    double capture_start = glfwGetTime();
    // The camera as the scene left it while it loads: no input moves it.
    ViewerCamera loading_camera = camera;
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
    bool capture_coupled = false;
    int capture_inspect_step = 0; // --capture-inspect: 1 opened, 2 clicked
    bool capture_driven = false;
    bool capture_taped = false;
    bool capture_boarded = false;
    int capture_minimise_step = 0; // --capture-minimise: 1 minimised, 2 back
    double capture_minimise_log = 0.0;
    int capture_minimise_frames = 0;
    double capture_minimise_clock = 0.0;
    int capture_board_step = 0; // --capture-board: 1 at the door, then the presses of Enter
    bool capture_passed = false; // --capture-passage done
    double capture_reported = 0.0; // --capture-report: the last pick
    int capture_path_switches = 0; // --capture-switch-path: switches made
    FrameLimiter limiter;

    ClockControls clock{
        .time_of_day = static_cast<double>(options.time_of_day),
        .speed = options.time_speed,
        .season = static_cast<int>(options.season),
        .weather = options.storm ? 3 : static_cast<int>(options.weather),
    };
    surface.set_vsync(options.vsync);
    // Each pass's GPU time (renderer::GpuPassTimer): the renderer's passes
    // ask it for their timestamps.
    const auto timer_queue = wgpuDeviceGetQueue(device);
    renderer::GpuPassTimer pass_timer(device, timer_queue);
    wgpuQueueRelease(timer_queue);
    renderer::gpu_pass_timer() = pass_timer.available() ? &pass_timer : nullptr;
    PanelState panel{
        .vsync = surface.vsync(), .vsync_optional = surface.vsync_optional(), .gpu_timing = pass_timer.available()};
    panel.menu = options.menu;
    panel.fps_limit = clamp_fps_limit(options.fps_limit);
    WindowModeKeeper window_mode;
    panel.window_mode_fixed = capture || options.menu;
    if (!panel.window_mode_fixed) {
        panel.window_mode = options.window_mode.value_or(WindowMode::windowed);
        if (panel.window_mode != WindowMode::windowed) panel.full_mode = panel.window_mode;
    }
    panel.hdr = renderer::scene_hdr();
    panel.backend_now = renderer::adapter_backend_name(context.context.adapter());
    panel.experimental_supported = experimental_supported;
    // Models made on loading threads; not on OpenGL (one context under a lock).
    renderer::models_made_off_thread() = experimental_supported;
    if (!panel.experimental_supported)
        std::cout << "OpenGL: the experimental renderer and the depth copy for ReShade are not available "
                     "(wgpu cannot read depth textures with textureLoad in GLSL); the RTS path only." << std::endl;
    if (const auto key = remembered("backend"))
        panel.backend_choice = static_cast<int>(renderer::parse_graphics_backend(*key).value_or(renderer::GraphicsBackend::automatic));
    // The graphics settings as the start menu keeps them, its defaults
    // (graphics_settings.hpp); the command line's options over them.
    panel.kept_graphics = load_graphics_settings();
    panel.msaa_asked = options.msaa;
    panel.fps_counter = remembered("fps_counter").value_or("0") == "1";
    panel.smooth_train_textures = remembered("smooth_train_textures").value_or("0") == "1";
    // Tests: OPENRTS_SMOOTH_TRAIN_TEXTURES turns it on for the run, not remembered.
    {
        char* forced = nullptr;
        std::size_t length = 0;
        if (_dupenv_s(&forced, &length, "OPENRTS_SMOOTH_TRAIN_TEXTURES") == 0 && forced && *forced == '1') panel.smooth_train_textures = true;
        std::free(forced);
    }
    renderer::texture_cache_directory() = user_data_directory() / "texture_cache" / "bc7";
    if (const auto kept = remembered("cab_near_plane")) panel.cab_near_plane = std::clamp(std::strtof(kept->c_str(), nullptr), 0.0f, 0.3f);
    set_panel_graphics(panel, panel.kept_graphics);
    if (options.reshade_depth) panel.reshade_depth = *options.reshade_depth;
    if (options.pbr) panel.pbr = *options.pbr;
    if (options.sky_light) renderer::sky_light_from_probe() = *options.sky_light;
    if (options.native_sky) panel.native_sky = *options.native_sky;
    renderer::set_forest_clearance_distance(panel.forest_clearance ? panel.forest_clearance_distance : 0.0f);
    if (options.light_range) panel.light_range = std::clamp(*options.light_range, 0.0f, 3000.0f);
    if (options.headlight_reach) panel.headlight_reach = std::clamp(*options.headlight_reach, 1.0f, 5.0f);
    if (options.lamps_daylight) panel.effects.lamps_in_daylight = std::clamp(*options.lamps_daylight, 0.0f, 1.0f);
    if (options.ground_bounce) panel.effects.ground_bounce = std::clamp(*options.ground_bounce, 0.0f, 0.5f);
    if (options.auto_exposure) panel.auto_exposure.on = *options.auto_exposure;
    if (options.material_view) panel.effects.material_view = std::clamp(*options.material_view, 0, 6);
    if (options.auto_exposure_strength) panel.auto_exposure.strength = std::clamp(*options.auto_exposure_strength, 0.0f, 1.0f);
    if (options.sky_light_strength) panel.effects.sky_light_strength = std::clamp(*options.sky_light_strength, 0.0f, 2.0f);
    if (options.sky_reflections) panel.effects.sky_reflection_strength = std::clamp(*options.sky_reflections, 0.0f, 2.0f);
    if (options.sky_reflections_indoors) panel.effects.sky_reflection_indoors = std::clamp(*options.sky_reflections_indoors, 0.0f, 1.0f);
    if (options.sky_light_tunnel) panel.effects.sky_light_tunnel = std::clamp(*options.sky_light_tunnel, 0.0f, 0.5f);
    constexpr int most_lamp_shadows = static_cast<int>(renderer::SceneEffects::max_lamp_shadows);
    if (options.lamp_shadows) panel.effects.lamp_shadows = static_cast<std::uint32_t>(std::clamp(*options.lamp_shadows, 0, most_lamp_shadows));
    if (const auto on = remembered("parallel_recording")) panel.parallel_recording = *on == "1";
    if (options.shadow_cache) panel.effects.cache_far_cascades = *options.shadow_cache;
    if (options.parallax) panel.effects.parallax = *options.parallax;
    if (options.compress_textures) panel.compress_textures = *options.compress_textures;
    if (options.texture_size) panel.texture_size = *options.texture_size;
    if (options.cab_shadows) panel.effects.cab_shadows = *options.cab_shadows;
    if (options.parallel_recording) panel.parallel_recording = *options.parallel_recording;
    if (options.start_paused) panel.paused = true;
    if (const auto on = remembered("multi_draw")) panel.multi_draw = *on == "1";
    if (options.multi_draw) panel.multi_draw = *options.multi_draw;
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
        panel.effects.light_shafts = has("shafts");
    }
    // The field of view: the panel's, else RTS's (config.ini FOV, vertical, degrees).
    if (const auto fov = remembered("fov")) {
        panel.fov = std::clamp(static_cast<float>(std::atof(fov->c_str())), 15.0f, 100.0f);
    } else if (const auto rts = rts_config_value(read_rts_config(options.identity.root), "fov")) {
        panel.fov = std::clamp(*rts, 15.0f, 100.0f);
    }
    StageClock stage_clock;
    StageTimes stages{};
    FovKeys fov_keys;
    double last_time = glfwGetTime();
    // Every 10 s the frame's breakdown goes to the console (openrts.log when
    // the menu runs the game): what a frame waits on, without the panel.
    double perf_at = last_time + 10.0;
    DiscordStatus discord(options.discord && !capture);
    while (!glfwWindowShouldClose(window.get())) {
        // The start's loading (as the last update left it): its screen, no
        // input for the scene, and for a start in the cab no time passing.
        const auto loading = scene.loading();
        // The FPS limit: the wait comes before the input is read. None while
        // the start loads: the world streams in a step a frame.
        // In the background (another window in front, or minimised): at most
        // 30 frames/s; in front again, the setting.
        const bool background = !options.fps_background
                                && (glfwGetWindowAttrib(window.get(), GLFW_FOCUSED) == GLFW_FALSE
                                    || glfwGetWindowAttrib(window.get(), GLFW_ICONIFIED) == GLFW_TRUE);
        limiter.set(frame_limit(panel.fps_limit, !loading.done, background));
        limiter.wait();
        stage_clock.start();
        glfwPollEvents();
        input.skip_motion = false;
        const double now = glfwGetTime();
        if (!panel.window_mode_fixed) {
            // Alt+Enter: the window, or the full screen kind chosen last.
            const auto down = [&window](const int key) { return glfwGetKey(window.get(), key) == GLFW_PRESS; };
            const bool switch_keys = (down(GLFW_KEY_LEFT_ALT) || down(GLFW_KEY_RIGHT_ALT)) && (down(GLFW_KEY_ENTER) || down(GLFW_KEY_KP_ENTER));
            if (switch_keys && !input.window_mode_key_down) {
                panel.window_mode = panel.window_mode == WindowMode::windowed ? panel.full_mode : WindowMode::windowed;
                remember("window_mode", window_mode_key(panel.window_mode));
            }
            input.window_mode_key_down = switch_keys;
            window_mode.apply(window.get(), panel.window_mode);
        }
        // The place and size in a window only (not the monitor's when full).
        if (placement_keeper && window_mode.applied() == WindowMode::windowed) placement_keeper->update(window.get(), now);
        panel.stats.add(now - last_time, stages, pass_timer.take());
        stages = {};
        const auto elapsed = static_cast<float>((std::min)(now - last_time, max_frame_seconds));
        input.keyboard_captured = scene.keyboard_captured();
        input.on_foot = scene.engineer_on_foot();
        if (scene.wants_close()) glfwSetWindowShouldClose(window.get(), GLFW_TRUE);
        input.scene_takes_scroll = scene.takes_scroll();
        update_camera_from_keyboard(window.get(), input, elapsed);
#ifdef OPENRTS_HAS_IMGUI
        const bool fov_keyboard_free = !ImGui::GetIO().WantCaptureKeyboard;
#else
        const bool fov_keyboard_free = true;
#endif
        if (update_fov_keys(window.get(), fov_keys, panel.fov, elapsed, fov_keyboard_free)) remember("fov", std::to_string(panel.fov));
        last_time = now;

        toggle_shading_from_keyboard(window.get(), input, renderer);
        if (!panel.experimental_supported) renderer.set_shading_path(renderer::ShadingPath::rts_compatible);
        {
            // Pause: trains, DLLs, the clock and the sound stop; the free camera still flies.
            // Pause, as RTS; F1 too (openRTS's), for keyboards without Pause
            // (laptops, testers 2026-11-18): no cab of the installed content
            // assigns an F key, and RTS's F1 works only with Shift
            // (0x402C88).
            const bool shift = glfwGetKey(window.get(), GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS
                               || glfwGetKey(window.get(), GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
            const bool pause_key = !imgui_has_keyboard()
                                   && (glfwGetKey(window.get(), GLFW_KEY_PAUSE) == GLFW_PRESS
                                       || (glfwGetKey(window.get(), GLFW_KEY_F1) == GLFW_PRESS && !shift));
            if (pause_key && !input.pause_key_down) panel.paused = !panel.paused;
            input.pause_key_down = pause_key;
        }
        if (sound) {
            sound->set_paused(panel.paused || !loading.done);
            // Not silent in the background unless so chosen (key_layout.hpp).
            sound->set_background_gain(background ? background_sound_gain(background_sound()) : 1.0f);
        }
        renderer::physical_light_falloff() = panel.physical_light;
        renderer::pbr_shading() = panel.pbr;
        renderer::native_water() = panel.native_water;
        renderer::native_sky() = panel.native_sky;
        renderer::signal_lights_real_size() = panel.signal_lights_real_size;
        renderer::cab_screens_supersampled() = panel.screen_supersampling;
        renderer::cloud_cover() = panel.cloud_cover_weather ? -1.0f : panel.cloud_cover / 100.0f;
        renderer::cloud_shadows() = panel.cloud_shadows;
        renderer::cloud_shadow_darkness() = panel.cloud_shadow_darkness / 100.0f;
        renderer::cloud_shadow_sharpness() = panel.cloud_shadow_sharpness;
        renderer::night_ambient_scale() = panel.night_ambient;
        renderer::light_range() = panel.light_range;
        renderer::headlight_reach() = panel.headlight_reach;
        renderer::light_fullbright() = panel.light_fullbright;
        renderer::night_textures() = panel.night_textures;
        renderer::smooth_train_textures() = panel.smooth_train_textures;
        renderer::texture_compression() = static_cast<renderer::TextureCompression>(std::clamp(panel.compress_textures, 0, 2));
        {
            constexpr std::uint32_t sides[] = {0, 1024, 512, 256};
            renderer::texture_size_limit() = sides[std::clamp(panel.texture_size, 0, 3)];
        }
        renderer::lamp_skips_own_model() = panel.lamp_own_model;
        renderer::set_forest_clearance_distance(panel.forest_clearance ? panel.forest_clearance_distance : 0.0f);
        auto frame_input = scene_input(window.get(), input);
        if (frame_input.clock) panel.show_clock = !panel.show_clock;
        if (!loading.done) {
            // The keys were read (their edges stay right) and are dropped;
            // the camera stays where the scene put it.
            frame_input = SceneInput{};
            camera = loading_camera;
            capture_start = glfwGetTime();
        }
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
        if (capture && options.capture_inspect && capture_inspect_step < 2 && glfwGetTime() - capture_start >= options.capture_at * 0.5) {
            if (capture_inspect_step == 0) {
                frame_input.inspector = true;
            } else {
                frame_input.pointer = true;
                frame_input.pointer_x = 0.0f;
                frame_input.pointer_y = 0.0f;
                frame_input.click = true;
            }
            ++capture_inspect_step;
        }
        if (capture && options.capture_uncouple && !capture_uncoupled && glfwGetTime() - capture_start >= options.capture_at * 0.6) {
            scene.uncouple_driven(*options.capture_uncouple);
            capture_uncoupled = true;
        }
        if (capture && options.capture_couple && !capture_coupled && glfwGetTime() - capture_start >= options.capture_at * 0.6) {
            scene.couple_driven(*options.capture_couple);
            capture_coupled = true;
        }
        if (capture && options.capture_passage && !capture_passed && glfwGetTime() - capture_start >= options.capture_at * 0.3) {
            scene.stand_at_passage(*options.capture_passage);
            capture_passed = true;
        }
        // Then a walk through the door into the next car (0.6 .. 0.75), and
        // back to look into the room left (the capture).
        if (capture && options.capture_passage && capture_passed) {
            const double since = glfwGetTime() - capture_start;
            if (since >= options.capture_at * 0.6 && since < options.capture_at * 0.75) frame_input.walk_forward = true;
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
        if (capture && options.capture_ok) scene.close_activity_message();
        if (capture && options.capture_report && glfwGetTime() - capture_start >= options.capture_at * 0.5
            && glfwGetTime() - capture_reported >= 2.0) {
            capture_reported = glfwGetTime();
            scene.report_activity();
        }
        if (capture && options.capture_tape && !capture_taped && glfwGetTime() - capture_start >= options.capture_at * 0.2) {
            frame_input.tape = true;
            frame_input.tape_window = true;
            capture_taped = true;
        }
        if (capture && options.capture_minimise) {
            const double since = glfwGetTime() - capture_start;
            if (capture_minimise_step == 0 && since >= options.capture_at * 0.3) {
                glfwIconifyWindow(window.get());
                capture_minimise_step = 1;
                capture_minimise_log = glfwGetTime();
                capture_minimise_clock = clock.time_of_day;
                std::cout << "minimise: the window minimised" << std::endl;
            } else if (capture_minimise_step == 1 && since >= options.capture_at * 0.7) {
                glfwRestoreWindow(window.get());
                capture_minimise_step = 2;
                std::cout << "minimise: the window back" << std::endl;
            }
            if (capture_minimise_step == 1) {
                ++capture_minimise_frames;
                if (glfwGetTime() - capture_minimise_log >= 1.0) {
                    std::cout << "minimise: iconified " << glfwGetWindowAttrib(window.get(), GLFW_ICONIFIED) << ", "
                              << capture_minimise_frames << " frames in " << glfwGetTime() - capture_minimise_log << " s, the clock +"
                              << clock.time_of_day - capture_minimise_clock << " s, sound "
                              << (sound ? std::to_string(sound->active_voices()) + " voices, " + std::to_string(sound->audible_voices())
                                              + " heard, output at frame " + std::to_string(sound->played_frames())
                                              + (sound->paused() ? ", paused" : "")
                                        : std::string("off"))
                              << std::endl;
                    capture_minimise_log = glfwGetTime();
                    capture_minimise_clock = clock.time_of_day;
                    capture_minimise_frames = 0;
                }
            }
        }
        if (capture && options.capture_boarding && !capture_boarded && glfwGetTime() - capture_start >= options.capture_at * 0.4) {
            frame_input.board = true;
            capture_boarded = true;
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
        if (capture && options.capture_switch_path && panel.experimental_supported) {
            const double since = glfwGetTime() - capture_start;
            const int wanted = since >= options.capture_at * (2.0 / 3.0) ? 2 : since >= options.capture_at / 3.0 ? 1 : 0;
            if (wanted > capture_path_switches) {
                capture_path_switches = wanted;
                renderer.set_shading_path(renderer.shading_path() == renderer::ShadingPath::rts_compatible ? renderer::ShadingPath::experimental
                                                                                                          : renderer::ShadingPath::rts_compatible);
                std::cout << "capture: the rendering path switched to "
                          << (renderer.shading_path() == renderer::ShadingPath::experimental ? "experimental" : "RTS") << std::endl;
            }
        }
        if (const auto time = scene.take_time_of_day()) clock.time_of_day = *time;
        auto scene_clock = clock.advance(panel.paused || scene.paused() || loading.hold_time ? 0.0f : elapsed);
        scene_clock.background = background;
        camera.head_nod = camera.head_tilt = 0.0f;
        // The frame's encoding (renderer::frame_encoding()), before any pass
        // writes its uniforms: linear light on the experimental path's HDR
        // frame, gamma 2.2 on the RTS path's and without HDR.
        renderer::frame_encoding() = tone && surface.hdr_view() && renderer.shading_path() == renderer::ShadingPath::experimental
                                             && renderer::linear_hdr()
                                         ? renderer::FrameEncoding::linear
                                         : renderer::FrameEncoding::gamma;
        scene.update(renderer, camera, scene_clock, frame_input);
        discord.update(scene);
        if (!scene.loading().done) loading_camera = camera;
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
        // --fly-test: the free camera flies on level at a speed and turns (a
        // moving view without a driver: the shadow cache's check, benchmarks).
        if (options.fly_speed > 0.0f && scene.loading().done) {
            camera.fly = true;
            const auto ahead = camera.forward();
            const auto level = glm::length(glm::vec2{ahead.x, ahead.z}) > 1e-4f ? glm::normalize(glm::vec3{ahead.x, 0.0f, ahead.z})
                                                                               : glm::vec3{0.0f, 0.0f, -1.0f};
            camera.position += level * options.fly_speed * elapsed;
            camera.yaw += glm::radians(options.fly_turn) * elapsed;
        }
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
        pass_timer.begin_frame();
        const auto frame_view = wgpuTextureCreateView(frame_texture, nullptr);
        const auto encoder = wgpuDeviceCreateCommandEncoder(device, nullptr);
        const bool hdr = tone && surface.hdr_view();

        const auto aspect = static_cast<float>(width) / static_cast<float>(height);
        seated_cab_near = panel.cab_near_plane > 0.0f && scene.seated_in_cab() ? panel.cab_near_plane : 0.0f;
        FrameView frame{
            .eye = camera.eye(),
            .view = camera.view(),
            .projection = projection(camera, aspect, camera.vertical_fov(glm::radians(panel.fov))),
            .vertical_fov = camera.vertical_fov(glm::radians(panel.fov)),
            .view_zoom = camera.view_zoom,
            .aspect = aspect,
            .viewport_height = static_cast<float>(height),
            .viewport_width = static_cast<float>(width),
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
                             .near_plane = near_plane_of(camera),
                             .far_plane = camera.far_plane(),
                             .width = static_cast<std::uint32_t>(width),
                             .height = static_cast<std::uint32_t>(height)},
                            environment.lighting, experimental, environment.cloud_shadow, [&] {
                                 // The sky's light, made last frame (PBR only).
                                 renderer::SkyLightView sky{};
                                 if (renderer::frame_sky_light()) {
                                     sky = sky_light.view();
                                     sky.openness = environment.sky_openness;
                                 }
                                 return sky;
                             }());
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
        effects.set_focus(scene.shadow_focus(), scene.lamp_shadow_focus(camera));
        stage_clock.mark(FrameStage::cull, stages);
        renderer.prepare();
        stage_clock.mark(FrameStage::prepare, stages);
        const renderer::SceneEffects::DepthDraw depth_draw = [&](const WGPURenderPassEncoder depth_pass, const std::uint32_t slot,
                                                                  const glm::mat4& view_projection, const renderer::DepthPassKind kind,
                                                                  const renderer::DepthCasters casters) {
            // The terrain stays where it is.
            if (casters != renderer::DepthCasters::moving) scene.draw_depth(depth_pass, slot, view_projection, kind);
            renderer.render_depth(depth_pass, slot, view_projection, kind,
                                  renderer::lamp_skips_own_model() ? effects.depth_slot_owner(slot) : 0u, casters);
        };
        scene.prepare_depth();
        // Nothing moving moved: the shadow maps may stay as they are.
        effects.set_moving_signature(renderer.moving_signature(), renderer.moving_signature(false));
        // In parallel the workers record and finish a command buffer each
        // (the cascades, the pre-pass) while this thread records the main pass.
        renderer.set_multi_draw(panel.multi_draw);
        const bool parallel = panel.parallel_recording && experimental;
        if (parallel) {
            effects.record_parallel(device, recording_pool, depth_draw);
        } else {
            effects.record(encoder, depth_draw);
        }
        effects.record_check(encoder);
        scene.before_main_pass(encoder, frame);
        stage_clock.mark(FrameStage::effects, stages);
        const auto pass =
            begin_main_pass(encoder,
                            {.color = hdr ? surface.hdr_view() : frame_view,
                             .depth = surface.depth_view(),
                             .msaa = surface.msaa_view(),
                             .keep_depth = reshade_depth},
                            renderer::frame_linear()
                                ? glm::vec4{glm::pow((glm::max)(glm::vec3{environment.clear_colour}, glm::vec3{0.0f}), glm::vec3{2.2f}),
                                            environment.clear_colour.a}
                                : environment.clear_colour,
                            renderer::render_pass_timestamps(renderer::GpuPass::main), scene.occlusion_query_set());
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
        stage_clock.mark(FrameStage::end, stages);
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
            renderer::ToneSettings settings{
                .curve = renderer.shading_path() == renderer::ShadingPath::experimental ? static_cast<renderer::ToneCurve>(panel.tone_curve)
                                                                                         : renderer::ToneCurve::none,
                .exposure = panel.exposure,
                .night = panel.night,
                .night_exposure = panel.night_exposure * panel.exposure,
                .saturation = panel.saturation,
                .linear_scene = renderer::frame_linear(),
            };
            // The bloom (the experimental path): the scene's lights drawn once
            // more into its source against the camera's depth pre-pass, then
            // taken down and up; the tone curve adds it.
            WGPUTextureView bloom_view = nullptr;
            if (experimental && settings.curve != renderer::ToneCurve::none && panel.effects.bloom && panel.effects.bloom_strength > 0.0f) {
                if (!bloom) bloom = std::make_unique<renderer::BloomRenderer>(device);
                if (bloom->ready()) {
                    const auto w = static_cast<std::uint32_t>(width), h = static_cast<std::uint32_t>(height);
                    if (const auto depth = effects.camera_depth()) {
                        if (const auto source = bloom->begin_sources(encoder, w, h, depth)) {
                            scene.draw_bloom_sources(source, frame);
                            wgpuRenderPassEncoderEnd(source);
                            wgpuRenderPassEncoderRelease(source);
                        }
                    }
                    bloom_view = bloom->build(encoder, surface.hdr_view(), w, h,
                                              {.textures = panel.effects.bloom_textures, .night = panel.night, .linear = renderer::frame_linear()});
                    settings.bloom = 0.15f * panel.effects.bloom_strength;
                }
            }
            // The light shafts (the experimental path): the sun's beams
            // marched through the effects' shadows to the camera's depth.
            WGPUTextureView shafts_view = nullptr;
            if (experimental && settings.curve != renderer::ToneCurve::none && panel.effects.light_shafts
                && panel.effects.light_shafts_strength > 0.0f) {
                if (!shafts) shafts = std::make_unique<renderer::LightShaftRenderer>(device);
                if (const auto depth = effects.camera_depth(); depth && shafts->ready()) {
                    shafts_view = shafts->render(encoder, effects.bind_group(), depth,
                                                 {.view = frame.view,
                                                  .projection = frame.projection,
                                                  .eye = frame.eye,
                                                  .width = static_cast<std::uint32_t>(width),
                                                  .height = static_cast<std::uint32_t>(height),
                                                  .strength = panel.effects.light_shafts_strength,
                                                  .shadow_distance = panel.effects.shadow_distance,
                                                  .fog_end = environment.lighting.fog_end,
                                                  .room = scene.light_shaft_room()});
                    settings.shafts = shafts_view != nullptr;
                }
            }
            // The auto exposure (PBR stage 3): the frame's light measured on
            // the GPU, the tone curve reads its factor from there.
            WGPUBuffer exposure_state = nullptr;
            if (experimental && settings.curve != renderer::ToneCurve::none && panel.auto_exposure.on) {
                if (!auto_exposure) auto_exposure = std::make_unique<renderer::AutoExposure>(device);
                if (auto_exposure->ready()) {
                    // From the frame's light at once: turned on, or still loading
                    // (no adapting from the loading screen's dark).
                    if (!auto_exposure_was_on || !loading.done) auto_exposure->reset();
                    auto_exposure->measure(encoder, surface.hdr_view(), static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height),
                                           renderer::frame_linear(), elapsed, panel.auto_exposure);
                    exposure_state = auto_exposure->state();
                    settings.auto_exposure = true;
                    auto_exposure->copy_for_panel(encoder);
                    panel.auto_exposure_factor = auto_exposure->latest().valid ? auto_exposure->latest().factor : 1.0f;
                }
            }
            auto_exposure_was_on = exposure_state != nullptr;
            tone->render(encoder, surface.hdr_view(), frame_view, settings, bloom_view, shafts_view, exposure_state);
#ifdef OPENRTS_HAS_IMGUI
            const WGPURenderPassColorAttachment ui_colour{
                .view = frame_view,
                .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
                .loadOp = WGPULoadOp_Load,
                .storeOp = WGPUStoreOp_Store,
            };
            const auto ui_timestamps = renderer::render_pass_timestamps(renderer::GpuPass::overlay);
            const WGPURenderPassDescriptor ui_descriptor{
                .colorAttachmentCount = 1, .colorAttachments = &ui_colour, .timestampWrites = ui_timestamps ? &*ui_timestamps : nullptr};
            const auto ui = wgpuCommandEncoderBeginRenderPass(encoder, &ui_descriptor);
            if (!capture) overlay.render(ui);
            wgpuRenderPassEncoderEnd(ui);
            wgpuRenderPassEncoderRelease(ui);
#endif
        }
        scene.after_main_pass(encoder);
        // The sky's light from the probe the scene has just drawn (PBR stage
        // 2), for the next frame: the sun (linear) and the ground's
        // reflectance light the ground under the horizon.
        if (renderer::frame_sky_light() && environment.sky_probe) {
            const glm::vec3 sun = glm::pow((glm::max)(glm::vec3{environment.lighting.sun_diffuse}, glm::vec3{0.0f}), glm::vec3{2.2f});
            sky_light.update(encoder, environment.sky_probe,
                             {.to_sun = -glm::normalize(environment.lighting.sun_direction),
                              .sun = sun,
                              .ground_albedo = std::clamp(panel.effects.ground_bounce, 0.0f, 0.5f)});
        }

        FrameCapture shot;
        FrameCapture depth_shot;
        if (capture_now) shot = begin_capture(device, encoder, frame_texture, static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
        if (capture_now && depth_copy && depth_export->texture())
            depth_shot = begin_capture(device, encoder, depth_export->texture(), static_cast<std::uint32_t>(width),
                                       static_cast<std::uint32_t>(height));
        FrameCapture hdr_shot;
        const bool hdr_linear = renderer::frame_linear();
        if (capture_now && hdr && surface.hdr_texture())
            hdr_shot = begin_capture(device, encoder, surface.hdr_texture(), static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), 8);
        FrameCapture shafts_shot;
        if (capture_now && shafts && shafts->texture() && panel.effects.light_shafts)
            shafts_shot = begin_capture(device, encoder, shafts->texture(), static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), 8);
        stage_clock.mark(FrameStage::post, stages);
        const auto commands = wgpuCommandEncoderFinish(encoder, nullptr);
        stage_clock.mark(FrameStage::encode, stages);
        // The workers' command buffers first: the shadows and the occlusion
        // the main pass reads.
        auto submitted = parallel ? effects.finish_parallel(recording_pool) : std::vector<WGPUCommandBuffer>{};
        stage_clock.mark(FrameStage::join, stages);
        submitted.push_back(commands);
        // The passes' timestamps resolved last, the workers' passes with them.
        if (const auto timing = pass_timer.finish()) submitted.push_back(timing);
        const auto queue = wgpuDeviceGetQueue(device);
        wgpuQueueSubmit(queue, submitted.size(), submitted.data());
        if (const auto report = effects.shadow_cache_check_report(context.context.instance()); !report.empty()) std::cout << report << std::flush;
        for (const auto buffer : submitted)
            if (buffer != commands) wgpuCommandBufferRelease(buffer);
        pass_timer.submitted();
        if (auto_exposure) auto_exposure->submitted();
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
            std::cout << renderer.draw_report(25) << std::flush;
            for (const auto& [id, asked] : options.capture_switches) {
                if (const auto where = scene.cab_switch_state(id)) {
                    std::cout << "cab switch " << id << ": asked " << asked << ", State " << where->first << ", SetState " << where->second << std::endl;
                }
            }
            const auto model_stats = renderer.stats();
            std::cout << "models: " << model_stats.instances << " instances, " << model_stats.draw_calls << " draw calls" << std::endl;
            if (const auto world = scene.world_report(); !world.empty()) std::cout << "world: " << world << std::endl;
            if (options.capture_menu) std::cout << "car menu: " << scene.car_menu_report() << std::endl;
            if (options.capture_inspect) std::cout << "inspector: " << scene.inspector_report() << std::endl;
            if (options.capture_tape) std::cout << "tape window: " << scene.tape_report() << std::endl;
            if (options.capture_boarding) std::cout << "timetable: " << scene.timetable_report() << std::endl;
            const auto effect_stats = effects.stats();
            std::cout << "effects: " << effect_stats.cascades << " cascades (" << effect_stats.cascades_drawn << " drawn, nearest texel "
                      << effect_stats.near_texel * 1000.0f << " mm), cab map " << (effect_stats.focus ? "on" : "off");
            if (effect_stats.focus) std::cout << " (texel " << effect_stats.focus_texel * 1000.0f << " mm)";
            std::cout << ", daylight " << effect_stats.daylight << ", lamps x" << effect_stats.lamp_strength << ", " << effect_stats.lamp_shadows
                      << " lamp shadows (of " << effect_stats.lamps_lighting << " lamps lighting " << effect_stats.lamp_radius << " m around)";
            std::cout << ", " << effect_stats.lights << " lights; workers' passes (ms):";
            for (std::uint32_t i = 0; i < effect_stats.cascades; ++i) std::cout << " cascade" << i << ' ' << effect_stats.record_cascade_ms[i];
            std::cout << " focus " << effect_stats.record_focus_ms << " lamps " << effect_stats.record_lamps_ms << " camera "
                      << effect_stats.record_camera_ms << std::endl;
            std::cout << "cpu stages (ms):";
            for (std::size_t i = 0; i < frame_stage_count; ++i) std::cout << ' ' << frame_stage_names[i] << ' ' << panel.stats.stage_ms()[i];
            std::cout << "; gpu main pass " << panel.stats.gpu_ms() << " ms" << std::endl;
            std::cout << "gpu frame " << panel.stats.gpu_frame_ms() << " ms: " << panel.stats.gpu_report() << std::endl;
            {
                const auto gpu = renderer.stats();
                if (video_memory_probe) {
                    const auto memory = video_memory_probe->query();
                    if (memory.known)
                        std::cout << "video memory: " << memory.usage / 1048576 << " of " << memory.budget / 1048576 << " MB budget, shared "
                                  << memory.shared_usage / 1048576 << " MB" << std::endl;
                }
                std::cout << "textures on the GPU: " << gpu.cached_textures << ", " << gpu.texture_bytes / 1048576 << " MB (uncompressed "
                          << gpu.rgba8_texture_bytes / 1048576 << " MB), the trains' "
                          << gpu.train_texture_bytes / 1048576 << " MB; model geometry " << gpu.geometry_bytes / 1048576
                          << " MB, the trains' " << gpu.train_geometry_bytes / 1048576 << " MB; all uploaded textures "
                          << renderer::uploaded_texture_bytes() / 1048576 << " MB; terrain tiles' buffers "
                          << renderer::terrain_tile_buffer_bytes() / 1048576 << " MB" << std::endl;
            }
            if (depth_shot.buffer) {
                const auto near_plane = near_plane_of(camera);
                report_reshade_depth(context.context, depth_shot, near_plane, camera.far_plane());
            }
            if (shafts_shot.buffer) report_light_shafts(context.context, shafts_shot);
            if (hdr_shot.buffer) report_hdr_frame(context.context, hdr_shot, hdr_linear);
            if (auto_exposure && auto_exposure_was_on) {
                const auto state = auto_exposure->read_state(context.context.instance());
                std::cout << "auto exposure: measured log2 " << state.measured << ", adapted " << state.adapted << ", factor " << state.factor
                          << (state.valid ? "" : " (not measured yet)") << std::endl;
            }
            if (renderer::frame_sky_light())
                std::cout << "sky light: " << (sky_light.view().on() ? "from the probe" : "not made yet") << ", the sky open round the eye "
                          << environment.sky_openness << std::endl;
            wgpuTextureRelease(frame_texture);
            return written ? exit_code::success : exit_code::device_failed;
        }
        surface.present();
        wgpuTextureRelease(frame_texture);
        renderer.collect_retired();
        stage_clock.mark(FrameStage::submit, stages);
        OPENRTS_FRAME();
        {
            const auto models = renderer.stats();
            OPENRTS_PLOT("draw calls", static_cast<std::int64_t>(models.draw_calls));
            OPENRTS_PLOT("draw commands", static_cast<std::int64_t>(models.draw_commands));
            OPENRTS_PLOT("model instances", static_cast<std::int64_t>(models.instances));
        }
        if (video_memory_probe && now >= video_memory_at) {
            video_memory_at = now + 0.5;
            panel.video_memory = video_memory_probe->query();
        }
        if (!capture && !options.menu && now >= perf_at) {
            perf_at = now + 10.0;
            if (const auto& memory = panel.video_memory; memory.known) {
                std::cout << "perf memory: " << memory.usage / 1048576 << " of " << memory.budget / 1048576 << " MB budget (card "
                          << memory.total / 1048576 << " MB), shared " << memory.shared_usage / 1048576 << " MB"
                          << (memory.over_budget() ? " OVER BUDGET" : "") << "; textures " << renderer.stats().texture_bytes / 1048576
                          << " MB, uncompressed " << renderer.stats().rgba8_texture_bytes / 1048576 << " MB" << std::endl;
            }
            const auto& times = panel.stats.stage_ms();
            double work = 0.0;
            for (std::size_t i = 0; i < frame_stage_count; ++i)
                if (static_cast<FrameStage>(i) != FrameStage::wait) work += times[i];
            const auto gpu = renderer.stats();
            const auto effect_stats = effects.stats();
            std::cout << "perf: " << static_cast<int>(panel.stats.fps() + 0.5) << " fps, " << panel.stats.average_ms() << " ms avg, "
                      << panel.stats.worst_ms() << " ms worst; cpu work " << work << " ms:";
            for (std::size_t i = 0; i < frame_stage_count; ++i) std::cout << ' ' << frame_stage_names[i] << ' ' << times[i];
            std::cout << "; gpu main pass " << (panel.gpu_timing ? panel.stats.gpu_ms() : -1.0) << " ms; " << gpu.instances
                      << " instances, " << gpu.draw_calls << " draw calls in " << gpu.draw_commands << " commands (multi-draw "
                      << (renderer.multi_draw() ? "on" : "off") << "), " << gpu.primitives << " primitives; "
                      << effect_stats.cascades << " cascades (" << effect_stats.cascades_drawn << " drawn), "
                      << effect_stats.lamp_shadows << " lamp shadows (" << effect_stats.lamp_shadows_drawn << " drawn), " << effect_stats.lights
                      << " lights; workers' passes (ms):";
            for (std::uint32_t i = 0; i < effect_stats.cascades; ++i) std::cout << " cascade" << i << ' ' << effect_stats.record_cascade_ms[i];
            std::cout << " focus " << effect_stats.record_focus_ms << " lamps " << effect_stats.record_lamps_ms << " camera "
                      << effect_stats.record_camera_ms;
            if (effect_stats.still_cache)
                std::cout << ", shadow cache " << effect_stats.still_full << " whole " << effect_stats.still_pieces << " pieces "
                          << effect_stats.still_share * 100.0f << " % " << effect_stats.maps_kept << " kept";
            std::cout << "; renderer "
                      << (renderer.shading_path() == renderer::ShadingPath::experimental ? "experimental" : "RTS") << std::endl;
            if (panel.gpu_timing)
                std::cout << "perf gpu: frame " << panel.stats.gpu_frame_ms() << " ms; " << panel.stats.gpu_report() << std::endl;
            if (auto_exposure && auto_exposure_was_on && auto_exposure->latest().valid) {
                const auto state = auto_exposure->latest();
                std::cout << "perf exposure: measured " << state.measured << " adapted " << state.adapted << " (key " << renderer::auto_exposure_key
                          << ", log2 luminance), factor x" << state.factor << std::endl;
            }
        }
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
