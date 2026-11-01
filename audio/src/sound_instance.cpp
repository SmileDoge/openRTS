#include <openrts/audio/sound_instance.hpp>

#include <algorithm>
#include <limits>
#include <optional>

namespace openrts::audio {

using formats::SoundActionKind;
using formats::SoundTriggerKind;

float evaluate_curve(const formats::SoundCurve& curve, const float x) {
    const auto& points = curve.points;
    if (points.empty()) return 1.0f;
    if (x <= points.front().first) return points.front().second;
    for (std::size_t i = 1; i < points.size(); ++i) {
        const auto& [x1, y1] = points[i];
        if (x <= x1) {
            const auto& [x0, y0] = points[i - 1];
            if (x1 <= x0) return y1;
            return y0 + (y1 - y0) * (x - x0) / (x1 - x0);
        }
    }
    return points.back().second;
}

struct SoundInstance::Stream {
    struct Voice {
        VoiceId id{};
        float volume = 1.0f; // Volume_Min_Max pick
        std::shared_ptr<const AudioClip> clip;
        float fade_in = 0.0f;   // seconds to full volume after start
        float fade_out = 0.0f;  // seconds from full to silent after release
        float age = 0.0f;
        float released = -1.0f; // age when released with a fade, else < 0

        // Fade factor at the current age; 0 once a fade-out has ended.
        [[nodiscard]] float fade() const {
            float factor = fade_in > 0.0f ? (std::min)(age / fade_in, 1.0f) : 1.0f;
            if (released >= 0.0f) factor *= (std::max)(1.0f - (age - released) / fade_out, 0.0f);
            return factor;
        }
    };

    const formats::SoundStream* definition = nullptr;
    bool running = true; // EnableStream / DisableStream / SuspendStream
    std::vector<bool> enabled;
    std::vector<float> countdown;      // random triggers: seconds
    std::vector<std::size_t> sequence; // next file of sequential selections, per trigger
    Voice loop;
    std::vector<Voice> one_shots;
    float volume = 1.0f; // SetStreamVolume
};

SoundInstance::SoundInstance(std::shared_ptr<const formats::SoundScript> script, const formats::SoundGroup& group,
                             const formats::SoundOwner owner, ClipLookup clips, const std::uint32_t seed)
    : script_(std::move(script)), group_(&group), clips_(std::move(clips)), random_(seed), owner_(owner) {
    for (const auto& definition : group.streams) {
        Stream stream;
        stream.definition = &definition;
        stream.enabled.assign(definition.triggers.size(), true);
        stream.countdown.assign(definition.triggers.size(), 0.0f);
        stream.sequence.assign(definition.triggers.size(), 0);
        streams_.push_back(std::move(stream));
    }
    pair_on_.assign(group.trigger_pairs.size(), false);
}

SoundInstance::SoundInstance(SoundInstance&&) noexcept = default;
SoundInstance& SoundInstance::operator=(SoundInstance&&) noexcept = default;
SoundInstance::~SoundInstance() = default;

std::size_t SoundInstance::voices() const noexcept {
    std::size_t count = 0;
    for (const auto& stream : streams_) count += stream.one_shots.size() + (stream.loop.id ? 1 : 0);
    return count;
}

bool SoundInstance::camera_listed(const formats::SoundActivation& activation, const SoundCamera camera) const {
    switch (camera) {
    case SoundCamera::cab:
        return activation.cab_camera;
    case SoundCamera::passenger:
        return activation.passenger_camera;
    case SoundCamera::external:
        return activation.external_camera;
    }
    return false;
}

void SoundInstance::stop(VoiceSink& sink) {
    for (auto& stream : streams_) {
        if (stream.loop.id) sink.stop(stream.loop.id);
        for (const auto& voice : stream.one_shots) sink.stop(voice.id);
        stream.loop = {};
        stream.one_shots.clear();
    }
    active_ = false;
    has_previous_ = false;
}

void SoundInstance::activate(const SoundInputs& inputs, VoiceSink& sink) {
    active_ = true;
    for (auto& stream : streams_) {
        std::fill(stream.enabled.begin(), stream.enabled.end(), true);
        stream.running = true;
        stream.volume = 1.0f;
        const auto& triggers = stream.definition->triggers;
        for (std::size_t i = 0; i < triggers.size(); ++i) {
            std::uniform_real_distribution<float> pick(triggers[i].minimum, (std::max)(triggers[i].minimum, triggers[i].maximum));
            stream.countdown[i] = pick(random_);
        }
    }
    for (auto& stream : streams_) {
        const auto& triggers = stream.definition->triggers;
        for (std::size_t i = 0; i < triggers.size(); ++i) {
            if (triggers[i].kind == SoundTriggerKind::initial) fire(stream, i, 1.0f, inputs, sink);
        }
    }
}

void SoundInstance::preload() {
    for (const auto& stream : streams_) {
        for (const auto& trigger : stream.definition->triggers) {
            for (const auto& action : trigger.actions) {
                for (const auto& file : action.files) (void)clips_(file.name);
            }
        }
    }
}

void SoundInstance::fire(Stream& stream, const std::size_t index, const float volume, const SoundInputs& inputs,
                         VoiceSink& sink) {
    const auto& trigger = stream.definition->triggers[index];
    // Weather and environment sounds have no position; rolling stock fades
    // with distance.
    const auto space = owner_ == formats::SoundOwner::environment ? VoiceSpace::flat
        : owner_ == formats::SoundOwner::train || owner_ == formats::SoundOwner::player_train ? VoiceSpace::positional
                                                                                             : VoiceSpace::panned;
    const auto start = [&](const formats::SoundAction& action, const VoicePlayback playback) -> Stream::Voice {
        if (action.files.empty()) return {};
        std::size_t choice = 0;
        if (action.selection == formats::SoundSelection::sequential) {
            choice = stream.sequence[index]++ % action.files.size();
        } else {
            choice = std::uniform_int_distribution<std::size_t>(0, action.files.size() - 1)(random_);
        }
        auto clip = clips_(action.files[choice].name);
        if (!clip) return {};
        const auto id = sink.play(clip, playback, space);
        if (!id) return {};
        if (playback == VoicePlayback::loop && random_loop_phase_ && clip->frames > 1) {
            sink.seek(id, std::uniform_int_distribution<std::uint64_t>(0, clip->frames - 1)(random_));
        }
        sink.set(id, 0.0f, 1.0f, inputs.position); // real values follow in this update
        return {.id = id, .volume = volume, .clip = std::move(clip), .fade_in = action.fade};
    };

    for (const auto& action : trigger.actions) {
        switch (action.kind) {
        case SoundActionKind::play_one_shot:
            if (auto voice = start(action, VoicePlayback::once); voice.id) stream.one_shots.push_back(std::move(voice));
            break;
        case SoundActionKind::start_loop:
        case SoundActionKind::start_loop_release:
            if (stream.loop.id) sink.stop(stream.loop.id);
            stream.loop = start(action, action.kind == SoundActionKind::start_loop ? VoicePlayback::loop
                                                                                    : VoicePlayback::loop_region);
            break;
        case SoundActionKind::release_loop_release:
        case SoundActionKind::release_loop_release_with_jump:
            if (stream.loop.id) {
                if (action.fade > 0.0f) {
                    // Fade out, then stop (the voice keeps looping meanwhile).
                    stream.loop.fade_out = action.fade;
                    stream.loop.released = stream.loop.age;
                } else {
                    sink.release(stream.loop.id);
                }
                stream.one_shots.push_back(std::move(stream.loop));
                stream.loop = {};
            }
            break;
        case SoundActionKind::set_stream_volume:
            stream.volume = action.value;
            break;
        case SoundActionKind::enable_trigger:
        case SoundActionKind::disable_trigger:
            if (action.index >= 1 && action.index <= stream.enabled.size()) {
                const auto target = action.index - 1;
                const bool enable = action.kind == SoundActionKind::enable_trigger;
                const auto& targeted = stream.definition->triggers[target];
                if (enable && !stream.enabled[target]) {
                    std::uniform_real_distribution<float> pick(targeted.minimum, (std::max)(targeted.minimum, targeted.maximum));
                    stream.countdown[target] = pick(random_);
                }
                stream.enabled[target] = enable;
            }
            break;
        case SoundActionKind::halt_playback:
            if (stream.loop.id) sink.stop(stream.loop.id);
            for (const auto& voice : stream.one_shots) sink.stop(voice.id);
            stream.loop = {};
            stream.one_shots.clear();
            break;
        case SoundActionKind::enable_stream:
        case SoundActionKind::disable_stream:
        case SoundActionKind::suspend_stream:
            if (action.index >= 1 && action.index <= streams_.size()) {
                auto& target = streams_[action.index - 1];
                target.running = action.kind == SoundActionKind::enable_stream;
                if (action.kind == SoundActionKind::disable_stream) {
                    if (target.loop.id) sink.stop(target.loop.id);
                    for (const auto& voice : target.one_shots) sink.stop(voice.id);
                    target.loop = {};
                    target.one_shots.clear();
                }
            }
            break;
        case SoundActionKind::post_trigger:
            posted_.push_back(action.index);
            break;
        case SoundActionKind::set_variable:
        case SoundActionKind::increase_variable:
            if (action.variable != formats::SoundVariable::unknown) {
                auto& value = overrides_[static_cast<std::size_t>(action.variable)];
                const auto base = value ? *value : inputs.variables[static_cast<std::size_t>(action.variable)];
                value = action.kind == SoundActionKind::set_variable ? action.value : base + action.value;
            }
            break;
        case SoundActionKind::none:
            break;
        }
    }
}

void SoundInstance::update(const float dt, const SoundInputs& raw_inputs, VoiceSink& sink) {
    using formats::SoundVariable;
    auto inputs = raw_inputs;
    for (std::size_t i = 0; i < overrides_.size(); ++i) {
        if (overrides_[i]) inputs.variables[i] = *overrides_[i];
    }
    const auto distance = inputs.variables[static_cast<std::size_t>(SoundVariable::distance)];
    // RTS defaults (0x4DD0EC): activation 500 m; deactivation 550 m, or the
    // activation distance + 50 when Deactivation has no Distance. Environment
    // sounds have no distances.
    const auto& activation = group_->activation;
    const auto& deactivation = group_->deactivation;
    const bool distances = owner_ != formats::SoundOwner::environment;
    const float activate_within = activation.distance.value_or(500.0f);
    const float deactivate_beyond =
        deactivation.present ? deactivation.distance.value_or(activate_within + 50.0f) : 550.0f;
    // A camera listed in both blocks keeps the sound active (RTS clears its
    // deactivation flag).
    const bool camera_deactivates =
        camera_listed(deactivation, inputs.camera) && !camera_listed(activation, inputs.camera);
    if (!active_) {
        const bool any_camera = !activation.cab_camera && !activation.external_camera && !activation.passenger_camera;
        const bool camera = any_camera || camera_listed(activation, inputs.camera);
        // Deactivation wins: with a deactivation distance below the
        // activation one (mdd_ed4m-engMotor.sms: 1000 / 300 m) a group
        // between the two would otherwise start and stop on alternate
        // updates, restarting its loops. RTS's runtime test is not traced;
        // its sounds do not stutter there (PROBABLE).
        if (!camera || (distances && (distance > activate_within || distance > deactivate_beyond))) return;
        activate(inputs, sink);
    } else if (camera_deactivates || (distances && distance > deactivate_beyond)) {
        stop(sink);
        return;
    }

    // Discrete codes: RTS trigger pairs pass only the first "on" until an
    // "off" arrives, and the other way round.
    std::vector<std::uint32_t> incoming(inputs.discrete.begin(), inputs.discrete.end());
    incoming.insert(incoming.end(), posted_.begin(), posted_.end());
    posted_.clear();
    std::vector<std::uint32_t> codes;
    for (const auto code : incoming) {
        bool pass = true;
        for (std::size_t p = 0; p < group_->trigger_pairs.size(); ++p) {
            const auto& pair = group_->trigger_pairs[p];
            if (std::find(pair.on.begin(), pair.on.end(), code) != pair.on.end()) {
                pass = !pair_on_[p];
                pair_on_[p] = true;
            } else if (std::find(pair.off.begin(), pair.off.end(), code) != pair.off.end()) {
                pass = pair_on_[p];
                pair_on_[p] = false;
            }
        }
        if (pass) codes.push_back(code);
    }

    for (auto& stream : streams_) {
        if (!stream.running) continue;
        const auto& triggers = stream.definition->triggers;
        for (std::size_t i = 0; i < triggers.size(); ++i) {
            if (!stream.enabled[i]) continue;
            const auto& trigger = triggers[i];
            switch (trigger.kind) {
            case SoundTriggerKind::unknown:
            case SoundTriggerKind::initial:
                break;
            case SoundTriggerKind::discrete:
                for (const auto code : codes) {
                    if (code == trigger.code) fire(stream, i, 1.0f, inputs, sink);
                }
                break;
            case SoundTriggerKind::event:
                for (const auto event : inputs.events) {
                    if (event == trigger.event) fire(stream, i, 1.0f, inputs, sink);
                }
                break;
            case SoundTriggerKind::variable: {
                if (trigger.variable == SoundVariable::unknown) break;
                const auto index = static_cast<std::size_t>(trigger.variable);
                const auto current = inputs.variables[index];
                // First update after activation: Distance is evaluated as if
                // the listener came from beyond the threshold, so a source
                // the camera appears next to starts at once. Other variables
                // start from their current value (no spurious one-shots).
                const bool from_outside = !has_previous_ && trigger.variable == SoundVariable::distance;
                const auto previous = from_outside ? (trigger.increase ? -std::numeric_limits<float>::infinity()
                                                                       : std::numeric_limits<float>::infinity())
                    : has_previous_ ? previous_[index]
                                    : current;
                const bool crossed = trigger.increase ? previous < trigger.threshold && current >= trigger.threshold
                                                      : previous > trigger.threshold && current <= trigger.threshold;
                if (crossed) fire(stream, i, 1.0f, inputs, sink);
                break;
            }
            case SoundTriggerKind::random: {
                stream.countdown[i] -= dt;
                if (stream.countdown[i] > 0.0f) break;
                std::uniform_real_distribution<float> pick(trigger.minimum, (std::max)(trigger.minimum, trigger.maximum));
                stream.countdown[i] = (std::max)(pick(random_), 0.05f);
                std::uniform_real_distribution<float> volume(trigger.volume_minimum,
                                                             (std::max)(trigger.volume_minimum, trigger.volume_maximum));
                fire(stream, i, volume(random_), inputs, sink);
                break;
            }
            }
        }
    }
    previous_ = inputs.variables;
    has_previous_ = true;

    // Voice parameters from the curves.
    for (auto& stream : streams_) {
        const auto& definition = *stream.definition;
        const auto curve_input = [&](const formats::SoundCurve& curve) {
            return curve.control == SoundVariable::unknown ? 0.0f
                                                           : inputs.variables[static_cast<std::size_t>(curve.control)];
        };
        float volume = group_->volume * definition.volume * stream.volume;
        if (definition.volume_curve) volume *= evaluate_curve(*definition.volume_curve, curve_input(*definition.volume_curve));
        const auto frequency = definition.frequency_curve
            ? std::optional(evaluate_curve(*definition.frequency_curve, curve_input(*definition.frequency_curve)))
            : std::nullopt;
        const auto apply = [&](Stream::Voice& voice) {
            voice.age += dt;
            const float pitch = frequency && voice.clip && voice.clip->sample_rate > 0
                ? *frequency / static_cast<float>(voice.clip->sample_rate)
                : 1.0f;
            sink.set(voice.id, (std::max)(volume * voice.volume * voice.fade(), 0.0f), (std::max)(pitch, 0.01f),
                     inputs.position);
        };
        if (stream.loop.id) {
            if (sink.playing(stream.loop.id)) {
                apply(stream.loop);
            } else {
                stream.loop = {};
            }
        }
        std::erase_if(stream.one_shots, [&](const Stream::Voice& voice) {
            const bool faded = voice.released >= 0.0f && voice.fade() <= 0.0f;
            if (faded) sink.stop(voice.id);
            return faded || !sink.playing(voice.id);
        });
        for (auto& voice : stream.one_shots) apply(voice);
    }
}

} // namespace openrts::audio
