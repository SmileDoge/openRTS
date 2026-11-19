#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <openrts/core/vfs.hpp>
#include <openrts/formats/cargo_file.hpp>
#include <openrts/formats/route_world.hpp>
#include <openrts/formats/shape_description.hpp>

namespace openrts::simulation {
class Train;
}

namespace openrts::viewer {

struct CargoSite {
    std::string name;
    std::array<float, 3> at{};
    std::array<float, 4> rotation{};
    formats::PickupObject pickup;
    double stock{};
};

class CargoSystem {
public:
    CargoSystem(std::shared_ptr<const core::Vfs> vfs, core::VirtualPath route, formats::RoutePosition origin);
    ~CargoSystem();

    CargoSystem(const CargoSystem&) = delete;
    CargoSystem& operator=(const CargoSystem&) = delete;

    [[nodiscard]] bool ready() const noexcept { return loaded_ != nullptr; }
    [[nodiscard]] const std::string& error() const noexcept;
    [[nodiscard]] std::size_t sites() const noexcept;
    [[nodiscard]] const formats::CargoCatalog& catalog() const noexcept;

    void update(float seconds, simulation::Train& train, const std::span<const std::array<float, 3>> car_centres);

    [[nodiscard]] bool poll();

    [[nodiscard]] const CargoSite* site_at(const std::array<float, 3>& point, float length) const;

private:
    struct Loaded {
        formats::CargoCatalog catalog;
        std::vector<CargoSite> sites;
        std::string error;
    };

    std::shared_ptr<const core::Vfs> vfs_;
    core::VirtualPath route_;
    formats::RoutePosition origin_;
    std::future<std::unique_ptr<Loaded>> loading_;
    std::unique_ptr<Loaded> loaded_;
    static const Loaded empty_;
};

} // namespace openrts::viewer
