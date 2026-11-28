#pragma once

#include <cstdint>
#include <cstring>
#include <optional>

#include <openrts/net/wire.hpp>

// openRTS's datagrams (docs/net.md, "Positions by UDP", experimental): the
// trains' fast positions (0x45EE) between an openRTS server and its openRTS
// players by UDP, on the TCP port's number, beside the TCP connection that
// keeps everything else. The server hands each player a token over TCP
// (openrts_udp_offer); the player says hello with it once a second, the
// server answers each; while answers come (3 s), both send the positions
// this way, else by TCP again.
//
// A datagram: "oRU1", a kind (u8), the token (u64), then for a package its
// frame (u16 id, u32 length, body) as on TCP.
namespace openrts::net {

enum class DatagramKind : std::uint8_t { hello = 1, answer = 2, package = 3 };

inline constexpr std::uint8_t datagram_magic[4] = {'o', 'R', 'U', '1'};
inline constexpr std::size_t datagram_header = 13;
// Below a usual path's MTU, so a datagram is never split.
inline constexpr std::size_t datagram_largest = 1200;
// Without a datagram from the other side for this long, back to TCP.
inline constexpr double datagram_silence = 3.0;

struct Datagram {
    DatagramKind kind{};
    std::uint64_t token{};
    ByteView payload;
};

[[nodiscard]] inline Bytes make_datagram(const DatagramKind kind, const std::uint64_t token, const ByteView payload = {}) {
    Bytes out(datagram_header + payload.size());
    std::memcpy(out.data(), datagram_magic, 4);
    out[4] = static_cast<std::uint8_t>(kind);
    for (int i = 0; i < 8; ++i) out[5 + i] = static_cast<std::uint8_t>(token >> (8 * i));
    if (!payload.empty()) std::memcpy(out.data() + datagram_header, payload.data(), payload.size());
    return out;
}

[[nodiscard]] inline std::optional<Datagram> read_datagram(const ByteView data) {
    if (data.size() < datagram_header || std::memcmp(data.data(), datagram_magic, 4) != 0) return std::nullopt;
    const auto kind = data[4];
    if (kind < 1 || kind > 3) return std::nullopt;
    Datagram out;
    out.kind = static_cast<DatagramKind>(kind);
    for (int i = 0; i < 8; ++i) out.token |= static_cast<std::uint64_t>(data[5 + i]) << (8 * i);
    out.payload = data.subspan(datagram_header);
    return out;
}

// The offer over TCP (openrts_udp_offer): the token and the UDP port.
struct OpenrtsUdpOffer {
    std::uint64_t token{};
    std::uint16_t port{};
};

[[nodiscard]] inline Bytes encode_openrts_udp_offer(const OpenrtsUdpOffer& offer) {
    Bytes out(10);
    for (int i = 0; i < 8; ++i) out[i] = static_cast<std::uint8_t>(offer.token >> (8 * i));
    out[8] = static_cast<std::uint8_t>(offer.port);
    out[9] = static_cast<std::uint8_t>(offer.port >> 8);
    return out;
}

[[nodiscard]] inline std::optional<OpenrtsUdpOffer> decode_openrts_udp_offer(const ByteView body) {
    if (body.size() < 10) return std::nullopt;
    OpenrtsUdpOffer out;
    for (int i = 0; i < 8; ++i) out.token |= static_cast<std::uint64_t>(body[i]) << (8 * i);
    out.port = static_cast<std::uint16_t>(body[8] | (body[9] << 8));
    return out;
}

} // namespace openrts::net
