#ifdef OPENRTS_HAS_WGPU
#include "signal_view.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>

#include <glm/geometric.hpp>
#include <glm/vec4.hpp>

#include <openrts/formats/ace_file.hpp>
#include <openrts/formats/signal_config.hpp>
#include <openrts/formats/signal_script.hpp>
#include <openrts/simulation/train.hpp>

#ifdef OPENRTS_HAS_IMGUI
#include <imgui.h>
#endif

namespace openrts::viewer {
namespace {

[[nodiscard]] const char* aspect_name(const float state) {
    return formats::to_string(static_cast<formats::SignalAspect>(std::clamp(static_cast<int>(state), 0, 9)));
}

} // namespace

struct SignalView::Loaded {
    std::shared_ptr<formats::SignalConfig> config;
    std::map<std::string, formats::AceRgbaImage> light_images; // LightTex name (lower case)
    std::shared_ptr<formats::SignalScriptFile> scripts;
    std::vector<formats::WorldObjectPlacement> signals;
    std::string error;
};

SignalView::SignalView(std::shared_ptr<const core::Vfs> vfs, core::VirtualPath route, const formats::RoutePosition origin,
                       simulation::TrackNetwork& network)
    : vfs_(std::move(vfs)), route_(std::move(route)), origin_(origin), network_(&network) {
    loading_ = std::async(std::launch::async, [vfs = vfs_, route = route_] {
        auto loaded = std::make_unique<Loaded>();
        loaded->config = std::make_shared<formats::SignalConfig>(formats::load_signal_config(*vfs, route));
        if (!loaded->config->ok()) {
            loaded->error = loaded->config->error;
            return loaded;
        }
        loaded->scripts = std::make_shared<formats::SignalScriptFile>(formats::load_signal_scripts(*vfs, route, *loaded->config));
        // LightTex images, from the route's TEXTURES (then GLOBAL/TEXTURES).
        for (const auto& texture : loaded->config->light_textures) {
            std::string key = texture.name;
            for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            for (const auto* folder : {"TEXTURES", "../../GLOBAL/TEXTURES"}) {
                const auto directory = route.resolve(std::string_view(folder));
                const auto path = directory ? directory->resolve(texture.file) : std::nullopt;
                if (!path || !vfs->exists(*path)) continue;
                auto decoded = formats::decode_ace_rgba(*vfs, *path);
                if (decoded.ok()) loaded->light_images[key] = std::move(*decoded.image);
                break;
            }
        }
        // Every world signal of the route (the streamer only loads tiles near the camera).
        if (const auto world = route.resolve(std::string_view("WORLD"))) {
            for (const auto& entry : vfs->list(*world)) {
                if (entry.directory) continue;
                const auto file = std::string(entry.path.filename());
                if (file.empty() || (file.back() != 'w' && file.back() != 'W')) continue;
                const auto tile = formats::load_world_tile(*vfs, entry.path);
                for (const auto& object : tile.objects) {
                    if (object.kind == formats::WorldObjectKind::signal) loaded->signals.push_back(object);
                }
            }
        }
        return loaded;
    });
}

SignalView::~SignalView() {
    if (loading_.valid()) loading_.wait();
}

void SignalView::attach(const WGPUDevice device, const WGPUTextureFormat target_format) {
    glows_ = std::make_unique<renderer::GlowSpriteRenderer>(device, target_format);
    if (!glows_->ready()) glows_.reset();
    markers_ = std::make_unique<renderer::DebugLineRenderer>(device, target_format);
    if (!markers_->ready()) markers_.reset();
}

void SignalView::detach() {
    glows_.reset();
    markers_.reset();
    light_textures_.clear();
    textures_uploaded_ = false;
}

void SignalView::update(const float elapsed, const simulation::Train* train, const glm::vec3& eye) {
    eye_ = eye;
    train_ = train && train->placed() ? train : nullptr;
    if (!system_ && loading_.valid() && loading_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        loaded_ = loading_.get();
        if (!loaded_->error.empty()) {
            status_ = "signals: " + loaded_->error;
            std::cout << status_ << '\n';
            return;
        }
        system_ = std::make_unique<simulation::SignalSystem>(*network_, loaded_->config, loaded_->scripts, loaded_->signals);
        for (std::uint32_t i = 0; i < system_->objects().size(); ++i) {
            const auto& placement = system_->objects()[i].placement;
            by_world_object[{placement.position.tile_x, placement.position.tile_z, placement.uid}] = i;
        }
        status_ = "signals: " + std::to_string(system_->objects().size()) + " objects, "
            + std::to_string(system_->heads().size()) + " heads, " + std::to_string(loaded_->scripts->scripts.size()) + " scripts";
        std::cout << status_ << '\n';
        for (const auto& message : loaded_->scripts->messages) std::cout << "  " << message << '\n';
        std::cout.flush();
        system_->update({}, simulation::train_step);
    }
    if (!system_) return;
    if (glows_ && !textures_uploaded_ && loaded_) {
        textures_uploaded_ = true;
        for (const auto& [name, image] : loaded_->light_images) {
            light_textures_[name] = glows_->add_texture({.width = image.width, .height = image.height, .pixels = &image.pixels});
            light_texture_widths_[name] = image.width;
        }
    }
    accumulator_ += (std::min)(elapsed, 0.25f);
    const simulation::Train* list[] = {train_};
    while (accumulator_ >= simulation::train_step) {
        accumulator_ -= simulation::train_step;
        system_->update(train_ ? std::span<const simulation::Train* const>(list) : std::span<const simulation::Train* const>{},
                        simulation::train_step);
        for (const auto& pass : system_->take_passes()) {
            ++passes_;
            if (pass.at_danger()) ++passes_at_danger_;
            last_pass_ = std::string(formats::to_string(pass.aspect)) + (pass.at_danger() ? " (passed at danger)" : "");
            if (pass.at_danger()) std::cout << "train passed a signal at " << formats::to_string(pass.aspect) << '\n';
        }
    }
}

std::vector<renderer::NodeAdjustment> SignalView::hidden_parts(const renderer::StreamedInstance& instance,
                                                               renderer::ModelLibrary& library) const {
    std::vector<renderer::NodeAdjustment> hidden;
    if (!system_ || instance.kind != formats::WorldObjectKind::signal) return hidden;
    const auto found = by_world_object.find({instance.world_tile_x, instance.world_tile_z, instance.uid});
    if (found == by_world_object.end()) return hidden;
    const auto& object = system_->objects()[found->second];
    const auto* shape = system_->config().find_shape(object.placement.shape_file.filename().string());
    const auto* model = library.find(instance.source_path);
    const auto* rig = model ? model->rig() : nullptr;
    if (!shape || !rig) return hidden;
    // RTS 0x4491E8 (CONFIRMED): a DECOR sub-object whose bit is clear in
    // the world's SignalSubObjs hides the shape matrix of that name (matrix
    // flag 1). Heads are never hidden, OPTIONAL plays no part. Collapsing
    // the matrix also hides its children (PROBABLE: decor matrices are leaves).
    const auto lower = [](std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    for (const auto& sub : shape->sub_objects) {
        if (sub.kind != formats::SignalSubObjectKind::decor) continue;
        if (sub.index < 32 && (object.placement.signal_sub_objects & (1u << sub.index))) continue;
        const auto name = lower(sub.name);
        for (std::size_t m = 0; m < rig->matrices.size(); ++m) {
            if (lower(rig->matrices[m].name) == name) {
                hidden.push_back({.matrix = static_cast<std::int32_t>(m), .local = glm::mat4{0.0f}});
            }
        }
    }
    return hidden;
}

void SignalView::collect_lights(const std::span<const renderer::StreamedInstance> instances,
                                renderer::ModelLibrary& library,
                                const glm::vec3& eye, const float draw_distance) {
    sprites_.clear();
    if (!system_ || !lights_) return;
    const auto& config = system_->config();
    const auto& heads = system_->heads();
    for (const auto& instance : instances) {
        if (instance.kind != formats::WorldObjectKind::signal) continue;
        const auto position = glm::vec3{instance.transform[3]};
        if (glm::distance(position, eye) > draw_distance) continue;
        const auto found = by_world_object.find({instance.world_tile_x, instance.world_tile_z, instance.uid});
        if (found == by_world_object.end()) continue;
        const auto& object = system_->objects()[found->second];
        const auto* model = library.find(instance.source_path);
        const auto* rig = model ? model->rig() : nullptr;
        const auto* shape = config.find_shape(object.placement.shape_file.filename().string());
        for (const auto h : object.heads) {
            const auto& head = heads[h];
            if (!head.type) continue;
            const auto& type = *head.type;
            std::uint32_t texture = 0;
            glm::vec4 uv{0.0f, 0.0f, 1.0f, 1.0f};
            // RTS 0x408574: far, distance * 0.012 * LightsScale * |u1 - u0|,
            // times width / 512 for an ACE texture (PROBABLE: flag +13).
            float far_scale = 0.012f;
            if (const auto* light_texture = config.find_texture(type.light_texture)) {
                std::string key = light_texture->name;
                for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (const auto it = light_textures_.find(key); it != light_textures_.end()) texture = it->second;
                uv = {light_texture->uv[0], light_texture->uv[1], light_texture->uv[2], light_texture->uv[3]};
                far_scale = 0.012f * std::abs(light_texture->uv[0] - light_texture->uv[2]);
                if (const auto width = light_texture_widths_.find(key); width != light_texture_widths_.end()) {
                    far_scale *= static_cast<float>(width->second / 512);
                }
            }
            const auto draw = static_cast<std::uint32_t>(std::max(0.0f, head.draw_state));
            const formats::SignalDrawState* state = nullptr;
            for (const auto& candidate : type.draw_states) {
                if (candidate.index == draw) state = &candidate;
            }
            if (!state) continue;
            // RTS 0x4491E8: the light's Position plus the translation (not
            // the rotation) of the head sub-object's matrix, in RTS space,
            // whose Z is the shape's flipped: (x - tx, y + ty, tz - z) here.
            // Checked on TestRoute_RTS: the lenses face -Z in the shapes.
            glm::vec3 offset{0.0f};
            if (rig && shape) {
                for (const auto& sub : shape->sub_objects) {
                    if (sub.index != head.sub_object) continue;
                    for (const auto& matrix : rig->matrices) {
                        std::string a = matrix.name;
                        std::string b = sub.name;
                        for (auto& c : a) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        for (auto& c : b) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        if (a == b) {
                            offset = {matrix.values[9], matrix.values[10], matrix.values[11]};
                            break;
                        }
                    }
                }
            }
            const glm::mat4& frame = instance.transform;
            for (const auto& lit : state->lights) {
                if (lit.flashing && !(head.flash_timer < type.flash_on)) continue;
                const formats::SignalLight* light = nullptr;
                for (const auto& candidate : type.lights) {
                    if (candidate.index == lit.light) light = &candidate;
                }
                if (!light) continue;
                const auto* colour = config.find_colour(light->colour);
                if (!colour || colour->argb[0] == 0 || (colour->argb[1] | colour->argb[2] | colour->argb[3]) == 0) continue;
                // RTS 0x449818: a quad of half size Radius in the signal's
                // own XY plane (identity light rotation), so it lies on the
                // lens: seen from the side it is edge-on, from behind the
                // housing hides it. Lifted 1 cm off the lens to its front.
                const float z = offset.z - light->position[2];
                const float lift = z < 0.0f ? -0.01f : 0.01f;
                const auto world = glm::vec3{frame * glm::vec4{light->position[0] - offset.x, light->position[1] + offset.y,
                                                               z + lift, 1.0f}};
                const auto axis_x = glm::normalize(glm::vec3{frame[0]});
                const auto axis_y = glm::normalize(glm::vec3{frame[1]});
                const auto packed = static_cast<std::uint32_t>(colour->argb[1]) | (static_cast<std::uint32_t>(colour->argb[2]) << 8)
                    | (static_cast<std::uint32_t>(colour->argb[3]) << 16) | (static_cast<std::uint32_t>(colour->argb[0]) << 24);
                sprites_.push_back({.position = world,
                                    .radius = light->radius,
                                    .uv = uv,
                                    .colour = packed,
                                    .texture = texture,
                                    .far_scale = far_scale,
                                    .axis_x = axis_x,
                                    .axis_y = axis_y});
            }
        }
    }
}

void SignalView::draw(const WGPURenderPassEncoder pass, const FrameView& frame, const float viewport_height) {
    if (glows_) {
        glows_->set_sprites(sprites_);
        glows_->render(pass, frame.projection, frame.view, viewport_height);
    }
    if (markers_ && system_ && show_markers_) {
        // A post per head, coloured by its aspect, within 3 km.
        std::vector<renderer::DebugVertex> lines;
        for (const auto& head : system_->heads()) {
            if (!head.placed) continue;
            const auto point = network_->point(head.node, head.offset);
            if (!point) continue;
            const glm::vec3 base{
                static_cast<float>((point->tile_x - origin_.tile_x) * simulation::tile_size + point->x - origin_.x),
                static_cast<float>(point->y - origin_.y) + 0.5f,
                static_cast<float>((point->tile_z - origin_.tile_z) * simulation::tile_size + point->z - origin_.z)};
            if (glm::distance(base, frame.eye) > 3000.0f) continue;
            const auto aspect = static_cast<int>(head.aspect());
            const auto colour = aspect == 0 ? renderer::debug_colour(255, 30, 30)
                : aspect <= 2              ? renderer::debug_colour(255, 120, 0)
                : aspect <= 5              ? renderer::debug_colour(255, 230, 0)
                : aspect <= 7              ? renderer::debug_colour(0, 230, 60)
                                           : renderer::debug_colour(160, 160, 160);
            lines.push_back({base, colour});
            lines.push_back({base + glm::vec3{0.0f, 6.0f, 0.0f}, colour});
        }
        markers_->set_lines(lines);
        markers_->render(pass, frame.projection * frame.view);
    }
}

void SignalView::collect_labels(std::vector<WorldLabel>& labels, const glm::vec3& eye, const float range) const {
    if (!system_) return;
    const auto& heads = system_->heads();
    const auto& objects = system_->objects();
    for (std::uint32_t o = 0; o < objects.size(); ++o) {
        const auto& object = objects[o];
        const auto& placement = object.placement;
        const glm::vec3 base{
            static_cast<float>((placement.position.tile_x - origin_.tile_x) * formats::route_tile_size + placement.position.x - origin_.x),
            static_cast<float>(placement.position.y - origin_.y),
            static_cast<float>((placement.position.tile_z - origin_.tile_z) * formats::route_tile_size + placement.position.z - origin_.z)};
        if (glm::distance(base, eye) > range) continue;
        // Above the highest light of its heads (lights below the ground are
        // the "no light" placeholders).
        float top = 1.0f;
        std::string text = "signal uid " + std::to_string(placement.uid);
        bool danger = false;
        for (const auto h : object.heads) {
            const auto& head = heads[h];
            if (head.type) {
                for (const auto& light : head.type->lights) top = (std::max)(top, light.position[1]);
            }
            text += "\n#" + std::to_string(h) + " " + (head.type ? head.type->name : std::string{"?"}) + "  "
                + aspect_name(head.state) + " (" + std::to_string(static_cast<int>(head.draw_state)) + ")";
            // `enabled` (script variable): the head is in a train's chain of
            // signals ahead; the others run free every frame (RTS 0x479AFC).
            if (head.enabled != 0.0f) text += "  [train]";
            danger = danger || head.state <= static_cast<float>(formats::SignalAspect::stop_and_proceed);
        }
        labels.push_back({.position = base + glm::vec3{0.0f, top + 1.2f, 0.0f},
                          .text = std::move(text),
                          .colour = danger ? 0xff6060ffu : 0xff80ff80u});
    }
}

void SignalView::draw_overlay() {
#ifdef OPENRTS_HAS_IMGUI
    if (!ImGui::CollapsingHeader("Signals", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::TextUnformatted(status_.c_str());
    if (!system_) return;
    const auto& stats = system_->stats();
    ImGui::Text("Last step: %zu chain runs, %zu free runs; %zu heads run every step", stats.chain_runs, stats.free_runs,
                stats.always_run_heads);
    ImGui::Text("Lights drawn: %zu", sprites_.size());
    ImGui::SameLine();
    ImGui::Checkbox("Lights", &lights_);
    ImGui::SameLine();
    ImGui::Checkbox("Aspect posts", &show_markers_);
    if (train_) {
        const auto ahead = system_->ahead(*train_);
        if (ahead.valid) {
            ImGui::Text("Next signal: %s in %.0f m%s", formats::to_string(ahead.aspect), ahead.distance,
                        ahead.speed >= 0.0f ? (" (limit " + std::to_string(static_cast<int>(ahead.speed * 3.6f)) + " km/h)").c_str() : "");
        } else {
            ImGui::TextUnformatted("Next signal: none ahead");
        }
        ImGui::Text("Passed %zu signals, %zu at danger%s%s", passes_, passes_at_danger_, last_pass_.empty() ? "" : "; last: ",
                    last_pass_.c_str());
    }
    if (ImGui::TreeNode("Nearest head")) {
        // The head nearest to the camera.
        const auto& heads = system_->heads();
        std::optional<std::uint32_t> best;
        float best_distance = 1e9f;
        for (std::uint32_t h = 0; h < heads.size(); ++h) {
            const auto& placement = system_->objects()[heads[h].object].placement;
            const glm::vec3 local{
                static_cast<float>((placement.position.tile_x - origin_.tile_x) * formats::route_tile_size + placement.position.x - origin_.x),
                static_cast<float>(placement.position.y - origin_.y),
                static_cast<float>((placement.position.tile_z - origin_.tile_z) * formats::route_tile_size + placement.position.z - origin_.z)};
            const float d = glm::distance(local, eye_);
            if (d < best_distance) {
                best_distance = d;
                best = h;
            }
        }
        if (best) {
            const auto& head = heads[*best];
            ImGui::Text("Head %u, %.0f m away: %s, item %u, node %u at %.1f m %s", *best, best_distance,
                        head.type ? head.type->display_name.c_str() : "?", head.track_item, head.node, head.offset,
                        head.forward ? "forward" : "backward");
            ImGui::Text("Aspect %s, draw state %.0f, speed %.1f, mode %.0f", aspect_name(head.state), head.draw_state,
                        head.speed, head.mode);
            ImGui::Text("block_state %d, route_set %d%s", system_->block_state(*best), system_->route_set(*best),
                        system_->reads_stale(*best) ? ", reads a stale local" : "");
            for (const auto& [name, value] : system_->locals(*best)) ImGui::Text("  local %s = %g", name.c_str(), value);
        }
        ImGui::TreePop();
    }
#endif
}

} // namespace openrts::viewer
#endif
