#pragma once

// Shared reader for the MSTS/RTS bracketed text format (.trk, .w, .env, ...).
// Internal to openrts_formats.

#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

namespace openrts::formats::detail {

[[nodiscard]] inline bool is_space(const char character) {
    return std::isspace(static_cast<unsigned char>(character)) != 0;
}

[[nodiscard]] inline bool is_bracket(const char character) {
    return character == '(' || character == ')';
}

// Splits the text into brackets, quoted strings and bare words.
class Tokenizer {
public:
    explicit Tokenizer(const std::string_view text) : text_(text) {}

    [[nodiscard]] std::optional<std::string> next() {
        while (at_ < text_.size() && is_space(text_[at_])) ++at_;
        if (at_ >= text_.size()) return std::nullopt;

        if (is_bracket(text_[at_])) return std::string(1, text_[at_++]);

        if (text_[at_] == '"') {
            const auto start = ++at_;
            while (at_ < text_.size() && text_[at_] != '"') ++at_;
            const auto length = at_ - start;
            if (at_ < text_.size()) ++at_;
            return std::string(text_.substr(start, length));
        }

        const auto start = at_;
        while (at_ < text_.size() && !is_space(text_[at_]) && !is_bracket(text_[at_])) ++at_;
        return std::string(text_.substr(start, at_ - start));
    }

    // One value of a block whose opening bracket was just read, as RTS reads
    // it (0x4578C0 finds the block's end, counting brackets outside quotes;
    // 0x458688 takes its first word, up to a space or that end, or a
    // quoted string): brackets inside the word are its characters. The rest
    // of the block is skipped; its closing bracket comes next. A screen
    // label's Alphabet needs it: the EP2D's "0123456789.АБВ...()+/Ё".
    [[nodiscard]] std::string raw_block_value() {
        std::size_t end = at_;
        int depth = 0;
        bool quoted = false;
        for (; end < text_.size(); ++end) {
            const char c = text_[end];
            if (c == '"') {
                quoted = !quoted;
            } else if (!quoted && c == '(') {
                ++depth;
            } else if (!quoted && c == ')') {
                if (depth == 0) break;
                --depth;
            }
        }
        std::size_t i = at_;
        while (i < end && is_space(text_[i])) ++i;
        std::string value;
        if (i < end && (text_[i] == '"' || text_[i] == '\'')) {
            const char quote = text_[i++];
            const auto start = i;
            while (i < end && text_[i] != quote) ++i;
            value = std::string(text_.substr(start, i - start));
        } else {
            const auto start = i;
            while (i < end && !is_space(text_[i])) ++i;
            value = std::string(text_.substr(start, i - start));
        }
        at_ = end;
        return value;
    }

    // Consumes an opening bracket; returns false when something else follows.
    [[nodiscard]] bool open_block() {
        const auto token = next();
        return token && *token == "(";
    }

    // Skips the remainder of a block whose opening bracket was already consumed.
    void skip_block() {
        for (int depth = 1; depth > 0;) {
            const auto token = next();
            if (!token) return;
            if (*token == "(") {
                ++depth;
            } else if (*token == ")") {
                --depth;
            }
        }
    }

private:
    std::string_view text_;
    std::size_t at_{};
};

// MSTS text files are either UTF-16LE with a BOM or plain 8-bit text. UTF-16
// is converted to UTF-8, so non-ASCII file names (Cyrillic routes and
// textures) survive for content lookup.
[[nodiscard]] inline std::optional<std::string> decode_text(const std::string_view bytes, std::string& error) {
    const bool utf16_bom = bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF
        && static_cast<unsigned char>(bytes[1]) == 0xFE;
    if (!utf16_bom) return std::string(bytes);

    if (bytes.size() % 2 != 0) {
        error = "UTF-16LE file has an odd byte count.";
        return std::nullopt;
    }

    const auto unit_at = [&](const std::size_t i) {
        return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i]) | (static_cast<unsigned char>(bytes[i + 1]) << 8));
    };
    std::string result;
    result.reserve(bytes.size() / 2);
    for (std::size_t i = 2; i < bytes.size(); i += 2) {
        auto code = unit_at(i);
        if (code >= 0xD800 && code <= 0xDBFF && i + 3 < bytes.size()) {
            const auto low = unit_at(i + 2);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                i += 2;
            }
        }
        if (code < 0x80) {
            result.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            result.push_back(static_cast<char>(0xC0 | (code >> 6)));
            result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            result.push_back(static_cast<char>(0xE0 | (code >> 12)));
            result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            result.push_back(static_cast<char>(0xF0 | (code >> 18)));
            result.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }
    return result;
}

[[nodiscard]] inline std::optional<std::string> read_text(const std::filesystem::path& file, std::string& error) {
    std::ifstream input(file, std::ios::binary);
    if (!input) {
        error = "Cannot open file: " + file.string();
        return std::nullopt;
    }
    const std::string bytes(std::istreambuf_iterator<char>(input), {});
    return decode_text(bytes, error);
}

[[nodiscard]] inline bool equals_ignore_case(const std::string_view a, const std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline std::optional<double> parse_double(const std::string& value) {
    char* end = nullptr;
    const auto result = std::strtod(value.c_str(), &end);
    if (!end || *end != '\0') return std::nullopt;
    return result;
}

template <typename Integer>
[[nodiscard]] std::optional<Integer> parse_integer(std::string_view value, const int base = 10) {
    Integer result{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result, base);
    if (error != std::errc{} || end != value.data() + value.size()) return std::nullopt;
    return result;
}

} // namespace openrts::formats::detail
