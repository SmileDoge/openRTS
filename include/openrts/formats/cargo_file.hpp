#pragma once

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include <openrts/core/vfs.hpp>

namespace openrts::formats {

struct CargoType {
    std::string id;
    std::string name;
    std::string texture;
    std::string loading_type;
    float density = 0.0f; // kg/m3
};

struct CargoIntake {
    std::array<float, 4> values{};
    bool dump = false;
};

struct CargoWagonType {
    std::string name;
    std::vector<std::string> filenames;
    std::vector<std::string> patterns;
    std::vector<std::string> loading_types;
    std::vector<std::array<float, 6>> areas;
    std::vector<CargoIntake> intakes;
    float capacity = 0.0f;
    float maximum_weight = 0.0f;
    float dump_speed = 0.0f;
    float dump_max_velocity = 0.0f;
};

struct CargoCatalog {
    std::vector<CargoType> cargoes;
    std::vector<CargoWagonType> wagons;
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
    [[nodiscard]] const CargoType* cargo(std::string_view id) const;
    [[nodiscard]] const CargoWagonType* wagon_for(std::string_view file_name) const;
};

[[nodiscard]] std::string_view cargo_mode(std::string_view cargo_id);
[[nodiscard]] bool cargo_accepts(const CargoWagonType& wagon, const CargoType& cargo);
[[nodiscard]] float cargo_capacity(const CargoWagonType& wagon, const CargoType& cargo, float width, float height,
                                   float length);

[[nodiscard]] CargoCatalog parse_cargo_catalog(std::string_view bytes);
[[nodiscard]] CargoCatalog load_cargo_catalog(const core::Vfs& vfs);

} // namespace openrts::formats
