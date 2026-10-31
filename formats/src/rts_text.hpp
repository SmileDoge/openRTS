#pragma once

// The RTS way of reading its text files (rts1.exe 0x4578C0 and helpers),
// used where RTS semantics depend on it: .wag, .eng, .con.
// - The text is lower-cased first.
// - A keyword is searched in a range of the text, in document order. It
//   matches with whitespace or the range start before it and "(" after it
//   (whitespace allowed), and yields the content of its block.
// - Values are read with C runtime semantics (`_wtof`, `_wtoi`): the leading
//   number, the rest of the token ignored.
// Internal to openrts_formats.

#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "text_tokenizer.hpp"

namespace openrts::formats::detail {

struct TextRange {
    std::size_t begin{};
    std::size_t end{};
    [[nodiscard]] bool empty() const noexcept { return end <= begin; }
};

[[nodiscard]] inline bool rts_space(const char c) {
    return static_cast<unsigned char>(c) <= 0x20;
}

// Content of the first block `keyword ( ... )` starting inside `within`.
// The content excludes the brackets and surrounding whitespace; quotes are
// respected when matching the closing bracket.
[[nodiscard]] inline std::optional<TextRange> rts_find(std::string_view text, std::string_view keyword,
                                                      TextRange within) {
    const auto size = text.size();
    if (keyword.size() + 1 > size) return std::nullopt;
    const auto search_end = (std::min)(within.end, size);
    for (std::size_t i = within.begin; i < search_end && i + keyword.size() < size; ++i) {
        if (i > 0 && !rts_space(text[i - 1])) continue;
        if (text.compare(i, keyword.size(), keyword) != 0) continue;
        auto j = i + keyword.size();
        while (j < size && rts_space(text[j])) ++j;
        if (j >= size || text[j] != '(') continue;
        ++j; // past "("
        if (j >= search_end) return std::nullopt;
        while (j < size && rts_space(text[j])) ++j;
        const auto begin = j;
        int depth = 0;
        bool quoted = false;
        for (; j < search_end; ++j) {
            const char c = text[j];
            if (c == '"') quoted = !quoted;
            if (quoted) continue;
            if (c == '(') {
                ++depth;
            } else if (c == ')') {
                if (depth == 0) break;
                --depth;
            }
        }
        auto end = j;
        while (end > begin && rts_space(text[end - 1])) --end;
        return TextRange{begin, end};
    }
    return std::nullopt;
}

[[nodiscard]] inline std::optional<TextRange> rts_find(std::string_view text, std::string_view keyword) {
    return rts_find(text, keyword, {0, text.size()});
}

// Position of the next token after the one at `position` (RTS 0x458190):
// skips the token (a quoted string as one) and the whitespace after it.
[[nodiscard]] inline std::size_t rts_next(std::string_view text, std::size_t position, std::size_t end) {
    end = (std::min)(end, text.size());
    if (position < end && text[position] == '"') {
        ++position;
        while (position < end && text[position] != '"') ++position;
        if (position < end) ++position;
    } else {
        while (position < end && !rts_space(text[position]) && text[position] != '(' && text[position] != ')') ++position;
    }
    while (position < end && rts_space(text[position])) ++position;
    return position;
}

// The token at `position` (RTS 0x458688): a quoted string without its
// quotes, or the characters up to whitespace or a bracket.
[[nodiscard]] inline std::string rts_token(std::string_view text, std::size_t position, std::size_t end) {
    end = (std::min)(end, text.size());
    while (position < end && rts_space(text[position])) ++position;
    if (position < end && text[position] == '"') {
        const auto start = ++position;
        while (position < end && text[position] != '"') ++position;
        return std::string(text.substr(start, position - start));
    }
    const auto start = position;
    while (position < end && !rts_space(text[position]) && text[position] != '(' && text[position] != ')') ++position;
    return std::string(text.substr(start, position - start));
}

// `_wtof`: the leading number of the text (0 when there is none).
[[nodiscard]] inline double rts_atof(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size() && rts_space(text[i])) ++i;
    if (i < text.size() && text[i] == '"') ++i;
    std::string number;
    for (; i < text.size() && number.size() < 64; ++i) {
        const char c = text[i];
        if ((c >= '0' && c <= '9') || c == '.' || c == '+' || c == '-' || c == 'e' || c == 'E') {
            number.push_back(c);
        } else {
            break;
        }
    }
    // strtod takes the longest valid prefix, as _wtof does.
    return number.empty() ? 0.0 : std::strtod(number.c_str(), nullptr);
}

[[nodiscard]] inline int rts_atoi(std::string_view text) {
    return static_cast<int>(rts_atof(text));
}

// RTS quantity parser (0x45A8AC) on one token: the leading number, scaled
// when the token contains kn, kw (x1000), mw (x1e6), cm (x0.01), mph, km
// (km/h), gal, or ends with t (tonnes). No other units are converted.
[[nodiscard]] inline double rts_quantity(std::string_view token) {
    std::size_t length = 0;
    while (length < token.size() && !rts_space(token[length])) ++length;
    const auto word = token.substr(0, length);
    double value = rts_atof(word);
    const auto has = [&](std::string_view unit) { return word.find(unit) != std::string_view::npos; };
    if (has("kn")) value *= 1000.0;
    if (has("cm")) value *= 0.01;
    if (has("kw")) value *= 1000.0;
    if (has("mw")) value *= 1000000.0;
    if (has("mph")) value = value * 1609.0 * (1.0 / 3600.0);
    if (has("km")) value *= 1.0 / 3.6;
    if (has("gal")) value *= 3.79;
    if (const auto t = word.find('t'); t != std::string_view::npos && t + 1 == word.size()) value *= 1000.0;
    return value;
}

// RTS product parser (0x45AA40): `a*b` as two quantities multiplied.
[[nodiscard]] inline double rts_product(std::string_view token) {
    std::size_t length = 0;
    while (length < token.size() && !rts_space(token[length])) ++length;
    const auto word = token.substr(0, length);
    const auto star = word.find('*');
    if (star == std::string_view::npos || star + 1 >= word.size()) return rts_quantity(word);
    return rts_quantity(word.substr(0, star)) * rts_quantity(word.substr(star + 1));
}

// Removes Comment / Skip / _Skip blocks by blanking them (RTS 0x459B38).
inline void rts_blank_comments(std::string& text) {
    for (const std::string_view keyword : {"comment", "skip", "_skip"}) {
        std::size_t from = 0;
        while (const auto block = rts_find(text, keyword, {from, text.size()})) {
            auto start = block->begin;
            while (start > 0 && text.compare(start, keyword.size(), keyword) != 0) --start;
            auto stop = block->end;
            while (stop < text.size() && text[stop] != ')') ++stop;
            for (auto i = start; i <= stop && i < text.size(); ++i) text[i] = ' ';
            from = stop;
        }
    }
}

// Merges the blocks `sections` of a companion text (.rtseng, .rtswag,
// RTSInclude files) into `text`, as RTS 0x566F68 does: the companion's block
// content is inserted at the start of the same block in `text` (so its values
// are found first), or the whole block is appended when `text` lacks it.
inline void rts_merge(std::string& text, std::string_view companion, const std::vector<std::string_view>& sections) {
    for (const auto section : sections) {
        const auto source = rts_find(companion, section);
        if (!source || source->empty()) continue;
        const auto content = std::string(companion.substr(source->begin, source->end - source->begin));
        if (const auto target = rts_find(text, section)) {
            text.insert(target->begin, content + "\r\n");
        } else {
            text += "\r\n" + std::string(section) + " (\r\n" + content + "\r\n)\r\n";
        }
    }
}

// Decodes and lower-cases a file as RTS reads it.
[[nodiscard]] inline std::optional<std::string> rts_text(std::string_view bytes, std::string& error) {
    auto text = decode_text(bytes, error);
    if (!text) return std::nullopt;
    for (auto& c : *text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

} // namespace openrts::formats::detail
