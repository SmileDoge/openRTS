#include "window_viewer.hpp"

#ifdef OPENRTS_HAS_WGPU
#include <openrts/renderer/depth.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <ctime>
#include <iostream>
#include <string>
#include <vector>

#include <glm/ext/matrix_clip_space.hpp>

#include <openrts/renderer/route_space.hpp>
#include <openrts/renderer/webgpu_context.hpp>

#include "gpu_timer.hpp"

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
constexpr float vertical_fov = 0.85f;
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
    PresentationSurface(const renderer::WgpuDeviceContext& context, GLFWwindow* window) : device_(context.device()) {
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
                .usage = WGPUTextureUsage_RenderAttachment,
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

    // Returns the frame texture, or nullptr when the surface has nothing to draw into.
    [[nodiscard]] WGPUTexture acquire() const {
        WGPUSurfaceTexture frame{};
        wgpuSurfaceGetCurrentTexture(surface_, &frame);
        const bool usable = frame.status == WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal
            || frame.status == WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal;
        return usable ? frame.texture : nullptr;
    }

    void present() const { wgpuSurfacePresent(surface_); }

private:
    void recreate_depth() {
        release_depth();
        const auto samples = renderer::render_sample_count();
        const WGPUTextureDescriptor descriptor{
            .usage = WGPUTextureUsage_RenderAttachment,
            .dimension = WGPUTextureDimension_2D,
            .size = {.width = config_.width, .height = config_.height, .depthOrArrayLayers = 1},
            .format = renderer::depth_format,
            .mipLevelCount = 1,
            .sampleCount = samples,
        };
        depth_ = wgpuDeviceCreateTexture(device_, &descriptor);
        if (depth_) depth_view_ = wgpuTextureCreateView(depth_, nullptr);
        if (samples > 1) {
            const WGPUTextureDescriptor colour{
                .usage = WGPUTextureUsage_RenderAttachment,
                .dimension = WGPUTextureDimension_2D,
                .size = {.width = config_.width, .height = config_.height, .depthOrArrayLayers = 1},
                .format = config_.format,
                .mipLevelCount = 1,
                .sampleCount = samples,
            };
            msaa_ = wgpuDeviceCreateTexture(device_, &colour);
            if (msaa_) msaa_view_ = wgpuTextureCreateView(msaa_, nullptr);
        }
    }

    void release_depth() {
        if (depth_view_) wgpuTextureViewRelease(depth_view_);
        if (depth_) wgpuTextureRelease(depth_);
        if (msaa_view_) wgpuTextureViewRelease(msaa_view_);
        if (msaa_) wgpuTextureRelease(msaa_);
        depth_view_ = nullptr;
        depth_ = nullptr;
        msaa_view_ = nullptr;
        msaa_ = nullptr;
    }

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
enum class FrameStage { update, panel, wait, begin, background, cull, models, foreground, finish, submit, count };
constexpr std::size_t frame_stage_count = static_cast<std::size_t>(FrameStage::count);
constexpr std::array<const char*, frame_stage_count> frame_stage_names{
    "update",  // input, camera, scene update (streaming, track, later simulation)
    "panel",   // ImGui panel
    "wait",    // waiting for the swap chain image (vsync and GPU back-pressure)
    "begin",   // frame view, encoder and main pass
    "backgr",  // sky and terrain
    "cull",    // scene culling and model submission
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
    bool gpu_timing = false;
    audio::AudioEngine* sound = nullptr;
    bool save_view = false;
    std::string save_status;
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
    DebugOverlay(GLFWwindow* window, const WGPUDevice device, const WGPUTextureFormat target_format) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        load_font();
        // Callbacks are forwarded by the viewer's own GLFW handlers.
        ImGui_ImplGlfw_InitForOther(window, false);

        ImGui_ImplWGPU_InitInfo info{};
        info.Device = device;
        info.NumFramesInFlight = 3;
        info.RenderTargetFormat = target_format;
        info.DepthStencilFormat = renderer::depth_format;
        info.PipelineMultisampleState.count = renderer::render_sample_count();
        initialized_ = ImGui_ImplWGPU_Init(&info);
    }

    ~DebugOverlay() {
        if (initialized_) ImGui_ImplWGPU_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
    }

    DebugOverlay(const DebugOverlay&) = delete;
    DebugOverlay& operator=(const DebugOverlay&) = delete;

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

        ImGui::Begin("openRTS viewer");
        ImGui::Text("%.*s", static_cast<int>(scene.title().size()), scene.title().data());
        ImGui::Text("FPS %.0f  |  %.2f ms avg, %.2f ms worst", panel.stats.fps(), panel.stats.average_ms(),
                    panel.stats.worst_ms());
        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        if (ImGui::TreeNode("Frame time")) {
            const auto& stages = panel.stats.stage_ms();
            double cpu = 0.0;
            for (std::size_t i = 0; i < frame_stage_count; ++i) {
                // "wait" is idle time, not work.
                if (static_cast<FrameStage>(i) != FrameStage::wait) cpu += stages[i];
                ImGui::Text("%-7s %6.2f ms", frame_stage_names[i], stages[i]);
            }
            ImGui::Text("CPU work %5.2f ms (without wait)", cpu);
            if (panel.gpu_timing) {
                ImGui::Text("GPU pass %5.2f ms", panel.stats.gpu_ms());
            } else {
                ImGui::TextDisabled("GPU pass: no timestamp queries on this device");
            }
            ImGui::TreePop();
        }
        ImGui::BeginDisabled(!panel.vsync_optional);
        ImGui::Checkbox("VSync", &panel.vsync);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", panel.present_mode);
        ImGui::Text("Camera: %s", camera.fly ? "fly" : "orbit");
        scene.draw_overlay(camera);
        ImGui::Separator();
        auto path = renderer.shading_path();
        ImGui::TextUnformatted("Shading (R):");
        ImGui::SameLine();
        if (ImGui::RadioButton("RTS", path == renderer::ShadingPath::rts_compatible)) {
            path = renderer::ShadingPath::rts_compatible;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Experimental", path == renderer::ShadingPath::experimental)) {
            path = renderer::ShadingPath::experimental;
        }
        renderer.set_shading_path(path);
        const auto gpu = renderer.stats();
        ImGui::Text("GPU: %zu textures, %zu retired waiting, frame %llu done", gpu.cached_textures, gpu.retired_pending,
                    static_cast<unsigned long long>(gpu.completed_frame));
        ImGui::Text("Models: %zu instances, %zu primitives, %zu draw calls", gpu.instances, gpu.primitives,
                    gpu.draw_calls);
        if (panel.sound) {
            float volume = panel.sound->volume();
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::SliderFloat("Sound volume", &volume, 0.0f, 1.0f, "%.2f")) panel.sound->set_volume(volume);
            ImGui::SameLine();
            ImGui::Text("%zu voices", panel.sound->active_voices());
        } else {
            ImGui::TextDisabled("Sound: off");
        }
        bool batching = renderer.batching();
        if (ImGui::Checkbox("Merge identical primitives (instancing)", &batching)) renderer.set_batching(batching);

        if (ImGui::Button("Save debug view (F6)")) panel.save_view = true;
        if (!panel.save_status.empty()) ImGui::TextWrapped("%s", panel.save_status.c_str());
        ImGui::Separator();
        draw_clock(clock, time_of_day_applies);
        ImGui::Separator();
        ImGui::TextUnformatted("RMB: look  WASD: move  Q/E: vertical");
        ImGui::TextUnformatted("Shift: boost  F: fly/orbit");
        ImGui::TextUnformatted("T: track  C: train here  J: switch ahead  V: follow  K: EP brake");
        ImGui::End();

        ImGui::Render();
    }

    void render(const WGPURenderPassEncoder pass) const { ImGui_ImplWGPU_RenderDrawData(ImGui::GetDrawData(), pass); }

private:
    static void draw_clock(ClockControls& clock, const bool time_of_day_applies) {
        auto hours = static_cast<float>(clock.time_of_day / 3600.0);
        const auto seconds = static_cast<int>(clock.time_of_day);
        ImGui::Text("Time %02d:%02d:%02d", seconds / 3600, seconds / 60 % 60, seconds % 60);
        if (!time_of_day_applies) {
            ImGui::SameLine();
            ImGui::TextUnformatted("(no environment: fixed light)");
        }
        if (ImGui::SliderFloat("##time", &hours, 0.0f, 24.0f, "%.2f h")) clock.time_of_day = hours * 3600.0;

        ImGui::TextUnformatted("Speed:");
        for (const float speed : {0.0f, 1.0f, 60.0f, 600.0f, 3600.0f}) {
            ImGui::SameLine();
            const auto label = speed == 0.0f ? std::string("pause") : "x" + std::to_string(static_cast<int>(speed));
            if (ImGui::RadioButton(label.c_str(), clock.speed == speed)) clock.speed = speed;
        }

        constexpr const char* seasons[] = {"Spring", "Summer", "Autumn", "Winter"};
        ImGui::Combo("Season", &clock.season, seasons, 4);
        // Order of the RTS weather variable: clear, snow, rain.
        constexpr const char* weathers[] = {"Clear", "Snow", "Rain"};
        ImGui::Combo("Weather", &clock.weather, weathers, 3);
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
        for (const auto& file : candidates) {
            if (!std::filesystem::is_regular_file(file)) continue;
            if (io.Fonts->AddFontFromFileTTF(file.c_str(), 15.0f, nullptr, ranges)) return;
        }
        io.Fonts->AddFontDefault();
    }

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
    // Scene keys, for press edges.
    bool junction_key_down = false;
    bool follow_key_down = false;
    bool track_key_down = false;
    bool labels_key_down = false;
    bool save_key_down = false;
    std::array<bool, 11> train_keys_down{};
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
    if (overlay_wants_mouse()) return;
    auto* input = camera_input(window);
    if (!input) return;

    const bool pressed = action == GLFW_PRESS;
    if (button == GLFW_MOUSE_BUTTON_LEFT && !input->camera->fly) input->orbit_dragging = pressed;
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
    if (auto* input = camera_input(window)) input->camera->zoom(static_cast<float>(y));
}

void on_key([[maybe_unused]] GLFWwindow* window,
            [[maybe_unused]] const int key,
            [[maybe_unused]] const int scancode,
            [[maybe_unused]] const int action,
            [[maybe_unused]] const int mods) {
#ifdef OPENRTS_HAS_IMGUI
    ImGui_ImplGlfw_KeyCallback(window, key, scancode, action, mods);
#endif
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

    const bool toggle = down(GLFW_KEY_F);
    if (toggle && !input.toggle_key_down) {
        camera.toggle_fly();
        if (!camera.fly) {
            input.fly_looking = false;
            set_cursor_captured(window, false);
        }
    }
    input.toggle_key_down = toggle;

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
    return {
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
        .turn_around = pressed(GLFW_KEY_X, input.train_keys_down[10]),
    };
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

[[nodiscard]] glm::mat4 projection(const ViewerCamera& camera, const float aspect) {
    const auto near_plane = (std::max)(camera.distance * 0.002f, 0.01f);
    return renderer::reversed_z_perspective(vertical_fov, aspect, near_plane, camera.distance * 20.0f);
}

struct FrameTargets {
    WGPUTextureView color;   // the frame
    WGPUTextureView depth;
    WGPUTextureView msaa = nullptr; // multisampled colour, resolved into `color`
};

[[nodiscard]] WGPURenderPassEncoder begin_main_pass(const WGPUCommandEncoder encoder,
                                                    const FrameTargets& targets,
                                                    const glm::vec4& clear_colour,
                                                    const WGPURenderPassTimestampWrites* timestamps) {
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
        .depthStoreOp = WGPUStoreOp_Discard,
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

int run_window_viewer(ViewerScene& scene, const WindowOptions& options) {
    auto context = renderer::create_headless_wgpu_device();
    if (!context.ok()) {
        std::cerr << "WebGPU device failed: " << context.error << '\n';
        return exit_code::device_failed;
    }
    const auto device = context.context.device();
    report_adapter(context.context.adapter());

    const GlfwSession glfw;
    if (!glfw.ok()) {
        std::cerr << "GLFW initialization failed.\n";
        return exit_code::platform_failed;
    }
    const Window window(initial_width, initial_height, "openRTS shape viewer");
    if (!window.get()) return exit_code::window_failed;

    ViewerCamera camera;
    CameraInput input{.camera = &camera};
    install_input_callbacks(window.get(), input);

    renderer::render_sample_count() = options.msaa;
    std::cout << "antialiasing: " << (options.msaa > 1 ? std::to_string(options.msaa) + "x MSAA, alpha-to-coverage" : "off")
              << ", anisotropy " << renderer::max_anisotropy << "x" << std::endl;
    PresentationSurface surface(context.context, window.get());
    if (!surface.ok()) return exit_code::surface_failed;

    renderer::ModelRenderer renderer(device, surface.format());
    if (!renderer.ready()) {
        std::cerr << "Model renderer initialization failed.\n";
        return exit_code::renderer_failed;
    }
    renderer.set_shading_path(options.shading);
    renderer.set_batching(options.batching);

#ifdef OPENRTS_HAS_IMGUI
    const DebugOverlay overlay(window.get(), device, surface.format());
    if (!overlay.ok()) {
        std::cerr << "ImGui initialization failed.\n";
        return exit_code::renderer_failed;
    }
#endif

    // Sound is optional: without a device the viewer runs silent.
    std::unique_ptr<audio::AudioEngine> sound;
    if (options.sound) {
        sound = std::make_unique<audio::AudioEngine>();
        if (!sound->ok()) {
            std::cout << "sound: " << sound->error() << ", running silent\n";
            sound.reset();
        } else {
            sound->set_volume(options.sound_volume);
        }
    }

    const SceneAttachment attachment(scene);
    if (const auto error = scene.attach(
            {.device = device, .target_format = surface.format(), .models = renderer, .audio = sound.get()}, camera);
        !error.empty()) {
        std::cerr << error << '\n';
        return exit_code::renderer_failed;
    }

    if (options.view) apply_view(*options.view, scene.origin(), camera);

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
    StageClock stage_clock;
    StageTimes stages{};
    double last_time = glfwGetTime();
    while (!glfwWindowShouldClose(window.get())) {
        stage_clock.start();
        glfwPollEvents();
        const double now = glfwGetTime();
        int gpu_count = 0;
        const double gpu_sum = gpu_timer.take_sum_ms(gpu_count);
        panel.stats.add(now - last_time, stages, gpu_sum, gpu_count);
        stages = {};
        const auto elapsed = static_cast<float>((std::min)(now - last_time, max_frame_seconds));
        update_camera_from_keyboard(window.get(), input, elapsed);
        last_time = now;

        toggle_shading_from_keyboard(window.get(), input, renderer);
        scene.update(renderer, camera, clock.advance(elapsed), scene_input(window.get(), input));
        if (sound) {
            sound->set_listener(camera.eye(), camera.forward());
            sound->update();
        }
        const auto environment = scene.environment();
        stage_clock.mark(FrameStage::update, stages);
#ifdef OPENRTS_HAS_IMGUI
        panel.save_view = glfwGetKey(window.get(), GLFW_KEY_F6) == GLFW_PRESS && !input.save_key_down;
        input.save_key_down = glfwGetKey(window.get(), GLFW_KEY_F6) == GLFW_PRESS;
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

        const auto frame_texture = surface.acquire();
        stage_clock.mark(FrameStage::wait, stages);
        if (!frame_texture) continue;
        const auto frame_view = wgpuTextureCreateView(frame_texture, nullptr);
        const auto encoder = wgpuDeviceCreateCommandEncoder(device, nullptr);
        const auto pass =
            begin_main_pass(encoder, {.color = frame_view, .depth = surface.depth_view(), .msaa = surface.msaa_view()}, environment.clear_colour,
                            gpu_timer.begin_frame());

        const auto aspect = static_cast<float>(width) / static_cast<float>(height);
        const FrameView frame{
            .eye = camera.eye(),
            .view = camera.view(),
            .projection = projection(camera, aspect),
            .vertical_fov = vertical_fov,
            .aspect = aspect,
            .viewport_height = static_cast<float>(height),
        };
        stage_clock.mark(FrameStage::begin, stages);
        scene.draw_background(pass, frame);
        stage_clock.mark(FrameStage::background, stages);
        renderer.begin_frame({
            .view_projection = frame.projection * frame.view,
            .camera_position = frame.eye,
            .lighting = environment.lighting,
        });
        scene.submit(renderer, frame);
        stage_clock.mark(FrameStage::cull, stages);
        renderer.render(pass);
        stage_clock.mark(FrameStage::models, stages);
        scene.draw_foreground(pass, frame);
#ifdef OPENRTS_HAS_IMGUI
        overlay.render(pass);
#endif
        stage_clock.mark(FrameStage::foreground, stages);
        wgpuRenderPassEncoderEnd(pass);
        gpu_timer.resolve(encoder);

        const auto commands = wgpuCommandEncoderFinish(encoder, nullptr);
        stage_clock.mark(FrameStage::finish, stages);
        const auto queue = wgpuDeviceGetQueue(device);
        wgpuQueueSubmit(queue, 1, &commands);
        gpu_timer.submitted();
        renderer.frame_submitted(queue);
        wgpuQueueRelease(queue);
        wgpuCommandBufferRelease(commands);
        wgpuRenderPassEncoderRelease(pass);
        wgpuCommandEncoderRelease(encoder);
        wgpuTextureViewRelease(frame_view);
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
