#include <openrts/simulation/electrification.hpp>

#include <algorithm>
#include <cmath>

#include <openrts/simulation/track_network.hpp>
#include <openrts/simulation/train.hpp>

namespace openrts::simulation {
namespace {

// Without an .electrotype and WorksOnLineVoltages RTS reads a value it never
// set (0x503CE4); openRTS gives such an engine the commonest Russian DC line.
constexpr LineSupply fallback_supply{3000.0f, 0.0f};

[[nodiscard]] bool electric(const CarState& car) {
    return car.definition && car.definition->engine && car.definition->engine->kind == formats::EngineKind::electric;
}

} // namespace

Electrification::Electrification(formats::RouteElectrification data, const TrackNetwork& network)
    : data_(std::move(data)), network_(&network) {
    if (data_.has_database && !data_.database.sections.empty()) {
        mode_ = Mode::sections;
    } else if (data_.has_types && !data_.types.supply_types.empty()) {
        mode_ = Mode::single_type;
    } else if (data_.electrified) {
        mode_ = Mode::by_engine;
    }
    const auto& sections = data_.database.sections;
    current_.resize(sections.size());
    active_.resize(sections.size());
    for (std::size_t i = 0; i < sections.size(); ++i) {
        current_[i] = sections[i].current_source;
        for (const auto& source : sections[i].sources) active_[i].push_back(source.active);
        if (sections[i].id >= section_by_id_.size()) section_by_id_.resize(sections[i].id + 1, -1);
        section_by_id_[sections[i].id] = static_cast<int>(i);
    }
    for (const auto& node : data_.database.nodes) {
        // RTS keeps only vector nodes (0x490A14).
        const auto* n = network.node(node.node);
        if (!n || n->kind != formats::TrackNodeKind::vector) continue;
        if (node.node >= nodes_.size()) nodes_.resize(node.node + 1, nullptr);
        if (!nodes_[node.node]) nodes_[node.node] = &node;
    }
}

std::unique_ptr<Electrification> Electrification::load(const core::Vfs& vfs, const core::VirtualPath& route,
                                                        const TrackNetwork& network) {
    return std::make_unique<Electrification>(formats::load_route_electrification(vfs, route), network);
}

const formats::ElectricSubstation* Electrification::substation(const std::uint32_t id) const {
    for (const auto& s : data_.types.substations)
        if (s.id == id) return &s;
    return nullptr;
}

const formats::ElectricSupplyType* Electrification::type_of(const formats::ElectroPowerSource& source) const {
    const auto* sub = substation(source.substation);
    if (!sub) return nullptr;
    for (const auto& t : data_.types.supply_types)
        if (t.id == sub->electro_type) return &t;
    return nullptr;
}

float Electrification::source_frequency(const formats::ElectroPowerSource& source) const {
    const auto* type = type_of(source);
    return type ? type->frequency : 0.0f;
}

float Electrification::source_voltage(const formats::ElectroPowerSource& source, const TrackPoint& where) const {
    const auto* sub = substation(source.substation);
    if (!sub) return 0.0f;
    float base = sub->voltage;
    if (const auto* state = state_of(source.substation)) {
        if (state->off) return 0.0f;
        if (state->voltage >= 0.0f) base = state->voltage;
        // PROBABLE: phases count from 1 in the sources (Kuban's are all 27.5 kV).
        if (!state->phases.empty()) {
            const std::size_t k = source.phase >= 1 ? source.phase - 1u : 0u;
            base = state->phases[(std::min)(k, state->phases.size() - 1)];
        }
    }
    float voltage = source.feeder_q * 0.01f * base;
    const auto* type = type_of(source);
    // The drop per km from the substation (UseSubstationPos) or the feeder.
    const formats::RoutePosition* from = source.use_substation_position ? &sub->position
                                         : source.feeder                ? &*source.feeder
                                                                        : nullptr;
    if (voltage != 0.0f && from && type && type->voltage_drop != 0.0f) {
        const double dx = (where.tile_x - from->tile_x) * 2048.0 + where.x - from->x;
        const double dz = (where.tile_z - from->tile_z) * 2048.0 + where.z - from->z;
        const double km = std::sqrt(dx * dx + dz * dz) * 0.001;
        voltage = (std::max)(0.0f, voltage - static_cast<float>(km) * type->voltage_drop);
    }
    return voltage;
}

// Bits set in a pantograph mask.
[[nodiscard]] static unsigned raised_count(const std::uint8_t mask) {
    unsigned n = 0;
    for (std::uint8_t m = mask; m; m &= static_cast<std::uint8_t>(m - 1)) ++n;
    return n;
}

bool Electrification::wire_at(const formats::NodeElectrification& node, const double offset) const {
    const auto& pieces = node.catenary;
    if (pieces.size() == 1 && pieces[0].start <= 0.0f && pieces[0].length < 0.0f) return true;
    return std::any_of(pieces.begin(), pieces.end(), [&](const formats::CatenaryPiece& p) {
        return p.length > 0.0f && p.start <= offset && p.start + p.length >= offset;
    });
}

const formats::ElectricityPiece* Electrification::piece_at(const formats::NodeElectrification& node,
                                                           const double offset) const {
    // The last piece starting at or before the point (0x5B53A8 walks back).
    for (auto it = node.electricity.rbegin(); it != node.electricity.rend(); ++it) {
        if (it->start <= offset) return &*it;
    }
    return nullptr;
}

LinePoint Electrification::at(const std::uint32_t node, const double offset, const TrackPoint& where,
                              const formats::EngineDefinition* engine) const {
    LinePoint out;
    switch (own_voltage && mode_ != Mode::none ? Mode::by_engine : mode_) {
    case Mode::none:
        return out;
    case Mode::single_type: {
        const auto& t = data_.types.supply_types.front();
        out.state = LineState::live;
        out.supply = {t.voltage, t.frequency};
        return out;
    }
    case Mode::by_engine:
        out.state = LineState::live;
        if (engine && !engine->line_voltages.empty()) {
            const auto& r = engine->line_voltages.front();
            out.supply = {(r.min_voltage + r.max_voltage) * 0.5f, r.max_frequency};
        } else {
            out.supply = fallback_supply;
        }
        return out;
    case Mode::sections:
        break;
    }
    const auto* data = node < nodes_.size() ? nodes_[node] : nullptr;
    if (!data || !wire_at(*data, offset)) return out;
    out.state = LineState::dead;
    // The wire's BurnQ (0x5B53A8): type-1 catenary pieces over the point.
    out.burn_q = -1.0f;
    const auto burn_of = [&](const formats::CatenaryPiece& p) {
        for (const auto& t : data_.types.catenary_types) {
            if (t.id != p.type || t.type != 1) continue;
            if (out.burn_q >= 0.0f) {
                if (t.burn_q == 0.0f || (out.burn_q > 0.0f && t.burn_q > out.burn_q)) out.burn_q = t.burn_q;
            } else {
                out.burn_q = t.burn_q;
            }
        }
    };
    if (data->catenary.size() == 1 && data->catenary[0].start <= 0.0f && data->catenary[0].length < 0.0f) {
        burn_of(data->catenary[0]);
    } else {
        for (const auto& p : data->catenary)
            if (p.length > 0.0f && p.start <= offset && p.start + p.length >= offset) burn_of(p);
    }
    const auto* piece = piece_at(*data, offset);
    // The piece's own supply from the server (0x4645).
    int piece_source = -1;
    if (piece && !piece_supply_.empty()) {
        const auto k = static_cast<std::uint32_t>(piece - data->electricity.data());
        if (const auto it = piece_supply_.find(static_cast<std::uint64_t>(node) << 32 | k); it != piece_supply_.end()) {
            if (piece->section == 0) {
                // A dead piece fed from a neighbour: 1 the piece before, 2 the one after.
                if (it->second == 1 && k > 0) piece = &data->electricity[k - 1];
                if (it->second == 2 && k + 1 < data->electricity.size()) piece = &data->electricity[k + 1];
            } else if (it->second <= -2) {
                return out; // no source: dead
            } else {
                piece_source = it->second;
            }
        }
    }
    if (!piece || piece->section == 0 || piece->section >= section_by_id_.size() || section_by_id_[piece->section] < 0) {
        return out;
    }
    const auto index = static_cast<std::size_t>(section_by_id_[piece->section]);
    out.section = static_cast<int>(index);
    const auto& section = data_.database.sections[index];
    const auto& sources = section.sources;
    const auto& active = active_[index];
    const int current = current_[index];

    // An activity's automatic choice: the first source the engine works on.
    int chosen = -1;
    if (automatic_sources && sources.size() > 1 && current >= 0 && engine) {
        for (const auto& range : engine->line_voltages) {
            for (std::size_t k = 0; k < sources.size() && chosen < 0; ++k) {
                const auto* sub = substation(sources[k].substation);
                if (sub && range.contains(sub->voltage, source_frequency(sources[k]))) chosen = static_cast<int>(k);
            }
            if (chosen >= 0) break;
        }
        if (chosen >= 0) {
            out.state = LineState::live;
            out.source = chosen;
            out.supply = {source_voltage(sources[static_cast<std::size_t>(chosen)], where),
                          source_frequency(sources[static_cast<std::size_t>(chosen)])};
            if (out.supply.voltage <= 0.0f) out.state = LineState::dead;
            return out;
        }
    }
    if (piece_source >= 0 && piece_source < static_cast<int>(sources.size())) {
        const auto& source = sources[static_cast<std::size_t>(piece_source)];
        out.source = piece_source;
        out.supply = {source_voltage(source, where), source_frequency(source)};
        if (out.supply.voltage > 0.0f) out.state = LineState::live;
        return out;
    }
    if (sources.size() == 1) {
        chosen = 0;
    } else if (current >= 0) {
        chosen = current;
    }
    if (chosen >= 0) {
        // One source: it must be enabled and switched in.
        const auto flags = active[static_cast<std::size_t>(chosen)];
        if ((flags & 2u) == 0 || (flags & 1u) == 0) return out;
        const auto& source = sources[static_cast<std::size_t>(chosen)];
        out.source = chosen;
        out.supply = {source_voltage(source, where), source_frequency(source)};
        if (out.supply.voltage > 0.0f) out.state = LineState::live;
        return out;
    }
    // Several sources by their Active flags: the highest voltage; with two
    // feeding, the lower frequency of the two.
    int best = -1, second = -1;
    float best_v = 0.0f, second_v = 0.0f;
    for (std::size_t k = 0; k < sources.size(); ++k) {
        if ((active[k] & 3u) == 0) continue;
        const float v = source_voltage(sources[k], where);
        if (v <= 0.0f) continue;
        if (best < 0 || v > best_v) {
            second = best;
            second_v = best_v;
            best = static_cast<int>(k);
            best_v = v;
        } else if (second < 0 || v > second_v) {
            second = static_cast<int>(k);
            second_v = v;
        }
    }
    if (best < 0) return out;
    if (second < 0) {
        const auto flags = active[static_cast<std::size_t>(best)];
        if ((flags & 2u) == 0 || (flags & 1u) == 0) return out;
        out.supply = {best_v, source_frequency(sources[static_cast<std::size_t>(best)])};
    } else {
        out.supply = {best_v, (std::min)(source_frequency(sources[static_cast<std::size_t>(best)]),
                                         source_frequency(sources[static_cast<std::size_t>(second)]))};
    }
    out.source = best;
    out.state = LineState::live;
    return out;
}

LineSupply Electrification::apply(Train& train, const std::vector<std::uint8_t>& pantographs, const double dt) {
    const auto& cars = train.cars();
    const auto centres = train.car_centres();
    const std::size_t n = cars.size();
    const auto step = static_cast<float>(dt);
    LineSupply highest;
    std::vector<bool> done(n, false);
    std::vector<LinePoint> points(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (!electric(cars[i]) || done[i]) continue;
        // The group joined by the HV bus (RTS 0x4F9FB4 sets each engine's
        // +436 / +440 so): back while this car's front and the next car's
        // rear end are connected, and on likewise.
        std::size_t lo = i, hi = i;
        while (lo > 0 && (cars[lo].cables & cable_hv_front) && (cars[lo - 1].cables & cable_hv_rear)) --lo;
        while (hi + 1 < n && (cars[hi].cables & cable_hv_rear) && (cars[hi + 1].cables & cable_hv_front)) ++hi;
        LineSupply line;
        float power = 0.0f;
        unsigned raised_total = 0;
        std::vector<float> heat(hi - lo + 1, 0.0f);
        std::vector<bool> burnt(hi - lo + 1, false);
        for (std::size_t k = lo; k <= hi; ++k) {
            if (!electric(cars[k])) continue;
            const auto& car = cars[k];
            const auto& engine = *car.definition->engine;
            float& h = heat[k - lo];
            bool b = car.line_burnt;
            h = car.line_heat;
            power += car.power;
            // The wire cools under a working pantograph (MLC_B + MLC_A x speed
            // a second); a burnt one counts its 60 s.
            const bool gradual = engine.max_line_current > 0.0f && engine.max_line_current_burn_time > 0.0f;
            if (gradual && !b) {
                h = (std::max)(h - (engine.line_current_cooling[1] * std::abs(car.velocity) + engine.line_current_cooling[0]) * step, 0.0f);
            }
            const std::uint8_t mask = k < pantographs.size() ? pantographs[k] : 0;
            if (b) {
                h += step;
                if (h > 60.0f) b = false;
            }
            burnt[k - lo] = b;
            // As 0x503CE4: a pantograph burnt at the start of the step gives
            // no line this step even if it has just cooled.
            if (!mask || car.line_burnt || k >= centres.size()) continue;
            const auto& traveller = centres[k];
            points[k] = at(traveller.node(), traveller.offset(), traveller.pose(), &engine);
            raised_total += raised_count(mask);
            if (points[k].supply.voltage != 0.0f) {
                // As 0x503CE4: the highest voltage, the frequency of the last one found.
                line.voltage = (std::max)(line.voltage, points[k].supply.voltage);
                line.frequency = points[k].supply.frequency;
            }
        }
        if (line.voltage > highest.voltage) highest = line;
        const float current_total = line.voltage > 0.0f ? power / line.voltage : 0.0f;
        for (std::size_t k = lo; k <= hi; ++k) {
            if (!electric(cars[k])) continue;
            done[k] = true;
            const auto& engine = *cars[k].definition->engine;
            train.set_line_supply(k, line.voltage, line.frequency);
            float h = heat[k - lo];
            bool b = burnt[k - lo];
            const std::uint8_t mask = k < pantographs.size() ? pantographs[k] : 0;
            const unsigned own = b || cars[k].line_burnt ? 0u : raised_count(mask);
            float current = 0.0f;
            if (own && raised_total && line.voltage > 0.0f) current = current_total * static_cast<float>(own) / static_cast<float>(raised_total);
            if (current != 0.0f && wire_burning) {
                const float per = std::abs(current) / static_cast<float>(own);
                const float limit = engine.max_line_current * engine.max_line_current_burn_time;
                bool burns = false;
                if (limit <= 0.0f) {
                    burns = engine.max_line_current > 0.0f && per > engine.max_line_current;
                } else if (points[k].burn_q > 0.0f) {
                    h += per / points[k].burn_q * step;
                    burns = h >= limit;
                }
                if (burns) {
                    h = 0.0f;
                    b = true;
                    burns_.push_back({.train = train.id(), .car = k, .current = per});
                }
            }
            train.set_line_load(k, b ? 0.0f : current, h, b);
            // The substation feeding it takes the group's power by this
            // engine's share of the pantographs (0x5B6030).
            const auto& point = points[k];
            if (!b && own && raised_total && point.section >= 0 && point.source >= 0 && step > 0.0f) {
                const auto& section = data_.database.sections[static_cast<std::size_t>(point.section)];
                const auto& source = section.sources[static_cast<std::size_t>(point.source)];
                const float share = power * static_cast<float>(own) / static_cast<float>(raised_total);
                const auto* state = state_of(source.substation);
                if (state && state->off) continue; // RTS 0x5B6030: an off substation sums nothing
                auto& load = loads_[static_cast<std::uint64_t>(source.substation) << 32 | source.phase];
                load.substation = source.substation;
                load.phase = source.phase;
                load.energy += share * step;
            }
        }
    }
    return highest;
}

void Electrification::advance_loads(const double dt) {
    for (auto& [key, load] : loads_) {
        load.window += static_cast<float>(dt);
        if (load.window < 2.0f) continue;
        if (load.energy > 0.0f) closed_.push_back({load.substation, load.phase, load.energy, load.window});
        load.power = load.energy / load.window;
        load.energy = 0.0f;
        load.window = 0.0f;
    }
}

std::vector<SubstationLoad> Electrification::substation_loads() const {
    std::vector<SubstationLoad> out;
    for (const auto& [key, load] : loads_) out.push_back(load);
    return out;
}

void Electrification::set_substation(const std::uint32_t id, const bool off, const float voltage) {
    auto& state = substation_state_[id];
    state.off = off;
    state.voltage = voltage;
}

void Electrification::set_phase_voltages(const std::uint32_t id, std::vector<float> voltages) {
    substation_state_[id].phases = std::move(voltages);
}

void Electrification::set_section_source(const std::uint32_t section_id, const int source) {
    if (section_id >= section_by_id_.size() || section_by_id_[section_id] < 0) return;
    const auto index = static_cast<std::size_t>(section_by_id_[section_id]);
    if (file_current_.empty()) {
        file_current_ = current_;
        file_active_ = active_;
    }
    if (source >= 0) {
        set_current_source(index, source);
    } else if (source == -1) {
        current_[index] = file_current_[index]; // its own, as the route file has it
        active_[index] = file_active_[index];
    } else {
        current_[index] = -1; // "отключено": every source out
        for (auto& flags : active_[index]) flags = 0;
    }
}

void Electrification::set_piece_supply(const std::uint32_t node, const std::uint32_t piece, const int source) {
    const auto key = static_cast<std::uint64_t>(node) << 32 | piece;
    if (source == -1) {
        piece_supply_.erase(key); // the section's choice
    } else {
        piece_supply_[key] = source;
    }
}

int Electrification::current_source(const std::size_t section) const {
    return section < current_.size() ? current_[section] : -1;
}

void Electrification::set_current_source(const std::size_t section, const int source) {
    if (section >= current_.size()) return;
    auto& active = active_[section];
    if (source < 0 || source >= static_cast<int>(active.size())) {
        current_[section] = -1;
        return;
    }
    current_[section] = source;
    // As CurrentSource in the file (0x491DC8): the chosen one switched in,
    // every source enabled.
    for (std::size_t k = 0; k < active.size(); ++k) {
        active[k] = (static_cast<int>(k) == source ? (active[k] | 1u) : (active[k] & ~1u)) | 2u;
    }
}

const formats::ElectricSupplyType* Electrification::supply_type(const std::size_t section) const {
    if (section >= data_.database.sections.size()) return nullptr;
    const auto& sources = data_.database.sections[section].sources;
    if (sources.empty()) return nullptr;
    const int current = current_[section];
    const auto& source = sources[current >= 0 ? static_cast<std::size_t>(current) : 0u];
    return type_of(source);
}

std::vector<Electrification::Stretch> Electrification::stretches(const std::uint32_t node) const {
    std::vector<Stretch> out;
    if (mode_ != Mode::sections) return out;
    const auto* data = node < nodes_.size() ? nodes_[node] : nullptr;
    const auto* n = network_ ? network_->node(node) : nullptr;
    if (!data || !n || n->length <= 0.0) return out;
    std::vector<double> cuts{0.0, n->length};
    for (const auto& p : data->electricity) cuts.push_back(p.start);
    for (const auto& p : data->catenary) {
        cuts.push_back(p.start);
        if (p.length > 0.0f) cuts.push_back(p.start + p.length);
    }
    for (auto& c : cuts) c = std::clamp(c, 0.0, n->length);
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end(), [](double a, double b) { return std::abs(a - b) < 0.01; }), cuts.end());
    for (std::size_t i = 0; i + 1 < cuts.size(); ++i) {
        const double middle = (cuts[i] + cuts[i + 1]) * 0.5;
        Stretch s{cuts[i], cuts[i + 1]};
        if (wire_at(*data, middle)) {
            s.state = LineState::dead;
            if (const auto* piece = piece_at(*data, middle);
                piece && piece->section != 0 && piece->section < section_by_id_.size() && section_by_id_[piece->section] >= 0) {
                s.section = section_by_id_[piece->section];
                s.state = LineState::live;
            }
        }
        if (!out.empty() && out.back().state == s.state && out.back().section == s.section) {
            out.back().to = s.to;
        } else {
            out.push_back(s);
        }
    }
    return out;
}

bool Electrification::works_on(const formats::EngineDefinition& engine, const LineSupply& supply) {
    if (supply.voltage <= 0.0f) return false;
    if (engine.line_voltages.empty()) return true;
    return std::any_of(engine.line_voltages.begin(), engine.line_voltages.end(), [&](const formats::LineVoltageRange& r) {
        return supply.frequency >= r.min_frequency && supply.frequency <= r.max_frequency;
    });
}

} // namespace openrts::simulation
