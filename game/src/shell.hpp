#pragma once

// The one window of the game: the menu, the loading of a single player's
// route and the route itself, all in the same process and the same window.
//
// ViewerScene is what run_window_viewer drives. GameShell is that scene: it
// owns the menu (Launcher) and, while a trip runs, the route's scene. Every
// call of the viewer goes to the menu or to the route (the "session"). A
// trip's scene is built on a worker (VFS, scenario, route) and attached to
// the GPU on the render thread; the shell draws its own loading screen and
// the Esc pause menu, which stops the clock only in a single player's trip.

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "launcher.hpp"
#include "viewer_scene.hpp"

namespace openrts::game {

class GameShell final : public viewer::ViewerScene {
public:
    explicit GameShell(std::filesystem::path exe_dir);
    ~GameShell() override;

    // Starts a trip at once, for the command line (`--play`): the route is a
    // name under ROUTES, the consist a .con's name; empty is none.
    bool play(std::string route, std::string consist = {});
    // For `--play --pause`: the pause menu opens once the route is loaded.
    void pause_when_loaded();
    // The menu's saved window and renderer choices, applied before the one
    // window is made.
    void apply_window_options(viewer::WindowOptions& options) const;

    [[nodiscard]] std::string_view title() const override;
    [[nodiscard]] std::string attach(const viewer::SceneGpu& gpu, viewer::ViewerCamera& camera) override;
    void detach() override;
    void update(renderer::ModelRenderer& renderer, viewer::ViewerCamera& camera, const viewer::SceneClock& clock,
                const viewer::SceneInput& input) override;
    [[nodiscard]] viewer::SceneEnvironment environment() const override;
    void draw_background(WGPURenderPassEncoder pass, const viewer::FrameView& frame) override;
    void submit(renderer::ModelRenderer& renderer, const viewer::FrameView& frame) override;
    void draw_foreground(WGPURenderPassEncoder pass, const viewer::FrameView& frame) override;
    void draw_before_translucent(WGPURenderPassEncoder pass, const viewer::FrameView& frame) override;
    void draw_depth(WGPURenderPassEncoder pass, std::uint32_t slot, const glm::mat4& view_projection,
                    renderer::DepthPassKind kind) override;
    void prepare_depth() override;
    void before_main_pass(WGPUCommandEncoder encoder, const viewer::FrameView& frame) override;
    void draw_after_translucent(WGPURenderPassEncoder pass, const viewer::FrameView& frame) override;
    void draw_bloom_sources(WGPURenderPassEncoder pass, const viewer::FrameView& frame) override;
    [[nodiscard]] WGPUQuerySet occlusion_query_set() const override;
    void after_main_pass(WGPUCommandEncoder encoder) override;
    void frame_submitted() override;
    [[nodiscard]] std::optional<formats::RoutePosition> origin() const override;
    void draw_overlay(const viewer::ViewerCamera& camera) const override;
    [[nodiscard]] bool keyboard_captured() const override;
    void open_track_map() override;
    [[nodiscard]] bool engineer_on_foot() const override;
    [[nodiscard]] int close_code() const override;
    [[nodiscard]] bool escape_taken() const override;
    [[nodiscard]] bool takes_scroll() const override;
    void restore_screen_focus() override;
    void set_cab_switch(std::uint32_t id, std::uint32_t state) override;
    [[nodiscard]] std::optional<std::pair<std::uint32_t, std::uint32_t>> cab_switch_state(std::uint32_t id) const override;
    [[nodiscard]] std::string world_report() const override;
    [[nodiscard]] std::optional<double> take_time_of_day() override;
    [[nodiscard]] std::filesystem::path saves_folder() const override;
    [[nodiscard]] std::vector<std::string> saved_games() const override;
    [[nodiscard]] std::string save_game(const std::string& name) override;
    [[nodiscard]] std::string load_game(const std::filesystem::path& file) override;
    [[nodiscard]] std::optional<viewer::ViewerScene::SavedClock> take_saved_clock() override;
    [[nodiscard]] bool paused() const override;
    bool take_resume_request() override;
    [[nodiscard]] viewer::SceneLoading loading() const override;
    [[nodiscard]] std::optional<viewer::ScenePresence> presence() const override;
    [[nodiscard]] std::optional<glm::mat4> train_car_transform(std::size_t car) const override;
    void enter_cab() override;
    [[nodiscard]] bool seated_in_cab() const override;
    [[nodiscard]] bool has_train_panel() const override;
    void draw_train_panel() const override;
    void choose_car_menu_item(int item) override;
    void uncouple_driven(std::size_t car) override;
    void couple_driven(bool front) override;
    void drive_unit(std::size_t unit) override;
    void stand_at_cab_door(std::size_t car) override;
    void stand_at_hand_switch() override;
    void report_hand_switch() const override;
    void stand_at_passage(std::size_t car) override;
    [[nodiscard]] std::string car_menu_report() const override;
    void close_activity_message() override;
    void report_activity() override;
    [[nodiscard]] std::string inspector_report() const override;
    [[nodiscard]] std::string tape_report() const override;
    [[nodiscard]] std::string timetable_report() const override;
    [[nodiscard]] std::optional<renderer::ShadowFocus> shadow_focus() const override;
    [[nodiscard]] std::optional<renderer::LightShaftRoom> light_shaft_room() const override;
    [[nodiscard]] std::optional<renderer::ShadowFocus> lamp_shadow_focus(const viewer::ViewerCamera& camera) const override;

    // The menu's own API, kept here so that main.cpp talks to one object.
    [[nodiscard]] std::string command() const;
    [[nodiscard]] std::string network_command() const;
    [[nodiscard]] std::string summary() const;
    void show_tab(std::string_view key);

private:
    void draw_loading_overlay() const;
    void draw_pause_overlay() const;
    void draw_pause_settings_overlay() const;

    struct State;
    std::unique_ptr<State> state_;
};

} // namespace openrts::game
