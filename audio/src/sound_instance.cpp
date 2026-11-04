#include <openrts/audio/sound_instance.hpp>

#include <algorithm>
#include <cstdio>
#include <string>
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

float attenuation_gain(const formats::SoundAttenuation& attenuation, const float distance, const float deactivation) {
    const float d = (std::max)(distance, 0.0f);
    const float d1 = attenuation.scale, d2 = attenuation.distance;
    if (attenuation.type == 1) {
        const float half = d1 == 0.0f ? 0.5f : 0.5f / d1;
        if (d2 <= 0.0f) return 1.0f;
        return d <= d2 ? 1.0f - half * d / d2 : half * d2 / d;
    }
    if (attenuation.type == 2) return d2 > 0.0f ? (std::max)(d1 - d / d2, 0.0f) : 0.0f;
    float knee = 50.0f;
    if (deactivation != 0.0f && deactivation < 250.0f) knee = 0.004f * deactivation * 50.0f;
    if (knee <= 0.0f) return 1.0f;
    return 0.5f * (d <= knee ? 2.0f - d / knee : knee / d);
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
        std::uint64_t start_frame = 0; // where a loop began in its clip
        std::string file;              // for the trace
        bool region = false;           // looping its clip's loop region (StartLoopRelease with cue points)

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
    // The trigger whose StartLoop / StartLoopRelease is this stream's loop
    // (RTS: the stream state's current trigger and its flag bit 0), and,
    // after a deactivation, the frame to resume the loop at.
    std::optional<std::size_t> current;
    std::optional<std::uint64_t> resume_frame;
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
    pair_state_.assign(group.trigger_pairs.size(), -1);
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

std::optional<TriggerPairState> SoundInstance::trigger_pairs() const {
    if (!pairs_seen_) return std::nullopt;
    TriggerPairState out;
    for (std::size_t p = 0; p < pair_state_.size() && p < 128; ++p) {
        if (pair_state_[p] < 0) continue;
        const std::uint32_t bit = 1u << (p & 31);
        out.known[p >> 5] |= bit;
        if (pair_state_[p] == 1) out.on[p >> 5] |= bit;
    }
    return out;
}

void SoundInstance::stop(VoiceSink& sink) {
    for (auto& stream : streams_) {
        // A playing loop is remembered with its position (RTS 0x4A9A84 keeps
        // the current trigger of a looping stream and stores the buffer
        // position); anything else is forgotten.
        stream.resume_frame.reset();
        if (stream.loop.id && stream.current && sink.playing(stream.loop.id) && stream.loop.clip &&
            stream.loop.clip->frames > 0) {
            const auto played = static_cast<std::uint64_t>(stream.loop.age * static_cast<float>(stream.loop.clip->sample_rate));
            stream.resume_frame = (stream.loop.start_frame + played) % stream.loop.clip->frames;
        } else {
            stream.current.reset();
        }
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
    if (!trace_.empty()) note(SIZE_MAX, activated_once_ ? "group on again" : "group on");
    if (activated_once_) {
        // Again (RTS 0x4A9BA8): random triggers wait their minimum delay, the
        // loops that played when the group went off start where they were;
        // trigger and stream states stay as they were.
        for (auto& stream : streams_) {
            const auto& triggers = stream.definition->triggers;
            for (std::size_t i = 0; i < triggers.size(); ++i) {
                if (triggers[i].kind == SoundTriggerKind::random) stream.countdown[i] = triggers[i].minimum;
            }
            if (stream.current && *stream.current < triggers.size()) fire(stream, *stream.current, 1.0f, inputs, sink);
            stream.resume_frame.reset();
        }
        return;
    }
    activated_once_ = true;
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

void SoundInstance::note(const std::size_t stream, const std::string& what) const {
    if (trace_.empty()) return;
    if (stream == SIZE_MAX) {
        std::fprintf(stderr, "[sound %8.2f] %s: %s\n", clock_, trace_.c_str(), what.c_str());
    } else {
        std::fprintf(stderr, "[sound %8.2f] %s stream %zu: %s\n", clock_, trace_.c_str(), stream + 1, what.c_str());
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
    const auto space = owner_ == formats::SoundOwner::environment || inputs.at_listener ? VoiceSpace::flat
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
        std::uint64_t start_frame = 0;
        if (playback != VoicePlayback::once && stream.resume_frame && clip->frames > 1) {
            start_frame = *stream.resume_frame % clip->frames; // a loop going on after a deactivation
            sink.seek(id, start_frame);
        } else if (playback == VoicePlayback::loop && random_loop_phase_ && clip->frames > 1) {
            start_frame = std::uniform_int_distribution<std::uint64_t>(0, clip->frames - 1)(random_);
            sink.seek(id, start_frame);
        }
        sink.set(id, 0.0f, 1.0f, inputs.position); // real values follow in this update
        const bool region = playback == VoicePlayback::loop_region && clip->has_loop();
        return {.id = id, .volume = volume, .clip = std::move(clip), .fade_in = action.fade, .start_frame = start_frame,
                .file = action.files[choice].name, .region = region};
    };
    const auto here = static_cast<std::size_t>(&stream - streams_.data());
    const auto name = [&](const formats::SoundAction& action) {
        return action.files.empty() ? std::string("(no file)") : action.files.front().name;
    };
    // A stream has one sound buffer (RTS 0x4A8C00): it is busy while that
    // plays, its loop or anything else (a one-shot, a released loop playing
    // out).
    const auto looping = [&] { return stream.loop.id && sink.playing(stream.loop.id); };
    const auto busy = [&] {
        return looping()
            || std::any_of(stream.one_shots.begin(), stream.one_shots.end(), [&](const Stream::Voice& v) { return sink.playing(v.id); });
    };
    const auto stop_all = [&] {
        if (stream.loop.id) sink.stop(stream.loop.id);
        for (const auto& voice : stream.one_shots) sink.stop(voice.id);
        stream.loop = {};
        stream.one_shots.clear();
    };

    for (const auto& action : trigger.actions) {
        switch (action.kind) {
        case SoundActionKind::play_one_shot:
            // Not while the stream plays something.
            if (busy()) {
                if (!trace_.empty()) note(here, "one-shot " + name(action) + " dropped: the stream is busy");
                break;
            }
            stop_all();
            if (auto voice = start(action, VoicePlayback::once); voice.id) {
                if (!trace_.empty()) note(here, "one-shot " + voice.file);
                stream.one_shots.push_back(std::move(voice));
            }
            break;
        case SoundActionKind::start_loop:
        case SoundActionKind::start_loop_release:
            // A looping loop goes on; anything else playing stops and the loop starts anew.
            if (looping()) {
                if (!trace_.empty()) note(here, "start " + name(action) + " ignored: " + stream.loop.file + " loops");
                break;
            }
            if (!trace_.empty() && busy()) note(here, "start " + name(action) + ": stops what plays out");
            stop_all();
            stream.loop = start(action, action.kind == SoundActionKind::start_loop ? VoicePlayback::loop
                                                                                    : VoicePlayback::loop_region);
            stream.current = index;
            if (!trace_.empty()) note(here, "loop " + stream.loop.file + (stream.loop.region ? " (region)" : ""));
            break;
        case SoundActionKind::release_loop_release:
        case SoundActionKind::release_loop_release_with_jump: {
            // What the stream plays: its loop, else a one-shot or a released loop.
            Stream::Voice* voice = stream.loop.id ? &stream.loop : nullptr;
            for (auto& v : stream.one_shots)
                if (!voice && sink.playing(v.id)) voice = &v;
            if (!voice || !sink.playing(voice->id)) {
                if (!trace_.empty()) note(here, "release: nothing plays");
                break;
            }
            stream.current.reset();
            const bool jump = action.kind == SoundActionKind::release_loop_release_with_jump;
            bool gone = false;
            if (jump && voice->clip && voice->clip->frames > 0) {
                // WithJump (case 6): from the place reached in a loop region
                // on to the end of the file; without one from (length - that
                // place), as much of the end as was played of the start.
                const auto frames = voice->clip->frames;
                std::uint64_t at = 0;
                if (const auto c = sink.cursor(voice->id)) {
                    at = *c;
                } else {
                    at = voice->start_frame + static_cast<std::uint64_t>(voice->age * static_cast<float>(voice->clip->sample_rate));
                }
                at %= frames;
                if (!voice->region) {
                    const auto from = frames - at;
                    if (from + 1 >= frames) {
                        gone = true; // nothing left to play: started and released in one go
                    } else {
                        sink.seek(voice->id, from);
                    }
                }
                if (!trace_.empty()) {
                    note(static_cast<std::size_t>(&stream - streams_.data()),
                         "jump " + voice->file + " at frame " + std::to_string(at) + " of " + std::to_string(frames)
                             + (voice->region ? ": on to the end" : gone ? ": nothing left, stopped" : ": from " + std::to_string(frames - at)));
                }
            } else if (!trace_.empty()) {
                note(static_cast<std::size_t>(&stream - streams_.data()), "release " + voice->file);
            }
            if (gone) {
                sink.stop(voice->id);
                if (voice == &stream.loop) {
                    stream.loop = {};
                } else {
                    std::erase_if(stream.one_shots, [&](const Stream::Voice& v) { return v.id == voice->id; });
                }
                break;
            }
            if (action.fade > 0.0f) {
                // Fade out, then stop (the voice keeps looping meanwhile).
                voice->fade_out = action.fade;
                voice->released = voice->age;
            } else {
                // The current pass plays to the end (a one-shot: no change).
                sink.release(voice->id);
            }
            if (voice == &stream.loop) {
                stream.one_shots.push_back(std::move(stream.loop));
                stream.loop = {};
            }
            break;
        }
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
            stream.current.reset();
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
                    target.current.reset();
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
    clock_ += dt;
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
        if (!camera || (distances && (distance > activate_within || distance > deactivate_beyond))) {
            // Codes wait in the queue (RTS: 96 entries) for the next activation.
            queued_.insert(queued_.end(), inputs.discrete.begin(), inputs.discrete.end());
            queued_.insert(queued_.end(), posted_.begin(), posted_.end());
            posted_.clear();
            if (queued_.size() > 96) queued_.erase(queued_.begin(), queued_.end() - 96);
            return;
        }
        activate(inputs, sink);
    } else if (camera_deactivates || (distances && distance > deactivate_beyond)) {
        if (!trace_.empty()) {
            char text[96];
            std::snprintf(text, sizeof text, "group off (%s, distance %.1f m)", camera_deactivates ? "camera" : "distance", distance);
            note(SIZE_MAX, text);
        }
        stop(sink);
        return;
    }

    // Discrete codes through the trigger pairs (RTS 0x4A87C8): an On code
    // is dropped while its pair is known to be on, an Off code while it is
    // known to be off; before a pair's first code both pass. A Halt code
    // sets its pair off once the object has had a pair code. Pairs past the
    // 128th keep no state.
    std::vector<std::uint32_t> incoming = std::move(queued_);
    queued_.clear();
    incoming.insert(incoming.end(), inputs.discrete.begin(), inputs.discrete.end());
    incoming.insert(incoming.end(), posted_.begin(), posted_.end());
    posted_.clear();
    std::vector<std::uint32_t> codes;
    const auto listed = [](const std::vector<std::uint32_t>& list, const std::uint32_t code) {
        return std::find(list.begin(), list.end(), code) != list.end();
    };
    for (const auto code : incoming) {
        bool pass = true;
        for (std::size_t p = 0; p < group_->trigger_pairs.size() && pass; ++p) {
            const auto& pair = group_->trigger_pairs[p];
            const bool kept = p < 128;
            auto& state = pair_state_[p];
            if (listed(pair.on, code)) {
                if (kept && state == 1) {
                    pass = false;
                } else {
                    if (kept) state = 1;
                    pairs_seen_ = true;
                }
            } else if (listed(pair.off, code)) {
                if (kept && state == 0) {
                    pass = false;
                } else {
                    if (kept) state = 0;
                    pairs_seen_ = true;
                }
            } else if (listed(pair.halt, code)) {
                if (kept && pairs_seen_) state = 0;
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
        float volume = group_->volume * definition.volume * stream.volume * inputs.gain;
        if (definition.volume_curve) volume *= evaluate_curve(*definition.volume_curve, curve_input(*definition.volume_curve));
        // The head out of a window silences the streams not allowed there.
        if (inputs.window && !definition.allow_on_window_camera) volume = 0.0f;
        // Sources: the loudest of the stream's points as heard (0x44B6B4). A
        // stream naming a point the group lacks has none (0x4DD0EC throws
        // while reading it, docs/sound.md).
        if (!definition.sources.empty() && !inputs.sources.empty()) {
            const bool valid = std::all_of(definition.sources.begin(), definition.sources.end(),
                                           [&](const std::uint32_t k) { return k < inputs.sources.size(); });
            if (valid) {
                float loudest = 0.0f;
                for (const auto k : definition.sources) {
                    const auto& point = inputs.sources[k];
                    float level = point.level;
                    if (definition.attenuation) level *= attenuation_gain(*definition.attenuation, point.distance, deactivate_beyond);
                    loudest = (std::max)(loudest, level);
                }
                volume *= loudest;
            }
        }
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
                if (!trace_.empty()) note(static_cast<std::size_t>(&stream - streams_.data()), "loop " + stream.loop.file + " ended");
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
