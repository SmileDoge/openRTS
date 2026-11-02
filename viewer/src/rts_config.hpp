#pragma once

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace openrts::viewer {

// A positive number from RTS's config.ini (UTF-16 LE as RTS writes it, or
// 8-bit text): the key, case-insensitive, at the start of a line, then "=".
[[nodiscard]] inline std::optional<float> rts_config_value(const std::string_view config, std::string key) {
    std::string text;
    const bool wide = config.size() >= 2 && static_cast<unsigned char>(config[0]) == 0xFF
                      && static_cast<unsigned char>(config[1]) == 0xFE;
    for (std::size_t i = wide ? 2 : 0; i < config.size(); i += wide ? 2 : 1) {
        text.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(config[i]))));
    }
    std::transform(key.begin(), key.end(), key.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (std::size_t at = text.find(key); at != std::string::npos; at = text.find(key, at + 1)) {
        if (at > 0 && text[at - 1] != '\n' && text[at - 1] != '\r' && text[at - 1] != ' ') continue;
        auto rest = at + key.size();
        while (rest < text.size() && text[rest] == ' ') ++rest;
        if (rest >= text.size() || text[rest] != '=') continue;
        const float v = std::strtof(text.c_str() + rest + 1, nullptr);
        if (v > 0.0f) return v;
    }
    return std::nullopt;
}

// RTS's config.ini of an installation (RTS\config.ini, else config.ini at
// its root); empty when there is none.
[[nodiscard]] inline std::string read_rts_config(const std::filesystem::path& root) {
    if (root.empty()) return {};
    for (const auto& file : {root / "RTS" / "config.ini", root / "config.ini"}) {
        std::ifstream in(file, std::ios::binary);
        if (!in) continue;
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    return {};
}

} // namespace openrts::viewer
