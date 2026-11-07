#include <openrts/trainplugin/legacy_consist.hpp>

#include <openrts/core/profile.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>

#include <openrts/formats/cab_description.hpp>
#include <openrts/formats/sound.hpp>
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
    demands.sets_handbrake = true; // RTS: HandbrakeValue = HandbrakePercent / 100, negative 0
    demands.systems = out.systems;
    demands.release_train_brake = (out.engine_flags & 1) != 0;
    demands.release_independent_brake = (out.engine_flags & 2) != 0;
    demands.sanding = out.sanding != 0;
    // RTS 0x503CE4: the line current comes from ElectricEngine::PowerConsuming.
    demands.power = out.power_consuming;
}

namespace {

// The sound scripts' tables for the host (SoundTableSpec): NamedTriggersTable
// and the On codes of the trigger pairs of the group the player's train
// plays (group 7, else the first). One per file.
struct SoundTables {
    std::vector<SoundTableSpec>& out;
    std::map<std::string, std::int32_t>& by_path; // kept by the consist across couplings
    std::int32_t base = 0;                        // tables the host has already

    std::int32_t add(const core::Vfs& vfs, const std::vector<std::optional<core::VirtualPath>>& candidates) {
        for (const auto& path : candidates) {
            if (!path || !vfs.exists(*path)) continue;
            const std::string key(path->str());
            if (const auto it = by_path.find(key); it != by_path.end()) return it->second;
            const auto script = formats::load_sound_script(vfs, *path);
            SoundTableSpec t;
            if (script.ok()) {
                for (const auto& [name, code] : script.named_triggers) {
                    t.names.push_back(u16(name));
                    t.codes.push_back(static_cast<std::uint16_t>(code));
                }
                if (const auto* group = script.select_group(formats::SoundOwner::player_train)) {
                    for (const auto& pair : group->trigger_pairs) {
                        std::vector<std::uint16_t> on;
                        for (const auto c : pair.on) on.push_back(static_cast<std::uint16_t>(c));
                        t.pair_on.push_back(std::move(on));
                    }
                }
            }
            if (t.names.empty() && t.pair_on.empty()) return by_path[key] = -1;
            out.push_back(std::move(t));
            return by_path[key] = base + static_cast<std::int32_t>(out.size() - 1);
        }
        return -1;
    }
};

struct BuiltCar {
    CarSpec spec;
    formats::CabDescription cab;
    bool dll = false;
    std::vector<std::int16_t> aux_index; // each AuxLibrary of the .eng as the host numbers them, -1 missing
};

std::optional<core::VirtualPath> in(const core::VirtualPath& folder, const std::string& file) {
    if (file.empty()) return std::nullopt;
    return folder.resolve(file);
}

// RTS's TrackItem and its kind's fields (0x482680) from the TDB item.
TrackItemSpec item_spec(const formats::TrackItem& item, const float route_speed_limit) {
    using formats::TrackItemKind;
    TrackItemSpec s;
    s.id = item.id;
    switch (item.kind) {
    case TrackItemKind::signal: s.type = 1; break;
    case TrackItemKind::speed_post: s.type = 2; break;
    case TrackItemKind::platform: s.type = 3; break;
    case TrackItemKind::siding: s.type = 4; break;
    case TrackItemKind::level_crossing: s.type = 5; break;
    case TrackItemKind::crossover: s.type = 6; break;
    case TrackItemKind::pickup: s.type = 7; break;
    case TrackItemKind::car_spawner: s.type = 8; break;
    default: s.type = 0; break;
    }
    s.node_offset = item.distance;
    s.data_flags = item.data_flags;
    // RTS axes: x negated.
    if (item.has_pdata) {
        s.pdata[0] = -item.pdata[0];
        s.pdata[1] = item.pdata[1];
        s.ptile[0] = static_cast<std::int16_t>(item.pdata_tile[0]);
        s.ptile[1] = static_cast<std::int16_t>(item.pdata_tile[1]);
    }
    if (item.has_rdata) {
        s.rdata[0] = -item.rdata[0];
        s.rdata[1] = item.rdata[1];
        s.rdata[2] = item.rdata[2];
        s.rtile[0] = static_cast<std::int16_t>(item.rdata_tile[0]);
        s.rtile[1] = static_cast<std::int16_t>(item.rdata_tile[1]);
    }
    switch (s.type) {
    case 2: {
        // SpeedpostTrItemData as 0x482680 reads it: flags; with flag 2 the
        // limit (flag 1: the route's, as it is) and with 0x10 a number;
        // without it a speed with flag 8, then the post's number (KM); the
        // sign's rotation (Off) last. km/h, mph with 0x100, to m/s unless
        // flag 1.
        const auto& v = item.speed_post_data;
        std::size_t i = 0;
        const auto next = [&] { return i < v.size() ? v[i++] : 0.0f; };
        const auto flags = static_cast<std::uint32_t>(static_cast<std::int32_t>(next()));
        s.flags = flags;
        if (flags & 2u) {
            s.value = (flags & 1u) ? route_speed_limit : next();
            if (flags & 0x10u) s.number = static_cast<std::int32_t>(next());
        } else {
            if (flags & 8u) s.value = next();
            s.km = next();
        }
        s.off = next();
        s.value = std::abs(s.value);
        if (!(flags & 1u)) s.value *= (flags & 0x100u) ? 0.4469444444f : 1.0f / 3.6f;
        break;
    }
    case 3:
        s.flags = item.item_flags;
        s.link = item.linked;
        s.name = u16(item.name);
        s.station = u16(item.station);
        s.min_wait = item.min_wait;
        s.passengers = item.passengers;
        break;
    case 4:
        s.flags = item.item_flags;
        s.link = item.linked;
        s.name = u16(item.name);
        break;
    case 6:
        s.link = item.crossover[0];
        s.section = item.crossover[1];
        break;
    case 7:
        s.value = item.pickup_amount;
        s.flags = item.pickup_flags;
        break;
    default:
        break;
    }
    return s;
}

// What the host needs of one car (CarSpec), with its cab and whether a DLL
// runs it. `tables` collects the sound scripts' tables.
BuiltCar build_spec(const core::Vfs& vfs, const std::filesystem::path& cache_root, const LegacyCar& car,
                    const core::VirtualPath* route, SoundTables& tables, std::string& problems) {
    BuiltCar out;
    CarSpec& spec = out.spec;
    spec.uid = car.uid;
    spec.key = car.key;
    if (!car.definition) return out;
    const auto global_sound = core::VirtualPath::parse("SOUND");
    const auto& d = *car.definition;
    spec.mass = d.mass;
    spec.name = u16(d.name);
    // The exterior and cab scripts, looked up as the viewer does
    // (TrainView::update_sound / update_cab_sound).
    const auto folder = car.file.parent();
    const auto sound_folder = folder.resolve("SOUND");
    if (!d.sound.empty())
        spec.sound_ext = tables.add(vfs, {sound_folder ? in(*sound_folder, d.sound) : std::nullopt,
                                          global_sound ? in(*global_sound, d.sound) : std::nullopt});
    if (!d.cab_sound.empty())
        spec.sound_cab = tables.add(vfs, {sound_folder ? in(*sound_folder, d.cab_sound) : std::nullopt, in(folder, d.cab_sound),
                                          global_sound ? in(*global_sound, d.cab_sound) : std::nullopt});
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
                out.dll = true;
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
            if (aux.has_sound) {
                // TrainView::update_aux_sounds: the route's SOUND first
                // for ROUTEFORWAGON, then next to the .eng.
                std::optional<core::VirtualPath> route_file;
                if (route && a.sound_type == formats::AuxSoundType::route_for_wagon)
                    if (const auto f = route->resolve("SOUND")) route_file = in(*f, a.sound);
                aux.sound_table = tables.add(vfs, {route_file, in(folder, a.sound)});
            }
            for (const auto& [fn, order] : a.calls)
                if (const auto ev = aux_event(fn)) aux.calls.push_back({ev, static_cast<std::int16_t>(order)});
            if (const auto path = find_module(vfs, cache_root, car.file, a.module)) {
                aux.module = path->u16string();
                out.aux_index.push_back(static_cast<std::int16_t>(spec.aux.size()));
                spec.aux.push_back(std::move(aux));
            } else {
                out.aux_index.push_back(-1);
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
                        out.cab = std::move(cab);
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
    return out;
}

} // namespace

std::unique_ptr<LegacyConsist> LegacyConsist::create(const core::Vfs& vfs, const std::filesystem::path& cache_root,
                                                     std::vector<LegacyCar> cars,
                                                     const std::filesystem::path& rts_root, std::string* error,
                                                     const std::filesystem::path& host_executable,
                                                     const core::VirtualPath* route) {
    std::unique_ptr<LegacyConsist> consist(new LegacyConsist());
    SetupRequest setup;
    setup.rts_root = rts_root.u16string();
    setup.train_type = 1;
    bool any = false;
    std::string problems;
    SoundTables tables{setup.sound_tables, consist->sound_paths_, 0};
    consist->cabs_.resize(cars.size());
    consist->dll_.assign(cars.size(), false);
    consist->aux_index_.resize(cars.size());
    for (std::size_t i = 0; i < cars.size(); ++i) {
        auto built = build_spec(vfs, cache_root, cars[i], route, tables, problems);
        consist->cabs_[i] = std::move(built.cab);
        consist->dll_[i] = built.dll;
        if (built.dll) any = true;
        consist->aux_index_[i] = std::move(built.aux_index);
        setup.cars.push_back(std::move(built.spec));
    }
    consist->sound_count_ = static_cast<std::int32_t>(setup.sound_tables.size());
    consist->train_first_ = {0, cars.size()};
    consist->cache_root_ = cache_root;
    if (route) consist->route_ = *route;
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

std::vector<CarInput> LegacyConsist::inputs(const std::vector<const simulation::Train*>& trains,
                                            const LegacyWorld* world) const {
    std::vector<CarInput> out(cars_.size());
    for (std::size_t t = 0; t < trains.size() && t < this->trains(); ++t) {
    if (!trains[t]) continue;
    const auto& cars = trains[t]->cars();
    const std::size_t n = (std::min)(cars.size(), train_first_[t + 1] - train_first_[t]);
    for (std::size_t i = 0; i < n; ++i) {
        const auto& c = cars[i];
        CarInput& in = out[train_first_[t] + i];
        std::uint32_t flags = c.flipped ? 1u : 0u;
        flags |= static_cast<std::uint32_t>(c.pipe_ends) << 8;
        // Byte 3, WagonData::Cables: SMECable 0x10000 front / 0x20000 rear,
        // HVBus 0x40000 / 0x80000 (CarState::cables, the car menu's); byte 4
        // bits 1, 2: the bogies fastened (brake shoes).
        flags |= static_cast<std::uint32_t>(c.cables & 0x0Fu) << 16;
        flags |= static_cast<std::uint32_t>(c.chocks & 0x03u) << 24;
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
                in.line_current = c.line_current;
                in.line_burnt = c.line_burnt ? 1 : 0;
            } else if (world) {
                in.line_voltage = world->line_voltage;
                in.line_frequency = world->line_frequency;
            }
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

bool LegacyConsist::step(double dt, const std::vector<const simulation::Train*>& trains, const LegacyWorld& world) {
    if (!host_ || !host_->alive()) {
        outputs_->valid = false;
        return false;
    }
    core::SequentialZone part;
    part.open("DLL: request");
    StepRequest request;
    request.day_time = world.day_time;
    request.dt = static_cast<float>(dt);
    request.air_temperature = world.air_temperature;
    request.night = world.night ? 1 : 0;
    request.train_direction = 1;
    request.cars = inputs(trains, &world);
    request.route = world.route;
    request.signal = world.signal;
    request.event = world.event;
    request.signals_ahead = world.signals_ahead;
    request.alsn_ahead = world.alsn_ahead;
    request.alsn_behind = world.alsn_behind;
    request.alsn_passing = world.alsn_passing;
    request.next_limit = world.next_limit;
    request.next_limit_distance = world.next_limit_distance;
    // What the host has not been sent yet: the items the list names and the
    // route database's signals the SignalInputs name.
    request.track_items = world.track_items;
    request.items_range = world.items_range;
    // The libraries' sounds by the host's numbering of the libraries.
    for (const auto& p : world.sound_pairs) {
        auto q = p;
        if (q.object >= 0) {
            const auto& map = q.car < aux_index_.size() ? aux_index_[q.car] : std::vector<std::int16_t>{};
            if (static_cast<std::size_t>(q.object) >= map.size() || map[static_cast<std::size_t>(q.object)] < 0) continue;
            q.object = map[static_cast<std::size_t>(q.object)];
        }
        request.sound_pairs.push_back(q);
    }
    if (world.tdb) {
        for (const auto& e : world.track_items) {
            if (e.item >= world.tdb->items.size() || !items_sent_.insert(e.item).second) continue;
            request.items.push_back(item_spec(world.tdb->items[e.item], world.route_speed_limit));
        }
    }
    if (world.route_database) {
        const auto& units = world.route_database->signal_units;
        for (const auto* list : {&request.signals_ahead, &request.alsn_ahead, &request.alsn_behind, &request.alsn_passing}) {
            for (const auto& s : *list) {
                if (s.unit < 0 || static_cast<std::size_t>(s.unit) >= units.size() || !units_sent_.insert(s.unit).second) continue;
                const auto& unit = units[static_cast<std::size_t>(s.unit)];
                SignalUnitSpec spec;
                spec.id = s.unit;
                spec.tile[0] = static_cast<std::int16_t>(unit.position.tile_x);
                spec.tile[1] = static_cast<std::int16_t>(unit.position.tile_z);
                spec.position[0] = static_cast<float>(-unit.position.x);
                spec.position[1] = static_cast<float>(unit.position.y);
                spec.position[2] = static_cast<float>(unit.position.z);
                spec.name = u16(unit.name);
                request.signal_units.push_back(std::move(spec));
            }
        }
    }
    part.close(nullptr);
    StepReply reply;
    const bool ok = host_->step(request, reply);
    messages_ += host_->take_messages();
    if (!ok) {
        outputs_->valid = false;
        return false;
    }
    outputs_->cars = std::move(reply.cars);
    outputs_->valid = true;
    items_wanted_ = reply.items_wanted;
    return true;
}

bool LegacyConsist::reconsist(const core::Vfs& vfs, std::vector<LegacyCar> cars, const simulation::Train& train) {
    if (player_ >= cars_.size()) return false;
    const auto player_key = cars_[player_].key;
    if (!player_key || std::none_of(cars.begin(), cars.end(), [&](const LegacyCar& c) { return c.key == player_key; })) return false;
    std::vector<std::vector<LegacyCar>> trains;
    trains.push_back(std::move(cars));
    return reconsist(vfs, std::move(trains), {&train});
}

bool LegacyConsist::reconsist(const core::Vfs& vfs, std::vector<std::vector<LegacyCar>> trains,
                              const std::vector<const simulation::Train*>& sims) {
    if (!host_ || !host_->alive() || trains.empty()) return false;
    const auto player_key = player_ < cars_.size() ? cars_[player_].key : 0u;
    std::unordered_map<std::uint32_t, std::size_t> before;
    for (std::size_t i = 0; i < cars_.size(); ++i)
        if (cars_[i].key) before.emplace(cars_[i].key, i);
    ReconsistRequest request;
    std::string problems;
    SoundTables tables{request.sound_tables, sound_paths_, sound_count_};
    std::vector<LegacyCar> cars;
    std::vector<std::size_t> first{0};
    for (auto& train : trains) {
        for (auto& car : train) cars.push_back(std::move(car));
        first.push_back(cars.size());
    }
    std::size_t player = SIZE_MAX;
    for (std::size_t i = 0; i < first[1] && player_key; ++i)
        if (cars[i].key == player_key) player = i;
    std::vector<formats::CabDescription> cabs(cars.size());
    std::vector<bool> dll(cars.size(), false);
    std::vector<std::vector<std::int16_t>> aux(cars.size());
    std::vector<bool> kept(cars_.size(), false);
    request.trains.resize(trains.size());
    for (std::size_t t = 0; t < trains.size(); ++t)
        request.trains[t].type = static_cast<std::uint8_t>(t < train_types_.size() ? train_types_[t] : t == 0 ? 1u : 3u); // the player's, LOOSE
    for (std::size_t i = 0; i < cars.size(); ++i) {
        const auto t = static_cast<std::size_t>(std::upper_bound(first.begin(), first.end(), i) - first.begin()) - 1;
        ReconsistCar entry;
        entry.spec.uid = cars[i].uid;
        entry.spec.key = cars[i].key;
        if (const auto it = before.find(cars[i].key); it != before.end() && !kept[it->second]) {
            const auto from = it->second;
            kept[from] = true;
            entry.from = static_cast<std::int32_t>(from);
            cabs[i] = std::move(cabs_[from]);
            dll[i] = dll_[from];
            aux[i] = std::move(aux_index_[from]);
        } else if (const auto p = parked_.find(cars[i].key); p != parked_.end()) {
            entry.from = reconsist_parked;
            cabs[i] = std::move(p->second.cab);
            dll[i] = p->second.dll;
            aux[i] = std::move(p->second.aux_index);
            if (dll[i]) ++restored_;
            parked_.erase(p);
        } else {
            auto built = build_spec(vfs, cache_root_, cars[i], route_ ? &*route_ : nullptr, tables, problems);
            entry.spec = std::move(built.spec);
            cabs[i] = std::move(built.cab);
            dll[i] = built.dll;
            aux[i] = std::move(built.aux_index);
        }
        request.trains[t].cars.push_back(std::move(entry));
    }
    // What left waits, as the host keeps its mirror.
    for (std::size_t i = 0; i < cars_.size(); ++i) {
        if (kept[i] || !cars_[i].key) continue;
        parked_[cars_[i].key] = Parked{cars_[i], std::move(cabs_[i]), dll_[i], std::move(aux_index_[i])};
    }
    sound_count_ += static_cast<std::int32_t>(request.sound_tables.size());
    cars_ = std::move(cars);
    cabs_ = std::move(cabs);
    dll_ = std::move(dll);
    aux_index_ = std::move(aux);
    train_first_ = std::move(first);
    request.running = !sims.empty() && sims[0] && std::abs(sims[0]->speed()) > 0.01;
    request.inputs = inputs(sims, nullptr);
    StepReply reply;
    const bool ok = host_->reconsist(request, reply);
    messages_ += problems + host_->take_messages();
    if (!ok) {
        outputs_->valid = false;
        return false;
    }
    player_ = player;
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

const CarOutput* LegacyConsist::output(std::size_t car) const { return output(0, car); }

const CarOutput* LegacyConsist::output(std::size_t train, std::size_t car) const {
    const auto at = flat(train, car);
    return outputs_->valid && at < outputs_->cars.size() ? &outputs_->cars[at] : nullptr;
}

const formats::CabDescription* LegacyConsist::cab(std::size_t car) const {
    const auto at = flat(0, car);
    return at < cabs_.size() && !cabs_[at].switches.empty() ? &cabs_[at] : nullptr;
}

bool LegacyConsist::has_dll(std::size_t car) const { return has_dll(0, car); }

bool LegacyConsist::has_dll(std::size_t train, std::size_t car) const {
    const auto at = flat(train, car);
    return at < dll_.size() && dll_[at];
}

bool LegacyConsist::alive() const { return host_ && host_->alive(); }

std::string LegacyConsist::error() const { return host_ ? host_->error() : std::string("no host"); }

std::string LegacyConsist::take_messages() { return std::exchange(messages_, {}); }

std::unique_ptr<simulation::LocomotiveController> LegacyConsist::controller(std::size_t car) const { return controller(0, car); }

std::unique_ptr<simulation::LocomotiveController> LegacyConsist::controller(std::size_t train, std::size_t car) const {
    return std::make_unique<LegacyController>(outputs_, flat(train, car));
}

} // namespace openrts::trainplugin
