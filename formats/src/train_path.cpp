#include <openrts/formats/train_path.hpp>

#include <algorithm>
#include <cctype>

#include "rts_text.hpp"

namespace openrts::formats {
namespace {

using detail::rts_find;
using detail::rts_next;
using detail::rts_token;
using detail::TextRange;

// Tokens of a block, numbers as written.
[[nodiscard]] std::vector<std::string> tokens(std::string_view text, const TextRange range) {
    std::vector<std::string> result;
    for (auto position = range.begin; position < range.end; position = rts_next(text, position, range.end)) {
        auto token = rts_token(text, position, range.end);
        if (token.empty()) break;
        result.push_back(std::move(token));
    }
    return result;
}

[[nodiscard]] std::uint32_t to_u32(const std::string& token, const int base = 10) {
    return static_cast<std::uint32_t>(std::strtoul(token.c_str(), nullptr, base));
}

// Repeated blocks `keyword ( ... )` inside `within`, in order.
template <typename F>
void each(std::string_view text, std::string_view keyword, TextRange within, F&& visit) {
    while (const auto block = rts_find(text, keyword, within)) {
        visit(*block);
        within.begin = block->end + 1;
        if (within.begin >= within.end) break;
    }
}

} // namespace

TrainPath parse_train_path(const std::string_view bytes) {
    TrainPath path;
    std::string error;
    auto original = detail::decode_text(bytes, error);
    if (!original) return {.error = error};
    // Keywords are matched on a lower-cased copy; names come from the
    // original text at the same positions (lower-casing keeps the length).
    std::string lower = *original;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const std::string_view t = lower;
    const std::string_view o = *original;

    if (const auto points = rts_find(t, "trackpdps")) {
        each(t, "trackpdp", *points, [&](const TextRange block) {
            const auto v = tokens(t, block);
            if (v.size() < 5) return;
            PathPoint point;
            point.position = {.tile_x = std::atoi(v[0].c_str()),
                              .tile_z = std::atoi(v[1].c_str()),
                              .x = std::atof(v[2].c_str()),
                              .y = std::atof(v[3].c_str()),
                              .z = std::atof(v[4].c_str())};
            if (v.size() > 5) point.kind = to_u32(v[5]);
            if (v.size() > 6) point.flags = to_u32(v[6]);
            path.points.push_back(point);
        });
    }
    const auto track_path = rts_find(t, "trackpath");
    if (!track_path) return {.error = "no TrackPath block"};
    const auto text_of = [&](std::string_view keyword) {
        const auto block = rts_find(t, keyword, *track_path);
        return block ? rts_token(o, block->begin, block->end) : std::string{};
    };
    path.id = text_of("trpathname");
    path.name = text_of("name");
    path.start = text_of("trpathstart");
    path.end = text_of("trpathend");
    if (const auto nodes = rts_find(t, "trpathnodes", *track_path)) {
        each(t, "trpathnode", *nodes, [&](const TextRange block) {
            const auto v = tokens(t, block);
            if (v.size() < 4) return;
            path.nodes.push_back({.flags = to_u32(v[0], 16),
                                  .next = to_u32(v[1]),
                                  .next_siding = to_u32(v[2]),
                                  .point = to_u32(v[3])});
        });
    }
    if (path.nodes.empty()) path.error = "no TrPathNodes";
    for (const auto& node : path.nodes) {
        if (node.point >= path.points.size()) {
            path.error = "TrPathNode refers to a missing TrackPDP";
            break;
        }
    }
    return path;
}

std::vector<TrainPath> load_train_paths(const core::Vfs& vfs, const core::VirtualPath& route) {
    std::vector<TrainPath> paths;
    const auto directory = route.resolve(std::string_view("PATHS"));
    if (!directory) return paths;
    for (const auto& entry : vfs.list(*directory)) {
        if (entry.directory) continue;
        const std::string name(entry.path.filename());
        if (name.size() < 5) continue;
        auto extension = name.substr(name.size() - 4);
        for (auto& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (extension != ".pat") continue;
        const auto bytes = vfs.read(entry.path);
        TrainPath path = bytes ? parse_train_path(*bytes) : TrainPath{.error = "cannot read"};
        path.file = name.substr(0, name.size() - 4);
        paths.push_back(std::move(path));
    }
    std::sort(paths.begin(), paths.end(), [](const TrainPath& a, const TrainPath& b) { return a.file < b.file; });
    return paths;
}

} // namespace openrts::formats
