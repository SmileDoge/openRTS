#pragma once

// openRTS's arcs between a pantograph and the contact wire (beyond RTS,
// 2026-11-24, testers). A jagged, flickering blue-white thread of light
// from the wire down to the pantograph's head (a new shape every frame),
// drawn out as the head drops away, blown back on the run; its light round
// it; its sound a snap as it strikes and a hissing buzz while it burns
// (pulsing at 100 Hz on an AC line), cut off with it:
// - when a pantograph's head touches the wire (risen) or leaves it (going
//   down), on a live line, in any season;
// - in winter (the season, or snow falling: ice on the wire), now and then
//   while running, only at the train's leading raised pantograph by the way
//   it goes (the one behind runs on a wire it has cleaned); more often the
//   faster.
// Its sounds are the content's SOUND/OPENRTS/PANTOGRAPH/arc_snap.wav and
// arc_buzz.wav when a mod lays them down, else ones made here.

#include <cstdint>
#include <memory>
#include <random>
#include <span>
#include <unordered_map>
#include <vector>

#include <glm/vec3.hpp>

#include <openrts/audio/sound_bank.hpp>

namespace openrts::viewer {

// One pantograph this frame.
struct ArcPantograph {
    std::uint64_t key{};    // the car and its pantograph
    glm::vec3 position{};   // where its head meets the wire, scene
    bool contact = false;   // its head at the wire (risen)
    bool live = true;       // the line here has voltage
    bool leading = false;   // the train's first raised pantograph the way it runs
    float speed{};          // m/s, without its sign
    bool ac = false;        // an AC line (its buzz pulses at 100 Hz)
    glm::vec3 back{0.0f, 0.0f, -1.0f}; // against the way it runs (the arc is blown so), scene, unit
};

struct PantographArc {
    std::uint64_t id{};     // this arc's own
    std::uint64_t key{};    // its pantograph
    glm::vec3 position{};   // at the wire
    float level{};          // this frame's brightness 0..1 (it flickers)
    float size = 1.0f;      // 1 a touch's, less a running spark's
    // Its thread this frame: from the wire down to the head, scene points.
    std::vector<glm::vec3> path;
};

class PantographArcs {
public:
    explicit PantographArcs(std::uint32_t seed = 20261124u) : random_(seed) {}

    // `icy`: winter or snow. Arcs begun this frame go to started().
    void update(float elapsed, std::span<const ArcPantograph> pantographs, bool icy);
    [[nodiscard]] const std::vector<PantographArc>& arcs() const noexcept { return shown_; }
    // Begun this frame: the arc and how long it lasts, s.
    struct Start {
        std::uint64_t id{};
        std::uint64_t key{};
        glm::vec3 position{};
        float length{};
        float size{};
        bool ac = false;
    };
    [[nodiscard]] const std::vector<Start>& started() const noexcept { return started_; }

    // Running sparks a second at `speed` m/s on an icy wire.
    [[nodiscard]] static float icy_rate(float speed) noexcept;

private:
    struct Arc {
        std::uint64_t id{};
        std::uint64_t key{};
        glm::vec3 position{};
        glm::vec3 back{0.0f, 0.0f, -1.0f};
        float length{};
        float left{};
        float size{};
        bool drawn_out = false; // the head leaving: the gap grows
        float speed{};
    };
    void begin(const ArcPantograph& p, float length, float size, bool drawn_out);

    std::minstd_rand random_;
    std::uint64_t next_id_ = 1;
    std::unordered_map<std::uint64_t, bool> contact_; // as last seen
    std::vector<Arc> arcs_;
    std::vector<PantographArc> shown_;
    std::vector<Start> started_;
};

// The arc's sounds made here, mono 44.1 kHz: its snap as it strikes (a sharp
// crack and a short sizzle), and its burning (a rough hiss and buzz to
// loop; `ac`: pulsing at 100 Hz).
[[nodiscard]] std::shared_ptr<const audio::AudioClip> make_arc_snap(std::uint32_t seed = 1);
[[nodiscard]] std::shared_ptr<const audio::AudioClip> make_arc_buzz(bool ac, std::uint32_t seed = 2);

} // namespace openrts::viewer
