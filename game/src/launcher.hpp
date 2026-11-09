#pragma once

// The start menu of openrts_game, in tabs:
// - Single player: a route, a consist and its path, the time of day, season,
//   weather and how the train starts;
// At its start it asks GitHub for the latest release (update.hpp) and offers
// it: update, later, or skip that version.
// - Network game: a server's address and the nick (the route, the time and
//   the train are the server's); the place for an own server, not there yet;
// - Settings: the RTS folder, language, keys, sound, the log, updates;
// - Graphics: presets, then every graphics setting of the game's panel in
//   groups, each with its load on the processor and the video card.
// All kept in viewer_state.ini. "Start" / "Connect" runs the game
// (openrts_viewer) as a process of its own; the menu waits for it and comes
// back. A network game's steps (connecting, waiting for a train, loading)
// are read from its "status:" lines in openrts.log and shown here.

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "viewer_scene.hpp"

namespace openrts::game {

class Launcher final : public viewer::ViewerScene {
public:
    // `exe_dir`: where openrts_game.exe is (the game is looked for next to it).
    explicit Launcher(std::filesystem::path exe_dir);
    ~Launcher() override;

    [[nodiscard]] std::string_view title() const override { return "openRTS"; }
    [[nodiscard]] std::string attach(const viewer::SceneGpu&, viewer::ViewerCamera&) override { return {}; }
    void detach() override {}
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
    // No check for a new version at the start (--capture, --no-update-check).
    void set_update_check(bool on);
    // Started again by an update from `version`: said in the status line.
    void set_updated_from(std::string version);
    // An update is installed: the window closes and the menu is started again.
    [[nodiscard]] bool wants_close() const override;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace openrts::game
