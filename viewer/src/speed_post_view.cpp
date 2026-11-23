#include "speed_post_view.hpp"

#include <chrono>
#include <cmath>
#include <array>
#include <cstdio>

#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>

#include <openrts/formats/track_database.hpp>
#include <openrts/simulation/track_network.hpp>

namespace openrts::viewer {

namespace {

std::string world_file_name(const std::int32_t tile_x, const std::int32_t tile_z) {
    char name[32];
    std::snprintf(name, sizeof name, "w%+07d%+07d.w", tile_x, tile_z);
    return name;
}

} // namespace

std::vector<PostDigitVertex> post_digit_quads(const std::string& text, const std::array<float, 4>& sign, const std::array<float, 3>& size) {
    std::vector<PostDigitVertex> out;
    const float height = size[0] > 0.0f ? size[0] : 0.2f;
    const float advance = size[1] > 0.0f ? size[1] : height * 0.6f;
    std::string digits;
    for (const char c : text)
        if (c >= '0' && c <= '9') digits.push_back(c);
    if (digits.empty()) return out;
    const float a = sign[3];
    // Facing out of the plate; read from in front: right across, up.
    const glm::vec3 facing{-std::sin(a), 0.0f, -std::cos(a)};
    const glm::vec3 up{0.0f, 1.0f, 0.0f};
    const glm::vec3 right = glm::cross(up, -facing);
    const glm::vec3 centre = glm::vec3{sign[0], sign[1] + size[2], sign[2]} + facing * 0.004f;
    const float width = advance * static_cast<float>(digits.size());
    for (std::size_t i = 0; i < digits.size(); ++i) {
        const int d = digits[i] - '0';
        const float u0 = static_cast<float>(d % 4) * 0.25f, v0 = static_cast<float>(d / 4) * 0.25f;
        const float left = -width * 0.5f + advance * static_cast<float>(i);
        const glm::vec3 tl = centre + right * left + up * (height * 0.5f);
        const glm::vec3 tr = centre + right * (left + advance) + up * (height * 0.5f);
        const glm::vec3 bl = centre + right * left - up * (height * 0.5f);
        const glm::vec3 br = centre + right * (left + advance) - up * (height * 0.5f);
        const PostDigitVertex q[4] = {{tl, u0, v0}, {tr, u0 + 0.25f, v0}, {br, u0 + 0.25f, v0 + 0.25f}, {bl, u0, v0 + 0.25f}};
        for (const int k : {0, 1, 2, 0, 2, 3}) out.push_back(q[k]);
    }
    return out;
}

std::string post_text(const simulation::TrackNetwork& network, const std::vector<std::uint32_t>& items) {
    const auto& all = network.database().items;
    for (const auto id : items) {
        if (id >= all.size()) continue;
        const auto post = formats::speed_post(all[id]);
        if (!post) continue;
        if (post->limit) {
            if (post->resume || post->speed <= 0.0f) return {};
            const bool mph = (post->flags & 0x100u) != 0;
            const float shown = post->speed * (mph ? 2.23694f : 3.6f);
            return std::to_string(static_cast<long>(std::lround(shown)));
        }
        if (post->number <= 0.0f) return {};
        return std::to_string(static_cast<long>(std::floor(post->number + 1e-3f)));
    }
    return {};
}

SpeedPostView::SpeedPostView(std::shared_ptr<const core::Vfs> vfs, core::VirtualPath route, const simulation::TrackNetwork* network)
    : vfs_(std::move(vfs)), route_(std::move(route)), network_(network) {}

SpeedPostView::~SpeedPostView() {
    for (auto& [_, job] : loading_)
        if (job.valid()) job.wait();
}

const formats::WorldObjectPlacement* SpeedPostView::world_object(const Key& key) {
    const TileKey tile{std::get<0>(key), std::get<1>(key)};
    auto found = tiles_.find(tile);
    if (found == tiles_.end()) {
        auto job = loading_.find(tile);
        if (job == loading_.end()) {
            const auto file = route_.resolve(std::string_view("WORLD")).value_or(route_).resolve(world_file_name(tile.first, tile.second));
            if (!file) return nullptr;
            loading_.emplace(tile, std::async(std::launch::async, [vfs = vfs_, path = *file] { return formats::load_world_tile(*vfs, path); }));
            return nullptr;
        }
        if (job->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return nullptr;
        found = tiles_.emplace(tile, job->second.get()).first;
        loading_.erase(job);
    }
    for (const auto& object : found->second.objects)
        if (object.kind == formats::WorldObjectKind::speed_post && object.uid == std::get<2>(key)) return &object;
    return nullptr;
}

// The route's TEXTURES first, then GLOBAL/TEXTURES.
std::optional<core::VirtualPath> SpeedPostView::texture_path(const std::string& name) const {
    if (name.empty()) return std::nullopt;
    if (const auto textures = route_.resolve(std::string_view("TEXTURES")))
        if (const auto file = textures->resolve(name); file && vfs_->exists(*file)) return file;
    if (const auto global = core::VirtualPath::parse("GLOBAL/TEXTURES/" + name); global && vfs_->exists(*global)) return global;
    return std::nullopt;
}

std::vector<SpeedPostView::Batch> SpeedPostView::collect(const std::vector<renderer::StreamedInstance>& visible, const glm::vec3& eye,
                                                         const float reach) {
    std::vector<Batch> batches;
    if (!network_) return batches;
    for (const auto& instance : visible) {
        if (instance.kind != formats::WorldObjectKind::speed_post) continue;
        const glm::vec3 at{instance.transform[3]};
        if (glm::distance(at, eye) > reach) continue;
        const Key key{instance.world_tile_x, instance.world_tile_z, instance.uid};
        auto found = posts_.find(key);
        if (found == posts_.end()) {
            const auto* object = world_object(key);
            if (!object) continue;
            Post post;
            post.texture = texture_path(object->speed_digit_texture);
            post.text = post_text(*network_, object->rail_items);
            for (const auto& sign : object->speed_signs)
                if (sign[1] > -50.0f) post.signs.push_back(sign);
            post.size = object->speed_text_size;
            found = posts_.emplace(key, std::move(post)).first;
        }
        const auto& post = found->second;
        if (!post.texture || post.text.empty() || post.signs.empty()) continue;
        Batch* batch = nullptr;
        for (auto& b : batches)
            if (b.texture == *post.texture) batch = &b;
        if (!batch) {
            batches.push_back({*post.texture, {}});
            batch = &batches.back();
        }
        const glm::mat4 to_scene = instance.transform;
        for (const auto& sign : post.signs) {
            for (auto v : post_digit_quads(post.text, sign, post.size)) {
                v.position = glm::vec3{to_scene * glm::vec4{v.position, 1.0f}};
                batch->vertices.push_back(v);
            }
        }
    }
    return batches;
}

} // namespace openrts::viewer
