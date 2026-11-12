#pragma once

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The tape (RTS's "tape roll", docs/tape.md): while it rolls, a line for
// each game second about the locomotive it records, in RTS's own text
// format (rts1.exe 0x5050B0), which RTS's lenta.xls (ImportWRLenta) reads.
// RTS starts it with Ctrl+Num5 (0x414B40) and writes
// RTS_TAPES\YYYY.MM.DD_hh_mm_ss_<LocomotiveSeries>_trainlog_rts.txt (0x504FA8).
namespace openrts::simulation {

// One line. Pressures in atm as RTS keeps them (the line has them in psi,
// x 14.69), speeds in m/s, the rest as named.
struct TapeRecord {
    float velocity{};            // m/s, towards the train front (Wagon::Velocity)
    std::int32_t game_time{};    // s since midnight (RTS's clock, RTS_VAR_TIME)
    // The obeyed signal ahead (train +0xA0, set by the signal pass 0x508520):
    // its SignalAspect (0 also when there is none), and the metres to it
    // (train +0x7C; kept from the last signal when there is none).
    std::uint32_t signal{};
    float signal_distance{};
    float train_pipe{}, independent{}, cylinder{}; // Wagon +0x64, +0x34, +0x60
    std::int32_t tile_x{}, tile_z{};
    float x{}, y{}, z{};         // in the tile, MSTS's axes (RTS writes -TrPos.X)
    std::uint32_t throttle{};    // Engine::ThrottlePosition
    std::uint8_t pantographs{};  // bit 0: the first one up, bit 1: the second
    float speed_limit{};         // m/s in force (0x4FB630)
    float sanding{};             // Engine::Sanding
    float consumption{};         // electric: PowerConsuming / 1000; diesel: FuelConsuming
    std::uint16_t clock_hour{}, clock_minute{}, clock_second{}; // the computer's clock
    double mass{};               // kg, every car's Wagon::Mass
    std::uint32_t cars{};
    std::uint32_t locomotives{}; // engines, not cold (LocoFlags bit 3)
    double length{};             // m
    std::uint8_t kind{};         // 3 electric, 2 diesel, 0 other
    std::uint8_t season{};       // 0 spring, 1 summer, 2 autumn, 3 winter
    std::uint8_t weather{};      // 0 clear, 1 snow, 2 rain
    bool ep = false;             // ЭПТ engaged (BrakeSystemsEngaged bit 1)
    float equalising{};          // atm (Engine::UR)
    float ordinate = -1.0f;      // km by the kilometre posts (train +0x70), -1 none
};

// The record's line as RTS writes it, "\r\n" included.
[[nodiscard]] std::string tape_line(const TapeRecord& record);

// The file's name for a tape started at local time `started` on a
// locomotive of `series` (characters Windows refuses in a name become _).
[[nodiscard]] std::string tape_file_name(const std::tm& started, std::string_view series);

// psi per atm in the line (RTS's 14.69).
inline constexpr double tape_psi_per_atm = 14.69;

// A line read back, the fields where lenta.xls's ReadLenta takes them
// (0..44; the equalising reservoir 45 and the kilometre 46 when there, as
// RTS 0.44.1 ended at 44): nullopt when it is not a tape's line. The
// pressures back in atm; the limit from field 26 (44 is the same).
[[nodiscard]] std::optional<TapeRecord> parse_tape_line(std::string_view line);

// A tape file read as it grows (also the one rolling): refresh() reads the
// whole lines added since the last one; a file made anew, shorter than what
// was read, is read again from its start.
class TapeReader {
public:
    explicit TapeReader(std::filesystem::path path) : path_(std::move(path)) {}
    // True when lines came (or the file was read anew).
    bool refresh();
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] const std::vector<TapeRecord>& records() const noexcept { return records_; }
    [[nodiscard]] std::size_t skipped() const noexcept { return skipped_; } // lines not a tape's
    [[nodiscard]] bool readable() const noexcept { return readable_; }      // opened at the last refresh

private:
    std::filesystem::path path_;
    std::vector<TapeRecord> records_;
    std::uint64_t offset_ = 0; // bytes read, to the end of the last whole line
    std::size_t skipped_ = 0;
    bool readable_ = false;
};

// The path at each line (m): the speeds added up, a line a second, as
// lenta.xls's Ордината does, but by their size (the paper of a speed
// recorder runs on whichever way the train goes).
[[nodiscard]] std::vector<double> tape_distances(const std::vector<TapeRecord>& records);

// Lines one after another: from `first`, `lines` of them.
struct TapeRun {
    std::size_t first{};
    std::size_t lines{};
};

// What a tape's reading starts with (openRTS's own figures, not RTS's).
struct TapeSummary {
    double distance{};         // m (tape_distances' last)
    std::size_t seconds{};     // lines
    float top_speed{};         // m/s
    // Standings (below 0.1 m/s) of 10 lines and more after the train moved.
    std::vector<TapeRun> stops;
    // Lines more than 0.5 km/h above the limit in force, and the most
    // above it, m/s.
    std::vector<TapeRun> overspeeds;
    float worst_overspeed{};
};
inline constexpr float tape_standing_speed = 0.1f;      // m/s
inline constexpr std::size_t tape_stop_lines = 10;
inline constexpr float tape_overspeed_margin = 0.5f / 3.6f; // m/s
[[nodiscard]] TapeSummary summarize_tape(const std::vector<TapeRecord>& records);

class TapeRecorder {
public:
    TapeRecorder() = default;
    ~TapeRecorder();
    TapeRecorder(const TapeRecorder&) = delete;
    TapeRecorder& operator=(const TapeRecorder&) = delete;

    // A new file in `folder` (made when missing); false when it cannot be
    // opened (RTS then does not start, 0x414B40). A tape rolling is stopped first.
    bool start(const std::filesystem::path& folder, std::string_view series, const std::tm& started);
    void stop();
    [[nodiscard]] bool recording() const noexcept { return file_ != nullptr; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] std::size_t lines() const noexcept { return lines_; }

    // `dt` game seconds passed; true when a line is due. RTS writes when its
    // count reaches 1 s and sets it back to 0 (0x424488), each line a frame
    // late; openRTS takes the second off, so a line comes each game second
    // (lenta.xls adds the speeds up for the distance, a line a second).
    bool advance(double dt);
    void write(const TapeRecord& record);

private:
    std::FILE* file_ = nullptr;
    std::filesystem::path path_;
    double time_ = 0.0;
    std::size_t lines_ = 0;
};

} // namespace openrts::simulation
