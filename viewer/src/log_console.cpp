#include "log_console.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>
#include <iostream>
#include <mutex>
#include <streambuf>

#ifdef OPENRTS_HAS_IMGUI
#include <imgui.h>
#endif

#include "ui_text.hpp"

namespace openrts::viewer {
namespace {

constexpr std::size_t kept_most = 5000;
constexpr std::size_t line_most = 4000; // a longer line is cut there

struct Store {
    std::mutex mutex;
    std::deque<LogLine> lines;
    std::uint64_t first = 0; // the number of lines.front()
    LogCounts counts;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

Store& store() {
    static Store* kept = new Store; // never destroyed: written to while the program ends
    return *kept;
}

void keep(std::string text) {
    auto& s = store();
    const auto level = classify_log_line(text);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - s.start).count();
    const std::lock_guard lock(s.mutex);
    s.lines.push_back({seconds, level, std::move(text)});
    ++s.counts.lines;
    if (level == LogLevel::warning) ++s.counts.warnings;
    if (level == LogLevel::error) ++s.counts.errors;
    while (s.lines.size() > kept_most) {
        s.lines.pop_front();
        ++s.first;
    }
}

// Each thread's line not ended yet, per stream (their characters arrive
// between other threads' ones).
thread_local std::string pending_out;
thread_local std::string pending_err;

// A stream's characters to where they went and, by lines, to the store.
class TeeBuffer final : public std::streambuf {
public:
    TeeBuffer(std::streambuf* original, const bool error) : original_(original), error_(error) {}

protected:
    int_type overflow(const int_type c) override {
        if (traits_type::eq_int_type(c, traits_type::eof())) return traits_type::not_eof(c);
        const char ch = traits_type::to_char_type(c);
        put(&ch, 1);
        return c;
    }
    std::streamsize xsputn(const char* s, const std::streamsize n) override {
        if (n > 0) put(s, static_cast<std::size_t>(n));
        return n;
    }
    int sync() override {
        const std::lock_guard lock(write_mutex());
        return original_ ? original_->pubsync() : 0;
    }

private:
    static std::mutex& write_mutex() {
        static auto* mutex = new std::mutex;
        return *mutex;
    }
    void put(const char* s, const std::size_t n) {
        {
            const std::lock_guard lock(write_mutex());
            if (original_) original_->sputn(s, static_cast<std::streamsize>(n));
        }
        auto& pending = error_ ? pending_err : pending_out;
        for (std::size_t i = 0; i < n; ++i) {
            const char c = s[i];
            if (c == '\n') {
                keep(std::move(pending));
                pending.clear();
            } else if (c != '\r' && pending.size() < line_most) {
                pending += c;
            }
        }
    }

    std::streambuf* original_;
    bool error_;
};

[[nodiscard]] bool has(const std::string& lower, const char* word) {
    return lower.find(word) != std::string::npos;
}

} // namespace

void start_log_capture() {
    static bool started = false;
    if (started) return;
    started = true;
    (void)store();
    // Never destroyed: the streams keep them to the end.
    std::cout.rdbuf(new TeeBuffer(std::cout.rdbuf(), false));
    std::cerr.rdbuf(new TeeBuffer(std::cerr.rdbuf(), true));
}

LogLines log_lines_from(const std::uint64_t from) {
    auto& s = store();
    const std::lock_guard lock(s.mutex);
    LogLines out;
    out.first = (std::max)(from, s.first);
    out.next = s.first + s.lines.size();
    for (auto n = out.first; n < out.next; ++n) out.lines.push_back(s.lines[static_cast<std::size_t>(n - s.first)]);
    return out;
}

LogCounts log_counts() {
    auto& s = store();
    const std::lock_guard lock(s.mutex);
    return s.counts;
}

void clear_log() {
    auto& s = store();
    const std::lock_guard lock(s.mutex);
    s.first += s.lines.size();
    s.lines.clear();
}

LogLevel classify_log_line(const std::string_view text) {
    std::string lower(text);
    for (auto& c : lower) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    // Reports of none ("errors 0", "0 errors") are not errors.
    const bool no_errors = has(lower, "errors 0") || has(lower, "0 errors") || has(lower, "no errors");
    if (!no_errors && (has(lower, "error") || has(lower, "failed") || has(lower, "failure") || has(lower, "fail:") || has(lower, "crash")
                       || has(lower, "exception") || has(lower, "fatal") || has(lower, "шибк")))
        return LogLevel::error;
    if (has(lower, "warning") || has(lower, "not found") || has(lower, "missing") || has(lower, "cannot") || has(lower, "can't")
        || has(lower, "could not") || has(lower, "unknown") || has(lower, "left off") || has(lower, "not a ") || has(lower, "invalid")
        || has(lower, "out of range") || has(lower, "skipped") || has(lower, "ignored") || has(lower, "not supported")
        || has(lower, "over budget") || has(lower, "is not read") || has(lower, "not taken") || has(lower, "редупрежд"))
        return LogLevel::warning;
    return LogLevel::info;
}

#ifdef OPENRTS_HAS_IMGUI
void draw_log_console(LogConsole& console) {
    // Ctrl+F2 (F2 alone saves the game, as in RTS).
    if (ImGui::IsKeyPressed(ImGuiKey_F2, false) && ImGui::GetIO().KeyCtrl && !ImGui::GetIO().WantTextInput) {
        console.open = !console.open;
        if (console.open) ImGui::SetNextWindowFocus();
    }
    if (!console.open) return;
    // The new lines since the last frame.
    {
        auto fresh = log_lines_from(console.next);
        if (fresh.first > console.next && !console.lines.empty() && fresh.first >= console.next + kept_most) console.lines.clear();
        for (auto& line : fresh.lines) console.lines.push_back(std::move(line));
        if (console.lines.size() > kept_most) console.lines.erase(console.lines.begin(), console.lines.end() - static_cast<std::ptrdiff_t>(kept_most));
        console.next = fresh.next;
    }
    ImGui::SetNextWindowSize({860.0f, 420.0f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos({40.0f, 60.0f}, ImGuiCond_FirstUseEver);
    const std::string title = std::string(tr("Console (Ctrl+F2): the game's messages")) + "###console";
    if (!ImGui::Begin(title.c_str(), &console.open)) {
        ImGui::End();
        return;
    }
    const auto counts = log_counts();
    const char* shown[] = {tr("Everything"), tr("Warnings and errors"), tr("Errors")};
    ImGui::SetNextItemWidth(200.0f);
    ImGui::Combo("##shown", &console.shown, shown, 3);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(240.0f);
    ImGui::InputTextWithHint("##filter", tr("words to find"), console.filter, sizeof console.filter);
    ImGui::SameLine();
    ImGui::Checkbox(tr("Follow the newest"), &console.follow);
    ImGui::SameLine();
    const bool copy = ImGui::Button(tr("Copy"));
    ImGui::SameLine();
    if (ImGui::Button(tr("Clear##log"))) {
        clear_log();
        console.lines.clear();
        console.next = log_lines_from(0).next;
    }
    ImGui::TextDisabled(tr("%llu lines, of them %llu warnings and %llu errors since the start (also in openrts.log)"),
                        static_cast<unsigned long long>(counts.lines), static_cast<unsigned long long>(counts.warnings),
                        static_cast<unsigned long long>(counts.errors));
    // The lines shown: by level and the words (any case).
    std::string filter(console.filter);
    for (auto& c : filter) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    std::vector<const LogLine*> rows;
    rows.reserve(console.lines.size());
    for (const auto& line : console.lines) {
        if (console.shown == 1 && line.level == LogLevel::info) continue;
        if (console.shown == 2 && line.level != LogLevel::error) continue;
        if (!filter.empty()) {
            std::string lower(line.text);
            for (auto& c : lower) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
            if (lower.find(filter) == std::string::npos) continue;
        }
        rows.push_back(&line);
    }
    if (copy) {
        std::string text;
        for (const auto* line : rows) text += line->text + "\n";
        ImGui::SetClipboardText(text.c_str());
    }
    ImGui::Separator();
    if (ImGui::BeginChild("##lines", {0.0f, 0.0f}, ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rows.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const auto& line = *rows[static_cast<std::size_t>(i)];
                const int minutes = static_cast<int>(line.seconds / 60.0);
                ImGui::TextDisabled("%02d:%04.1f", minutes, line.seconds - minutes * 60.0);
                ImGui::SameLine();
                const ImVec4 colour = line.level == LogLevel::error     ? ImVec4{1.0f, 0.45f, 0.40f, 1.0f}
                                      : line.level == LogLevel::warning ? ImVec4{1.0f, 0.82f, 0.35f, 1.0f}
                                                                        : ImGui::GetStyleColorVec4(ImGuiCol_Text);
                ImGui::PushStyleColor(ImGuiCol_Text, colour);
                ImGui::TextUnformatted(line.text.c_str());
                ImGui::PopStyleColor();
            }
        }
        if (console.follow && ImGui::GetScrollY() < ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::End();
}
#else
void draw_log_console(LogConsole&) {}
#endif

} // namespace openrts::viewer
