// The console's capture (viewer/src/log_console.hpp): std::cout's and
// std::cerr's lines kept whole and marked by their words, written by
// several threads at once; the numbering through a clear.

#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "log_console.hpp"

namespace {

int failures = 0;

void check(const bool condition, const std::string& what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

} // namespace

int main() {
    using namespace openrts::viewer;
    start_log_capture();
    std::cout << "loading the route" << std::endl;
    std::cout << "material: body.material.json5: warning: the roughness map has its red 0 everywhere\n";
    std::cerr << "texture 'x.ace': upload failed (out of memory)" << std::endl;
    std::cout << "the frame: 2 errors 0 here\r\n";
    auto lines = log_lines_from(0);
    check(lines.lines.size() == 4, "four lines kept (" + std::to_string(lines.lines.size()) + ")");
    if (lines.lines.size() == 4) {
        check(lines.lines[0].level == LogLevel::info && lines.lines[0].text == "loading the route", "an ordinary line");
        check(lines.lines[1].level == LogLevel::warning, "a warning");
        check(lines.lines[2].level == LogLevel::error, "an error (std::cerr's)");
        check(lines.lines[3].level == LogLevel::info && lines.lines[3].text == "the frame: 2 errors 0 here", "\"errors 0\" is no error, the \\r dropped");
    }
    // Four threads, each its own lines in pieces: every line kept whole.
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([t] {
            for (int i = 0; i < 200; ++i) std::cout << "thread " << t << " line " << i << " end" << '\n';
        });
    }
    for (auto& each : threads) each.join();
    lines = log_lines_from(4);
    bool whole = lines.lines.size() == 800;
    for (const auto& line : lines.lines) whole = whole && line.text.rfind("thread ", 0) == 0 && line.text.size() > 4
                                                 && line.text.compare(line.text.size() - 4, 4, " end") == 0;
    check(whole, "800 lines from four threads, each whole (" + std::to_string(lines.lines.size()) + ")");
    const auto counts = log_counts();
    check(counts.lines == 804 && counts.warnings == 1 && counts.errors == 1, "the counts since the start");
    clear_log();
    check(log_lines_from(0).lines.empty() && log_lines_from(0).next == 804, "cleared, the numbers go on");
    std::cout << "after the clear" << std::endl;
    check(log_lines_from(0).first == 804 && log_lines_from(0).lines.size() == 1, "a new line numbered on");
    check(classify_log_line("Ошибка чтения") == LogLevel::error && classify_log_line("shape 'x.s': Not found: x.s") == LogLevel::warning,
          "Russian error words and 'not found'");
    if (failures == 0) std::cerr << "log console: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
