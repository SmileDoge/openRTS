#pragma once

// A small C++ layer over train_plugin.h for plugin authors: derive from
// orts::Locomotive, then ORTS_TRAIN_PLUGIN(MyLoco, "name") exports the
// entry point. Header-only, no openRTS libraries needed.
//
//   class MyLoco : public orts::Locomotive {
//   public:
//       MyLoco(orts::Host host, const OrtsTrainSetup& setup) : orts::Locomotive(host) {
//           throttle_ = this->host().control("C_KM");
//       }
//       void step(double dt, const OrtsTrainInputs& in, OrtsTrainOutputs& out) override { ... }
//   private:
//       OrtsHandle throttle_;
//   };
//   ORTS_TRAIN_PLUGIN(MyLoco, "my loco")

#include <cstdint>
#include <memory>
#include <string>

#include <openrts/plugin/train_plugin.h>

namespace orts {

class Host {
public:
    explicit Host(const OrtsHost* host = nullptr) : host_(host) {}
    [[nodiscard]] OrtsHandle control(const char* name) const { return has(&OrtsHost::find_control) ? host_->find_control(host_->ctx, name) : -1; }
    [[nodiscard]] float state(OrtsHandle control) const { return has(&OrtsHost::control) ? host_->control(host_->ctx, control) : 0.0f; }
    void set(OrtsHandle control, float state) const { if (has(&OrtsHost::set_control)) host_->set_control(host_->ctx, control, state); }
    [[nodiscard]] OrtsHandle indicator(const char* name) const { return has(&OrtsHost::find_indicator) ? host_->find_indicator(host_->ctx, name) : -1; }
    void show(OrtsHandle indicator, float value, std::uint32_t state = 0) const {
        if (has(&OrtsHost::set_indicator)) host_->set_indicator(host_->ctx, indicator, value, state);
    }
    void sound(std::uint32_t trigger, bool cab = true) const { if (has(&OrtsHost::sound_trigger)) host_->sound_trigger(host_->ctx, cab ? 1u : 0u, trigger); }
    [[nodiscard]] float parameter(const char* name, float fallback) const {
        return has(&OrtsHost::parameter) ? host_->parameter(host_->ctx, name, fallback) : fallback;
    }
    void message(std::uint32_t kind, const std::string& utf8) const { if (has(&OrtsHost::message)) host_->message(host_->ctx, kind, utf8.c_str()); }
    void log(std::uint32_t level, const std::string& utf8) const { if (has(&OrtsHost::log)) host_->log(host_->ctx, level, utf8.c_str()); }

private:
    // A member is usable when the host's struct is large enough to have it.
    template <class M>
    [[nodiscard]] bool has(M OrtsHost::*member) const {
        if (!host_) return false;
        static const OrtsHost probe{};
        const auto offset = static_cast<std::size_t>(reinterpret_cast<const char*>(&(probe.*member)) -
                                                     reinterpret_cast<const char*>(&probe));
        return offset + sizeof(M) <= host_->size && host_->*member != nullptr;
    }
    const OrtsHost* host_;
};

class Locomotive {
public:
    explicit Locomotive(Host host) : host_(host) {}
    virtual ~Locomotive() = default;
    virtual void step(double dt, const OrtsTrainInputs& inputs, OrtsTrainOutputs& outputs) = 0;
    // CanSwitch: false refuses the move.
    virtual bool request(OrtsHandle control, float from, float to) {
        (void)control;
        (void)from;
        (void)to;
        return true;
    }
    // Switched.
    virtual void changed(OrtsHandle control, float previous) {
        (void)control;
        (void)previous;
    }
    [[nodiscard]] const Host& host() const noexcept { return host_; }

private:
    Host host_;
};

template <class T>
const OrtsTrainPlugin* plugin_table(const char* name) {
    static const OrtsTrainPlugin table{
        sizeof(OrtsTrainPlugin),
        ORTS_TRAIN_API_VERSION,
        name,
        [](const OrtsHost* host, const OrtsTrainSetup* setup) -> void* {
            try {
                return new T(Host(host), *setup);
            } catch (...) {
                return nullptr;
            }
        },
        [](void* plugin) { delete static_cast<T*>(plugin); },
        [](void* plugin, double dt, const OrtsTrainInputs* inputs, OrtsTrainOutputs* outputs) {
            static_cast<T*>(plugin)->step(dt, *inputs, *outputs);
        },
        [](void* plugin, OrtsHandle control, float from, float to) -> std::int32_t {
            return static_cast<T*>(plugin)->request(control, from, to) ? 1 : 0;
        },
        [](void* plugin, OrtsHandle control, float previous) { static_cast<T*>(plugin)->changed(control, previous); },
    };
    return &table;
}

} // namespace orts

#define ORTS_TRAIN_PLUGIN(Class, Name)                                                                    \
    extern "C" ORTS_PLUGIN_EXPORT const OrtsTrainPlugin* orts_train_plugin(std::uint32_t host_api_version) { \
        return host_api_version >= 1 ? ::orts::plugin_table<Class>(Name) : nullptr;                          \
    }
