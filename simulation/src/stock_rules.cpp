#include <openrts/simulation/stock_rules.hpp>

#include <openrts/script/luau.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include <lua.h>

namespace openrts::simulation {

struct StockRules::Impl {
    struct Script {
        std::string name;
        std::unique_ptr<script::Vm> vm;
        bool broken = false;
    };
    std::vector<Script> scripts;
    std::string messages;
    std::mutex lock; // load_wagon may run on loading threads
};

StockRules::StockRules() : impl_(std::make_unique<Impl>()) {}
StockRules::~StockRules() = default;

std::shared_ptr<StockRules> StockRules::load(const core::Vfs& vfs, std::string& messages) {
    std::shared_ptr<StockRules> rules(new StockRules());
    rules->self_ = rules;
    const auto folder = core::VirtualPath::parse("TRAINS/OPENRTS/STOCK");
    if (!folder || !vfs.is_directory(*folder)) return rules;
    auto entries = vfs.list(*folder);
    std::sort(entries.begin(), entries.end(), [](const core::VfsEntry& a, const core::VfsEntry& b) { return a.path.key() < b.path.key(); });
    for (const auto& entry : entries) {
        const auto name = std::string(entry.path.filename());
        if (entry.directory || !core::fold_case(name).ends_with(".luau")) continue;
        std::string error;
        const auto source = vfs.read(entry.path, &error);
        if (!source) {
            messages += "stock rules: " + name + ": " + error + "\n";
            continue;
        }
        const auto chunk = script::compile(*source, name, error);
        if (!chunk) {
            messages += "stock rules: " + error + "\n";
            continue;
        }
        auto vm = std::make_unique<script::Vm>();
        if (!vm->run(*chunk, error)) {
            messages += "stock rules: " + error + "\n";
            continue;
        }
        lua_State* t = vm->thread();
        lua_getglobal(t, "car");
        const bool has_car = lua_isfunction(t, -1);
        lua_pop(t, 1);
        if (!has_car) {
            messages += "stock rules: " + name + ": no function car(c)\n";
            continue;
        }
        rules->impl_->scripts.push_back({name, std::move(vm), false});
    }
    return rules;
}

std::size_t StockRules::scripts() const noexcept { return impl_->scripts.size(); }

std::string StockRules::take_messages() {
    const std::scoped_lock guard(impl_->lock);
    return std::exchange(impl_->messages, {});
}

namespace {

const char* type_name(const formats::WagonKind kind) {
    switch (kind) {
    case formats::WagonKind::engine: return "engine";
    case formats::WagonKind::passenger: return "passenger";
    case formats::WagonKind::freight: return "freight";
    case formats::WagonKind::tender: return "tender";
    default: return "other";
    }
}

const char* engine_type_name(const formats::EngineKind kind) {
    switch (kind) {
    case formats::EngineKind::electric: return "electric";
    case formats::EngineKind::diesel: return "diesel";
    case formats::EngineKind::steam: return "steam";
    default: return nullptr;
    }
}

} // namespace

formats::WagonOverrides StockRules::apply(const formats::WagonDefinition& car, const std::string_view folder, const std::string_view file,
                                          const bool engine) {
    formats::WagonOverrides out;
    const std::scoped_lock guard(impl_->lock);
    if (impl_->scripts.empty()) return out;
    // What the car says so far, and what each script makes of it.
    std::string distributor = formats::brake_distributor_name(car.brakes.distributor);
    float on = car.engine ? car.engine->compressor_restart : 0.0f;
    float off = car.engine ? car.engine->main_reservoir_max : 0.0f;
    float capacity = car.engine ? car.engine->compressor_capacity : 0.0f;
    const bool ep = (car.brakes.equipment & 4) || (car.brakes.system & 2);
    for (auto& s : impl_->scripts) {
        if (s.broken) continue;
        lua_State* t = s.vm->thread();
        lua_getglobal(t, "car");
        lua_createtable(t, 0, 12);
        const auto text = [t](const char* key, const std::string_view value) {
            lua_pushlstring(t, value.data(), value.size());
            lua_setfield(t, -2, key);
        };
        const auto number = [t](const char* key, const double value) {
            lua_pushnumber(t, value);
            lua_setfield(t, -2, key);
        };
        const auto flag = [t](const char* key, const bool value) {
            lua_pushboolean(t, value ? 1 : 0);
            lua_setfield(t, -2, key);
        };
        text("folder", folder);
        text("file", file);
        text("name", car.display_name.empty() ? car.name : car.display_name);
        flag("engine", engine);
        text("type", type_name(car.kind));
        if (car.engine) {
            if (const char* kind = engine_type_name(car.engine->kind)) text("engine_type", kind);
        }
        flag("ep", ep);
        number("mass", car.mass / 1000.0);
        if (!distributor.empty()) text("distributor", distributor);
        if (on > 0.0f) number("compressor_on", on);
        if (off > 0.0f) number("compressor_off", off);
        if (capacity > 0.0f) number("compressor_capacity", capacity);
        lua_pushvalue(t, -1);
        const int table = lua_ref(t, -1);
        lua_pop(t, 1);
        std::string error;
        if (!s.vm->call(1, 0, error)) {
            lua_unref(t, table);
            impl_->messages += "stock rules: " + error + " (the script is left out)\n";
            s.broken = true;
            continue;
        }
        // Back: what it set.
        lua_getref(t, table);
        lua_unref(t, table);
        lua_getfield(t, -1, "distributor");
        if (lua_isstring(t, -1)) {
            const std::string value = lua_tostring(t, -1);
            if (formats::brake_distributor_from(value)) {
                distributor = formats::brake_distributor_name(*formats::brake_distributor_from(value));
                out.distributor = formats::brake_distributor_from(value);
            } else if (impl_->messages.find(value) == std::string::npos) {
                impl_->messages += "stock rules: " + s.name + ": distributor \"" + value + "\" is not 483, 292, 305-000 or 305-001\n";
            }
        }
        lua_pop(t, 1);
        const auto read = [&](const char* key, float& value, std::optional<float>& given) {
            lua_getfield(t, -1, key);
            if (lua_isnumber(t, -1)) {
                const double v = lua_tonumber(t, -1);
                if (std::isfinite(v) && v > 0.0 && v < 20.0 && static_cast<float>(v) != value) {
                    value = static_cast<float>(v);
                    given = value;
                }
            }
            lua_pop(t, 1);
        };
        read("compressor_on", on, out.compressor_on);
        read("compressor_off", off, out.compressor_off);
        read("compressor_capacity", capacity, out.compressor_capacity);
        lua_pop(t, 1);
    }
    return out;
}

formats::WagonRules StockRules::of(const core::Vfs& vfs) {
    static std::mutex lock;
    static std::map<const core::Vfs*, std::shared_ptr<StockRules>> cache;
    const std::scoped_lock guard(lock);
    auto& rules = cache[&vfs];
    if (!rules) {
        std::string messages;
        rules = load(vfs, messages);
        if (!messages.empty()) std::fprintf(stderr, "%s", messages.c_str());
    }
    return rules->hook();
}

formats::WagonRules StockRules::hook() {
    auto self = self_.lock();
    if (!self || self->scripts() == 0) return {};
    return [self](const formats::WagonDefinition& car, const std::string_view folder, const std::string_view file, const bool engine) {
        return self->apply(car, folder, file, engine);
    };
}

} // namespace openrts::simulation
