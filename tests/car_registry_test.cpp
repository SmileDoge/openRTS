// Cars as entities, step 1 (docs/car-architecture.md): the world's car
// registry. Ids are given once and never again, a released id is found no
// more, running out is an error; a saved game's keys come back without
// clashing with the world's cars (a save from before the keys gets new
// ones); a client that connects again finds its cars by the server's uids.
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <openrts/simulation/car_registry.hpp>

namespace {

using namespace openrts;
using simulation::CarId;
using simulation::CarRegistry;

int failures = 0;

void check(const bool condition, const std::string& what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

void create_and_release() {
    CarRegistry cars;
    const auto a = cars.create(), b = cars.create(), c = cars.create();
    check(a.valid() && b.valid() && c.valid() && a != b && b != c && a != c, "three cars, three different valid ids");
    check(cars.alive(a) && cars.alive(b) && cars.alive(c) && cars.size() == 3, "all three alive");
    check(!cars.alive(CarId{}) && !CarId{}, "no id is no car");
    cars.release(b);
    check(!cars.alive(b) && cars.size() == 2, "a released id is found no more");
    cars.release(b);
    cars.release(CarId{999});
    check(cars.size() == 2, "releasing it again, or an unknown id: nothing");
    std::set<std::uint32_t> later;
    for (int i = 0; i < 100; ++i) later.insert(cars.create().value);
    check(!later.contains(a.value) && !later.contains(b.value) && !later.contains(c.value) && later.size() == 100,
          "new cars never take an id given before, a released one neither");
}

void exhausted() {
    CarRegistry cars;
    const auto top = cars.restore(CarId{std::numeric_limits<std::uint32_t>::max()});
    check(top.value == std::numeric_limits<std::uint32_t>::max(), "the last id restored");
    bool thrown = false;
    try {
        (void)cars.create();
    } catch (const simulation::CarIdsExhausted&) {
        thrown = true;
    }
    check(thrown, "no ids left: an error, not a wrap to an old one");
    cars.release(top);
    thrown = false;
    try {
        (void)cars.create();
    } catch (const simulation::CarIdsExhausted&) {
        thrown = true;
    }
    check(thrown, "an id released at the top is not given again either");
}

// A saved game (TrainView::load_game): its keys as the world's ids.
void saved_games() {
    {
        // At the start: the world has given nothing, the keys come back as they were.
        CarRegistry cars;
        std::vector<std::uint32_t> keys{7, 8, 9};
        std::map<std::uint32_t, std::uint32_t> mapped;
        simulation::restore_saved_keys(cars, keys, mapped);
        check(keys == std::vector<std::uint32_t>{7, 8, 9} && cars.size() == 3, "a save into a new world: its keys kept");
        check(cars.create().value == 10, "new cars after the saved ones");
    }
    {
        // Loaded again over the same cars (alive) and over gone ones: new
        // ids, the same saved key twice the same id, the DLLs' keys after them.
        CarRegistry cars;
        const auto a = cars.create(), b = cars.create(), c = cars.create();
        cars.release(c);
        std::vector<std::uint32_t> first{a.value, b.value};
        std::vector<std::uint32_t> second{c.value, 40};
        std::vector<std::uint32_t> again{b.value};
        std::map<std::uint32_t, std::uint32_t> mapped;
        simulation::restore_saved_keys(cars, first, mapped);
        simulation::restore_saved_keys(cars, second, mapped);
        simulation::restore_saved_keys(cars, again, mapped);
        std::set<std::uint32_t> now{first.begin(), first.end()};
        now.insert(second.begin(), second.end());
        check(now.size() == 4 && !now.contains(a.value) && !now.contains(b.value) && !now.contains(c.value),
              "a save over the world's cars: new ids, none of a car alive or gone");
        check(now.contains(40) && again.front() == first[1], "a key never given kept; the same key, the same id");
        check(mapped.at(b.value) == first[1] && mapped.size() == 4, "the map for what the save keeps by key");
        check(cars.alive(a) && cars.alive(b), "the cars it was loaded over are untouched");
    }
    {
        // A save from before the keys: 0, a new car each, no clash.
        CarRegistry cars;
        const auto a = cars.create();
        std::vector<std::uint32_t> keys{0, 0, 0};
        std::map<std::uint32_t, std::uint32_t> mapped;
        simulation::restore_saved_keys(cars, keys, mapped);
        check(keys == std::vector<std::uint32_t>{0, 0, 0} && mapped.empty(), "keys 0 stay 0: the train made gives them ids");
        std::set<std::uint32_t> made;
        for (auto& key : keys) made.insert(key = cars.create().value);
        check(made.size() == 3 && !made.contains(a.value) && !made.contains(0), "an old save's cars: fresh ids, no clash");
    }
}

// A network game: the server's uid -> the client's CarId.
void network() {
    CarRegistry server, client;
    const auto s1 = server.create(), s2 = server.create();
    (void)s1;
    (void)s2;
    const std::uint32_t uid_a = (101u << 18) | 0u, uid_b = (101u << 18) | 1u;
    // The client's world is its own: a car of its own first.
    const auto own = client.create();
    const auto a = client.for_server_uid(uid_a), b = client.for_server_uid(uid_b);
    check(a != b && a != own && b != own, "a client gives the server's cars its own ids");
    check(client.server_uid_of(a) == uid_a && client.find_server_uid(uid_b) == b, "the uid <-> id both ways");
    check(server.size() == 2 && client.size() == 3, "the server's and the client's registries are not shared");
    // Connected again: the server names the same cars; the client finds them.
    check(client.for_server_uid(uid_a) == a && client.for_server_uid(uid_b) == b, "connected again: the same cars");
    // Another train of the same number (uids repeat): not taken for ours.
    const std::set<std::uint32_t> taken{a.value, b.value};
    const auto other = client.for_server_uid(uid_a, taken);
    check(other != a && other != b && client.alive(a) && client.alive(other), "the same uid on another train: another car");
    // Deleted on the server: released; the uid named again is a new car.
    client.release(b);
    check(!client.find_server_uid(uid_b) && !client.server_uid_of(b), "a released car: its uid found no more");
    const auto again = client.for_server_uid(uid_b);
    check(again != b && client.alive(again), "its uid again: a new car, never the old id");
    check(!client.server_uid_of(CarId{12345}), "an unknown id has no uid");
}

// Step 2: a car's components (its doors) live by its id, as long as it.
void components() {
    CarRegistry cars;
    const auto a = cars.create(), b = cars.create();
    check(!cars.find_components(a), "nothing made for a car until asked");
    auto& doors = cars.components(a).doors;
    doors.doors.push_back({.name = "door_r", .index = 0, .group = 1, .time = 2.0f});
    doors.doors.push_back({.name = "door_l", .index = 2, .group = 2, .time = 2.0f});
    doors.groups = {1, 0, 2};
    // Many more cars: the reference stays good.
    for (int i = 0; i < 1000; ++i) (void)cars.components(cars.create());
    check(&cars.components(a).doors == &doors && doors.doors.size() == 2, "a car's components stay where they are");
    doors.step(0, true, 1.0f);
    check(std::abs(doors.doors[0].open - 0.5f) < 1e-6f && doors.doors[1].open == 0.0f, "a door opens at its AnimTime's pace");
    check(doors.open_groups() == 0x01u, "half open: its group is open");
    doors.step(0, true, 5.0f);
    doors.step(1, true, 0.5f);
    check(doors.doors[0].open == 1.0f && doors.open_groups() == 0x01u, "open, no further; the other a quarter: shut yet");
    doors.step(0, false, 0.5f);
    check(std::abs(doors.doors[0].open - 0.75f) < 1e-6f, "and shuts at the same pace");
    doors.step(9, true, 1.0f);
    doors.groups_open = 0x02u;
    check(doors.group_sent_open(2) && !doors.group_sent_open(1) && !doors.group_sent_open(0) && !doors.group_sent_open(9),
          "the groups another computer sends");
    check(doors.has_group(2) && doors.has_group(0) && !doors.has_group(3), "the car's GlobalIDs");
    check(doors.at_index(2) == &doors.doors[1] && !doors.at_index(1), "a door by its .sd index; none without a node");
    check(!cars.find_components(b) && cars.find_components(a) == &cars.components(a), "made only for the car asked");
    cars.release(a);
    check(!cars.find_components(a), "released: its components gone with it");
    bool thrown = false;
    try {
        (void)cars.components(a);
    } catch (const std::out_of_range&) {
        thrown = true;
    }
    check(thrown, "no components for a car gone");
}

} // namespace

int main() {
    create_and_release();
    exhausted();
    saved_games();
    network();
    components();
    if (failures == 0) std::cout << "car registry: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
