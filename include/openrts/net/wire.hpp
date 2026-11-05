#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The RTS network wire format (docs/net.md; docs/reverse/multiplayer.md,
// "Transport"): little-endian fields; a package is a u16 id, a u32 length
// that counts these six bytes, then its body; packages nest inside bodies.
// RTS reads and writes it field by field (client 0x5581DC / 0x558050,
// server 0x45DD24 / 0x45DBC0).
namespace openrts::net {

using Bytes = std::vector<std::uint8_t>;
using ByteView = std::span<const std::uint8_t>;

// One package: its id and its body (the bytes after the six-byte header).
struct Frame {
    std::uint16_t id{};
    ByteView body;
    std::size_t offset{}; // where the header starts in the data it was split from
};

// Packages laid one after another. A header that does not fit (a length
// under 6 or past the end) stops the split; `error` says where.
struct FrameList {
    std::vector<Frame> frames;
    std::string error;
};
[[nodiscard]] FrameList split_frames(ByteView data);

// Reads fields in order. Reading past the end sets `ok()` false and returns
// zeros from then on, so a parser checks once at the end.
class WireReader {
public:
    explicit WireReader(ByteView data) noexcept : data_(data) {}

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] std::size_t position() const noexcept { return position_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return ok_ ? data_.size() - position_ : 0; }
    [[nodiscard]] bool at_end() const noexcept { return remaining() == 0; }
    void fail() noexcept { ok_ = false; }

    std::uint8_t u8();
    std::uint16_t u16();
    std::int16_t i16();
    std::uint32_t u32();
    std::int32_t i32();
    float f32();
    double f64();
    ByteView bytes(std::size_t count);
    ByteView rest();
    void skip(std::size_t count) { bytes(count); }
    // RTS strings: a length in UTF-16 units, then the units and a zero unit;
    // a length of 0 has neither (0x461A, 0x460F, 0x450B).
    std::u16string text_u32();
    std::u16string text_u16();
    // A package nested at the current position (0x460A in 0x4609, …).
    Frame frame();

private:
    ByteView data_;
    std::size_t position_{};
    bool ok_ = true;
};

// Writes fields in order; begin()/end() wrap a package and fill in its
// length when it closes, so packages nest.
class WireWriter {
public:
    void u8(std::uint8_t value);
    void u16(std::uint16_t value);
    void i16(std::int16_t value);
    void u32(std::uint32_t value);
    void i32(std::int32_t value);
    void f32(float value);
    void f64(double value);
    void bytes(ByteView data);
    void text_u32(std::u16string_view text);
    void text_u16(std::u16string_view text);
    void begin(std::uint16_t id);
    void end();
    // A whole package with this body.
    void frame(std::uint16_t id, ByteView body);

    [[nodiscard]] const Bytes& data() const noexcept { return data_; }
    [[nodiscard]] Bytes take() noexcept { return std::move(data_); }
    [[nodiscard]] std::size_t open() const noexcept { return open_.size(); }

private:
    Bytes data_;
    std::vector<std::size_t> open_;
};

// UTF-16 text as RTS sends it, to and from UTF-8 for display and files.
[[nodiscard]] std::string to_utf8(std::u16string_view text);
[[nodiscard]] std::u16string from_utf8(std::string_view text);

} // namespace openrts::net
