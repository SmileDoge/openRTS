#include "profile_strip.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#ifdef OPENRTS_HAS_IMGUI
#include <imgui.h>

#include <openrts/ui/fonts.hpp>
#endif

#include "hud_style.hpp"
#include "ui_text.hpp"

namespace openrts::viewer {

namespace {

constexpr double spans[] = {2000.0, 5000.0, 10000.0, 20000.0};
constexpr int span_count = static_cast<int>(std::size(spans));
// How far a drag may take it from the train: 4 km behind, 30 km ahead.
constexpr double shift_back = -4000.0, shift_ahead = 30000.0;

#ifdef OPENRTS_HAS_IMGUI
ImU32 mix(const int r0, const int g0, const int b0, const int r1, const int g1, const int b1, double t, const int alpha) {
    t = std::clamp(t, 0.0, 1.0);
    const auto c = [&](const int a, const int b) { return static_cast<int>(a + (b - a) * t + 0.5); };
    return IM_COL32(c(r0, r1), c(g0, g1), c(b0, b1), alpha);
}

// A grade's colour (per mille): rising warm, falling cool, level grey; the
// deeper, the stronger (to 10 per mille).
ImU32 grade_colour(const double grade, const int alpha) {
    if (grade > 1.0) return mix(235, 190, 70, 235, 70, 50, (grade - 1.0) / 9.0, alpha);
    if (grade < -1.0) return mix(110, 205, 120, 60, 140, 235, (-grade - 1.0) / 9.0, alpha);
    return IM_COL32(205, 205, 205, alpha);
}
#endif

// "-0,4": one decimal, the comma.
std::string grade_text(const double grade) {
    const double shown = std::round(grade * 10.0) / 10.0;
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.1f", std::abs(shown));
    std::string text = buffer;
    for (auto& c : text)
        if (c == '.') c = ',';
    // A hyphen for the minus: the panels' font has no U+2212 (testers: "?9,9").
    if (shown < 0.0) return "-" + text;
    if (shown > 0.0) return "+" + text;
    return text;
}

} // namespace

double ProfileStrip::span() const noexcept { return spans[std::clamp(span_index_, 0, span_count - 1)]; }

float ProfileStrip::height() {
#ifdef OPENRTS_HAS_IMGUI
    if (const auto* face = ui::hud_text_font()) return face->FontSize * 9.8f;
    return ImGui::GetFontSize() * 9.8f;
#else
    return 0.0f;
#endif
}

bool ProfileStrip::take_changed() noexcept {
    const bool changed = changed_;
    changed_ = false;
    return changed;
}

void ProfileStrip::draw(const ProfileView& view) {
#ifdef OPENRTS_HAS_IMGUI
    if (!view.profile || view.profile->points.size() < 2) return;
    const auto& profile = *view.profile;
    const auto& io = ImGui::GetIO();
    const auto display = io.DisplaySize;
    if (display.x < 50.0f || display.y < 50.0f) return;
    const float font = ui::hud_text_font() ? ui::hud_text_font()->FontSize : ImGui::GetFontSize();
    const float height = ProfileStrip::height();
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings
        | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollWithMouse
        | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground;
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(display.x, height), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0f);
    if (ImGui::Begin("##profile_strip", nullptr, flags)) {
        auto* draw = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetWindowPos();
        hud::push_window_font();
        hud::plate(draw, origin, ImVec2(display.x, height), 12.0f, 0.95f);
        const float pad = font * 0.6f;
        const float x0 = origin.x + pad, x1 = origin.x + display.x - pad;
        const float row0 = origin.y + pad * 0.4f;     // the grade, the stations, the span
        const float row1 = row0 + font * 1.25f;       // the speed limits
        const float top = row1 + font * 1.6f;         // the profile's area
        const float bottom = origin.y + height - font * 2.7f; // the grades' row under it, then the kilometres'
        const double left = this->left(), right = this->right();
        const auto x_of = [&](const double d) { return x0 + static_cast<float>((d - left) / (right - left)) * (x1 - x0); };

        // The heights shown: the visible part's, at least 6 m of them (a
        // level line stays level), a little room above and below.
        double low = 1e30, high = -1e30;
        for (const auto& p : profile.points) {
            if (p.distance < left || p.distance > right) continue;
            low = (std::min)(low, p.height);
            high = (std::max)(high, p.height);
        }
        // The joints are far apart: the edges' heights too.
        for (const double edge : {left, right}) {
            low = (std::min)(low, profile.height_at(edge));
            high = (std::max)(high, profile.height_at(edge));
        }
        const double middle = (low + high) * 0.5;
        const double range = (std::max)(high - low, 6.0) * 1.15;
        low = middle - range * 0.5;
        high = middle + range * 0.5;
        const auto y_of = [&](const double h) { return bottom - static_cast<float>((h - low) / (high - low)) * (bottom - top); };

        // The line through the track's joints as they are (each stretch of
        // one grade), by its grade's colour.
        const auto& points = profile.points;
        draw->PushClipRect(ImVec2(x0, top - font), ImVec2(x1, bottom), true);
        for (std::size_t i = 1; i < points.size(); ++i) {
            const auto& a = points[i - 1];
            const auto& b = points[i];
            if (b.distance < left || a.distance > right) continue;
            const double grade = profile.grade(a.distance, b.distance);
            // Cut to the strip (a joint may be kilometres off).
            const double from = (std::max)(a.distance, left - 1.0), to = (std::min)(b.distance, right + 1.0);
            const ImVec2 pa(x_of(from), y_of(profile.height_at(from)));
            const ImVec2 pb(x_of(to), y_of(profile.height_at(to)));
            draw->AddQuadFilled(pa, pb, ImVec2(pb.x, bottom), ImVec2(pa.x, bottom), grade_colour(grade, 45));
            draw->AddLine(pa, pb, grade_colour(grade, 255), 2.0f);
        }
        const auto line_y = [&](const double d) { return y_of(profile.height_at(d)); };
        // The kilometre posts: a faint line and the kilometre under it.
        float km_end = -1e9f;
        for (const auto& mark : view.marks) {
            if (mark.kind != ProfileMark::Kind::kilometre || mark.distance < left || mark.distance > right) continue;
            const float x = x_of(mark.distance);
            draw->AddLine(ImVec2(x, top - font * 0.3f), ImVec2(x, bottom), IM_COL32(255, 255, 255, 50), 1.0f);
        }
        draw->PopClipRect();
        for (const auto& mark : view.marks) {
            if (mark.kind != ProfileMark::Kind::kilometre || mark.distance < left || mark.distance > right) continue;
            const float x = x_of(mark.distance);
            const float w = ImGui::CalcTextSize(mark.text.c_str()).x;
            if (x - w * 0.5f < km_end + font * 0.4f || x + w * 0.5f > x1) continue;
            draw->AddText(ImVec2(x - w * 0.5f, bottom + font * 1.35f), hud::label_colour, mark.text.c_str());
            km_end = x + w * 0.5f;
        }
        // The elements' grades under the line, per mille with the sign
        // (rising +), their ends ticked, as on a regime map.
        for (const auto& element : view.elements) {
            if (element.to < left || element.from > right) continue;
            const double from = (std::max)(element.from, left), to = (std::min)(element.to, right);
            for (const double end : {element.from, element.to}) {
                if (end <= left || end >= right) continue;
                const float x = x_of(end);
                draw->AddLine(ImVec2(x, bottom), ImVec2(x, bottom + font * 1.15f), IM_COL32(255, 255, 255, 110), 1.0f);
            }
            const double shown = std::round(element.grade * 10.0) / 10.0;
            const std::string text = shown == 0.0 ? std::string("0") : grade_text(element.grade);
            const float w = ImGui::CalcTextSize(text.c_str()).x;
            const float a = x_of(from), b = x_of(to);
            if (b - a < w + 4.0f) continue;
            draw->AddText(ImVec2((a + b - w) * 0.5f, bottom + font * 0.1f), grade_colour(element.grade, 255), text.c_str());
        }
        // The train: a stroke of its length on the line, its mass and cars over it.
        {
            const double length = (std::max)(view.train_length, 1.0);
            const double from = (std::max)(-length, left), to = (std::min)(0.0, right);
            if (to > from) {
                const int steps = (std::max)(2, static_cast<int>((x_of(to) - x_of(from)) / 3.0f));
                ImVec2 last(x_of(from), line_y(from));
                for (int k = 1; k <= steps; ++k) {
                    const double d = from + (to - from) * k / steps;
                    const ImVec2 next(x_of(d), line_y(d));
                    draw->AddLine(last, next, hud::text_colour, 5.0f);
                    last = next;
                }
                const std::string label = trf("%.0f t, %zu cars", view.mass_tonnes, view.cars);
                const float w = ImGui::CalcTextSize(label.c_str()).x;
                const float cx = std::clamp(x_of((from + to) * 0.5) - w * 0.5f, x0, x1 - w);
                const float ty = (std::max)(top - font * 0.2f, line_y((from + to) * 0.5) - font * 1.6f);
                draw->AddText(ImVec2(cx, ty), hud::text_colour, label.c_str());
            }
        }
        // The signals on the line, their names over them on the near spans.
        float name_end = -1e9f;
        for (const auto& mark : view.marks) {
            if (mark.kind != ProfileMark::Kind::signal || mark.distance < left || mark.distance > right) continue;
            const float x = x_of(mark.distance);
            const float y = line_y(mark.distance) - font * 0.55f;
            draw->AddLine(ImVec2(x, y), ImVec2(x, line_y(mark.distance)), IM_COL32(230, 230, 230, 200), 1.0f);
            draw->AddCircleFilled(ImVec2(x, y), font * 0.32f, mark.colour);
            draw->AddCircle(ImVec2(x, y), font * 0.32f, IM_COL32(20, 20, 20, 255), 0, 1.0f);
            if (span() <= 5000.0 && !mark.text.empty()) {
                const float w = ImGui::CalcTextSize(mark.text.c_str()).x;
                if (x - w * 0.5f > name_end + font * 0.3f && x + w * 0.5f < x1) {
                    draw->AddText(ImVec2(x - w * 0.5f, y - font * 1.45f), hud::text_colour, mark.text.c_str());
                    name_end = x + w * 0.5f;
                }
            }
        }
        // The speed limits: their speed in a box over the profile.
        float limit_end = -1e9f;
        for (const auto& mark : view.marks) {
            if (mark.kind != ProfileMark::Kind::limit || mark.distance < left || mark.distance > right) continue;
            const float x = x_of(mark.distance);
            const auto size = ImGui::CalcTextSize(mark.text.c_str());
            if (x - size.x * 0.5f - 3.0f < limit_end + 2.0f || x + size.x * 0.5f + 3.0f > x1) continue;
            const ImVec2 a(x - size.x * 0.5f - 3.0f, row1), b(x + size.x * 0.5f + 3.0f, row1 + size.y + 2.0f);
            draw->AddRectFilled(a, b, IM_COL32(255, 255, 255, 235), 3.0f);
            draw->AddRect(a, b, hud::red_colour, 3.0f, 0, 2.0f);
            draw->AddText(ImVec2(a.x + 3.0f, a.y + 1.0f), IM_COL32(20, 20, 20, 255), mark.text.c_str());
            draw->AddLine(ImVec2(x, b.y), ImVec2(x, top - font * 0.2f), IM_COL32(210, 40, 40, 160), 1.0f);
            limit_end = b.x;
        }
        // The first row: the grade under the train, the stations, the span.
        const std::string grade = trf("Grade = %s ‰", grade_text(profile.grade(-(std::max)(view.train_length, 1.0), 0.0)).c_str());
        draw->AddText(ImVec2(x0, row0), hud::text_colour, grade.c_str());
        const std::string scale = trf("%.0f km", span() / 1000.0);
        const float scale_w = ImGui::CalcTextSize(scale.c_str()).x;
        draw->AddText(ImVec2(x1 - scale_w, row0), hud::dim_colour, scale.c_str());
        float station_end = x0 + ImGui::CalcTextSize(grade.c_str()).x + font;
        for (const auto& mark : view.marks) {
            if ((mark.kind != ProfileMark::Kind::station && mark.kind != ProfileMark::Kind::siding) || mark.distance < left ||
                mark.distance > right)
                continue;
            const float x = x_of(mark.distance);
            const float w = ImGui::CalcTextSize(mark.text.c_str()).x;
            const float at = x - w * 0.5f;
            if (at < station_end || x + w * 0.5f > x1 - scale_w - font) continue;
            const ImU32 colour = mark.kind == ProfileMark::Kind::siding ? IM_COL32(240, 220, 90, 255) : hud::text_colour;
            draw->AddText(ImVec2(at, row0), colour, mark.text.c_str());
            draw->AddTriangleFilled(ImVec2(x - 4.0f, row0 + font * 1.05f), ImVec2(x + 4.0f, row0 + font * 1.05f),
                                    ImVec2(x, row0 + font * 1.45f), colour);
            station_end = x + w * 0.5f + font * 0.5f;
        }
        // The wheel changes the span, a drag shifts it, a double click puts it back.
        if (ImGui::IsWindowHovered()) {
            if (io.MouseWheel > 0.0f && span_index_ > 0) {
                --span_index_;
                changed_ = true;
            } else if (io.MouseWheel < 0.0f && span_index_ < span_count - 1) {
                ++span_index_;
                changed_ = true;
            }
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                shift_ = 0.0;
                changed_ = true;
            } else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f) && io.MouseDelta.x != 0.0f) {
                shift_ = std::clamp(shift_ - io.MouseDelta.x * (right - left) / static_cast<double>(x1 - x0), shift_back, shift_ahead);
                changed_ = true;
            }
        }
        hud::pop_window_font();
    }
    ImGui::End();
#else
    (void)view;
#endif
}

} // namespace openrts::viewer
