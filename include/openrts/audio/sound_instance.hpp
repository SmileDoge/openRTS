#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <random>
#include <span>
#include <vector>

#include <glm/vec3.hpp>

#include <openrts/audio/sound_bank.hpp>
#include <openrts/formats/sound.hpp>

namespace openrts::audio {

using VoiceId = std::uint32_t;

enum class VoicePlayback : std::uint8_t {
    once,        // play to the end
    loop,        // loop the whole clip
    loop_region, // play into the clip's loop region and loop it (whole clip when it has none)
};

// How a voice is placed: flat (weather, environment), panned by direction
// only (world sounds: their scripts carry distance curves), or positional
// with distance rolloff (rolling stock: scripts rely on the engine's 3D
// attenuation, as RTS with DirectSound 3D buffers).
enum class VoiceSpace : std::uint8_t { flat, panned, positional };

// Where sound scripts send their voices: the miniaudio engine, or a recorder
// in tests. Positions are in scene coordinates.
class VoiceSink {
public:
    virtual ~VoiceSink() = default;
    // Returns 0 when the voice could not be started.
    virtual VoiceId play(std::shared_ptr<const AudioClip> clip, VoicePlayback playback, VoiceSpace space) = 0;
    // Leaves the loop: the voice plays to the end of its clip and finishes.
    virtual void release(VoiceId voice) = 0;
    virtual void stop(VoiceId voice) = 0;
    // volume 0..; pitch 1 = the clip's own rate.
    virtual void set(VoiceId voice, float volume, float pitch, const glm::vec3& position) = 0;
    [[nodiscard]] virtual bool playing(VoiceId voice) const = 0;
    // Moves a voice to `frame` of its clip (a looped voice starting
    // elsewhere than at its beginning).
    virtual void seek(VoiceId voice, std::uint64_t frame) {
        (void)voice;
        (void)frame;
    }
    // The frame of its clip a voice is at (RTS: the buffer's play cursor);
    // nullopt when the sink cannot tell.
    [[nodiscard]] virtual std::optional<std::uint64_t> cursor(VoiceId voice) const {
        (void)voice;
        return std::nullopt;
    }
};

enum class SoundCamera : std::uint8_t { external, cab, passenger };

// One SoundSources point of the group as the listener hears it (RTS
// 0x44B6B4): its volume times the share of it that reaches the listener's
// room, and its distance from the listener, m.
struct SoundSourceLevel {
    float level = 1.0f;
    float distance = 0.0f;
};

// The trigger pairs' states as RTS keeps them for RTSIsSMSTriggerOn
// (SMSObject::PairedFlags, 0x4A87C8): bit p of `on` / `known` for pair p.
struct TriggerPairState {
    std::array<std::uint32_t, 4> on{}, known{};
};

// What a sound object reacts to in one update.
struct SoundInputs {
    std::array<float, formats::sound_variable_count> variables{};
    SoundCamera camera = SoundCamera::external;
    glm::vec3 position{}; // of the sound source, scene coordinates
    std::span<const std::uint32_t> discrete;    // trigger codes since the last update
    std::span<const formats::SoundEvent> events; // events since the last update
    // Every voice's volume times this: the share of the sound that gets into
    // the listener's room (RTS 0x44AB7C with 0x5143DC; docs/sound.md).
    float gain = 1.0f;
    // The head out of a window: streams without AllowStreamOnWindowCamera
    // are silent (RTS 0x4A78A0, sound object flag 0x200).
    bool window = false;
    // The group's SoundSources, by index; empty: streams play at their own
    // volume. A stream with Sources plays at the loudest of its points, each
    // through the stream's Attenuation over its distance (0x44B6B4).
    std::span<const SoundSourceLevel> sources;
    // Heard at the listener (the cab's own script, a library sound in the
    // car): its voices are not placed. RTS places no sound at all (buffers
    // without pan or 3-D, 0x4AA178); a source at the listener's head, a
    // frame's travel away from it while the train moves, would be panned
    // and lose up to half its power by turns (miniaudio's spatializer).
    bool at_listener = false;
    // RTS sound object flag 0x20 (.ws sources and level crossings, 0x41B6D0
    // and 0x4210D9): every stream fades with the distance variable by its
    // Attenuation, the default rule when it has none (0x4A7764), unless its
    // volume curve is controlled by the distance (0x4A78A0).
    bool fades_with_distance = false;

    [[nodiscard]] float& operator[](formats::SoundVariable variable) {
        return variables[static_cast<std::size_t>(variable)];
    }
};

// Runs one ScalabiltyGroup of a sound script: activation, triggers, curves,
// loops and one-shots. Semantics: docs/sound.md.
class SoundInstance {
public:
    using ClipLookup = std::function<std::shared_ptr<const AudioClip>(const std::string& file)>;

    SoundInstance(std::shared_ptr<const formats::SoundScript> script, const formats::SoundGroup& group,
                  formats::SoundOwner owner, ClipLookup clips, std::uint32_t seed);
    SoundInstance(const SoundInstance&) = delete;
    SoundInstance& operator=(const SoundInstance&) = delete;
    SoundInstance(SoundInstance&&) noexcept;
    SoundInstance& operator=(SoundInstance&&) noexcept;
    ~SoundInstance();

    void update(float dt, const SoundInputs& inputs, VoiceSink& sink);
    // Stops every voice (deactivation, unloading). As RTS (0x4A9CF4), the
    // loop each stream was playing is remembered with its position and
    // starts again there when the group activates again (0x4A9BA8).
    void stop(VoiceSink& sink);
    // Fetches every clip the group can play, so the first update does not
    // decode files while voices already started elsewhere keep playing (one
    // car of a train running ahead of the others).
    void preload();
    // StartLoop voices (whole-clip loops) begin at a random frame instead of
    // the first: the same loop on many cars of a train does not add up in
    // phase (one loud point, comb filtering). openRTS choice; RTS starts its
    // buffers at 0 (PROBABLE, docs/sound.md).
    void set_random_loop_phase(bool on) noexcept { random_loop_phase_ = on; }
    // Logs what the streams do (starts, ignored starts, restarts, dropped
    // one-shots, releases and jumps, the group going on and off) to stderr,
    // each line starting with `label` (OPENRTS_SOUND_TRACE in the viewer).
    void set_trace(std::string label) { trace_ = std::move(label); }

    [[nodiscard]] bool active() const noexcept { return active_; }
    // The whole state as a network sends it (RTS 0x463A): per stream the
    // trigger whose loop plays, + 1 (0 none), with the stream's Volume and
    // trigger count.
    struct StreamState {
        std::uint32_t current{};
        float volume = 1.0f;
        std::uint32_t triggers{};
    };
    [[nodiscard]] std::vector<StreamState> stream_states() const;
    // A stream's state as a scenario reads it (SMSStream, RTS's
    // activity_WagonStateVariables): the trigger that last started a sound
    // in it, + 1, while a sound of it plays (a one-shot too); 0 when silent.
    [[nodiscard]] std::uint32_t stream_sounding(std::size_t stream) const;
    // Another client's whole state: a stream with a current trigger plays
    // it at the next active update (RTS 0x4A8C00 restarts the stream on it;
    // an inactive object keeps it for when it wakes).
    void restore(const std::vector<std::uint32_t>& current);
    // The trigger pairs' states; none before the object's first pair code
    // (RTS allocates PairedFlags then).
    [[nodiscard]] std::optional<TriggerPairState> trigger_pairs() const;
    [[nodiscard]] std::size_t voices() const noexcept;
    [[nodiscard]] const formats::SoundGroup& group() const noexcept { return *group_; }

private:
    struct Stream;
    // update() without the streams' audible marks, set after it.
    void update_streams(float dt, const SoundInputs& raw_inputs, VoiceSink& sink);
    void activate(const SoundInputs& inputs, VoiceSink& sink);
    void fire(Stream& stream, std::size_t trigger, float volume, const SoundInputs& inputs, VoiceSink& sink);
    // The codes through the trigger pairs (their states kept): those that pass.
    [[nodiscard]] std::vector<std::uint32_t> pass_pairs(const std::vector<std::uint32_t>& incoming);
    // Each code to the running streams' enabled discrete triggers.
    void run_codes(const std::vector<std::uint32_t>& codes, const SoundInputs& inputs, VoiceSink& sink);
    // The codes queued while inactive, at the activation: as states only
    // (catching_up_), then each stream's loop as they left it.
    void catch_up(const SoundInputs& inputs, VoiceSink& sink);
    [[nodiscard]] bool camera_listed(const formats::SoundActivation& activation, SoundCamera camera) const;

    std::shared_ptr<const formats::SoundScript> script_;
    const formats::SoundGroup* group_;
    ClipLookup clips_;
    std::mt19937 random_;
    std::vector<Stream> streams_;
    formats::SoundOwner owner_;
    std::array<float, formats::sound_variable_count> previous_{};
    bool has_previous_ = false;
    std::vector<std::int8_t> pair_state_; // DiscreteTriggerPairs: -1 not known yet, 0 off, 1 on
    bool pairs_seen_ = false;             // a pair code came (RTS: PairedFlags allocated)
    std::vector<std::uint32_t> posted_; // PostTrigger codes for the next update
    // SetVariableValue / IncreaseVariableValue: replace the input from then on.
    std::array<std::optional<float>, formats::sound_variable_count> overrides_{};
    bool active_ = false;
    bool activated_once_ = false; // Initial_Triggers fire on the first activation only
    // Discrete codes that came while inactive: RTS keeps them in the object's
    // 96-entry queue and runs them at its next active update; openRTS takes
    // only the states they leave (catch_up).
    std::vector<std::uint32_t> queued_;
    bool catching_up_ = false; // fire(): states only, no sound
    std::vector<std::uint32_t> restore_; // restore(): per stream, trigger + 1
    bool random_loop_phase_ = false;
    std::string trace_; // set_trace: empty, none
    float clock_ = 0.0f; // s of updates, for the trace
    void note(std::size_t stream, const std::string& what) const;
};

// Value of a curve at `x`: linear between points, clamped at the ends.
[[nodiscard]] float evaluate_curve(const formats::SoundCurve& curve, float x);

// A stream's Attenuation ( D1 D2 type ) at `distance` m (RTS 0x4A7764):
// type 1 falls from 1 to 0.5 / D1 at D2, then as D2 / distance; type 2 is
// D1 - distance / D2 down to 0; any other type halves at 50 m (or at a fifth
// of `deactivation` when that is below 250 m) and falls as 1 / distance
// beyond.
[[nodiscard]] float attenuation_gain(const formats::SoundAttenuation& attenuation, float distance,
                                     float deactivation = 0.0f);

} // namespace openrts::audio
