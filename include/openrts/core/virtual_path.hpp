#pragma once

#include <compare>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace openrts::core {

// Case folding used for content lookup: ASCII, Latin-1 and Cyrillic letters
// are folded to lower case, everything else is kept. Input and output are
// UTF-8; invalid sequences pass through unchanged.
[[nodiscard]] std::string fold_case(std::string_view utf8);

// A path in the content namespace, relative to the installation root, e.g.
// `ROUTES/Kuban_RTS/SHAPES/a.s`. Separators are '/', `.` and `..` are resolved
// lexically (as Windows and RTS do), so intermediate folders need not exist.
// Lookups compare `key()`, the case-folded form; `str()` keeps the spelling.
class VirtualPath {
public:
    VirtualPath() = default; // the root

    // Accepts '\' or '/' separators. Refuses rooted paths (`/x`, `C:\x`,
    // `\\server`). `..` above the root is clamped to the root and reported by
    // clamped().
    [[nodiscard]] static std::optional<VirtualPath> parse(std::string_view utf8);
    [[nodiscard]] static std::optional<VirtualPath> from_filesystem(const std::filesystem::path& relative);

    // Resolves `relative` against this path taken as a directory.
    [[nodiscard]] std::optional<VirtualPath> resolve(std::string_view relative) const;
    [[nodiscard]] std::optional<VirtualPath> resolve_filesystem(const std::filesystem::path& relative) const;

    [[nodiscard]] VirtualPath parent() const;
    [[nodiscard]] std::string_view filename() const;
    // Extension including the dot, case as written ("" when none).
    [[nodiscard]] std::string_view extension() const;
    // Same path with the extension replaced (`ext` includes the dot).
    [[nodiscard]] VirtualPath with_extension(std::string_view ext) const;

    [[nodiscard]] const std::string& str() const noexcept { return text_; }
    [[nodiscard]] const std::string& key() const noexcept { return key_; }
    [[nodiscard]] bool empty() const noexcept { return text_.empty(); }
    [[nodiscard]] bool clamped() const noexcept { return clamped_; }
    // True when this path equals `directory` or lies below it.
    [[nodiscard]] bool is_within(const VirtualPath& directory) const;
    // Native relative path (for folder sources and extraction).
    [[nodiscard]] std::filesystem::path to_filesystem() const;

    friend bool operator==(const VirtualPath& a, const VirtualPath& b) noexcept { return a.key_ == b.key_; }
    friend std::strong_ordering operator<=>(const VirtualPath& a, const VirtualPath& b) noexcept {
        return a.key_ <=> b.key_;
    }

private:
    std::string text_;
    std::string key_;
    bool clamped_ = false;
};

struct VirtualPathHash {
    [[nodiscard]] std::size_t operator()(const VirtualPath& path) const noexcept {
        return std::hash<std::string>{}(path.key());
    }
};

} // namespace openrts::core
