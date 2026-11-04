#include <openrts/trainplugin/legacy_consist.hpp>

#include <algorithm>
#include <optional>

#include <openrts/formats/cab_description.hpp>
#include <openrts/formats/shape_binary.hpp>
#include <openrts/formats/shape_file.hpp>

namespace openrts::trainplugin {

using namespace dllhost;

namespace {

std::u16string u16(std::string_view s) { return utf8_to_utf16(s); }

// A screen sub-object for the host: RTS CloseUpSubObject's Pt by type (0x519B8C).
dllhost::ScreenObjectSpec screen_object_spec(const formats::ScreenObject& o) {
    dllhost::ScreenObjectSpec s;
    s.type = static_cast<std::uint32_t>(o.type);
    s.flags = o.flags;
    s.initial_state = o.initial_state;
    s.parent = o.parent;
    s.frames = static_cast<std::uint32_t>(o.frames.size());
    s.textures = static_cast<std::uint32_t>(o.textures.size());
    s.shader = o.shader;
    s.colour = o.colour;
    for (int k = 0; k < 4; ++k) {
        s.area[k] = o.area[k];
        s.uv[k] = o.uv[k];
    }
    s.anim_time = o.anim_time;
    s.fade = o.fade;
    s.blink = o.blink;
    using T = formats::ScreenObjectType;
    switch (o.type) {
    case T::arrow:
        s.pt[0] = o.center[0];
        s.pt[1] = o.center[1];
        for (int k = 0; k < 4; ++k) s.pt[2 + k] = o.range[k];
        break;
    case T::link:
        s.pt[0] = static_cast<float>(o.link);
        break;
    case T::label:
        if (o.char_width[1] > 0.0f && o.max_length > 0) {
            s.text_size = o.max_length + 1u;
            s.pt[1] = static_cast<float>(s.text_size);
        }
        break;
    case T::bar:
        s.pt[0] = static_cast<float>(o.bar_style);
        for (int k = 0; k < 4; ++k) s.pt[2 + k] = o.range[k];
        break;
    case T::grid:
        s.pt[0] = static_cast<float>(o.grid_rows[0]);
        s.pt[1] = static_cast<float>(o.grid_rows[1]);
        s.pt[2] = static_cast<float>(o.grid_columns[0]);
        s.pt[3] = static_cast<float>(o.grid_columns[1]);
        break;
    case T::sector:
        for (int k = 0; k < 4; ++k) s.pt[2 + k] = o.range[k];
        s.pt[6] = static_cast<float>(o.sector_shape + 10u * o.sector_fill);
        s.pt[7] = o.sector_granularity;
        break;
    case T::frame:
        break;
    }
    // RTS: a value above the range's minimum starts at the minimum.
    if (o.type == T::arrow || o.type == T::bar || o.type == T::sector) s.value = (std::min)(0.0f, o.range[0]);
    return s;
}

std::vector<Parameter> parameters(const std::vector<formats::EngineParameter>& list) {
    std::vector<Parameter> out;
    for (const auto& p : list) out.push_back({u16(p.name), p.value, u16(p.text)});
    return out;
}

std::uint16_t aux_event(const std::string& name) {
    if (name == "run") return aux_run;
    if (name == "alsn") return aux_alsn;
    if (name == "speedlimit") return aux_speed_limit;
    if (name == "switched") return aux_switched;
    if (name == "canswitch") return aux_can_switch;
    return 0;
}

// A DLL named in an .eng, as RTS looks for it (0x56D9C8): next to the .eng,
// then under TRAINS/TRAINSET and GLOBAL/LIB. Returns the real file.
std::optional<std::filesystem::path> find_module(const core::Vfs& vfs, const std::filesystem::path& cache_root,
                                                 const core::VirtualPath& file, const std::string& module) {
    std::vector<std::optional<core::VirtualPath>> candidates{
        file.parent().resolve(module),
        core::VirtualPath::parse("TRAINS/TRAINSET/" + module),
        core::VirtualPath::parse("GLOBAL/LIB/" + module),
    };
    for (const auto& c : candidates) {
        if (c && vfs.exists(*c)) return vfs.real_path(*c, cache_root);
    }
    return std::nullopt;
}

using formats::shape_description;

// The Shortcut frames of a cab's animation nodes as positions of the
// switches with those names. RTS (0x4B30DC): a position's frame is the
// node's first key frame + position x (its key span / (States - 1)), whole
// frames per position.
void add_shortcuts(const core::Vfs& vfs, const core::VirtualPath& cabin, const formats::CabDescription& cab,
                   std::vector<SwitchSpec>& switches) {
    if (cab.animation_nodes.empty()) return;
    core::VirtualPath shape = cabin;
    if (core::fold_case(cabin.extension()) == ".repaint") {
        const auto text = vfs.read(cabin);
        const auto repaint = text ? formats::parse_repaint(*text) : formats::Repaint{};
        const auto target = repaint.ok() ? cabin.parent().resolve(repaint.shape) : std::nullopt;
        if (!target) return;
        shape = *target;
    }
    const auto body = formats::load_binary_shape_body(vfs, shape);
    if (!body.ok()) return;
    const auto model = formats::parse_binary_shape_model(*body.body);
    if (!model.ok() || model.animations.empty()) return;
    for (const auto& node : cab.animation_nodes) {
        // The node's keys: first frame and span.
        std::optional<std::uint32_t> first, last;
        for (const auto& animation : model.animations) {
            for (const auto& track : animation.nodes) {
                if (core::fold_case(track.name) != core::fold_case(node.name)) continue;
                const auto see = [&](const std::uint32_t frame) {
                    first = first ? (std::min)(*first, frame) : frame;
                    last = last ? (std::max)(*last, frame) : frame;
                };
                for (const auto& key : track.positions) see(key.frame);
                for (const auto& key : track.rotations) see(key.frame);
            }
        }
        if (!first || !last) continue;
        for (auto& sw : switches) {
            if (core::fold_case(utf16_to_utf8(sw.name)) != core::fold_case(node.name) || sw.num_states < 2) continue;
            const std::uint32_t per_position = (*last - *first) / (sw.num_states - 1);
            if (per_position == 0) continue;
            const float key_out = (sw.flags & 2) != 0 ? 1.0f : 0.0f;
            for (const auto& frames : node.shortcuts) {
                std::vector<float> set;
                for (const auto frame : frames) {
                    set.push_back((static_cast<float>(frame) - static_cast<float>(*first)) / static_cast<float>(per_position) + key_out);
                }
                sw.shortcuts.push_back(std::move(set));
            }
        }
    }
}

class LegacyController final : public simulation::LocomotiveController {
public:
    LegacyController(std::weak_ptr<const LegacyConsist::Outputs> outputs, std::size_t car)
        : outputs_(std::move(outputs)), car_(car) {}
    void step(double, const simulation::EngineInputs&, simulation::EngineDemands& demands) override {
        const auto outputs = outputs_.lock();
        if (!outputs || !outputs->valid || car_ >= outputs->cars.size() || !outputs->cars[car_].has_engine) {
            demands = {};
            return;
        }
        apply_engine_output(outputs->cars[car_].engine, demands);
    }
    [[nodiscard]] bool rts_brakes() const override { return true; }

private:
    std::weak_ptr<const LegacyConsist::Outputs> outputs_;
    std::size_t car_;
};

} // namespace

void apply_engine_output(const EngineOutput& out, simulation::EngineDemands& demands) {
    demands.force = out.force;
    demands.brake_force = out.brake_force;
    demands.train_pipe_rate = out.train_pipe_rate;
    demands.independent_brake = out.independent_brake;
    demands.main_reservoir_rate = out.main_reservoir_rate;
    demands.charging_rate = out.charging_rate;
    demands.ep_value = out.ep_value;
    demands.handbrake = out.handbrake_percent;
    demands.systems = out.systems;
    demands.release_train_brake = (out.engine_flags & 1) != 0;
    demands.release_independent_brake = (out.engine_flags & 2) != 0;
}

std::unique_ptr<LegacyConsist> LegacyConsist::create(const core::Vfs& vfs, const std::filesystem::path& cache_root,
                                                     std::vector<LegacyCar> cars,
                                                     const std::filesystem::path& rts_root, std::string* error,
                                                     const std::filesystem::path& host_executable) {
    std::unique_ptr<LegacyConsist> consist(new LegacyConsist());
    SetupRequest setup;
    setup.rts_root = rts_root.u16string();
    setup.train_type = 1;
    bool any = false;
    std::string problems;
    consist->cabs_.resize(cars.size());
    consist->dll_.assign(cars.size(), false);
    for (std::size_t i = 0; i < cars.size(); ++i) {
        const auto& car = cars[i];
        CarSpec spec;
        spec.uid = car.uid;
        if (!car.definition) {
            setup.cars.push_back(std::move(spec));
            continue;
        }
        const auto& d = *car.definition;
        spec.mass = d.mass;
        spec.name = u16(d.name);
        if (d.engine) {
            const auto& e = *d.engine;
            spec.kind = e.kind == formats::EngineKind::electric ? car_electric
                      : e.kind == formats::EngineKind::diesel   ? car_diesel
                                                                : car_wagon;
            spec.series = u16(e.series);
            spec.lib_param = e.library_param;
            spec.stack_size = e.variable_stack;
            spec.num_engines = e.num_engines;
            spec.num_axles = e.num_axles;
            spec.parameters = parameters(e.parameters);
            if (!e.library_module.empty() && spec.kind != car_wagon) {
                if (const auto path = find_module(vfs, cache_root, car.file, e.library_module)) {
                    spec.module = path->u16string();
                    consist->dll_[i] = true;
                    any = true;
                } else {
                    problems += "DLL not found: " + e.library_module + " (" + d.name + ")\n";
                }
            }
            for (const auto& a : e.aux_libraries) {
                AuxLibrarySpec aux;
                aux.name = u16(a.name);
                aux.stack_size = a.variable_stack;
                aux.parameters = parameters(a.parameters);
                aux.has_sound = !a.sound.empty();
                for (const auto& [fn, order] : a.calls)
                    if (const auto ev = aux_event(fn)) aux.calls.push_back({ev, static_cast<std::int16_t>(order)});
                if (const auto path = find_module(vfs, cache_root, car.file, a.module)) {
                    aux.module = path->u16string();
                    spec.aux.push_back(std::move(aux));
                } else {
                    problems += "aux library not found: " + a.module + " (" + d.name + ")\n";
                }
            }
            // The cab: the .sd of the EngExtended Cabin shape (through a
            // .repaint file when the cab is a repaint of another one).
            if (!e.cabin.empty()) {
                std::optional<core::VirtualPath> sd;
                const auto cabin = car.file.parent().resolve(e.cabin);
                if (cabin) sd = shape_description(vfs, *cabin);
                if (sd) {
                    if (const auto bytes = vfs.read(*sd)) {
                        auto cab = formats::parse_cab_description(*bytes);
                        if (cab.ok()) {
                            CabSpec& c = spec.cab;
                            c.present = true;
                            for (const auto& s : cab.switches) {
                                SwitchSpec ss;
                                ss.id = s.id;
                                ss.num_states = s.num_states;
                                ss.initial_state = s.initial_state;
                                ss.fixed_state = s.fixed_state;
                                ss.flags = s.flags;
                                ss.anim_time = s.anim_time;
                                ss.name = u16(s.name);
                                ss.precision[0] = s.precision[0];
                                ss.precision[1] = s.precision[1];
                                c.switches.push_back(std::move(ss));
                            }
                            add_shortcuts(vfs, *cabin, cab, c.switches);
                            for (const auto& dsp : cab.displays) {
                                DisplaySpec ds;
                                ds.id = dsp.id;
                                std::copy(dsp.range.begin(), dsp.range.end(), ds.range);
                                ds.granularity = dsp.granularity;
                                ds.flags = dsp.flags;
                                ds.name = u16(dsp.name);
                                ds.initial_state = dsp.initial_state;
                                if (dsp.screen.present) {
                                    const auto& screen = dsp.screen;
                                    ds.has_screen = true;
                                    for (int k = 0; k < 3; ++k) {
                                        ds.corners[k] = screen.top_left[k];
                                        ds.corners[3 + k] = screen.top_right[k];
                                        ds.corners[6 + k] = screen.bottom_right[k];
                                    }
                                    ds.reference_screen = screen.reference_screen;
                                    for (const auto& o : screen.objects) ds.screen.push_back(screen_object_spec(o));
                                }
                                c.displays.push_back(std::move(ds));
                            }
                            c.internal_lights = static_cast<std::uint32_t>(cab.lights.size());
                            c.tex_changes = static_cast<std::uint32_t>(cab.texture_changes.size());
                            c.doors = cab.interior_doors;
                            for (const auto& door : cab.doors) {
                                c.door_names.push_back(u16(door.name));
                                c.door_groups.push_back(door.global_id);
                            }
                            c.signal_lights = static_cast<std::uint32_t>(cab.signalization.lights.size());
                            c.signal_aspects = static_cast<std::uint32_t>(cab.signalization.aspects.size());
                            c.signal_scripted = cab.signalization.scripted;
                            c.switch_count = cab.switch_count;
                            for (const auto& lamp : cab.lights) c.internal_light_groups.push_back(lamp.global_id);
                            for (const auto& change : cab.texture_changes) {
                                c.tex_change_images.push_back(change.texture);
                                c.tex_change_groups.push_back(change.values[1]);
                            }
                            c.display_count = cab.display_count;
                            for (const auto& a : cab.animations) c.animations.push_back(u16(a.name));
                            consist->cabs_[i] = std::move(cab);
                        } else {
                            problems += "cab " + std::string(sd->str()) + ": " + cab.error + "\n";
                        }
                    }
                }
            }
        }
        // Exterior lights by LightID: a DLL switches them (SwitchLight).
        spec.lights = static_cast<std::uint32_t>(d.lights.size());
        for (const auto& light : d.lights) {
            dllhost::LightSpec l;
            l.type = light.type;
            l.fade_in = light.fade_in;
            l.fade_out = light.fade_out;
            l.spot_exponent = light.spot_exponent;
            if (!light.states.empty()) {
                const auto& s = light.states.front();
                l.colour = s.colour;
                l.radius = s.radius;
                for (int k = 0; k < 3; ++k) l.position[k] = s.position[k];
            }
            spec.light_specs.push_back(l);
        }
        // The car's own shape: free animations and doors a DLL looks up by name.
        if (!d.shape.empty()) {
            if (const auto shape = car.file.parent().resolve(d.shape)) {
                if (const auto bytes = vfs.read(shape_description(vfs, *shape))) {
                    const auto ext = formats::parse_exterior_details(*bytes);
                    for (const auto& a : ext.animations) spec.animations.push_back(u16(a.name));
                    spec.doors = static_cast<std::uint32_t>(ext.doors.size());
                    for (const auto& door : ext.door_details) {
                        spec.door_times.push_back(door.anim_time);
                        spec.door_groups.push_back(door.global_id);
                    }
                }
            }
        }
        setup.cars.push_back(std::move(spec));
    }
    if (!any) {
        if (error) *error = problems.empty() ? "no DLL in the consist" : problems;
        return nullptr;
    }
    std::string why;
    consist->host_ = LegacyHost::start(&why, host_executable);
    if (!consist->host_) {
        if (error) *error = why;
        return nullptr;
    }
    if (!consist->host_->setup(setup, consist->modules_)) {
        if (error) *error = "dllhost setup: " + consist->host_->error() + "\n" + consist->host_->take_messages();
        return nullptr;
    }
    consist->messages_ = problems + consist->host_->take_messages();
    consist->cars_ = std::move(cars);
    return consist;
}

LegacyConsist::~LegacyConsist() {
    outputs_->valid = false;
    outputs_->cars.clear();
}

std::vector<CarInput> LegacyConsist::inputs(const simulation::Train& train, const LegacyWorld* world) const {
    const auto& cars = train.cars();
    std::vector<CarInput> out(cars_.size());
    const std::size_t n = (std::min)(cars.size(), cars_.size());
    // Multiple-unit cables (Flags byte 3, 0x10000 towards the train front,
    // 0x20000 towards the rear): connected where both touching ends have one.
    auto cable_at = [&](std::size_t i, bool train_front) -> bool {
        const auto* d = cars[i].definition;
        if (!d || !d->engine) return false;
        const auto& e = *d->engine;
        // A flipped car's own front faces the train rear.
        const bool own_front = train_front != cars[i].flipped;
        return e.sme_cable[own_front ? 0 : 1] != 0;
    };
    for (std::size_t i = 0; i < n; ++i) {
        const auto& c = cars[i];
        CarInput& in = out[i];
        std::uint32_t flags = c.flipped ? 1u : 0u;
        flags |= static_cast<std::uint32_t>(c.pipe_ends) << 8;
        if (i > 0 && cable_at(i, true) && cable_at(i - 1, false)) flags |= 0x10000;
        if (i + 1 < n && cable_at(i, false) && cable_at(i + 1, true)) flags |= 0x20000;
        in.flags = flags;
        in.coupling = c.coupler_open;
        in.mass = c.definition ? c.definition->mass : 0.0f;
        in.velocity = c.velocity;
        in.acceleration = c.acceleration;
        in.resistance = c.resistance;
        in.slope = c.grade;
        in.zord = c.offset;
        in.brake_force = c.brake_force;
        in.brake_cylinder = c.cylinder;
        in.independent = c.independent;
        in.auxiliary = c.auxiliary;
        in.control = c.control;
        in.train_pipe = c.pipe;
        in.charging_pipe = c.charging_pipe;
        in.handbrake = c.handbrake;
        in.main_reservoir = c.main_reservoir;
        if (c.definition && c.definition->engine && c.definition->engine->kind == formats::EngineKind::electric) {
            // The route's line (Electrification::apply) when modelled, else the world's.
            if (c.line_voltage >= 0.0f) {
                in.line_voltage = c.line_voltage;
                in.line_frequency = c.line_frequency;
            } else if (world) {
                in.line_voltage = world->line_voltage;
                in.line_frequency = world->line_frequency;
            }
        }
    }
    return out;
}

bool LegacyConsist::enter(std::size_t car, const simulation::Train& train, bool running, int state) {
    if (!host_ || !host_->alive()) return false;
    EnterRequest request;
    request.car = static_cast<std::uint32_t>(car);
    request.running = running;
    request.state = state;
    LegacyWorld world;
    request.cars = inputs(train, &world);
    StepReply reply;
    const bool ok = host_->enter(request, reply);
    messages_ += host_->take_messages();
    if (!ok) return false;
    player_ = car;
    outputs_->cars = std::move(reply.cars);
    outputs_->valid = true;
    return true;
}

bool LegacyConsist::step(double dt, const simulation::Train& train, const LegacyWorld& world) {
    if (!host_ || !host_->alive()) {
        outputs_->valid = false;
        return false;
    }
    StepRequest request;
    request.day_time = world.day_time;
    request.dt = static_cast<float>(dt);
    request.air_temperature = world.air_temperature;
    request.night = world.night ? 1 : 0;
    request.train_direction = 1;
    request.cars = inputs(train, &world);
    request.route = world.route;
    request.signal = world.signal;
    request.event = world.event;
    request.signals_ahead = world.signals_ahead;
    StepReply reply;
    const bool ok = host_->step(request, reply);
    messages_ += host_->take_messages();
    if (!ok) {
        outputs_->valid = false;
        return false;
    }
    outputs_->cars = std::move(reply.cars);
    outputs_->valid = true;
    return true;
}

bool LegacyConsist::operate(std::size_t car, std::uint32_t id, std::uint32_t state) {
    if (!host_ || !host_->alive()) return false;
    bool allowed = false;
    const bool ok = host_->can_switch({static_cast<std::uint32_t>(car), id, state}, allowed);
    messages_ += host_->take_messages();
    return ok && allowed;
}

bool LegacyConsist::drag(std::size_t car, std::uint32_t id, float delta, bool release) {
    if (!host_ || !host_->alive()) return false;
    bool moved = false;
    SwitchState state;
    const bool ok = host_->drag_switch({static_cast<std::uint32_t>(car), id, delta, release ? 1u : 0u}, moved, state);
    messages_ += host_->take_messages();
    // The cab shows the dragged switch at once (RTS moves its node in the
    // mouse handler), not at the next 50 ms step.
    if (ok && outputs_->valid && car < outputs_->cars.size() && id < outputs_->cars[car].switches.size())
        outputs_->cars[car].switches[id] = state;
    return ok && moved;
}

const CarOutput* LegacyConsist::output(std::size_t car) const {
    return outputs_->valid && car < outputs_->cars.size() ? &outputs_->cars[car] : nullptr;
}

const formats::CabDescription* LegacyConsist::cab(std::size_t car) const {
    return car < cabs_.size() && !cabs_[car].switches.empty() ? &cabs_[car] : nullptr;
}

bool LegacyConsist::has_dll(std::size_t car) const { return car < dll_.size() && dll_[car]; }

bool LegacyConsist::alive() const { return host_ && host_->alive(); }

std::string LegacyConsist::error() const { return host_ ? host_->error() : std::string("no host"); }

std::string LegacyConsist::take_messages() { return std::exchange(messages_, {}); }

std::unique_ptr<simulation::LocomotiveController> LegacyConsist::controller(std::size_t car) const {
    return std::make_unique<LegacyController>(outputs_, car);
}

} // namespace openrts::trainplugin
