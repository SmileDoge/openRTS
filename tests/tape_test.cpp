// The tape (simulation/tape.hpp): the line as RTS writes it (rts1.exe
// 0x5050B0) field by field, the file's name (0x504FA8), a line each game
// second, and the file read back; the lines read as lenta.xls reads them, a
// file read as it grows, the path and the summary.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <openrts/simulation/tape.hpp>

namespace {

int failures = 0;

void check(const bool condition, const std::string& what) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << what << '\n';
    }
}

std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> out;
    std::string field;
    for (const char c : line) {
        if (c == '#') {
            out.push_back(field);
            field.clear();
        } else {
            field.push_back(c);
        }
    }
    out.push_back(field);
    return out;
}

} // namespace

int main() {
    using namespace openrts::simulation;

    TapeRecord r;
    r.velocity = 12.5f;
    r.game_time = 3723; // 01:02:03
    r.signal = 6;
    r.signal_distance = 1234.5f;
    r.train_pipe = 5.0f;
    r.independent = 0.0f;
    r.cylinder = 3.8f;
    r.tile_x = -6000;
    r.tile_z = 14000;
    r.x = -100.25f;
    r.y = 150.5f;
    r.z = -200.75f;
    r.throttle = 7;
    r.pantographs = 1;
    r.speed_limit = 22.2222f;
    r.sanding = 1.0f;
    r.consumption = 1500.5f;
    r.clock_hour = 10;
    r.clock_minute = 20;
    r.clock_second = 30;
    r.mass = 4347000.0;
    r.cars = 52;
    r.locomotives = 2;
    r.length = 638.3;
    r.kind = 3;
    r.season = 3;
    r.weather = 1;
    r.ep = true;
    r.equalising = 5.2f;
    r.ordinate = 123.456f;
    const std::string line = tape_line(r);
    const std::string expected =
        "12.500#1#2#3#6#0#73.45#0.00# 55.82#  1234.500#6#-100.25#  150.50#-200.75#-6000#14000#0#0#8#7#1.00#1#0#5#2#0.00#22.222#0#"
        "   1.00#  1500.50#10#20#30# 4.347000E+06#52#2#1#  6.383000E+02#3#3#1#1# 0.00#-4663#22.222#76.388#123.456\r\n";
    check(line == expected, "the line as RTS writes it:\n  " + line + "  expected\n  " + expected);
    const auto fields = split(line.substr(0, line.size() - 2));
    check(fields.size() == 47, "47 fields, not " + std::to_string(fields.size()));
    // Where lenta.xls (ReadLenta) reads them.
    if (fields.size() == 47) {
        check(fields[0] == "12.500", "0: the speed, m/s");
        check(fields[4] == "6" && fields[10] == "6", "4 and 10: the signal's aspect");
        check(fields[6] == "73.45" && fields[8] == " 55.82", "6, 8: the pipe and the cylinder, psi");
        check(fields[9] == "  1234.500", "9: to the signal, m");
        check(fields[12] == "  150.50", "12: Y");
        check(fields[28] == "   1.00" && fields[29] == "  1500.50", "28, 29: sand, energy");
        check(fields[30] == "10" && fields[31] == "20" && fields[32] == "30", "30..32: the computer's clock");
        check(fields[33] == " 4.347000E+06" && fields[34] == "52" && fields[35] == "2", "33..35: mass, cars, engines");
        check(fields[37] == "  6.383000E+02", "37: the length");
        check(fields[41] == "1", "41: ЭПТ");
        check(fields[44] == "22.222" && fields[26] == "22.222", "26, 44: the limit, m/s");
        check(fields[45] == "76.388", "45: the equalising reservoir, psi");
        check(fields[46] == "123.456", "46: the kilometre");
    }

    // RTS's clock into the day.
    for (const auto& [time, text] : std::vector<std::pair<int, std::string>>{
             {-10, "#23#59#50#"}, {90000, "#1#0#0#"}, {86400, "#24#0#0#"}, {0, "#0#0#0#"}}) {
        TapeRecord t;
        t.game_time = time;
        const auto l = tape_line(t);
        check(l.find(text) == l.find('#'), "time " + std::to_string(time) + " is " + text + " in " + l);
    }

    // The name.
    std::tm started{};
    started.tm_year = 126;
    started.tm_mon = 10;
    started.tm_mday = 11;
    started.tm_hour = 9;
    started.tm_min = 5;
    started.tm_sec = 7;
    check(tape_file_name(started, "\xD0\x92\xD0\x9B\x38\x30") == "2026.11.11_09_05_07_\xD0\x92\xD0\x9B\x38\x30_trainlog_rts.txt",
          "the name: date, time, series");
    check(tape_file_name(started, "a/b:c") == "2026.11.11_09_05_07_a_b_c_trainlog_rts.txt", "refused characters become _");

    // A line each game second, read back.
    const auto folder = std::filesystem::temp_directory_path() / "openrts_tape_test";
    std::error_code error;
    std::filesystem::remove_all(folder, error);
    {
        TapeRecorder tape;
        check(tape.start(folder / "RTS_TAPES", "test", started), "the tape starts");
        check(tape.recording(), "it rolls");
        check(!tape.advance(0.4) && !tape.advance(0.4), "not yet a second");
        check(tape.advance(0.4), "a second");
        tape.write(r);
        check(!tape.advance(0.7), "0.2 s carried on, 0.9 s");
        check(tape.advance(0.1), "the next second");
        tape.write(r);
        // Readable while it rolls.
        std::ifstream reading(tape.path(), std::ios::binary);
        check(reading.good(), "another reader opens it while it rolls");
        check(tape.lines() == 2, "two lines");
        const auto path = tape.path();
        tape.stop();
        check(!tape.recording() && !tape.advance(5.0), "stopped");
        std::ifstream in(path, std::ios::binary);
        std::stringstream all;
        all << in.rdbuf();
        check(all.str() == expected + expected, "the file holds the two lines");
        check(path.filename().string() == "2026.11.11_09_05_07_test_trainlog_rts.txt", "in RTS_TAPES by its name");
    }
    std::filesystem::remove_all(folder, error);

    // Read back: the fields where lenta.xls takes them, the pressures in atm.
    const auto near = [](const double a, const double b, const double within) { return std::abs(a - b) <= within; };
    const auto back = parse_tape_line(expected);
    check(back.has_value(), "the line reads back");
    if (back) {
        check(near(back->velocity, 12.5, 1e-6) && back->game_time == 3723 && back->signal == 6, "speed, time, aspect");
        check(near(back->signal_distance, 1234.5, 1e-3), "to the signal");
        check(near(back->train_pipe, 5.0, 0.001) && near(back->independent, 0.0, 1e-6) && near(back->cylinder, 3.8, 0.001),
              "the pressures in atm");
        check(back->tile_x == -6000 && back->tile_z == 14000 && near(back->x, -100.25, 1e-3) && near(back->y, 150.5, 1e-3) &&
                  near(back->z, -200.75, 1e-3),
              "where");
        check(back->throttle == 7 && back->pantographs == 1, "throttle, pantographs");
        check(near(back->speed_limit, 22.222, 1e-4) && near(back->sanding, 1.0, 1e-6) && near(back->consumption, 1500.5, 1e-3),
              "limit, sand, energy");
        check(back->clock_hour == 10 && back->clock_minute == 20 && back->clock_second == 30, "the computer's clock");
        check(near(back->mass, 4347000.0, 1e-3) && back->cars == 52 && back->locomotives == 2 && near(back->length, 638.3, 1e-6),
              "mass, cars, engines, length");
        check(back->kind == 3 && back->season == 3 && back->weather == 1 && back->ep, "kind, season, weather, ЭПТ");
        check(near(back->equalising, 5.2, 0.001) && near(back->ordinate, 123.456, 1e-3), "УР, the kilometre");
        check(tape_line(*back) == expected, "and written again, the same line");
    }
    // RTS 0.44.1's line ended at field 44: no УР, no kilometre.
    {
        const auto short_line = expected.substr(0, expected.find("#76.388")) + "\r\n";
        const auto short_back = parse_tape_line(short_line);
        check(short_back && short_back->equalising == 0.0f && short_back->ordinate == -1.0f && near(short_back->speed_limit, 22.222, 1e-4),
              "a 45-field line (RTS 0.44.1)");
    }
    check(!parse_tape_line("") && !parse_tape_line("hello") && !parse_tape_line("1#2#3#4\r\n"), "not a tape's line");
    {
        auto broken = expected;
        broken.replace(0, 6, "12.5x0");
        check(!parse_tape_line(broken), "a field that is not a number");
    }

    // A file read as it grows: whole lines only; made anew, read again.
    {
        std::filesystem::create_directories(folder, error);
        const auto path = folder / "grow_trainlog_rts.txt";
        const auto put = [&](const std::string& text, const bool append) {
            std::ofstream out(path, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
            out << text;
        };
        TapeReader reader(path);
        check(!reader.refresh() && !reader.readable(), "no file yet");
        put(expected + expected + expected.substr(0, 20), false);
        check(reader.refresh() && reader.readable() && reader.records().size() == 2, "two whole lines, the third waits");
        check(!reader.refresh(), "nothing more");
        put(expected.substr(20) + "junk\r\n", true);
        check(reader.refresh() && reader.records().size() == 3 && reader.skipped() == 1, "the third line, a junk one skipped");
        put(expected, false);
        check(reader.refresh() && reader.records().size() == 1 && reader.skipped() == 0, "made anew: read from the start");
    }
    std::filesystem::remove_all(folder, error);

    // The path and the summary: 10 s at 10 m/s, 3 s at 25 m/s over a
    // 20 m/s limit, a 15 s stop, 5 s at 10 m/s backing.
    {
        std::vector<TapeRecord> run;
        const auto add = [&](const std::size_t lines, const float speed) {
            for (std::size_t i = 0; i < lines; ++i) {
                TapeRecord t;
                t.velocity = speed;
                t.speed_limit = 20.0f;
                run.push_back(t);
            }
        };
        add(3, 0.0f); // standing at the start: not a stop
        add(10, 10.0f);
        add(3, 25.0f);
        add(15, 0.0f);
        add(5, -10.0f);
        const auto path = tape_distances(run);
        check(path.size() == run.size() && path.back() == 225.0 && path[2] == 0.0 && path[12] == 100.0, "the path, by the speeds' size");
        const auto s = summarize_tape(run);
        check(s.distance == 225.0 && s.seconds == 36 && s.top_speed == 25.0f, "distance, time, top speed");
        check(s.stops.size() == 1 && s.stops[0].first == 16 && s.stops[0].lines == 15, "one stop, 15 s (not the standing at the start)");
        check(s.overspeeds.size() == 1 && s.overspeeds[0].first == 13 && s.overspeeds[0].lines == 3 && near(s.worst_overspeed, 5.0, 1e-6),
              "one run above the limit, 3 s, 5 m/s");
    }

    if (failures == 0) std::cout << "tape: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
