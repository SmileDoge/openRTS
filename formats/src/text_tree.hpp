#pragma once

// Tree view of the MSTS/RTS bracketed text format: `name ( values blocks )`.
// Names are lower-cased; `Skip ( ... )` and `Comment ( ... )` blocks are
// dropped. Counts written in the files are kept as plain values, so readers
// can ignore them (hand-edited files often have wrong counts).
// Internal to openrts_formats.

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "text_tokenizer.hpp"

namespace openrts::formats::detail {

struct TextNode {
    std::string name; // lower case
    std::vector<std::string> values;
    std::vector<TextNode> children;

    // First child with this (lower-case) name.
    [[nodiscard]] const TextNode* child(std::string_view key) const {
        for (const auto& node : children) {
            if (node.name == key) return &node;
        }
        return nullptr;
    }
    [[nodiscard]] double number(std::size_t index, double fallback = 0.0) const {
        if (index >= values.size()) return fallback;
        return parse_double(values[index]).value_or(fallback);
    }
};

[[nodiscard]] inline std::string lower_ascii(std::string_view text) {
    std::string result(text);
    for (auto& character : result) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return result;
}

// Reads every top-level block of `text` into `root.children`. Unbalanced
// trailing text is tolerated; returns false only when nothing was read.
inline bool read_text_tree(std::string_view text, TextNode& root) {
    Tokenizer tokenizer(text);
    std::vector<std::string> tokens;
    while (auto token = tokenizer.next()) tokens.push_back(std::move(*token));

    std::vector<TextNode*> stack{&root};
    std::vector<bool> dropped{false};
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const auto& token = tokens[i];
        if (token == ")") {
            if (stack.size() > 1) {
                const bool drop = dropped.back();
                stack.pop_back();
                dropped.pop_back();
                if (drop) stack.back()->children.pop_back();
            }
            continue;
        }
        if (token == "(") {
            // A bracket without a name: an anonymous block, kept as a child.
            stack.back()->children.push_back({});
            stack.push_back(&stack.back()->children.back());
            dropped.push_back(false);
            continue;
        }
        if (i + 1 < tokens.size() && tokens[i + 1] == "(") {
            auto name = lower_ascii(token);
            const bool drop = name == "skip" || name == "comment" || name == "_skip";
            stack.back()->children.push_back({.name = std::move(name)});
            stack.push_back(&stack.back()->children.back());
            dropped.push_back(drop);
            ++i;
            continue;
        }
        stack.back()->values.push_back(token);
    }
    return !root.children.empty();
}

} // namespace openrts::formats::detail
