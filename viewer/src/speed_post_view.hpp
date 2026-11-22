#pragma once

// The numbers on the route's speed posts and kilometre posts, as MSTS and
// RTS write them (testers: openRTS drew the kilometre posts blank). A
// Speedpost of the world file names its digit texture (Speed_Digit_Tex,
// 0-9 in a 4 x 4 grid), its plates (Speed_Sign_Shape: x y z in the shape
// and the plate's turn about Y) and the digits' size (Speed_Text_Size:
// height, advance, raise); the number is its track item's
// (SpeedpostTrItemData): a kilometre post's number, a limit's speed. A
// plate below the ground (the Russian signs' -100: they carry no number)
// is left out.
//
// Each plate faces along (-sin a, 0, -cos a) in the shape, the digits
// centred on its point, read from in front.

#include <array>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <openrts/core/vfs.hpp>
#include <openrts/formats/route_world.hpp>
#include <openrts/renderer/route_streamer.hpp>

namespace openrts::simulation {
class TrackNetwork;
}

namespace openrts::viewer {

struct PostDigitVertex {
    glm::vec3 position{}; // scene
    float u{}, v{};
};

// The quads of `text` (digits only, others skipped) on one plate, in the
// shape's frame: 6 vertices a digit.
[[nodiscard]] std::vector<PostDigitVertex> post_digit_quads(const std::string& text, const std::array<float, 4>& sign,
                                                            const std::array<float, 3>& size);
// What a post shows: a kilometre post its number (whole), a limit its speed
// (km/h, whole); nothing for the end of a limit or no item.
[[nodiscard]] std::string post_text(const simulation::TrackNetwork& network, const std::vector<std::uint32_t>& items);

class SpeedPostView {
public:
    SpeedPostView(std::shared_ptr<const core::Vfs> vfs, core::VirtualPath route, const simulation::TrackNetwork* network);
    ~SpeedPostView();

    struct Batch {
        core::VirtualPath texture;
        std::vector<PostDigitVertex> vertices; // scene
    };
    // The visible posts within `reach` of the eye, by texture.
    [[nodiscard]] std::vector<Batch> collect(const std::vector<renderer::StreamedInstance>& visible, const glm::vec3& eye,
                                             float reach = 300.0f);

private:
    using Key = std::tuple<std::int32_t, std::int32_t, std::uint32_t>;
    struct Post {
        std::optional<core::VirtualPath> texture;
        std::string text;
        std::vector<std::array<float, 4>> signs;
        std::array<float, 3> size{};
    };
    const formats::WorldObjectPlacement* world_object(const Key& key);
    std::optional<core::VirtualPath> texture_path(const std::string& name) const;

    std::shared_ptr<const core::Vfs> vfs_;
    core::VirtualPath route_;
    const simulation::TrackNetwork* network_;
    using TileKey = std::pair<std::int32_t, std::int32_t>;
    std::map<TileKey, formats::WorldTile> tiles_;
    std::map<TileKey, std::future<formats::WorldTile>> loading_;
    std::map<Key, Post> posts_;
};

} // namespace openrts::viewer
