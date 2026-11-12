// openrts-brake-stand: a brake distributor (ВР) on the test stand
// (simulation::run_stand, docs/scripting.md): the built-in 292 / 483 / 305
// or one in Luau, through the standard program; its figures and, with
// --table, the pressures every 10 s.
//
//   openrts-brake-stand [--kind 292|483|305] [--script file.luau]
//                       [--mass kg] [--ratio r] [--full atm] [--table]
//                       [--load empty|medium|loaded] [--mountain] [--plug К|Д|УВ]
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <openrts/simulation/distributor.hpp>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

// The command line as UTF-8 (Windows hands main() the ANSI code page: "Д"
// and Cyrillic folder names would not match or open).
std::vector<std::string> utf8_arguments(int argc, char** argv) {
#ifdef _WIN32
    int count = 0;
    if (wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count)) {
        std::vector<std::string> out;
        for (int i = 0; i < count; ++i) {
            const int n = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
            std::string a(static_cast<std::size_t>((std::max)(n - 1, 0)), '\0');
            if (n > 1) WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, a.data(), n, nullptr, nullptr);
            out.push_back(std::move(a));
        }
        LocalFree(wide);
        return out;
    }
#endif
    return std::vector<std::string>(argv, argv + argc);
}

} // namespace

int main(int raw_argc, char** raw_argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    const auto args = utf8_arguments(raw_argc, raw_argv);
    const int argc = static_cast<int>(args.size());
    const auto argv = [&args](const int i) { return args[static_cast<std::size_t>(i)].c_str(); };
    using namespace openrts::simulation;
    DistributorCar car;
    car.kind = DistributorKind::vr292;
    car.mass = 50000.0f;
    std::string script_path;
    bool table = false;
    bool full_given = false;
    DistributorLoad load = DistributorLoad::by_mass;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv(i);
        const bool has_value = i + 1 < argc;
        if (arg == "--kind" && has_value) {
            const std::string_view kind = argv(++i);
            car.kind = kind == "483" ? DistributorKind::vr483 : kind == "305" ? DistributorKind::evr305 : DistributorKind::vr292;
        } else if (arg == "--script" && has_value) {
            script_path = argv(++i);
        } else if (arg == "--mass" && has_value) {
            car.mass = static_cast<float>(std::atof(argv(++i)));
        } else if (arg == "--ratio" && has_value) {
            car.ratio = static_cast<float>(std::atof(argv(++i)));
        } else if (arg == "--full" && has_value) {
            car.full_cylinder = static_cast<float>(std::atof(argv(++i)));
            full_given = true;
        } else if (arg == "--table") {
            table = true;
        } else if (arg == "--load" && has_value) {
            const std::string_view value = argv(++i);
            load = value == "empty" ? DistributorLoad::empty : value == "medium" ? DistributorLoad::medium : DistributorLoad::loaded;
        } else if (arg == "--mountain") {
            car.mountain = true;
        } else if (arg == "--plug" && has_value) {
            // К / Д / УВ, in Cyrillic or Latin letters.
            const std::string_view value = argv(++i);
            car.plug = value == "Д" || value == "D" || value == "d"                     ? DistributorPlug::long_train
                       : value == "УВ" || value == "UV" || value == "uv" || value == "YB" ? DistributorPlug::no_accelerator
                                                                                          : DistributorPlug::short_train;
        } else {
            std::cerr << "usage: openrts-brake-stand [--kind 292|483|305] [--script file.luau] [--mass kg] [--ratio r] [--full atm] [--table]\n"
                         "                           [--load empty|medium|loaded] [--mountain] [--plug К|Д|УВ]\n";
            return 2;
        }
    }
    car.auxiliary_volume = distributor_auxiliary_volume(car.kind);
    car.cylinder_volume = distributor_cylinder_volume;
    car.pipe_volume = 0.016f;
    car.load = effective_load(load, car.mass);
    if (!full_given) car.full_cylinder = distributor_full_cylinder(car.kind, car.load);

    std::unique_ptr<ScriptDistributor> script;
    if (!script_path.empty()) {
        std::ifstream in(std::filesystem::path(std::u8string(script_path.begin(), script_path.end())), std::ios::binary);
        if (!in) {
            std::cerr << "cannot read " << script_path << '\n';
            return 1;
        }
        std::stringstream source;
        source << in.rdbuf();
        std::string error;
        const auto slash = script_path.find_last_of("/\\");
        script = ScriptDistributor::load(source.str(), slash == std::string::npos ? script_path : script_path.substr(slash + 1), error);
        if (!script) {
            std::cerr << "ошибка скрипта: " << error << '\n';
            return 1;
        }
    }

    const auto report = run_stand(car, script.get());
    const char* kind = car.kind == DistributorKind::vr483 ? "483" : car.kind == DistributorKind::evr305 ? "305" : "292";
    std::printf("ВР: %s; масса %.0f т, ТЦ полного %.2f, TripleValveRatio %.1f\n",
                script ? script_path.c_str() : (std::string("встроенный ") + kind).c_str(), car.mass / 1000.0f, car.full_cylinder,
                car.ratio);
    if (car.kind == DistributorKind::vr483) {
        std::printf("Режим: %s, %s\n", car.load == DistributorLoad::empty ? "порожний" : car.load == DistributorLoad::medium ? "средний" : "гружёный",
                    car.mountain ? "горный" : "равнинный");
    } else {
        std::printf("Пробка: %s\n", car.plug == DistributorPlug::short_train ? "К (короткий поезд)"
                                     : car.plug == DistributorPlug::long_train ? "Д (длинный поезд)"
                                                                               : "УВ (ускоритель выключен)");
    }
    if (!report.error.empty()) {
        std::printf("ошибка скрипта: %s\n", report.error.c_str());
        return 1;
    }
    const auto seconds = [](const float s) { return s < 0.0f ? std::string("не дошло") : std::to_string(static_cast<int>(s + 0.5f)) + " с"; };
    std::printf("Зарядка ЗР до 4,9:                      %s\n", seconds(report.charged).c_str());
    std::printf("Ступень 0,5: ТЦ                         %.2f, 95 %% за %s\n", report.step_cylinder, seconds(report.step_fill).c_str());
    std::printf("Отпуск после ступени до ТЦ < 0,4:       %s\n", seconds(report.release).c_str());
    std::printf("Полное служебное (ТМ 3,5): ТЦ           %.2f\n", report.full_cylinder);
    std::printf("Мягкость: снятие сверхзарядки, ТЦ до    %.2f %s\n", report.creep, report.creep < 0.05f ? "(не тормозит)" : "(ТОРМОЗИТ)");
    std::printf("Экстренное: 95 %% полного за %s, ТЦ %.2f\n", seconds(report.emergency_fill).c_str(), report.emergency_cylinder);
    if (table) {
        std::printf("\n   с     ТМ     ЗР     ТЦ    память  торм\n");
        for (const auto& s : report.samples) {
            const int t = static_cast<int>(s.time + 0.5f);
            if (t % 10 != 0) continue;
            std::printf("%4d  %5.2f  %5.2f  %5.2f  %5.2f   %s\n", t, s.pipe, s.auxiliary, s.cylinder, s.reference, s.braking ? "да" : "");
        }
    }
    return 0;
}
