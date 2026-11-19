#pragma once

#include <charconv>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace openrts::simulation {

// A saved game's numbers as text: one line a key, its values after it
// ("c3.pipe 4.98"). Keys a later version adds are simply absent from an
// older file, and keys it no longer knows are skipped, so a save outlives
// changes of the structures it came from.
class StateWriter {
public:
    template <typename... T>
    void put(const std::string_view key, const T&... values) {
        text_ += key;
        (append(values), ...);
        text_ += '\n';
    }
    template <typename T, std::size_t N>
    void put_array(const std::string_view key, const T (&values)[N]) {
        text_ += key;
        for (const auto& v : values) append(v);
        text_ += '\n';
    }
    template <typename T>
    void put_list(const std::string_view key, const std::vector<T>& values) {
        text_ += key;
        for (const auto& v : values) append(v);
        text_ += '\n';
    }
    [[nodiscard]] const std::string& text() const noexcept { return text_; }

private:
    template <typename T>
    void append(const T& value) {
        text_ += ' ';
        if constexpr (std::is_enum_v<T>) {
            append_number(static_cast<double>(static_cast<std::int64_t>(value)));
        } else if constexpr (std::is_same_v<T, bool>) {
            text_ += value ? '1' : '0';
        } else {
            append_number(static_cast<double>(value));
        }
    }
    void append_number(const double value) {
        if (!std::isfinite(value)) {
            text_ += value > 0 ? "inf" : value < 0 ? "-inf" : "nan";
            return;
        }
        char buffer[32];
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
        text_.append(buffer, result.ptr);
    }
    std::string text_;
};

class StateReader {
public:
    StateReader() = default;
    explicit StateReader(const std::string_view text) {
        std::size_t start = 0;
        while (start < text.size()) {
            auto end = text.find('\n', start);
            if (end == std::string_view::npos) end = text.size();
            auto line = text.substr(start, end - start);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            start = end + 1;
            const auto space = line.find(' ');
            const auto key = std::string(line.substr(0, space));
            if (key.empty() || key.front() == '#') continue;
            std::vector<double> numbers;
            auto rest = space == std::string_view::npos ? std::string_view{} : line.substr(space + 1);
            while (!rest.empty()) {
                const auto next = rest.find(' ');
                const auto word = rest.substr(0, next);
                rest = next == std::string_view::npos ? std::string_view{} : rest.substr(next + 1);
                if (word.empty()) continue;
                if (word == "inf") {
                    numbers.push_back(INFINITY);
                } else if (word == "-inf") {
                    numbers.push_back(-INFINITY);
                } else if (word == "nan") {
                    numbers.push_back(NAN);
                } else {
                    double value = 0.0;
                    std::from_chars(word.data(), word.data() + word.size(), value);
                    numbers.push_back(value);
                }
            }
            values_[key] = std::move(numbers);
        }
    }
    [[nodiscard]] bool has(const std::string_view key) const { return values_.find(key) != values_.end(); }
    // The values of `key` into `out...`; false (and `out` untouched) when the
    // key is absent or has fewer values.
    template <typename... T>
    bool get(const std::string_view key, T&... out) const {
        const auto it = values_.find(key);
        if (it == values_.end() || it->second.size() < sizeof...(T)) return false;
        std::size_t i = 0;
        (assign(out, it->second[i++]), ...);
        return true;
    }
    template <typename T, std::size_t N>
    bool get_array(const std::string_view key, T (&out)[N]) const {
        const auto it = values_.find(key);
        if (it == values_.end() || it->second.size() < N) return false;
        for (std::size_t i = 0; i < N; ++i) assign(out[i], it->second[i]);
        return true;
    }

    // All the values of `key`, as many as there are; false when absent.
    template <typename T>
    bool get_list(const std::string_view key, std::vector<T>& out) const {
        const auto it = values_.find(key);
        if (it == values_.end()) return false;
        out.resize(it->second.size());
        for (std::size_t i = 0; i < out.size(); ++i) assign(out[i], it->second[i]);
        return true;
    }

private:
    template <typename T>
    static void assign(T& out, const double value) {
        if constexpr (std::is_enum_v<T>) {
            out = static_cast<T>(static_cast<std::underlying_type_t<T>>(value));
        } else if constexpr (std::is_same_v<T, bool>) {
            out = value != 0.0;
        } else {
            out = static_cast<T>(value);
        }
    }
    std::map<std::string, std::vector<double>, std::less<>> values_;
};

} // namespace openrts::simulation
