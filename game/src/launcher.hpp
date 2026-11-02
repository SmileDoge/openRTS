#pragma once

// The start menu of openrts_game: the RTS folder, a route, a consist and its
// path, the time of day, season, weather and how the train starts, and the
// settings (language, keys, sound, volume, anti-aliasing, VSync), kept in
// viewer_state.ini. "Start" runs the game (openrts_viewer) as a process of
// its own; the menu waits for it and comes back.

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
    // The game's command line for the current choice (empty: nothing chosen),
    // and what the menu read (for --print-command).
    [[nodiscard]] std::string command() const;
    [[nodiscard]] std::string summary() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace openrts::game
