#pragma once

// RTS's cargo and loading in the game (docs/cargo.md, docs/reverse/cargo.md):
// the pickups among the world's objects, Num3 on them, loading cars each
// frame (simulation::Pickup), the cargo drawn on the cars and falling from
// the loaders, and the crane's "Load cargo" window.

#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <openrts/audio/sound_bank.hpp>
#include <openrts/audio/sound_instance.hpp>
#include <openrts/core/vfs.hpp>
#include <openrts/formats/cargo.hpp>
#include <openrts/formats/sound.hpp>
#include <openrts/formats/route_world.hpp>
#include <openrts/renderer/model_library.hpp>
#include <openrts/renderer/model_renderer.hpp>
#include <openrts/renderer/route_streamer.hpp>
#include <openrts/simulation/cargo.hpp>

namespace openrts::viewer {

class TrainView;

// What the route's cargo is made of: the kinds, the cars' loading, the cargo
// objects, and where their textures and shapes are.
class CargoCatalog {
public:
    CargoCatalog(std::shared_ptr<const core::Vfs> vfs, const core::VirtualPath& route);

    formats::CargoKinds kinds;
    formats::CargoDescriptions descriptions;
    std::vector<formats::CargoObject> objects;

    [[nodiscard]] const formats::CargoObject* object(std::string_view id) const;
    // A bulk kind's texture: GLOBAL/TEXTURES/<stem>1.ace (as rts1.exe
    // 0x50D298 names it), else without the number, else .png; nullopt none.
    [[nodiscard]] std::optional<core::VirtualPath> texture(const formats::CargoKind& kind) const;
    // A cargo object's shape: TRAINS/OBJECTS/CARGO, TRAINS/OBJECTS, GLOBAL/SHAPES.
    [[nodiscard]] std::optional<core::VirtualPath> shape(const formats::CargoObject& object) const;
    // The name shown (Russian first: tr() for RTS's, RussianName for own).
    [[nodiscard]] std::string name(const formats::CargoKind& kind) const;

private:
    std::shared_ptr<const core::Vfs> vfs_;
    mutable std::unordered_map<std::uint16_t, std::optional<core::VirtualPath>> textures_;
    mutable std::unordered_map<std::string, std::optional<core::VirtualPath>> shapes_;
};

// One model of a car's load, in the car's space.
struct CargoDraw {
    core::VirtualPath path;
    glm::mat4 local{1.0f};
};

// The models of a car's loads: a pile in each visible area, the objects
// (those still on a crane's hook where it holds them).
[[nodiscard]] std::vector<CargoDraw> cargo_draws(const CargoCatalog& catalog, const simulation::CarCargo& cargo);
// The car's LoadingAnimation nodes at CarCargo::opening (hatches, tipping).
void cargo_node_adjustments(const simulation::CarCargo& cargo, const renderer::RenderModel& model,
                            std::vector<renderer::NodeAdjustment>& adjustments);

class CargoView {
public:
    CargoView(std::shared_ptr<const core::Vfs> vfs, core::VirtualPath route);
    ~CargoView();
    CargoView(const CargoView&) = delete;
    CargoView& operator=(const CargoView&) = delete;

    [[nodiscard]] const CargoCatalog& catalog() const noexcept { return catalog_; }

    // Finds the pickups among `visible` (their .sd's ESD_Pickup_Object, or a
    // world Pickup's own fields), then runs those switched on over the cars
    // near them; their sounds (pickup.sms) heard at `listener`.
    void update(float elapsed, const std::vector<renderer::StreamedInstance>& visible, renderer::ModelLibrary& library,
                TrainView& trains, const glm::vec3& listener = {}, audio::VoiceSink* sink = nullptr, float gain = 1.0f);
    void stop_sound(audio::VoiceSink& sink);
    // A crane's nodes moving to the load it lowers (PickupCraneShapeNodes).
    [[nodiscard]] std::optional<std::vector<renderer::NodeAdjustment>> adjustments(const renderer::StreamedInstance& instance,
                                                                                  renderer::ModelLibrary& library) const;
    // Num3: the pickup nearest the aim (within 30 m, on the screen) and how
    // far from the aim it shows, as TrainView's car menu scores its own.
    [[nodiscard]] std::optional<float> pick(const glm::vec3& eye, const glm::mat4& view_projection, glm::vec2 aim);
    // Num3 (Ctrl+Num3: unload) on the pickup pick() chose.
    void use(bool unload, TrainView& trains);
    // The streams falling from the loaders pouring now.
    void submit(renderer::ModelRenderer& renderer, renderer::ModelLibrary& library);
    // The crane's window (ImGui), open while a crane waits on a standing car.
    void draw_ui(TrainView& trains);
    [[nodiscard]] bool window_open() const noexcept { return window_.has_value(); }
    void close_window();
    [[nodiscard]] std::size_t pickups() const noexcept { return pickups_.size(); }
    [[nodiscard]] std::string report() const;

private:
    using Key = std::tuple<std::int32_t, std::int32_t, std::uint32_t>; // world tile, UiD
    using TileKey = std::pair<std::int32_t, std::int32_t>;
    struct Place {
        std::unique_ptr<simulation::Pickup> pickup;
        glm::mat4 transform{1.0f};
        std::string shape;
        bool seen = false;          // among the visible ones at the last scan
        bool too_fast_said = false; // messages said once per approach
        bool unsuitable_said = false;
        simulation::PickupState said_state = simulation::PickupState::off;
        bool window_shown = false;  // its window opened for this activation
        // pickup.sms: its sound, the triggers to fire, the loop it plays.
        std::unique_ptr<audio::SoundInstance> sound;
        std::vector<std::uint32_t> triggers;
        std::uint32_t loop_stop{}; // the trigger that ends the loop playing, 0 none
        simulation::PickupState sound_state = simulation::PickupState::off;
        float fill{};              // the car's percent (Variable1)
        // A crane lowering or lifting: the point it works at (the scene) and
        // how far its hook is down, 0..1.
        std::optional<glm::vec3> crane_target;
        float crane_reach{};
        float crane_hook{};        // the hook's height over the target, m
    };
    // A crane lowering objects onto a car (CarLoad::arriving, lift).
    struct Lowering {
        Key pickup;
        std::size_t unit{}, car{};
        float drop{}; // m it lowers each from
    };
    // An object lifted off a car (unloaded), rising and gone.
    struct Lifted {
        Key pickup;
        std::size_t unit{}, car{};
        CargoDraw draw;
        float rise{}, top{};
        float height{};         // the object's
        std::uint32_t loading{}; // its loading type (the sound it ends with)
    };
    struct Stream {
        core::VirtualPath path;
        glm::mat4 transform{1.0f};
    };
    struct Window {
        Key pickup;
        std::vector<std::pair<std::size_t, std::size_t>> cars; // unit, car in TrainView
        std::vector<std::string> labels;
        int car = 0;
        int area = 0;
        int placement = 0;
        bool centred = true;
        int filter = 0;
        int item = -1;
        float weight_t = 0.0f;
        int amount = 1;
        int loaded = -1;
        double opened = -1.0; // ImGui's time it opened
    };
    // The cargo a crane offers: its CargoTable, else every object and bulk
    // kind of its loading types (RTS's one list for all).
    struct Offer {
        const formats::CargoObject* object = nullptr;
        const formats::CargoKind* kind = nullptr;
        float mass{}, mass_maximum{};
        int count = -1;
        std::string label;
    };

    const formats::WorldObjectPlacement* world_object(const Key& key);
    void scan(const std::vector<renderer::StreamedInstance>& visible, renderer::ModelLibrary& library);
    void say_state(Place& place, TrainView& trains);
    [[nodiscard]] std::vector<Offer> offers(const Place& place) const;
    void load_selected(Place& place, TrainView& trains);
    void unload_selected(Place& place, TrainView& trains, std::optional<std::size_t> index);
    void start_sound(Place& place, std::uint32_t seed);
    // The trigger pickup.sms gives a crane's work by the loading type done
    // (221 boxes .. 229 fridge; +1 unloading).
    static std::uint32_t crane_trigger(std::uint32_t loading, bool unloading);
    void animate(float elapsed, TrainView& trains);

    std::shared_ptr<const core::Vfs> vfs_;
    core::VirtualPath route_;
    CargoCatalog catalog_;
    std::map<Key, Place> pickups_;
    std::map<TileKey, formats::WorldTile> tiles_;
    std::map<TileKey, std::future<formats::WorldTile>> loading_;
    std::uint64_t scanned_generation_ = ~0ull;
    float since_scan_ = 0.0f;
    std::optional<Key> picked_;
    std::vector<Stream> streams_;
    float stream_turn_ = 0.0f;
    std::vector<Lowering> lowering_;
    std::vector<Lifted> lifted_;
    // Cars whose LoadingAnimation plays (unit, car): what it opens for this
    // frame (1 loading, 2 unloading; 0 shutting).
    std::map<std::pair<std::size_t, std::size_t>, std::uint32_t> openings_;
    std::shared_ptr<formats::SoundScript> sound_script_;
    core::VirtualPath sound_path_;
    bool sound_looked_up_ = false;
    audio::SoundBank bank_;
    std::vector<core::VirtualPath> stream_models_asked_;
    std::vector<std::future<renderer::StreamedModelAsset>> model_loading_;
    std::optional<Window> window_;
};

} // namespace openrts::viewer
