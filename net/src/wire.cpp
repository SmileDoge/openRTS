#include <openrts/net/wire.hpp>

#include <bit>
#include <cstring>

namespace openrts::net {

namespace {

template <typename T>
T load(const std::uint8_t* at) {
    T value{};
    std::memcpy(&value, at, sizeof(T));
    if constexpr (std::endian::native == std::endian::big && sizeof(T) > 1) {
        auto* bytes = reinterpret_cast<std::uint8_t*>(&value);
        for (std::size_t i = 0; i < sizeof(T) / 2; ++i) std::swap(bytes[i], bytes[sizeof(T) - 1 - i]);
    }
    return value;
}

template <typename T>
void store(Bytes& out, T value) {
    std::uint8_t bytes[sizeof(T)];
    std::memcpy(bytes, &value, sizeof(T));
    if constexpr (std::endian::native == std::endian::big && sizeof(T) > 1) {
        for (std::size_t i = 0; i < sizeof(T) / 2; ++i) std::swap(bytes[i], bytes[sizeof(T) - 1 - i]);
    }
    out.insert(out.end(), bytes, bytes + sizeof(T));
}

} // namespace

FrameList split_frames(const ByteView data) {
    FrameList list;
    std::size_t at = 0;
    while (at < data.size()) {
        if (data.size() - at < 6) {
            list.error = "a header cut short at byte " + std::to_string(at);
            break;
        }
        const auto id = load<std::uint16_t>(data.data() + at);
        const auto length = load<std::uint32_t>(data.data() + at + 2);
        if (length < 6 || length > data.size() - at) {
            list.error = "package " + std::to_string(id) + " at byte " + std::to_string(at) + " has a bad length " +
                         std::to_string(length);
            break;
        }
        list.frames.push_back({id, data.subspan(at + 6, length - 6), at});
        at += length;
    }
    return list;
}

ByteView WireReader::bytes(const std::size_t count) {
    if (!ok_ || count > data_.size() - position_) {
        ok_ = false;
        return {};
    }
    const auto view = data_.subspan(position_, count);
    position_ += count;
    return view;
}

ByteView WireReader::rest() { return bytes(remaining()); }

std::uint8_t WireReader::u8() {
    const auto b = bytes(1);
    return b.empty() ? 0 : b[0];
}

std::uint16_t WireReader::u16() {
    const auto b = bytes(2);
    return b.empty() ? 0 : load<std::uint16_t>(b.data());
}

std::int16_t WireReader::i16() { return static_cast<std::int16_t>(u16()); }

std::uint32_t WireReader::u32() {
    const auto b = bytes(4);
    return b.empty() ? 0 : load<std::uint32_t>(b.data());
}

std::int32_t WireReader::i32() { return static_cast<std::int32_t>(u32()); }

float WireReader::f32() {
    const auto b = bytes(4);
    return b.empty() ? 0.0f : load<float>(b.data());
}

double WireReader::f64() {
    const auto b = bytes(8);
    return b.empty() ? 0.0 : load<double>(b.data());
}

namespace {

std::u16string read_units(WireReader& reader, const std::size_t count) {
    std::u16string text;
    if (count == 0) return text;
    // A sane cap: no RTS string comes near it, and a broken length must not
    // allocate gigabytes.
    if (count > reader.remaining() / 2) {
        reader.fail();
        return text;
    }
    text.resize(count);
    for (auto& unit : text) unit = static_cast<char16_t>(reader.u16());
    reader.u16(); // the zero unit
    return text;
}

} // namespace

std::u16string WireReader::text_u32() { return read_units(*this, u32()); }

std::u16string WireReader::text_u16() { return read_units(*this, u16()); }

Frame WireReader::frame() {
    const auto start = position_;
    const auto id = u16();
    const auto length = u32();
    if (!ok_ || length < 6) {
        ok_ = false;
        return {};
    }
    const auto body = bytes(length - 6);
    return {id, body, start};
}

void WireWriter::u8(const std::uint8_t value) { data_.push_back(value); }
void WireWriter::u16(const std::uint16_t value) { store(data_, value); }
void WireWriter::i16(const std::int16_t value) { store(data_, value); }
void WireWriter::u32(const std::uint32_t value) { store(data_, value); }
void WireWriter::i32(const std::int32_t value) { store(data_, value); }
void WireWriter::f32(const float value) { store(data_, value); }
void WireWriter::f64(const double value) { store(data_, value); }
void WireWriter::bytes(const ByteView data) { data_.insert(data_.end(), data.begin(), data.end()); }

void WireWriter::text_u32(const std::u16string_view text) {
    u32(static_cast<std::uint32_t>(text.size()));
    if (text.empty()) return;
    for (const auto unit : text) u16(static_cast<std::uint16_t>(unit));
    u16(0);
}

void WireWriter::text_u16(const std::u16string_view text) {
    u16(static_cast<std::uint16_t>(text.size()));
    if (text.empty()) return;
    for (const auto unit : text) u16(static_cast<std::uint16_t>(unit));
    u16(0);
}

void WireWriter::begin(const std::uint16_t id) {
    open_.push_back(data_.size());
    u16(id);
    u32(0);
}

void WireWriter::end() {
    if (open_.empty()) return;
    const auto start = open_.back();
    open_.pop_back();
    const auto length = static_cast<std::uint32_t>(data_.size() - start);
    std::uint8_t bytes[4];
    std::memcpy(bytes, &length, 4);
    if constexpr (std::endian::native == std::endian::big) {
        std::swap(bytes[0], bytes[3]);
        std::swap(bytes[1], bytes[2]);
    }
    std::memcpy(data_.data() + start + 2, bytes, 4);
}

void WireWriter::frame(const std::uint16_t id, const ByteView body) {
    begin(id);
    bytes(body);
    end();
}

std::string to_utf8(const std::u16string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        std::uint32_t code = text[i];
        if (code >= 0xD800 && code < 0xDC00 && i + 1 < text.size() && text[i + 1] >= 0xDC00 && text[i + 1] < 0xE000) {
            code = 0x10000 + ((code - 0xD800) << 10) + (text[i + 1] - 0xDC00);
            ++i;
        } else if (code >= 0xD800 && code < 0xE000) {
            code = 0xFFFD;
        }
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
    return out;
}

std::u16string from_utf8(const std::string_view text) {
    std::u16string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<std::uint8_t>(text[i]);
        std::uint32_t code = 0xFFFD;
        std::size_t length = 1;
        if (lead < 0x80) {
            code = lead;
        } else if ((lead >> 5) == 0x6) {
            length = 2;
        } else if ((lead >> 4) == 0xE) {
            length = 3;
        } else if ((lead >> 3) == 0x1E) {
            length = 4;
        }
        if (length > 1) {
            if (i + length > text.size()) {
                length = 1;
            } else {
                code = lead & (0xFF >> (length + 1));
                for (std::size_t k = 1; k < length; ++k) {
                    code = (code << 6) | (static_cast<std::uint8_t>(text[i + k]) & 0x3F);
                }
            }
        }
        if (code >= 0x10000) {
            code -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (code >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (code & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(code));
        }
        i += length;
    }
    return out;
}

} // namespace openrts::net
