#pragma once

// The start menu of openrts_game, in tabs:
// - Single player: a route, a consist and its path, the time of day, season,
//   weather and how the train starts;
// - Network game: a server's address and the nick (the route, the time and
//   the train are the server's); the place for an own server, not there yet;
// - Settings: the RTS folder, language, keys, sound, the log;
// - Graphics: presets, then every graphics setting of the game's panel in
//   groups, each with its load on the processor and the video card.
// All kept in viewer_state.ini. "Start" / "Connect" runs the game
// (openrts_viewer) as a process of its own; the menu waits for it and comes
// back. A network game's steps (connecting, waiting for a train, loading)
// are read from its "status:" lines in openrts.log and shown here.

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "viewer_scene.hpp"
#include "brake_settings_ui.hpp"

namespace openrts::viewer {
struct WindowOptions;
}

namespace openrts::game {

// What the menu asks the shell to start: the single player's route, train and
// world. The shell builds the scene on a worker and attaches it to the one
// window; the menu stays attached underneath.
struct SessionRequest {
    std::filesystem::path root;
    std::string route;
    std::string consist;
    std::string path;
    std::filesystem::path situation;
    std::filesystem::path save;
    std::string activity;
    int start_state = -1; // -1 as it is, 1 switched on, 0 cold
    int start_view = 1;   // viewer's --spawn: 0 as before, 1 cab, 2 car, 3 above
    int minutes = 12 * 60;
    int season = 1;
    int weather = 0;
    bool realistic_air = false;
    // The brakes' preset of the start menu (brake_settings_ui.hpp): applied to
    // the scene before its first train, so a Brakes.dll consist starts on the
    // devices the settings name.
    viewer::BrakeSettings brakes;
    bool sound = true;
    float volume = 0.5f;
    std::string title;
};

class Launcher final : public viewer::ViewerScene {
public:
    // `exe_dir`: where openrts_game.exe is (the game is looked for next to it).
    explicit Launcher(std::filesystem::path exe_dir);
    ~Launcher() override;

    [[nodiscard]] std::string_view title() const override { return "DTRE"; }
    [[nodiscard]] std::string attach(const viewer::SceneGpu&, viewer::ViewerCamera&) override;
    void detach() override;
    void update(renderer::ModelRenderer&, viewer::ViewerCamera&, const viewer::SceneClock&, const viewer::SceneInput&) override;
    [[nodiscard]] viewer::SceneEnvironment environment() const override;
    void submit(renderer::ModelRenderer&, const viewer::FrameView&) override {}
    void draw_overlay(const viewer::ViewerCamera&) const override;
    // The game's command line for the single player choice and for joining
    // the server (empty: not ready), and what the menu read (for
    // --print-command).
    [[nodiscard]] std::string command() const;
    [[nodiscard]] std::string network_command() const;
    [[nodiscard]] std::string summary() const;
    // Opens a tab by its key: single, network, settings.
    void show_tab(std::string_view key);
    // The start's window and renderer choices (MSAA, shading path, HDR,
    // window mode, sound) before run_window_viewer creates them.
    void apply_window_options(viewer::WindowOptions& options) const;
    // What the menu asked to play: the shell takes it once.
    [[nodiscard]] std::optional<SessionRequest> take_session_request();
    // The session is over (or failed): the menu is the home page again and
    // the lists are read anew (a save made in the game shows up).
    void session_finished(const std::string& note = {});
    void session_failed(const std::string& error);
    // The page Esc returns to when the shell's pause menu closes.
    void show_home();
    // The full settings page, drawn on top of a running (paused) session.
    // Returns true when its back button or Esc asks to return to the pause menu.
    bool draw_embedded_settings();
    // The Discord profile: "in the menu"; nothing while the game runs (the
    // game says its own).
    [[nodiscard]] std::optional<viewer::ScenePresence> presence() const override;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace openrts::game
