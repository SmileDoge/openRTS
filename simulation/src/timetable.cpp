#include <openrts/simulation/timetable.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include <openrts/core/virtual_path.hpp>
#include <openrts/formats/track_database.hpp>
#include <openrts/simulation/track_network.hpp>

namespace openrts::simulation {

std::vector<PlatformPass> platform_passes(const TrackNetwork& network, const TrainRoute& route) {
    std::vector<PlatformPass> passes;
    const auto& items = network.database().items;
    const auto platform = [&](const std::uint32_t item) {
        return item < items.size() && items[item].kind == formats::TrackItemKind::platform;
    };
    for (std::size_t l = 0; l < route.legs.size(); ++l) {
        const auto& leg = route.legs[l];
        // A platform whose other end lies on another node: its first end met (by item).
        std::map<std::uint32_t, double> open;
        for (const auto& span : leg.spans) {
            const auto* node = network.node(span.node);
            if (!node) continue;
            const double low = (std::min)(span.from, span.to), high = (std::max)(span.from, span.to);
            std::vector<std::pair<double, const NetworkItem*>> here;
            for (const auto& entry : node->items) {
                if (entry.offset < low || entry.offset > high || !platform(entry.item)) continue;
                here.emplace_back(span.start + (span.forward ? entry.offset - span.from : span.from - entry.offset), &entry);
            }
            std::sort(here.begin(), here.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            // Each platform once a node (RTS 0x4755C8 keeps the pairs it has counted).
            std::set<std::uint32_t> counted;
            for (const auto& [along, entry] : here) {
                const auto other = items[entry->item].linked;
                if (counted.contains(entry->item) || !platform(other)) continue;
                counted.insert(entry->item);
                counted.insert(other);
                // Its other end on this node: ahead of it (this the near end)
                // or behind it (the train starts on the platform).
                const NetworkItem* partner = nullptr;
                for (const auto& e : node->items)
                    if (e.item == other) partner = &e;
                if (partner) {
                    const double ahead = span.forward ? partner->offset - entry->offset : entry->offset - partner->offset;
                    if (ahead >= 0.0) {
                        passes.push_back({.leg = l, .far = along + ahead, .length = ahead, .far_item = other});
                    } else {
                        passes.push_back({.leg = l, .far = along, .length = -ahead, .far_item = entry->item});
                    }
                    continue;
                }
                if (const auto first = open.find(other); first != open.end()) {
                    passes.push_back({.leg = l, .far = along, .length = along - first->second, .far_item = entry->item});
                    open.erase(first);
                } else {
                    open.emplace(entry->item, along);
                }
            }
        }
    }
    // In the route's order.
    std::stable_sort(passes.begin(), passes.end(), [](const PlatformPass& a, const PlatformPass& b) {
        return a.leg != b.leg ? a.leg < b.leg : a.far < b.far;
    });
    return passes;
}

namespace {
} // namespace

void locate_stops(const TrackNetwork& network, const TrainRoute& route, std::vector<TimetableStop>& stops) {
    const auto passes = platform_passes(network, route);
    const auto& items = network.database().items;
    std::size_t next = 0; // the first pass after the stop before
    for (auto& stop : stops) {
        stop.found = false;
        const auto wanted = core::fold_case(stop.station);
        std::int32_t skip = (std::max)(stop.skipped, 0);
        for (std::size_t p = next; p < passes.size(); ++p) {
            const auto& item = items[passes[p].far_item];
            if (core::fold_case(item.station) != wanted) continue;
            if (skip > 0) {
                --skip;
                continue;
            }
            stop.found = true;
            stop.leg = passes[p].leg;
            stop.distance = passes[p].far;
            stop.length = passes[p].length;
            stop.platform = passes[p].far_item;
            stop.min_wait = item.min_wait;
            stop.waiting = item.passengers;
            stop.station = item.station;
            next = p + 1;
            break;
        }
    }
}

Timetable::Timetable(std::vector<TimetableStop> stops, const TrainRoute& route, const float tolerance, const bool check_doors)
    : stops_(std::move(stops)), tolerance_((std::max)(tolerance, 0.0f)), check_doors_(check_doors) {
    double start = 0.0;
    for (const auto& leg : route.legs) {
        leg_start_.push_back(start);
        start += leg.length();
    }
}

double Timetable::route_distance(const std::size_t leg, const double distance) const {
    return (leg < leg_start_.size() ? leg_start_[leg] : (leg_start_.empty() ? 0.0 : leg_start_.back())) + distance;
}

double Timetable::distance(const std::size_t stop, const TimetableTrain& train) const {
    if (stop >= stops_.size()) return 0.0;
    return route_distance(stops_[stop].leg, stops_[stop].distance) - route_distance(train.leg, train.head);
}

StopPlace Timetable::place(const std::size_t stop, const TimetableTrain& train) const {
    // RTS 0x466F70: d from the head to the platform's far end, P its length,
    // T the train's, the leading car's half the margin of a long train.
    const double d = distance(stop, train);
    const double platform = stops_[stop].length;
    if (d < -train.length) return StopPlace::passed;
    if (d >= platform) return StopPlace::ahead;
    if (d >= 0.0 && platform - d >= train.length) return StopPlace::in_place;
    if (train.length > platform && d < train.lead_car * 0.5) return StopPlace::covering;
    return StopPlace::partly;
}

std::optional<std::size_t> Timetable::stop_at(const TimetableTrain& train) const {
    for (std::size_t s = 0; s < stops_.size(); ++s) {
        const auto& stop = stops_[s];
        // On the train's leg: a stop past a reverse point is not under it.
        if (!stop.found || stop.departed || stop.leg != train.leg) continue;
        const double d = stop.distance - train.head;
        if (d < stop.length && d > -train.length) return s;
    }
    return std::nullopt;
}

bool Timetable::arrived_on_time(const TimetableStop& stop) const {
    return stop.arrived_at && static_cast<double>(*stop.arrived_at) <= static_cast<double>(stop.arrival) + tolerance_;
}

bool Timetable::left_on_time(const TimetableStop& stop) { return stop.departed_at && *stop.departed_at >= stop.departure; }

BoardingStart Timetable::start_boarding(const TimetableTrain& train, const std::int32_t time_of_day) {
    if (boarding_) return BoardingStart::already;
    if (std::abs(train.speed) >= 0.5f) return BoardingStart::moving;
    const auto at = stop_at(train);
    if (!at) return BoardingStart::no_stop;
    if (check_doors_ && !train.doors_open) return BoardingStart::doors_closed;
    auto& stop = stops_[*at];
    boarding_ = at;
    room_ = 0.0f;
    over_ = false;
    stop.arrived = true;
    stop.arrived_at = time_of_day;
    // The platform's wait for its waiting passengers, for the stop's (RTS:
    // the stop's count as seconds when the platform has none waiting).
    left_ = stop.waiting > 0 ? static_cast<float>(stop.min_wait) * stop.passengers / static_cast<float>(stop.waiting) : stop.passengers;
    events_.push_back({.kind = TimetableEventKind::boarding_started, .stop = *at});
    check_room(train, time_of_day);
    return BoardingStart::started;
}

void Timetable::step(const TimetableTrain& train, const float elapsed, const std::int32_t time_of_day) {
    if (!boarding_) return;
    left_ -= elapsed;
    if (left_ <= 0.0f) {
        left_ = 0.0f;
        if (!over_) {
            over_ = true;
            events_.push_back({.kind = TimetableEventKind::boarding_over, .stop = *boarding_});
        }
    }
    check_room(train, time_of_day);
}

void Timetable::check_room(const TimetableTrain& train, const std::int32_t time_of_day) {
    if (!boarding_) return;
    auto& stop = stops_[*boarding_];
    // The cars' room at the platform (their centres on it) with the doors
    // open, against the whole train's.
    const double far = route_distance(stop.leg, stop.distance);
    const double near = far - stop.length;
    const double head = route_distance(train.leg, train.head);
    float open = 0.0f, all = 0.0f;
    for (const auto& [behind, room] : train.cars) {
        if (room <= 0.0f) continue;
        all += room;
        const double at = head - behind;
        if (at >= near && at <= far && (!check_doors_ || train.doors_open)) open += room;
    }
    if (open > 0.0f && std::abs(train.speed) <= 0.5f) {
        // Fewer open: longer (the first time against the whole train's room).
        if (left_ != 0.0f) left_ = (room_ != 0.0f ? room_ : all) / open * left_;
        room_ = open;
        return;
    }
    stop.departed_at = time_of_day;
    const bool done = left_ == 0.0f;
    if (done) stop.departed = true;
    left_ = 0.0f;
    room_ = 0.0f;
    events_.push_back({.kind = TimetableEventKind::left, .stop = *boarding_, .done = done});
    boarding_.reset();
}

std::vector<TimetableEvent> Timetable::take_events() { return std::exchange(events_, {}); }

} // namespace openrts::simulation
