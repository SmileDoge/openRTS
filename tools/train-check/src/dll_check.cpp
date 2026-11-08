// openrts-train-check <root> --dll <TRAINSET folder> <eng|wag>[!] ... [--script <file>] [--seconds <s>]
//
// Runs legacy RTS locomotive DLLs through openrts-dllhost32 on a straight
// level test track: the consist is the named .eng / .wag files of one
// TRAINSET folder in order ("!" after a name turns that car), the first
// engine with a DLL is the one the player takes. Every second it prints
// what the DLLs report. A script drives the cab and checks the outcome:
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
//
// Exit code 0: every expectation held; 1: one failed; 2: could not run.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <map>

#include <glm/geometric.hpp>

#include <openrts/core/vfs.hpp>
#include <openrts/formats/ace_file.hpp>
#include <openrts/formats/rolling_stock.hpp>

#include "cab_screens.hpp"
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
    return std::nan("");
}

bool compare(double a, const std::string& op, double b) {
    if (op == ">") return a > b;
    if (op == "<") return a < b;
    if (op == ">=") return a >= b;
    if (op == "<=") return a <= b;
    if (op == "==") return std::abs(a - b) < 1e-6;
    if (op == "!=") return std::abs(a - b) >= 1e-6;
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

int dll_check(const core::Vfs& vfs, const std::filesystem::path& root, const std::vector<std::string>& args) {
    if (args.empty()) {
        std::fprintf(stderr, "--dll <TRAINSET folder> <eng|wag>[!] ... [--script <file>] [--seconds <s>] [--start cold|on|running] [--cars [--light-defs]] [--triggers]\n");
        return 2;
    }
    const std::string folder = args[0];
    std::vector<std::string> names;
    std::string script_path;
    double seconds = 30.0;
    bool per_car = false;
    bool show_triggers = false; // --triggers: every SMS trigger a DLL posts
    bool show_screens = false;  // --screens: the cab screens' shown objects and texts, each second
    bool show_light_defs = false; // --light-defs: with --cars, each lit exterior light's .eng definition
    std::string screens_image;  // --screens-image <dir>: the cab screens as RTS lays them out, at the end (PPM)
    int start_state = -1; // --start cold|on|running
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--script" && i + 1 < args.size()) {
            script_path = args[++i];
        } else if (args[i] == "--start" && i + 1 < args.size()) {
            const auto& value = args[++i];
            start_state = value == "cold" ? 0 : value == "on" ? 1 : -1;
        } else if (args[i] == "--cars") {
            per_car = true;
        } else if (args[i] == "--triggers") {
            show_triggers = true;
        } else if (args[i] == "--screens") {
            show_screens = true;
        } else if (args[i] == "--light-defs") {
            show_light_defs = true;
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
    if (!script_path.empty()) {
        script = read_script(script_path, error);
        if (!error.empty()) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 2;
        }
        for (const auto& c : script) seconds = (std::max)(seconds, c.time + 1.0);
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
        for (auto name : names) {
            const bool flipped = !name.empty() && name.back() == '!';
            if (flipped) name.pop_back();
            entries.push_back({folder, name, flipped});
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
        auto def = std::make_unique<formats::WagonDefinition>(formats::load_wagon(vfs, entry.folder, name, engine));
        if (!def->ok()) {
            std::fprintf(stderr, "%s/%s: %s\n", folder.c_str(), name.c_str(), def->error.c_str());
            return 2;
        }
        setups.push_back({def.get(), flipped, 0.0f});
        const auto n = static_cast<std::uint32_t>(legacy.size() + 1);
        legacy.push_back({def.get(), path, n, n}); // uid, key
        stock.push_back(std::move(def));
    }
    const auto cache = std::filesystem::temp_directory_path() / "openrts-cache";
    auto consist = trainplugin::LegacyConsist::create(vfs, cache, legacy, root / "RTS", &error);
    if (!consist) {
        std::fprintf(stderr, "legacy DLLs: %s\n", error.c_str());
        return 2;
    }
    for (const auto& m : consist->modules())
        std::printf("module %s: %s, TS_VERSION %u\n", trainplugin::utf16_to_utf8(m.path).c_str(),
                    m.status == 0 ? "loaded" : "REFUSED", m.version[1]);

    simulation::Train train(setups);
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
    // Its air from Init, as the game's first entering (RTS's).
    if (!trainplugin::enter_new_train(*consist, player, train, true, start_state)) {
        std::fprintf(stderr, "enter failed: %s\n%s", consist->error().c_str(), consist->take_messages().c_str());
        return 2;
    }
    std::printf("player in car %zu (%s)\n", player, names[player].c_str());

    trainplugin::LegacyWorld world;
    const auto* cab = consist->cab(player);
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
    std::printf("%6s %7s %9s %5s %4s %5s %3s %6s %6s %6s %6s %8s %5s\n", "t,s", "km/h", "F,kN", "notch", "rev",
                "panto", "BV", "TM", "UR", "GR", "TC", "brake,kN", "snd");
    while (time <= seconds + 1e-9) {
        for (auto& c : script) {
            if (c.done || c.time > time + 1e-9) continue;
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
                std::printf("%6.1f switch %u -> %u: %s\n", time, id, state, ok ? "allowed" : "refused");
            } else if (c.verb == "show" && !c.args.empty()) {
                // show <switch id>: its State / SetState / frame as the host keeps them
                const auto id = static_cast<std::size_t>(std::atoi(c.args[0].c_str()));
                if (const auto* out = consist->output(player); out && id < out->switches.size()) {
                    const auto& sw = out->switches[id];
                    std::printf("%6.1f switch %zu: state %u, set %u, fixed %u, frame %.2f, flags 0x%X\n", time, id,
                                sw.state, sw.set_state, sw.fixed_state, sw.current_frame, sw.flags);
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
        if (!consist->step(simulation::train_step, host_trains(), world)) {
            std::printf("%6.1f host failed: %s\n", time, consist->error().c_str());
            ++failures;
            break;
        }
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
        if (cut_train) cut_train->step();
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
                std::printf("%6.1f %7.2f %9.1f %5u %4d %5u %3u %6.2f %6.2f %6.2f %6.2f %8.1f %5llu\n", time,
                            train.speed() * 3.6, e.force / 1000.0, e.throttle, e.reverse, e.panto, e.main_switch,
                            s.pipe, e.equalising, s.main_reservoir, (std::max)(s.cylinder, s.independent),
                            brake / 1000.0, static_cast<unsigned long long>(triggers));
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
                        if (!o || !o->has_engine) continue;
                        std::printf("         car %zu: F %.1f kN, notch %u, rev %d, panto %u, BV %u, car force %.1f kN, flags 0x%X, "
                                    "pipe rate %.3f, systems %u, pipe %.2f\n",
                                    i, o->engine.force / 1000.0, o->engine.throttle, o->engine.reverse, o->engine.panto,
                                    o->engine.main_switch, cs.force / 1000.0, o->loco_flags, o->engine.train_pipe_rate,
                                    static_cast<unsigned>(o->engine.systems), cs.pipe);
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
    std::printf("%s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}

} // namespace openrts
