#pragma once

// JSON5 (https://json5.org): openRTS's own new text formats, materials
// first (docs/pbr-materials.md). JSON with comments (// and /* */), keys
// without quotes, single-quoted strings, a comma after the last item,
// hexadecimal numbers, a leading or trailing decimal point, a plus sign,
// Infinity and NaN, and a string continued on the next line after a
// backslash. Every value keeps where it began (line and column, from 1),
// so a format's checks can say where the problem is.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace openrts::core::json5 {

enum class Kind : std::uint8_t { null, boolean, number, string, array, object };

struct Value {
    Kind kind = Kind::null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Value> items;                           // an array's
    std::vector<std::pair<std::string, Value>> members; // an object's, in the file's order
    int line = 0;
    int column = 0;

    [[nodiscard]] bool is_null() const noexcept { return kind == Kind::null; }
    [[nodiscard]] bool is_boolean() const noexcept { return kind == Kind::boolean; }
    [[nodiscard]] bool is_number() const noexcept { return kind == Kind::number; }
    [[nodiscard]] bool is_string() const noexcept { return kind == Kind::string; }
    [[nodiscard]] bool is_array() const noexcept { return kind == Kind::array; }
    [[nodiscard]] bool is_object() const noexcept { return kind == Kind::object; }
    // An object's member (the last one of that name), null when there is none.
    [[nodiscard]] const Value* find(std::string_view key) const noexcept;
    // "line 3, column 7".
    [[nodiscard]] std::string where() const;
};

struct ParseResult {
    std::optional<Value> value;
    std::string error; // with its line and column
    int line = 0;
    int column = 0;
    [[nodiscard]] bool ok() const noexcept { return value.has_value(); }
};

// One value (usually an object) and nothing after it but blanks and comments.
[[nodiscard]] ParseResult parse(std::string_view text);

// A string as JSON5 writes it (double quotes, escapes).
[[nodiscard]] std::string quote(std::string_view text);
// A number as written: whole numbers without a point, others with as few
// digits as read back the same (single: as the same float).
[[nodiscard]] std::string number(double value, bool single = false);

} // namespace openrts::core::json5
