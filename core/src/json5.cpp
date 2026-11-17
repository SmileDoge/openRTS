#include <openrts/core/json5.hpp>

#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>

namespace openrts::core::json5 {

const Value* Value::find(const std::string_view key) const noexcept {
    if (kind != Kind::object) return nullptr;
    for (auto it = members.rbegin(); it != members.rend(); ++it) {
        if (it->first == key) return &it->second;
    }
    return nullptr;
}

std::string Value::where() const {
    return "line " + std::to_string(line) + ", column " + std::to_string(column);
}

namespace {

constexpr int max_depth = 64;

class Parser {
public:
    explicit Parser(const std::string_view text) : text_(text) {}

    ParseResult run() {
        ParseResult result;
        // A UTF-8 byte order mark is skipped.
        if (text_.substr(0, 3) == "\xEF\xBB\xBF") advance(3);
        skip_blanks();
        auto value = parse_value(0);
        if (value && error_.empty()) {
            skip_blanks();
            if (pos_ < text_.size()) fail("something after the value");
        }
        if (!error_.empty() || !value) {
            result.error = error_.empty() ? std::string("no value") : error_;
            result.line = error_line_;
            result.column = error_column_;
            return result;
        }
        result.value = std::move(*value);
        return result;
    }

private:
    std::string_view text_;
    std::size_t pos_ = 0;
    int line_ = 1;
    int column_ = 1;
    std::string error_;
    int error_line_ = 0;
    int error_column_ = 0;

    [[nodiscard]] bool at_end() const noexcept { return pos_ >= text_.size(); }
    [[nodiscard]] char peek(const std::size_t ahead = 0) const noexcept {
        return pos_ + ahead < text_.size() ? text_[pos_ + ahead] : '\0';
    }
    void advance(const std::size_t count = 1) {
        for (std::size_t i = 0; i < count && pos_ < text_.size(); ++i) {
            const auto c = static_cast<unsigned char>(text_[pos_++]);
            if (c == '\n') {
                ++line_;
                column_ = 1;
            } else if ((c & 0xC0) != 0x80) { // a column per character, not per UTF-8 byte
                ++column_;
            }
        }
    }
    void fail(const std::string& what) {
        if (!error_.empty()) return;
        error_line_ = line_;
        error_column_ = column_;
        error_ = what + " (line " + std::to_string(line_) + ", column " + std::to_string(column_) + ")";
    }

    void skip_blanks() {
        while (!at_end()) {
            const char c = peek();
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') {
                advance();
            } else if (static_cast<unsigned char>(c) == 0xC2 && static_cast<unsigned char>(peek(1)) == 0xA0) {
                advance(2); // no-break space
            } else if (static_cast<unsigned char>(c) == 0xEF && static_cast<unsigned char>(peek(1)) == 0xBB
                       && static_cast<unsigned char>(peek(2)) == 0xBF) {
                advance(3); // a byte order mark
            } else if (static_cast<unsigned char>(c) == 0xE2 && static_cast<unsigned char>(peek(1)) == 0x80
                       && (static_cast<unsigned char>(peek(2)) == 0xA8 || static_cast<unsigned char>(peek(2)) == 0xA9)) {
                advance(3); // line and paragraph separators
            } else if (c == '/' && peek(1) == '/') {
                while (!at_end() && peek() != '\n') advance();
            } else if (c == '/' && peek(1) == '*') {
                advance(2);
                while (!at_end() && !(peek() == '*' && peek(1) == '/')) advance();
                if (at_end()) {
                    fail("a comment not closed with */");
                    return;
                }
                advance(2);
            } else {
                break;
            }
        }
    }

    [[nodiscard]] static bool identifier_start(const char c) noexcept {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' || static_cast<unsigned char>(c) >= 0x80;
    }
    [[nodiscard]] static bool identifier_part(const char c) noexcept {
        return identifier_start(c) || (c >= '0' && c <= '9');
    }

    void append_utf8(std::string& out, const std::uint32_t code) {
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }

    std::optional<std::uint32_t> hex_digits(const int count) {
        std::uint32_t value = 0;
        for (int i = 0; i < count; ++i) {
            const char c = peek();
            int digit = -1;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            if (digit < 0) {
                fail("a hexadecimal digit expected");
                return std::nullopt;
            }
            value = value * 16 + static_cast<std::uint32_t>(digit);
            advance();
        }
        return value;
    }

    std::optional<std::string> parse_string() {
        const char quote = peek();
        advance();
        std::string out;
        while (true) {
            if (at_end()) {
                fail("a string not closed");
                return std::nullopt;
            }
            const char c = peek();
            if (c == quote) {
                advance();
                return out;
            }
            if (c == '\n' || c == '\r') {
                fail("a line break in a string (end the line with a backslash to continue it)");
                return std::nullopt;
            }
            if (c != '\\') {
                out += c;
                advance();
                continue;
            }
            advance();
            const char e = peek();
            switch (e) {
            case 'b': out += '\b'; advance(); break;
            case 'f': out += '\f'; advance(); break;
            case 'n': out += '\n'; advance(); break;
            case 'r': out += '\r'; advance(); break;
            case 't': out += '\t'; advance(); break;
            case 'v': out += '\v'; advance(); break;
            case '0': out += '\0'; advance(); break;
            case '\n': advance(); break; // the string goes on on the next line
            case '\r':
                advance();
                if (peek() == '\n') advance();
                break;
            case 'x': {
                advance();
                const auto code = hex_digits(2);
                if (!code) return std::nullopt;
                append_utf8(out, *code);
                break;
            }
            case 'u': {
                advance();
                auto code = hex_digits(4);
                if (!code) return std::nullopt;
                if (*code >= 0xD800 && *code < 0xDC00 && peek() == '\\' && peek(1) == 'u') {
                    advance(2);
                    const auto low = hex_digits(4);
                    if (!low) return std::nullopt;
                    if (*low >= 0xDC00 && *low < 0xE000) code = 0x10000 + ((*code - 0xD800) << 10) + (*low - 0xDC00);
                }
                append_utf8(out, *code);
                break;
            }
            default:
                if (at_end()) {
                    fail("a string not closed");
                    return std::nullopt;
                }
                out += e;
                advance();
                break;
            }
        }
    }

    std::optional<double> parse_number() {
        double sign = 1.0;
        if (peek() == '+' || peek() == '-') {
            if (peek() == '-') sign = -1.0;
            advance();
        }
        if (text_.substr(pos_, 8) == "Infinity") {
            advance(8);
            return sign * std::numeric_limits<double>::infinity();
        }
        if (text_.substr(pos_, 3) == "NaN") {
            advance(3);
            return std::numeric_limits<double>::quiet_NaN();
        }
        if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
            advance(2);
            double value = 0.0;
            bool any = false;
            while (true) {
                const char c = peek();
                int digit = -1;
                if (c >= '0' && c <= '9') digit = c - '0';
                else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
                if (digit < 0) break;
                value = value * 16.0 + digit;
                any = true;
                advance();
            }
            if (!any) {
                fail("a hexadecimal number without digits");
                return std::nullopt;
            }
            return sign * value;
        }
        const auto start = pos_;
        bool digits = false;
        while (peek() >= '0' && peek() <= '9') {
            digits = true;
            advance();
        }
        if (peek() == '.') {
            advance();
            while (peek() >= '0' && peek() <= '9') {
                digits = true;
                advance();
            }
        }
        if (!digits) {
            fail("a number expected");
            return std::nullopt;
        }
        if (peek() == 'e' || peek() == 'E') {
            advance();
            if (peek() == '+' || peek() == '-') advance();
            if (!(peek() >= '0' && peek() <= '9')) {
                fail("an exponent without digits");
                return std::nullopt;
            }
            while (peek() >= '0' && peek() <= '9') advance();
        }
        std::string digits_text(text_.substr(start, pos_ - start));
        if (!digits_text.empty() && digits_text.front() == '.') digits_text.insert(digits_text.begin(), '0');
        double value = 0.0;
        const auto [end, error] = std::from_chars(digits_text.data(), digits_text.data() + digits_text.size(), value);
        if (error != std::errc{} || end != digits_text.data() + digits_text.size()) {
            fail("a number that cannot be read");
            return std::nullopt;
        }
        return sign * value;
    }

    std::optional<std::string> parse_key() {
        if (peek() == '"' || peek() == '\'') return parse_string();
        if (!identifier_start(peek())) {
            fail("a key expected");
            return std::nullopt;
        }
        const auto start = pos_;
        while (!at_end() && identifier_part(peek())) advance();
        return std::string(text_.substr(start, pos_ - start));
    }

    std::optional<Value> parse_value(const int depth) {
        if (depth > max_depth) {
            fail("nested too deep");
            return std::nullopt;
        }
        Value value;
        value.line = line_;
        value.column = column_;
        if (at_end()) {
            fail("a value expected");
            return std::nullopt;
        }
        const char c = peek();
        if (c == '{') {
            value.kind = Kind::object;
            advance();
            while (true) {
                skip_blanks();
                if (!error_.empty()) return std::nullopt;
                if (peek() == '}') {
                    advance();
                    return value;
                }
                auto key = parse_key();
                if (!key) return std::nullopt;
                skip_blanks();
                if (peek() != ':') {
                    fail("':' expected after the key \"" + *key + "\"");
                    return std::nullopt;
                }
                advance();
                skip_blanks();
                auto member = parse_value(depth + 1);
                if (!member) return std::nullopt;
                value.members.emplace_back(std::move(*key), std::move(*member));
                skip_blanks();
                if (peek() == ',') {
                    advance();
                    continue;
                }
                if (peek() == '}') {
                    advance();
                    return value;
                }
                fail("',' or '}' expected");
                return std::nullopt;
            }
        }
        if (c == '[') {
            value.kind = Kind::array;
            advance();
            while (true) {
                skip_blanks();
                if (!error_.empty()) return std::nullopt;
                if (peek() == ']') {
                    advance();
                    return value;
                }
                auto item = parse_value(depth + 1);
                if (!item) return std::nullopt;
                value.items.push_back(std::move(*item));
                skip_blanks();
                if (peek() == ',') {
                    advance();
                    continue;
                }
                if (peek() == ']') {
                    advance();
                    return value;
                }
                fail("',' or ']' expected");
                return std::nullopt;
            }
        }
        if (c == '"' || c == '\'') {
            auto text = parse_string();
            if (!text) return std::nullopt;
            value.kind = Kind::string;
            value.string = std::move(*text);
            return value;
        }
        if (text_.substr(pos_, 4) == "true" && !identifier_part(peek(4))) {
            advance(4);
            value.kind = Kind::boolean;
            value.boolean = true;
            return value;
        }
        if (text_.substr(pos_, 5) == "false" && !identifier_part(peek(5))) {
            advance(5);
            value.kind = Kind::boolean;
            return value;
        }
        if (text_.substr(pos_, 4) == "null" && !identifier_part(peek(4))) {
            advance(4);
            return value;
        }
        if (c == '+' || c == '-' || c == '.' || (c >= '0' && c <= '9') || c == 'I' || c == 'N') {
            const auto number = parse_number();
            if (!number) return std::nullopt;
            value.kind = Kind::number;
            value.number = *number;
            return value;
        }
        fail(std::string("unexpected '") + c + "'");
        return std::nullopt;
    }
};

} // namespace

ParseResult parse(const std::string_view text) {
    return Parser(text).run();
}

std::string quote(const std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char escaped[8];
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += escaped;
            } else {
                out += c;
            }
        }
    }
    out += '"';
    return out;
}

std::string number(const double value, const bool single) {
    if (std::isnan(value)) return "NaN";
    if (std::isinf(value)) return value < 0 ? "-Infinity" : "Infinity";
    if (value == std::floor(value) && std::abs(value) < 1e15) return std::to_string(static_cast<long long>(value));
    char buffer[32];
    for (int precision = 1; precision <= 17; ++precision) {
        std::snprintf(buffer, sizeof(buffer), "%.*g", precision, value);
        double back = 0.0;
        std::from_chars(buffer, buffer + std::char_traits<char>::length(buffer), back);
        if (single ? static_cast<float>(back) == static_cast<float>(value) : back == value) break;
    }
    return buffer;
}

} // namespace openrts::core::json5
