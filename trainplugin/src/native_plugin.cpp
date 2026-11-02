#include <openrts/trainplugin/native_plugin.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <utility>

namespace openrts::trainplugin {

namespace {

std::string lower(std::string_view s) {
    std::string r(s);
    for (auto& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

void* open_library(const std::filesystem::path& path) {
#ifdef _WIN32
    return LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
#else
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void* symbol(void* module, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(module), name));
#else
    return dlsym(module, name);
#endif
}

void close_library(void* module) {
#ifdef _WIN32
    if (module) FreeLibrary(static_cast<HMODULE>(module));
#else
    if (module) dlclose(module);
#endif
}

} // namespace

// --- CabModel ----------------------------------------------------------------

CabModel::CabModel(const formats::CabDescription* description) {
    if (!description) return;
    for (const auto& s : description->switches) {
        Control c;
        c.name = s.name;
        c.key = (s.flags & formats::cab_key) != 0;
        c.states = s.num_states + (c.key ? 1u : 0u);
        c.anim_time = s.anim_time;
        c.state = c.target = c.frame = static_cast<float>(s.initial_state);
        if (s.id >= controls_.size()) controls_.resize(s.id + 1);
        controls_[s.id] = std::move(c);
    }
    for (const auto& d : description->displays) {
        if (d.id >= indicators_.size()) indicators_.resize(d.id + 1);
        indicators_[d.id] = {d.name, 0.0f, d.initial_state};
    }
}

OrtsHandle CabModel::find_control(std::string_view name) const {
    const auto wanted = lower(name);
    for (std::size_t i = 0; i < controls_.size(); ++i)
        if (lower(controls_[i].name) == wanted) return static_cast<OrtsHandle>(i);
    return -1;
}

OrtsHandle CabModel::find_indicator(std::string_view name) const {
    const auto wanted = lower(name);
    for (std::size_t i = 0; i < indicators_.size(); ++i)
        if (lower(indicators_[i].name) == wanted) return static_cast<OrtsHandle>(i);
    return -1;
}

std::vector<std::pair<OrtsHandle, float>> CabModel::step(const float dt) {
    std::vector<std::pair<OrtsHandle, float>> arrived;
    for (std::size_t i = 0; i < controls_.size(); ++i) {
        auto& c = controls_[i];
        if (c.state == c.target) {
            c.frame = c.target;
            continue;
        }
        const float rate = c.anim_time > 0.0f ? dt / c.anim_time : 1e9f;
        if (std::abs(c.target - c.frame) <= rate) {
            const float previous = c.state;
            c.frame = c.state = c.target;
            arrived.emplace_back(static_cast<OrtsHandle>(i), previous);
        } else {
            c.frame += c.target > c.frame ? rate : -rate;
        }
    }
    return arrived;
}

// --- the host functions a plugin calls ---------------------------------------

struct NativeHostCalls {
    static NativeLocomotive& self(void* ctx) { return *static_cast<NativeLocomotive*>(ctx); }
    static OrtsHandle find_control(void* ctx, const char* name) { return name ? self(ctx).cab_.find_control(name) : -1; }
    static float control(void* ctx, OrtsHandle h) {
        auto& c = self(ctx).cab_.controls();
        return h >= 0 && static_cast<std::size_t>(h) < c.size() ? c[static_cast<std::size_t>(h)].state : 0.0f;
    }
    static void set_control(void* ctx, OrtsHandle h, float state) {
        auto& c = self(ctx).cab_.controls();
        if (h < 0 || static_cast<std::size_t>(h) >= c.size()) return;
        auto& x = c[static_cast<std::size_t>(h)];
        x.state = x.target = x.frame = state; // the plugin's own move: no request, no change event
    }
    static OrtsHandle find_indicator(void* ctx, const char* name) { return name ? self(ctx).cab_.find_indicator(name) : -1; }
    static void set_indicator(void* ctx, OrtsHandle h, float value, uint32_t state) {
        auto& d = self(ctx).cab_.indicators();
        if (h < 0 || static_cast<std::size_t>(h) >= d.size()) return;
        d[static_cast<std::size_t>(h)].value = value;
        d[static_cast<std::size_t>(h)].state = state;
    }
    static void sound_trigger(void* ctx, uint32_t cab, uint32_t trigger) {
        (cab ? self(ctx).sounds_cab_ : self(ctx).sounds_ext_).push_back(trigger);
    }
    static float parameter(void* ctx, const char* name, float fallback) {
        const auto* engine = self(ctx).engine_;
        if (!name || !engine || !engine->engine) return fallback;
        const auto wanted = lower(name);
        for (const auto& p : engine->engine->parameters)
            if (p.name == wanted) return p.value;
        return fallback;
    }
    static void message(void* ctx, uint32_t kind, const char* utf8) {
        (void)kind;
        if (utf8) self(ctx).messages_.emplace_back(utf8);
    }
    static void log(void* ctx, uint32_t level, const char* utf8) {
        if (utf8) self(ctx).messages_.push_back(std::string(level >= 2 ? "error: " : "") + utf8);
    }
};

// --- NativeLocomotive ----------------------------------------------------------

std::unique_ptr<NativeLocomotive> NativeLocomotive::load(const std::filesystem::path& dll,
                                                         const formats::WagonDefinition& engine,
                                                         const formats::CabDescription* cab, std::string* error) {
    auto fail = [&](const std::string& why) -> std::unique_ptr<NativeLocomotive> {
        if (error) *error = why;
        return nullptr;
    };
    std::unique_ptr<NativeLocomotive> loco(new NativeLocomotive(cab));
    loco->module_ = open_library(dll);
    if (!loco->module_) return fail("cannot load " + dll.string());
    auto* entry = reinterpret_cast<OrtsTrainPluginEntry>(symbol(loco->module_, "orts_train_plugin"));
    if (!entry) return fail(dll.string() + ": no orts_train_plugin export");
    loco->plugin_ = entry(ORTS_TRAIN_API_VERSION);
    const auto* p = loco->plugin_;
    if (!p || p->size < sizeof(OrtsTrainPlugin) || !p->create || !p->step)
        return fail(dll.string() + ": the plugin table is incomplete or newer than this openRTS");
    loco->name_ = p->name ? p->name : dll.filename().string();
    loco->engine_ = &engine;
    auto& h = loco->host_;
    h.size = sizeof(OrtsHost);
    h.ctx = loco.get();
    h.find_control = &NativeHostCalls::find_control;
    h.control = &NativeHostCalls::control;
    h.set_control = &NativeHostCalls::set_control;
    h.find_indicator = &NativeHostCalls::find_indicator;
    h.set_indicator = &NativeHostCalls::set_indicator;
    h.sound_trigger = &NativeHostCalls::sound_trigger;
    h.parameter = &NativeHostCalls::parameter;
    h.message = &NativeHostCalls::message;
    h.log = &NativeHostCalls::log;
    OrtsTrainSetup setup{};
    setup.size = sizeof(setup);
    setup.engine_name = engine.name.c_str();
    setup.engine_kind = engine.engine && engine.engine->kind == formats::EngineKind::diesel ? 2u : 1u;
    setup.num_engines = engine.engine ? engine.engine->num_engines : 0;
    setup.num_axles = engine.engine ? engine.engine->num_axles : 0;
    setup.mass = engine.mass;
    loco->context_ = p->create(&h, &setup);
    if (!loco->context_) return fail(loco->name_ + ": create failed");
    loco->outputs_.size = sizeof(OrtsTrainOutputs);
    return loco;
}

NativeLocomotive::~NativeLocomotive() {
    if (plugin_ && context_ && plugin_->destroy) plugin_->destroy(context_);
    close_library(module_);
}

bool NativeLocomotive::operate(OrtsHandle handle, float state) {
    auto& c = cab_.controls();
    if (handle < 0 || static_cast<std::size_t>(handle) >= c.size()) return false;
    auto& x = c[static_cast<std::size_t>(handle)];
    if (plugin_->control_request && !plugin_->control_request(context_, handle, x.target, state)) return false;
    x.target = state;
    return true;
}

void NativeLocomotive::step(double dt, const simulation::EngineInputs& inputs, simulation::EngineDemands& demands) {
    time_ += dt;
    OrtsTrainInputs in{};
    in.size = sizeof(in);
    in.time = time_;
    in.speed = inputs.speed;
    in.brake_pipe = inputs.train_pipe;
    in.brake_cylinder = inputs.cylinder;
    in.main_reservoir = inputs.main_reservoir;
    in.independent = inputs.independent;
    // Without a modelled line (negative) the engine has the commonest DC one.
    in.line_voltage = inputs.line_voltage >= 0.0f ? inputs.line_voltage : 3000.0f;
    in.line_frequency = inputs.line_voltage >= 0.0f ? inputs.line_frequency : 0.0f;
    in.flags = (player_ ? ORTS_INPUT_PLAYER : 0u) | (inputs.flipped ? ORTS_INPUT_FLIPPED : 0u) | ORTS_INPUT_MASTER;
    OrtsTrainOutputs out{};
    out.size = sizeof(out);
    out.independent_brake = -1.0f;
    out.equalising = -1.0f;
    out.ep_value = -1.0f;
    out.brake_systems = 1;
    plugin_->step(context_, dt, &in, &out);
    outputs_ = out;
    // As RTS: after Run, the cab update moves the switches and reports the
    // ones that arrived (0x5031D8 step 11, 0x526CB0).
    for (const auto& [handle, previous] : cab_.step(static_cast<float>(dt))) {
        if (plugin_->control_changed) plugin_->control_changed(context_, handle, previous);
    }
    demands = {};
    demands.force = out.traction_force;
    demands.brake_force = out.brake_force;
    demands.train_pipe_rate = out.brake_pipe_rate;
    demands.main_reservoir_rate = out.main_reservoir_rate;
    demands.independent_brake = out.independent_brake;
    demands.ep_value = out.ep_value;
    demands.systems = static_cast<std::uint16_t>(out.brake_systems);
    demands.equalising = out.equalising;
    demands.emergency = out.emergency != 0;
}

std::vector<std::uint32_t> NativeLocomotive::take_sounds(bool cab) { return std::exchange(cab ? sounds_cab_ : sounds_ext_, {}); }

std::vector<std::string> NativeLocomotive::take_messages() { return std::exchange(messages_, {}); }

} // namespace openrts::trainplugin
