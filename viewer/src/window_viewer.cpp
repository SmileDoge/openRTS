#include "window_viewer.hpp"

#ifdef OPENRTS_HAS_WGPU
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>

#include <glm/ext/matrix_clip_space.hpp>

#include <openrts/renderer/webgpu_context.hpp>

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
constexpr WGPUTextureFormat depth_format = WGPUTextureFormat_Depth24Plus;

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
        if (capabilities.formatCount > 0 && capabilities.alphaModeCount > 0) {
            config_ = {
                .device = device_,
                .format = capabilities.formats[0],
                .usage = WGPUTextureUsage_RenderAttachment,
                .width = initial_width,
                .height = initial_height,
                .alphaMode = capabilities.alphaModes[0],
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
        const WGPUTextureDescriptor descriptor{
            .usage = WGPUTextureUsage_RenderAttachment,
            .dimension = WGPUTextureDimension_2D,
            .size = {.width = config_.width, .height = config_.height, .depthOrArrayLayers = 1},
            .format = depth_format,
            .mipLevelCount = 1,
            .sampleCount = 1,
        };
        depth_ = wgpuDeviceCreateTexture(device_, &descriptor);
        if (depth_) depth_view_ = wgpuTextureCreateView(depth_, nullptr);
    }

    void release_depth() {
        if (depth_view_) wgpuTextureViewRelease(depth_view_);
        if (depth_) wgpuTextureRelease(depth_);
        depth_view_ = nullptr;
        depth_ = nullptr;
    }

    WGPUDevice device_ = nullptr;
    WGPUSurface surface_ = nullptr;
    WGPUSurfaceConfiguration config_{};
    bool configured_ = false;
    WGPUTexture depth_ = nullptr;
    WGPUTextureView depth_view_ = nullptr;
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
        // Callbacks are forwarded by the viewer's own GLFW handlers.
        ImGui_ImplGlfw_InitForOther(window, false);

        ImGui_ImplWGPU_InitInfo info{};
        info.Device = device;
        info.NumFramesInFlight = 3;
        info.RenderTargetFormat = target_format;
        info.DepthStencilFormat = depth_format;
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
               const bool time_of_day_applies) const {
        ImGui_ImplWGPU_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::Begin("openRTS viewer");
        ImGui::Text("%.*s", static_cast<int>(scene.title().size()), scene.title().data());
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

        ImGui::Separator();
        draw_clock(clock, time_of_day_applies);
        ImGui::Separator();
        ImGui::TextUnformatted("RMB: look  WASD: move  Q/E: vertical");
        ImGui::TextUnformatted("Shift: boost  F: fly/orbit");
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

void toggle_shading_from_keyboard(GLFWwindow* window, CameraInput& input, renderer::ModelRenderer& renderer) {
    const bool down = glfwGetKey(window, GLFW_KEY_R) == GLFW_PRESS;
    if (down && !input.shading_key_down) {
        renderer.set_shading_path(renderer.shading_path() == renderer::ShadingPath::rts_compatible
                                      ? renderer::ShadingPath::experimental
                                      : renderer::ShadingPath::rts_compatible);
    }
    input.shading_key_down = down;
}

[[nodiscard]] glm::mat4 projection(const ViewerCamera& camera, const float aspect) {
    const auto near_plane = (std::max)(camera.distance * 0.002f, 0.01f);
    return glm::perspectiveRH_ZO(vertical_fov, aspect, near_plane, camera.distance * 20.0f);
}

struct FrameTargets {
    WGPUTextureView color;
    WGPUTextureView depth;
};

[[nodiscard]] WGPURenderPassEncoder begin_main_pass(const WGPUCommandEncoder encoder,
                                                    const FrameTargets& targets,
                                                    const glm::vec4& clear_colour) {
    const WGPURenderPassColorAttachment color{
        .view = targets.color,
        .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
        .loadOp = WGPULoadOp_Clear,
        .storeOp = WGPUStoreOp_Store,
        .clearValue = {.r = clear_colour.r, .g = clear_colour.g, .b = clear_colour.b, .a = clear_colour.a},
    };
    const WGPURenderPassDepthStencilAttachment depth{
        .view = targets.depth,
        .depthLoadOp = WGPULoadOp_Clear,
        .depthStoreOp = WGPUStoreOp_Store,
        .depthClearValue = 1.0f,
        .depthReadOnly = false,
        .stencilLoadOp = WGPULoadOp_Undefined,
        .stencilStoreOp = WGPUStoreOp_Undefined,
        .stencilReadOnly = true,
    };
    const WGPURenderPassDescriptor descriptor{
        .colorAttachmentCount = 1,
        .colorAttachments = &color,
        .depthStencilAttachment = &depth,
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

    PresentationSurface surface(context.context, window.get());
    if (!surface.ok()) return exit_code::surface_failed;

    renderer::ModelRenderer renderer(device, surface.format());
    if (!renderer.ready()) {
        std::cerr << "Model renderer initialization failed.\n";
        return exit_code::renderer_failed;
    }
    renderer.set_shading_path(options.shading);

#ifdef OPENRTS_HAS_IMGUI
    const DebugOverlay overlay(window.get(), device, surface.format());
    if (!overlay.ok()) {
        std::cerr << "ImGui initialization failed.\n";
        return exit_code::renderer_failed;
    }
#endif

    const SceneAttachment attachment(scene);
    if (const auto error = scene.attach({.device = device, .target_format = surface.format(), .models = renderer}, camera);
        !error.empty()) {
        std::cerr << error << '\n';
        return exit_code::renderer_failed;
    }

    ClockControls clock{
        .time_of_day = static_cast<double>(options.time_of_day),
        .speed = options.time_speed,
        .season = static_cast<int>(options.season),
        .weather = static_cast<int>(options.weather),
    };
    double last_time = glfwGetTime();
    while (!glfwWindowShouldClose(window.get())) {
        glfwPollEvents();
        const double now = glfwGetTime();
        const auto elapsed = static_cast<float>((std::min)(now - last_time, max_frame_seconds));
        update_camera_from_keyboard(window.get(), input, elapsed);
        last_time = now;

        toggle_shading_from_keyboard(window.get(), input, renderer);
        scene.update(renderer, camera, clock.advance(elapsed));
        const auto environment = scene.environment();
#ifdef OPENRTS_HAS_IMGUI
        overlay.build(scene, camera, renderer, clock, environment.time_of_day_applies);
#endif

        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window.get(), &width, &height);
        if (width <= 0 || height <= 0) continue;
        surface.resize(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));

        const auto frame_texture = surface.acquire();
        if (!frame_texture) continue;
        const auto frame_view = wgpuTextureCreateView(frame_texture, nullptr);
        const auto encoder = wgpuDeviceCreateCommandEncoder(device, nullptr);
        const auto pass =
            begin_main_pass(encoder, {.color = frame_view, .depth = surface.depth_view()}, environment.clear_colour);

        const auto aspect = static_cast<float>(width) / static_cast<float>(height);
        const FrameView frame{
            .eye = camera.eye(),
            .view = camera.view(),
            .projection = projection(camera, aspect),
            .vertical_fov = vertical_fov,
            .aspect = aspect,
        };
        scene.draw_background(pass, frame);
        renderer.begin_frame({
            .view_projection = frame.projection * frame.view,
            .camera_position = frame.eye,
            .lighting = environment.lighting,
        });
        scene.submit(renderer, frame);
        renderer.render(pass);
#ifdef OPENRTS_HAS_IMGUI
        overlay.render(pass);
#endif
        wgpuRenderPassEncoderEnd(pass);

        const auto commands = wgpuCommandEncoderFinish(encoder, nullptr);
        const auto queue = wgpuDeviceGetQueue(device);
        wgpuQueueSubmit(queue, 1, &commands);
        renderer.frame_submitted(queue);
        wgpuQueueRelease(queue);
        wgpuCommandBufferRelease(commands);
        wgpuRenderPassEncoderRelease(pass);
        wgpuCommandEncoderRelease(encoder);
        wgpuTextureViewRelease(frame_view);
        surface.present();
        wgpuTextureRelease(frame_texture);
        renderer.collect_retired();
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
