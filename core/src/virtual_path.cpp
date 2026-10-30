#include <openrts/core/virtual_path.hpp>

#include <vector>

namespace openrts::core {
namespace {

// Decodes one UTF-8 sequence at `at`; returns the code point and advances, or
// returns nullopt for an invalid sequence (the caller copies the byte).
[[nodiscard]] std::optional<char32_t> decode_utf8(const std::string_view text, std::size_t& at) {
    const auto lead = static_cast<unsigned char>(text[at]);
    std::size_t length = 0;
    char32_t code = 0;
    if (lead < 0x80) {
        ++at;
        return lead;
    }
    if ((lead & 0xE0) == 0xC0) {
        length = 2;
        code = lead & 0x1F;
    } else if ((lead & 0xF0) == 0xE0) {
        length = 3;
        code = lead & 0x0F;
    } else if ((lead & 0xF8) == 0xF0) {
        length = 4;
        code = lead & 0x07;
    } else {
        return std::nullopt;
    }
    if (at + length > text.size()) return std::nullopt;
    for (std::size_t i = 1; i < length; ++i) {
        const auto next = static_cast<unsigned char>(text[at + i]);
        if ((next & 0xC0) != 0x80) return std::nullopt;
        code = (code << 6) | (next & 0x3F);
    }
    at += length;
    return code;
}

void append_utf8(std::string& out, const char32_t code) {
    if (code < 0x80) {
        out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
}

[[nodiscard]] char32_t fold(const char32_t code) {
    if (code >= U'A' && code <= U'Z') return code + 0x20;
    if (code >= 0xC0 && code <= 0xDE && code != 0xD7) return code + 0x20; // Latin-1
    if (code >= 0x410 && code <= 0x42F) return code + 0x20;               // А..Я
    if (code >= 0x400 && code <= 0x40F) return code + 0x50;               // Ѐ..Џ (Ё)
    return code;
}

[[nodiscard]] bool is_separator(const char c) {
    return c == '/' || c == '\\';
}

// Splits `text` into components and applies them to `parts`. Returns false
// when the text is rooted.
[[nodiscard]] bool apply_components(const std::string_view text, std::vector<std::string>& parts, bool& clamped) {
    if (!text.empty() && is_separator(text.front())) return false;          // `/x`, `\\server`
    if (text.size() >= 2 && text[1] == ':') return false;                    // `C:x`, `C:\x`

    std::size_t at = 0;
    while (at <= text.size()) {
        auto end = at;
        while (end < text.size() && !is_separator(text[end])) ++end;
        const auto part = text.substr(at, end - at);
        if (part.empty() || part == ".") {
            // skip
        } else if (part == "..") {
            if (parts.empty()) {
                clamped = true;
            } else {
                parts.pop_back();
            }
        } else {
            parts.emplace_back(part);
        }
        at = end + 1;
    }
    return true;
}

} // namespace

std::string fold_case(const std::string_view utf8) {
    std::string result;
    result.reserve(utf8.size());
    std::size_t at = 0;
    while (at < utf8.size()) {
        const auto start = at;
        if (const auto code = decode_utf8(utf8, at)) {
            append_utf8(result, fold(*code));
        } else {
            result.push_back(utf8[start]);
            at = start + 1;
        }
    }
    return result;
}

std::optional<VirtualPath> VirtualPath::parse(const std::string_view utf8) {
    return VirtualPath{}.resolve(utf8);
}

std::optional<VirtualPath> VirtualPath::from_filesystem(const std::filesystem::path& relative) {
    const auto text = relative.generic_u8string();
    return parse(std::string_view(reinterpret_cast<const char*>(text.data()), text.size()));
}

std::optional<VirtualPath> VirtualPath::resolve(const std::string_view relative) const {
    std::vector<std::string> parts;
    std::size_t at = 0;
    while (at < text_.size()) {
        auto end = text_.find('/', at);
        if (end == std::string::npos) end = text_.size();
        parts.emplace_back(text_.substr(at, end - at));
        at = end + 1;
    }

    bool clamped = clamped_;
    if (!apply_components(relative, parts, clamped)) return std::nullopt;

    VirtualPath result;
    for (const auto& part : parts) {
        if (!result.text_.empty()) result.text_.push_back('/');
        result.text_ += part;
    }
    result.key_ = fold_case(result.text_);
    result.clamped_ = clamped;
    return result;
}

std::optional<VirtualPath> VirtualPath::resolve_filesystem(const std::filesystem::path& relative) const {
    const auto text = relative.generic_u8string();
    return resolve(std::string_view(reinterpret_cast<const char*>(text.data()), text.size()));
}

VirtualPath VirtualPath::parent() const {
    const auto slash = text_.rfind('/');
    if (slash == std::string::npos) return {};
    VirtualPath result;
    result.text_ = text_.substr(0, slash);
    result.key_ = key_.substr(0, key_.rfind('/'));
    return result;
}

std::string_view VirtualPath::filename() const {
    const auto slash = text_.rfind('/');
    return slash == std::string::npos ? std::string_view(text_) : std::string_view(text_).substr(slash + 1);
}

std::string_view VirtualPath::extension() const {
    const auto name = filename();
    const auto dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0) return {};
    return name.substr(dot);
}

VirtualPath VirtualPath::with_extension(const std::string_view ext) const {
    const auto old = extension();
    std::string text = text_.substr(0, text_.size() - old.size());
    text += ext;
    return *parse(text);
}

bool VirtualPath::is_within(const VirtualPath& directory) const {
    if (directory.key_.empty()) return true;
    if (key_.size() < directory.key_.size() || key_.compare(0, directory.key_.size(), directory.key_) != 0) return false;
    return key_.size() == directory.key_.size() || key_[directory.key_.size()] == '/';
}

std::filesystem::path VirtualPath::to_filesystem() const {
    return std::filesystem::path(std::u8string(text_.begin(), text_.end()));
}

} // namespace openrts::core
