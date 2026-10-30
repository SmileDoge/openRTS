#include <openrts/formats/shape_description.hpp>

#include <cctype>
#include <fstream>
#include <iterator>
#include <utility>

namespace openrts::formats {
namespace {

class TokenReader {
public:
    explicit TokenReader(std::string_view text) : text_(text) {}

    [[nodiscard]] std::optional<std::string> next() {
        skip_whitespace_and_comments();
        if (position_ >= text_.size()) return std::nullopt;
        if (text_[position_] == '(' || text_[position_] == ')') return std::string(1, text_[position_++]);
        if (text_[position_] == '"') return read_quoted();
        return read_bare();
    }

private:
    void skip_whitespace_and_comments() {
        for (;;) {
            while (position_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[position_]))) ++position_;
            if (position_ + 1 >= text_.size() || text_[position_] != '/' || text_[position_ + 1] != '/') return;
            position_ += 2;
            while (position_ < text_.size() && text_[position_] != '\n') ++position_;
        }
    }

    [[nodiscard]] std::string read_quoted() {
        ++position_;
        std::string value;
        while (position_ < text_.size() && text_[position_] != '"') value.push_back(text_[position_++]);
        if (position_ < text_.size()) ++position_;
        return value;
    }

    [[nodiscard]] std::string read_bare() {
        const auto begin = position_;
        while (position_ < text_.size() && !std::isspace(static_cast<unsigned char>(text_[position_]))
               && text_[position_] != '(' && text_[position_] != ')') ++position_;
        return std::string(text_.substr(begin, position_ - begin));
    }

    std::string_view text_;
    std::size_t position_ = 0;
};

[[nodiscard]] bool equal_ignore_case(const std::string& left, std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(left[index]))
            != std::tolower(static_cast<unsigned char>(right[index]))) return false;
    }
    return true;
}

void append_utf8(std::string& output, const char32_t code_point) {
    if (code_point <= 0x7F) {
        output.push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7FF) {
        output.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
        output.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else if (code_point <= 0xFFFF) {
        output.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
        output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else {
        output.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
        output.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    }
}

[[nodiscard]] std::optional<std::string> decode_file_text(const std::string& bytes, std::string& error) {
    if (bytes.size() < 2 || static_cast<unsigned char>(bytes[0]) != 0xFF
        || static_cast<unsigned char>(bytes[1]) != 0xFE) {
        return bytes;
    }
    if (bytes.size() % 2 != 0) {
        error = "UTF-16LE SD file has an odd byte count.";
        return std::nullopt;
    }

    std::string utf8;
    for (std::size_t index = 2; index < bytes.size(); index += 2) {
        const auto unit = static_cast<char16_t>(static_cast<unsigned char>(bytes[index])
                                                | (static_cast<unsigned char>(bytes[index + 1]) << 8));
        char32_t code_point = unit;
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            if (index + 3 >= bytes.size()) {
                error = "UTF-16LE SD file ends in a high surrogate.";
                return std::nullopt;
            }
            const auto low = static_cast<char16_t>(static_cast<unsigned char>(bytes[index + 2])
                                                   | (static_cast<unsigned char>(bytes[index + 3]) << 8));
            if (low < 0xDC00 || low > 0xDFFF) {
                error = "UTF-16LE SD file has an invalid surrogate pair.";
                return std::nullopt;
            }
            code_point = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
            index += 2;
        } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
            error = "UTF-16LE SD file has an unpaired low surrogate.";
            return std::nullopt;
        }
        append_utf8(utf8, code_point);
    }
    return utf8;
}

} // namespace

ShapeDescriptionResult parse_shape_description(std::string_view text) {
    TokenReader tokens(text);
    while (const auto token = tokens.next()) {
        if (!equal_ignore_case(*token, "shape")) continue;
        const auto open_paren = tokens.next();
        const auto shape_name = tokens.next();
        if (!open_paren || *open_paren != "(" || !shape_name || *shape_name == ")") {
            return {.error = "Malformed Shape(...) node in SD file."};
        }
        return {.value = ShapeDescription{.shape_file = *shape_name}};
    }
    return {.error = "SD file does not contain a Shape(...) node."};
}

ShapeDescriptionResult load_shape_description(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) return {.error = "Cannot open SD file: " + file.string()};
    const std::string text(std::istreambuf_iterator<char>(input), {});
    std::string decode_error;
    const auto decoded = decode_file_text(text, decode_error);
    if (!decoded) return {.error = std::move(decode_error)};
    return parse_shape_description(*decoded);
}

ShapeDescriptionResult load_shape_description(const core::Vfs& vfs, const core::VirtualPath& file) {
    std::string error;
    const auto bytes = vfs.read(file, &error);
    if (!bytes) return {.error = std::move(error)};
    std::string decode_error;
    const auto decoded = decode_file_text(*bytes, decode_error);
    if (!decoded) return {.error = std::move(decode_error)};
    return parse_shape_description(*decoded);
}

} // namespace openrts::formats
