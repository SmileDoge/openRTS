#pragma once

// Level crossings (world LevelCr objects) as RTS runs them: trains mark the
// crossing items on the track ahead of and behind them (rts1.exe 0x508520),
// each crossing closes its gates and flashes its lights from those marks
// (0x422C18), rings crossing.sms, and tells the road its state (cars stop at
// a closed crossing, 0x49396C). See docs/world-objects.md.

#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <glm/vec3.hpp>

#include <openrts/audio/sound_bank.hpp>
#include <openrts/audio/sound_instance.hpp>
#include <openrts/core/vfs.hpp>
#include <openrts/formats/route_world.hpp>
#include <openrts/renderer/model_library.hpp>
#include <openrts/renderer/model_renderer.hpp>
#include <openrts/renderer/route_streamer.hpp>
#include <openrts/simulation/track_network.hpp>
#include <openrts/simulation/train.hpp>

namespace openrts::viewer {

class CrossingView {
public:
    CrossingView(std::shared_ptr<const core::Vfs> vfs, core::VirtualPath route, const simulation::TrackNetwork* network);
    ~CrossingView();
    CrossingView(const CrossingView&) = delete;
    CrossingView& operator=(const CrossingView&) = delete;

    // Each frame: the crossings among the streamed objects, the trains'
    // marks, the gates, the lights and the bells.
    // `gain`: the share of outside sound that reaches the listener (his room's doors and windows).
    void update(float elapsed, const std::vector<renderer::StreamedInstance>& visible,
                const std::vector<const simulation::Train*>& trains, const glm::vec3& listener, audio::VoiceSink* sink,
                float gain = 1.0f);
    void stop_sound(audio::VoiceSink& sink);
    // The gates' and lights' frames of a crossing's shape (nullopt: not a crossing).
    [[nodiscard]] std::optional<std::vector<renderer::NodeAdjustment>> adjustments(const renderer::StreamedInstance& instance,
                                                                                    renderer::ModelLibrary& library);
    // A road database item's state for the cars: 0 open, 1 warned, 2 closed
    // (0x422C18 writes warned + closed into the road item).
    [[nodiscard]] std::uint8_t road_state(std::uint32_t road_item) const;
    [[nodiscard]] std::size_t crossings() const noexcept { return crossings_.size(); }
    [[nodiscard]] std::size_t closed() const noexcept;

private:
    using Key = std::tuple<std::int32_t, std::int32_t, std::uint32_t>; // world tile, UiD
    using TileKey = std::pair<std::int32_t, std::int32_t>;
    struct Mark {
        float time{};     // s until a train arrives (0: none known)
        float distance{}; // m to it; negative: under a train
    };
    struct Crossing {
        formats::WorldObjectPlacement object;
        glm::vec3 position{};
        bool closed = false;
        bool warned = false;
        float gates = 0.0f;  // 0 open .. 1 closed
        float lights = 0.0f; // 0 .. 1, swinging while warned
        bool lights_rising = true;
        std::unique_ptr<audio::SoundInstance> sound;
        std::vector<std::uint32_t> triggers; // for the next sound update
        bool seen = false;
    };

    void mark_trains(const std::vector<const simulation::Train*>& trains);
    [[nodiscard]] const formats::WorldObjectPlacement* world_object(const Key& key);
    void start_sound(Crossing& crossing);

    std::shared_ptr<const core::Vfs> vfs_;
    core::VirtualPath route_;
    const simulation::TrackNetwork* network_;
    std::map<Key, Crossing> crossings_;
    std::map<TileKey, std::future<formats::WorldTile>> loading_;
    std::map<TileKey, formats::WorldTile> tiles_;
    std::unordered_map<std::uint32_t, Mark> marks_; // by track item
    std::unordered_map<std::uint32_t, std::uint8_t> road_states_;
    audio::SoundBank bank_;
    std::shared_ptr<const formats::SoundScript> bell_;
    core::VirtualPath bell_path_;
    bool bell_looked_up_ = false;
};

} // namespace openrts::viewer
