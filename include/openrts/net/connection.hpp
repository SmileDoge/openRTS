#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <openrts/net/wire.hpp>

// A TCP connection to an RTS server as a player (docs/net.md;
// docs/reverse/multiplayer.md, "Login"): the login and its reply, then
// packages both ways. A thread reads the server and queues whole packages
// for the main loop; when the socket closes it queues a 0x4612 server_down
// of its own, as rts1.exe does (0x4CB390).
namespace openrts::net {

// The server's reply to a login (no length field on the wire).
struct LoginReply {
    std::uint16_t version{};
    std::u16string description; // route title, "Сценарий: …", "Сезон: …"
    std::uint8_t season{};
    std::uint8_t weather{};
    std::u16string route;       // the route id, lower case
    std::array<std::uint32_t, 4> counts{}; // junctions, signals and two more
};

struct LoginResult {
    std::optional<LoginReply> reply;
    std::uint16_t refused{}; // 0x4701..0x47FF when the server refused
    std::string error;       // a connection problem
    [[nodiscard]] bool ok() const noexcept { return reply.has_value(); }
};

// A package taken from the connection.
struct ReceivedPackage {
    std::uint16_t id{};
    Bytes body;
};

class RtsConnection {
public:
    RtsConnection();
    ~RtsConnection();
    RtsConnection(const RtsConnection&) = delete;
    RtsConnection& operator=(const RtsConnection&) = delete;

    // Connects to "host:port" (port 800 when none is given), logs in with
    // the protocol version 101 and reads the reply; then starts reading.
    // `timeout_ms` bounds the connect and the reply. `console`: as a
    // dispatcher console (0x4500; an openRTS server answers it as a player),
    // with the server's dispatcher password after the nick (openRTS's own:
    // needed from another computer, docs/server.md).
    LoginResult connect(const std::string& address, std::u16string_view nick, int timeout_ms = 10000, bool console = false,
                        std::u16string_view password = {});
    // Sends one package (any thread).
    bool send(std::uint16_t id, ByteView body);
    // The packages the server sent since the last call, in order.
    [[nodiscard]] std::vector<ReceivedPackage> receive();
    [[nodiscard]] bool connected() const noexcept { return connected_; }
    void close();

    // openRTS's positions by UDP (net/datagram.hpp, experimental): a UDP
    // socket to the server's host on `port`, saying hello with `token` once
    // a second; the packages it brings come with receive()'s.
    bool open_datagrams(std::uint64_t token, std::uint16_t port);
    // One package by UDP while the server answers (false: not open or
    // silent for 3 s: send it by TCP).
    bool send_datagram(std::uint16_t id, ByteView body);
    [[nodiscard]] bool datagrams_live() const noexcept;

private:
    void read_loop();
    void datagram_loop();
    void close_datagrams();
    std::string host_; // the server's, as connected to
    std::uintptr_t udp_;
    std::uint64_t token_{};
    std::atomic<bool> udp_running_ = false;
    std::atomic<long long> heard_ms_ = -1000000; // the server's last datagram (steady clock)
    std::thread udp_reader_;

    std::uintptr_t socket_;
    std::atomic<bool> connected_ = false;
    std::mutex queue_mutex_;
    std::vector<ReceivedPackage> queue_;
    std::mutex send_mutex_;
    std::thread reader_;
};

// Splits "host:port"; the port defaults to 800 (RTS's).
struct ServerAddress {
    std::string host;
    std::uint16_t port = 800;
};
[[nodiscard]] std::optional<ServerAddress> parse_address(const std::string& address);

} // namespace openrts::net
