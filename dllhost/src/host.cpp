#include "host.hpp"

#include <float.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace openrts::dllhost {

namespace {

Host* g_host = nullptr;

// Runs `f`, catching any structured exception (access violation, FP trap,
// stack overflow) a DLL raises. Returns the exception code, 0 when fine.
// Where the last caught exception happened (for the crash report).
struct CrashInfo {
    void* address = nullptr;
    ULONG_PTR access[2]{}; // access violations: 0 read / 1 write / 8 execute, and the address
    std::uint32_t params = 0;
};
thread_local CrashInfo g_crash;

int record_crash(EXCEPTION_POINTERS* info) {
    g_crash = {};
    if (info && info->ExceptionRecord) {
        g_crash.address = info->ExceptionRecord->ExceptionAddress;
        g_crash.params = info->ExceptionRecord->NumberParameters;
        for (DWORD i = 0; i < 2 && i < info->ExceptionRecord->NumberParameters; ++i)
            g_crash.access[i] = info->ExceptionRecord->ExceptionInformation[i];
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

template <class F>
DWORD guarded(F&& f) {
    __try {
        f();
        return 0;
    } __except (record_crash(GetExceptionInformation())) {
        return GetExceptionCode();
    }
}

// "module+0xRVA" for a code address.
std::wstring describe_address(const void* address) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(address), &module) || !module) {
        wchar_t b[24];
        swprintf_s(b, L"0x%p", address);
        return b;
    }
    wchar_t name[MAX_PATH];
    const DWORD n = GetModuleFileNameW(module, name, MAX_PATH);
    std::wstring file(name, n);
    if (const auto slash = file.find_last_of(L"\\/"); slash != std::wstring::npos) file.erase(0, slash + 1);
    wchar_t b[48];
    swprintf_s(b, L"+0x%X", static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(address) - reinterpret_cast<std::uintptr_t>(module)));
    return file + b;
}

std::wstring from_u16(std::u16string_view s) { return {reinterpret_cast<const wchar_t*>(s.data()), s.size()}; }
std::u16string to_u16(const wchar_t* s, std::size_t n) { return {reinterpret_cast<const char16_t*>(s), n}; }

std::wstring hex(std::uint32_t v) {
    wchar_t b[16];
    swprintf_s(b, L"0x%08X", v);
    return b;
}

std::wstring last_error_text() {
    const DWORD e = GetLastError();
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e,
                   0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::wstring r = L"error " + std::to_wstring(e);
    if (text) {
        r += L": ";
        r += text;
        LocalFree(text);
        while (!r.empty() && (r.back() == L'\n' || r.back() == L'\r' || r.back() == L' ')) r.pop_back();
    }
    return r;
}

// The codes DLLVersion answers with a struct size, compared on load.
constexpr std::uint32_t size_codes[] = {
    abi::v_switch, abi::v_display, abi::v_screen, abi::v_loco, abi::v_loco_e, abi::v_loco_d, abi::v_engine,
    abi::v_engine_e, abi::v_engine_d, abi::v_light, abi::v_cabin, abi::v_int_light, abi::v_cls_sub, abi::v_object,
    abi::v_wagon, abi::v_activator, abi::v_track_item, abi::v_track_items_item,
};

const wchar_t* version_name(std::uint32_t code) {
    static const wchar_t* names[] = {L"", L"VERSION", L"SWITCH", L"DISPLAY", L"SCREEN", L"LOCO", L"LOCOE", L"LOCOD",
                                     L"ENGINE", L"ENGINEE", L"ENGINED", L"LIGHT", L"CABIN", L"INTLIGHT", L"CLSSUB",
                                     L"OBJECT", L"WAGON", L"ACTIVATOR", L"ADAPTER_COMPLIANCE", L"STACKSIZE",
                                     L"TRACKITEM", L"TRACKITEMITEM"};
    return code < 22 ? names[code] : L"?";
}

bool same_name(const wchar_t* a, const wchar_t* b) { return a && b && lstrcmpiW(a, b) == 0; }


// A switch's node moving `distance` positions from `from` towards `target`
// (RTS 0x4B30DC). Through a point of one of its Shortcut sets it jumps to
// the set's farthest point that is not past the target, at no cost:
// upwards the first point in (at, next] (0x4B3778), downwards the last in
// (next, at] (0x4B3884), which counts the point it stands on.
float advance_switch(const float from, const float target, float distance, const std::vector<std::vector<float>>* shortcuts) {
    float at = from;
    for (int guard = 0; guard < 16 && distance > 0.0f && at != target; ++guard) {
        const bool up = target > at;
        const float next = up ? (std::min)(at + distance, target) : (std::max)(at - distance, target);
        std::optional<std::pair<float, float>> jump; // the point passed, where it lands
        if (shortcuts) {
            for (const auto& set : *shortcuts) {
                std::optional<float> hit;
                std::optional<float> land;
                if (up) {
                    for (const float v : set) {
                        if (v > at && v <= next) {
                            hit = v;
                            break;
                        }
                    }
                    if (!hit) continue;
                    for (auto v = set.rbegin(); v != set.rend() && *v > *hit; ++v) {
                        if (*v <= target) {
                            land = *v;
                            break;
                        }
                    }
                } else {
                    for (auto v = set.rbegin(); v != set.rend(); ++v) {
                        if (*v <= at && *v > next) {
                            hit = *v;
                            break;
                        }
                    }
                    if (!hit) continue;
                    for (const float v : set) {
                        if (v >= *hit) break;
                        if (v >= target) {
                            land = v;
                            break;
                        }
                    }
                }
                if (land) {
                    jump = std::pair{*hit, *land};
                    break;
                }
            }
        }
        if (!jump) return next;
        distance -= std::abs(jump->first - at);
        at = jump->second;
    }
    return at;
}

} // namespace

const wchar_t* keep(std::deque<std::wstring>& strings, std::u16string_view text) {
    strings.push_back(from_u16(text));
    return strings.back().c_str();
}

bool load_module(Module& module, const std::wstring& path, std::wstring* exports) {
    module.path = path;
    // LOAD_WITH_ALTERED_SEARCH_PATH needs an absolute path: the DLL's own
    // folder is then searched for its dependencies.
    wchar_t full[MAX_PATH * 2];
    const DWORD n = GetFullPathNameW(path.c_str(), static_cast<DWORD>(std::size(full)), full, nullptr);
    const std::wstring absolute = n && n < std::size(full) ? std::wstring(full, n) : path;
    module.handle = LoadLibraryExW(absolute.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module.handle) {
        module.failed = true;
        module.error = L"LoadLibrary: " + last_error_text();
        return false;
    }
    auto proc = [&](const char* name) -> void* {
        void* p = reinterpret_cast<void*>(GetProcAddress(module.handle, name));
        if (p && exports) {
            if (!exports->empty()) *exports += L' ';
            for (const char* c = name + 1; *c; ++c) *exports += static_cast<wchar_t>(*c);
        }
        return p;
    };
    module.init = proc("_Init");
    module.run = proc("_Run");
    module.change_loco = proc("_ChangeLoco");
    module.lost_master = proc("_LostMaster");
    module.can_work_with = proc("_CanWorkWith");
    module.can_switch = proc("_CanSwitch");
    module.switched = proc("_Switched");
    module.alsn = proc("_ALSN");
    module.speed_limit = proc("_SpeedLimit");
    auto* version = reinterpret_cast<abi::DLLVersionFn>(GetProcAddress(module.handle, "_DLLVersion"));
    if (!GetProcAddress(module.handle, "_InitRTSLibrary")) {
        module.failed = true;
        module.error = L"no _InitRTSLibrary export: not an RTS DLL";
        return false;
    }
    if (!version) {
        module.failed = true;
        module.error = L"no _DLLVersion export";
        return false;
    }
    for (std::uint32_t code = 1; code < 22; ++code) {
        std::uint32_t value = 0;
        const DWORD crash = guarded([&] { value = version(code); });
        if (crash) {
            module.failed = true;
            module.error = L"DLLVersion(" + std::to_wstring(code) + L") crashed: " + hex(crash);
            return false;
        }
        module.version[code] = value;
    }
    return true;
}

std::wstring layout_mismatch(const Module& module) {
    std::wstring r;
    if (module.version[abi::v_version] != abi::ts_version)
        r += L"TS_VERSION " + std::to_wstring(module.version[abi::v_version]) + L" (host 104); ";
    for (const auto code : size_codes) {
        if (module.version[code] != abi::expected_size(code))
            r += std::wstring(version_name(code)) + L" " + std::to_wstring(module.version[code]) + L" (host " +
                 std::to_wstring(abi::expected_size(code)) + L"); ";
    }
    return r;
}

std::wstring choose_adapters(Module& module) {
    const std::uint32_t version = module.version[abi::v_version];
    module.loco_variant = module.cab_variant = module.signals_variant = 0;
    if (version < 101) return L"TS_VERSION " + std::to_wstring(version) + L" is older than RTS accepts (101)";
    if (module.version[abi::v_adapter_compliance] < abi::adapter_compliance) {
        // Without adapter compliance RTS wants the current sizes exactly.
        const auto mismatch = layout_mismatch(module);
        return mismatch.empty() ? L"" : L"layout differs and the DLL is not adapter-compliant: " + mismatch;
    }
    switch (version) {
    case 104: return {};
    case 101: module.loco_variant = 1; module.cab_variant = 1; module.signals_variant = 1; return {};
    case 102: module.loco_variant = 2; module.cab_variant = 1; module.signals_variant = 1; return {};
    case 103: module.cab_variant = 1; module.signals_variant = 1; return {};
    default: return L"TS_VERSION " + std::to_wstring(version) + L" has no adapters";
    }
}

Host::Host() { g_host = this; }

Host::~Host() {
    if (g_host == this) g_host = nullptr;
    // Modules are left loaded: unloading Borland DLLs at exit is where they
    // tend to crash, and the process ends anyway.
}

Host* Host::current() { return g_host; }

void Host::log(const std::wstring& line) {
    if (log_.size() > 16000) return;
    log_ += line;
    log_ += L'\n';
}

std::wstring Host::take_log() { return std::exchange(log_, {}); }

void Host::prepare_fpu() {
    // RTS (C++ Builder) runs with 64-bit precision. Exceptions stay masked
    // here so a DLL's division by zero gives inf instead of killing the host
    // (an openRTS choice, docs/reverse/train-dll.md section 9).
    unsigned int cw = 0;
    _controlfp_s(&cw, _PC_64 | _MCW_EM, _MCW_PC | _MCW_EM);
}

void Host::fail(Module& module, const wchar_t* call, std::uint32_t code) {
    module.failed = true;
    std::wstring where = L" at " + describe_address(g_crash.address);
    if (code == EXCEPTION_ACCESS_VIOLATION && g_crash.params >= 2) {
        wchar_t b[64];
        swprintf_s(b, L", %ls 0x%08X", g_crash.access[0] == 1 ? L"writing" : g_crash.access[0] == 8 ? L"executing" : L"reading",
                   static_cast<unsigned>(g_crash.access[1]));
        where += b;
    }
    log(L"DLL " + module.path + L": " + call + L" crashed (" + hex(code) + where + L"); the DLL is switched off");
}

Module* Host::module_for(const std::wstring& path) {
    for (auto& m : modules_) {
        if (lstrcmpiW(m->path.c_str(), path.c_str()) == 0) return m->failed ? nullptr : m.get();
    }
    auto module = std::make_unique<Module>();
    Module* m = module.get();
    modules_.push_back(std::move(module));
    if (!load_module(*m, path)) {
        log(L"DLL " + path + L": " + m->error);
        return nullptr;
    }
    if (const auto refusal = choose_adapters(*m); !refusal.empty()) {
        m->failed = true;
        m->error = refusal;
        log(L"DLL " + path + L": " + m->error);
        return nullptr;
    }
    if (!init_rts_library(*m)) return nullptr;
    return m;
}

bool Host::init_rts_library(Module& module) {
    auto* fn = reinterpret_cast<abi::InitRTSLibraryFn>(GetProcAddress(module.handle, "_InitRTSLibrary"));
    bool ok = false;
    prepare_fpu();
    if (const DWORD crash = guarded([&] { ok = fn(GetModuleHandleW(nullptr), &module); })) {
        fail(module, L"InitRTSLibrary", crash);
        return false;
    }
    module.rts_library = ok;
    if (!ok) {
        module.failed = true;
        module.error = L"InitRTSLibrary returned false (host functions missing?)";
        log(L"DLL " + module.path + L": " + module.error);
    }
    return ok;
}

// --- setup -------------------------------------------------------------------

bool Host::setup(const SetupRequest& request, std::vector<ModuleReport>& modules) {
    cars_.clear();
    parked_.clear();
    cables_.clear();
    by_object_.clear();
    by_cabin_.clear();
    by_aux_.clear();
    strings_.clear();
    player_ = UINT32_MAX;
    // One train: the player's.
    if (trains_.empty()) trains_.push_back(std::make_unique<TrainMirror>());
    train_count_ = 1;
    trains_[0]->object.fill(0);
    trains_[0]->type = request.train_type;
    trains_[0]->object[0] = request.train_type;
    trains_[0]->first = 0;
    trains_[0]->count = request.cars.size();
    rts_root_ = from_u16(request.rts_root);
    if (!rts_root_.empty()) SetDllDirectoryW(rts_root_.c_str()); // bass.dll, CC3260MT.DLL
    items_.clear();
    units_.clear();
    window_.clear();
    sound_tables_.clear();
    add_sound_tables(request.sound_tables);

    for (std::uint32_t i = 0; i < request.cars.size(); ++i) {
        auto car = std::make_unique<CarMirror>();
        car->spec = request.cars[i];
        car->index = i;
        build_car(*car);
        cars_.push_back(std::move(car));
    }
    link_cars();

    // Load (but do not bind) every module now, so the game learns early which
    // DLLs refuse to run.
    for (const auto& car : cars_) {
        auto report_module = [&](const std::u16string& path) {
            if (path.empty()) return;
            for (const auto& r : modules)
                if (r.path == path) return;
            ModuleReport report;
            report.path = path;
            const Module* m = module_for(from_u16(path));
            report.status = m ? 0 : -1;
            for (const auto& mod : modules_)
                if (lstrcmpiW(mod->path.c_str(), from_u16(path).c_str()) == 0)
                    std::memcpy(report.version, mod->version, sizeof(report.version));
            modules.push_back(std::move(report));
        };
        report_module(car->spec.module);
        for (const auto& aux : car->spec.aux) report_module(aux.module);
    }
    return true;
}

void Host::add_sound_tables(const std::vector<SoundTableSpec>& tables) {
    for (const auto& spec : tables) {
        auto table = std::make_unique<SoundTable>();
        for (const auto& name : spec.names) table->names.push_back(from_u16(name));
        table->codes = spec.codes;
        table->pair_on = spec.pair_on;
        sound_tables_.push_back(std::move(table));
    }
}

// --- coupling and cutting (0x469ED8, 0x469858) ------------------------------------

bool Host::reconsist(const ReconsistRequest& request, StepReply& reply) {
    add_sound_tables(request.sound_tables);
    const CarMirror* player = player_ < cars_.size() ? cars_[player_].get() : nullptr;
    auto old = std::move(cars_);
    cars_.clear();
    while (trains_.size() < request.trains.size()) trains_.push_back(std::make_unique<TrainMirror>());
    train_count_ = request.trains.size();
    for (std::size_t t = 0; t < request.trains.size(); ++t) {
        auto& train = *trains_[t];
        train.type = request.trains[t].type;
        train.object[0] = train.type;
        train.first = cars_.size();
        train.count = request.trains[t].cars.size();
        for (const auto& entry : request.trains[t].cars) {
            std::unique_ptr<CarMirror> car;
            if (entry.from >= 0 && static_cast<std::size_t>(entry.from) < old.size()) {
                car = std::move(old[static_cast<std::size_t>(entry.from)]);
            } else if (entry.from == reconsist_parked) {
                if (const auto it = parked_.find(entry.spec.key); it != parked_.end() && entry.spec.key) {
                    car = std::move(it->second);
                    parked_.erase(it);
                }
            }
            if (!car) {
                // A car not seen before (or named twice): a new mirror.
                car = std::make_unique<CarMirror>();
                car->spec = entry.spec;
                build_car(*car);
            }
            car->index = static_cast<std::uint32_t>(cars_.size());
            car->train = static_cast<std::uint32_t>(t);
            cars_.push_back(std::move(car));
        }
    }
    // The cars that left keep their mirrors (and stay known to the host
    // functions: a lost slave's LostMaster names them).
    for (auto& car : old) {
        if (!car) continue;
        car->wagon()->flags &= ~4u;
        if (!car->spec.key) continue; // nothing to find it by: dropped
        const auto key = car->spec.key;
        parked_[key] = std::move(car);
    }
    player_ = UINT32_MAX;
    for (std::size_t i = 0; i < cars_.size(); ++i)
        if (cars_[i].get() == player) player_ = static_cast<std::uint32_t>(i);
    link_cars();
    write_inputs(request.inputs);
    // RTS binds the new slaves with the train's state at the time (0x5026FC:
    // bit 1 when it moves); an activity's TrainState still replaces it.
    start_running_ = request.running;
    cables_.assign(cars_.size(), 0);
    for (std::size_t i = 0; i < cars_.size(); ++i) cables_[i] = cars_[i]->wagon()->flags & 0x30000u;
    regroup();
    collect(reply);
    return true;
}

void Host::build_car(CarMirror& car) {
    const auto& spec = car.spec;
    abi::Wagon* w = car.wagon();
    w->is_locomotive = spec.kind;
    w->uid = spec.uid;
    w->durability = 1.0f;
    w->mass = spec.mass;
    w->ept_value = -1.0f;
    w->obj = &car.shape_object;
    w->wag = car.wag_type.data();

    car.lights.resize(spec.lights);
    for (std::size_t k = 0; k < car.lights.size(); ++k) {
        auto& light = car.lights[k];
        light.parent = w;
        if (k >= spec.light_specs.size()) continue;
        const auto& l = spec.light_specs[k];
        light.type = l.type;
        light.colour = l.colour;
        light.radius = l.radius;
        light.fade_in = l.fade_in;
        light.fade_out = l.fade_out;
        light.spot_exp = l.spot_exponent;
        // RTS's model axes negate x.
        light.position[0] = -l.position[0];
        light.position[1] = l.position[1];
        light.position[2] = l.position[2];
    }
    w->lights = car.lights.empty() ? nullptr : car.lights.data();
    car.doors.resize(spec.doors);
    car.door_clock.assign(spec.doors, 0.0f);
    for (std::size_t k = 0; k < car.doors.size(); ++k) {
        auto& door = car.doors[k];
        door.parent = w;
        door.anim_time = k < spec.door_times.size() ? spec.door_times[k] : 1.0f;
        door.global_id = k < spec.door_groups.size() ? spec.door_groups[k] : 0u;
    }
    w->num_doors = static_cast<abi::u16>(car.doors.size());
    w->doors = car.doors.empty() ? nullptr : car.doors.data();
    car.anims.resize(spec.animations.size());
    for (std::size_t i = 0; i < car.anims.size(); ++i)
        car.anims[i].name = const_cast<wchar_t*>(keep(strings_, spec.animations[i]));
    w->num_anim = static_cast<abi::u16>(car.anims.size());
    w->anims = car.anims.empty() ? nullptr : car.anims.data();
    car.sound_ext.flags = 1 | 2; // discrete triggers, on
    car.sound_ext.sms = sound_table(spec.sound_ext);
    w->sound = &car.sound_ext;

    if (!car.is_loco()) return;
    abi::Locomotive* l = car.loco();
    l->num_engines = spec.num_engines;
    l->num_axles = spec.num_axles;
    l->lib_param = spec.lib_param;
    l->stack_size = spec.stack_size;
    if (spec.kind == car_diesel) {
        car.exhaust.resize(4);
        car.diesel()->fuel_amount = 5000.0f;
    }

    // Cabin
    if (spec.cab.present) {
        const auto& cab = spec.cab;
        abi::Cabin& c = car.cabin;
        c.parent = w;
        // As many as the .sd declares (RTS 0x51CD88), and every id it uses:
        // a DLL asking for an undeclared id below the count gets a switch at
        // state 0, not "none" (the MaDaDev brakes read "none" as -1, set).
        std::uint32_t n_switches = (std::min)(cab.switch_count, 4096u);
        for (const auto& s : cab.switches) n_switches = (std::max)(n_switches, s.id + 1);
        car.switches.resize(n_switches);
        car.held_switches.assign(n_switches, 0);
        car.switch_shortcuts.assign(n_switches, {});
        for (auto& s : car.switches) s.parent = w;
        for (const auto& s : cab.switches) {
            abi::ActSwitch& sw = car.switches[s.id];
            sw.name = const_cast<wchar_t*>(keep(strings_, s.name));
            // As RTS's loader (0x51B98C): State and SetState start at the
            // States' second value; a KEY switch gets a "key out" state 0
            // below its positions (NumStates + 1) and starts with the key
            // out; FixedState only from its own keyword.
            const bool key = (s.flags & 2) != 0;
            sw.num_states = s.num_states + (key ? 1u : 0u);
            sw.initial_state = s.initial_state;
            sw.state = sw.set_state = key ? 0u : s.initial_state;
            sw.fixed_state = s.fixed_state;
            sw.flags = s.flags;
            std::memcpy(sw.key, s.keys, 4);
            sw.anim_time = s.anim_time;
            sw.current_frame = static_cast<float>(sw.state);
            sw.prec_dx = s.precision[0];
            sw.prec_dy = s.precision[1];
            car.switch_shortcuts[s.id] = s.shortcuts;
        }
        std::uint32_t n_displays = (std::min)(cab.display_count, 4096u);
        for (const auto& d : cab.displays) n_displays = (std::max)(n_displays, d.id + 1);
        car.displays.resize(n_displays);
        for (auto& d : car.displays) d.parent = w;
        for (const auto& d : cab.displays) {
            abi::ActDisplay& dp = car.displays[d.id];
            dp.name = const_cast<wchar_t*>(keep(strings_, d.name));
            std::memcpy(dp.values_range, d.range, sizeof(d.range));
            dp.granularity = d.granularity;
            dp.flags = d.flags;
            dp.num_hints = d.num_hints;
            dp.show_hint = -1;
            dp.state = d.initial_state; // RTS 0x51CD88: InitialState goes to State
        }
        build_screens(car, cab);
        car.internal_lights.resize(cab.internal_lights);
        for (std::size_t k = 0; k < car.internal_lights.size() && k < cab.internal_light_groups.size(); ++k)
            car.internal_lights[k].global_id = cab.internal_light_groups[k];
        car.tex_changes.resize(cab.tex_changes);
        for (std::size_t k = 0; k < car.tex_changes.size(); ++k) {
            if (k < cab.tex_change_images.size()) car.tex_changes[k].tex_id = cab.tex_change_images[k];
            if (k < cab.tex_change_groups.size()) car.tex_changes[k].global_id = cab.tex_change_groups[k];
        }
        car.cab_doors.resize(cab.doors);
        for (std::size_t k = 0; k < car.cab_doors.size(); ++k) {
            auto& d = car.cab_doors[k];
            d.parent = w;
            if (k < cab.door_names.size()) d.name = const_cast<wchar_t*>(keep(strings_, cab.door_names[k]));
            if (k < cab.door_groups.size()) d.global_id = cab.door_groups[k];
        }
        car.cab_anims.resize(cab.animations.size());
        for (std::size_t i = 0; i < car.cab_anims.size(); ++i)
            car.cab_anims[i].name = const_cast<wchar_t*>(keep(strings_, cab.animations[i]));
        c.num_switches = static_cast<abi::u32>(car.switches.size());
        c.switches = car.switches.empty() ? nullptr : car.switches.data();
        c.num_displays = static_cast<abi::u32>(car.displays.size());
        c.displays = car.displays.empty() ? nullptr : car.displays.data();
        c.num_lights = static_cast<abi::u32>(car.internal_lights.size());
        c.light = car.internal_lights.empty() ? nullptr : car.internal_lights.data();
        c.num_tex_changes = static_cast<abi::u32>(car.tex_changes.size());
        c.tex = car.tex_changes.empty() ? nullptr : car.tex_changes.data();
        c.num_doors = static_cast<abi::u32>(car.cab_doors.size());
        c.doors = car.cab_doors.empty() ? nullptr : car.cab_doors.data();
        c.num_anim = static_cast<abi::u32>(car.cab_anims.size());
        c.anims = car.cab_anims.empty() ? nullptr : car.cab_anims.data();
        c.signal_num_lights = cab.signal_lights;
        c.signal_num_aspects = cab.signal_aspects;
        c.signal_scripted = cab.signal_scripted ? 1u : 0u;
        c.signal.distance = -1.0f;
        c.signal.speed_limit = -1.0f;
        l->cab = &c;
    }
    car.sound_cab.flags = 1 | 2;
    car.sound_cab.sms = sound_table(spec.sound_cab);
}

void Host::link_cars() {
    for (std::size_t t = 0; t < train_count_; ++t) trains_[t]->wagons.clear();
    for (auto& car : cars_) {
        auto& train = train_of(*car);
        train.wagons.push_back(car->wagon());
        car->wagon()->train_in = train.object.data();
        by_object_[car->object.data()] = car.get();
        by_object_[car->engine.data()] = car.get();
        if (car->spec.cab.present) by_cabin_[&car->cabin] = car.get();
        for (auto& shadow : car->shadows) {
            by_object_[shadow.loco.data()] = car.get();
            by_cabin_[shadow.cab.data()] = car.get();
        }
    }
    for (std::size_t i = 0; i < cars_.size(); ++i) {
        if (!cars_[i]->is_loco()) continue;
        auto& train = train_of(*cars_[i]);
        abi::Locomotive* l = cars_[i]->loco();
        l->first_in_group = &train.wagons[i - train.first];
        l->last_in_group = &train.wagons[i - train.first];
    }
}

// --- binding and Init (0x5026FC) ----------------------------------------------

bool Host::bind(CarMirror& car) {
    if (car.has_engine) return car.module && !car.module->failed;
    if (car.spec.module.empty()) return false;
    Module* m = module_for(from_u16(car.spec.module));
    if (!m) return false;
    car.module = m;
    ++m->refs;
    abi::Locomotive* l = car.loco();
    // StackSize: VariableStack, raised to what the DLL asks for.
    std::uint32_t stack = car.spec.stack_size;
    stack = (std::max)(stack, m->version[abi::v_stack_size]);
    l->stack_size = stack;
    car.var.assign((std::max)(stack, 1u), 0.0f);
    car.params.resize(car.spec.parameters.size());
    for (std::size_t i = 0; i < car.params.size(); ++i) {
        const auto& p = car.spec.parameters[i];
        car.params[i].name = const_cast<wchar_t*>(keep(strings_, p.name));
        car.params[i].value = p.value;
        car.params[i].str = p.text.empty() ? nullptr : const_cast<wchar_t*>(keep(strings_, p.text));
    }
    const std::uint32_t engines = (std::max)(car.spec.num_engines, 1u);
    car.engine_voltage.assign(engines, 0.0f);
    car.engine_current.assign(engines, 0.0f);
    car.engine_force.assign(engines, 0.0f);

    abi::Engine* e = car.eng();
    std::memset(car.engine.data(), 0, car.engine.size());
    e->cab = car.spec.cab.present ? &car.cabin : nullptr;
    e->sound = &car.sound_cab;
    e->var = car.var.data();
    e->parameters = car.params.empty() ? nullptr : car.params.data();
    e->num_parameters = static_cast<abi::u16>(car.params.size());
    e->num_lights = static_cast<abi::u16>(car.lights.size());
    e->engine_voltage = car.engine_voltage.data();
    e->engine_current = car.engine_current.data();
    e->engine_force = car.engine_force.data();
    e->ept_value = -1.0f;
    l->loco_a = e;
    l->lib_module = m;
    l->can_work_with = m->can_work_with;
    l->change_loco = m->change_loco;
    l->lost_slave = m->lost_master;
    l->alsn = m->alsn;
    l->speed_limit = m->speed_limit;
    if (car.spec.kind == car_electric) {
        auto* el = car.electric();
        el->loco = car.eng_e();
        el->init = m->init;
        el->run = m->run;
        el->can_switch = m->can_switch;
        el->switched = m->switched;
    } else {
        auto* dl = car.diesel();
        dl->loco = car.eng_d();
        dl->init = m->init;
        dl->run = m->run;
        dl->can_switch = m->can_switch;
        dl->switched = m->switched;
    }

    // Auxiliary libraries (0x56DC3C, 0x56E970).
    car.aux.clear();
    car.aux_info.clear();
    car.aux.reserve(car.spec.aux.size());
    car.aux_info.reserve(car.spec.aux.size());
    for (const auto& spec : car.spec.aux) {
        Module* am = module_for(from_u16(spec.module));
        if (!am) continue;
        ++am->refs;
        AuxMirror info;
        info.module = am;
        info.calls = spec.calls;
        info.var.assign((std::max)((std::max)(spec.stack_size, am->version[abi::v_stack_size]), 1u), 0.0f);
        info.params.resize(spec.parameters.size());
        for (std::size_t i = 0; i < info.params.size(); ++i) {
            const auto& p = spec.parameters[i];
            info.params[i].name = const_cast<wchar_t*>(keep(strings_, p.name));
            info.params[i].value = p.value;
            info.params[i].str = p.text.empty() ? nullptr : const_cast<wchar_t*>(keep(strings_, p.text));
        }
        info.has_sound = spec.has_sound;
        info.sound.flags = 1 | 2;
        info.sound.sms = sound_table(spec.sound_table);
        car.aux_info.push_back(std::move(info));
        abi::AuxLibrary lib{};
        lib.lib = am;
        lib.loco = l;
        lib.name = const_cast<wchar_t*>(keep(strings_, spec.name));
        lib.change_loco = am->change_loco;
        lib.lost_master = am->lost_master;
        lib.alsn = am->alsn;
        lib.speed_limit = am->speed_limit;
        lib.init = am->init;
        lib.run = am->run;
        lib.can_switch = am->can_switch;
        lib.switched = am->switched;
        car.aux.push_back(lib);
    }
    // Pointers into aux_info only after it stopped growing.
    for (std::size_t i = 0; i < car.aux.size(); ++i) {
        auto& info = car.aux_info[i];
        car.aux[i].var = info.var.data();
        car.aux[i].stack_size = static_cast<abi::u32>(info.var.size());
        car.aux[i].num_params = static_cast<abi::u16>(info.params.size());
        car.aux[i].params = info.params.empty() ? nullptr : info.params.data();
        car.aux[i].sound = info.has_sound ? &info.sound : nullptr;
        by_aux_[&car.aux[i]] = &car;
    }
    car.aux_stack.libs = car.aux.empty() ? nullptr : car.aux.data();
    car.aux_stack.count = static_cast<abi::u32>(car.aux.size());
    l->aux_libs = car.aux.empty() ? nullptr : &car.aux_stack;
    car.has_engine = true;
    return true;
}

std::uint32_t Host::init_state(const CarMirror& car, bool player) const {
    std::uint32_t state = 0;
    // Bit 0: the train has cars (a wagon or a cold engine); bit 1: it starts running.
    for (const auto& c : cars_) {
        if (c->train == car.train && (!c->is_loco() || (c->loco()->loco_flags & 8))) state |= 1;
    }
    if (start_running_) state |= 2;
    // An activity's TrainState replaces both bits (RTS 0x5026FC: train +0x14 -> +0x48).
    if (start_state_ >= 0) state = static_cast<std::uint32_t>(start_state_) & 0xFFu;
    if (player) state |= 0x100;
    const auto type = train_of(car).type;
    if (type == 4) state |= 0x200;
    if (type == 5) state |= 0x400;
    return state;
}

std::uint32_t Host::run_state(const CarMirror& car) const {
    std::uint32_t state = 0;
    if (const_cast<CarMirror&>(car).wagon()->flags & 4) state |= 0x100;
    const auto type = train_of(car).type;
    if (type == 4) state |= 0x200;
    if (type == 5) state |= 0x400;
    return state;
}

bool Host::init_car(CarMirror& car, std::uint32_t state) {
    abi::Locomotive* l = car.loco();
    if (l->loco_flags & 0x20) return true; // already initialised
    Module& m = *car.module;
    bool ok = true;
    prepare_fpu();
    if (m.init) {
        auto* fn = reinterpret_cast<abi::InitFn>(m.init);
        abi::Locomotive* v = view(car, &m);
        const DWORD crash = guarded([&] { ok = fn(car.eng(), v, state, 0.0f, air_temperature_); });
        sync();
        if (crash) {
            fail(m, L"Init", crash);
            return false;
        }
    }
    if (ok) {
        // Auxiliary Init for every library that exports it (0x56EE38).
        for (std::size_t i = 0; i < car.aux.size() && ok; ++i) {
            Module& am = *car.aux_info[i].module;
            if (am.failed || !am.init) continue;
            auto* fn = reinterpret_cast<abi::AuxInitFn>(am.init);
            abi::Locomotive* v = view(car, &am);
            const DWORD crash = guarded([&] { ok = fn(&car.aux[i], v, state, 0.0f, air_temperature_); });
            sync();
            if (crash) {
                fail(am, L"Init (aux)", crash);
                ok = true;
            }
        }
    }
    if (!ok) {
        log(L"DLL " + m.path + L": Init returned false for car " + std::to_wstring(car.index));
        return false;
    }
    l->loco_flags |= 0x20;
    car.initialised = true;
    if (car.spec.kind == car_electric) car.wagon()->panto_raised = car.eng_e()->panto;
    // A DLL exporting SpeedLimit makes the train compute limits for it (train flag 0x400000).
    return true;
}

// --- entering a locomotive (0x468C74) ---------------------------------------------

bool Host::enter(const EnterRequest& request, const std::vector<CarInput>& inputs, StepReply& reply) {
    if (request.car >= cars_.size() || !cars_[request.car]->is_loco()) {
        log(L"enter: car " + std::to_wstring(request.car) + L" is not a locomotive");
        return false;
    }
    start_running_ = request.running;
    start_state_ = request.state;
    write_inputs(inputs);
    CarMirror& car = *cars_[request.car];
    abi::Locomotive* l = car.loco();
    for (auto& c : cars_) {
        if (c->is_loco()) c->loco()->loco_flags &= ~4u;
        c->wagon()->flags &= ~4u;
    }
    player_ = request.car;
    car.wagon()->flags |= 4;
    l->loco_flags |= 0x10 | 0x40;
    if (l->loco_flags & 0x88) { // cold or AI: no DLL
        collect(reply);
        return true;
    }
    if (!bind(car)) {
        collect(reply);
        return !car.spec.module.empty() ? false : true;
    }
    if (!init_car(car, init_state(car, true))) {
        collect(reply);
        return false;
    }
    // ChangeLoco: the previous master is a locomotive that had this one as a slave.
    const abi::Locomotive* previous = nullptr;
    for (auto& c : cars_) {
        if (c.get() == &car || !c->is_loco()) continue;
        abi::Locomotive* o = c->loco();
        if (!(o->loco_flags & 0x11)) continue;
        for (std::uint32_t k = 0; k < o->num_slaves; ++k)
            if (o->slaves && o->slaves[k] == l) previous = o;
    }
    aux_change_loco(car, false, previous, car.wagon()->flags);
    if (car.module->change_loco && !car.module->failed) {
        auto* fn = reinterpret_cast<abi::ChangeLocoFn>(car.module->change_loco);
        const std::uint32_t flags = car.wagon()->flags;
        prepare_fpu();
        abi::Locomotive* v = view(car, car.module);
        CarMirror* pc = previous ? car_of(previous) : nullptr;
        const abi::Locomotive* pv = pc ? view(*pc, car.module) : nullptr;
        const DWORD crash = guarded([&] { fn(v, pv, flags); });
        sync();
        if (crash) fail(*car.module, L"ChangeLoco", crash);
    }
    group(car);
    collect(reply);
    return true;
}

void Host::group(CarMirror& player, const bool players) {
    const auto& train = train_of(player);
    const std::size_t n = train.first + train.count;
    const std::size_t at = player.index;
    std::size_t first = at, last = at;
    // The group: cars joined by control cables (Flags byte 3: 0x10000 front, 0x20000 rear).
    if (player.wagon()->flags & 0x20000) {
        for (std::size_t k = at + 1; k < n; ++k) {
            const auto f = cars_[k]->wagon()->flags;
            if (!(f & 0x10000)) break;
            last = k;
            if (!(f & 0x20000)) break;
        }
    }
    if ((player.wagon()->flags & 0x10000) && at > train.first) {
        for (std::size_t k = at; k-- > train.first;) {
            const auto f = cars_[k]->wagon()->flags;
            if (!(f & 0x20000)) break;
            first = k;
            if (!(f & 0x10000)) break;
        }
    }
    abi::Locomotive* master = player.loco();
    const bool is_master = (master->loco_flags & 1) != 0;
    std::uint32_t slaves = static_cast<std::uint32_t>(last - first);
    player.slaves.clear();
    if (!(slaves && is_master)) slaves = 0;
    for (std::size_t m = first; m <= last; ++m) {
        CarMirror& c = *cars_[m];
        if (c.is_loco()) {
            c.loco()->first_in_group = const_cast<abi::Wagon**>(&train.wagons[first - train.first]);
            c.loco()->last_in_group = const_cast<abi::Wagon**>(&train.wagons[last - train.first]);
        }
        if (slaves && (!c.is_loco() || (c.loco()->loco_flags & 0x88))) {
            --slaves;
            continue;
        }
        if (!c.is_loco() || m == at) continue;
        abi::Locomotive* sl = c.loco();
        if (slaves && master->can_work_with && player.module && !player.module->failed) {
            bool works = true;
            auto* fn = reinterpret_cast<abi::CanWorkWithFn>(master->can_work_with);
            const wchar_t* series = keep(strings_, c.spec.series);
            prepare_fpu();
            if (const DWORD crash = guarded([&] { works = fn(sl, series); })) {
                fail(*player.module, L"CanWorkWith", crash);
                works = false;
            }
            if (!works) {
                --slaves;
                continue;
            }
        }
        // In the player's join only in his group (0x468C74); the regroup of a
        // train the player left loads the cabs alone (0x4F995C).
        sl->loco_flags |= players ? 0x10u | 4u : 0x10u;
        if (slaves) {
            sl->loco_flags &= ~1u;
            c.slaves.clear();
            sl->slaves = nullptr;
            sl->num_slaves = 0;
        }
        if (bind(c)) init_car(c, init_state(c, false));
        if (slaves) player.slaves.push_back(sl);
    }
    master->num_slaves = static_cast<std::uint32_t>(player.slaves.size());
    master->slaves = player.slaves.empty() ? nullptr : player.slaves.data();
}

// --- each step ---------------------------------------------------------------------

void Host::write_inputs(const std::vector<CarInput>& inputs) {
    const std::size_t n = (std::min)(inputs.size(), cars_.size());
    for (std::size_t i = 0; i < n; ++i) {
        CarMirror& car = *cars_[i];
        const CarInput& in = inputs[i];
        abi::Wagon* w = car.wagon();
        // The player bit is the host's (set on enter); the rest is the game's.
        w->flags = (in.flags & ~4u) | (w->flags & 4u);
        w->coupling = in.coupling;
        w->mass = in.mass;
        w->velocity = in.velocity;
        w->acceleration = in.acceleration;
        w->resistance = in.resistance;
        w->slope = in.slope;
        w->zord = in.zord;
        w->brake_force = in.brake_force;
        w->brake_cylinder_pressure = in.brake_cylinder;
        w->independent_brake_pressure = in.independent;
        w->cylinder_pressure = (std::max)(in.brake_cylinder, in.independent);
        w->auxiliary_pressure = in.auxiliary;
        w->pt_value = in.control;
        w->train_pipe_pressure = in.train_pipe;
        w->charging_pipe_pressure = in.charging_pipe;
        w->handbrake_value = in.handbrake;
        // RTS resets these before the DLLs run (0x424488).
        w->force = 0.0f;
        w->train_pipe_rate = in.train_pipe;
        w->ept_value = -1.0f;
        if (car.is_loco()) car.loco()->main_res_pressure = in.main_reservoir;
        if (car.spec.kind == car_electric) {
            // RTS 0x503CE4 sets these before Run (the current from the last
            // step's PowerConsuming); a burnt pantograph is LocoFlags 0x200.
            car.electric()->line_voltage = in.line_voltage;
            car.electric()->line_freq = in.line_frequency;
            car.electric()->line_current = in.line_current;
            if (in.line_burnt)
                car.loco()->loco_flags |= 0x200u;
            else
                car.loco()->loco_flags &= ~0x200u;
        }
    }
}

void Host::regroup() {
    // RTS 0x4F995C, train by train: the player's group, and in the trains
    // he left every master (LocoFlags 0x21) of a group.
    if (player_ < cars_.size() && cars_[player_]->is_loco()) regroup(*cars_[player_], true);
    for (auto& c : cars_) {
        if (c->train == 0 || !c->is_loco() || (c->loco()->loco_flags & 0x21) != 0x21) continue;
        regroup(*c, false);
    }
}

void Host::regroup(CarMirror& player, const bool players) {
    const std::vector<abi::Locomotive*> before = player.slaves;
    group(player, players);
    abi::Locomotive* master = player.loco();
    for (auto* slave : before) {
        if (std::find(player.slaves.begin(), player.slaves.end(), slave) != player.slaves.end()) continue;
        CarMirror* lost = car_of(slave);
        if (!lost) continue;
        slave->loco_flags &= ~4u; // no longer in the player's join
        if (!lost->initialised || !lost->module || lost->module->failed) continue;
        const std::uint32_t state = lost->wagon()->flags;
        aux_change_loco(*lost, true, master, state);
        if (lost->module->lost_master) {
            auto* fn = reinterpret_cast<abi::LostMasterFn>(lost->module->lost_master);
            prepare_fpu();
            abi::Locomotive* v = view(*lost, lost->module);
            const abi::Locomotive* mv = view(player, lost->module);
            const DWORD crash = guarded([&] { fn(v, mv, state); });
            sync();
            if (crash) fail(*lost->module, L"LostMaster", crash);
        }
    }
}

bool Host::step(const StepRequest& request, StepReply& reply) {
    day_time_ = request.day_time;
    night_ = request.night != 0;
    train_reversed_ = request.train_direction == 0;
    air_temperature_ = request.air_temperature;
    set_direction_ = 0;
    items_wanted_ = 0.0f;
    // The route database's signals, the track items and the player's
    // train's item list, and the SMS objects' trigger pairs.
    for (const auto& u : request.signal_units) {
        auto data = std::make_unique<abi::RTSSignalData>();
        data->tile[0] = u.tile[0];
        data->tile[1] = u.tile[1];
        std::memcpy(data->pos, u.position, sizeof(data->pos));
        data->name = u.name.empty() ? nullptr : const_cast<wchar_t*>(keep(strings_, u.name));
        units_[u.id] = std::move(data);
    }
    for (const auto& item : request.items) add_item(item);
    window_.clear();
    for (const auto& e : request.track_items) {
        const auto it = items_.find(e.item);
        if (it == items_.end()) continue;
        window_.push_back({e.distance, e.z, e.pos, reinterpret_cast<abi::TrackItem*>(it->second.get())});
    }
    for (const auto& p : request.sound_pairs) {
        if (p.car >= cars_.size()) continue;
        CarMirror& car = *cars_[p.car];
        std::array<std::uint32_t, 8>* block = nullptr;
        abi::SMSObject* sound = nullptr;
        if (p.object == sound_exterior) {
            block = &car.pairs_ext;
            sound = &car.sound_ext;
        } else if (p.object == sound_cabin) {
            block = &car.pairs_cab;
            sound = &car.sound_cab;
        } else if (p.object >= 0 && static_cast<std::size_t>(p.object) < car.aux_info.size()) {
            block = &car.aux_info[static_cast<std::size_t>(p.object)].pairs;
            sound = &car.aux_info[static_cast<std::size_t>(p.object)].sound;
        }
        if (!block) continue;
        std::memcpy(block->data(), p.on, sizeof(p.on));
        std::memcpy(block->data() + 4, p.known, sizeof(p.known));
        sound->paired_flags = block->data();
    }
    write_inputs(request.cars);
    // A control cable connected or parted (the car menu): the multiple-unit
    // groups again (RTS 0x4F995C).
    {
        bool changed = cables_.size() != cars_.size();
        cables_.resize(cars_.size());
        for (std::size_t i = 0; i < cars_.size(); ++i) {
            const std::uint32_t bits = cars_[i]->wagon()->flags & 0x30000u;
            if (bits != cables_[i]) changed = true;
            cables_[i] = bits;
        }
        if (changed) regroup();
    }
    signals(request);
    for (auto& car : cars_) {
        if (car->is_loco() && car->initialised && car->module && !car->module->failed)
            run_car(*car, request.dt, request.air_temperature);
    }
    // Cab switches move towards SetState and report arriving (0x526CB0).
    for (auto& car : cars_) {
        if (!car->initialised) continue;
        for (std::uint32_t id = 0; id < car->switches.size(); ++id) {
            abi::ActSwitch& sw = car->switches[id];
            const float target = static_cast<float>(sw.set_state);
            if (id < car->held_switches.size() && car->held_switches[id]) {
                // Dragged: the frame stays where the player put it, until
                // the switch is sent elsewhere (a DLL's SetSwitch, a click).
                if (sw.state == sw.set_state) continue;
                car->held_switches[id] = 0;
            }
            if (sw.state == sw.set_state) {
                sw.current_frame = target;
                continue;
            }
            const float rate = sw.anim_time > 0.0f ? request.dt / sw.anim_time : 1e9f;
            const auto* shortcuts = id < car->switch_shortcuts.size() ? &car->switch_shortcuts[id] : nullptr;
            const float next = sw.anim_time > 0.0f ? advance_switch(sw.current_frame, target, rate, shortcuts) : target;
            if (next == target) {
                sw.current_frame = target;
                const std::uint32_t previous = sw.state;
                sw.state = sw.set_state;
                switched(*car, id, previous);
            } else {
                sw.current_frame = next;
            }
        }
    }
    // Exterior doors a DLL sends by SetState itself: State follows once the
    // door has had its AnimTime (not traced in RTS; GM_DOOR_OPEN / CLOSE set
    // State at once, below).
    for (auto& car : cars_) {
        for (std::size_t k = 0; k < car->doors.size() && k < car->door_clock.size(); ++k) {
            auto& door = car->doors[k];
            if (door.state == door.set_state) {
                car->door_clock[k] = 0.0f;
                continue;
            }
            car->door_clock[k] += request.dt;
            if (car->door_clock[k] >= door.anim_time) {
                door.state = door.set_state;
                car->door_clock[k] = 0.0f;
            }
        }
    }
    collect(reply);
    reply.set_direction = set_direction_;
    reply.items_wanted = items_wanted_;
    return true;
}

void Host::run_car(CarMirror& car, float dt, float air_temperature) {
    abi::Wagon* w = car.wagon();
    const std::uint32_t state = run_state(car);
    if (car.spec.kind == car_electric) w->panto_raised = car.eng_e()->panto;
    const bool flipped = (w->flags & 1) != 0;
    if (flipped) {
        w->velocity = -w->velocity;
        w->acceleration = -w->acceleration;
    }
    prepare_fpu();
    for (std::size_t i = 0; i < car.aux.size(); ++i) {
        // Run handlers before the main DLL (AuxCall order < 0).
        Module& am = *car.aux_info[i].module;
        if (am.failed || !am.run) continue;
        for (const auto& call : car.aux_info[i].calls) {
            if (call.event != aux_run || call.order >= 0) continue;
            car.aux[i].lib_flags &= ~1u;
            auto* fn = reinterpret_cast<abi::AuxRunFn>(am.run);
            abi::Locomotive* v = view(car, &am);
            const DWORD crash = guarded([&] { fn(&car.aux[i], v, state, dt, air_temperature); });
            sync();
            if (crash) fail(am, L"Run (aux)", crash);
        }
    }
    if (car.module->run) {
        auto* fn = reinterpret_cast<abi::RunFn>(car.module->run);
        abi::Locomotive* v = view(car, car.module);
        const DWORD crash = guarded([&] { fn(car.eng(), v, state, dt, air_temperature); });
        sync();
        if (crash) fail(*car.module, L"Run", crash);
    }
    for (std::size_t i = 0; i < car.aux.size(); ++i) {
        Module& am = *car.aux_info[i].module;
        if (am.failed || !am.run) continue;
        for (const auto& call : car.aux_info[i].calls) {
            if (call.event != aux_run || call.order < 0) continue;
            car.aux[i].lib_flags |= 1u;
            auto* fn = reinterpret_cast<abi::AuxRunFn>(am.run);
            abi::Locomotive* v = view(car, &am);
            const DWORD crash = guarded([&] { fn(&car.aux[i], v, state, dt, air_temperature); });
            sync();
            if (crash) fail(am, L"Run (aux)", crash);
        }
    }
    if (flipped) {
        w->velocity = -w->velocity;
        w->acceleration = -w->acceleration;
    }
    if (car.spec.kind == car_electric) w->panto_raised = car.eng_e()->panto;
}

void Host::signals(const StepRequest& request) {
    const abi::SpeedLimitDescr route{request.route.limit, request.route.next_limit, request.route.distance};
    const abi::SpeedLimitDescr signal{request.signal.limit, request.signal.next_limit, request.signal.distance};
    const abi::SpeedLimitDescr event{request.event.limit, request.event.next_limit, request.event.distance};
    std::vector<abi::SignalsInfo> ahead(request.signals_ahead.size());
    for (std::size_t i = 0; i < ahead.size(); ++i) {
        const auto& s = request.signals_ahead[i];
        ahead[i].distance = s.distance;
        std::memcpy(ahead[i].aspect, s.aspect, sizeof(s.aspect));
        ahead[i].flags = s.flags;
        ahead[i].speed_limit = s.speed_limit;
        ahead[i].signal = unit(s.unit);
    }
    float limit = -1.0f;
    for (const float v : {route.limit, signal.limit, event.limit})
        if (v >= 0.0f && (limit < 0.0f || v < limit)) limit = v;
    // Cabin::Signal (RTS 0x508520): the leading cab gets the next signal
    // (distance, aspect, its speed limit), the cab at the other end
    // BLOCK_OBSTRUCTED (8) at 0 m; the DLLs read it, the cab's locomotive
    // signal lights show its aspect.
    if (train_count_ > 0 && trains_[0]->count > 0) {
        // The player's train: the signal pass runs for train types 1, 2 and 4.
        const auto& train = *trains_[0];
        const std::size_t leading = train_reversed_ ? train.first + train.count - 1 : train.first;
        for (std::size_t i = train.first; i < train.first + train.count; ++i) {
            CarMirror& car = *cars_[i];
            if (!car.spec.cab.present) continue;
            auto& s = car.cabin.signal;
            s = {};
            s.speed_limit = -1.0f;
            if (i == leading && !ahead.empty()) {
                s.distance = ahead.front().distance;
                std::memcpy(s.aspect, ahead.front().aspect, sizeof(s.aspect));
                s.flags = ahead.front().flags;
                s.speed_limit = ahead.front().speed_limit;
                s.signal = ahead.front().signal;
            } else if (i == leading) {
                s.aspect[0] = 0; // no signal known ahead
            } else {
                s.aspect[0] = 8;
            }
        }
    }
    prepare_fpu();
    for (auto& carp : cars_) {
        CarMirror& car = *carp;
        if (car.train != 0 || !car.is_loco() || !car.initialised || !car.module || car.module->failed) continue;
        if (car.module->speed_limit) {
            auto* fn = reinterpret_cast<abi::SpeedLimitFn>(car.module->speed_limit);
            abi::Locomotive* v = view(car, car.module);
            const DWORD crash = guarded([&] { fn(v, route, signal, event); });
            sync();
            if (crash) fail(*car.module, L"SpeedLimit", crash);
        }
        for (std::size_t i = 0; i < car.aux.size(); ++i) {
            Module& am = *car.aux_info[i].module;
            if (am.failed || !am.speed_limit) continue;
            for (const auto& call : car.aux_info[i].calls) {
                if (call.event != aux_speed_limit) continue;
                auto* fn = reinterpret_cast<abi::AuxSpeedLimitFn>(am.speed_limit);
                const DWORD crash = guarded([&] { fn(&car.aux[i], route, signal, event); });
                sync();
                if (crash) fail(am, L"SpeedLimit (aux)", crash);
                break;
            }
        }
        // ALSN only for cabs with scripted signal lights (Cabin::SignalScripted).
        if (!car.spec.cab.present || !car.cabin.signal_scripted) continue;
        // RTS 0x508520: ALSN(loco, A ahead of the front, B behind the rear,
        // C being passed, the lowest limit, the next speed post's limit and
        // its distance (floats in the two integer slots), the locomotive
        // facing against the train's direction).
        const bool reversed = ((car.wagon()->flags & 1) != 0) != train_reversed_;
        std::int32_t x = 0, y = 0;
        std::memcpy(&x, &request.next_limit, sizeof x);
        std::memcpy(&y, &request.next_limit_distance, sizeof y);
        // Older DLLs get 28-byte SignalsInfo (no RTSSignalData pointer, adapter code 23).
        struct Array {
            std::vector<abi::SignalsInfo> info;
            std::vector<std::uint8_t> packed;
        };
        std::array<Array, 3> arrays;
        const std::vector<SignalInput>* sources[3] = {&request.alsn_ahead, &request.alsn_behind, &request.alsn_passing};
        for (std::size_t k = 0; k < 3; ++k) {
            arrays[k].info.resize(sources[k]->size());
            for (std::size_t j = 0; j < sources[k]->size(); ++j) {
                const auto& s = (*sources[k])[j];
                auto& d = arrays[k].info[j];
                d.distance = s.distance;
                std::memcpy(d.aspect, s.aspect, sizeof(s.aspect));
                d.flags = s.flags;
                d.speed_limit = s.speed_limit;
                d.signal = unit(s.unit);
            }
        }
        auto signals_for = [&](const Module& m, Array& array) -> abi::SignalsInfo* {
            if (array.info.empty()) return nullptr;
            if (!m.signals_variant) return array.info.data();
            array.packed.assign(array.info.size() * 28, 0);
            for (std::size_t k = 0; k < array.info.size(); ++k) std::memcpy(array.packed.data() + k * 28, &array.info[k], 28);
            return reinterpret_cast<abi::SignalsInfo*>(array.packed.data());
        };
        const auto na = static_cast<std::uint32_t>(arrays[0].info.size());
        const auto nb = static_cast<std::uint32_t>(arrays[1].info.size());
        const auto nc = static_cast<std::uint32_t>(arrays[2].info.size());
        if (car.module->alsn) {
            auto* fn = reinterpret_cast<abi::ALSNFn>(car.module->alsn);
            abi::Locomotive* v = view(car, car.module);
            abi::SignalsInfo* a = signals_for(*car.module, arrays[0]);
            abi::SignalsInfo* b = signals_for(*car.module, arrays[1]);
            abi::SignalsInfo* c = signals_for(*car.module, arrays[2]);
            const DWORD crash = guarded([&] { fn(v, a, na, b, nb, c, nc, limit, x, y, reversed); });
            sync();
            if (crash) fail(*car.module, L"ALSN", crash);
        }
        for (std::size_t i = 0; i < car.aux.size(); ++i) {
            Module& am = *car.aux_info[i].module;
            if (am.failed || !am.alsn) continue;
            for (const auto& call : car.aux_info[i].calls) {
                if (call.event != aux_alsn) continue;
                auto* fn = reinterpret_cast<abi::AuxALSNFn>(am.alsn);
                abi::SignalsInfo* a = signals_for(am, arrays[0]);
                abi::SignalsInfo* b = signals_for(am, arrays[1]);
                abi::SignalsInfo* c = signals_for(am, arrays[2]);
                const DWORD crash = guarded([&] { fn(&car.aux[i], a, na, b, nb, c, nc, limit, x, y, reversed); });
                sync();
                if (crash) fail(am, L"ALSN (aux)", crash);
                break;
            }
        }
    }
}

void Host::switched(CarMirror& car, std::uint32_t id, std::uint32_t previous) {
    if (!car.module || car.module->failed) return;
    if (GetEnvironmentVariableW(L"OPENRTS_DLLHOST_TRACE", nullptr, 0)) log(L"Switched " + std::to_wstring(id) + L" from " + std::to_wstring(previous) + L" to " + std::to_wstring(car.switches[id].state));
    prepare_fpu();
    aux_event(car, aux_switched, false, id, previous);
    if (car.module->switched) {
        auto* fn = reinterpret_cast<abi::SwitchedFn>(car.module->switched);
        abi::Locomotive* v = view(car, car.module);
        const DWORD crash = guarded([&] { fn(v, car.eng(), id, previous); });
        sync();
        if (crash) fail(*car.module, L"Switched", crash);
    }
    aux_event(car, aux_switched, true, id, previous);
}

bool Host::aux_event(CarMirror& car, std::uint16_t event, bool after, std::uint32_t a, std::uint32_t b) {
    bool result = true;
    for (std::size_t i = 0; i < car.aux.size(); ++i) {
        Module& am = *car.aux_info[i].module;
        if (am.failed) continue;
        for (const auto& call : car.aux_info[i].calls) {
            if (call.event != event || (call.order < 0) == after) continue;
            if (after)
                car.aux[i].lib_flags |= 1u;
            else
                car.aux[i].lib_flags &= ~1u;
            if (event == aux_switched && am.switched) {
                auto* fn = reinterpret_cast<abi::AuxSwitchedFn>(am.switched);
                abi::Locomotive* v = view(car, &am);
                const DWORD crash = guarded([&] { fn(v, &car.aux[i], a, b); });
                sync();
                if (crash) fail(am, L"Switched (aux)", crash);
            } else if (event == aux_can_switch && am.can_switch) {
                auto* fn = reinterpret_cast<abi::AuxCanSwitchFn>(am.can_switch);
                bool ok = true;
                abi::Locomotive* v = view(car, &am);
                const DWORD crash = guarded([&] { ok = fn(v, &car.aux[i], a, b); });
                sync();
                if (crash) fail(am, L"CanSwitch (aux)", crash);
                result = result && ok;
            }
        }
    }
    return result;
}

void Host::aux_change_loco(CarMirror& car, bool lost, const abi::Locomotive* other, std::uint32_t state) {
    // PROBABLE: RTS keeps ChangeLoco / LostMaster entries (events 7 / 8) for
    // libraries exporting them; there is no AuxCall keyword for these.
    for (std::size_t i = 0; i < car.aux.size(); ++i) {
        Module& am = *car.aux_info[i].module;
        void* fp = lost ? am.lost_master : am.change_loco;
        if (am.failed || !fp) continue;
        car.aux[i].lib_flags |= 1u;
        auto* fn = reinterpret_cast<abi::AuxChangeLocoFn>(fp);
        CarMirror* oc = other ? car_of(other) : nullptr;
        const abi::Locomotive* ov = oc ? view(*oc, &am) : nullptr;
        const DWORD crash = guarded([&] { fn(&car.aux[i], ov, state); });
        sync();
        if (crash) fail(am, lost ? L"LostMaster (aux)" : L"ChangeLoco (aux)", crash);
    }
}

bool Host::allows_switch(CarMirror& car, const std::uint32_t id, const std::uint32_t state) {
    bool allowed = true;
    if (car.initialised && car.module && !car.module->failed) {
        prepare_fpu();
        allowed = aux_event(car, aux_can_switch, false, id, state);
        if (allowed && car.module->can_switch) {
            auto* fn = reinterpret_cast<abi::CanSwitchFn>(car.module->can_switch);
            bool ok = true;
            abi::Locomotive* v = view(car, car.module);
            const DWORD crash = guarded([&] { ok = fn(v, car.eng(), id, state); });
            sync();
            if (crash) fail(*car.module, L"CanSwitch", crash);
            allowed = ok;
        }
        if (allowed) allowed = aux_event(car, aux_can_switch, true, id, state);
    }
    return allowed;
}

bool Host::can_switch(const CanSwitchRequest& request, bool& allowed) {
    allowed = false;
    if (request.car >= cars_.size()) return false;
    CarMirror& car = *cars_[request.car];
    if (request.id >= car.switches.size()) return false;
    abi::ActSwitch& sw = car.switches[request.id];
    // RTS 0x437AA4: SetState takes the new state before CanSwitch and a
    // refusal puts the old one back; what the DLL writes there meanwhile
    // stays. The ChS7 sends its controller from 11 straight to +1 (1) and
    // from 2 back to 0 (12); its field weakening is another switch's.
    const std::uint32_t before = sw.set_state;
    sw.set_state = request.set_state;
    allowed = allows_switch(car, request.id, request.set_state);
    if (!allowed) sw.set_state = before;
    return true;
}

SwitchState Host::switch_state(const std::uint32_t car, const std::uint32_t id) const {
    if (car >= cars_.size() || id >= cars_[car]->switches.size()) return {};
    const auto& sw = cars_[car]->switches[id];
    return {sw.state, sw.set_state, sw.fixed_state, sw.flags, sw.current_frame};
}

// RTS 0x437E64 (a drag) and 0x527190 (let go), in states rather than the
// node's frames (the game has converted the cursor's move).
bool Host::drag_switch(const DragSwitchRequest& request, bool& moved) {
    moved = false;
    if (request.car >= cars_.size()) return false;
    CarMirror& car = *cars_[request.car];
    if (request.id >= car.switches.size()) return false;
    abi::ActSwitch& sw = car.switches[request.id];
    if (car.held_switches.size() < car.switches.size()) car.held_switches.resize(car.switches.size(), 0);
    auto& held = car.held_switches[request.id];
    constexpr std::uint32_t disabled = 0x20000, loop = 0x10000, key_flag = 2, precise = 0x40;
    if ((sw.flags & disabled) != 0) return true;
    const std::uint32_t key = (sw.flags & key_flag) != 0 ? 1u : 0u;
    // The node's positions (a KEY switch's key-out state 0 is not one).
    const std::uint32_t positions = sw.num_states > key ? sw.num_states - key : 0u;
    if (positions <= 1) return true;
    const float from = (std::max)(sw.current_frame - static_cast<float>(key), 0.0f);
    if (request.release != 0) {
        if ((sw.flags & precise) != 0) {
            // A PRECISE switch stays where it was left (0x4121BC).
            sw.set_state = sw.state;
        } else if (key == 0 || sw.set_state != 0) {
            // A controller takes the nearest position and animates there.
            sw.set_state = static_cast<std::uint32_t>(std::floor(from + 0.5f)) + key;
            held = 0;
        }
        moved = true;
        return true;
    }
    const float states = static_cast<float>(positions);
    float to = from + request.delta;
    if (to > states - 1.0f) to = (sw.flags & loop) != 0 ? to - states : states - 1.0f;
    if (to < 0.0f) to = (sw.flags & loop) != 0 ? to + states : 0.0f;
    // The state asked of the DLL: one further in the move's direction.
    auto asked = static_cast<std::uint32_t>(std::floor(to));
    if (key != 0) {
        asked += 1;
    } else if (request.delta > 0.0f && asked < sw.num_states) {
        ++asked;
    } else if (request.delta < 0.0f && asked > 0) {
        --asked;
    }
    if (!allows_switch(car, request.id, asked)) return true;
    const auto whole_from = static_cast<std::uint32_t>(std::floor(from));
    const auto whole_to = static_cast<std::uint32_t>(std::floor(to));
    sw.current_frame = to + static_cast<float>(key);
    sw.set_state = whole_to + key;
    held = 1;
    // Past a whole position: State follows and the DLL hears Switched.
    if (whole_to != whole_from && sw.state != sw.set_state) {
        const std::uint32_t previous = sw.state;
        sw.state = sw.set_state;
        switched(car, request.id, previous);
    }
    moved = true;
    return true;
}

// --- adapters (RTS 0x537718 / 0x53782C) ------------------------------------------------

namespace {

// Loco shadow: the mirror's ranges [from, to), in order, for each variant.
struct Range { std::uint32_t from, to; };

std::vector<Range> loco_ranges(std::uint32_t variant, std::uint32_t size) {
    if (variant == 1) return {{0, 384}, {388, 476}, {480, size}};
    return {{0, 384}, {388, size}}; // variant 2
}

std::uint32_t object_size(const CarMirror& car) {
    return car.spec.kind == car_electric ? sizeof(abi::ElectricLocomotive)
         : car.spec.kind == car_diesel   ? sizeof(abi::DieselLocomotive)
                                         : sizeof(abi::Wagon);
}

} // namespace

abi::Locomotive* Host::view(CarMirror& car, const Module* module) {
    const std::uint32_t variant = module ? module->loco_variant : 0;
    if (!variant || variant > 2) return car.loco();
    auto& shadow = car.shadows[variant];
    if (!shadow.loco_live) {
        std::uint32_t at = 0;
        for (const auto& r : loco_ranges(variant, object_size(car))) {
            std::memcpy(shadow.loco.data() + at, car.object.data() + r.from, r.to - r.from);
            at += r.to - r.from;
        }
        shadow.loco_live = true;
        live_shadows_.emplace_back(&car, variant);
    }
    return reinterpret_cast<abi::Locomotive*>(shadow.loco.data());
}

abi::Cabin* Host::cab_view(CarMirror& car, const Module* module) {
    if (!car.spec.cab.present) return nullptr;
    if (!module || module->cab_variant != 1) return &car.cabin;
    // The cab shadow lives in shadows[1] whatever the loco variant is.
    auto& shadow = car.shadows[1];
    if (!shadow.cab_live) {
        const auto* real = reinterpret_cast<const std::uint8_t*>(&car.cabin);
        std::memcpy(shadow.cab.data(), real, 136);
        std::memcpy(shadow.cab.data() + 136, real + 140, sizeof(abi::Cabin) - 140);
        shadow.cab_live = true;
        live_shadows_.emplace_back(&car, 0x100u);
    }
    return reinterpret_cast<abi::Cabin*>(shadow.cab.data());
}

void Host::sync() {
    for (const auto& [car, what] : live_shadows_) {
        if (what == 0x100u) {
            auto& shadow = car->shadows[1];
            if (!shadow.cab_live) continue;
            auto* real = reinterpret_cast<std::uint8_t*>(&car->cabin);
            std::memcpy(real + 12, shadow.cab.data() + 12, 136 - 12); // the adapter fields stay the mirror's
            std::memcpy(real + 140, shadow.cab.data() + 136, sizeof(abi::Cabin) - 140);
            shadow.cab_live = false;
            continue;
        }
        auto& shadow = car->shadows[what];
        if (!shadow.loco_live) continue;
        std::uint32_t at = 0;
        for (const auto& r : loco_ranges(what, object_size(*car))) {
            const std::uint32_t from = (std::max)(r.from, 12u); // keep the mirror's adapter fields
            std::memcpy(car->object.data() + from, shadow.loco.data() + at + (from - r.from), r.to - from);
            at += r.to - r.from;
        }
        shadow.loco_live = false;
    }
    live_shadows_.clear();
}

Module* Host::module_of(const void* dll) {
    for (auto& m : modules_)
        if (m.get() == dll) return m.get();
    return nullptr;
}

// --- outputs ---------------------------------------------------------------------------

void Host::collect(StepReply& reply) {
    reply.cars.resize(cars_.size());
    for (std::size_t i = 0; i < cars_.size(); ++i) collect_car(*cars_[i], reply.cars[i]);
}

namespace {

void drain(abi::SMSObject& sound, std::vector<std::uint16_t>& out) {
    out.clear();
    for (std::uint32_t k = 0; k < abi::max_sound_queue; ++k) {
        const std::uint32_t idx = (sound.queue_pos + k) % abi::max_sound_queue;
        if (sound.queue[idx]) {
            out.push_back(sound.queue[idx]);
            sound.queue[idx] = 0;
        }
    }
    sound.queue_pos = 0;
}

std::u16string message_text(const wchar_t* text) {
    std::u16string r;
    guarded([&] {
        if (!text) return;
        std::size_t n = 0;
        while (n < 1024 && text[n]) ++n;
        r = to_u16(text, n);
    });
    return r;
}

} // namespace

void Host::build_screens(CarMirror& car, const CabSpec& cab) {
    car.screens.clear();
    for (const auto& d : cab.displays) {
        if (!d.has_screen || !(d.flags & 0x800) || d.id >= car.displays.size()) continue;
        CarMirror::Screen m;
        m.display = d.id;
        // RTS's model axes negate x (0x529F00 reads -x).
        const float tl[3] = {-d.corners[0], d.corners[1], d.corners[2]};
        const float tr[3] = {-d.corners[3], d.corners[4], d.corners[5]};
        const float br[3] = {-d.corners[6], d.corners[7], d.corners[8]};
        auto& s = m.screen;
        float radius2 = 0.0f, width2 = 0.0f, height2 = 0.0f;
        for (int k = 0; k < 3; ++k) {
            s.pos[k] = (tl[k] + br[k]) * 0.5f;
            radius2 += (tl[k] - s.pos[k]) * (tl[k] - s.pos[k]);
            width2 += (tl[k] - tr[k]) * (tl[k] - tr[k]);
            height2 += (tr[k] - br[k]) * (tr[k] - br[k]);
            s.pivot[k] = tl[k];
        }
        s.pos[3] = std::sqrt(radius2);
        s.width = std::sqrt(width2);
        s.height = std::sqrt(height2);
        if (s.width > 0.0f && s.height > 0.0f) {
            float right[3], up[3];
            for (int k = 0; k < 3; ++k) {
                right[k] = (tl[k] - tr[k]) / s.width;
                up[k] = (tr[k] - br[k]) / s.height;
                s.up[k] = up[k];
            }
            s.norm[0] = right[1] * up[2] - right[2] * up[1];
            s.norm[1] = right[2] * up[0] - right[0] * up[2];
            s.norm[2] = right[0] * up[1] - right[1] * up[0];
            const float n = std::sqrt(s.norm[0] * s.norm[0] + s.norm[1] * s.norm[1] + s.norm[2] * s.norm[2]);
            if (n > 0.0f)
                for (float& v : s.norm) v /= n;
        }
        m.objects.resize(d.screen.size());
        m.text.resize(d.screen.size());
        for (std::size_t k = 0; k < d.screen.size(); ++k) {
            const auto& spec = d.screen[k];
            auto& o = m.objects[k];
            o.id = static_cast<abi::u32>(k);
            o.type = spec.type;
            o.shader = spec.shader;
            o.parent = car.wagon();
            o.flags = spec.flags;
            std::memcpy(o.area, spec.area, sizeof(o.area));
            std::memcpy(o.uv, spec.uv, sizeof(o.uv));
            std::memcpy(o.pt, spec.pt, sizeof(o.pt));
            o.anim_time = spec.anim_time;
            o.value = spec.value;
            o.fade[1] = spec.fade;
            o.fade[2] = spec.blink;
            o.state = o.set_state = spec.initial_state;
            o.colour = spec.colour;
            o.num_tex = spec.textures;
            o.num_frames = spec.frames;
            if (spec.text_size > 0) m.text[k].assign(spec.text_size, L'\0');
        }
        car.screens.push_back(std::move(m));
    }
    // Pointers once the list stopped growing.
    for (auto& m : car.screens) {
        const auto n = static_cast<abi::u32>(m.objects.size());
        m.screen.num_sub = n;
        m.screen.obj = m.objects.empty() ? nullptr : m.objects.data();
        for (std::size_t k = 0; k < m.objects.size(); ++k) {
            auto& o = m.objects[k];
            o.hint = m.text[k].empty() ? nullptr : m.text[k].data();
            o.belongs_to_list = m.screen.obj;
            o.parent_list_count = n;
        }
        for (const auto& d : cab.displays) {
            if (d.id != m.display) continue;
            for (std::size_t k = 0; k < d.screen.size() && k < m.objects.size(); ++k) {
                const auto parent = d.screen[k].parent;
                m.objects[k].parent_sub = parent >= 0 && static_cast<std::size_t>(parent) < m.objects.size()
                    ? static_cast<abi::u32>(parent + 1)
                    : 0u;
            }
            if (d.reference_screen >= 0) {
                for (auto& other : car.screens)
                    if (other.display == static_cast<std::uint32_t>(d.reference_screen)) m.screen.reference_scr = &other.screen;
            }
        }
        // RTS keeps the ActScreen pointer in the display's Granularity slot.
        abi::ActScreen* pointer = &m.screen;
        static_assert(sizeof(pointer) == sizeof(float));
        std::memcpy(&car.displays[m.display].granularity, &pointer, sizeof(pointer));
    }
}

void Host::collect_car(CarMirror& car, CarOutput& out) {
    abi::Wagon* w = car.wagon();
    out.has_engine = car.initialised;
    out.panto_raised = w->panto_raised;
    out.coupling = w->coupling;
    out.loco_flags = car.is_loco() ? car.loco()->loco_flags : 0;
    auto vars = [](const abi::SMSObject& sms, std::array<float, 8>& to) {
        const float* from[8] = {sms.var1, sms.var2, sms.var3, sms.var4, sms.var5, sms.var6, sms.var7, sms.var8};
        for (int k = 0; k < 8; ++k) to[static_cast<std::size_t>(k)] = from[k][0];
    };
    drain(car.sound_ext, out.sound_ext);
    drain(car.sound_cab, out.sound_cab);
    // Each AuxLibrarySound is an SMS of its own (Brakes.sms, Usavp.sms):
    // its codes mean nothing to the cab's script.
    out.aux_sound.resize(car.aux_info.size());
    for (std::size_t i = 0; i < car.aux_info.size(); ++i) {
        auto& info = car.aux_info[i];
        auto& o = out.aux_sound[i];
        o.triggers.clear();
        if (!info.has_sound) continue;
        drain(info.sound, o.triggers);
        vars(info.sound, o.vars);
    }
    out.light_flags.resize(car.lights.size());
    for (std::size_t i = 0; i < car.lights.size(); ++i) out.light_flags[i] = car.lights[i].flags;
    out.messages.clear();
    out.switches.resize(car.switches.size());
    for (std::size_t i = 0; i < car.switches.size(); ++i) {
        const auto& sw = car.switches[i];
        out.switches[i] = SwitchState{sw.state, sw.set_state, sw.fixed_state, sw.flags, sw.current_frame};
    }
    out.cab_aspect = car.spec.cab.present ? car.cabin.signal.aspect[0] : 8u;
    out.displays.resize(car.displays.size());
    for (std::size_t i = 0; i < car.displays.size(); ++i) {
        const auto& d = car.displays[i];
        DisplayState s;
        s.value = d.value;
        s.state = d.state;
        s.show_hint = d.show_hint;
        std::memcpy(s.fade, d.fade, sizeof(s.fade));
        out.displays[i] = s;
    }
    out.doors.resize(car.doors.size());
    for (std::size_t k = 0; k < car.doors.size(); ++k) out.doors[k] = {car.doors[k].state, car.doors[k].set_state};
    out.exhausts.resize(car.exhaust.size());
    for (std::size_t k = 0; k < car.exhaust.size(); ++k) {
        const auto& e = car.exhaust[k];
        out.exhausts[k] = {e.rate, e.magnitude, e.dispersion, e.mass, e.colour};
    }
    // Screens: RTS copies SetState to State each time it draws one (0x46B668).
    out.screens.resize(car.screens.size());
    for (std::size_t s = 0; s < car.screens.size(); ++s) {
        auto& m = car.screens[s];
        auto& o = out.screens[s];
        o.display = m.display;
        o.objects.resize(m.objects.size());
        o.texts.resize(m.objects.size());
        for (std::size_t k = 0; k < m.objects.size(); ++k) {
            auto& sub = m.objects[k];
            sub.state = sub.set_state;
            auto& st = o.objects[k];
            st.state = sub.state;
            st.value = sub.value;
            std::memcpy(st.fade, sub.fade, sizeof(st.fade));
            std::memcpy(st.area, sub.area, sizeof(st.area));
            o.texts[k].clear();
            if (!m.text[k].empty()) {
                const auto& t = m.text[k];
                std::size_t n = 0;
                while (n < t.size() && t[n]) ++n;
                o.texts[k].assign(reinterpret_cast<const char16_t*>(t.data()), n);
            }
        }
    }
    out.internal_lights.resize(car.internal_lights.size());
    for (std::size_t i = 0; i < car.internal_lights.size(); ++i) out.internal_lights[i] = car.internal_lights[i].state;
    out.tex_changes.resize(car.tex_changes.size());
    for (std::size_t i = 0; i < car.tex_changes.size(); ++i) out.tex_changes[i] = car.tex_changes[i].set_state;
    vars(car.sound_ext, out.sms_ext);
    vars(car.sound_cab, out.sms_cab);
    if (!car.initialised) {
        out.engine = {};
        return;
    }
    abi::Engine* e = car.eng();
    EngineOutput& o = out.engine;
    o.power = e->power;
    o.force = e->force;
    o.brake_force = e->brake_force;
    o.independent_brake = e->independent_brake_value;
    o.train_pipe_rate = e->train_pipe_rate;
    o.auxiliary_rate = e->auxilary_rate;
    o.main_reservoir_rate = e->main_res_rate;
    o.charging_rate = e->charging_rate;
    o.handbrake_percent = e->handbrake_percent;
    o.dynamic_brake_percent = e->dynamic_brake_percent;
    o.ep_value = e->ept_value;
    o.equalising = e->ur;
    o.systems = e->brake_systems_engaged;
    o.alsn_on = e->alsn_on;
    o.sanding = e->sanding;
    o.engine_flags = e->engine_flags;
    o.reverse = e->reverse;
    o.wheelslip = e->wheelslip;
    o.throttle = e->throttle_position;
    if (car.spec.kind == car_electric) {
        o.panto = car.eng_e()->panto;
        o.main_switch = car.eng_e()->main_switch;
        o.power_consuming = car.eng_e()->power_consuming;
    } else if (car.spec.kind == car_diesel) {
        o.diesel_on = car.eng_d()->diesel_on;
        o.fuel_consuming = car.eng_d()->fuel_consuming;
        o.rpm = car.eng_d()->rpm;
    }
    // GlobalMsg: {action, id} pairs from the front; GM_MSG_POST carries a
    // wchar_t* in the next two shorts (ts.h ShowMessage). Consumed here.
    for (std::uint32_t i = 0; i < abi::max_msg_queue * 2;) {
        const std::uint16_t action = e->global_msg[i];
        if (!action) break;
        GlobalMessage m;
        m.action = action;
        m.id = i + 1 < abi::max_msg_queue * 2 ? e->global_msg[i + 1] : 0;
        if (action == abi::gm_msg_post && i + 3 < abi::max_msg_queue * 2) {
            const wchar_t* text = nullptr;
            std::memcpy(&text, &e->global_msg[i + 2], sizeof(text));
            m.text = message_text(text);
            i += 4;
        } else {
            i += 2;
        }
        // GM_DOOR_OPEN / GM_DOOR_CLOSE id: every exterior door of the train
        // with that GlobalID (the ED9M opens its sides so: 1 right, 2 left).
        // GM_TEX_CHANGE / GM_TEX_REVERT id: the texture changes of the
        // train's cabs with that GlobalID.
        if ((action == abi::gm_tex_change || action == abi::gm_tex_revert) && m.id != 0) {
            for (auto& other : cars_) {
                for (auto& change : other->tex_changes)
                    if (change.global_id == m.id) change.set_state = action == abi::gm_tex_change ? 1 : 0;
            }
        }
        // GM_LIGHT_ON / GM_LIGHT_OFF id: the cab and salon lamps of the
        // train with that GlobalID (the ED9M's salon light, group 2).
        if ((action == abi::gm_light_on || action == abi::gm_light_off) && m.id != 0) {
            for (auto& other : cars_) {
                for (auto& lamp : other->internal_lights)
                    if (lamp.global_id == m.id) lamp.state = action == abi::gm_light_on ? 1u : 0u;
            }
        }
        // RTS (0x424488) sets the doors' State at once (and their nodes'
        // target): the door animation follows on its own. DLLs read State
        // right after: the ED9M's door lamp went back on when it stayed 0.
        if (action == abi::gm_door_open || action == abi::gm_door_close) {
            const std::uint32_t open = action == abi::gm_door_open ? 1u : 0u;
            for (auto& other : cars_) {
                for (std::size_t k = 0; k < other->doors.size(); ++k) {
                    auto& door = other->doors[k];
                    if (door.global_id != m.id || m.id == 0) continue;
                    door.state = door.set_state = open;
                    if (k < other->door_clock.size()) other->door_clock[k] = 0.0f;
                }
                // The same doors seen from inside: the cab's or salon's
                // InteriorDoors with that GlobalID (the ED9M's DOOR_A .. E).
                for (auto& door : other->cab_doors)
                    if (door.global_id == m.id && m.id != 0) door.state = door.set_state = open;
            }
        }
        out.messages.push_back(std::move(m));
    }
    std::memset(e->global_msg, 0, sizeof(e->global_msg));
}

// --- host function support -------------------------------------------------------------

CarMirror* Host::car_of(const void* object) {
    const auto it = by_object_.find(object);
    return it == by_object_.end() ? nullptr : it->second;
}

CarMirror* Host::car_of_cabin(const void* cabin) {
    const auto it = by_cabin_.find(cabin);
    return it == by_cabin_.end() ? nullptr : it->second;
}

abi::AuxLibrary* Host::aux_of(const void* object) {
    const auto it = by_aux_.find(object);
    return it == by_aux_.end() ? nullptr : const_cast<abi::AuxLibrary*>(static_cast<const abi::AuxLibrary*>(object));
}

CarMirror* Host::car_of_aux(const abi::AuxLibrary* aux) {
    const auto it = by_aux_.find(aux);
    return it == by_aux_.end() ? nullptr : it->second;
}

int Host::get_integer(const abi::Locomotive* loco, std::uint32_t code, int index) {
    CarMirror* car = loco ? car_of(loco) : nullptr;
    switch (code) {
    case 1: // RTS_VAR_TIME: world time, s
        return static_cast<int>(day_time_);
    case 2:   // RTS_NUM_WAG: wagons (not engines); index -1 ahead of this car, 1 behind, 0 all (PROBABLE)
    case 4: { // RTS_NUM_UNITS: all cars
        if (!car) return 0;
        int count = 0;
        for (std::size_t i = 0; i < cars_.size(); ++i) {
            if (cars_[i]->train != car->train) continue;
            if (code == 2 && cars_[i]->is_loco()) continue;
            if (index < 0 && i >= car->index) continue;
            if (index > 0 && i <= car->index) continue;
            ++count;
        }
        return count;
    }
    case 3: // RTS_ISNIGHT
        return night_ ? 1 : 0;
    case 6: { // RTS_TRAINDIR: the train's direction relative to this locomotive
        if (!car) return 0;
        const bool flipped = (car->wagon()->flags & 1) != 0;
        return flipped != train_reversed_ ? -1 : 1;
    }
    case 7: // RTS_TAPEROLL: not recording
        return 0;
    default:
        return 0;
    }
}

int Host::set_integer(const abi::Locomotive* loco, std::uint32_t code, int value) {
    CarMirror* car = loco ? car_of(loco) : nullptr;
    if (code == 6 && car) { // set the train's direction (only a lone engine, 0x43939C)
        if (car->wagon()->flags & 1) value = -value;
        if (value && train_of(*car).count == 1) {
            set_direction_ = value;
            return 2;
        }
        return 1;
    }
    return 0; // 7: tape roll is not recorded
}

void* Host::loco_object(const Module* caller, const abi::Locomotive* loco, int code, int index, int sub) {
    (void)sub;
    CarMirror* car = loco ? car_of(loco) : nullptr;
    if (!car) return nullptr;
    const auto i = static_cast<std::uint32_t>(index);
    switch (code) {
    case abi::ot_loco:
        return view(*car, caller);
    case abi::ot_wagon:
        return car->wagon();
    case abi::ot_door:
        if (i >= car->doors.size() && GetEnvironmentVariableW(L"OPENRTS_DLLHOST_TRACE", nullptr, 0))
            log(L"Door(" + std::to_wstring(index) + L") of car " + std::to_wstring(car->index) + L": it has " + std::to_wstring(car->doors.size()));
        return i < car->doors.size() ? &car->doors[i] : nullptr;
    case abi::ot_cabin:
        return cab_view(*car, caller);
    case abi::ot_engine:
        return car->has_engine ? car->eng() : nullptr;
    case abi::ot_exhaust:
        return car->spec.kind == car_diesel && i < car->exhaust.size() ? &car->exhaust[i] : nullptr;
    case abi::ot_free_anim:
        return i < car->anims.size() ? &car->anims[i] : nullptr;
    case abi::ot_light:
        return i < car->lights.size() ? &car->lights[i] : nullptr;
    case abi::ot_sound:
        if (index == 0) return &car->sound_ext;
        if (index == 1 && car->has_engine) return &car->sound_cab;
        return nullptr;
    case abi::ot_eng_param:
        return i < car->params.size() ? &car->params[i] : nullptr;
    case abi::ot_slave_loco: {
        if (i >= car->slaves.size()) return nullptr;
        CarMirror* slave = car_of(car->slaves[i]);
        return slave ? view(*slave, caller) : car->slaves[i];
    }
    default:
        return nullptr;
    }
}

void* Host::loco_object_by_name(const Module* caller, const abi::Locomotive* loco, int code, const wchar_t* name, int sub) {
    (void)caller;
    (void)sub;
    CarMirror* car = loco ? car_of(loco) : nullptr;
    if (!car || !name) return nullptr;
    switch (code) {
    case abi::ot_door:
        for (auto& d : car->doors)
            if (same_name(d.name, name)) return &d;
        return nullptr;
    case abi::ot_eng_param:
        for (auto& p : car->params)
            if (same_name(p.name, name)) return &p;
        return nullptr;
    case abi::ot_free_anim:
        for (auto& a : car->anims)
            if (same_name(a.name, name)) return &a;
        return nullptr;
    case abi::ot_aux_lib:
        for (auto& a : car->aux)
            if (same_name(a.name, name)) return &a;
        return nullptr;
    default:
        return nullptr;
    }
}

void* Host::cab_object(const Module* caller, const abi::Cabin* cab, int code, int index, int sub) {
    CarMirror* car = cab ? car_of_cabin(cab) : nullptr;
    if (!car) return nullptr;
    const auto i = static_cast<std::uint32_t>(index);
    switch (code) {
    case abi::ot_scr:
    case abi::ot_scr_sub: {
        // RTS 0x536B14: the display at `index` when it is a screen; its
        // sub-object at position `sub`.
        for (auto& s : car->screens) {
            if (s.display != i) continue;
            if (code == abi::ot_scr) return &s.screen;
            const auto k = static_cast<std::uint32_t>(sub);
            return k < s.objects.size() ? &s.objects[k] : nullptr;
        }
        return nullptr;
    }
    case abi::ot_switch:
        return i < car->switches.size() ? &car->switches[i] : nullptr;
    case abi::ot_display:
        return i < car->displays.size() ? &car->displays[i] : nullptr;
    case abi::ot_door:
        return i < car->cab_doors.size() ? &car->cab_doors[i] : nullptr;
    case abi::ot_cabin:
        return cab_view(*car, caller);
    case abi::ot_free_anim:
        return i < car->cab_anims.size() ? &car->cab_anims[i] : nullptr;
    case abi::ot_int_light:
        return i < car->internal_lights.size() ? &car->internal_lights[i] : nullptr;
    case abi::ot_tex_change:
        return i < car->tex_changes.size() ? &car->tex_changes[i] : nullptr;
    default: // close-up scenes, screens, areas, windows, chairs, seats: none yet
        return nullptr;
    }
}

void* Host::cab_object_by_name(const Module* caller, const abi::Cabin* cab, int code, const wchar_t* name, int sub) {
    (void)caller;
    (void)sub;
    CarMirror* car = cab ? car_of_cabin(cab) : nullptr;
    if (!car || !name) return nullptr;
    switch (code) {
    case abi::ot_switch:
        for (auto& s : car->switches)
            if (same_name(s.name, name)) return &s;
        return nullptr;
    case abi::ot_display:
        for (auto& d : car->displays)
            if (same_name(d.name, name)) return &d;
        return nullptr;
    case abi::ot_door:
        for (auto& d : car->cab_doors)
            if (same_name(d.name, name)) return &d;
        return nullptr;
    case abi::ot_free_anim:
        for (auto& a : car->cab_anims)
            if (same_name(a.name, name)) return &a;
        return nullptr;
    default:
        return nullptr;
    }
}

void* Host::aux_object(const abi::AuxLibrary* aux, int code, int index, int sub) {
    (void)index;
    (void)sub;
    if (!aux || !car_of_aux(aux)) return nullptr;
    switch (code) {
    case abi::ot_sound:
        return aux->sound;
    case abi::ot_loco:
        return aux->loco;
    default:
        return nullptr;
    }
}

void* Host::aux_object_by_name(const abi::AuxLibrary* aux, int code, const wchar_t* name, int sub) {
    (void)sub;
    if (!aux || !name || !car_of_aux(aux)) return nullptr;
    if (code == abi::ot_eng_param) {
        for (std::uint32_t i = 0; i < aux->num_params; ++i)
            if (same_name(aux->params[i].name, name)) return &aux->params[i];
    }
    return nullptr;
}

void Host::trace_once(const std::wstring& key, const std::wstring& line) const {
    if (!GetEnvironmentVariableW(L"OPENRTS_DLLHOST_TRACE", nullptr, 0)) return;
    for (const auto& k : traced_)
        if (k == key) return;
    traced_.push_back(key);
    const_cast<Host*>(this)->log(line);
}

void* Host::sound_table(const std::int32_t index) const {
    return index >= 0 && static_cast<std::size_t>(index) < sound_tables_.size() ? sound_tables_[static_cast<std::size_t>(index)].get()
                                                                                 : nullptr;
}

abi::RTSSignalData* Host::unit(const std::int32_t id) const {
    if (id < 0) return nullptr;
    const auto it = units_.find(id);
    return it == units_.end() ? nullptr : it->second.get();
}

// RTS 0x482680: TrackItem (44 bytes) and its kind's fields after it.
void Host::add_item(const TrackItemSpec& s) {
    if (items_.contains(s.id)) return;
    static constexpr std::uint32_t sizes[] = {44, 128, 64, 76, 76, 52, 52, 56, 48};
    const std::uint32_t bytes = s.type < 9 ? sizes[s.type] : 44;
    auto block = std::make_unique<std::uint32_t[]>(bytes / 4);
    std::memset(block.get(), 0, bytes);
    auto* item = reinterpret_cast<abi::TrackItem*>(block.get());
    item->type = s.type;
    item->id = s.id;
    item->s_node_off = s.node_offset;
    item->s_data_flags = s.data_flags;
    item->p_data_x = s.pdata[0];
    item->p_data_z = s.pdata[1];
    item->p_data_tile_x = s.ptile[0];
    item->p_data_tile_z = s.ptile[1];
    item->r_data_x = s.rdata[0];
    item->r_data_y = s.rdata[1];
    item->r_data_z = s.rdata[2];
    item->r_tile_x = s.rtile[0];
    item->r_tile_z = s.rtile[1];
    auto* extra = reinterpret_cast<std::uint8_t*>(block.get()) + 44;
    const auto u32 = [&](std::size_t at, std::uint32_t v) { std::memcpy(extra + at, &v, 4); };
    const auto f32 = [&](std::size_t at, float v) { std::memcpy(extra + at, &v, 4); };
    const auto text = [&](std::size_t at, const std::u16string& t) {
        const wchar_t* p = t.empty() ? nullptr : keep(strings_, t);
        std::memcpy(extra + at, &p, sizeof(p));
    };
    switch (s.type) {
    case 2: // SpeedPostItem: Flags, Value, Off, KM, Number
        u32(0, s.flags);
        f32(4, s.value);
        f32(8, s.off);
        f32(12, s.km);
        u32(16, static_cast<std::uint32_t>(s.number));
        break;
    case 3: // PlatformItem: TrItemFlags, TrItemConj, PlatformName, StationName, MinWaitTime, Passengers, pl, at
        u32(0, s.flags);
        u32(4, s.link);
        text(8, s.name);
        text(12, s.station);
        u32(16, s.min_wait);
        u32(20, s.passengers);
        break;
    case 4: // SidingItem: flags, conj, SidingName
        u32(0, s.flags);
        u32(4, s.link);
        text(8, s.name);
        break;
    case 6: // CrossoverItem: Conj, SectionIDX
        u32(0, s.link);
        u32(4, s.section);
        break;
    case 7: // PickupItem: Amount, PickupFlags, at
        f32(0, s.value);
        u32(4, s.flags);
        break;
    default:
        break;
    }
    items_[s.id] = std::move(block);
}

// RTS 0x4805D8: the player's train's items (gathered from its rear,
// 0x4808DC) the DLL asks for: ahead (code 1) / behind (2) within
// `distance` (<= 0: 5000 m), under the train (4) always; signals left out
// (ALSN gives them). A locomotive facing the train's rear has ahead and
// behind swapped and Z negated, in its request and in what it gets.
int Host::track_items(const abi::Locomotive* loco, std::uint32_t code, float distance, abi::TrackItemsItem*& items,
                      std::uint32_t& count) {
    count = 0;
    const CarMirror* car = loco ? car_of(loco) : nullptr;
    if (!car) {
        items = nullptr;
        return 0;
    }
    if (distance <= 0.0f) distance = 5000.0f;
    if (car->train != 0) {
        // Only the player's train has its list here (RTS keeps one per train, +392).
        items = nullptr;
        return 0;
    }
    items_wanted_ = (std::max)(items_wanted_, distance);
    trace_once(L"items" + std::to_wstring(code), L"RTSGetTrackItems code " + std::to_wstring(code) + L" distance "
                                                     + std::to_wstring(static_cast<int>(distance)) + L", "
                                                     + std::to_wstring(window_.size()) + L" items in the list");
    const bool flipped = (const_cast<CarMirror*>(car)->wagon()->flags & 1) != 0;
    std::uint32_t want = code & 0xFFFFu;
    if (flipped) {
        if ((want & 1) && !(want & 2)) want = (want & ~3u) | 2u;
        else if ((want & 2) && !(want & 1)) want = (want & ~3u) | 1u;
    }
    track_items_.clear();
    for (const auto& e : window_) {
        if (!(want & e.pos)) continue;
        if (e.pos != item_under && e.distance > distance) continue;
        if (!e.obj || e.obj->type == 1) continue;
        abi::TrackItemsItem out = e;
        if (flipped) {
            if (out.pos == item_ahead) out.pos = item_behind;
            else if (out.pos == item_behind) out.pos = item_ahead;
            if (out.pos != item_under) out.z = -out.z;
        }
        track_items_.push_back(out);
    }
    if (track_items_.empty()) return 0;
    items = track_items_.data();
    count = static_cast<std::uint32_t>(track_items_.size());
    return static_cast<int>(count);
}

int Host::named_integer(const void* table, const wchar_t* name) const {
    if (!name) return 0;
    int found = 0;
    for (const auto& t : sound_tables_) {
        if (!table || t.get() != table) continue;
        for (std::size_t i = 0; i < t->names.size() && i < t->codes.size(); ++i) {
            if (lstrcmpiW(t->names[i].c_str(), name) == 0) {
                found = t->codes[i];
                break;
            }
        }
        break;
    }
    trace_once(L"named", L"RTSGetNamedInteger \"" + std::wstring(name) + L"\" = " + std::to_wstring(found)
                             + (table ? L"" : L" (no script)"));
    return found;
}

// 0x439548: -3 without a script or pairs, -2 before any pair code (no
// PairedFlags), the pair's state for one of its On codes (0x4A8A58: -1 while
// it is not known), -4 for any other code.
int Host::trigger_on(const abi::SMSObject* sound, const std::uint16_t code) const {
    trace_once(L"trigger_on", L"RTSIsSMSTriggerOn " + std::to_wstring(code));
    if (!sound || !sound->sms) return -3;
    const SoundTable* table = nullptr;
    for (const auto& t : sound_tables_)
        if (t.get() == sound->sms) table = t.get();
    if (!table || table->pair_on.empty()) return -3;
    if (!sound->paired_flags) return -2;
    const auto* flags = static_cast<const std::uint32_t*>(sound->paired_flags);
    for (std::size_t i = 0; i < table->pair_on.size(); ++i) {
        for (const auto c : table->pair_on[i]) {
            if (c != code) continue;
            const std::size_t word = i >> 5;
            const std::uint32_t bit = 1u << (i & 31);
            if (word > 3) return -1;
            if (!(flags[4 + word] & bit)) return -1;
            return (flags[word] & bit) != 0 ? 1 : 0;
        }
    }
    return -4;
}

} // namespace openrts::dllhost
