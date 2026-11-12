#include "tape_view.hpp"

#include "key_layout.hpp"
#include "ui_text.hpp"
#include "viewer_state.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string_view>
#include <system_error>

#ifdef OPENRTS_HAS_IMGUI
#include <imgui.h>
#endif

namespace openrts::viewer {
namespace {

namespace fs = std::filesystem;

[[nodiscard]] std::string utf8(const fs::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

constexpr std::string_view tape_suffix = "_trainlog_rts.txt";

// "2026.11.11_22_07_09_vl10k_trainlog_rts.txt": "2026.11.11 22:07:09  vl10k".
[[nodiscard]] std::string entry_label(std::string name) {
    if (name.size() > tape_suffix.size() && name.ends_with(tape_suffix)) name.resize(name.size() - tape_suffix.size());
    if (name.size() > 20 && name[10] == '_' && name[13] == '_' && name[16] == '_' && name[19] == '_') {
        return name.substr(0, 10) + " " + name.substr(11, 2) + ":" + name.substr(14, 2) + ":" + name.substr(17, 2) + "  " +
               name.substr(20);
    }
    return name;
}

// Seconds into the day as RTS's clock shows them.
[[nodiscard]] std::string clock_text(const std::int32_t t, const bool seconds = true) {
    const int day = static_cast<int>(((t % 86400) + 86400) % 86400);
    char text[16];
    if (seconds) {
        std::snprintf(text, sizeof text, "%02d:%02d:%02d", day / 3600, day % 3600 / 60, day % 60);
    } else {
        std::snprintf(text, sizeof text, "%02d:%02d", day / 3600, day % 3600 / 60);
    }
    return text;
}

[[nodiscard]] std::string duration_text(const std::size_t seconds) {
    char text[24];
    if (seconds >= 3600) {
        std::snprintf(text, sizeof text, "%zu:%02zu:%02zu", seconds / 3600, seconds % 3600 / 60, seconds % 60);
    } else {
        std::snprintf(text, sizeof text, "%zu:%02zu", seconds / 60, seconds % 60);
    }
    return text;
}

// The aspect as lenta.xls's SignalType names it (RTS's SignalAspect: 1
// STOP_AND_PROCEED КЖ, 2 RESTRICTING Б, 3 and 5 Ж, 4 Ж-Ж, 6 and 7 З, 8 К).
// 0 is STOP or no signal: the line does not tell them apart.
[[nodiscard]] const char* aspect_name(const std::uint32_t aspect) {
    switch (aspect) {
    case 0: return tr("R/-");
    case 1: return tr("RY");
    case 2: return tr("W");
    case 3:
    case 5: return tr("Y");
    case 4: return tr("YY");
    case 6:
    case 7: return tr("G");
    case 8: return tr("R");
    default: return "?";
    }
}

} // namespace

void TapeView::scan(const fs::path& rts_root) {
    scanned_ = true;
    entries_.clear();
    const auto list = [this](const fs::path& folder, const bool rts) {
        std::error_code error;
        for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
            std::error_code kind;
            if (!it->is_regular_file(kind)) continue;
            const std::string name = utf8(it->path().filename());
            if (!name.ends_with(tape_suffix)) continue;
            entries_.push_back({it->path(), entry_label(name), rts});
        }
    };
    const fs::path own = user_data_directory() / "RTS_TAPES";
    list(own, false);
    if (!rts_root.empty()) {
        std::error_code error;
        const fs::path theirs = rts_root / "RTS_TAPES";
        if (!fs::equivalent(theirs, own, error)) list(theirs, true);
    }
    // The newest first: the names start with the date.
    std::stable_sort(entries_.begin(), entries_.end(),
                     [](const Entry& a, const Entry& b) { return utf8(a.path.filename()) > utf8(b.path.filename()); });
}

void TapeView::select(const fs::path& path) {
    reader_.emplace(path);
    summarized_ = SIZE_MAX;
    hovered_ = SIZE_MAX;
    refreshed_at_ = -1e9;
    refresh();
    // The tape rolling from its end; another one whole.
    follow_ = !rolling_.empty() && path == rolling_;
    fit_ = !follow_;
    offset_ = 0.0;
}

void TapeView::refresh() {
    if (!reader_) return;
    reader_->refresh();
    const auto& records = reader_->records();
    if (records.size() == summarized_) return;
    distances_ = simulation::tape_distances(records);
    summary_ = simulation::summarize_tape(records);
    summarized_ = records.size();
}

double TapeView::axis_at(const std::size_t line) const {
    if (axis_ == Axis::time) return static_cast<double>(line);
    return line < distances_.size() ? distances_[line] : 0.0;
}

double TapeView::axis_end() const {
    if (!reader_ || reader_->records().empty()) return 0.0;
    return axis_at(reader_->records().size() - 1);
}

std::size_t TapeView::line_at(const double axis) const {
    const std::size_t count = reader_ ? reader_->records().size() : 0;
    if (count == 0) return SIZE_MAX;
    if (axis_ == Axis::time) {
        const double line = std::round(axis);
        return line <= 0.0 ? 0 : (std::min)(static_cast<std::size_t>(line), count - 1);
    }
    const auto it = std::lower_bound(distances_.begin(), distances_.end(), axis);
    if (it == distances_.end()) return count - 1;
    auto line = static_cast<std::size_t>(it - distances_.begin());
    if (line > 0 && axis - distances_[line - 1] < distances_[line] - axis) --line;
    return line;
}

std::string TapeView::report() const {
    if (!reader_) return "no tape";
    char text[512];
    std::snprintf(text, sizeof text, "%s: %zu lines (%zu not read), path %.1f m, top %.1f km/h, stops %zu, overspeeds %zu%s",
                  utf8(reader_->path().filename()).c_str(), reader_->records().size(), reader_->skipped(), summary_.distance,
                  summary_.top_speed * 3.6, summary_.stops.size(), summary_.overspeeds.size(),
                  !rolling_.empty() && reader_->path() == rolling_ ? ", rolling" : "");
    return text;
}

void TapeView::draw(const fs::path& rolling, const fs::path& rts_root) {
    // A new tape is followed when the window showed the one rolling (or none).
    if (rolling != rolling_) {
        const bool on_rolling = !reader_ || (!rolling_.empty() && reader_->path() == rolling_);
        rolling_ = rolling;
        scanned_ = false;
        if (!rolling.empty() && on_rolling) select(rolling);
    }
    if (rts_root != rts_root_) {
        rts_root_ = rts_root;
        scanned_ = false;
    }
#ifdef OPENRTS_HAS_IMGUI
    if (!open_) return;
    if (!scanned_) scan(rts_root);
    if (!reader_) {
        if (!rolling.empty()) {
            select(rolling);
        } else if (!entries_.empty()) {
            select(entries_.front().path);
        }
    }
    if (reader_ && ImGui::GetTime() - refreshed_at_ >= 0.5) {
        refreshed_at_ = ImGui::GetTime();
        refresh();
    }

    ImGui::SetNextWindowSize(ImVec2(1150.0f, 640.0f), ImGuiCond_FirstUseEver);
    // The wheel is the paper's (its scale), not the window's.
    if (!ImGui::Begin(tr("Speed recorder tape (Ctrl+F9)###tape_view"), &open_,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End();
        return;
    }

    // Which tape, how it runs.
    const auto entry_text = [this](const Entry& e) {
        std::string text = e.label;
        if (e.rts) text += " (RTS)";
        if (!rolling_.empty() && e.path == rolling_) text += tr(" (rolling)");
        return text;
    };
    std::string preview = tr("(none)");
    if (reader_) {
        preview = entry_label(utf8(reader_->path().filename()));
        for (const auto& e : entries_)
            if (e.path == reader_->path()) preview = entry_text(e);
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 26.0f);
    if (ImGui::BeginCombo(tr("Tape"), preview.c_str())) {
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            const bool chosen = reader_ && reader_->path() == entries_[i].path;
            if (ImGui::Selectable(entry_text(entries_[i]).c_str(), chosen)) select(entries_[i].path);
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Refresh the list"))) scan(rts_root);
    ImGui::SameLine();
    if (ImGui::RadioButton(tr("by the path"), axis_ == Axis::path) && axis_ != Axis::path) {
        axis_ = Axis::path;
        fit_ = !follow_;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton(tr("by the time"), axis_ == Axis::time) && axis_ != Axis::time) {
        axis_ = Axis::time;
        fit_ = !follow_;
    }
    ImGui::SameLine();
    ImGui::Checkbox(tr("follow the end"), &follow_);
    ImGui::SameLine();
    if (ImGui::Button(tr("Whole tape"))) {
        fit_ = true;
        follow_ = false;
    }

    if (!reader_) {
        ImGui::TextWrapped("%s", trf("No tapes yet: %s starts one on the locomotive driven; the files are in %s.", tape_key_name(),
                                     utf8(user_data_directory() / "RTS_TAPES").c_str())
                                     .c_str());
        ImGui::End();
        return;
    }
    if (!reader_->readable()) {
        ImGui::TextWrapped("%s", trf("The file cannot be read: %s", utf8(reader_->path()).c_str()).c_str());
        ImGui::End();
        return;
    }

    // What its reading starts with.
    std::string summary = trf("path %.2f km, time %s, top speed %.0f km/h, stops: %zu", summary_.distance / 1000.0,
                              duration_text(summary_.seconds).c_str(), summary_.top_speed * 3.6, summary_.stops.size());
    summary += ", ";
    summary += summary_.overspeeds.empty()
                   ? std::string(tr("above the limit: none"))
                   : trf("above the limit: %zu times, by %.1f km/h at most", summary_.overspeeds.size(), summary_.worst_overspeed * 3.6);
    if (reader_->skipped() > 0) summary += ", " + trf("lines not read: %zu", reader_->skipped());
    ImGui::TextUnformatted(summary.c_str());

    // The inks, named.
    const auto legend = [](const ImVec4& colour, const char* name) {
        ImGui::TextColored(colour, "%s", name);
        ImGui::SameLine();
    };
    legend(ImVec4(0.22f, 0.14f, 0.51f, 1.0f), tr("speed"));
    legend(ImVec4(0.80f, 0.14f, 0.14f, 1.0f), tr("limit"));
    legend(ImVec4(0.95f, 0.0f, 0.0f, 1.0f), tr("above the limit"));
    legend(ImVec4(0.10f, 0.33f, 0.69f, 1.0f), tr("brake pipe"));
    legend(ImVec4(0.67f, 0.29f, 0.08f, 1.0f), tr("brake cylinder"));
    legend(ImVec4(0.45f, 0.45f, 0.45f, 1.0f), tr("equalising reservoir"));
    ImGui::TextDisabled("%s", tr("wheel: scale; drag: scroll; double click: the whole tape; below: the line under the cursor (else the last)"));

    draw_tape();
    const auto& records = reader_->records();
    if (!records.empty()) draw_values(hovered_ < records.size() ? hovered_ : records.size() - 1);
    ImGui::End();
#endif
}

#ifdef OPENRTS_HAS_IMGUI
namespace {

// One ink over the lines: in a pixel column a stroke from the least to the
// most there (a standing train by the path, a long tape seen whole); a NaN
// lifts the pen. Drawn in pieces ImGui's 16-bit indices hold.
class Pen {
public:
    Pen(ImDrawList* list, const ImU32 colour, const float thickness) : list_(list), colour_(colour), thickness_(thickness) {}
    ~Pen() { finish(); }
    Pen(const Pen&) = delete;
    Pen& operator=(const Pen&) = delete;

    void to(const float x, const float y) {
        if (!std::isfinite(y)) {
            lift();
            return;
        }
        const int column = static_cast<int>(std::floor(x));
        if (open_ && column == column_) {
            low_ = (std::min)(low_, y);
            high_ = (std::max)(high_, y);
            last_ = y;
            ++count_;
            return;
        }
        close_column();
        open_ = true;
        column_ = column;
        x_ = x;
        first_ = low_ = high_ = last_ = y;
        count_ = 1;
    }
    void lift() {
        close_column();
        stroke();
        points_.clear();
    }
    void finish() { lift(); }

private:
    void close_column() {
        if (!open_) return;
        open_ = false;
        put({x_, first_});
        if (count_ > 1) {
            put({x_, low_});
            put({x_, high_});
            put({x_, last_});
        }
    }
    void put(const ImVec2 p) {
        if (!points_.empty() && points_.back().x == p.x && points_.back().y == p.y) return;
        points_.push_back(p);
        if (points_.size() >= 1000) {
            stroke();
            const ImVec2 last = points_.back();
            points_.clear();
            points_.push_back(last);
        }
    }
    void stroke() {
        if (points_.size() >= 2) {
            list_->AddPolyline(points_.data(), static_cast<int>(points_.size()), colour_, ImDrawFlags_None, thickness_);
        } else if (points_.size() == 1) {
            list_->AddCircleFilled(points_.front(), thickness_, colour_, 6);
        }
    }

    ImDrawList* list_;
    ImU32 colour_;
    float thickness_;
    std::vector<ImVec2> points_;
    bool open_ = false;
    int column_ = 0;
    float x_{}, first_{}, low_{}, high_{}, last_{};
    std::size_t count_ = 0;
};

// The paper and its inks.
constexpr ImU32 paper = IM_COL32(246, 240, 224, 255);
constexpr ImU32 grid_minor = IM_COL32(228, 210, 200, 255);
constexpr ImU32 grid_major = IM_COL32(206, 165, 152, 255);
constexpr ImU32 print = IM_COL32(95, 70, 60, 255);
constexpr ImU32 ink_speed = IM_COL32(56, 36, 130, 255);
constexpr ImU32 ink_limit = IM_COL32(205, 36, 36, 255);
constexpr ImU32 ink_over = IM_COL32(242, 0, 0, 255);
constexpr ImU32 ink_time = IM_COL32(45, 45, 45, 255);
constexpr ImU32 ink_pipe = IM_COL32(26, 84, 176, 255);
constexpr ImU32 ink_cylinder = IM_COL32(170, 74, 20, 255);
constexpr ImU32 ink_equalising = IM_COL32(115, 115, 115, 255);
constexpr ImU32 cursor_line = IM_COL32(0, 0, 0, 120);

struct Lamp {
    ImU32 top, bottom;
};
[[nodiscard]] Lamp lamp(const std::uint32_t aspect) {
    constexpr ImU32 green = IM_COL32(40, 160, 70, 255), yellow = IM_COL32(236, 190, 30, 255), red = IM_COL32(212, 40, 40, 255),
                    white = IM_COL32(252, 252, 252, 255), none = IM_COL32(222, 206, 200, 255), other = IM_COL32(190, 190, 190, 255);
    switch (aspect) {
    case 0: return {none, none};
    case 1: return {red, yellow};
    case 2: return {white, white};
    case 3:
    case 4:
    case 5: return {yellow, yellow};
    case 6:
    case 7: return {green, green};
    case 8: return {red, red};
    default: return {other, other};
    }
}

// A step for the marks: the first that leaves `pixels` between them.
[[nodiscard]] double mark_step(const double scale, const double pixels, const std::initializer_list<double> steps) {
    for (const double step : steps)
        if (step * scale >= pixels) return step;
    return *(steps.end() - 1);
}

} // namespace

void TapeView::draw_tape() {
    const auto& records = reader_->records();
    const float line_height = ImGui::GetTextLineHeightWithSpacing();
    ImVec2 size = ImGui::GetContentRegionAvail();
    size.y -= line_height * 3.0f + ImGui::GetStyle().ItemSpacing.y;
    size.x = (std::max)(size.x, 200.0f);
    size.y = (std::max)(size.y, 260.0f);
    const ImVec2 corner = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##tape_paper", size, ImGuiButtonFlags_MouseButtonLeft);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    auto& io = ImGui::GetIO();

    const float font = ImGui::GetFontSize();
    const float margin = font * 3.6f;
    const float left = corner.x + margin;
    const float right = corner.x + size.x - 6.0f;
    const float width = (std::max)(right - left, 50.0f);

    // Where it is: the follow, the fit, the wheel and the drag.
    const double end = axis_end();
    double& px = scale();
    const double least = axis_ == Axis::path ? 1e-4 : 1e-3;
    const double most = axis_ == Axis::path ? 50.0 : 60.0;
    if (fit_ && !records.empty()) {
        fit_ = false;
        px = std::clamp(width * 0.98 / (std::max)(end, 1.0), least, most);
        offset_ = 0.0;
    }
    if (hovered && io.MouseWheel != 0.0f) {
        const double at = offset_ + (io.MousePos.x - left) / px;
        px = std::clamp(px * std::pow(1.25, static_cast<double>(io.MouseWheel)), least, most);
        offset_ = at - (io.MousePos.x - left) / px;
    }
    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f) && io.MouseDelta.x != 0.0f) {
        offset_ -= io.MouseDelta.x / px;
        follow_ = false;
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        fit_ = true;
        follow_ = false;
    }
    const double visible = width / px;
    if (follow_) offset_ = (std::max)(0.0, end - visible * 0.92);
    offset_ = std::clamp(offset_, -visible * 0.5, (std::max)(end - visible * 0.1, 0.0));
    const auto x_of = [&](const std::size_t line) { return static_cast<float>(left + (axis_at(line) - offset_) * px); };

    // The lanes, top to bottom.
    const float top = corner.y + 4.0f;
    const float ruler = font * 2.0f + 10.0f;
    const float room = size.y - 8.0f - ruler - 18.0f;
    const float speed_top = top, speed_bottom = speed_top + room * 0.50f;
    const float time_top = speed_bottom + 6.0f, time_bottom = time_top + room * 0.11f;
    const float signal_top = time_bottom + 4.0f, signal_bottom = signal_top + (std::max)(room * 0.07f, font + 4.0f);
    const float pressure_top = signal_bottom + 8.0f, pressure_bottom = (std::max)(pressure_top + 30.0f, corner.y + size.y - 4.0f - ruler);
    const float ruler_top = pressure_bottom + 2.0f;

    ImDrawList* list = ImGui::GetWindowDrawList();
    list->PushClipRect(corner, ImVec2(corner.x + size.x, corner.y + size.y), true);
    list->AddRectFilled(corner, ImVec2(corner.x + size.x, corner.y + size.y), paper);

    // The speed's scale: the 3СЛ-2М's 150 km/h, more when the tape went faster.
    const double top_kmh = summary_.top_speed * 3.6 > 145.0 ? 220.0 : 150.0;
    const auto speed_y = [&](const double kmh) {
        return static_cast<float>(speed_bottom - (speed_bottom - speed_top) * (std::clamp(kmh, 0.0, top_kmh) / top_kmh));
    };
    constexpr double top_atm = 7.0;
    const auto pressure_y = [&](const double atm) {
        return static_cast<float>(pressure_bottom - (pressure_bottom - pressure_top) * (std::clamp(atm, 0.0, top_atm) / top_atm));
    };
    const float label_x = corner.x + 3.0f;
    const auto scale_label = [&](const float y, const char* text) {
        list->AddText(ImVec2(label_x, y - font * 0.5f), print, text);
    };
    char text[64];
    for (int kmh = 0; kmh <= static_cast<int>(top_kmh); kmh += 10) {
        const float y = speed_y(kmh);
        list->AddLine(ImVec2(left, y), ImVec2(right, y), kmh % 50 == 0 ? grid_major : grid_minor);
        if (kmh % (top_kmh > 150.0 ? 40 : 20) == 0 && (speed_bottom - speed_top) > font * 4.0f) {
            std::snprintf(text, sizeof text, "%d", kmh);
            scale_label(y, text);
        }
    }
    for (int atm = 0; atm <= static_cast<int>(top_atm); ++atm) {
        const float y = pressure_y(atm);
        list->AddLine(ImVec2(left, y), ImVec2(right, y), atm % 5 == 0 ? grid_major : grid_minor);
        if ((pressure_bottom - pressure_top) > font * (atm % 2 == 0 ? 3.5f : 7.0f)) {
            std::snprintf(text, sizeof text, "%d", atm);
            scale_label(y, text);
        }
    }
    list->AddRect(ImVec2(left, time_top), ImVec2(right, time_bottom), grid_major);
    list->AddRect(ImVec2(left, signal_top), ImVec2(right, signal_bottom), grid_major);
    list->AddText(ImVec2(left + 4.0f, speed_top + 1.0f), print, tr("km/h"));
    list->AddText(ImVec2(label_x, time_top + 1.0f), print, tr("time"));
    list->AddText(ImVec2(label_x, signal_top + (signal_bottom - signal_top - font) * 0.5f), print, tr("cab signal"));
    list->AddText(ImVec2(left + 4.0f, pressure_top + 1.0f), print, tr("kgf/cm2"));

    if (records.empty()) {
        list->AddText(ImVec2(left + 8.0f, speed_top + (speed_bottom - speed_top) * 0.5f), print, tr("no lines yet"));
        list->PopClipRect();
        hovered_ = SIZE_MAX;
        return;
    }

    // The lines in view, one more each side.
    const std::size_t count = records.size();
    std::size_t first = line_at(offset_);
    std::size_t last = line_at(offset_ + visible);
    first = first > 0 ? first - 1 : 0;
    last = (std::min)(last + 1, count - 1);
    if (axis_ == Axis::path) {
        // A standing at either edge: all of it.
        while (first > 0 && distances_[first - 1] >= offset_ - 1.0 / px) --first;
        while (last + 1 < count && distances_[last + 1] <= offset_ + visible + 1.0 / px) ++last;
    }

    // The marks under the paper: the path (from the tape's start) or the
    // game clock, a line across the lanes each; the kilometre posts.
    const float mark_y = ruler_top + 2.0f;
    const float post_y = mark_y + font + 3.0f;
    if (axis_ == Axis::path) {
        const double step = mark_step(px, font * 6.0, {10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000, 100000, 200000, 500000});
        for (double at = std::ceil(offset_ / step) * step; at <= offset_ + visible; at += step) {
            if (at < 0.0) continue;
            const float x = static_cast<float>(left + (at - offset_) * px);
            list->AddLine(ImVec2(x, speed_top), ImVec2(x, pressure_bottom + 3.0f), grid_minor);
            if (step < 1000.0) {
                std::snprintf(text, sizeof text, tr("%.0f m"), at);
            } else if (std::fmod(step, 1000.0) == 0.0) {
                std::snprintf(text, sizeof text, tr("%.0f km"), at / 1000.0);
            } else {
                std::snprintf(text, sizeof text, tr("%.1f km"), at / 1000.0);
            }
            list->AddText(ImVec2(x + 2.0f, mark_y), print, text);
        }
    } else {
        const double step = mark_step(px, font * 6.0, {10, 30, 60, 120, 300, 600, 1200, 1800, 3600, 7200, 14400, 28800});
        const auto every = static_cast<std::int32_t>(step);
        for (std::size_t i = first; i <= last; ++i) {
            const std::int32_t t = records[i].game_time;
            if (((t % every) + every) % every != 0) continue;
            if (i > 0 && records[i - 1].game_time == t) continue;
            const float x = x_of(i);
            list->AddLine(ImVec2(x, speed_top), ImVec2(x, pressure_bottom + 3.0f), grid_minor);
            list->AddText(ImVec2(x + 2.0f, mark_y), print, clock_text(t, step < 60.0).c_str());
        }
    }
    {
        float free_from = -1e9f;
        for (std::size_t i = (std::max)(first, std::size_t{1}); i <= last; ++i) {
            const float a = records[i - 1].ordinate, b = records[i].ordinate;
            if (a < 0.0f || b < 0.0f || std::floor(a) == std::floor(b)) continue;
            const float x = x_of(i);
            list->AddLine(ImVec2(x, post_y), ImVec2(x, post_y + font), print, 1.5f);
            if (x < free_from) continue;
            std::snprintf(text, sizeof text, tr("%d km"), static_cast<int>(std::floor((std::max)(a, b))));
            list->AddText(ImVec2(x + 3.0f, post_y), print, text);
            free_from = x + ImGui::CalcTextSize(text).x + 8.0f;
        }
    }

    // The cab signal: a ribbon in its lamps' colours, named where it changes.
    {
        float free_from = -1e9f;
        const float mid = (signal_top + signal_bottom) * 0.5f;
        for (std::size_t i = first; i <= last;) {
            std::size_t j = i;
            while (j + 1 <= last && records[j + 1].signal == records[i].signal) ++j;
            const float x0 = x_of(i);
            const float x1 = j + 1 < count ? x_of(j + 1) : x_of(j) + static_cast<float>((axis_ == Axis::path ? std::abs(records[j].velocity) : 1.0) * px);
            const auto colours = lamp(records[i].signal);
            const float x1v = (std::max)(x1, x0 + 1.0f);
            list->AddRectFilled(ImVec2(x0, signal_top + 1.0f), ImVec2(x1v, mid), colours.top);
            list->AddRectFilled(ImVec2(x0, mid), ImVec2(x1v, signal_bottom - 1.0f), colours.bottom);
            if (records[i].signal == 4) list->AddLine(ImVec2(x0, mid), ImVec2(x1v, mid), print);
            const char* name = aspect_name(records[i].signal);
            const float w = ImGui::CalcTextSize(name).x;
            if (x1v - x0 >= w + 6.0f && (std::max)(x0, left) >= free_from) {
                const float at = (std::max)(x0, left) + 3.0f;
                list->AddText(ImVec2(at, signal_top + (signal_bottom - signal_top - font) * 0.5f), print, name);
                free_from = at + w + 4.0f;
            }
            i = j + 1;
        }
    }

    // The time pen: across its lane in ten minutes of the game's clock,
    // then back (by the path a standing draws it up and down in place).
    {
        Pen pen(list, ink_time, 1.2f);
        float free_from = -1e9f;
        for (std::size_t i = first; i <= last; ++i) {
            const std::int32_t t = ((records[i].game_time % 86400) + 86400) % 86400;
            const float x = x_of(i);
            pen.to(x, time_bottom - 2.0f - (time_bottom - time_top - 4.0f) * static_cast<float>(t % 600) / 600.0f);
            if (i > first && t / 600 != (((records[i - 1].game_time % 86400) + 86400) % 86400) / 600 && x >= free_from) {
                const std::string clock = clock_text(t / 600 * 600, false);
                list->AddText(ImVec2(x + 2.0f, time_bottom - font - 1.0f), print, clock.c_str());
                free_from = x + ImGui::CalcTextSize(clock.c_str()).x + 6.0f;
            }
        }
    }

    // The pressures: the brake pipe, the cylinder, the equalising reservoir
    // (RTS 0.44.1's tapes have none: not drawn then).
    bool any_equalising = false;
    for (std::size_t i = first; i <= last && !any_equalising; ++i) any_equalising = records[i].equalising > 0.0f;
    if (any_equalising) {
        Pen pen(list, ink_equalising, 1.0f);
        for (std::size_t i = first; i <= last; ++i) pen.to(x_of(i), pressure_y(records[i].equalising));
    }
    {
        Pen pen(list, ink_cylinder, 1.5f);
        for (std::size_t i = first; i <= last; ++i) pen.to(x_of(i), pressure_y(records[i].cylinder));
    }
    {
        Pen pen(list, ink_pipe, 1.5f);
        for (std::size_t i = first; i <= last; ++i) pen.to(x_of(i), pressure_y(records[i].train_pipe));
    }

    // The limit in force (in steps; off the scale, as with no limit, not
    // drawn), the speed, and in red where it went above the limit.
    {
        Pen pen(list, ink_limit, 1.3f);
        const auto limit_y = [&](const std::size_t i) {
            const double kmh = records[i].speed_limit * 3.6;
            return kmh > top_kmh ? std::nanf("") : speed_y(kmh);
        };
        for (std::size_t i = first; i <= last; ++i) {
            const float x = x_of(i);
            if (i > first && records[i].speed_limit != records[i - 1].speed_limit) pen.to(x, limit_y(i - 1));
            pen.to(x, limit_y(i));
        }
    }
    {
        Pen pen(list, ink_speed, 1.6f);
        for (std::size_t i = first; i <= last; ++i) pen.to(x_of(i), speed_y(std::abs(records[i].velocity) * 3.6));
    }
    for (const auto& run : summary_.overspeeds) {
        if (run.first > last || run.first + run.lines <= first) continue;
        Pen pen(list, ink_over, 2.6f);
        const std::size_t to = (std::min)(run.first + run.lines - 1, last);
        for (std::size_t i = (std::max)(run.first, first); i <= to; ++i) pen.to(x_of(i), speed_y(std::abs(records[i].velocity) * 3.6));
    }
    // The stops, with how long.
    {
        float free_from = -1e9f;
        for (const auto& run : summary_.stops) {
            if (run.first > last || run.first + run.lines <= first) continue;
            const float x = axis_ == Axis::path ? x_of(run.first) : (x_of(run.first) + x_of(run.first + run.lines - 1)) * 0.5f;
            list->AddLine(ImVec2(x, speed_bottom - font * 1.6f), ImVec2(x, speed_bottom), print, 1.5f);
            if (x < free_from) continue;
            const std::string label = trf("standing %s", duration_text(run.lines).c_str());
            list->AddText(ImVec2(x + 3.0f, speed_bottom - font * 1.4f), print, label.c_str());
            free_from = x + ImGui::CalcTextSize(label.c_str()).x + 8.0f;
        }
    }

    // The line under the cursor.
    hovered_ = SIZE_MAX;
    if (hovered && io.MousePos.x >= left && io.MousePos.x <= right) {
        hovered_ = line_at(offset_ + (io.MousePos.x - left) / px);
        if (hovered_ < count) {
            const float x = x_of(hovered_);
            list->AddLine(ImVec2(x, speed_top), ImVec2(x, pressure_bottom), cursor_line, 1.0f);
        }
    }
    list->AddLine(ImVec2(left, speed_top), ImVec2(left, pressure_bottom), grid_major);
    list->PopClipRect();
}

void TapeView::draw_values(const std::size_t line) const {
    const auto& records = reader_->records();
    const auto& r = records[line];
    const auto join = [](std::string& to, const std::string& part) {
        if (!to.empty()) to += ", ";
        to += part;
    };
    std::string first = trf("%s (computer %s)", clock_text(r.game_time).c_str(),
                            clock_text(r.clock_hour * 3600 + r.clock_minute * 60 + r.clock_second).c_str());
    join(first, trf("path %.3f km", (line < distances_.size() ? distances_[line] : 0.0) / 1000.0));
    if (r.ordinate >= 0.0f) join(first, trf("kilometre %.3f", static_cast<double>(r.ordinate)));
    join(first, trf("line %zu of %zu", line + 1, records.size()));

    std::string second = trf("speed %.1f km/h", std::abs(r.velocity) * 3.6);
    join(second, r.speed_limit >= 99.9f ? std::string(tr("no limit")) : trf("limit %.0f km/h", r.speed_limit * 3.6));
    join(second, trf("signal %s, %.0f m to it", aspect_name(r.signal), static_cast<double>(r.signal_distance)));
    join(second, trf("throttle %u", r.throttle));
    join(second, trf("sand %.2f", static_cast<double>(r.sanding)));
    join(second, tr((r.pantographs & 3u) == 3u ? "pantographs 1 and 2 up"
                    : (r.pantographs & 1u) != 0 ? "pantograph 1 up"
                    : (r.pantographs & 2u) != 0 ? "pantograph 2 up"
                                                : "pantographs down"));

    std::string third = trf("BP %.2f, BC %.2f, independent %.2f, ER %.2f kgf/cm2", static_cast<double>(r.train_pipe),
                            static_cast<double>(r.cylinder), static_cast<double>(r.independent), static_cast<double>(r.equalising));
    join(third, tr(r.ep ? "EP brake on" : "EP brake off"));
    join(third, trf("consumption %.2f", static_cast<double>(r.consumption)));
    join(third, trf("%.0f t, %u cars, %u locomotives, %.1f m", r.mass / 1000.0, r.cars, r.locomotives, r.length));

    ImGui::TextUnformatted(first.c_str());
    ImGui::TextUnformatted(second.c_str());
    ImGui::TextUnformatted(third.c_str());
}
#endif

} // namespace openrts::viewer
