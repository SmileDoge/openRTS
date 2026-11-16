#include "mod_files.hpp"

#include <cctype>
#include <fstream>
#include <optional>
#include <sstream>
#include <vector>

#include "viewer_state.hpp"

namespace openrts::viewer {
namespace fs = std::filesystem;
namespace {

[[nodiscard]] std::string lower(std::string_view text) {
    std::string out(text);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

[[nodiscard]] std::string trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return std::string(text);
}

[[nodiscard]] fs::path from_utf8(std::string_view text) { return fs::path(std::u8string(text.begin(), text.end())); }

} // namespace

std::string edits_mod() {
    auto mod = trim(remembered("inspector_mod").value_or(default_edits_mod));
    if (mod.empty() || mod.find_first_of("/\\:") != std::string::npos || mod == "." || mod == "..") mod = default_edits_mod;
    return mod;
}

fs::path mod_file(const fs::path& root, const std::string_view mod, const std::string_view virtual_file) {
    return root / "MODS_OPENRTS" / from_utf8(mod) / from_utf8(virtual_file);
}

std::string utf8_path(const fs::path& path) {
    const auto text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

// Adds `mod` to the installation's openrts.ini [Mods] (after the others,
// numbered when they are); false when it was there already.
bool list_mod(const fs::path& root, const std::string& mod) {
    const auto file = root / "openrts.ini";
    std::string text;
    {
        std::ifstream in(file, std::ios::binary);
        if (in) {
            std::ostringstream all;
            all << in.rdbuf();
            text = all.str();
        }
    }
    const bool crlf = text.find("\r\n") != std::string::npos;
    std::vector<std::string> lines;
    {
        std::string line;
        std::istringstream in(text);
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
    }
    std::optional<std::size_t> section, last;
    int highest = 0;
    bool numbered = false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::string line = trim(lines[i]);
        if (const auto comment = line.find(';'); comment != std::string::npos) line = trim(line.substr(0, comment));
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') {
            if (section && !last) last = section;
            if (lower(trim(line.substr(1, line.size() - 2))) == "mods") {
                section = i;
                last.reset();
            } else if (section) {
                break;
            }
            continue;
        }
        if (!section) continue;
        last = i;
        std::string value = line;
        if (const auto equals = line.find('='); equals != std::string::npos) {
            numbered = true;
            try {
                highest = (std::max)(highest, std::stoi(trim(line.substr(0, equals))));
            } catch (...) {
            }
            value = trim(line.substr(equals + 1));
        }
        if (lower(value) == lower(mod)) return false;
    }
    const std::string entry = numbered ? std::to_string(highest + 1) + " = " + mod : mod;
    if (section) {
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>((last ? *last : *section) + 1), entry);
    } else {
        if (!lines.empty() && !trim(lines.back()).empty()) lines.emplace_back();
        lines.emplace_back("[Mods]");
        lines.push_back(entry);
    }
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    for (const auto& line : lines) out << line << (crlf ? "\r\n" : "\n");
    return true;
}

} // namespace openrts::viewer
