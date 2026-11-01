#include "viewer_state.hpp"

#include <fstream>
#include <map>

namespace openrts::viewer {
namespace {

const std::filesystem::path state_file = "viewer_state.ini";

[[nodiscard]] std::map<std::string, std::string> read_state() {
    std::map<std::string, std::string> values;
    std::ifstream input(state_file, std::ios::binary);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto equals = line.find('=');
        if (equals == std::string::npos) continue;
        values[line.substr(0, equals)] = line.substr(equals + 1);
    }
    return values;
}

} // namespace

std::optional<std::string> remembered(const std::string_view key) {
    const auto values = read_state();
    const auto it = values.find(std::string(key));
    if (it == values.end()) return std::nullopt;
    return it->second;
}

void remember(const std::string_view key, const std::string_view value) {
    auto values = read_state();
    values[std::string(key)] = std::string(value);
    std::ofstream output(state_file, std::ios::binary | std::ios::trunc);
    for (const auto& [name, text] : values) output << name << '=' << text << '\n';
}

std::filesystem::path remembered_folder(const std::string_view key) {
    const auto value = remembered(key);
    if (!value) return {};
    return std::filesystem::path(std::u8string(value->begin(), value->end()));
}

void remember_folder(const std::string_view key, const std::filesystem::path& folder) {
    const auto text = folder.u8string();
    remember(key, std::string(text.begin(), text.end()));
}

} // namespace openrts::viewer
