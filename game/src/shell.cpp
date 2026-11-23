#include "shell.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <future>
#include <string>
#include <utility>
#include <vector>

#include <openrts/core/core.hpp>
#include <openrts/core/installation.hpp>
#include <openrts/core/vfs.hpp>
#include <openrts/core/virtual_path.hpp>
#include <openrts/formats/activity.hpp>
#include <openrts/ui/fonts.hpp>
#include <openrts/ui/text.hpp>

#ifdef OPENRTS_HAS_IMGUI
#include <imgui.h>
#endif

#include "key_layout.hpp"
#include "menu/ui_image.hpp"
#include "menu/ui_sound.hpp"
#include "viewer_state.hpp"

namespace openrts::game {
namespace {

namespace fs = std::filesystem;

constexpr int stage_count = 5;
constexpr const char* stage_names[stage_count] = {
    "Подготовка окружения",
    "Чтение путей и объектов",
    "Подготовка поезда",
    "Загрузка кабины",
    "Финальная проверка",
};

constexpr const char* loading_tips[] = {
    "Тормозной путь зависит от скорости, массы поезда и уклона пути.",
    "Перед отправлением проверьте показания светофоров и положение стрелок.",
    "В дождь сцепление колёс с рельсами снижается — тормозной путь растёт.",
    "Давление в тормозной магистрали контролируется по манометрам в кабине.",
    "На затяжных подъёмах следите за нагревом тяговых двигателей.",
};

struct BuildResult {
    std::unique_ptr<viewer::ViewerScene> scene;
    std::string error;
};

[[nodiscard]] std::string utf8(const fs::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}


// The scene's CPU part on a worker: the VFS over the installation, the
// scenario (if one is chosen) and the route scene itself. No GPU, no window.
BuildResult build_session(const SessionRequest& request, std::atomic<int>& stage, std::atomic<float>& progress) {
    try {
        stage = 0;
        progress = 0.03f;
        auto vfs = std::make_shared<core::Vfs>();
        const auto config = core::load_installation_config(request.root);
        core::mount_installation(*vfs, request.root, config);
        vfs->build_index();
        progress = 0.13f;

        const auto route = core::VirtualPath::parse("ROUTES/" + request.route);
        if (!route) return {nullptr, "Не найден маршрут " + request.route};
        if (!vfs->is_directory(*route)) return {nullptr, "Маршрут " + request.route + " отсутствует в папке игры"};
        stage = 1;
        progress = 0.2f;

        std::shared_ptr<const formats::Activity> activity;
        core::VirtualPath activity_folder;
        if (!request.activity.empty()) {
            auto file = formats::load_activity(*vfs, *route, request.activity);
            if (!file) return {nullptr, "Не найден сценарий " + request.activity};
            if (!file->activity.ok()) return {nullptr, file->path.str() + ": " + file->activity.error};
            activity_folder = formats::activity_folder(*route, file->file).value_or(core::VirtualPath{});
            activity = std::make_shared<const formats::Activity>(std::move(file->activity));
        }

        viewer::SceneContent content;
        content.vfs = vfs;
        content.consist = request.consist;
        content.path = request.path;
        content.start_state = request.start_state;
        content.situation = request.situation;
        content.start_view = request.start_view;
        content.realistic_air = request.realistic_air;
        content.root = request.root;
        content.activity = activity;
        content.activity_folder = activity_folder;
        content.load_game = request.save;
        content.activity_name = request.activity;

        auto loaded = viewer::load_route_scene(std::move(content), *route);
        if (!loaded.error.empty()) return {nullptr, loaded.error};
        stage = 2;
        progress = 0.62f;
        return {std::move(loaded.scene), {}};
    } catch (const std::exception& error) {
        return {nullptr, std::string("Ошибка загрузки: ") + error.what()};
    } catch (...) {
        return {nullptr, "Неизвестная ошибка загрузки"};
    }
}

#ifdef OPENRTS_HAS_IMGUI
[[nodiscard]] float menu_scale() {
    const auto size = ImGui::GetIO().DisplaySize;
    return (std::min)(size.x / 1920.0f, size.y / 1080.0f);
}

// The exact filled arrow of the mockup's 13x13 SVG.
void arrow_up_right(ImDrawList* draw, const ImVec2 centre, const float size, const ImU32 colour) {
    constexpr ImVec2 outline[] = {
        {1.248f, 12.0478f}, {0.0f, 10.7998f}, {8.856f, 1.9438f}, {1.32f, 2.7598f}, {1.176f, 0.983804f},
        {12.048f, -0.000196457f}, {11.064f, 10.8958f}, {9.288f, 10.7278f}, {10.104f, 3.1918f},
    };
    ImVec2 points[9];
    const auto scale = size / 13.0f;
    const auto origin = ImVec2{centre.x - 6.024f * scale, centre.y - 6.024f * scale};
    for (int i = 0; i < 9; ++i) points[i] = {origin.x + outline[i].x * scale, origin.y + outline[i].y * scale};
    draw->AddConcavePolyFilled(points, 9, colour);
}


void cover(ImDrawList* draw, const WGPUTextureView view, const std::pair<int, int> pixels, const ImVec2 min, const ImVec2 max,
           const float rounding = 0.0f, const ImU32 tint = IM_COL32_WHITE) {
    if (!view || pixels.first <= 0 || pixels.second <= 0) {
        draw->AddRectFilled(min, max, IM_COL32(18, 20, 25, 255), rounding);
        return;
    }
    const auto width = max.x - min.x;
    const auto height = max.y - min.y;
    const auto source = static_cast<float>(pixels.first) / static_cast<float>(pixels.second);
    const auto target = width / (std::max)(height, 1.0f);
    ImVec2 uv0{0.0f, 0.0f}, uv1{1.0f, 1.0f};
    if (target > source) {
        const auto share = source / target;
        uv0.y = (1.0f - share) * 0.5f;
        uv1.y = 1.0f - uv0.y;
    } else {
        const auto share = target / source;
        uv0.x = (1.0f - share) * 0.5f;
        uv1.x = 1.0f - uv0.x;
    }
    draw->AddImageRounded(reinterpret_cast<ImTextureID>(view), min, max, uv0, uv1, tint, rounding);
}

void label(ImDrawList* draw, ImFont* font, const float size, const ImVec2 at, const ImU32 colour, const std::string& text) {
    if (!font) return;
    draw->AddText(font, size, at, colour, text.c_str());
}

[[nodiscard]] ImVec2 label_size(ImFont* font, const float size, const std::string& text) {
    if (!font) return {0.0f, 0.0f};
    return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str());
}

void label_right(ImDrawList* draw, ImFont* font, const float size, const float right, const float y, const ImU32 colour,
                 const std::string& text) {
    const auto measured = label_size(font, size, text);
    label(draw, font, size, {right - measured.x, y}, colour, text);
}

[[nodiscard]] std::string percent_text(const float value) {
    char text[16]{};
    std::snprintf(text, sizeof text, "%d%%", static_cast<int>(std::lround(std::clamp(value, 0.0f, 1.0f) * 100.0f)));
    return text;
}

[[nodiscard]] std::string save_name() {
    const auto now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char name[64]{};
    std::strftime(name, sizeof name, "quick-%Y%m%d-%H%M%S", &local);
    return name;
}
#endif

} // namespace

struct GameShell::State {
    explicit State(fs::path directory) : exe_dir(std::move(directory)), menu(exe_dir) {}

    fs::path exe_dir;
    Launcher menu;

    std::unique_ptr<viewer::ViewerScene> session;
    SessionRequest request;
    std::future<BuildResult> build;
    bool building = false;
    bool session_attached = false;
    float build_started = 0.0f;
    std::chrono::steady_clock::time_point load_started{};
    std::size_t loading_tip = 0;
    int frames_in_session = 0;
    bool clock_pending = false;

    std::atomic<int> build_stage{0};
    std::atomic<float> build_progress{0.0f};

    bool paused = false;
    bool pause_settings = false;
    bool pause_full_settings = false;
    bool pause_next = false;
    int resume_frames = 0;
    bool resume_requested = false;
    std::string pause_note;
    float pause_hover[6]{};
    float volume = 0.5f;
    float sensitivity = 1.0f;
    float head_motion = 1.0f;

    WGPUDevice device = nullptr;
    WGPUTextureFormat target_format = WGPUTextureFormat_Undefined;
    renderer::ModelRenderer* models = nullptr;
    viewer::ViewerCamera* camera = nullptr;
    audio::AudioEngine* audio = nullptr;

    menu::ImageCache images;
    menu::UiSound ui_sound;
    std::string image_dir;

    [[nodiscard]] int loading_stage() const {
        if (!session) return std::clamp(build_stage.load(), 0, stage_count - 1);
        const auto state = session->loading();
        const std::string_view text = state.stage ? state.stage : "";
        if (text.find("cab") != std::string_view::npos) return 3;
        if (text.find("train") != std::string_view::npos) return 2;
        if (text.find("route") != std::string_view::npos) return 1;
        const auto value = build_progress.load();
        if (value < 0.15f) return 0;
        if (value < 0.55f) return 1;
        if (value < 0.78f) return 2;
        if (value < 0.92f) return 3;
        return 4;
    }

    [[nodiscard]] float loading_progress() const {
        float value = build_progress.load();
        if (session && session_attached) {
            const auto state = session->loading();
            value = 0.62f + 0.37f * std::clamp(state.progress, 0.0f, 1.0f);
            if (state.done && frames_in_session > 2) value = 1.0f;
        }
        return std::clamp(value, 0.0f, 1.0f);
    }

    [[nodiscard]] bool loading() const {
        if (building) return true;
        if (!session) return false;
        if (!session->loading().done || frames_in_session <= 2) return true;
        // Let the mockup's screen be seen even when a small route loads in
        // one frame: the check screen is part of the game, not a flicker.
        return std::chrono::steady_clock::now() - load_started < std::chrono::milliseconds(2000);
    }

    void start_build(const SessionRequest& next) {
        request = next;
        volume = next.volume;
        if (audio) audio->set_volume(next.volume);
        clock_pending = true;
        load_started = std::chrono::steady_clock::now();
        loading_tip = static_cast<std::size_t>(load_started.time_since_epoch().count()) % std::size(loading_tips);
        paused = false;
        pause_settings = false;
        pause_full_settings = false;
        pause_note.clear();
        build_stage = 0;
        build_progress = 0.02f;
        building = true;
        frames_in_session = 0;
        const auto captured = next;
        auto* stage = &build_stage;
        auto* progress = &build_progress;
        build = std::async(std::launch::async, [captured, stage, progress] { return build_session(captured, *stage, *progress); });
    }

    void end_session(const std::string& note) {
        if (session) session->detach();
        session.reset();
        session_attached = false;
        paused = false;
        pause_settings = false;
        pause_full_settings = false;
        clock_pending = false;
        building = false;
        frames_in_session = 0;
        menu.session_finished(note);
    }

    void attach_session() {
        if (!session || session_attached || !device || !models || !camera) return;
        const auto error = session->attach({.device = device, .target_format = target_format, .models = *models, .audio = audio}, *camera);
        if (!error.empty()) {
            session.reset();
            session_attached = false;
            building = false;
            menu.session_failed(error);
            return;
        }
        session_attached = true;
        frames_in_session = 0;
    }
};

GameShell::GameShell(fs::path exe_dir) : state_(std::make_unique<State>(std::move(exe_dir))) {
    state_->image_dir = utf8(state_->exe_dir / "menu");
}

GameShell::~GameShell() {
    if (state_->build.valid()) {
        try {
            state_->build.wait();
        } catch (...) {
        }
    }
    if (state_->session) state_->session->detach();
}

std::string_view GameShell::title() const {
    if (state_->session) return state_->session->title();
    return "DTRE";
}

bool GameShell::play(std::string route, std::string consist) {
    if (route.empty()) return false;
    const auto root_text = viewer::remembered("root");
    if (!root_text || root_text->empty()) return false;
    SessionRequest request;
    request.root = fs::path(std::u8string(root_text->begin(), root_text->end()));
    request.route = std::move(route);
    request.consist = std::move(consist);
    request.start_view = 1;
    request.sound = true;
    request.volume = state_->volume;
    request.title = request.route;
    if (!request.consist.empty()) request.title += " — " + request.consist;
    state_->start_build(request);
    return true;
}

void GameShell::pause_when_loaded() { state_->pause_next = true; }

void GameShell::apply_window_options(viewer::WindowOptions& options) const { state_->menu.apply_window_options(options); }

std::string GameShell::attach(const viewer::SceneGpu& gpu, viewer::ViewerCamera& camera) {
    state_->device = gpu.device;
    state_->target_format = gpu.target_format;
    state_->models = &gpu.models;
    state_->camera = &camera;
    state_->audio = gpu.audio;
    state_->images.set_device(gpu.device);
    state_->ui_sound.load(state_->image_dir);
    const auto error = state_->menu.attach(gpu, camera);
    if (!error.empty()) return error;
    if (state_->building || state_->session) state_->load_started = std::chrono::steady_clock::now();
    if (state_->audio) {
        state_->audio->set_volume(state_->volume);
    }
    return {};
}

void GameShell::detach() {
    if (state_->session) {
        state_->session->detach();
        state_->session.reset();
    }
    state_->session_attached = false;
    state_->menu.detach();
    state_->images.clear();
}

void GameShell::update(renderer::ModelRenderer& renderer, viewer::ViewerCamera& camera, const viewer::SceneClock& clock,
                       const viewer::SceneInput& input) {
    auto& s = *state_;
    s.menu.update(renderer, camera, clock, input);
    if (s.resume_frames > 0) {
        // The pause overlay's click lands after update; keep the next frame
        // unpaused even if the scene reports a scenario hold for one tick.
        s.paused = false;
        s.pause_next = false;
        --s.resume_frames;
    }

    if (!s.session && !s.building) {
        if (auto request = s.menu.take_session_request()) s.start_build(*request);
    }

    if (s.building && s.build.valid() && s.build.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        BuildResult result;
        try {
            result = s.build.get();
        } catch (const std::exception& error) {
            result.error = std::string("Ошибка загрузки: ") + error.what();
        } catch (...) {
            result.error = "Неизвестная ошибка загрузки";
        }
        s.building = false;
        if (!result.scene) {
            s.menu.session_failed(result.error.empty() ? "Не удалось загрузить маршрут" : result.error);
        } else {
            s.session = std::move(result.scene);
            s.session_attached = false;
            s.build_progress = 0.62f;
            s.attach_session();
        }
    }

    if (!s.session) return;
    if (s.session->wants_close()) {
        s.end_session("Поездка завершена");
        return;
    }

    viewer::SceneClock session_clock = clock;
    viewer::SceneInput session_input = input;
    if (s.paused) {
        session_clock.time_speed = 0.0f;
        session_clock.elapsed = 0.0f;
        session_input = viewer::SceneInput{};
    }
    s.session->update(renderer, camera, session_clock, session_input);
    ++s.frames_in_session;

    if (s.pause_next && s.session->loading().done) {
        s.paused = true;
        s.pause_next = false;
    }
    if (s.session->loading().done && s.frames_in_session > 2) s.build_progress = 0.99f;
}

viewer::SceneEnvironment GameShell::environment() const {
    if (state_->session) return state_->session->environment();
    viewer::SceneEnvironment environment;
    environment.clear_colour = {0.02f, 0.025f, 0.032f, 1.0f};
    return environment;
}

void GameShell::draw_background(WGPURenderPassEncoder pass, const viewer::FrameView& frame) {
    if (state_->session) state_->session->draw_background(pass, frame);
}

void GameShell::submit(renderer::ModelRenderer& renderer, const viewer::FrameView& frame) {
    if (state_->session) state_->session->submit(renderer, frame);
}

void GameShell::draw_foreground(WGPURenderPassEncoder pass, const viewer::FrameView& frame) {
    if (state_->session) state_->session->draw_foreground(pass, frame);
}

void GameShell::draw_before_translucent(WGPURenderPassEncoder pass, const viewer::FrameView& frame) {
    if (state_->session) state_->session->draw_before_translucent(pass, frame);
}

void GameShell::draw_depth(WGPURenderPassEncoder pass, const std::uint32_t slot, const glm::mat4& view_projection,
                           const renderer::DepthPassKind kind) {
    if (state_->session) state_->session->draw_depth(pass, slot, view_projection, kind);
}

void GameShell::prepare_depth() {
    if (state_->session) state_->session->prepare_depth();
}

void GameShell::before_main_pass(WGPUCommandEncoder encoder, const viewer::FrameView& frame) {
    if (state_->session) state_->session->before_main_pass(encoder, frame);
}

void GameShell::draw_after_translucent(WGPURenderPassEncoder pass, const viewer::FrameView& frame) {
    if (state_->session) state_->session->draw_after_translucent(pass, frame);
}

void GameShell::draw_bloom_sources(WGPURenderPassEncoder pass, const viewer::FrameView& frame) {
    if (state_->session) state_->session->draw_bloom_sources(pass, frame);
}

WGPUQuerySet GameShell::occlusion_query_set() const {
    return state_->session ? state_->session->occlusion_query_set() : nullptr;
}

void GameShell::after_main_pass(WGPUCommandEncoder encoder) {
    if (state_->session) state_->session->after_main_pass(encoder);
}

void GameShell::frame_submitted() {
    if (state_->session) state_->session->frame_submitted();
}

std::optional<formats::RoutePosition> GameShell::origin() const {
    return state_->session ? state_->session->origin() : std::nullopt;
}

void GameShell::draw_overlay(const viewer::ViewerCamera& camera) const {
#ifdef OPENRTS_HAS_IMGUI
    auto& s = *state_;
    // The menu's own sounds ring after every window of this frame is drawn:
    // any hovered button plays select, any click plays click.
    const bool ui_sound_active = (!s.session && !s.building) || s.paused;
    struct UiSoundTick {
        menu::UiSound& sound;
        bool active;
        ~UiSoundTick() {
            if (active) sound.tick();
        }
    } ui_sound_tick{s.ui_sound, ui_sound_active};

    if (s.session) {
        // The route scene's own overlay is its debug page (positions, tile
        // counts, checkboxes): the viewer keeps it in an off-screen window and
        // so does the shell. The train's HUD and the scene's own windows are
        // separate windows and still show.
        ImGui::SetNextWindowPos({-200.0f, -200.0f});
        ImGui::SetNextWindowSize({1.0f, 1.0f});
        ImGui::Begin("##session_unseen", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground
                         | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
                         | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove);
        s.session->draw_overlay(camera);
        ImGui::End();
        if (s.loading()) {
            draw_loading_overlay();
            return;
        }

        if (s.paused) {
            if (s.pause_full_settings) draw_pause_settings_overlay();
            else draw_pause_overlay();
            return;
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !s.session->escape_taken()) {
            s.paused = true;
            s.pause_settings = false;
            s.pause_note.clear();
        }
        return;
    }

    if (s.building) {
        draw_loading_overlay();
        return;
    }

    s.menu.draw_overlay(camera);
#endif
}

void GameShell::draw_loading_overlay() const {
#ifdef OPENRTS_HAS_IMGUI
    auto& s = *state_;
    const auto display = ImGui::GetIO().DisplaySize;
    const auto scale = menu_scale();
    const auto margin = 72.0f * scale;

    ImGui::SetNextWindowPos({0.0f, 0.0f});
    ImGui::SetNextWindowSize(display);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4{0.0f, 0.0f, 0.0f, 0.0f});
    ImGui::Begin("##loading", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    auto* draw = ImGui::GetWindowDrawList();

    const auto background = fs::path(s.image_dir) / "emu.png";
    cover(draw, s.images.load(background), s.images.size(background), {0, 0}, display);
    draw->AddRectFilledMultiColor({0, 0}, display, IM_COL32(7, 9, 13, 60), IM_COL32(7, 9, 13, 70), IM_COL32(7, 9, 13, 200),
                                  IM_COL32(7, 9, 13, 180));

    const auto white = IM_COL32(247, 248, 250, 255);
    const auto grey = IM_COL32(168, 174, 185, 255);
    const auto dim = IM_COL32(112, 118, 130, 255);

    label(draw, ui::brand_font(), 34.0f * scale, {margin, 42.0f * scale}, white, "DARK TEAM RAILWAY ENGINE");
    label(draw, ui::body_font(), 14.0f * scale, {margin + 1.0f * scale, 86.0f * scale}, grey, "Инновации в движении");

    const auto panel_height = 227.0f * scale;
    const auto panel_bottom = display.y - 87.0f * scale;
    const auto panel_top = panel_bottom - panel_height;
    const ImVec2 panel_min{margin, panel_top};
    const ImVec2 panel_max{display.x - margin, panel_bottom};
    // The mockup's glass: the train stays visible through the panel, which is
    // a faint dark tint under a soft outline, with a light top sheen and a
    // shadow that lifts it off the picture.
    const auto radius = 24.0f * scale;
    for (int i = 3; i >= 1; --i) {
        const auto grow = static_cast<float>(i) * 3.0f * scale;
        draw->AddRectFilled({panel_min.x - grow, panel_min.y - grow + 2.0f * scale},
                            {panel_max.x + grow, panel_max.y + grow + 2.0f * scale}, IM_COL32(0, 0, 0, 12), radius + grow);
    }
    draw->AddRectFilled(panel_min, panel_max, IM_COL32(10, 14, 22, 58), radius);
    draw->AddRectFilledMultiColor({panel_min.x + radius, panel_min.y + 1.0f * scale},
                                  {panel_max.x - radius, panel_min.y + 54.0f * scale}, IM_COL32(255, 255, 255, 16),
                                  IM_COL32(255, 255, 255, 16), IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0));
    draw->AddRect(panel_min, panel_max, IM_COL32(255, 255, 255, 84), radius, 0, 1.0f * scale);
    draw->AddLine({panel_min.x + radius, panel_min.y + 1.0f * scale},
                  {panel_max.x - radius, panel_min.y + 1.0f * scale}, IM_COL32(255, 255, 255, 120), 1.0f * scale);
    // The mockup's inner separator and its progress track.
    draw->AddLine({panel_min.x + 33.0f * scale, panel_min.y + 116.0f * scale},
                  {panel_max.x - 33.0f * scale, panel_min.y + 116.0f * scale}, IM_COL32(255, 255, 255, 40), 1.0f);

    const auto pad = 36.0f * scale;
    label(draw, ui::heading_font(), 26.0f * scale, {panel_min.x + pad, panel_min.y + 32.0f * scale}, white, "Знаете ли вы?");
    const auto tip = std::string(loading_tips[s.loading_tip % std::size(loading_tips)]);
    draw->AddText(ui::body_font(), 18.0f * scale, {panel_min.x + pad, panel_min.y + 70.0f * scale}, grey, tip.c_str());

    const auto stage = std::clamp(s.loading_stage(), 0, stage_count - 1);
    char counter[32]{};
    std::snprintf(counter, sizeof counter, "%02d / %02d", stage + 1, stage_count);
    label_right(draw, ui::clock_font(), 29.0f * scale, panel_max.x - pad, panel_min.y + 30.0f * scale, white, counter);

    const auto heading_y = panel_min.y + 144.0f * scale;
    const auto heading_width = label_size(ui::heading_font(), 18.0f * scale, "Загрузка маршрута").x;
    label(draw, ui::heading_font(), 18.0f * scale, {panel_min.x + pad, heading_y}, white, "Загрузка маршрута");
    label(draw, ui::body_font(), 16.0f * scale, {panel_min.x + pad + heading_width + 16.0f * scale, panel_min.y + 146.0f * scale},
          grey, stage_names[stage]);

    const auto progress = s.loading_progress();
    label_right(draw, ui::clock_font(), 32.0f * scale, panel_max.x - pad, panel_min.y + 144.0f * scale, white, percent_text(progress));
    const auto bar_min = ImVec2{panel_min.x + 33.0f * scale, panel_max.y - 35.0f * scale};
    const auto bar_max = ImVec2{panel_max.x - 33.0f * scale, panel_max.y - 32.0f * scale};
    draw->AddRectFilled(bar_min, bar_max, IM_COL32(255, 255, 255, 40), 3.0f * scale);
    draw->AddRectFilled(bar_min, {bar_min.x + (bar_max.x - bar_min.x) * progress, bar_max.y}, white, 3.0f * scale);

    const auto footer_y = display.y - 60.0f * scale;
    label(draw, ui::small_font(), 11.0f * scale, {margin, footer_y}, dim,
          "DARK TEAM  /  Инновации в движении");
    label_right(draw, ui::small_font(), 11.0f * scale, display.x - margin, footer_y, dim, "dark-team.ru");

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
#endif
}

void GameShell::draw_pause_overlay() const {
#ifdef OPENRTS_HAS_IMGUI
    auto& s = *state_;
    const auto display = ImGui::GetIO().DisplaySize;
    const auto scale = menu_scale();

    ImGui::SetNextWindowPos({0.0f, 0.0f});
    ImGui::SetNextWindowSize(display);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4{0.0f, 0.0f, 0.0f, 0.0f});
    ImGui::Begin("##pause", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    auto* draw = ImGui::GetWindowDrawList();
    const auto close_overlay = [&] {
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
    };

    draw->AddRectFilled({0, 0}, display, IM_COL32(4, 6, 10, 188));

    const auto width = 470.0f * scale;
    const auto height = (s.pause_settings ? 430.0f : 430.0f) * scale;
    const ImVec2 min{(display.x - width) * 0.5f, (display.y - height) * 0.5f};
    const ImVec2 max{(display.x + width) * 0.5f, (display.y + height) * 0.5f};
    draw->AddRectFilled(min, max, IM_COL32(20, 22, 27, 252), 24.0f * scale);
    draw->AddRect(min, max, IM_COL32(255, 255, 255, 28), 24.0f * scale, 0, 1.0f);

    const auto pad = 30.0f * scale;
    label(draw, ui::small_font(), 11.0f * scale, {min.x + pad, min.y + 24.0f * scale}, IM_COL32(120, 126, 138, 255), "ПАУЗА");
    label(draw, ui::heading_font(), 27.0f * scale, {min.x + pad, min.y + 46.0f * scale}, IM_COL32(247, 248, 250, 255),
          s.pause_settings ? "Настройки" : "Одиночная игра");

    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        if (s.pause_settings) {
            s.pause_settings = false;
        } else {
            s.paused = false;
            s.pause_next = false;
            s.resume_frames = 2;
            s.resume_requested = true;
            close_overlay();
            return;
        }
    }

    if (s.pause_settings) {
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4{0.10f, 0.11f, 0.14f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4{0.90f, 0.91f, 0.93f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.82f, 0.84f, 0.88f, 1.0f});
        ImGui::PushFont(ui::body_font());
        ImGui::SetCursorScreenPos({min.x + pad, min.y + 102.0f * scale});
        ImGui::BeginGroup();
        ImGui::SetNextItemWidth(width - pad * 2.0f);
        if (ImGui::SliderFloat("Громкость", &s.volume, 0.0f, 1.0f, "%.2f")) {
            if (s.audio) s.audio->set_volume(s.volume);
            viewer::remember("volume", std::to_string(s.volume));
            s.request.volume = s.volume;
        }
        ImGui::SetNextItemWidth(width - pad * 2.0f);
        if (ImGui::SliderFloat("Чувствительность мыши", &s.sensitivity, 0.1f, 3.0f, "%.2f")) {
            viewer::set_mouse_sensitivity(s.sensitivity);
            viewer::remember("mouse_sensitivity", std::to_string(s.sensitivity));
        }
        ImGui::SetNextItemWidth(width - pad * 2.0f);
        if (ImGui::SliderFloat("Движение головы", &s.head_motion, 0.0f, 1.0f, "%.2f")) {
            viewer::set_head_motion(s.head_motion);
            viewer::remember("head_motion", std::to_string(s.head_motion));
        }
        ImGui::EndGroup();
        ImGui::PopFont();
        ImGui::PopStyleColor(3);

        label(draw, ui::small_font(), 12.0f * scale, {min.x + pad, min.y + height - 78.0f * scale},
              IM_COL32(120, 126, 138, 255), "Остальные настройки — в главном меню.");
    } else {
        constexpr const char* items[4] = {"Продолжить", "Карта маршрута", "Сохранить игру", "Настройки"};
        const auto top = min.y + 112.0f * scale;
        const auto item_height = 52.0f * scale;
        for (int i = 0; i < 4; ++i) {
            const ImVec2 item_min{min.x + pad, top + item_height * static_cast<float>(i)};
            const ImVec2 item_max{max.x - pad, item_min.y + item_height - 8.0f * scale};
            ImGui::SetCursorScreenPos(item_min);
            ImGui::PushID(500 + i);
            const bool pressed = ImGui::InvisibleButton("##pause_item", {item_max.x - item_min.x, item_max.y - item_min.y});
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            s.pause_hover[i] += ((hovered ? 1.0f : 0.0f) - s.pause_hover[i]) * (std::min)(1.0f, ImGui::GetIO().DeltaTime * 12.0f);
            const auto h = s.pause_hover[i];
            const auto fill = i == 0 ? ImVec4{0.96f + 0.04f * h, 0.96f + 0.04f * h, 0.97f + 0.03f * h, 1.0f}
                                     : ImVec4{0.10f + 0.05f * h, 0.11f + 0.05f * h, 0.13f + 0.06f * h, 1.0f};
            draw->AddRectFilled(item_min, item_max, ImGui::ColorConvertFloat4ToU32(fill), 11.0f * scale);
            if (i != 0) draw->AddRect(item_min, item_max, IM_COL32(255, 255, 255, static_cast<int>(20 + 40 * h)), 11.0f * scale);
            const auto text_colour = i == 0 ? IM_COL32(19, 21, 25, 255) : IM_COL32(225, 229, 236, 255);
            label(draw, ui::button_font(), 15.0f * scale, {item_min.x + 18.0f * scale, item_min.y + 15.0f * scale}, text_colour, items[i]);
            const auto arrow_centre = ImVec2{item_max.x - 26.0f * scale, (item_min.y + item_max.y) * 0.5f};
            arrow_up_right(draw, arrow_centre, 12.0f * scale,
                           i == 0 ? IM_COL32(19, 21, 25, 255) : IM_COL32(140, 146, 158, 255));
            if (!pressed) continue;
            switch (i) {
            case 0:
                s.paused = false;
                s.pause_next = false;
                s.pause_settings = false;
                s.resume_frames = 2;
                s.resume_requested = true;
                if (s.session && s.session->paused()) s.session->close_activity_message();
                close_overlay();
                return;
            case 1:
                if (s.session) s.session->open_track_map();
                s.paused = false;
                s.pause_next = false;
                s.resume_frames = 2;
                s.resume_requested = true;
                close_overlay();
                return;
            case 2: {
                const auto name = save_name();
                const auto result = s.session ? s.session->save_game(name) : std::string("nothing to save");
                s.pause_note = result.empty() ? "Игра сохранена: " + name : result;
                break;
            }
            case 3:
                s.pause_full_settings = true;
                s.sensitivity = viewer::mouse_sensitivity();
                s.head_motion = viewer::head_motion();
                break;
            }
        }

        const ImVec2 exit_min{min.x + pad, min.y + 330.0f * scale};
        const ImVec2 exit_max{max.x - pad, exit_min.y + 44.0f * scale};
        ImGui::SetCursorScreenPos(exit_min);
        const bool exit_pressed = ImGui::InvisibleButton("##leave", {exit_max.x - exit_min.x, exit_max.y - exit_min.y});
        const bool exit_hovered = ImGui::IsItemHovered();
        draw->AddRect(exit_min, exit_max, exit_hovered ? IM_COL32(255, 255, 255, 70) : IM_COL32(255, 255, 255, 24), 11.0f * scale);
        label(draw, ui::button_font(), 14.0f * scale, {exit_min.x + 18.0f * scale, exit_min.y + 12.0f * scale},
              exit_hovered ? IM_COL32(247, 248, 250, 255) : IM_COL32(160, 166, 178, 255), "Выйти в меню");
        if (exit_pressed) {
            s.end_session("Вы вышли в главное меню");
            close_overlay();
            return;
        }

        if (!s.pause_note.empty()) {
            label(draw, ui::small_font(), 12.0f * scale, {min.x + pad, max.y - 104.0f * scale},
                  IM_COL32(150, 156, 168, 255), s.pause_note);
        }
    }

    // The full-screen window takes the clicks and keys behind the buttons.
    close_overlay();
#endif
}

void GameShell::draw_pause_settings_overlay() const {
#ifdef OPENRTS_HAS_IMGUI
    auto& s = *state_;
    const auto display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos({0.0f, 0.0f});
    ImGui::SetNextWindowSize(display);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4{0.0f, 0.0f, 0.0f, 0.0f});
    ImGui::Begin("##pause_settings", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    const bool back = s.menu.draw_embedded_settings();
    const bool escape = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    if (back || escape) {
        s.pause_full_settings = false;
        s.pause_settings = false;
    }
#endif
}

bool GameShell::keyboard_captured() const { return state_->session ? state_->session->keyboard_captured() : true; }

void GameShell::open_track_map() {
    if (state_->session) state_->session->open_track_map();
}

bool GameShell::engineer_on_foot() const { return state_->session ? state_->session->engineer_on_foot() : false; }

int GameShell::close_code() const { return state_->session ? state_->session->close_code() : 0; }

bool GameShell::escape_taken() const { return state_->paused || (state_->session && state_->session->escape_taken()); }

bool GameShell::takes_scroll() const { return state_->session && state_->session->takes_scroll(); }

void GameShell::restore_screen_focus() {
    if (state_->session) state_->session->restore_screen_focus();
}

void GameShell::set_cab_switch(const std::uint32_t id, const std::uint32_t value) {
    if (state_->session) state_->session->set_cab_switch(id, value);
}

std::optional<std::pair<std::uint32_t, std::uint32_t>> GameShell::cab_switch_state(const std::uint32_t id) const {
    return state_->session ? state_->session->cab_switch_state(id) : std::nullopt;
}

std::string GameShell::world_report() const { return state_->session ? state_->session->world_report() : std::string(); }

std::optional<double> GameShell::take_time_of_day() {
    return state_->session ? state_->session->take_time_of_day() : std::nullopt;
}

fs::path GameShell::saves_folder() const { return state_->session ? state_->session->saves_folder() : fs::path(); }

std::vector<std::string> GameShell::saved_games() const { return state_->session ? state_->session->saved_games() : std::vector<std::string>(); }

std::string GameShell::save_game(const std::string& name) {
    return state_->session ? state_->session->save_game(name) : std::string("nothing to save");
}

std::string GameShell::load_game(const fs::path& file) {
    return state_->session ? state_->session->load_game(file) : std::string("nothing to load into");
}

std::optional<viewer::ViewerScene::SavedClock> GameShell::take_saved_clock() {
    auto& s = *state_;
    if (s.session) {
        if (auto clock = s.session->take_saved_clock()) return clock;
    }
    if (!s.clock_pending) return std::nullopt;
    s.clock_pending = false;
    if (!s.request.save.empty()) return std::nullopt;
    viewer::ViewerScene::SavedClock clock;
    clock.time_of_day = static_cast<double>(std::clamp(s.request.minutes, 0, 24 * 60 - 1)) * 60.0;
    clock.season = std::clamp(s.request.season, 0, 3);
    clock.weather = std::clamp(s.request.weather, 0, 2);
    clock.storm = s.request.weather == 3;
    if (clock.storm) clock.weather = 2;
    return clock;
}

bool GameShell::paused() const { return state_->paused || (state_->session && state_->session->paused()); }

bool GameShell::take_resume_request() {
    const bool requested = state_->resume_requested;
    state_->resume_requested = false;
    return requested;
}

viewer::SceneLoading GameShell::loading() const {
    viewer::SceneLoading state;
    state.done = !state_->loading();
    state.progress = state_->loading_progress();
    state.stage = "";
    state.title = state_->request.title;
    return state;
}

std::optional<viewer::ScenePresence> GameShell::presence() const {
    if (state_->session) return state_->session->presence();
    return state_->menu.presence();
}

std::optional<glm::mat4> GameShell::train_car_transform(const std::size_t car) const {
    return state_->session ? state_->session->train_car_transform(car) : std::nullopt;
}

void GameShell::enter_cab() {
    if (state_->session) state_->session->enter_cab();
}

bool GameShell::seated_in_cab() const { return state_->session ? state_->session->seated_in_cab() : false; }

bool GameShell::has_train_panel() const { return state_->session ? state_->session->has_train_panel() : false; }

void GameShell::draw_train_panel() const {
    if (state_->session) state_->session->draw_train_panel();
}

void GameShell::choose_car_menu_item(const int item) {
    if (state_->session) state_->session->choose_car_menu_item(item);
}

void GameShell::uncouple_driven(const std::size_t car) {
    if (state_->session) state_->session->uncouple_driven(car);
}

void GameShell::couple_driven(const bool front) {
    if (state_->session) state_->session->couple_driven(front);
}

void GameShell::drive_unit(const std::size_t unit) {
    if (state_->session) state_->session->drive_unit(unit);
}

void GameShell::stand_at_cab_door(const std::size_t car) {
    if (state_->session) state_->session->stand_at_cab_door(car);
}

void GameShell::stand_at_hand_switch() {
    if (state_->session) state_->session->stand_at_hand_switch();
}

void GameShell::report_hand_switch() const {
    if (state_->session) state_->session->report_hand_switch();
}

void GameShell::stand_at_passage(const std::size_t car) {
    if (state_->session) state_->session->stand_at_passage(car);
}

std::string GameShell::car_menu_report() const {
    return state_->session ? state_->session->car_menu_report() : std::string();
}

void GameShell::close_activity_message() {
    if (state_->session) state_->session->close_activity_message();
}

void GameShell::report_activity() {
    if (state_->session) state_->session->report_activity();
}

std::string GameShell::inspector_report() const {
    return state_->session ? state_->session->inspector_report() : std::string();
}

std::string GameShell::tape_report() const { return state_->session ? state_->session->tape_report() : std::string(); }

std::string GameShell::timetable_report() const {
    return state_->session ? state_->session->timetable_report() : std::string();
}

std::optional<renderer::ShadowFocus> GameShell::shadow_focus() const {
    return state_->session ? state_->session->shadow_focus() : std::nullopt;
}

std::optional<renderer::LightShaftRoom> GameShell::light_shaft_room() const {
    return state_->session ? state_->session->light_shaft_room() : std::nullopt;
}

std::optional<renderer::ShadowFocus> GameShell::lamp_shadow_focus(const viewer::ViewerCamera& camera) const {
    return state_->session ? state_->session->lamp_shadow_focus(camera) : std::nullopt;
}

std::string GameShell::command() const { return state_->menu.command(); }
std::string GameShell::network_command() const { return state_->menu.network_command(); }
std::string GameShell::summary() const { return state_->menu.summary(); }

void GameShell::show_tab(const std::string_view key) { state_->menu.show_tab(key); }

} // namespace openrts::game

