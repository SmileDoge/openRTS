#include <openrts/simulation/cargo.hpp>
#include <openrts/simulation/train.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace openrts::simulation {

namespace {

// A car "standing" for a SpeedRange of 0: what the physics leaves of a
// braked train.
constexpr float standing_speed = 0.05f;

[[nodiscard]] bool bulk_load(const CarLoad& load) noexcept {
    return load.object.empty();
}

} // namespace

CarCargo::CarCargo(const formats::WagonCargoDescription* description, std::array<float, 3> size)
    : description_(description), size_(size) {}

std::uint32_t CarCargo::loading() const noexcept {
    if (!description_) return 0;
    std::uint32_t bits = description_->loading;
    for (const auto& area : description_->areas) bits |= area.loading;
    return bits;
}

formats::CargoBox CarCargo::area_box(int area) const {
    if (description_ && area >= 1 && static_cast<std::size_t>(area) <= description_->areas.size()) {
        return description_->areas[static_cast<std::size_t>(area - 1)].box;
    }
    // No area: the car's Size box above its floor (RTS's "Bounding box").
    formats::CargoBox box;
    box.low = {-size_[0] * 0.5f, 1.3f, -size_[2] * 0.5f};
    box.high = {size_[0] * 0.5f, (std::max)(size_[1], 1.4f), size_[2] * 0.5f};
    return box;
}

float CarCargo::room(int area) const {
    if (!description_) return 0.0f;
    if (area >= 1) return area_box(area).volume();
    if (!description_->areas.empty()) {
        float sum = 0.0f;
        for (const auto& a : description_->areas) sum += a.box.volume();
        return sum;
    }
    return description_->capacity * box_volume();
}

float CarCargo::bulk_volume(int area) const {
    float sum = 0.0f;
    for (const auto& load : loads_) {
        if (area == 0 || load.area == area) sum += load.volume;
    }
    return sum;
}

float CarCargo::load_mass() const {
    float sum = 0.0f;
    for (const auto& load : loads_) sum += load.mass;
    return sum;
}

float CarCargo::mass_limit() const {
    return description_ && description_->maximum_weight > 0.0f ? description_->maximum_weight * box_volume() : 0.0f;
}

float CarCargo::fill_percent() const {
    // rts1.exe 0x501FFC: by volume, or by mass when that is more.
    if (!description_) return 100.0f;
    const float space = room();
    const float by_volume = space > 0.0f ? bulk_volume() / space * 100.0f : 0.0f;
    const float limit = mass_limit();
    const float by_mass = limit > 0.0f ? load_mass() / limit * 100.0f : 0.0f;
    return (std::max)(by_volume, by_mass);
}

std::optional<int> CarCargo::area_at(float z, float margin) const {
    if (!description_) return std::nullopt;
    if (description_->areas.empty()) return 0;
    for (std::size_t i = 0; i < description_->areas.size(); ++i) {
        const auto& area = description_->areas[i];
        if (area.loading && !(area.loading & (formats::cargo_pile | formats::cargo_liquid | formats::cargo_gas))) continue;
        if (z >= area.box.low[2] - margin && z <= area.box.high[2] + margin) return static_cast<int>(i + 1);
    }
    return std::nullopt;
}

float CarCargo::move_bulk(const formats::CargoKind& kind, float volume, int area) {
    if (!description_ || volume == 0.0f) return 0.0f;
    if (volume > 0.0f) {
        // rts1.exe 0x5020E0: capped by the car's room and its mass limit.
        volume = (std::min)(volume, (std::max)(0.0f, room() - bulk_volume()));
        if (area >= 1) volume = (std::min)(volume, (std::max)(0.0f, room(area) - bulk_volume(area)));
        if (const float limit = mass_limit(); limit > 0.0f && kind.density > 0.0f) {
            volume = (std::min)(volume, (std::max)(0.0f, (limit - load_mass()) / kind.density));
        }
        if (volume <= 0.0f) return 0.0f;
        auto it = std::find_if(loads_.begin(), loads_.end(),
                               [&](const CarLoad& l) { return bulk_load(l) && l.cargo == kind.code && l.area == area; });
        if (it == loads_.end()) {
            loads_.push_back({.cargo = kind.code, .area = area});
            it = std::prev(loads_.end());
        }
        it->volume += volume;
        it->mass += volume * kind.density;
        return volume;
    }
    // Taking: from the bulk loads of the area (any kind), the last first.
    float wanted = -volume;
    float taken = 0.0f;
    for (auto it = loads_.rbegin(); it != loads_.rend() && wanted > 0.0f;) {
        if (!bulk_load(*it) || (area >= 1 && it->area != area) || it->volume <= 0.0f) {
            ++it;
            continue;
        }
        const float part = (std::min)(wanted, it->volume);
        const float density = it->volume > 0.0f ? it->mass / it->volume : 0.0f;
        it->volume -= part;
        it->mass -= part * density;
        wanted -= part;
        taken += part;
        if (it->volume <= 1e-6f) {
            it = std::make_reverse_iterator(loads_.erase(std::next(it).base()));
        } else {
            ++it;
        }
    }
    return -taken;
}

int CarCargo::place_objects(const formats::CargoObject& object, std::uint16_t cargo, float mass, int count, int area,
                            CargoPlacement placement, bool centred, std::string* why) {
    if (!description_) {
        if (why) *why = "Failed to place cargo in the selected area";
        return 0;
    }
    // Area 0 with areas: the first that takes this loading type.
    if (area == 0 && !description_->areas.empty()) {
        for (std::size_t i = 0; i < description_->areas.size(); ++i) {
            const auto bits = description_->areas[i].loading;
            if (!bits || (bits & object.loading)) {
                area = static_cast<int>(i + 1);
                break;
            }
        }
    }
    const auto box = area_box(area);
    const auto e = object.extent();
    const float width = box.high[0] - box.low[0];
    const float height = box.high[1] - box.low[1];
    const float centre_x = (box.low[0] + box.high[0]) * 0.5f;
    const float centre_z = (box.low[2] + box.high[2]) * 0.5f;
    int placed = 0;
    for (int n = 0; n < count; ++n) {
        if (const float limit = mass_limit(); limit > 0.0f && load_mass() + mass > limit) {
            if (why) *why = "Wagon maximum weight limit reached";
            break;
        }
        // On a stack of its own kind first (Stackable high, room above).
        bool stacked = false;
        if (object.stackable > 1) {
            for (auto& load : loads_) {
                if (load.object != object.id || load.area != area || load.count >= static_cast<int>(object.stackable)) continue;
                if (static_cast<float>(load.count + 1) * e[1] > height + 0.01f) continue;
                ++load.count;
                load.mass += mass;
                load.volume += e[0] * e[1] * e[2];
                stacked = true;
                break;
            }
        }
        if (!stacked) {
            if (e[0] > width + 0.01f || e[1] > height + 0.01f || e[2] > box.high[2] - box.low[2] + 0.01f) {
                if (why) *why = "Failed to place cargo in the selected area";
                break;
            }
            // Free where no object of the area lies (x and z overlap).
            const auto free = [&](float x, float z) {
                if (x - e[0] * 0.5f < box.low[0] - 0.01f || x + e[0] * 0.5f > box.high[0] + 0.01f) return false;
                if (z - e[2] * 0.5f < box.low[2] - 0.01f || z + e[2] * 0.5f > box.high[2] + 0.01f) return false;
                for (const auto& load : loads_) {
                    if (bulk_load(load) || load.area != area) continue;
                    const bool apart_x = std::abs(load.position[0] - x) >= (load.extent[0] + e[0]) * 0.5f - 0.01f;
                    const bool apart_z = std::abs(load.position[2] - z) >= (load.extent[2] + e[2]) * 0.5f - 0.01f;
                    if (!apart_x && !apart_z) return false;
                }
                return true;
            };
            std::vector<float> xs;
            if (centred || e[0] * 2.0f > width) {
                xs = {centre_x};
            } else {
                xs = {box.low[0] + e[0] * 0.5f, box.high[0] - e[0] * 0.5f, centre_x};
            }
            std::vector<float> zs{box.low[2] + e[2] * 0.5f, box.high[2] - e[2] * 0.5f, centre_z};
            for (const auto& load : loads_) {
                if (bulk_load(load) || load.area != area) continue;
                zs.push_back(load.position[2] + (load.extent[2] + e[2]) * 0.5f);
                zs.push_back(load.position[2] - (load.extent[2] + e[2]) * 0.5f);
            }
            // Front: the largest z (the car's front), Back: the smallest,
            // Center / Manual: the nearest the middle.
            const auto score = [&](float z) {
                switch (placement) {
                case CargoPlacement::front: return -z;
                case CargoPlacement::back: return z;
                default: return std::abs(z - centre_z);
                }
            };
            std::optional<std::array<float, 2>> best;
            float best_score = std::numeric_limits<float>::max();
            for (const float z : zs) {
                for (const float x : xs) {
                    if (!free(x, z)) continue;
                    const float s = score(z) + std::abs(x - centre_x) * 1e-3f;
                    if (s < best_score) {
                        best_score = s;
                        best = std::array<float, 2>{x, z};
                    }
                }
            }
            if (!best) {
                if (why) *why = "Failed to place cargo in the selected area";
                break;
            }
            loads_.push_back({.cargo = cargo,
                              .object = object.id,
                              .area = area,
                              .volume = e[0] * e[1] * e[2],
                              .mass = mass,
                              .count = 1,
                              .position = {(*best)[0], box.low[1], (*best)[1]},
                              .extent = e});
        }
        ++placed;
        if (placement == CargoPlacement::manual) break; // RTS: one item in manual mode
    }
    return placed;
}

bool CarCargo::unload(std::size_t index) {
    if (index >= loads_.size()) return false;
    loads_.erase(loads_.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

void CarCargo::unload_all(int area) {
    std::erase_if(loads_, [&](const CarLoad& load) { return area == 0 || load.area == area; });
}

Pickup::Pickup(formats::PickupDescription description, const formats::CargoKinds& kinds) : description_(std::move(description)) {
    cargo_ = description_.cargo.empty() ? nullptr : kinds.find(description_.cargo);
    if (!cargo_) {
        // MSTS's types name their cargo.
        switch (description_.type) {
        case 1: cargo_ = kinds.find("grain"); break;
        case 2:
        case 6: cargo_ = kinds.find("coal"); break;
        case 3: cargo_ = kinds.find("gravel"); break;
        case 4:
        case 10: cargo_ = kinds.find("sand"); break;
        case 5: cargo_ = kinds.find("water"); break;
        case 7: cargo_ = kinds.find("diesel"); break;
        default: break;
        }
    }
    // The stock: CargoTable's mass of its cargo, else none (unlimited).
    for (const auto& item : description_.table) {
        if (cargo_ && item.object.empty() && item.cargo == cargo_->id && item.mass >= 0.0f) stock_ = item.mass;
    }
}

bool Pickup::crane() const noexcept {
    return description_.type == 12 || description_.type == 14 || description_.type == 15;
}

bool Pickup::unloads() const noexcept {
    return description_.type == 13;
}

const char* Pickup::toggle(bool unload) {
    const auto type = description_.type;
    if (unload) {
        if (type == 12 || type == 14) {
            state_ = PickupState::off;
        } else if (type == 16 || type == 13 || type == 7 || type == 8 || type == 5) {
            state_ = PickupState::unloading;
            return "Unloading cargo";
        } else {
            return "This pickup does not unload";
        }
    } else {
        // off → ready → active → off (rts1.exe 0x4210FC: active only with a
        // car at it, else back to off).
        switch (state_) {
        case PickupState::off: state_ = PickupState::ready; break;
        case PickupState::ready: state_ = PickupState::active; break;
        default: state_ = PickupState::off; break;
        }
    }
    switch (state_) {
    case PickupState::ready: return "Pickup ready";
    case PickupState::active: return "Pickup activated";
    default: return "Pickup off";
    }
}

void Pickup::take(float mass) {
    if (stock_ >= 0.0f) stock_ = (std::max)(0.0f, stock_ - mass);
}

PickupStep Pickup::step(float dt, std::span<PickupCar> cars) {
    PickupStep result;
    if (state_ == PickupState::off) return result;
    const auto type = description_.type;
    const float top = (std::max)(description_.speed_max, standing_speed);
    const auto in_range = [&](const PickupCar& car) { return car.speed <= top && car.speed >= description_.speed_min - 1e-3f; };

    if (crane()) {
        for (std::size_t i = 0; i < cars.size(); ++i) {
            if (!cars[i].in_reach) continue;
            if (!in_range(cars[i])) {
                result.too_fast = true;
                continue;
            }
            if (state_ == PickupState::active) {
                result.dialog = true;
                result.car = static_cast<int>(i);
            }
            break;
        }
        return result;
    }

    // rts1.exe 0x4E59A0: a diesel rack fills a diesel's fuel, a sand box
    // an engine's sand, at PickupSpeed per second, while switched on; a
    // diesel rack without a diesel at it fills a tank car.
    if (type == 7 || type == 10) {
        for (std::size_t i = 0; i < cars.size(); ++i) {
            auto& car = cars[i];
            if (!car.in_reach || !car.engine || !car.train || (type == 7 && !car.diesel)) continue;
            if (!in_range(car)) {
                result.too_fast = true;
                continue;
            }
            result.car = static_cast<int>(i);
            result.engine = true;
            if (state_ == PickupState::ready) break;
            const float rate = description_.speed > 0.0f ? description_.speed : 1.0f;
            float amount = rate * dt;
            if (stock_ >= 0.0f) amount = (std::min)(amount, stock_);
            result.volume = type == 7 ? car.train->add_fuel(car.index, amount) : car.train->add_sand(car.index, amount);
            if (stock_ >= 0.0f) stock_ = (std::max)(0.0f, stock_ - result.volume);
            if (result.volume <= 0.0f) state_ = stock_ == 0.0f ? PickupState::empty : PickupState::done;
            if (result.volume > 0.0f && state_ == PickupState::done) state_ = PickupState::active;
            return result;
        }
    }

    const bool bulk = (type >= 1 && type <= 4) || type == 6 || type == 10;
    const bool liquid = type == 5 || type == 7 || type == 16;
    if (unloads()) {
        // rts1.exe 0x4E59A0 case 13: the car's DumpSpeed (else the
        // pickup's), below its DumpMaxVelocity; ready becomes active by
        // itself with a loaded car.
        if (state_ == PickupState::unloading) state_ = PickupState::ready;
        for (std::size_t i = 0; i < cars.size(); ++i) {
            auto& car = cars[i];
            if (!car.in_reach || !car.cargo || !car.cargo->loadable()) continue;
            if (car.cargo->bulk_volume() <= 0.0f) {
                if (state_ == PickupState::active) state_ = PickupState::done;
                continue;
            }
            const auto* d = car.cargo->description();
            const float limit = d->dump_max_velocity >= 0.0f ? (std::max)(d->dump_max_velocity, standing_speed) : top;
            if (!in_range(car) || car.speed > limit) {
                result.too_fast = true;
                continue;
            }
            if (state_ == PickupState::ready || state_ == PickupState::done) state_ = PickupState::active;
            const float rate = d->dump_speed > 0.0f ? d->dump_speed : (description_.speed > 0.0f ? description_.speed : 1.0f);
            result.car = static_cast<int>(i);
            result.volume = car.cargo->move_bulk(formats::CargoKind{}, -rate * dt);
            result.pouring = result.volume < 0.0f;
            break;
        }
        return result;
    }

    if (!cargo_ || (!bulk && !liquid)) return result;
    for (std::size_t i = 0; i < cars.size(); ++i) {
        auto& car = cars[i];
        if (!car.in_reach || !car.area) continue;
        if (!car.cargo || !car.cargo->loadable() || !(car.cargo->loading() & cargo_->loading)) {
            result.unsuitable = true;
            continue;
        }
        if (!in_range(car)) {
            result.too_fast = true;
            continue;
        }
        result.car = static_cast<int>(i);
        if (bulk && state_ == PickupState::ready && !car.cargo->description()->areas.empty()) state_ = PickupState::active;
        if (state_ == PickupState::done || state_ == PickupState::empty) {
            if (car.cargo->fill_percent() < 100.0f && stock_ != 0.0f) state_ = PickupState::active;
        }
        if (state_ == PickupState::unloading && liquid) {
            // Ctrl+Num3 on a rack: the car drained at the filling rate.
            const float rate = (description_.speed > 0.0f ? description_.speed : 1.0f) * 0.01f;
            result.volume = car.cargo->move_bulk(*cargo_, -rate * dt);
            if (result.volume == 0.0f) state_ = PickupState::done;
            break;
        }
        if (state_ != PickupState::active) break;
        if (car.cargo->fill_percent() >= 100.0f) {
            state_ = PickupState::done;
            break;
        }
        // Bulk: PickupSpeed m³/s; liquids: a hundredth of it, as RTS
        // (0x4E59A0 case 5/7/16).
        float rate = description_.speed > 0.0f ? description_.speed : 1.0f;
        if (liquid) rate *= 0.01f;
        float volume = rate * dt;
        if (stock_ >= 0.0f && cargo_->density > 0.0f) volume = (std::min)(volume, stock_ / cargo_->density);
        if (stock_ == 0.0f) {
            state_ = PickupState::empty;
            break;
        }
        result.volume = car.cargo->move_bulk(*cargo_, volume, *car.area);
        if (stock_ >= 0.0f) stock_ = (std::max)(0.0f, stock_ - result.volume * cargo_->density);
        result.pouring = bulk && result.volume > 0.0f;
        if (result.volume <= 0.0f) state_ = car.cargo->fill_percent() >= 99.9f ? PickupState::done : state_;
        break;
    }
    return result;
}

} // namespace openrts::simulation
