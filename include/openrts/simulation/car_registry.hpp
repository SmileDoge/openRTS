#pragma once

// Cars as entities, step 1 (docs/car-architecture.md, "Moving there"): a
// car's stable identity. A CarId is the car's key the viewer has always
// kept (one u32, no second system): made once with the car, carried with it
// through couplings, cuts and trains handed over (it lives in the car's
// CarState, so whatever reorders a train's cars moves it along), saved with
// it in a saved game.
//
// A CarRegistry belongs to one world (the viewer's scene, the server's
// game): the server's and a client's are never shared. Within its world an
// id is never used twice, not even after its car is gone: a stale id finds
// nothing, never another car. Running out of ids is an error, not a wrap.

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <vector>

namespace openrts::simulation {

struct CarId {
    std::uint32_t value{}; // 0: none

    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return valid(); }
    friend constexpr bool operator==(CarId, CarId) noexcept = default;
    friend constexpr auto operator<=>(CarId, CarId) noexcept = default;
};

// Thrown when a world has given out every id (2^32 - 1 cars).
class CarIdsExhausted : public std::overflow_error {
public:
    CarIdsExhausted() : std::overflow_error("the world has no car ids left (2^32 - 1 given out)") {}
};

class CarRegistry {
public:
    // A new car: an id never given out in this world.
    [[nodiscard]] CarId create();
    // A car coming back with the id it had (a saved game): that id when this
    // world has never given it out, else a new one (the id was another car's
    // here, alive or gone: the caller maps the saved id to the one returned).
    // An invalid id gets a new one too.
    [[nodiscard]] CarId restore(CarId saved);
    // The car is gone for good: its id stays retired (never given again); a
    // network mapping to it is dropped. Unknown or retired ids: nothing.
    void release(CarId id);

    [[nodiscard]] bool alive(CarId id) const noexcept { return alive_.contains(id.value); }
    [[nodiscard]] std::size_t size() const noexcept { return alive_.size(); }
    // The highest id given out (every id up to it is taken or retired).
    [[nodiscard]] std::uint32_t last() const noexcept { return last_; }

    // A network game: the server names a car by its uid. The car of that uid
    // in this world, made when it is new or its car is gone (a client that
    // connects again finds the cars it already has by their uids). Its own
    // ids need not match another computer's. A uid is the train's number and
    // the car's place (number << 18 | index), and two trains may share a
    // number: `taken` says an id another car of the world holds now, and then
    // the uid gets a new car instead of that one.
    [[nodiscard]] CarId for_server_uid(std::uint32_t uid, const std::set<std::uint32_t>& taken = {});
    [[nodiscard]] std::optional<CarId> find_server_uid(std::uint32_t uid) const;
    [[nodiscard]] std::optional<std::uint32_t> server_uid_of(CarId id) const;

private:
    std::uint32_t last_ = 0;
    std::set<std::uint32_t> alive_;
    std::map<std::uint32_t, std::uint32_t> by_uid_; // server uid -> id
    std::map<std::uint32_t, std::uint32_t> uid_of_; // id -> server uid
};

// A saved game's car keys as this world's ids, in place: a key the world has
// never given out is that car's again, one it has (alive or retired) maps to
// a new id, the same key met again (`mapped`, kept across a save's trains)
// to the same id. 0, a car saved before cars had keys, stays 0: the train
// made from it gives it a new id. `mapped` (saved key -> id) is for the
// rest the save keeps by key (the DLLs' states, a train's first car).
void restore_saved_keys(CarRegistry& registry, std::vector<std::uint32_t>& keys, std::map<std::uint32_t, std::uint32_t>& mapped);

} // namespace openrts::simulation
