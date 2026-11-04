#include "particle_effects.hpp"

#ifdef OPENRTS_HAS_WGPU
#include <algorithm>
#include <cmath>
#include <iostream>

#include <glm/geometric.hpp>

#include <openrts/formats/ace_file.hpp>

namespace openrts::viewer {
namespace {

// rts1.exe 0x41A318: the drops live in a box around the eye, 50 m to each
// side and 30 m up and down; a drop is a quad 4 m on a side.
constexpr float box_half_width = 50.0f;
constexpr float box_half_height = 30.0f;
constexpr float drop_size = 4.0f;

[[nodiscard]] std::uint32_t pack_abgr(const glm::vec4& colour) {
    const auto c = glm::clamp(colour, glm::vec4{0.0f}, glm::vec4{1.0f});
    const auto r = static_cast<std::uint32_t>(c.r * 255.0f + 0.5f), g = static_cast<std::uint32_t>(c.g * 255.0f + 0.5f),
               b = static_cast<std::uint32_t>(c.b * 255.0f + 0.5f), a = static_cast<std::uint32_t>(c.a * 255.0f + 0.5f);
    return a << 24 | b << 16 | g << 8 | r;
}

[[nodiscard]] std::uint32_t upload(renderer::GlowSpriteRenderer& sprites, const core::Vfs& vfs, const core::VirtualPath& path) {
    const auto decoded = formats::decode_ace_rgba(vfs, path);
    if (!decoded.ok()) {
        std::cout << "effect texture " << path.str() << ": " << decoded.error << '\n';
        return 0;
    }
    const auto& image = *decoded.image;
    return sprites.add_texture({.width = image.width, .height = image.height, .pixels = &image.pixels, .mips = &image.mips});
}

} // namespace

PrecipitationView::PrecipitationView(const WGPUDevice device, const WGPUTextureFormat target_format, std::shared_ptr<const core::Vfs> vfs)
    : sprites_(std::make_unique<renderer::GlowSpriteRenderer>(device, target_format)), vfs_(std::move(vfs)) {
    sprites_->set_size_factor(1.0f);
}

PrecipitationView::~PrecipitationView() = default;

void PrecipitationView::set_environment(const formats::EnvPrecipitation* precipitation, std::vector<formats::EnvWindLayer> wind,
                                        const std::vector<core::VirtualPath>& folders) {
    wind_ = std::move(wind);
    drops_.clear();
    texture_ = 0;
    precipitation_.reset();
    if (!precipitation) return;
    precipitation_ = *precipitation;
    for (const auto& folder : folders) {
        if (precipitation->shader.texture.empty()) break;
        if (auto file = folder.resolve(precipitation->shader.texture); file && vfs_->exists(*file)) {
            texture_ = upload(*sprites_, *vfs_, *file);
            break;
        }
    }
}

void PrecipitationView::update(const float elapsed, const glm::vec3& eye, const glm::vec3& right, const float amount, const glm::vec4& light) {
    drawn_ = false;
    if (!precipitation_ || amount <= 0.0f || texture_ == 0) {
        drops_.clear();
        return;
    }
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    const glm::vec3 low = eye - glm::vec3{box_half_width, box_half_height, box_half_width};
    const glm::vec3 high = eye + glm::vec3{box_half_width, box_half_height, box_half_width};
    const auto count = static_cast<std::size_t>(static_cast<float>(precipitation_->density) * std::clamp(amount, 0.0f, 1.0f));
    while (drops_.size() < count) {
        drops_.push_back({low.x + unit(random_) * (high.x - low.x), low.y + unit(random_) * (high.y - low.y),
                          low.z + unit(random_) * (high.z - low.z)});
    }
    drops_.resize(count);
    const float fall = precipitation_->speed * elapsed;
    const auto& frames = precipitation_->shader.frames;
    std::vector<renderer::GlowSprite> sprites;
    sprites.reserve(drops_.size());
    const auto tint = pack_abgr(light);
    for (std::size_t i = 0; i < drops_.size(); ++i) {
        auto& drop = drops_[i];
        // The frame by the distance (stamped frames: 0 far, 2 within 5 m, 3 within 2.5 m).
        glm::vec4 rect{0.0f, 1.0f, 1.0f, 0.0f};
        if (!frames.empty()) {
            const float distance = std::hypot(drop.x - eye.x, drop.z - eye.z);
            std::size_t frame = distance < 5.0f ? (distance >= 2.5f ? 2u : 3u) : 0u;
            frame = (std::min)(frame, frames.size() - 1);
            if (frames[frame].has_uv_stamp) {
                const auto* s = frames[frame].uv_stamp;
                rect = {s[0], s[3], s[2], s[1]};
            }
        }
        // The wind below its layers' heights above the eye.
        for (const auto& layer : wind_) {
            if (drop.y - eye.y <= layer.max_height) {
                drop += glm::vec3{layer.direction[0], layer.direction[1], layer.direction[2]} * layer.speed * elapsed;
            }
        }
        renderer::GlowSprite sprite;
        sprite.position = drop + right * (drop_size * 0.5f) - glm::vec3{0.0f, drop_size * 0.5f, 0.0f};
        sprite.radius = drop_size * 0.5f;
        sprite.uv = rect;
        sprite.colour = tint;
        sprite.texture = texture_;
        sprite.axis_x = right;
        sprite.axis_y = glm::vec3{0.0f, 1.0f, 0.0f};
        sprites.push_back(sprite);
        // Down, and back into the box where it left it.
        drop.y -= fall;
        if (drop.x < low.x || drop.x > high.x) {
            drop.x = drop.x < low.x ? high.x : low.x;
            drop.y = unit(random_) * (high.y - drop.y) + low.y;
        }
        if (drop.z < low.z || drop.z > high.z) {
            drop.z = drop.z < low.z ? high.z : low.z;
            drop.y = unit(random_) * (high.y - drop.y) + low.y;
        }
        if (drop.y < low.y || drop.y > high.y) {
            drop.x = low.x + unit(random_) * (high.x - low.x);
            drop.z = low.z + unit(random_) * (high.z - low.z);
            drop.y = high.y - (unit(random_) + static_cast<float>(i % 50) * 0.02f) * drop_size * 0.5f;
        }
    }
    sprites_->set_sprites(std::move(sprites));
    drawn_ = true;
}

void PrecipitationView::render(const WGPURenderPassEncoder pass, const glm::mat4& projection, const glm::mat4& view,
                               const float viewport_height) {
    if (!drawn_ || drops_.empty()) return;
    sprites_->render(pass, projection, view, viewport_height, renderer::GlowBlend::over);
}

SmokeView::SmokeView(const WGPUDevice device, const WGPUTextureFormat target_format, std::shared_ptr<const core::Vfs> vfs)
    : sprites_(std::make_unique<renderer::GlowSpriteRenderer>(device, target_format)), vfs_(std::move(vfs)) {
    sprites_->set_size_factor(1.0f);
}

SmokeView::~SmokeView() = default;

std::uint32_t SmokeView::texture(const core::VirtualPath& path) {
    const auto found = textures_.find(path.key());
    if (found != textures_.end()) return found->second;
    const auto id = vfs_->exists(path) ? upload(*sprites_, *vfs_, path) : 0u;
    textures_.emplace(path.key(), id);
    return id;
}

void SmokeView::set_origin_shift(const glm::vec3& shift) {
    for (auto& [_, emitter] : emitters_) {
        for (auto& particle : emitter.particles) particle.position -= shift;
    }
}

// rts1.exe 0x50D3B4: puffs at the exhaust's rate (more of them as the car
// runs), each pushed out along the nozzle, carried by the car's own motion
// fading away, rising as it thins, blown by the wind, growing until it is gone.
void SmokeView::update(const float elapsed, const std::vector<ExhaustSource>& sources, const glm::vec4& light) {
    for (auto& [_, emitter] : emitters_) emitter.seen = false;
    std::uniform_int_distribution<int> tenth(0, 9);
    for (const auto& source : sources) {
        auto& emitter = emitters_[source.key];
        emitter.seen = true;
        emitter.nozzle = source.position;
        emitter.direction = source.direction;
        emitter.velocity = source.velocity;
        emitter.nozzle_size = (std::max)(source.nozzle, 0.01f);
        if (emitter.texture == 0) emitter.texture = texture(source.texture);
        const float rate = source.rate;
        const float magnitude = source.magnitude;
        const float spread = magnitude * source.dispersion;
        const float mass = std::clamp(source.mass, 0.5f, 5.0f);
        if (rate <= 0.0f || elapsed <= 0.0f) continue;
        emitter.clock += elapsed;
        // More puffs as the car runs (and in the wind), so they keep together.
        const float speed = glm::length(source.velocity);
        float more = std::fabs(speed / elapsed + source.acceleration / elapsed / elapsed) * 0.003f / emitter.nozzle_size;
        more = rate >= more ? 1.0f : more / rate;
        float wind = 0.0f;
        for (const auto& layer : wind_) {
            if (layer.max_height <= source.position.y) continue;
            const glm::vec3 d{layer.direction[0], layer.direction[1], layer.direction[2]};
            wind = (std::max)(wind, glm::dot(d, d));
        }
        if (wind > 0.0f) more = (std::max)(more, std::sqrt(wind) * 0.001f / emitter.nozzle_size);
        const float interval = 1.0f / (rate * more);
        const glm::vec4 colour{static_cast<float>((source.colour >> 16) & 0xFFu) / 255.0f, static_cast<float>((source.colour >> 8) & 0xFFu) / 255.0f,
                               static_cast<float>(source.colour & 0xFFu) / 255.0f, static_cast<float>(source.colour >> 24) / 255.0f};
        const auto spawn = [&](const float sent_rate, const float sent_magnitude, const float sent_spread, const glm::vec4& sent_colour) -> Particle& {
            Particle particle;
            particle.growth = sent_magnitude;
            particle.dispersion = sent_spread;
            particle.rate = sent_rate;
            particle.mass = mass;
            particle.colour = sent_colour;
            emitter.particles.push_back(particle);
            return emitter.particles.back();
        };
        if (emitter.clock <= interval) {
            if (0.5f * interval < emitter.clock && emitter.clock - elapsed <= 0.5f * interval) {
                auto& half = spawn(rate, 1.0f, 0.5f, glm::vec4{colour.r, colour.g, colour.b, colour.a * 0.5f});
                half.size = 0.6f;
                half.scale = magnitude;
                half.started = false;
            }
        } else {
            float left = elapsed;
            do {
                const float jitter = static_cast<float>(tenth(random_)) * 0.04f - 0.2f + 1.0f;
                float dispersion = spread;
                if (more > 1.0f) dispersion = (spread + 9.0f) / (0.3f * jitter * more) - 9.0f;
                auto& puff = spawn(rate, magnitude * jitter, dispersion, colour);
                if (left > interval) puff.head_start = left;
                left -= interval;
            } while (left >= interval);
            emitter.clock = 0.0f;
        }
    }
    // Exhausts gone (the car left, the train changed): their smoke goes on fading.
    std::vector<renderer::GlowSprite> sprites;
    const float lift_factor = 0.1f; // PROBABLE: 0x50D3B4's third input
    for (auto it = emitters_.begin(); it != emitters_.end();) {
        auto& emitter = it->second;
        for (auto p = emitter.particles.begin(); p != emitter.particles.end();) {
            auto& particle = *p;
            float dt = elapsed;
            if (!particle.started) {
                particle.started = true;
                particle.position = emitter.nozzle;
                particle.size = 1.0f; // 0x50D3B4: a new puff (size below 1) starts at 1
                const float push = emitter.nozzle_size * 1.4f * particle.growth * particle.rate;
                particle.push = emitter.direction * push;
                particle.drift = emitter.velocity;
                if (particle.head_start > 0.0f) dt = particle.head_start;
            }
            particle.size += particle.growth * dt;
            if (particle.size <= particle.growth || particle.growth <= 0.0f) {
                particle.alpha = 0.7f * particle.growth / particle.size;
            } else {
                particle.alpha = (1.0f - particle.size / particle.growth) / (particle.dispersion + 9.0f) + 0.7f;
            }
            if (particle.alpha < 0.01f) {
                p = emitter.particles.erase(p);
                continue;
            }
            particle.drift *= 1.0f - 0.08f * dt;
            particle.lift = (1.0f - particle.alpha) * (1.0f - particle.mass * lift_factor);
            glm::vec3 step = particle.push * dt + particle.drift * dt + glm::vec3{0.0f, particle.lift * dt, 0.0f};
            for (const auto& layer : wind_) {
                if (layer.max_height <= particle.position.y) continue;
                const float k = (1.0f - particle.position.y / layer.max_height) * dt * 0.1f / particle.alpha;
                step += glm::vec3{layer.direction[0], layer.direction[1], layer.direction[2]} * k;
            }
            particle.position += step;
            renderer::GlowSprite sprite;
            sprite.position = particle.position;
            sprite.radius = particle.scale * particle.size * emitter.nozzle_size;
            sprite.colour = pack_abgr(glm::vec4{glm::vec3{particle.colour} * glm::vec3{light}, std::clamp(particle.colour.a * particle.alpha, 0.0f, 1.0f)});
            sprite.texture = emitter.texture;
            sprites.push_back(sprite);
            ++p;
        }
        if (!emitter.seen && emitter.particles.empty()) {
            it = emitters_.erase(it);
        } else {
            ++it;
        }
    }
    sprites_->set_sprites(std::move(sprites));
}

void SmokeView::render(const WGPURenderPassEncoder pass, const glm::mat4& projection, const glm::mat4& view, const float viewport_height) {
    if (emitters_.empty()) return;
    sprites_->render(pass, projection, view, viewport_height, renderer::GlowBlend::over);
}

std::size_t SmokeView::particles() const noexcept {
    std::size_t total = 0;
    for (const auto& [_, emitter] : emitters_) total += emitter.particles.size();
    return total;
}

} // namespace openrts::viewer
#endif
