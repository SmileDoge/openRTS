#pragma once

// openRTS's own sound of a train braking to a stop, beyond RTS (2026-11-22,
// testers; the "Brake shoes squealing" sound setting). Heard where the
// content's scripts have no squeal of their own.
//
// - The squeal, car by car: each car near the listener whose brake
//   cylinder holds 0.5 atm or more squeals below 26 km/h (loudest at a few
//   km/h). Not steadily: it catches, holds a second or three, drops out,
//   catches again (more and longer the slower); while it holds it now and
//   then steps to another mode of the shoe (a few per cent up or down), it
//   creeps up as the speed falls, and the cars differ in pitch. Its clip is
//   the content's own recording (the ТУ6's braking_squeal.wav, else RTS's
//   SOUND/x_uk_brakesqueal02a.wav or squeal.wav), its highs (an 11 kHz
//   recording has none above 5.5 kHz) remade and a little grit added when
//   loaded. Without any recording it stays silent (a made squeal sounded
//   cheap, testers).
// - The stop's moan, once, when there is one (SOUND/OPENRTS/BRAKES/
//   stop_moan.wav: openRTS ships none, a player's own mod may lay one
//   down, e.g. MODS_OPENRTS/<mod>/SOUND/OPENRTS/BRAKES): begun so that it
//   ends as the train stands (the
//   time to the stop from the speed and the deceleration), the cars'
//   squeal giving way under it; let go if the train speeds up again.
// - A quiet squeak from about 3 km/h to the stop (stop_squeak.wav, the
//   same way).
//
// Each voice is placed at its car; the train the listener is in hears its
// own a little more than the room's share of the outside (under the
// floor).

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <vector>

#include <glm/vec3.hpp>

#include <openrts/audio/audio_engine.hpp>
#include <openrts/core/vfs.hpp>

namespace openrts::viewer {

struct ExtraSoundCar {
    glm::vec3 position{}; // scene
    float speed{};        // m/s, without its sign
    float cylinder{};     // brake cylinder, atm
    bool squeals = true;  // false: its own script has a squeal
};

struct ExtraSoundTrain {
    std::uint64_t id{};          // the train's (its voices follow it)
    std::vector<ExtraSoundCar> cars;
    float speed{};               // m/s, without its sign
    float deceleration{};        // m/s², slowing positive
    bool inside = false;         // the listener is in this train (in a cab or salon)
};

// One car's squeal level (0..1) at its speed and brake cylinder pressure.
[[nodiscard]] float brake_squeal_level(float speed, float cylinder) noexcept;
// An 11 kHz recording's highs remade (a harmonic exciter over its top band,
// a little air following its loudness), at 44.1 kHz, its loudest stretch
// as a seamless loop, with a little grit. Clips of 32 kHz and more: only
// the loop.
[[nodiscard]] std::shared_ptr<const audio::AudioClip> lift_squeal(const audio::AudioClip& recording, std::uint32_t seed = 1);
class TrainExtraSounds {
public:
    // `load`: a file of the content (VFS path) or nothing.
    using Loader = std::function<std::shared_ptr<const audio::AudioClip>(const core::VirtualPath&)>;
    explicit TrainExtraSounds(Loader load) : load_(std::move(load)) {}

    // `inside_gain`: the outside's share in the listener's room.
    void update(float elapsed, std::span<const ExtraSoundTrain> trains, const glm::vec3& eye, float inside_gain, bool squeal,
                audio::AudioEngine& sound);
    void stop(audio::AudioEngine& sound);
    // What it took (the log): the squeal's file, "none" without one.
    [[nodiscard]] const std::string& squeal_source() const noexcept { return squeal_source_; }

private:
    struct CarSqueal {
        audio::VoiceId voice = 0;
        float level = 0.0f;      // eased
        float pitch0 = 1.0f;     // the car's own
        float mode = 1.0f;       // the shoe's mode now (a step of a few per cent)
        float mode_timer = 0.0f; // until the next chance of a step
        bool on = false;         // catching
        float timer = 0.0f;      // of this hold or gap
        float wobble = 1.0f, wobble_target = 1.0f, wobble_timer = 0.0f;
        glm::vec3 at{};
        bool seen = false;       // this frame
    };
    struct TrainState {
        std::map<std::size_t, CarSqueal> cars; // by car index
        audio::VoiceId moan = 0;
        float moan_age = 0.0f, moan_level = 0.0f;
        bool moan_done = false;  // played this stop (again once it runs on)
        audio::VoiceId squeak = 0;
        float squeak_level = 0.0f, squeak_pitch = 1.0f;
        glm::vec3 at{};
        bool seen = false;
    };
    void load_clips();
    float random(float lo, float hi);

    Loader load_;
    bool loaded_ = false;
    std::shared_ptr<const audio::AudioClip> squeal_, moan_, squeak_;
    float moan_seconds_ = 0.0f;
    std::string squeal_source_;
    std::map<std::uint64_t, TrainState> trains_;
    std::uint32_t random_ = 0x2545F491u;
};

} // namespace openrts::viewer
