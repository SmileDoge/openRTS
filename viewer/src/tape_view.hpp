#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <openrts/simulation/tape.hpp>

namespace openrts::viewer {

// The tape window (Ctrl+F9, docs/tape.md "The tape window"), openRTS's own
// (RTS has none; its tapes are read in lenta.xls): a tape file drawn as a
// speed recorder's paper, in the manner of the 3СЛ-2М: the speed and the
// limit in force, the time pen's ten-minute teeth, the cab signal, the
// brake pressures, the path and the kilometre posts; below it the values
// of the line under the cursor. It lists the tapes of RTS_TAPES (openRTS's
// folder and the RTS installation's) and reads the one rolling as it grows.
class TapeView {
public:
    void toggle() noexcept { open_ = !open_; }
    void open() noexcept { open_ = true; }
    [[nodiscard]] bool is_open() const noexcept { return open_; }
    // `rolling`: the file the tape writes now (empty: none); `rts_root`:
    // the RTS installation, whose RTS_TAPES is listed too. Draws the window
    // (ImGui builds); follows a new tape when it was on the one rolling.
    void draw(const std::filesystem::path& rolling, const std::filesystem::path& rts_root);
    // The tape shown, as text (--capture-tape).
    [[nodiscard]] std::string report() const;

private:
    struct Entry {
        std::filesystem::path path;
        std::string label;
        bool rts = false; // in the RTS installation's RTS_TAPES
    };
    enum class Axis : unsigned char { path, time };

    void scan(const std::filesystem::path& rts_root);
    void select(const std::filesystem::path& path);
    void refresh();
    void draw_tape();
    void draw_values(std::size_t line) const;
    [[nodiscard]] double axis_at(std::size_t line) const;
    [[nodiscard]] double axis_end() const;
    [[nodiscard]] double& scale() noexcept { return axis_ == Axis::path ? metres_px_ : seconds_px_; }
    [[nodiscard]] std::size_t line_at(double axis) const;

    bool open_ = false;
    bool scanned_ = false;
    std::vector<Entry> entries_;
    std::filesystem::path rolling_;  // the last frame's
    std::filesystem::path rts_root_; // the last frame's
    std::optional<simulation::TapeReader> reader_;
    std::vector<double> distances_;  // simulation::tape_distances of the records
    simulation::TapeSummary summary_{};
    std::size_t summarized_ = SIZE_MAX; // records in distances_ and summary_
    double refreshed_at_ = -1e9;        // ImGui's time
    Axis axis_ = Axis::path;
    double metres_px_ = 0.25; // pixels a metre, by the path (1 km 250 px)
    double seconds_px_ = 1.0; // pixels a second, by the time
    double offset_ = 0.0;     // the left edge, m or s
    bool follow_ = true;      // the end kept in view
    bool fit_ = false;        // the whole tape into the width, next frame
    std::size_t hovered_ = SIZE_MAX;
};

} // namespace openrts::viewer
