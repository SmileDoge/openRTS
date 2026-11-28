#include <openrts/simulation/savpe.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <set>

#include <openrts/core/virtual_path.hpp>
#include <openrts/simulation/milepost.hpp>
#include <openrts/simulation/timetable.hpp>
#include <openrts/simulation/track_network.hpp>

namespace openrts::simulation {

namespace {

// A halt (о.п.) by its name: "пл.", "о.п.", "платформа", "... км".
bool halt_name(const std::string& name) {
    const auto n = core::fold_case(name);
    for (const char* mark : {"пл.", "о.п", "о. п", "платформа", "пп.", " км", "км "})
        if (n.find(mark) != std::string::npos) return true;
    return false;
}

} // namespace

std::vector<SavpeStop> savpe_stops(const TrackNetwork& network, const TrainRoute& route) {
    std::vector<SavpeStop> stops;
    const auto& items = network.database().items;
    std::set<std::uint32_t> seen;
    for (const auto& pass : platform_passes(network, route)) {
        if (pass.far_item >= items.size()) continue;
        const auto& item = items[pass.far_item];
        // Each platform once (both its ends).
        if (seen.contains(pass.far_item) || (item.linked != 0xFFFFFFFFu && seen.contains(item.linked))) continue;
        seen.insert(pass.far_item);
        SavpeStop stop;
        stop.name = item.station.empty() ? item.name : item.station;
        stop.station = !halt_name(stop.name);
        stop.leg = pass.leg;
        stop.far = pass.far;
        stop.length = pass.length;
        stop.item = pass.far_item;
        if (pass.leg < route.legs.size()) {
            if (const auto at = route.legs[pass.leg].at(pass.far)) {
                MilepostState milepost;
                update_milepost(milepost, network, at->node, at->offset, at->forward);
                stop.km = milepost.base;
            }
        }
        // A station's several platforms in a row (the route on one of its
        // tracks, then the next of the same station): one stop.
        if (!stops.empty() && stops.back().name == stop.name && stops.back().leg == stop.leg && stop.far - stops.back().far < 400.0) {
            stops.back() = stop;
            continue;
        }
        stops.push_back(std::move(stop));
    }
    // A stop without a milepost (no post within 5 km of it: the path's
    // start on a station's far track) from its neighbour on the same leg,
    // by the metres between them, the numbers' way as theirs.
    for (std::size_t i = 0; i < stops.size(); ++i) {
        if (stops[i].km >= 0.0f) continue;
        for (const std::size_t j : {i + 1, i + 2}) {
            if (j >= stops.size() || stops[j].leg != stops[i].leg || stops[j].km < 0.0f) continue;
            // The way the numbers run: from the pair after it.
            const std::size_t k = j + 1;
            const bool rising = k >= stops.size() || stops[k].km < 0.0f || stops[k].km >= stops[j].km;
            const auto metres = static_cast<float>(stops[j].far - stops[i].far);
            const float km = stops[j].km + (rising ? -metres : metres) / 1000.0f;
            if (km >= 0.0f) stops[i].km = km;
            break;
        }
    }
    return stops;
}

std::vector<SavpeVoiceLine> savpe_voice_lines(const formats::SoundScript& script) {
    std::map<std::string, std::uint32_t> named;
    for (const auto& [name, value] : script.named_triggers) named[core::fold_case(name)] = value;
    // A discrete code's files (its PlayOneShot).
    std::map<std::uint32_t, std::vector<std::string>> files;
    for (const auto& group : script.groups)
        for (const auto& stream : group.streams)
            for (const auto& trigger : stream.triggers) {
                if (trigger.kind != formats::SoundTriggerKind::discrete) continue;
                for (const auto& action : trigger.actions) {
                    if (action.kind != formats::SoundActionKind::play_one_shot || action.files.empty()) continue;
                    auto& list = files[trigger.code];
                    if (!list.empty()) continue;
                    for (const auto& f : action.files) list.push_back(f.name);
                }
            }
    const auto value = [&](const std::string& key) -> long long {
        const auto it = named.find(key);
        return it == named.end() ? -1 : static_cast<long long>(it->second);
    };
    const auto files_of = [&](const long long code) -> const std::vector<std::string>* {
        if (code < 0) return nullptr;
        const auto it = files.find(static_cast<std::uint32_t>(code));
        return it == files.end() ? nullptr : &it->second;
    };
    // The trains: MDD_INFO_R<route>_<train>_NUMBER.
    std::map<std::uint32_t, SavpeVoiceLine> lines;
    for (const auto& [key, number] : named) {
        if (key.rfind("mdd_info_r", 0) != 0 || key.size() < 8 || key.compare(key.size() - 7, 7, "_number") != 0) continue;
        const auto route = static_cast<std::uint32_t>(std::strtoul(key.c_str() + 10, nullptr, 10));
        const std::string prefix = "mdd_info_r" + std::to_string(route) + "_" + std::to_string(number) + "_";
        auto& line = lines[route];
        line.route = route;
        line.trains.push_back(number);
        if (const auto* w = files_of(value(prefix + "wel_sound")); w && w->size() >= 3) {
            if (line.welcome.empty()) line.welcome = w->front();
            if (line.all_stops.empty()) line.all_stops = w->back();
        }
        const auto count = value(prefix + "count");
        std::vector<std::string> leaving; // said leaving the stop before (S odd, POS 0)
        for (long long s = 1; s <= count; ++s) {
            const auto* f = files_of(value(prefix + "s" + std::to_string(s) + "_sound"));
            if (!f) continue;
            const auto pos = value(prefix + "s" + std::to_string(s) + "_pos");
            if (pos <= 0) {
                leaving = *f;
                continue;
            }
            SavpeVoicePoint point{.km = static_cast<float>(pos) / 10.0f, .name = {}, .approach = leaving, .arrival = *f};
            leaving.clear();
            // Made of parts: leaving "следующая", the name, "двери
            // закрываются"; arriving the name, "следующая" and the next
            // one's, or "конечная".
            if (point.approach.size() >= 3) {
                point.name = point.approach[1];
                if (line.next.empty()) line.next = point.approach.front();
                if (line.doors.empty()) line.doors = point.approach.back();
            }
            if (f->size() >= 2 && point.name.empty() && line.made_of_parts()) point.name = f->front();
            if (f->size() == 2 && line.terminus.empty() && line.made_of_parts()) line.terminus = (*f)[1];
            line.points.push_back(std::move(point));
        }
    }
    std::vector<SavpeVoiceLine> out;
    for (auto& [_, line] : lines) {
        // Each stop once (the first train's), by km.
        const auto key = [](const SavpeVoicePoint& p) { return p.arrival.empty() ? std::string() : p.arrival.front(); };
        std::stable_sort(line.points.begin(), line.points.end(), [&](const auto& a, const auto& b) {
            return a.km < b.km || (a.km == b.km && key(a) < key(b));
        });
        line.points.erase(std::unique(line.points.begin(), line.points.end(),
                                      [&](const auto& a, const auto& b) { return a.km == b.km && key(a) == key(b); }),
                          line.points.end());
        out.push_back(std::move(line));
    }
    return out;
}

int match_savpe_voices(std::vector<SavpeStop>& stops, const std::vector<SavpeVoiceLine>& lines, const float tolerance) {
    // The nearest point of a line to a km, within the tolerance.
    const auto nearest = [&](const SavpeVoiceLine& line, const float km) -> const SavpeVoicePoint* {
        const SavpeVoicePoint* best = nullptr;
        for (const auto& p : line.points) {
            const float d = std::abs(p.km - km);
            if (d <= tolerance && (!best || d < std::abs(best->km - km))) best = &p;
        }
        return best;
    };
    int chosen = -1;
    std::size_t most = 1;
    for (std::size_t l = 0; l < lines.size(); ++l) {
        std::size_t fit = 0;
        for (const auto& stop : stops)
            if (stop.km >= 0.0f && nearest(lines[l], stop.km)) ++fit;
        if (fit > most) {
            most = fit;
            chosen = static_cast<int>(l);
        }
    }
    for (auto& stop : stops) {
        stop.voice.clear();
        stop.approach.clear();
        stop.arrival.clear();
        if (chosen < 0 || stop.km < 0.0f) continue;
        if (const auto* p = nearest(lines[static_cast<std::size_t>(chosen)], stop.km)) {
            stop.voice = p->name;
            stop.approach = p->approach;
            stop.arrival = p->arrival;
        }
    }
    return chosen;
}

} // namespace openrts::simulation
