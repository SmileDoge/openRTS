#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <openrts/simulation/track_profile.hpp>

// The profile strip at the top of the screen (Ctrl+F4; testers' request
// 2026-11-19-4): the track's profile along the train's way ahead and a
// little behind, through the junctions as they are set (simulation/
// track_profile.hpp), its grade by colour (up, down, level), the grade
// under the train, the kilometre posts, the speed limits, the signals and
// the stations on it, and the train as a stroke of its length with its
// mass and cars. The wheel over it changes the span (2, 5, 10, 20 km), a
// drag shifts it, a double click puts it back.
namespace openrts::viewer {

struct ProfileMark {
    enum class Kind : std::uint8_t { kilometre, limit, signal, station, siding };
    Kind kind{};
    double distance{};      // m from the train's leading end, ahead +
    std::string text;       // the kilometre, the speed, the signal's name, the station's
    std::uint32_t colour{}; // a signal's lamp (ImGui's IM_COL32)
};

struct ProfileView {
    const simulation::TrackProfile* profile = nullptr;
    // Its elements of one grade, numbered under the line (a regime map's):
    // the track's own, so neither their ends nor their numbers move as the
    // train comes near; one too short for its number is left unnumbered.
    std::vector<simulation::TrackProfileElement> elements;
    std::vector<ProfileMark> marks;
    double train_length{};  // m behind the leading end
    double mass_tonnes{};
    std::size_t cars{};
};

class ProfileStrip {
public:
    // The way the strip shows now: from `left()` to `right()` m (ahead +).
    [[nodiscard]] double span() const noexcept;
    [[nodiscard]] double left() const noexcept { return -0.12 * span() + shift_; }
    [[nodiscard]] double right() const noexcept { return 0.88 * span() + shift_; }
    // Changed since the last call (the span or the shift): the profile to build again.
    [[nodiscard]] bool take_changed() noexcept;
    // Its height on the screen (px): the panels keep below it.
    [[nodiscard]] static float height();
    void draw(const ProfileView& view);

private:
    int span_index_ = 1; // into 2, 5, 10, 20 km
    double shift_ = 0.0; // m
    bool changed_ = true;
};

} // namespace openrts::viewer
