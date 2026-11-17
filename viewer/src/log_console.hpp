#pragma once

// The game's messages in a window (F2, «Консоль»): every line written to
// std::cout and std::cerr (what goes to openrts.log) kept as well, the last
// 5000, each marked an error, a warning or neither by its words; the window
// shows them filtered, coloured, to copy.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace openrts::viewer {

enum class LogLevel : std::uint8_t { info, warning, error };

struct LogLine {
    double seconds = 0.0; // since the capture began
    LogLevel level = LogLevel::info;
    std::string text;
};

// From now on std::cout's and std::cerr's lines are kept too, still written
// where they went. Once (later calls do nothing); the first thing a program
// does, before other threads write.
void start_log_capture();

// The kept lines from number `from` on (numbers go on counting past the
// lines dropped and cleared): `first` the first one's number, `next` the
// number the next line will take.
struct LogLines {
    std::vector<LogLine> lines;
    std::uint64_t first = 0;
    std::uint64_t next = 0;
};
[[nodiscard]] LogLines log_lines_from(std::uint64_t from);
// Lines, warnings and errors since the start (clearing does not reset them).
struct LogCounts {
    std::uint64_t lines = 0, warnings = 0, errors = 0;
};
[[nodiscard]] LogCounts log_counts();
// Forgets the kept lines.
void clear_log();
// An error or a warning by its words (English as the log writes them, some Russian).
[[nodiscard]] LogLevel classify_log_line(std::string_view text);

// The window (ImGui): open or not, what it shows, the lines it has.
struct LogConsole {
    bool open = false;
    int shown = 0; // 0 everything, 1 warnings and errors, 2 errors
    char filter[160]{};
    bool follow = true; // scrolled to the newest line
    std::vector<LogLine> lines;
    std::uint64_t next = 0;
};
// F2 opens and closes it; drawn when open.
void draw_log_console(LogConsole& console);

} // namespace openrts::viewer
