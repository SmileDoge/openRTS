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
    float voltage = source.feeder_q * 0.01f * sub->voltage;
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
    const auto* piece = piece_at(*data, offset);
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

LineSupply Electrification::apply(Train& train, const std::vector<bool>& raised) const {
    const auto& cars = train.cars();
    const auto centres = train.car_centres();
    LineSupply line;
    bool any = false;
    for (std::size_t i = 0; i < cars.size() && i < centres.size(); ++i) {
        if (!electric(cars[i]) || i >= raised.size() || !raised[i]) continue;
        any = true;
        const auto& traveller = centres[i];
        const auto point = at(traveller.node(), traveller.offset(), traveller.pose(), &*cars[i].definition->engine);
        if (point.supply.voltage != 0.0f) {
            // As 0x503CE4: the highest voltage, the frequency of the last one found.
            line.voltage = (std::max)(line.voltage, point.supply.voltage);
            line.frequency = point.supply.frequency;
        }
    }
    if (!any) line = {};
    for (std::size_t i = 0; i < cars.size(); ++i) {
        if (electric(cars[i])) train.set_line_supply(i, line.voltage, line.frequency);
    }
    return line;
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
