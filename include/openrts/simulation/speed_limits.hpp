#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include <openrts/formats/track_database.hpp>
#include <openrts/simulation/track_network.hpp>

// Speed limits from the route's speed posts (docs/paths.md): a limit post
// sets the limit for trains passing it in the direction its sign faces, a
// post that ends a restriction sets the route's SpeedLimit back (RTS
// 0x482680 reads them). A lower limit applies from the train's front, a
// higher one once the whole train has passed the post.
namespace openrts::simulation {

class Train;

struct SpeedPostOnTrack {
    std::uint32_t item{};
    std::uint32_t node{};
    double offset{};
    // The direction of travel on the node the post is for (along the node,
    // or against it); posts whose angle does not tell apply both ways.
    bool along = true;
    bool against = true;
    formats::SpeedPost post;
};

struct SpeedLimit {
    float current = -1.0f;      // m/s, -1: none
    float ahead = -1.0f;        // m/s, the next post's limit (-1: none within the look-ahead)
    double ahead_distance = 0.0; // m from the leading end
};

// A train's category for speed posts: the ТЧД console's "Прочий" /
// "Пассажирский" / "Грузовой" (RTS train byte +1, from the server's 0x4607 /
// 0x4605 in a network game).
enum class TrainCategory : std::uint8_t { other = 0, passenger = 1, freight = 2 };
// openRTS's own choice where nothing names it: no passenger cars, freight.
[[nodiscard]] constexpr TrainCategory category_from_cars(const bool freight) noexcept {
    return freight ? TrainCategory::freight : TrainCategory::passenger;
}

class SpeedLimits {
public:
    // `route_limit`: the .trk SpeedLimit (m/s, 0: none).
    SpeedLimits(const TrackNetwork& network, float route_limit);

    // The limit for a train moving the way it last moved; `category` picks
    // the posts it obeys (applies).
    [[nodiscard]] SpeedLimit evaluate(const Train& train, TrainCategory category, double look_ahead = 3000.0) const;
    // The limit in force for a train travelling `forward` on `node` at
    // `offset`: the nearest applicable post behind, else the route's limit.
    [[nodiscard]] float limit_at(std::uint32_t node, double offset, bool forward, TrainCategory category) const;
    [[nodiscard]] float route_limit() const noexcept { return route_limit_; }
    // The limits ahead of the train within `distance` m: (speed m/s, metres
    // from the leading end), nearest first, at most `count` (Ctrl+F5).
    [[nodiscard]] std::vector<std::pair<float, double>> limits_ahead(const Train& train, TrainCategory category, double distance,
                                                                     std::size_t count) const;
    // The train's ordinate (kilometre and picket, RTS's "Milepost"): the
    // nearest kilometre post behind its leading end plus the distance from
    // it, counting up or down as the next post ahead tells; nothing without
    // a post within 5 km behind.
    [[nodiscard]] std::optional<double> ordinate(const Train& train) const;
    [[nodiscard]] const std::vector<SpeedPostOnTrack>& posts() const noexcept { return posts_; }
    // The post for item `item`, if it is a limit post on the network.
    [[nodiscard]] const SpeedPostOnTrack* post(std::uint32_t item) const;
    // Whether `post` applies to travel `forward` on its node by a train of
    // `category` (RTS 0x47F7B4: "Прочий" obeys every post; one with flag
    // 0x20 only passenger trains, one with 0x40 only freight trains, so a
    // post with both only "Прочий").
    [[nodiscard]] static bool applies(const SpeedPostOnTrack& post, bool forward, TrainCategory category);

private:
    const TrackNetwork* network_;
    float route_limit_;
    std::vector<SpeedPostOnTrack> posts_;
    std::unordered_map<std::uint32_t, std::size_t> by_item_;
    std::unordered_map<std::uint32_t, float> mileposts_; // item -> the kilometre (or mile) it shows
};

} // namespace openrts::simulation
