#include <openrts/simulation/car_registry.hpp>

#include <limits>

namespace openrts::simulation {

CarId CarRegistry::create() {
    if (last_ == std::numeric_limits<std::uint32_t>::max()) throw CarIdsExhausted();
    ++last_;
    alive_.insert(last_);
    return CarId{last_};
}

CarId CarRegistry::restore(const CarId saved) {
    // Never given out here: it is this car's again, and the ids from now on
    // go above it.
    if (saved.valid() && saved.value > last_) {
        last_ = saved.value;
        alive_.insert(last_);
        return saved;
    }
    return create();
}

void CarRegistry::release(const CarId id) {
    if (alive_.erase(id.value) == 0) return;
    if (const auto it = uid_of_.find(id.value); it != uid_of_.end()) {
        by_uid_.erase(it->second);
        uid_of_.erase(it);
    }
}

CarId CarRegistry::for_server_uid(const std::uint32_t uid, const std::set<std::uint32_t>& taken) {
    if (const auto it = by_uid_.find(uid); it != by_uid_.end() && alive_.contains(it->second) && !taken.contains(it->second))
        return CarId{it->second};
    const auto id = create();
    if (const auto old = by_uid_.find(uid); old != by_uid_.end()) uid_of_.erase(old->second);
    by_uid_[uid] = id.value;
    uid_of_[id.value] = uid;
    return id;
}

std::optional<CarId> CarRegistry::find_server_uid(const std::uint32_t uid) const {
    if (const auto it = by_uid_.find(uid); it != by_uid_.end() && alive_.contains(it->second)) return CarId{it->second};
    return std::nullopt;
}

std::optional<std::uint32_t> CarRegistry::server_uid_of(const CarId id) const {
    if (!alive(id)) return std::nullopt;
    if (const auto it = uid_of_.find(id.value); it != uid_of_.end()) return it->second;
    return std::nullopt;
}

void restore_saved_keys(CarRegistry& registry, std::vector<std::uint32_t>& keys, std::map<std::uint32_t, std::uint32_t>& mapped) {
    for (auto& key : keys) {
        if (key == 0) continue;
        const auto [it, added] = mapped.try_emplace(key, 0u);
        if (added) it->second = registry.restore(CarId{key}).value;
        key = it->second;
    }
}

} // namespace openrts::simulation
