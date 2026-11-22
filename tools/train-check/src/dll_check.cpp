// openrts-train-check <root> --dll <TRAINSET folder> <eng|wag>[!] ... [--script <file>] [--seconds <s>]
//
// Runs legacy RTS locomotive DLLs through openrts-dllhost32 on a straight
// level test track: the consist is the named .eng / .wag files of one
// TRAINSET folder in order ("!" after a name turns that car), the first
// engine with a DLL is the one the player takes. Every second it prints
// what the DLLs report (--cars: each car, its exhausts too; --body: each
// car's body motion, openRTS's model, the most of each second). A script
// drives the cab and checks the outcome:
//
//   # time in seconds, then a command
//   1    switch 38 2          # CanSwitch + move cab switch ID 38 to state 2
//   2    switch B_S 1         # by .sd name
//   3    switch B_S 0
//   0    voltage 3000         # line voltage at the pantographs (V [Hz], DC without Hz)
//   30   expect force > 10000 # fails the run when false (force, throttle,
//                             # reverse, panto, main_switch, speed (km/h),
//                             # pipe, mr, ur, alive)
//   20   screen 180 24 043    # a cab screen's label text (or shown / hidden)
//   5    press 404 416 408    # momentary keys, one a step (1, then 0)
//   40   expect var140 == 60  # BRAKES' variable stack at byte 140, a float;
//   40   expect bits8 & 8     # bitsN its bits: & any set, !& none set
//                             # (also aspect: the cab signal's)
//
// A line for the КЛУБ (step 3.2): signals and speed posts at metres from
// the train's front at the start; the ALSN's arrays, the train's limit and
// the next speed post follow the front as it runs.
//
//   0    line_limit 120         # km/h before the first post
//   0    line_signal S1 1500 3  # name, metres, RTS aspect [limit km/h]
//   0    line_post 2500 80      # metres, km/h
//   90   line_aspect S1 7       # the signal cleared
//   0    when 2650 switch 2 5   # a command once the front is that far
//   0    then 3 switch 2 2      # so many seconds after the `when` above
//
// --record <file> [--record-library <AuxLibraryName>]: a golden run
// (docs/brakes-dll-replacement.md, step 1): every step, for every car with
// that auxiliary library (BRAKES by default), a tab-separated row: the
// time, the car, the speed, its air, its Engine's brake outputs, the SMS
// triggers of that library and of the locomotive, the cab switches and
// displays (gauges, lamps) that changed (a display: id=state/value), and
// the library's whole variable stack as raw 32-bit words (hex: some are
// floats, some flags).
//
// --brakes openrts [--numbers books]: openRTS's cab brake devices in place
// of Brakes.dll's (docs/brakes-dll-replacement.md, step 2) on the realistic
// air; a record then has their УР, independent brake, ЭПТ, gauges and
// lamps (tools/reverse/golden_compare.py sets it against a Brakes.dll run).
//
// --klub shadow: openRTS's КЛУБ-У (simulation/klub.hpp; step 3.3) beside
// Brakes.dll's on the same inputs (the line's ALSN, the keys, the cab's
// switches), every step set against Brakes.dll's variables (the aspect,
// the limits, the target, the permitted speed, the vigilance timer, the
// flags, the track, the entry, the ЭПК and the КЛУБ's sounds); a step that
// differs fails the run (tests/data/golden/ed9m_klub*.script); its screen
// (simulation/klub_screen.hpp) against Brakes.dll's too.
//
// --klub openrts: openRTS's КЛУБ in place of Brakes.dll's, as the game's
// setting has it (LegacyConsist::set_own_klub; step 3.4): Brakes.dll's
// switched off, the realistic air (--air own unless given).
// A script line `klub openrts` / `klub brakes` switches it on the way
// (with --air own or bpm).
//
// Exit code 0: every expectation held; 1: one failed; 2: could not run.

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <optional>
#include <sstream>
#include <span>
#include <string>
#include <vector>

#include <map>

#include <glm/geometric.hpp>

#include <openrts/core/vfs.hpp>
#include <openrts/formats/ace_file.hpp>
#include <openrts/formats/rolling_stock.hpp>
#include <openrts/simulation/stock_rules.hpp>

#include "cab_screens.hpp"
#include <openrts/simulation/klub_screen.hpp>
#include <openrts/simulation/train.hpp>
#include <openrts/trainplugin/legacy_consist.hpp>

namespace openrts {

namespace {

struct Command {
    double time{};
    std::string verb;
    std::vector<std::string> args;
    bool done = false;
};

std::vector<Command> read_script(const std::string& path, std::string& error) {
    std::vector<Command> out;
    std::ifstream in(path);
    if (!in) {
        error = "cannot read " + path;
        return out;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (const auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        std::istringstream words(line);
        Command c;
        if (!(words >> c.time >> c.verb)) continue;
        std::string w;
        while (words >> w) c.args.push_back(w);
        out.push_back(std::move(c));
    }
    return out;
}

double field(const std::string& name, const trainplugin::LegacyConsist& consist, std::size_t car,
             const simulation::Train& train) {
    const auto* out = consist.output(car);
    const auto& state = train.cars()[car];
    if (name == "alive") return consist.alive() ? 1.0 : 0.0;
    if (name == "speed") return std::abs(train.speed()) * 3.6;
    if (name == "pipe") return state.pipe;
    if (name == "mr") return state.main_reservoir;
    if (!out) return std::nan("");
    const auto& e = out->engine;
    if (name == "force") return e.force;
    if (name == "throttle") return e.throttle;
    if (name == "reverse") return e.reverse;
    if (name == "panto") return e.panto;
    if (name == "main_switch") return e.main_switch;
    if (name == "ur") return e.equalising;
    if (name == "brake_force") return e.brake_force;
    if (name == "loco_flags") return out->loco_flags;
    if (name == "raised") return out->panto_raised; // the pantographs the DLL raises (bit 0: 1, bit 1: 2)
    if (name == "master") return (out->loco_flags & 1) ? 1.0 : 0.0;
    if (name == "bound") return (out->loco_flags & 0x20) ? 1.0 : 0.0;
    if (name.rfind("disp", 0) == 0) { // dispN: state of cab display N (a lamp: lit or not)
        const auto id = static_cast<std::size_t>(std::atoi(name.c_str() + 4));
        return id < out->displays.size() ? out->displays[id].state : std::nan("");
    }
    if (name.rfind("key", 0) == 0 || name.rfind("sw", 0) == 0) { // keyN / swN: state of cab switch N
        const auto id = static_cast<std::size_t>(std::atoi(name.c_str() + (name[0] == 'k' ? 3 : 2)));
        return id < out->switches.size() ? out->switches[id].state : std::nan("");
    }
    if (name == "aspect") return out->cab_aspect; // Cabin::Signal's aspect as the DLLs left it
    // varN / bitsN: BRAKES' variable stack at byte offset N, as a float / as
    // its bits (with --record, or a script: CarOutput::aux_vars).
    if (name.rfind("var", 0) == 0 || name.rfind("bits", 0) == 0) {
        const bool bits = name[0] == 'b';
        const auto offset = static_cast<std::size_t>(std::atoi(name.c_str() + (bits ? 4 : 3)));
        const int lib = consist.aux_library(car, "BRAKES");
        if (lib < 0 || static_cast<std::size_t>(lib) >= out->aux_vars.size()) return std::nan("");
        const auto& var = out->aux_vars[static_cast<std::size_t>(lib)].var;
        if (offset % 4 != 0 || offset / 4 >= var.size()) return std::nan("");
        const float v = var[offset / 4];
        if (!bits) return v;
        std::uint32_t u = 0;
        std::memcpy(&u, &v, sizeof u);
        return u;
    }
    return std::nan("");
}

bool compare(double a, const std::string& op, double b) {
    if (op == ">") return a > b;
    if (op == "<") return a < b;
    if (op == ">=") return a >= b;
    if (op == "<=") return a <= b;
    if (op == "==") return std::abs(a - b) < 1e-6;
    if (op == "!=") return std::abs(a - b) >= 1e-6;
    if (op == "&" || op == "!&") {
        if (!(a >= 0.0) || !(b >= 0.0)) return false;
        const bool any = (static_cast<std::uint64_t>(a) & static_cast<std::uint64_t>(b)) != 0;
        return op == "&" ? any : !any;
    }
    return false;
}

// The cab's SCREEN displays rasterised as viewer/src/cab_screens lays them
// out (one PPM per screen, 1024 px per metre), to look at them
// without the viewer.
void write_screen_images(const core::Vfs& vfs, const trainplugin::LegacyCar& car, const formats::CabDescription* cab,
                         const dllhost::CarOutput* out, const std::string& folder) {
    if (!cab || !car.definition || !car.definition->engine) return;
    const auto shape = car.file.parent().resolve(car.definition->engine->cabin);
    if (!shape) return;
    viewer::CabScreens screens(*cab, *shape);
    std::vector<viewer::ScreenBatch> batches;
    screens.build(out, 0.0f, batches);
    std::map<std::string, formats::AceDecodeResult> textures;
    for (const auto& display : cab->displays) {
        if (display.type != formats::CabDisplayType::screen || !display.screen.present) continue;
        const glm::vec3 tl{display.screen.top_left[0], display.screen.top_left[1], display.screen.top_left[2]};
        const glm::vec3 tr{display.screen.top_right[0], display.screen.top_right[1], display.screen.top_right[2]};
        const glm::vec3 br{display.screen.bottom_right[0], display.screen.bottom_right[1], display.screen.bottom_right[2]};
        const glm::vec3 across = tr - tl, down = br - tr;
        const float width = glm::length(across), height = glm::length(down);
        if (width <= 0.0f || height <= 0.0f) continue;
        const int w = (std::max)(64, static_cast<int>(width * 1024.0f)), h = (std::max)(64, static_cast<int>(height * 1024.0f));
        std::vector<float> image(static_cast<std::size_t>(w) * h * 3, 0.08f);
        const glm::vec3 normal = glm::normalize(glm::cross(across, down));
        for (const auto& batch : batches) {
            auto& tex = textures[batch.texture.key()];
            if (!tex.ok() && tex.error.empty()) {
                tex = formats::decode_ace_rgba(vfs, batch.texture);
                if (!tex.ok() && tex.error.empty()) tex.error = "?";
            }
            if (!tex.ok()) continue;
            const auto& img = *tex.image;
            for (std::size_t v = 0; v + 2 < batch.vertices.size(); v += 3) {
                glm::vec2 p[3], t[3];
                bool on = true;
                for (int k = 0; k < 3; ++k) {
                    const auto& vertex = batch.vertices[v + static_cast<std::size_t>(k)];
                    const glm::vec3 d = vertex.position - tl;
                    if (std::abs(glm::dot(d, normal)) > 0.002f) on = false;
                    p[k] = {glm::dot(d, across) / (width * width) * static_cast<float>(w),
                            glm::dot(d, down) / (height * height) * static_cast<float>(h)};
                    t[k] = vertex.uv;
                }
                if (!on) continue;
                const float area = (p[1].x - p[0].x) * (p[2].y - p[0].y) - (p[2].x - p[0].x) * (p[1].y - p[0].y);
                if (std::abs(area) < 1e-6f) continue;
                const int x0 = (std::max)(0, static_cast<int>(std::floor((std::min)({p[0].x, p[1].x, p[2].x}))));
                const int x1 = (std::min)(w - 1, static_cast<int>(std::ceil((std::max)({p[0].x, p[1].x, p[2].x}))));
                const int y0 = (std::max)(0, static_cast<int>(std::floor((std::min)({p[0].y, p[1].y, p[2].y}))));
                const int y1 = (std::min)(h - 1, static_cast<int>(std::ceil((std::max)({p[0].y, p[1].y, p[2].y}))));
                for (int y = y0; y <= y1; ++y) {
                    for (int x = x0; x <= x1; ++x) {
                        const glm::vec2 q{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f};
                        const float b0 = ((p[1].x - q.x) * (p[2].y - q.y) - (p[2].x - q.x) * (p[1].y - q.y)) / area;
                        const float b1 = ((p[2].x - q.x) * (p[0].y - q.y) - (p[0].x - q.x) * (p[2].y - q.y)) / area;
                        const float b2 = 1.0f - b0 - b1;
                        if (b0 < 0.0f || b1 < 0.0f || b2 < 0.0f) continue;
                        const glm::vec2 uv = t[0] * b0 + t[1] * b1 + t[2] * b2;
                        const int tx = std::clamp(static_cast<int>(uv.x * static_cast<float>(img.width)), 0, static_cast<int>(img.width) - 1);
                        const int ty = std::clamp(static_cast<int>(uv.y * static_cast<float>(img.height)), 0, static_cast<int>(img.height) - 1);
                        const auto* px = &img.pixels[(static_cast<std::size_t>(ty) * img.width + static_cast<std::size_t>(tx)) * 4];
                        const float a = batch.blend ? px[3] / 255.0f : 1.0f;
                        auto* dst = &image[(static_cast<std::size_t>(y) * w + x) * 3];
                        for (int c = 0; c < 3; ++c) dst[c] = dst[c] * (1.0f - a) + px[c] / 255.0f * a;
                    }
                }
            }
        }
        const auto file = folder + "/screen_" + std::to_string(display.id) + ".ppm";
        std::ofstream ppm(file, std::ios::binary);
        ppm << "P6\n" << w << ' ' << h << "\n255\n";
        for (const float f : image) ppm.put(static_cast<char>(static_cast<unsigned char>(std::clamp(f, 0.0f, 1.0f) * 255.0f)));
        std::printf("screen %u (%s): %s\n", display.id, display.name.c_str(), file.c_str());
    }
}

} // namespace

// --brakes-survey: every TRAINSET .eng with Brakes.dll, and why "without
// Brakes.dll" keeps it loaded (trainplugin::brakes_dll_kept_reason): by
// family (the folder without its number), the .eng counted per reason.
int brakes_survey(const core::Vfs& vfs) {
    std::map<std::string, std::map<std::string, int>> families;
    std::map<std::string, int> totals;
    const auto top = core::VirtualPath::parse("TRAINS/TRAINSET");
    if (!top) return 1;
    for (const auto& folder : vfs.list(*top)) {
        if (!folder.directory) continue;
        const auto folder_name = std::string(folder.path.filename());
        for (const auto& entry : vfs.list(folder.path)) {
            const auto name = std::string(entry.path.filename());
            if (entry.directory || name.size() < 5 || core::fold_case(name.substr(name.size() - 4)) != ".eng") continue;
            const auto wagon = formats::load_wagon(vfs, folder_name, name.substr(0, name.size() - 4), true, simulation::StockRules::of(vfs));
            if (!wagon.ok() || !wagon.engine) continue;
            auto reason = trainplugin::brakes_dll_kept_reason(*wagon.engine);
            if (reason == "no Brakes.dll") continue;
            if (reason.empty()) reason = "openRTS's (not loaded)";
            auto family = folder_name;
            while (!family.empty() && (std::isdigit(static_cast<unsigned char>(family.back())) || family.back() == '-' || family.back() == '_'))
                family.pop_back();
            ++families[family][reason];
            ++totals[reason];
        }
    }
    for (const auto& [family, reasons] : families) {
        std::printf("%-28s", family.c_str());
        for (const auto& [reason, count] : reasons) std::printf("  %s: %d", reason.c_str(), count);
        std::printf("\n");
    }
    std::printf("\ntotal:");
    for (const auto& [reason, count] : totals) std::printf("  %s: %d", reason.c_str(), count);
    std::printf("\n");
    return 0;
}

int dll_check(const core::Vfs& vfs, const std::filesystem::path& root, const std::vector<std::string>& args) {
    if (args.empty()) {
        std::fprintf(stderr, "--dll <TRAINSET folder> <eng|wag>[!] ... [--script <file>] [--seconds <s>] [--start cold|on|running] [--rails dry|wet|snow|ice|leaves] [--cars [--light-defs]] [--triggers] [--body]\n");
        return 2;
    }
    const std::string folder = args[0];
    std::vector<std::string> names;
    std::string script_path;
    auto rails = simulation::RailCondition::rts;
    double seconds = 30.0;
    bool per_car = false;
    bool show_triggers = false; // --triggers: every SMS trigger a DLL posts
    bool show_anims = false;    // --anims: every change of a FreeAnimation the DLL leaves
    std::map<std::pair<std::size_t, std::size_t>, std::string> anims_seen; // (car, cab anims first, k) -> last line
    // --body: each car's body motion (simulation/body_motion.hpp), the most
    // of each second: its acceleration, the sway, bounce and surge (cm),
    // roll, pitch and yaw (mrad).
    bool show_body = false;
    bool show_screens = false;  // --screens: the cab screens' shown objects and texts, each second
    bool show_light_defs = false; // --light-defs: with --cars, each lit exterior light's .eng definition
    std::string screens_image;  // --screens-image <dir>: the cab screens as RTS lays them out, at the end (PPM)
    std::string record_path;    // --record <file>: the golden run (above)
    std::string record_library = "BRAKES";
    int start_state = -1; // --start cold|on|running
    // --air rts|dll|own: the realistic air with the DLL's valve (its УР), or
    // openRTS's 395 over Brakes.dll (docs/train.md); rts: as RTS.
    trainplugin::DllAir air = trainplugin::DllAir::rts;
    // --brakes openrts: openRTS's cab brake devices in place of Brakes.dll's
    // (docs/brakes-dll-replacement.md, step 2; the realistic air, --air own
    // unless given); --numbers books: the books' numbers, not Brakes.dll's.
    bool own_brakes = false;
    bool book_numbers = false;
    bool klub_shadow = false; // --klub shadow: openRTS's КЛУБ beside Brakes.dll's
    bool klub_own = false;    // --klub openrts: openRTS's КЛУБ in place of Brakes.dll's
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--script" && i + 1 < args.size()) {
            script_path = args[++i];
        } else if (args[i] == "--start" && i + 1 < args.size()) {
            const auto& value = args[++i];
            start_state = value == "cold" ? 0 : value == "on" ? 1 : -1;
        } else if (args[i] == "--air" && i + 1 < args.size()) {
            const auto& value = args[++i];
            air = value == "dll" ? trainplugin::DllAir::dll_valve : value == "own" ? trainplugin::DllAir::own_valve : trainplugin::DllAir::rts;
        } else if (args[i] == "--brakes" && i + 1 < args.size()) {
            own_brakes = args[++i] == "openrts";
        } else if (args[i] == "--brakes-dll" && i + 1 < args.size()) {
            // none: Brakes.dll not loaded where every part of it is openRTS's
            // (docs/brakes-dll-replacement.md "The rest", 7).
            trainplugin::LegacyConsist::set_without_brakes_dll(args[++i] == "none");
        } else if (args[i] == "--klub" && i + 1 < args.size()) {
            klub_shadow = args[i + 1] == "shadow";
            klub_own = args[++i] == "openrts";
        } else if (args[i] == "--numbers" && i + 1 < args.size()) {
            book_numbers = args[++i] == "books";
        } else if (args[i] == "--rails" && i + 1 < args.size()) {
            // The rails' state (adhesion.hpp): openRTS's traction limit; RTS's by default.
            const auto& value = args[++i];
            rails = value == "dry"    ? simulation::RailCondition::dry
                  : value == "wet"    ? simulation::RailCondition::wet
                  : value == "snow"   ? simulation::RailCondition::snow
                  : value == "ice"    ? simulation::RailCondition::ice
                  : value == "leaves" ? simulation::RailCondition::leaves
                                      : simulation::RailCondition::rts;
        } else if (args[i] == "--cars") {
            per_car = true;
        } else if (args[i] == "--triggers") {
            show_triggers = true;
        } else if (args[i] == "--anims") {
            show_anims = true;
        } else if (args[i] == "--body") {
            show_body = true;
        } else if (args[i] == "--screens") {
            show_screens = true;
        } else if (args[i] == "--light-defs") {
            show_light_defs = true;
        } else if (args[i] == "--record" && i + 1 < args.size()) {
            record_path = args[++i];
        } else if (args[i] == "--record-library" && i + 1 < args.size()) {
            record_library = args[++i];
        } else if (args[i] == "--screens-image" && i + 1 < args.size()) {
            screens_image = args[++i];
        } else if (args[i] == "--seconds" && i + 1 < args.size()) {
            seconds = std::atof(args[++i].c_str());
        } else {
            names.push_back(args[i]);
        }
    }
    std::string error;
    std::vector<Command> script;
    bool want_aux_vars = !record_path.empty(); // BRAKES' variable stack in the outputs
    if (!script_path.empty()) {
        script = read_script(script_path, error);
        if (!error.empty()) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 2;
        }
        for (const auto& c : script) seconds = (std::max)(seconds, c.time + 1.0);
        want_aux_vars = true;
    }

    // The cars: <folder> <names...>, or "consist" <consist name> as RTS looks it up.
    struct Entry {
        std::string folder, name;
        bool flipped = false;
    };
    std::vector<Entry> entries;
    if (folder == "consist" && !names.empty()) {
        const auto consist = formats::load_consist(vfs, names.front());
        if (!consist.ok()) {
            std::fprintf(stderr, "consist %s: %s\n", names.front().c_str(), consist.error.c_str());
            return 2;
        }
        for (const auto& e : consist.entries) entries.push_back({e.folder, e.name, e.flipped});
        names.clear();
        for (const auto& e : entries) names.push_back(e.name);
    } else {
        // A name "<folder>/<name>" takes its car from another folder (a
        // locomotive with the cars of a passenger train).
        for (auto name : names) {
            const bool flipped = !name.empty() && name.back() == '!';
            if (flipped) name.pop_back();
            if (const auto slash = name.find('/'); slash != std::string::npos) {
                entries.push_back({name.substr(0, slash), name.substr(slash + 1), flipped});
            } else {
                entries.push_back({folder, name, flipped});
            }
        }
    }

    std::vector<std::unique_ptr<formats::WagonDefinition>> stock;
    std::vector<simulation::CarSetup> setups;
    std::vector<trainplugin::LegacyCar> legacy;
    for (const auto& entry : entries) {
        const auto& name = entry.name;
        const bool flipped = entry.flipped;
        bool engine = true;
        auto path = formats::wagon_path(entry.folder, name, true);
        if (!vfs.exists(path)) {
            engine = false;
            path = formats::wagon_path(entry.folder, name, false);
        }
        auto def = std::make_unique<formats::WagonDefinition>(formats::load_wagon(vfs, entry.folder, name, engine, simulation::StockRules::of(vfs)));
        if (!def->ok()) {
            std::fprintf(stderr, "%s/%s: %s\n", entry.folder.c_str(), name.c_str(), def->error.c_str());
            return 2;
        }
        setups.push_back({def.get(), flipped, 0.0f});
        const auto n = static_cast<std::uint32_t>(legacy.size() + 1);
        legacy.push_back({def.get(), path, n, n}); // uid, key
        stock.push_back(std::move(def));
    }
    const auto cache = std::filesystem::temp_directory_path() / "openrts-cache";
    auto consist = trainplugin::LegacyConsist::create(vfs, cache, legacy, root / "RTS", &error);
    if (consist && consist->without_brakes_dll()) std::printf("Brakes.dll not loaded on %zu cars\n", consist->without_brakes_dll());
    if (!consist) {
        std::fprintf(stderr, "legacy DLLs: %s\n", error.c_str());
        return 2;
    }
    for (const auto& m : consist->modules())
        std::printf("module %s: %s, TS_VERSION %u\n", trainplugin::utf16_to_utf8(m.path).c_str(),
                    m.status == 0 ? "loaded" : m.status > 0 ? "not loaded (openRTS's devices)" : "REFUSED", m.version[1]);

    simulation::Train train(setups);
    train.set_rails(rails);
    if (show_body) train.set_body_motion(simulation::BodyMotionModel::openrts); // the game's by default
    auto network = simulation::straight_test_track(30000.0);
    if (!network || !train.place(*network, 1, 15000.0, true, &error)) {
        std::fprintf(stderr, "cannot place the train: %s\n", error.c_str());
        return 2;
    }
    std::size_t player = SIZE_MAX;
    for (std::size_t i = 0; i < legacy.size(); ++i) {
        if (consist->has_dll(i)) {
            train.set_controller(i, consist->controller(i));
            if (player == SIZE_MAX) player = i;
        }
    }
    if (own_brakes && air == trainplugin::DllAir::rts) air = trainplugin::DllAir::own_valve;
    consist->set_air(air);
    consist->set_own_brakes(own_brakes, book_numbers);
    if (klub_own && air == trainplugin::DllAir::rts) air = trainplugin::DllAir::own_valve; // it needs the realistic air
    consist->set_air(air);
    consist->set_own_klub(klub_own);
    if (klub_own) std::printf("openRTS's КЛУБ in place of Brakes.dll's\n");
    if (air != trainplugin::DllAir::rts) {
        train.set_brake_model(simulation::BrakeModel::realistic);
        std::printf("realistic air, %s\n", air == trainplugin::DllAir::own_valve ? "openRTS's valve over Brakes.dll" : "the DLL's valve");
    }
    if (own_brakes) {
        std::printf("openRTS's cab brake devices, %s numbers\n", book_numbers ? "the books'" : "Brakes.dll's");
        for (std::size_t i = 0; i < legacy.size(); ++i) {
            const auto* definition = train.cars()[i].definition;
            if (!definition || !definition->engine) continue;
            const auto parameters = trainplugin::brakes_parameters(*definition->engine);
            if (!parameters.present) continue;
            if (parameters.understood()) std::printf("  car %zu: openRTS's\n", i);
            else std::printf("  car %zu keeps Brakes.dll: %s\n", i, parameters.unknown.c_str());
        }
    }
    // Its air from Init, as the game's first entering (RTS's).
    if (!trainplugin::enter_new_train(*consist, player, train, true, start_state)) {
        std::fprintf(stderr, "enter failed: %s\n%s", consist->error().c_str(), consist->take_messages().c_str());
        return 2;
    }
    std::printf("player in car %zu (%s)\n", player, names[player].c_str());

    trainplugin::LegacyWorld world;
    world.record_aux_vars = want_aux_vars;
    // A scripted line for the КЛУБ's golden runs (docs/brakes-dll-replacement.md,
    // step 3.2): signals and speed posts at metres from where the train's
    // front starts. The ALSN's arrays, the train's limits and the next speed
    // post follow the front as the train runs (RTS 0x508520's, simplified:
    // one NORMAL head per signal, nothing behind the train).
    struct LineSignal {
        std::string name;
        double at{};
        std::uint32_t aspect{};
        float limit = -1.0f; // m/s, -1 none
    };
    struct LinePost {
        double at{};
        float limit{}; // m/s
    };
    std::vector<LineSignal> line_signals;
    std::vector<LinePost> line_posts;
    bool line = false;
    double front = 0.0, last_front = 0.0; // the train's front along the line, m
    float line_limit = 120.0f / 3.6f;     // before the first post
    float signal_limit = -1.0f;           // the last signal passed's
    const auto fill_line = [&] {
        if (!line) return;
        world.alsn_ahead.clear();
        world.alsn_passing.clear();
        world.signals_ahead.clear();
        std::sort(line_signals.begin(), line_signals.end(), [](const auto& a, const auto& b) { return a.at < b.at; });
        std::sort(line_posts.begin(), line_posts.end(), [](const auto& a, const auto& b) { return a.at < b.at; });
        for (const auto& s : line_signals) {
            dllhost::SignalInput in;
            in.distance = static_cast<float>(s.at - front);
            in.aspect[0] = s.aspect;
            in.flags = 1; // a NORMAL head
            in.speed_limit = s.limit;
            if (s.at > front) {
                if (world.alsn_ahead.size() < 16) world.alsn_ahead.push_back(in);
            } else if (s.at > last_front) {
                world.alsn_passing.push_back(in);
                signal_limit = s.limit;
            }
        }
        if (!world.alsn_ahead.empty()) world.signals_ahead.push_back(world.alsn_ahead.front());
        float limit = line_limit;
        const LinePost* next = nullptr;
        for (const auto& p : line_posts) {
            if (p.at <= front) limit = p.limit;
            else if (!next) next = &p;
        }
        world.next_limit = next ? next->limit : -1.0f;
        world.next_limit_distance = next ? static_cast<float>(next->at - front) : -1.0f;
        world.route = {limit, world.next_limit, world.next_limit_distance};
        world.signal = {signal_limit, -1.0f, -1.0f};
    };
    // press: the keys still to go, and the one down this step.
    std::deque<std::uint32_t> presses;
    std::optional<std::uint32_t> pressed;
    // --klub shadow (docs/brakes-dll-replacement.md, step 3.3): openRTS's
    // КЛУБ (simulation/klub.hpp) on the same inputs as Brakes.dll's, set
    // against its variables every step.
    trainplugin::KlubParameters klub;
    trainplugin::RecorderParameters recorder;
    if (klub_shadow) {
        if (const auto* d = train.cars()[player].definition; d && d->engine) klub = trainplugin::klub_parameters(*d->engine);
        if (!klub.present) {
            std::fprintf(stderr, "--klub shadow: the player's engine has no Brakes.dll\n");
            return 2;
        }
        world.record_aux_vars = true;
        if (const auto* d = train.cars()[player].definition; d && d->engine) recorder = trainplugin::recorder_parameters(*d->engine);
        if (recorder.own)
            std::printf("openRTS's speed recorder beside Brakes.dll's: SL_SETUP %u, %zu cabs\n", recorder.settings.setup,
                        recorder.settings.cabs.size());
        std::printf("openRTS's КЛУБ beside Brakes.dll's: PB_SETUP %u, ALSN_DELAY %.1f, KLUB_GRSPD %.0f, %zu cabs, %zu keys\n",
                    klub.settings.setup, klub.settings.alsn_delay, klub.settings.green_speed, klub.cabs.size(), klub.cabs[0].keys.size());
    }
    simulation::KlubState klub_state;
    simulation::KlubOutput klub_out;
    std::vector<simulation::KlubScreen> klub_screens(klub.cabs.size()); // a cab's each
    bool klub_alsn_on = false; // Engine::ALSNOn: the ЭПК key last turned, of any cab
    bool klub_799_settling = false; // command 799's speed entered, the screen not redrawn since
    bool klub_track_settling = false; // a track's sign entered, the screen not redrawn since
    float klub_last_ur = 0.0f, klub_last_gauge = 0.0f; // the last step's (updateKLUB draws before the brakes run)
    std::vector<simulation::KlubPress> klub_keys;
    std::vector<simulation::SautKey> saut_keys; // САУТ's, the working cab's
    std::uint32_t saut_last_var4 = 0;          // as the step before left it (САУТ reads it first)
    std::uint32_t saut_last_var88 = 0;
    // The speed recorder's (SL_SPEED1): its state, what Run read of the last step.
    simulation::RecorderState recorder_state;
    simulation::RecorderOutput recorder_out;
    bool recorder_started = false;
    std::uint32_t recorder_last_var4 = 0;
    float recorder_last_ep = 0.0f;
    // The cab's switches as they stood in the step before: what Brakes.dll
    // read during this step's Run (RTS moves them after the DLLs' Run).
    std::vector<dllhost::SwitchState> klub_switches;
    struct ShadowDiff {
        std::size_t steps{};
        double first{};
        double golden{}, ours{};
    };
    std::map<std::string, ShadowDiff> shadow_diffs;
    std::size_t shadow_steps = 0, shadow_printed = 0;
    // A switch the script moved: the КЛУБ's events.
    const auto operated = [&](const std::uint32_t id, const std::uint32_t state) {
        if (!klub_shadow) return;
        const auto i = static_cast<std::int32_t>(id);
        // The working cab's keys only (Brakes.dll's SwitchKLUB).
        const auto* now = consist->output(player);
        const int cab = now ? klub.working_cab(now->switches) : 0;
        if (state != 0 && cab >= 0) {
            if (const auto* press = klub.cabs[static_cast<std::size_t>(cab)].key(i)) klub_keys.push_back(*press);
            const auto& saut = klub.cabs[static_cast<std::size_t>(cab)].saut_keys;
            for (std::size_t k = 0; k < saut.size(); ++k)
                if (saut[k] >= 0 && saut[k] == i) saut_keys.push_back(static_cast<simulation::SautKey>(k));
        }
    };
    // One step of openRTS's КЛУБ after the host's, on what Brakes.dll had.
    const auto shadow_step = [&](const double now) {
        const auto* out = consist->output(player);
        const int lib = consist->aux_library(player, "BRAKES");
        if (!out || lib < 0 || static_cast<std::size_t>(lib) >= out->aux_vars.size()) return;
        const auto& var = out->aux_vars[static_cast<std::size_t>(lib)].var;
        const auto at = [&](const std::size_t offset) { return offset / 4 < var.size() ? var[offset / 4] : 0.0f; };
        const auto bits = [&](const std::size_t offset) {
            std::uint32_t u = 0;
            const float v = at(offset);
            std::memcpy(&u, &v, sizeof u);
            return u;
        };
        const auto state_in = [](const std::vector<dllhost::SwitchState>& list, const std::int32_t id) {
            return id >= 0 && static_cast<std::size_t>(id) < list.size() ? list[static_cast<std::size_t>(id)].state : 0u;
        };
        const bool first_step = klub_switches.empty();
        if (first_step) klub_switches = out->switches; // the first step: as they stand
        const auto on = [&](const std::int32_t id) { return state_in(klub_switches, id) != 0; };
        // Switched after this step's Run: a switch whose state changed.
        const auto moved = [&](const std::int32_t id) { return state_in(klub_switches, id) != state_in(out->switches, id); };
        std::vector<simulation::KlubSignal> signals;
        for (const auto& g : world.alsn_ahead) {
            simulation::KlubSignal k;
            k.distance = g.distance;
            std::memcpy(k.aspect, g.aspect, sizeof k.aspect);
            k.flags = g.flags;
            k.speed_limit = g.speed_limit;
            signals.push_back(k);
        }
        simulation::KlubInputs in;
        in.dt = static_cast<float>(simulation::train_step);
        in.speed = static_cast<float>(train.speed());
        in.signals = signals;
        // Cabin::Signal before ALSN (dllhost Host::signals): the leading cab's.
        if (!world.signals_ahead.empty()) {
            const auto& g = world.signals_ahead.front();
            in.cab_signal.distance = g.distance;
            std::memcpy(in.cab_signal.aspect, g.aspect, sizeof in.cab_signal.aspect);
            in.cab_signal.flags = g.flags;
            in.cab_signal.speed_limit = g.speed_limit;
        } else {
            in.cab_signal = {};
            in.cab_signal.speed_limit = -1.0f;
        }
        float limit = -1.0f;
        for (const float v : {world.route.limit, world.signal.limit, world.event.limit})
            if (v >= 0.0f && (limit < 0.0f || v < limit)) limit = v;
        in.limit = limit;
        in.next_limit = world.next_limit;
        in.next_limit_distance = world.next_limit_distance;
        // Powered unless one of its switches is there and at 0 (0x4015A4).
        in.powered = true;
        for (const auto id : klub.power) {
            const bool there = id >= 0 && static_cast<std::size_t>(id) < klub_switches.size();
            in.powered = in.powered && (!there || on(id));
        }
        // The working cab's devices; the РБ, the РБС and the ЭПК key of any
        // (Switched), the key last turned setting Engine::ALSNOn.
        const int working = klub.working_cab(klub_switches);
        const auto& wc = klub.cabs[static_cast<std::size_t>(working < 0 ? 0 : working)];
        if (first_step) {
            klub_alsn_on = false;
            for (const auto& c : klub.cabs) klub_alsn_on = klub_alsn_on || on(c.epk_key);
        }
        in.epk_key = klub_alsn_on;
        for (const auto& c : klub.cabs) {
            in.rb_held = in.rb_held || on(c.rb);
            in.rb = in.rb || moved(c.rb);
            in.rbs = in.rbs || moved(c.rbs);
            if (moved(c.epk_key)) {
                in.epk_key_turned = true;
                in.epk_key_to = state_in(out->switches, c.epk_key) != 0;
            }
        }
        if (in.epk_key_turned) klub_alsn_on = in.epk_key_to;
        // ТСКБМ's switch of any cab.
        for (const auto& c : klub.cabs) in.tskbm = in.tskbm || on(c.tskbm);
        in.alsn_on = out->engine.alsn_on;
        // Any switch moved after the step's Run (Switched): ТСКБМ's timer again.
        for (std::size_t k = 0; k < out->switches.size() && !in.switched; ++k)
            in.switched = k < klub_switches.size() && klub_switches[k].state != out->switches[k].state;
        in.saut = (bits(4) & 0x40u) != 0; // САУТ's own "on" (Brakes.dll's)
        in.saut_permitted = at(256);
        in.wheelslip = out->engine.wheelslip != 0;
        const auto& car = train.cars()[player];
        // openRTS's САУТ (PB_SETUP 1) beside Brakes.dll's, on what it read:
        // as the step before left them.
        std::vector<simulation::SautInputs::Cab> saut_cabs;
        // Its "announced" (0x20000): Brakes.dll's vigilance lets it go on the
        // РБ (Switched); САУТ only sets it.
        if ((klub.settings.setup & 1) && (saut_last_var88 & 0x20000u) && !(bits(88) & 0x20000u)) klub_state.saut_flags &= ~0x20000u;
        saut_last_var88 = bits(88);
        if (klub.settings.setup & 1) {
            auto& u = in.saut_in;
            u.own = true;
            u.powered = true;
            for (const auto id : klub.saut_power)
                u.powered = u.powered && (id < 0 || static_cast<std::size_t>(id) >= klub_switches.size() || on(id));
            u.high = klub.saut_mode || (klub.saut_mode_switch >= 0 && state_in(klub_switches, klub.saut_mode_switch) == klub.saut_mode_on);
            u.clock = (klub.settings.setup & 16) != 0;
            u.acceleration = car.acceleration;
            u.cylinder = car.cylinder;
            u.ept = (out->engine.systems & 2u) != 0;
            u.brake_bits = saut_last_var4 & 0x180u;
            u.reverser = out->engine.reverse;
            u.traction = out->engine.throttle != 0;
            u.equalising = klub_last_ur;
            u.milepost = out->engine.milepost;
            u.keys = saut_keys;
            for (std::size_t c = 0; c < klub.cabs.size(); ++c)
                saut_cabs.push_back({.working = static_cast<int>(c) == working,
                                     .on = on(klub.cabs[c].saut),
                                     .os = on(klub.cabs[c].saut_os),
                                     .cassette = on(klub.cabs[c].cassette),
                                     .turned_on = moved(klub.cabs[c].saut) && state_in(out->switches, klub.cabs[c].saut) != 0,
                                     .rb_moved = moved(klub.cabs[c].rb)});
            u.cabs = saut_cabs;
            u.flip = klub.flip;
        }
        in.cylinder = (std::max)(car.cylinder, car.independent);
        in.day_time = world.day_time;
        in.cassette = on(wc.cassette);
        in.keys = klub_keys;
        // The ЭПК key turned on at Init (the train's DLL sets it: Switched,
        // its 903 and the КЛУБ's beep in the first step).
        if (shadow_steps == 0 && in.epk_key) in.epk_key_turned = in.epk_key_to = true;
        simulation::step_klub(klub_state, klub.settings, in, klub_out);
        klub_keys.clear();
        saut_keys.clear();
        saut_last_var4 = bits(4);
        // openRTS's speed recorder (simulation/recorder.hpp) on what
        // Brakes.dll's read: the aspect after the ALSN delay, the step
        // before's values and this one's ALSN.
        if (recorder.own) {
            if (!recorder_started) {
                recorder_started = true;
                recorder_state.distance = at(224) - static_cast<float>(std::abs(train.speed()) * simulation::train_step);
                recorder_last_var4 = bits(4);
                recorder_last_ep = at(40);
            }
            std::vector<simulation::RecorderCabInputs> cabs;
            for (const auto& c : recorder.settings.cabs) {
                simulation::RecorderCabInputs ci;
                ci.epk_key = on(c.epk_key);
                ci.als = on(c.als);
                ci.rb = on(c.rb);
                ci.rbs = on(c.rbs);
                ci.h = on(c.h);
                ci.dvij = on(c.dvij);
                ci.kid = on(c.kid);
                const auto after = [&](const std::int32_t id) { return state_in(out->switches, id); };
                ci.rb_pressed = moved(c.rb) && after(c.rb) != 0;
                ci.rbs_pressed = moved(c.rbs) && after(c.rbs) != 0;
                ci.vk_pressed = moved(c.vk) && after(c.vk) != 0;
                ci.zavod_moved = moved(c.zavod);
                ci.epk_key_moved = moved(c.epk_key);
                ci.epk_key_to = after(c.epk_key) != 0;
                cabs.push_back(ci);
            }
            simulation::RecorderInputs ri;
            ri.dt = static_cast<float>(simulation::train_step);
            ri.speed = static_cast<float>(train.speed());
            const auto& rcar = train.cars()[player];
            ri.cylinder = rcar.cylinder;
            ri.independent = rcar.independent;
            ri.main_reservoir = rcar.main_reservoir;
            ri.reverser = out->engine.reverse;
            ri.traction = out->engine.throttle != 0;
            ri.aspect = bits(72);
            ri.powered = (bits(32) & 4u) != 0;
            ri.tskbm_on = (recorder_last_var4 & 8u) != 0;
            ri.bypass = (recorder_last_var4 & 0x200u) != 0;
            ri.ep_pressure = recorder_last_ep;
            ri.day_time = world.day_time;
            ri.cabs = cabs;
            simulation::step_recorder(recorder_state, recorder.settings, ri, recorder_out);
            simulation::recorder_switched(recorder_state, recorder.settings, ri, recorder_out);
            recorder_last_var4 = bits(4);
            recorder_last_ep = at(40);
        }
        // Each cab's ТСКБМ switch as this step's Run saw it (before Switched).
        std::vector<bool> tskbm_on(klub.cabs.size());
        for (std::size_t c = 0; c < klub.cabs.size(); ++c) tskbm_on[c] = on(klub.cabs[c].tskbm);
        klub_switches = out->switches;
        ++shadow_steps;
        // Set against Brakes.dll's (docs/reverse/brakes-dll.md §11's offsets).
        // While command 799 waits for its speed openRTS's КЛУБ is not on
        // white yet and Brakes.dll's is (KlubState::white_799_entry), and
        // once it is entered until its screen redraws (the lamps, the beep
        // of the new aspect): only the entry is set against it then. So
        // too while a track is entered and waits for its sign (Brakes.dll's
        // is the ЭК's at once; KlubState::pending_track), the entry too
        // (ours shows the last track and sign, Brakes.dll's 0), and until
        // the screen redraws after it.
        if (klub_state.white_799_entry) klub_799_settling = true;
        else if (klub_799_settling && klub_state.redraw == 0.0f) klub_799_settling = false;
        const bool track_pending = ((klub_state.flags & simulation::klub_flag::right_entry) && klub_state.pending_track)
                                   || (klub_state.flags & simulation::klub_flag::track_entry);
        if (track_pending) klub_track_settling = true;
        else if (klub_track_settling && klub_state.redraw == 0.0f) klub_track_settling = false;
        const bool own_entry = klub_state.white_799_entry || klub_799_settling || track_pending || klub_track_settling;
        const auto differ = [&](const char* name, const double golden, const double ours, const double within) {
            if (own_entry && (track_pending || std::strcmp(name, "entry") != 0)) return;
            if (std::abs(golden - ours) <= within) return;
            auto& d = shadow_diffs[name];
            if (d.steps++ == 0) {
                d.first = now;
                d.golden = golden;
                d.ours = ours;
            }
            if (shadow_printed < 12) {
                ++shadow_printed;
                std::printf("%6.2f КЛУБ %s: Brakes.dll %g, openRTS %g\n", now, name, golden, ours);
            }
        };
        // The speed recorder against Brakes.dll's.
        if (recorder.own) {
            const auto& r = recorder_state;
            namespace rf = simulation::recorder_flag;
            differ("rec_periodic", at(44), r.periodic, 0.001);
            differ("rec_aspect", at(48), r.aspect_seen, 0.0);
            differ("rec_whistle", at(52), r.whistle, 0.0);
            differ("rec_blink", at(56), r.blink, 0.001);
            differ("rec_kon", at(16), r.kon_timer, 0.001);
            differ("rec_vented", at(20), r.vented, 0.0);
            differ("rec_needle", at(36), r.needle_timer, 0.001);
            differ("rec_distance", at(224), r.distance, 0.0);
            differ("rec_flags", bits(4) & rf::mask, r.flags & rf::mask, 0.0);
            differ("rec_whistle_flag", bits(8) & 0x10000000u, r.whistle_flag ? 0x10000000u : 0u, 0.0);
            differ("rec_epk_bit", bits(32) & 1u, r.epk_bit ? 1u : 0u, 0.0);
            differ("rec_alsn_on", out->engine.alsn_on, r.alsn_on ? 1 : 0, 0.0);
            if (recorder_out.cab_aspect >= 0) differ("rec_cab_aspect", out->cab_aspect, recorder_out.cab_aspect, 0.0);
            // Its cab objects: the needle within its error (−1.67 .. +0.28 m/s).
            for (const auto& [id, v] : recorder_out.values) {
                if (id < 0 || static_cast<std::size_t>(id) >= out->displays.size()) continue;
                bool needle = false;
                for (const auto& c : recorder.settings.cabs) needle = needle || c.speed == id;
                differ(("rec_value" + std::to_string(id)).c_str(), out->displays[static_cast<std::size_t>(id)].value, v, needle ? 1.7 : 0.001);
            }
            for (const auto& [id, v] : recorder_out.states) {
                if (id < 0 || static_cast<std::size_t>(id) >= out->displays.size()) continue;
                differ(("rec_state" + std::to_string(id)).c_str(), out->displays[static_cast<std::size_t>(id)].state, v, 0.0);
            }
            // Its sounds, in order (Brakes.sms, or the locomotive's with TRANS_SOUND).
            const auto rec_code = [](const std::uint16_t c) { return c == 56 || c == 57 || c == 106 || c == 970 || c == 171; };
            std::vector<std::uint16_t> golden;
            if (static_cast<std::size_t>(lib) < out->aux_sound.size())
                for (const auto c : out->aux_sound[static_cast<std::size_t>(lib)].triggers)
                    if (rec_code(c)) golden.push_back(c);
            if (golden.empty() && recorder.trans_sound)
                for (const auto c : out->sound_ext)
                    if (rec_code(c)) golden.push_back(c);
            if (golden != recorder_out.sounds) {
                double g = 0, o = 0;
                for (const auto c : golden) g = g * 10000.0 + c;
                for (const auto c : recorder_out.sounds) o = o * 10000.0 + c;
                differ("rec_sounds", g, o, 0.0);
            }
        }
        if (klub.setup_index < 0 || !(klub.settings.setup & 2u)) return; // no КЛУБ: the rest is its
        const auto& s = klub_state;
        constexpr std::uint32_t flags_compared = 0x1 | 0x8 | 0x10 | 0x40 | 0x80 | 0x400 | 0x800 | 0x1000 | 0x2000 | 0x8000 | 0x10000 |
                                                 0x80000 | 0x400000 | 0x800000 | 0x1000000 | 0x2000000 | 0x4000000 | 0x10000000;
        differ("aspect", out->cab_aspect, s.signal.aspect[0], 0.0);
        differ("limit", at(80), s.limit, 0.01);
        differ("target", at(116), s.target_speed, 0.01);
        differ("target_m", at(120), s.target_distance, 0.05);
        differ("permitted", at(140), s.permitted, 0.01);
        differ("timer", at(24), s.timer, 0.06);
        differ("flags", bits(8) & flags_compared, s.flags & flags_compared, 0.0);
        differ("kon", bits(4) & 0x400, s.device & 0x400, 0.0);
        // ТСКБМ: its timer, on, whistle, warning.
        differ("tskbm_timer", at(12), s.tskbm_timer, 0.06);
        differ("tskbm_bits", bits(4) & 0x402008u, s.device & 0x402008u, 0.0);
        differ("track", bits(160), s.track, 0.0);
        differ("wagons", bits(176), s.wagons, 0.0);
        differ("entry", bits(132), s.entry, 0.0);
        differ("epk_bit", bits(32) & 1, s.epk_bit ? 1 : 0, 0.0);
        // САУТ: on, braking, its flags, target,
        // shown speed, distance, mode's metres, coefficient, its timer, the
        // aspect announced.
        if (klub.settings.setup & 1) {
            differ("saut_on", bits(4) & 0x40u, s.device & 0x40u, 0.0);
            differ("saut_braking", bits(32) & 8u, s.saut_braking ? 8u : 0u, 0.0);
            differ("saut_flags", bits(88) & 0x3BFFFu, s.saut_flags & 0x3BFFFu, 0.0);
            differ("saut_target", at(100), s.saut_target, 0.01);
            differ("saut_shown", at(256), s.saut_shown, 0.01);
            differ("saut_distance", at(308), s.saut_distance, 0.05);
            differ("saut_mode_m", at(104), s.saut_mode_distance, 0.0);
            differ("saut_coefficient", out->engine.flags, s.saut_coefficient, 0.0);
            differ("saut_timer", at(252), s.saut_coefficient_timer, 0.06);
            differ("saut_aspect", bits(124), s.saut_aspect, 0.0);
            differ("saut_clock", at(288), s.saut_clock, 0.001);
        }
        // The screens, drawn from the КЛУБ's state, against Brakes.dll's
        // (simulation/klub_screen.hpp): each cab's (updateKLUB draws them
        // all), named c<n>. when there are more.
        for (std::size_t c = 0; c < klub.cabs.size(); ++c) {
            const auto& ids = klub.cabs[c];
            auto& drawn = klub_screens[c];
            const std::string prefix = klub.cabs.size() > 1 ? "c" + std::to_string(c + 1) + "." : std::string();
            simulation::KlubScreenInputs si;
            si.powered = in.powered;
            si.epk_key = in.epk_key;
            si.working_cab = static_cast<int>(c) == working;
            si.speed = in.speed;
            si.direction = klub.flip == static_cast<std::int32_t>(c) ? -1 : 1; // PB_FLIP is the cab (from 0) whose forward is the other way
            si.pipe = klub_last_gauge > 0.0f ? klub_last_gauge : car.pipe;
            si.cylinder = in.cylinder;
            si.equalising = klub_last_ur;
            si.acceleration = car.acceleration;
            si.line_frequency = world.line_frequency;
            si.wheelslip = in.wheelslip;
            si.day_time = world.day_time;
            si.cassette = on(ids.cassette);
            si.cassette_given = ids.cassette >= 0;
            si.saut_coefficient = ids.saut >= 0 && on(ids.saut) ? static_cast<float>(out->engine.flags) * 0.01f : -1.0f;
            si.tskbm_timer = klub_state.tskbm_shown;
            si.tskbm_switch = tskbm_on[c];
            si.block_value = at(296);
            si.saut_distance = at(308);
            si.milepost = out->engine.milepost;
            si.saut_caught = (klub_state.saut_flags & 0x100u) != 0;
            simulation::draw_klub(klub_state, klub.settings, si, drawn);
            const auto compare = [&](const std::int32_t id, std::span<const simulation::KlubScreenObject> objects, const char* what) {
                if (id < 0) return;
                const formats::CabDisplay* display = nullptr;
                if (const auto* cab_now = consist->cab(player))
                    for (const auto& dd : cab_now->displays)
                        if (static_cast<std::int32_t>(dd.id) == id) display = &dd;
                for (const auto& screen : out->screens) {
                    if (static_cast<std::int32_t>(screen.display) != id) continue;
                    for (std::size_t k = 0; k < objects.size() && k < screen.objects.size(); ++k) {
                        const auto& o = objects[k];
                        const auto& g = screen.objects[k];
                        const auto name = [&](const char* field) { return prefix + what + std::to_string(k) + "." + field; };
                        if (o.state_set) differ(name("state").c_str(), g.state, o.state, 0.0);
                        if (o.value_set) differ(name("value").c_str(), g.value, o.value, 0.001);
                        if (o.blink_set) differ(name("blink").c_str(), g.fade[2], o.blink, 0.001);
                        const auto* def = display && k < display->screen.objects.size() ? &display->screen.objects[k] : nullptr;
                        if (o.text_set && k < screen.texts.size() && (!def || (def->char_width[1] > 0.0f && def->max_length > 0))) {
                            auto ours = o.text;
                            if (def && ours.size() > def->max_length) ours.resize(def->max_length);
                            if (ours != screen.texts[k] && !own_entry) {
                                if (shadow_printed < 30) {
                                    std::printf("%6.2f КЛУБ %s: Brakes.dll '%s', openRTS '%s'\n", now, name("text").c_str(),
                                                trainplugin::utf16_to_utf8(screen.texts[k]).c_str(), trainplugin::utf16_to_utf8(ours).c_str());
                                }
                                differ(name("text").c_str(), 0.0, 1.0, 0.0);
                            }
                        }
                    }
                }
            };
            compare(ids.screen, drawn.bil, "bil");
            compare(ids.dals, drawn.dals, "dals");
            compare(ids.dpom, drawn.dpom, "dpom");
            for (std::size_t k = 0; k < ids.lamps.size(); ++k) {
                const auto id = ids.lamps[k];
                if (id < 0 || static_cast<std::size_t>(id) >= out->displays.size()) continue;
                differ((prefix + "lamp" + std::to_string(k + 1)).c_str(), out->displays[static_cast<std::size_t>(id)].state, drawn.lamps[k], 0.0);
            }
            // ТСКБМ's display, lamps and L2's blink.
            if (klub.settings.setup & 4) {
                compare(ids.tskbm_screen, drawn.tskbm, "tskbm");
                const auto lamp = [&](const char* name, const std::int32_t id, const double ours) {
                    if (id < 0 || static_cast<std::size_t>(id) >= out->displays.size()) return;
                    differ((prefix + name).c_str(), out->displays[static_cast<std::size_t>(id)].state, ours, 0.0);
                };
                lamp("tskbm_pit", ids.tskbm_power, drawn.tskbm_power);
                lamp("tskbm_l1", ids.tskbm_lamps[0], drawn.tskbm_lamps[0]);
                lamp("tskbm_l2", ids.tskbm_lamps[1], drawn.tskbm_lamps[1]);
                lamp("tskbm_l3", ids.tskbm_lamps[2], drawn.tskbm_lamps[2]);
                if (const auto l2 = ids.tskbm_lamps[1]; l2 >= 0 && static_cast<std::size_t>(l2) < out->displays.size())
                    differ((prefix + "tskbm_l2_blink").c_str(), out->displays[static_cast<std::size_t>(l2)].fade[2], drawn.tskbm_l2_blink, 0.001);
            }
            // САУТ's displays (SAUT_DBID, SAUT_DFID) as its step drew them.
            if ((klub.settings.setup & 1) && c < klub_out.saut_screens.size()) {
                const auto as_klub = [](std::span<const simulation::SautObject> objects) {
                    std::vector<simulation::KlubScreenObject> list(objects.size());
                    for (std::size_t k = 0; k < objects.size(); ++k) {
                        list[k].state = objects[k].state;
                        list[k].state_set = objects[k].state_set;
                        list[k].blink = objects[k].blink;
                        list[k].blink_set = objects[k].blink_set;
                        list[k].text = objects[k].text;
                        list[k].text_set = objects[k].text_set;
                    }
                    return list;
                };
                compare(ids.saut_db, as_klub(klub_out.saut_screens[c].db), "saut_db");
                compare(ids.saut_df, as_klub(klub_out.saut_screens[c].df), "saut_df");
            }
        }
        klub_last_ur = out->engine.equalising;
        klub_last_gauge = at(28);
        // The КЛУБ's sounds of Brakes.sms, in order; САУТ's apart.
        const auto klub_code = [](const std::uint16_t c) {
            return c == 56 || c == 57 || (c >= 3192 && c <= 3199) || c == 3956 || c == 3957;
        };
        const auto saut_code = [](const std::uint16_t c) { return (c >= 3200 && c <= 3223) || c == 3250 || c == 3251; };
        // Without its own SMS, on the locomotive's (TRANS_SOUND 1, or the
        // train's DLL's var + 32 bit 0x100): its outside SMS's.
        const bool to_loco = klub.trans_sound || (static_cast<std::size_t>(lib) < out->aux_exchange.size() &&
                                                  (out->aux_exchange[static_cast<std::size_t>(lib)].before & 0x100u));
        const auto golden_of = [&](const auto& wanted) {
            std::vector<std::uint16_t> list;
            if (static_cast<std::size_t>(lib) < out->aux_sound.size())
                for (const auto c : out->aux_sound[static_cast<std::size_t>(lib)].triggers)
                    if (wanted(c)) list.push_back(c);
            if (list.empty() && to_loco)
                for (const auto c : out->sound_ext)
                    if (wanted(c)) list.push_back(c);
            return list;
        };
        const auto ours_of = [&](const auto& wanted) {
            std::vector<std::uint16_t> list;
            for (const auto c : klub_out.sounds)
                if (wanted(c)) list.push_back(c);
            return list;
        };
        if (klub.settings.setup & 1) {
            const auto golden = golden_of(saut_code), ours = ours_of(saut_code);
            if (golden != ours) {
                double g = 0, o = 0;
                for (const auto c : golden) g = g * 10000.0 + c;
                for (const auto c : ours) o = o * 10000.0 + c;
                differ("saut_sounds", g, o, 0.0);
            }
        }
        const auto golden_sounds = golden_of(klub_code);
        const auto klub_sounds = ours_of(klub_code);
        if (golden_sounds != klub_sounds) {
            const auto join = [](const std::vector<std::uint16_t>& list) {
                double v = 0;
                for (const auto c : list) v = v * 10000.0 + c;
                return v;
            };
            differ("sounds", join(golden_sounds), join(klub_sounds), 0.0);
        }
    };
    const auto* cab = consist->cab(player);
    // The golden run's file and what it last saw of each car's switches.
    std::FILE* record = nullptr;
    std::vector<std::vector<std::uint32_t>> recorded_switches;
    std::vector<std::vector<std::pair<std::uint32_t, float>>> recorded_displays;
    std::vector<std::map<std::pair<std::uint32_t, std::size_t>, std::string>> recorded_screens;
    if (!record_path.empty()) {
#ifdef _WIN32
        if (fopen_s(&record, record_path.c_str(), "w") != 0) record = nullptr;
#else
        record = std::fopen(record_path.c_str(), "w");
#endif
        if (!record) {
            std::fprintf(stderr, "cannot write %s\n", record_path.c_str());
            return 2;
        }
        want_aux_vars = true;
        std::fprintf(record, "t\tcar\tkmh\tpipe\tcyl\tind\taux\tmr\tur\te_ind\te_pipe_rate\te_aux_rate\te_mr_rate\te_ept\te_systems\te_alsn\t"
                             "e_flags\tsnd_lib\tsnd_ext\tsnd_cab\tswitches\tdisplays\taspect\tscreens\tvar\n");
    }
    int failures = 0;
    double time = 0.0;
    double next_print = 0.0;
    // cut / couple: the part cut off stands behind until coupled back.
    std::unique_ptr<simulation::Train> cut_train;
    std::vector<trainplugin::LegacyCar> cut_cars;
    // The host's trains: the player's, and the part cut off while it stands
    // apart (its DLLs go on, RTS 0x424488).
    const auto host_trains = [&] {
        std::vector<const simulation::Train*> trains{&train};
        if (cut_train) trains.push_back(cut_train.get());
        return trains;
    };
    const auto reconsisted = [&](const char* what) {
        std::vector<std::vector<trainplugin::LegacyCar>> parts{legacy};
        if (cut_train) parts.push_back(cut_cars);
        const bool ok = consist->reconsist(vfs, parts, host_trains());
        for (std::size_t i = 0; i < legacy.size(); ++i)
            if (consist->has_dll(i)) train.set_controller(i, consist->controller(i));
        for (std::size_t i = 0; cut_train && i < cut_cars.size(); ++i)
            if (consist->has_dll(1, i)) cut_train->set_controller(i, consist->controller(1, i));
        player = consist->player();
        cab = consist->cab(player);
        std::printf("%6.1f %s: %zu cars, %s, the player in car %zu\n", time, what, legacy.size(),
                    ok ? "the DLLs kept" : "FAILED", player);
        if (!ok) ++failures;
        if (const auto text = consist->take_messages(); !text.empty()) std::printf("%s", text.c_str());
    };
    std::uint64_t triggers = 0;
    std::vector<std::array<float, 9>> body_most; // --body: this second's most, by car (7, 8: the acceleration's least and most)
    std::printf("%6s %7s %9s %5s %4s %5s %3s %6s %6s %6s %6s %8s %5s\n", "t,s", "km/h", "F,kN", "notch", "rev",
                "panto", "BV", "TM", "UR", "GR", "TC", "brake,kN", "snd");
    while (time <= seconds + 1e-9) {
        for (auto& c : script) {
            if (c.done || c.time > time + 1e-9) continue;
            // when <metres> <command ...>: the command once the train's front
            // is that far along the line (from the line's time on).
            if (c.verb == "then") continue; // waits for its `when`
            if (c.verb == "when") {
                if (c.args.size() < 2 || front < std::atof(c.args[0].c_str())) continue;
                std::printf("%6.1f at %.0f m:", time, front);
                for (std::size_t k = 1; k < c.args.size(); ++k) std::printf(" %s", c.args[k].c_str());
                std::printf("\n");
                c.verb = c.args[1];
                c.args.erase(c.args.begin(), c.args.begin() + 2);
                // then <seconds> <command ...>: the lines after it, so long
                // after it fired.
                for (auto k = static_cast<std::size_t>(&c - script.data()) + 1;
                     k < script.size() && script[k].verb != "when"; ++k) {
                    auto& t = script[k];
                    if (t.verb != "then" || t.args.size() < 2) continue;
                    t.time = time + std::atof(t.args[0].c_str());
                    t.verb = t.args[1];
                    t.args.erase(t.args.begin(), t.args.begin() + 2);
                }
            }
            c.done = true;
            if (c.verb == "switch" && c.args.size() >= 2) {
                std::uint32_t id = 0;
                if (!c.args[0].empty() && std::isdigit(static_cast<unsigned char>(c.args[0][0]))) {
                    id = static_cast<std::uint32_t>(std::atoi(c.args[0].c_str()));
                } else if (const auto* s = cab ? cab->find_switch(c.args[0]) : nullptr) {
                    id = s->id;
                } else {
                    std::printf("%6.1f switch %s: no such switch\n", time, c.args[0].c_str());
                    ++failures;
                    continue;
                }
                const auto state = static_cast<std::uint32_t>(std::atoi(c.args[1].c_str()));
                const bool ok = consist->operate(player, id, state);
                if (ok) operated(id, state);
                std::printf("%6.1f switch %u -> %u: %s\n", time, id, state, ok ? "allowed" : "refused");
            } else if (c.verb == "show" && !c.args.empty()) {
                // show <switch id>: its State / SetState / frame as the host keeps them
                const auto id = static_cast<std::size_t>(std::atoi(c.args[0].c_str()));
                if (const auto* out = consist->output(player); out && id < out->switches.size()) {
                    const auto& sw = out->switches[id];
                    std::printf("%6.1f switch %zu: state %u, set %u, fixed %u, frame %.2f, flags 0x%X\n", time, id,
                                sw.state, sw.set_state, sw.fixed_state, sw.current_frame, sw.flags);
                }
            } else if (c.verb == "display" && !c.args.empty()) {
                // display <id> ...: the cab displays' Value and State the DLL set
                if (const auto* out = consist->output(player)) {
                    const auto& s = train.cars()[player];
                    std::printf("%6.1f displays (pipe %.2f cylinder %.2f independent %.2f MR %.2f UR %.2f):", time, s.pipe, s.cylinder,
                                s.independent, s.main_reservoir, out->engine.equalising);
                    for (const auto& a : c.args) {
                        const auto id = static_cast<std::size_t>(std::atoi(a.c_str()));
                        if (id < out->displays.size()) {
                            std::printf(" %zu=%.3f/%u", id, out->displays[id].value, out->displays[id].state);
                        }
                    }
                    std::printf("\n");
                }
            } else if (c.verb == "screen" && c.args.size() >= 3) {
                // screen <display> <object> <text | shown | hidden>
                const auto display = static_cast<std::uint32_t>(std::atoi(c.args[0].c_str()));
                const auto object = static_cast<std::size_t>(std::atoi(c.args[1].c_str()));
                std::string got = "(none)";
                if (const auto* out = consist->output(player)) {
                    for (const auto& screen : out->screens) {
                        if (screen.display != display || object >= screen.objects.size()) continue;
                        if (c.args[2] == "shown" || c.args[2] == "hidden") got = screen.objects[object].state >= 0 ? "shown" : "hidden";
                        else got = object < screen.texts.size() ? trainplugin::utf16_to_utf8(screen.texts[object]) : "";
                    }
                }
                std::string want = c.args[2];
                for (std::size_t k = 3; k < c.args.size(); ++k) want += " " + c.args[k];
                const bool ok = got == want;
                std::printf("%6.1f screen %u object %zu: '%s' %s\n", time, display, object, got.c_str(), ok ? "ok" : "FAILED");
                if (!ok) ++failures;
            } else if (c.verb == "door" && c.args.size() >= 3) {
                // door <car> <index> open | closed: an exterior door's State
                const auto car_index = static_cast<std::size_t>(std::atoi(c.args[0].c_str()));
                const auto door = static_cast<std::size_t>(std::atoi(c.args[1].c_str()));
                std::string got = "(none)";
                if (const auto* out = consist->output(car_index); out && door < out->doors.size())
                    got = out->doors[door].state != 0 ? "open" : "closed";
                const bool ok = got == c.args[2];
                std::printf("%6.1f door %zu/%zu: %s %s\n", time, car_index, door, got.c_str(), ok ? "ok" : "FAILED");
                if (!ok) ++failures;
            } else if (c.verb == "lamp" && c.args.size() >= 3) {
                // lamp <car> <index> on | off: a cab or salon lamp's State
                const auto car_index = static_cast<std::size_t>(std::atoi(c.args[0].c_str()));
                const auto lamp = static_cast<std::size_t>(std::atoi(c.args[1].c_str()));
                std::string got = "(none)";
                if (const auto* out = consist->output(car_index); out && lamp < out->internal_lights.size())
                    got = out->internal_lights[lamp] != 0 ? "on" : "off";
                const bool ok = got == c.args[2];
                std::printf("%6.1f lamp %zu/%zu: %s %s\n", time, car_index, lamp, got.c_str(), ok ? "ok" : "FAILED");
                if (!ok) ++failures;
            } else if (c.verb == "enter" && !c.args.empty()) {
                // enter <car>: the player walks into another engine's cab
                // (the viewer's LegacyCab::enter: Init, ChangeLoco, grouping).
                const auto car = static_cast<std::size_t>(std::atoi(c.args[0].c_str()));
                const bool ok = car < legacy.size() && consist->enter(car, train, true);
                std::printf("%6.1f enter car %zu: %s\n", time, car, ok ? "ok" : "refused");
                if (ok) player = car; // the switches are its cab's from now (one cab description for the series)
                std::printf("%s", consist->take_messages().c_str());
            } else if (c.verb == "signal" && !c.args.empty()) {
                // signal <aspect> [distance m] [flags]: one signal ahead for the
                // ALSN (its RTS aspect: 0 stop, 1 stop and proceed, 2
                // restricting, 3..5 approach, 6..7 clear; flags as ts.h
                // SignalsInfo::Flags, 1 normal by default); "signal off": none.
                world.alsn_ahead.clear();
                if (c.args[0] != "off") {
                    dllhost::SignalInput signal;
                    signal.aspect[0] = static_cast<std::uint32_t>(std::atoi(c.args[0].c_str()));
                    signal.distance = c.args.size() > 1 ? static_cast<float>(std::atof(c.args[1].c_str())) : 500.0f;
                    signal.flags = c.args.size() > 2 ? static_cast<std::uint32_t>(std::atoi(c.args[2].c_str())) : 1u; // 1 normal
                    world.alsn_ahead.push_back(signal);
                }
            } else if (c.verb == "line_signal" && c.args.size() >= 3) {
                // line_signal <name> <metres> <aspect> [limit km/h]: a signal on the line
                line = true;
                line_signals.push_back({c.args[0], std::atof(c.args[1].c_str()),
                                        static_cast<std::uint32_t>(std::atoi(c.args[2].c_str())),
                                        c.args.size() > 3 ? static_cast<float>(std::atof(c.args[3].c_str()) / 3.6) : -1.0f});
            } else if (c.verb == "line_aspect" && c.args.size() >= 2) {
                // line_aspect <name> <aspect>: the signal cleared or put back
                for (auto& s : line_signals)
                    if (s.name == c.args[0]) s.aspect = static_cast<std::uint32_t>(std::atoi(c.args[1].c_str()));
            } else if (c.verb == "milepost" && !c.args.empty()) {
                // milepost <km> [falling]: Engine::CurrentMilepost from now on
                // (the game's, by the km posts, simulation/milepost.hpp)
                world.milepost.known = true;
                world.milepost.base = static_cast<float>(std::atof(c.args[0].c_str()));
                world.milepost.decreasing = c.args.size() > 1 && c.args[1] == "falling";
            } else if (c.verb == "line_post" && c.args.size() >= 2) {
                // line_post <metres> <km/h>: a speed post on the line
                line = true;
                line_posts.push_back({std::atof(c.args[0].c_str()), static_cast<float>(std::atof(c.args[1].c_str()) / 3.6)});
            } else if (c.verb == "line_limit" && !c.args.empty()) {
                // line_limit <km/h>: the limit before the first post
                line = true;
                line_limit = static_cast<float>(std::atof(c.args[0].c_str()) / 3.6);
            } else if (c.verb == "press" && !c.args.empty()) {
                // press <switch id>...: momentary keys, one a step, each to 1
                // and back to 0 the step after (the КЛУБ's keys: Switched on 1)
                for (const auto& a : c.args) presses.push_back(static_cast<std::uint32_t>(std::atoi(a.c_str())));
            } else if (c.verb == "klub" && !c.args.empty()) {
                // klub openrts | brakes: switch the КЛУБ on the way, as the
                // Train window's setting does (needs the realistic air)
                consist->set_own_klub(c.args[0] == "openrts");
            } else if (c.verb == "handbrake" && c.args.size() >= 2) {
                // handbrake <car> <percent>: that car's handbrake (the car menu's)
                const auto car = static_cast<std::size_t>(std::atoi(c.args[0].c_str()));
                if (car < train.cars().size()) train.set_handbrake(car, static_cast<float>(std::atof(c.args[1].c_str())) / 100.0f);
            } else if (c.verb == "handbrakes") {
                // handbrakes: every car's handbrake and cylinder
                std::printf("%6.1f handbrakes:", time);
                for (std::size_t k = 0; k < train.cars().size(); ++k)
                    std::printf(" %zu=%.0f%%/TC %.2f/IND %.2f/ZR %.2f/EP %.2f", k, 100.0f * train.cars()[k].handbrake, train.cars()[k].cylinder,
                                train.cars()[k].independent, train.cars()[k].auxiliary, train.cars()[k].ep);
                if (const auto* out = consist->output(player); out && out->has_engine) std::printf(" systems %u", static_cast<unsigned>(out->engine.systems));
                std::printf("\n");
            } else if (c.verb == "voltage" && !c.args.empty()) {
                world.line_voltage = static_cast<float>(std::atof(c.args[0].c_str()));
                world.line_frequency = c.args.size() > 1 ? static_cast<float>(std::atof(c.args[1].c_str())) : 0.0f;
            } else if (c.verb == "cut" && !c.args.empty() && !cut_train) {
                // cut <index>: uncouple behind car index - 1 (the car menu's way: cocks closed)
                const auto index = static_cast<std::size_t>(std::atoi(c.args[0].c_str()));
                cut_train = train.uncouple(index, true, true);
                if (!cut_train) {
                    std::printf("%6.1f cut %zu: no such car\n", time, index);
                    ++failures;
                    continue;
                }
                cut_cars.assign(legacy.begin() + static_cast<std::ptrdiff_t>(index), legacy.end());
                legacy.resize(index);
                reconsisted("cut");
            } else if (c.verb == "couple" && cut_train) {
                // couple: the part cut off back on at the rear, its control cable connected
                using Flags = simulation::Train::CarFlags;
                const auto joint = legacy.size() - 1;
                if (!train.couple(*cut_train, false, true, 0.0)) {
                    std::printf("%6.1f couple: failed\n", time);
                    ++failures;
                    continue;
                }
                cut_train.reset();
                legacy.insert(legacy.end(), cut_cars.begin(), cut_cars.end());
                cut_cars.clear();
                // (the part's DLLs come back with it: one train again)
                train.set_car_flags(joint, Flags::cables, simulation::cable_sme_rear, true);
                train.set_car_flags(joint + 1, Flags::cables, simulation::cable_sme_front, true);
                // The hoses and cocks by hand, as the driver joins them.
                train.set_car_flags(joint, Flags::pipe_ends,
                                    simulation::pipe_hose_rear | simulation::charging_hose_rear | simulation::ep_cable_rear |
                                        simulation::pipe_cock_rear,
                                    false);
                train.set_car_flags(joint + 1, Flags::pipe_ends,
                                    simulation::pipe_hose_front | simulation::charging_hose_front | simulation::ep_cable_front |
                                        simulation::pipe_cock_front,
                                    false);
                reconsisted("coupled");
            } else if (c.verb == "expect_cut" && c.args.size() >= 4 && cut_train) {
                // expect_cut <car> <field> <op> <value>: a car of the part cut off
                const auto car_index = static_cast<std::size_t>(std::atoi(c.args[0].c_str()));
                double value = std::nan("");
                if (car_index < cut_cars.size()) {
                    const auto* out = consist->output(1, car_index);
                    const auto& s = cut_train->cars()[car_index];
                    const auto& f = c.args[1];
                    if (f == "bound") value = out && (out->loco_flags & 0x20) ? 1.0 : 0.0;
                    else if (f == "master") value = out && (out->loco_flags & 1) ? 1.0 : 0.0;
                    else if (f == "join") value = out && (out->loco_flags & 4) ? 1.0 : 0.0;
                    else if (f == "mr") value = s.main_reservoir;
                    else if (f == "pipe") value = s.pipe;
                    else if (out && f == "mr_rate") value = out->engine.main_reservoir_rate;
                    else if (out && f == "panto") value = out->engine.panto;
                    else if (out && f == "main_switch") value = out->engine.main_switch;
                    else if (out && f == "force") value = out->engine.force;
                }
                const double want = std::atof(c.args[3].c_str());
                const bool ok = compare(value, c.args[2], want);
                std::printf("%6.1f expect cut car %zu %s %s %g: %g %s\n", time, car_index, c.args[1].c_str(), c.args[2].c_str(), want, value,
                            ok ? "ok" : "FAILED");
                if (!ok) ++failures;
            } else if (c.verb == "expect_car" && c.args.size() >= 4) {
                // expect_car <car> <field> <op> <value>
                const auto car_index = static_cast<std::size_t>(std::atoi(c.args[0].c_str()));
                const double value = car_index < train.cars().size() ? field(c.args[1], *consist, car_index, train) : std::nan("");
                const double want = std::atof(c.args[3].c_str());
                const bool ok = compare(value, c.args[2], want);
                std::printf("%6.1f expect car %zu %s %s %g: %g %s\n", time, car_index, c.args[1].c_str(), c.args[2].c_str(), want, value,
                            ok ? "ok" : "FAILED");
                if (!ok) ++failures;
            } else if (c.verb == "expect" && c.args.size() >= 3) {
                const double value = field(c.args[0], *consist, player, train);
                const double want = std::atof(c.args[2].c_str());
                const bool ok = compare(value, c.args[1], want);
                std::printf("%6.1f expect %s %s %g: %g %s\n", time, c.args[0].c_str(), c.args[1].c_str(), want, value,
                            ok ? "ok" : "FAILED");
                if (!ok) ++failures;
            }
        }
        // press: the last key up, the next one down.
        if (pressed) {
            if (consist->operate(player, *pressed, 0)) operated(*pressed, 0);
            pressed.reset();
        }
        if (!presses.empty()) {
            pressed = presses.front();
            presses.pop_front();
            const bool ok = consist->operate(player, *pressed, 1);
            if (ok) operated(*pressed, 1);
            std::printf("%6.1f press %u: %s\n", time, *pressed, ok ? "ok" : "refused");
            if (!ok) ++failures;
        }
        fill_line();
        if (!consist->step(simulation::train_step, host_trains(), world)) {
            std::printf("%6.1f host failed: %s\n", time, consist->error().c_str());
            ++failures;
            break;
        }
        if (klub_shadow) shadow_step(time);
        for (std::size_t i = 0; i < legacy.size(); ++i) {
            if (const auto* out = consist->output(i)) {
                triggers += out->sound_ext.size() + out->sound_cab.size();
                for (const auto& aux : out->aux_sound) triggers += aux.triggers.size();
                if (show_triggers) {
                    auto print = [&](const std::string& to, const std::vector<std::uint16_t>& list) {
                        if (list.empty()) return;
                        std::printf("%6.1f car %zu %s:", time, i, to.c_str());
                        for (const auto t : list) std::printf(" %u", t);
                        std::printf("\n");
                    };
                    print("ext", out->sound_ext);
                    print("cab", out->sound_cab);
                    for (std::size_t k = 0; k < out->aux_sound.size(); ++k)
                        print("aux " + std::to_string(k), out->aux_sound[k].triggers);
                }
                if (show_anims) {
                    const auto print = [&](const char* where, const std::vector<dllhost::AnimState>& list, const std::size_t base) {
                        for (std::size_t k = 0; k < list.size(); ++k) {
                            const auto& a = list[k];
                            char line[160];
                            std::snprintf(line, sizeof line, "flags 0x%x to %u speed %.3f q %.3f max %.3f", a.flags, a.animate_to, a.speed, a.speed_q,
                                          a.max_speed);
                            auto& last = anims_seen[{i, base + k}];
                            if (last == line) continue;
                            last = line;
                            std::printf("%6.1f car %zu %s anim %zu: %s\n", time, i, where, k, line);
                        }
                    };
                    print("shape", out->anims, 0);
                    print("cab", out->cab_anims, 1000);
                }
                if (show_triggers) {
                    for (const auto& m : out->messages)
                        if (m.text.empty()) std::printf("%6.1f car %zu global message %u %u\n", time, i, m.action, m.id);
                }
                for (const auto& m : out->messages)
                    if (!m.text.empty())
                        std::printf("%6.1f car %zu message: %s\n", time, i, trainplugin::utf16_to_utf8(m.text).c_str());
            }
        }
        if (const auto text = consist->take_messages(); !text.empty()) std::printf("%s", text.c_str());
        train.step();
        last_front = front;
        front += train.speed() * simulation::train_step;
        if (cut_train) cut_train->step();
        if (record) {
            recorded_switches.resize(legacy.size());
            recorded_displays.resize(legacy.size());
            recorded_screens.resize(legacy.size());
            for (std::size_t i = 0; i < legacy.size(); ++i) {
                const auto* out = consist->output(i);
                const int lib = consist->aux_library(i, record_library);
                if (!out || lib < 0) continue;
                const auto& s = train.cars()[i];
                const auto& e = out->engine;
                const auto list = [](const std::vector<std::uint16_t>& codes) {
                    std::string text;
                    for (const auto c : codes) text += (text.empty() ? "" : " ") + std::to_string(c);
                    return text.empty() ? std::string("-") : text;
                };
                const auto l = static_cast<std::size_t>(lib);
                // openRTS's cab brake devices in place of Brakes.dll's: their
                // УР, independent brake, ЭПТ, gauges and lamps.
                float ur = e.equalising, independent = e.independent_brake, ep = e.ep_value;
                unsigned systems = e.systems;
                std::vector<dllhost::DisplayState> shown_displays = out->displays;
                if (const auto* asked = train.demands_of(i); asked && asked->cab_brakes && s.definition && s.definition->engine) {
                    const auto& r = *asked->cab_brakes;
                    ur = r.equalising_kept;
                    independent = r.independent_brake;
                    ep = r.ep_value;
                    systems = r.systems;
                    trainplugin::show_cab_brakes(trainplugin::brakes_parameters(*s.definition->engine),
                                                 trainplugin::working_cab(s.definition->engine->brakes_library, out->switches), r,
                                                 shown_displays);
                }
                std::string switches;
                auto& seen = recorded_switches[i];
                seen.resize(out->switches.size(), ~0u);
                for (std::size_t k = 0; k < out->switches.size(); ++k) {
                    if (out->switches[k].state == seen[k]) continue;
                    seen[k] = out->switches[k].state;
                    switches += (switches.empty() ? "" : " ") + std::to_string(k) + "=" + std::to_string(seen[k]);
                }
                // Displays: a change of state, or of value by over 0.005.
                std::string displays;
                auto& shown = recorded_displays[i];
                shown.resize(shown_displays.size(), {~0u, -1e9f});
                for (std::size_t k = 0; k < shown_displays.size(); ++k) {
                    const auto& d = shown_displays[k];
                    if (d.state == shown[k].first && std::abs(d.value - shown[k].second) <= 0.005f) continue;
                    shown[k] = {d.state, d.value};
                    char text[48];
                    std::snprintf(text, sizeof text, "%s%zu=%u/%.3g", displays.empty() ? "" : " ", k, d.state, d.value);
                    displays += text;
                }
                std::fprintf(record, "%.2f\t%zu\t%.3f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%u\t%u\t%u\t%s\t%s\t%s\t%s\t",
                             time, i, train.speed() * 3.6, s.pipe, s.cylinder, s.independent, s.auxiliary, s.main_reservoir, ur,
                             independent, e.train_pipe_rate, e.auxiliary_rate, e.main_reservoir_rate, ep, systems,
                             static_cast<unsigned>(e.alsn_on), e.engine_flags,
                             l < out->aux_sound.size() ? list(out->aux_sound[l].triggers).c_str() : "-", list(out->sound_ext).c_str(),
                             list(out->sound_cab).c_str(), switches.empty() ? "-" : switches.c_str());
                // The screens' texts that changed: display:object=text, ';'
                // between (a hidden object's text empty).
                std::string screens;
                auto& texts = recorded_screens[i];
                for (const auto& screen : out->screens) {
                    for (std::size_t k = 0; k < screen.texts.size(); ++k) {
                        std::string text = k < screen.objects.size() && screen.objects[k].state < 0
                                               ? std::string()
                                               : trainplugin::utf16_to_utf8(screen.texts[k]);
                        for (auto& ch : text)
                            if (ch == '\t' || ch == ';' || ch == '\n') ch = ' ';
                        auto& seen_text = texts[{screen.display, k}];
                        if (text == seen_text) continue;
                        seen_text = text;
                        screens += (screens.empty() ? "" : ";") + std::to_string(screen.display) + ":" + std::to_string(k) + "=" + text;
                    }
                }
                std::fprintf(record, "%s\t%u\t%s\t", displays.empty() ? "-" : displays.c_str(), out->cab_aspect,
                             screens.empty() ? "-" : screens.c_str());
                if (l < out->aux_vars.size()) {
                    for (const float v : out->aux_vars[l].var) {
                        std::uint32_t bits = 0;
                        std::memcpy(&bits, &v, sizeof bits);
                        std::fprintf(record, "%08X ", bits);
                    }
                }
                std::fprintf(record, "\n");
            }
        }
        if (show_body) {
            body_most.resize(train.cars().size());
            for (std::size_t i = 0; i < train.cars().size(); ++i) {
                const auto& cs = train.cars()[i];
                auto& m = body_most[i];
                const float now[7] = {cs.acceleration, cs.body[0], cs.body[1], cs.motion.pose_shift[2], cs.body_turn[0], cs.body_turn[1],
                                      cs.body_turn[2]};
                for (int k = 0; k < 7; ++k) m[static_cast<std::size_t>(k)] = (std::max)(m[static_cast<std::size_t>(k)], std::abs(now[k]));
                m[7] = (std::min)(m[7], cs.acceleration);
                m[8] = (std::max)(m[8], cs.acceleration);
            }
        }
        // The world clock runs with the simulation (RTS_VAR_TIME).
        world.day_time = std::fmod(world.day_time + simulation::train_step, 24.0 * 3600.0);
        if (time >= next_print - 1e-9) {
            next_print += 1.0;
            const auto* out = consist->output(player);
            const auto& s = train.cars()[player];
            if (out) {
                const auto& e = out->engine;
                double brake = 0.0;
                for (const auto& c : train.cars()) brake += c.brake_force;
                // The УР: the valve working the pipe (realistic air), else the DLL's.
                const auto* asked = train.demands_of(player);
                const float ur = asked && asked->equalising >= 0.0f ? asked->equalising : e.equalising;
                std::printf("%6.1f %7.2f %9.1f %5u %4d %5u %3u %6.2f %6.2f %6.2f %6.2f %8.1f %5llu  tail %.2f/%.2f\n", time,
                            train.speed() * 3.6, e.force / 1000.0, e.throttle, e.reverse, e.panto, e.main_switch,
                            s.pipe, ur, s.main_reservoir, (std::max)(s.cylinder, s.independent),
                            brake / 1000.0, static_cast<unsigned long long>(triggers), train.cars().back().pipe,
                            train.cars().back().cylinder);
                if (line) {
                    // On the line: where the front is, the next signal, and
                    // what BRAKES' КЛУБ makes of it (its limit, target,
                    // permitted speed, flags; docs/reverse/brakes-dll.md §11).
                    const auto v = [&](const char* name) { return field(name, *consist, player, train); };
                    std::printf("       line %.0f m, next signal %s, aspect %.0f, limit %.0f, target %.0f at %.0f, permitted %.0f, "
                                "flags 0x%08llX, timer %.2f\n",
                                front, world.alsn_ahead.empty() ? "none" : (std::to_string(static_cast<int>(world.alsn_ahead.front().distance)) + " m").c_str(),
                                v("aspect"), v("var80"), v("var116"), v("var120"), v("var140"),
                                static_cast<unsigned long long>(v("bits8")), v("var24"));
                }
                if (show_body) {
                    for (std::size_t i = 0; i < body_most.size(); ++i) {
                        const auto& m = body_most[i];
                        std::printf("         car %zu body: accel %.2f..%.2f m/s2, sway %.2f bounce %.2f surge %.2f cm, roll %.2f pitch %.2f yaw %.2f mrad\n",
                                    i, m[7], m[8], m[1] * 100.0f, m[2] * 100.0f, m[3] * 100.0f, m[4] * 1000.0f, m[5] * 1000.0f, m[6] * 1000.0f);
                    }
                    for (auto& m : body_most) m.fill(0.0f);
                }
                if (show_screens) {
                    for (const auto& screen : out->screens) {
                        std::printf("         screen %u:", screen.display);
                        for (std::size_t k = 0; k < screen.objects.size(); ++k) {
                            const auto& o = screen.objects[k];
                            if (o.state < 0) continue;
                            std::printf(" %zu", k);
                            if (o.state > 0) std::printf("=%d", o.state);
                            if (k < screen.texts.size() && !screen.texts[k].empty())
                                std::printf("'%s'", trainplugin::utf16_to_utf8(screen.texts[k]).c_str());
                            if (o.value != 0.0f) std::printf("(%.1f)", o.value);
                        }
                        std::printf("\n");
                    }
                }
                if (per_car) {
                    for (std::size_t i = 0; i < legacy.size(); ++i) {
                        const auto* o = consist->output(i);
                        const auto& cs = train.cars()[i];
                        if (!o || !o->has_engine) {
                            // A wagon: its air and the ЭПТ it is given.
                            std::printf("         car %zu: pipe %.2f, aux %.2f, cylinder %.2f, EP %.2f\n", i, cs.pipe, cs.auxiliary,
                                        cs.cylinder, cs.ep);
                            continue;
                        }
                        std::printf("         car %zu: F %.1f kN, notch %u, rev %d, panto %u, BV %u, car force %.1f kN, flags 0x%X, "
                                    "pipe rate %.3f, systems %u, pipe %.2f, aux %.2f, cylinder %.2f, independent %.2f (wants %.2f), "
                                    "EP %.2f (asks %.2f), UR %.2f, brake %.1f kN (DLL %.1f), ALSNOn %d, cab aspect %u, MR %.2f (rate %.3f), engine flags 0x%X\n",
                                    i, o->engine.force / 1000.0, o->engine.throttle, o->engine.reverse, o->engine.panto,
                                    o->engine.main_switch, cs.force / 1000.0, o->loco_flags, o->engine.train_pipe_rate,
                                    static_cast<unsigned>(o->engine.systems), cs.pipe, cs.auxiliary, cs.cylinder, cs.independent,
                                    o->engine.independent_brake, cs.ep, o->engine.ep_value, o->engine.equalising,
                                    cs.brake_force / 1000.0, o->engine.brake_force / 1000.0,
                                    static_cast<int>(static_cast<std::int16_t>(o->engine.alsn_on)), o->cab_aspect, cs.main_reservoir,
                                    o->engine.main_reservoir_rate, o->engine.engine_flags);
                        // Wheel slip (боксование): the motors' forces, the axles slipping.
                        if (o->engine.engine_force_count > 0) {
                            std::printf("           motors");
                            for (std::size_t m = 0; m < o->engine.engine_force_count; ++m) std::printf(" %.1f", o->engine.engine_forces[m] / 1000.0);
                            std::printf(" kN; wheel slip 0x%X", cs.wheelslip);
                            if (cs.wheelslip != 0) {
                                std::printf(" (");
                                for (std::size_t a = 0; a < cs.slip_ratio.size(); ++a)
                                    if (cs.wheelslip & (1u << a)) std::printf(" axle %zu x%.2f", a, cs.slip_ratio[a]);
                                std::printf(" )");
                            }
                            std::printf("\n");
                        }
                        if (!o->exhausts.empty()) {
                            std::printf("           exhausts");
                            for (const auto& x : o->exhausts) std::printf(" rate %.2f magnitude %.2f;", x.rate, x.magnitude);
                            std::printf("\n");
                        }
                        if (!o->doors.empty()) {
                            std::printf("           doors");
                            for (const auto& d : o->doors) std::printf(" %u/%u", d.state, d.set_state);
                            std::printf("\n");
                        }
                        bool lit = false;
                        for (std::size_t k = 0; k < o->light_flags.size(); ++k) {
                            if (!(o->light_flags[k] & 9u)) continue; // TurnOn or On
                            if (!lit) std::printf("           lights on:");
                            lit = true;
                            std::printf(" %zu%s", k, (o->light_flags[k] & 16u) ? "(reversed)" : "");
                        }
                        if (lit) std::printf("\n");
                        // --light-defs: what the .eng says of each lit light (the viewer draws from it).
                        if (lit && show_light_defs && i < stock.size()) {
                            const auto& defs = stock[i]->lights;
                            for (std::size_t k = 0; k < o->light_flags.size(); ++k) {
                                if (!(o->light_flags[k] & 9u)) continue;
                                if (k >= defs.size()) {
                                    std::printf("             light %zu: no definition\n", k);
                                    continue;
                                }
                                const auto& d = defs[k];
                                std::printf("             light %zu: type %u states %zu unit %u", k, d.type, d.states.size(), d.unit);
                                if (!d.states.empty()) {
                                    const auto& st = d.states.front();
                                    std::printf(" at %.3f %.3f %.3f radius %.3f colour %08X azimuth %.1f", st.position[0], st.position[1],
                                                st.position[2], st.radius, st.colour, st.azimuth[1]);
                                }
                                std::printf(" texture '%s'\n", d.texture.c_str());
                            }
                        }
                        bool lamps = false;
                        for (std::size_t k = 0; k < o->internal_lights.size(); ++k) {
                            if (!o->internal_lights[k]) continue;
                            if (!lamps) std::printf("           lamps on:");
                            lamps = true;
                            std::printf(" %zu", k);
                        }
                        if (lamps) std::printf("\n");
                        bool changed = false;
                        for (std::size_t k = 0; k < o->tex_changes.size(); ++k) {
                            if (!o->tex_changes[k]) continue;
                            if (!changed) std::printf("           texture changes on:");
                            changed = true;
                            std::printf(" %zu", k);
                        }
                        if (changed) std::printf("\n");
                    }
                }
            }
        }
        time += simulation::train_step;
    }
    if (!consist->alive()) {
        std::printf("host ended: %s\n", consist->error().c_str());
        ++failures;
    }
    if (!screens_image.empty()) write_screen_images(vfs, legacy[player], consist->cab(player), consist->output(player), screens_image);
    if (record) std::fclose(record);
    if (klub_shadow) {
        std::printf("КЛУБ shadow: %zu steps\n", shadow_steps);
        for (const auto& [name, d] : shadow_diffs)
            std::printf("  %-10s %6zu steps differ, first at %.2f s: Brakes.dll %g, openRTS %g\n", name.c_str(), d.steps, d.first,
                        d.golden, d.ours);
        if (shadow_diffs.empty()) std::printf("  all the same\n");
        else ++failures;
    }
    std::printf("%s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}

} // namespace openrts
