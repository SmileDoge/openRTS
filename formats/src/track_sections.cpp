#include <openrts/formats/track_sections.hpp>

#include <cmath>
#include <numbers>

#include "text_tokenizer.hpp"

namespace openrts::formats {
namespace {

using detail::equals_ignore_case;
using detail::Tokenizer;

[[nodiscard]] std::optional<double> number(Tokenizer& tokens) {
    const auto token = tokens.next();
    if (!token || *token == "(" || *token == ")") return std::nullopt;
    return detail::parse_double(*token);
}

[[nodiscard]] std::optional<std::uint32_t> integer(Tokenizer& tokens) {
    const auto value = number(tokens);
    if (!value || *value < 0) return std::nullopt;
    return static_cast<std::uint32_t>(*value);
}

// Parses `TrackSection ( index ... )` after its opening bracket.
void read_section(Tokenizer& tokens, TrackSectionLibrary& library) {
    const auto index = integer(tokens);
    TrackSectionDefinition section{.index = index.value_or(0)};
    std::string name;
    while (const auto token = tokens.next()) {
        if (*token == ")") break;
        if (*token != "(") {
            name = *token;
            continue;
        }
        const auto key = &name;
        if (equals_ignore_case(*key, "SectionSize")) {
            section.gauge = static_cast<float>(number(tokens).value_or(0.0));
            if (!section.curved) section.length = static_cast<float>(number(tokens).value_or(0.0));
        } else if (equals_ignore_case(*key, "SectionCurve")) {
            section.radius = static_cast<float>(number(tokens).value_or(0.0));
            section.angle_degrees = static_cast<float>(number(tokens).value_or(0.0));
            section.curved = true;
            // RTS (0x483694): length = |angle| * pi / 180 * radius.
            section.length = std::abs(section.angle_degrees) * std::numbers::pi_v<float> / 180.0f * section.radius;
        } else if (equals_ignore_case(*key, "SectionSkew")) {
            section.angle_degrees = static_cast<float>(number(tokens).value_or(0.0));
            section.skewed = true;
        }
        tokens.skip_block();
    }
    if (index) library.sections[section.index] = section;
}

void read_shape(Tokenizer& tokens, TrackSectionLibrary& library) {
    const auto index = integer(tokens);
    TrackShapeDefinition shape{.index = index.value_or(0)};
    std::string name;
    while (const auto token = tokens.next()) {
        if (*token == ")") break;
        if (*token != "(") {
            name = *token;
            continue;
        }
        const auto key = &name;
        if (equals_ignore_case(*key, "FileName")) {
            if (const auto file = tokens.next(); file && *file != ")") shape.file_name = *file;
        } else if (equals_ignore_case(*key, "NumPaths")) {
            shape.path_count = integer(tokens).value_or(0);
        } else if (equals_ignore_case(*key, "MainRoute")) {
            shape.main_route = integer(tokens);
        } else if (equals_ignore_case(*key, "ClearanceDist")) {
            shape.clearance_distance = static_cast<float>(number(tokens).value_or(0.0));
        } else if (equals_ignore_case(*key, "SectionIdx")) {
            TrackShapePath path;
            const auto count = integer(tokens).value_or(0);
            for (auto& value : path.start) value = static_cast<float>(number(tokens).value_or(0.0));
            path.heading_degrees = static_cast<float>(number(tokens).value_or(0.0));
            for (std::uint32_t i = 0; i < count; ++i) {
                if (const auto section = integer(tokens)) path.sections.push_back(*section);
            }
            shape.paths.push_back(std::move(path));
        } else if (equals_ignore_case(*key, "TunnelShape")) {
            shape.tunnel = true;
        } else if (equals_ignore_case(*key, "RoadShape")) {
            shape.road = true;
        } else if (equals_ignore_case(*key, "CrossoverShape")) {
            shape.crossover = true;
        } else if (equals_ignore_case(*key, "ManualJunctionShape")) {
            shape.manual_junction = true;
        } else if (equals_ignore_case(*key, "XoverPts")) {
            const auto count = integer(tokens).value_or(0);
            for (std::uint32_t i = 0; i < count; ++i) {
                const auto point = tokens.next();
                if (!point || !equals_ignore_case(*point, "XoverPt") || !tokens.open_block()) break;
                std::array<float, 3> xyz{};
                for (auto& value : xyz) value = static_cast<float>(number(tokens).value_or(0.0));
                shape.crossover_points.push_back(xyz);
                tokens.skip_block();
            }
        }
        tokens.skip_block();
    }
    if (index) library.shapes[shape.index] = std::move(shape);
}

// Reads the entries of a `TrackSections` / `TrackShapes` block.
// Blocks are named by the word before their bracket; bare words (counts,
// headers) are skipped.
template <typename ReadEntry>
void read_list(Tokenizer& tokens, const std::string_view entry, ReadEntry read_entry) {
    std::string name;
    while (const auto token = tokens.next()) {
        if (*token == ")") return;
        if (*token != "(") {
            name = *token;
            continue;
        }
        if (equals_ignore_case(name, entry)) {
            read_entry(tokens);
        } else {
            tokens.skip_block(); // _Skip, _INFO, comment, unknown
        }
        name.clear();
    }
}

} // namespace

const TrackSectionDefinition* TrackSectionLibrary::section(const std::uint32_t index) const {
    const auto it = sections.find(index);
    return it == sections.end() ? nullptr : &it->second;
}

const TrackShapeDefinition* TrackSectionLibrary::shape(const std::uint32_t index) const {
    const auto it = shapes.find(index);
    return it == shapes.end() ? nullptr : &it->second;
}

void TrackSectionLibrary::merge(TrackSectionLibrary other) {
    for (auto& [index, section] : other.sections) sections[index] = section;
    for (auto& [index, shape] : other.shapes) shapes[index] = std::move(shape);
}

TrackSectionLibrary parse_track_sections(const std::string_view bytes) {
    TrackSectionLibrary library;
    std::string error;
    const auto text = detail::decode_text(bytes, error);
    if (!text) {
        library.error = error;
        return library;
    }
    Tokenizer tokens(*text);
    std::string name;
    while (const auto token = tokens.next()) {
        if (*token == ")") continue;
        if (*token != "(") {
            name = *token;
            continue;
        }
        if (equals_ignore_case(name, "TrackSections")) {
            read_list(tokens, "TrackSection", [&](Tokenizer& t) { read_section(t, library); });
        } else if (equals_ignore_case(name, "TrackShapes")) {
            read_list(tokens, "TrackShape", [&](Tokenizer& t) { read_shape(t, library); });
        } else {
            tokens.skip_block();
        }
        name.clear();
    }
    return library;
}

TrackSectionLibrary load_track_sections(const core::Vfs& vfs, const core::VirtualPath& route) {
    std::string error;
    const auto global = vfs.read(*core::VirtualPath::parse("GLOBAL/tsection.dat"), &error);
    if (!global) return {.error = error};
    auto library = parse_track_sections(*global);
    if (const auto local = route.resolve(std::string_view("tsection.dat"))) {
        if (const auto bytes = vfs.read(*local)) library.merge(parse_track_sections(*bytes));
    }
    return library;
}

} // namespace openrts::formats
