#include <openrts/simulation/tape.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#include <share.h>
#endif

namespace openrts::simulation {

std::string tape_line(const TapeRecord& r) {
    // RTS's clock into the day (0x5050B0): a negative time wraps, one past
    // a day is taken modulo a day, 86400 itself stays.
    std::int32_t t = r.game_time;
    if (t < 0) {
        t = t % 86400 + 86400;
    } else if (t > 86400) {
        t %= 86400;
    }
    const double psi = tape_psi_per_atm;
    const int signal = static_cast<int>(r.signal);
    char buffer[1024];
    const int n = std::snprintf(
        buffer, sizeof buffer,
        "%0.3f#%i#%i#%i#%i#%i#%0.2f#%0.2f# %0.2f#  %0.3f#%i#%0.2f#  %0.2f#%0.2f#%i#%i#0#0#8#%i#1.00#%i#%i#5#2#0.00#%0.3f#0#"
        "   %0.2f#  %0.2f#%i#%i#%i# %E#%i#%i#1#  %E#%i#%i#%i#%i# 0.00#-4663#%0.3f#%0.3f#%0.3f\r\n",
        static_cast<double>(r.velocity), t / 3600, t % 3600 / 60, t % 3600 % 60, signal, 0,
        r.train_pipe * psi, r.independent * psi, r.cylinder * psi, static_cast<double>(r.signal_distance), signal,
        static_cast<double>(r.x), static_cast<double>(r.y), static_cast<double>(r.z), static_cast<int>(r.tile_x),
        static_cast<int>(r.tile_z), static_cast<int>(r.throttle), (r.pantographs & 1u) != 0 ? 1 : 0,
        (r.pantographs & 2u) != 0 ? 1 : 0, static_cast<double>(r.speed_limit), static_cast<double>(r.sanding),
        static_cast<double>(r.consumption), static_cast<int>(r.clock_hour), static_cast<int>(r.clock_minute),
        static_cast<int>(r.clock_second), r.mass, static_cast<int>(r.cars), static_cast<int>(r.locomotives), r.length,
        static_cast<int>(r.kind), static_cast<int>(r.season), static_cast<int>(r.weather), r.ep ? 1 : 0,
        static_cast<double>(r.speed_limit), r.equalising * psi, static_cast<double>(r.ordinate));
    if (n <= 0) return {};
    return std::string(buffer, static_cast<std::size_t>(n) < sizeof buffer ? static_cast<std::size_t>(n) : sizeof buffer - 1);
}

std::string tape_file_name(const std::tm& started, const std::string_view series) {
    char stamp[64];
    std::snprintf(stamp, sizeof stamp, "%04i.%02i.%02i_%02i_%02i_%02i_", started.tm_year + 1900, started.tm_mon + 1,
                  started.tm_mday, started.tm_hour, started.tm_min, started.tm_sec);
    std::string name = stamp;
    for (const char c : series) {
        const auto u = static_cast<unsigned char>(c);
        const bool refused = u < 0x20 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' ||
                             c == '?' || c == '*';
        name.push_back(refused ? '_' : c);
    }
    name += "_trainlog_rts.txt";
    return name;
}

namespace {

// A field as a number: spaces around it allowed ("%E" and the padded
// fields), nothing else.
[[nodiscard]] bool number(std::string_view field, double& out) {
    while (!field.empty() && (field.front() == ' ' || field.front() == '\t')) field.remove_prefix(1);
    while (!field.empty() && (field.back() == ' ' || field.back() == '\t')) field.remove_suffix(1);
    if (field.empty()) return false;
    if (field.front() == '+') field.remove_prefix(1);
    const auto result = std::from_chars(field.data(), field.data() + field.size(), out);
    return result.ec == std::errc{} && result.ptr == field.data() + field.size() && std::isfinite(out);
}

} // namespace

std::optional<TapeRecord> parse_tape_line(std::string_view line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
    constexpr std::size_t most = 64;
    std::string_view fields[most];
    std::size_t count = 0;
    for (std::size_t start = 0;;) {
        const auto hash = line.find('#', start);
        if (count == most) return std::nullopt;
        fields[count++] = line.substr(start, hash == std::string_view::npos ? std::string_view::npos : hash - start);
        if (hash == std::string_view::npos) break;
        start = hash + 1;
    }
    if (count < 45) return std::nullopt;
    double v[47]{};
    const std::size_t read = (std::min)(count, std::size_t{47});
    for (std::size_t i = 0; i < read; ++i) {
        if (!number(fields[i], v[i])) return std::nullopt;
    }
    const auto whole = [](const double x) { return static_cast<std::int32_t>(std::lround(x)); };
    const auto count_of = [](const double x) { return x > 0.0 ? static_cast<std::uint32_t>(std::lround(x)) : 0u; };
    const double psi = tape_psi_per_atm;
    TapeRecord r;
    r.velocity = static_cast<float>(v[0]);
    r.game_time = whole(v[1]) * 3600 + whole(v[2]) * 60 + whole(v[3]);
    r.signal = count_of(v[4]);
    r.train_pipe = static_cast<float>(v[6] / psi);
    r.independent = static_cast<float>(v[7] / psi);
    r.cylinder = static_cast<float>(v[8] / psi);
    r.signal_distance = static_cast<float>(v[9]);
    r.x = static_cast<float>(v[11]);
    r.y = static_cast<float>(v[12]);
    r.z = static_cast<float>(v[13]);
    r.tile_x = whole(v[14]);
    r.tile_z = whole(v[15]);
    r.throttle = count_of(v[19]);
    r.pantographs = static_cast<std::uint8_t>((v[21] != 0.0 ? 1u : 0u) | (v[22] != 0.0 ? 2u : 0u));
    r.speed_limit = static_cast<float>(v[26]);
    r.sanding = static_cast<float>(v[28]);
    r.consumption = static_cast<float>(v[29]);
    r.clock_hour = static_cast<std::uint16_t>(count_of(v[30]));
    r.clock_minute = static_cast<std::uint16_t>(count_of(v[31]));
    r.clock_second = static_cast<std::uint16_t>(count_of(v[32]));
    r.mass = v[33];
    r.cars = count_of(v[34]);
    r.locomotives = count_of(v[35]);
    r.length = v[37];
    r.kind = static_cast<std::uint8_t>(count_of(v[38]));
    r.season = static_cast<std::uint8_t>(count_of(v[39]));
    r.weather = static_cast<std::uint8_t>(count_of(v[40]));
    r.ep = v[41] != 0.0;
    if (count > 45) r.equalising = static_cast<float>(v[45] / psi);
    r.ordinate = count > 46 ? static_cast<float>(v[46]) : -1.0f;
    return r;
}

bool TapeReader::refresh() {
    std::ifstream in(path_, std::ios::binary);
    readable_ = in.good();
    if (!readable_) return false;
    in.seekg(0, std::ios::end);
    const auto size = static_cast<std::uint64_t>(in.tellg());
    bool changed = false;
    if (size < offset_) {
        // Made anew: read it again.
        records_.clear();
        skipped_ = 0;
        offset_ = 0;
        changed = true;
    }
    if (size == offset_) return changed;
    in.seekg(static_cast<std::streamoff>(offset_));
    std::string text(static_cast<std::size_t>(size - offset_), '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(in.gcount()));
    // Whole lines only: the one being written waits for its end.
    const auto end = text.rfind('\n');
    if (end == std::string::npos) return changed;
    std::string_view rest(text.data(), end + 1);
    while (!rest.empty()) {
        const auto newline = rest.find('\n');
        const auto line = rest.substr(0, newline);
        rest.remove_prefix(newline + 1);
        if (line.empty() || line == "\r") continue;
        if (auto record = parse_tape_line(line)) {
            records_.push_back(*record);
        } else {
            ++skipped_;
        }
    }
    offset_ += end + 1;
    return true;
}

std::vector<double> tape_distances(const std::vector<TapeRecord>& records) {
    std::vector<double> path;
    path.reserve(records.size());
    double sum = 0.0;
    for (const auto& r : records) {
        sum += std::abs(static_cast<double>(r.velocity));
        path.push_back(sum);
    }
    return path;
}

TapeSummary summarize_tape(const std::vector<TapeRecord>& records) {
    TapeSummary s;
    s.seconds = records.size();
    bool moved = false;
    std::size_t standing = 0;
    bool over = false;
    for (std::size_t i = 0; i < records.size(); ++i) {
        const auto& r = records[i];
        const float speed = std::abs(r.velocity);
        s.distance += speed;
        s.top_speed = (std::max)(s.top_speed, speed);
        if (speed < tape_standing_speed) {
            ++standing;
            if (moved && standing == tape_stop_lines) s.stops.push_back({i + 1 - standing, standing});
            if (moved && standing > tape_stop_lines) s.stops.back().lines = standing;
        } else {
            moved = true;
            standing = 0;
        }
        const float above = speed - r.speed_limit;
        if (above > tape_overspeed_margin) {
            if (!over) s.overspeeds.push_back({i, 0});
            ++s.overspeeds.back().lines;
            over = true;
            s.worst_overspeed = (std::max)(s.worst_overspeed, above);
        } else {
            over = false;
        }
    }
    return s;
}

TapeRecorder::~TapeRecorder() { stop(); }

bool TapeRecorder::start(const std::filesystem::path& folder, const std::string_view series, const std::tm& started) {
    stop();
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    const std::string name = tape_file_name(started, series);
    path_ = folder / std::filesystem::path(std::u8string(name.begin(), name.end()));
#ifdef _WIN32
    // Others may read it while it rolls (RTS: FILE_SHARE_READ).
    file_ = _wfsopen(path_.c_str(), L"wb", _SH_DENYWR);
#else
    file_ = std::fopen(path_.c_str(), "wb");
#endif
    time_ = 0.0;
    lines_ = 0;
    return file_ != nullptr;
}

void TapeRecorder::stop() {
    if (file_) std::fclose(file_);
    file_ = nullptr;
    time_ = 0.0;
}

bool TapeRecorder::advance(const double dt) {
    if (!file_) return false;
    time_ += dt;
    if (time_ < 1.0) return false;
    time_ -= 1.0;
    if (time_ >= 1.0) time_ = 0.0; // a long pause: one line, not a burst
    return true;
}

void TapeRecorder::write(const TapeRecord& record) {
    if (!file_) return;
    const std::string line = tape_line(record);
    std::fwrite(line.data(), 1, line.size(), file_);
    // Readable while it rolls (RTS writes each line straight to the file).
    std::fflush(file_);
    ++lines_;
}

} // namespace openrts::simulation
