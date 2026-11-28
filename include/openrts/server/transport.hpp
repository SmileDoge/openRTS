#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <openrts/net/wire.hpp>

// How the openRTS server's bytes reach its users (docs/server.md): a TCP
// listener, or a test's recorder. Raw bytes: the RTS login reply and the
// error codes have no length field (docs/reverse/multiplayer.md, "Login").
namespace openrts::server {

using ConnectionId = std::uint32_t;

class Transport {
public:
    virtual ~Transport() = default;
    virtual void send(ConnectionId to, net::Bytes bytes) = 0;
    // Closes once what was queued for it has gone out.
    virtual void close(ConnectionId to) = 0;
    // openRTS's positions by UDP (net/datagram.hpp, experimental): the
    // token a connection's player says hello with (0: no more); a package
    // by UDP while that player says hello (false: send it by TCP); the UDP
    // port (0: none).
    virtual void offer_datagrams(ConnectionId, std::uint64_t) {}
    virtual bool send_datagram(ConnectionId, const net::Bytes&) { return false; }
    [[nodiscard]] virtual std::uint16_t datagram_port() const { return 0; }
};

// What came in, for the game's thread.
struct NetEvent {
    enum class Kind : std::uint8_t { connected, package, closed };
    Kind kind = Kind::package;
    ConnectionId connection{};
    std::uint16_t id{};
    net::Bytes body;
    std::string address; // connected: "a.b.c.d:port"
};

// RTS's limits on what a user sends (rts_server.exe `0x4436C8`): a login of
// at most 64 bytes, then packages of at most 0x3C00 bytes; a longer one ends
// the connection.
inline constexpr std::uint32_t login_limit = 64;
// openRTS's dispatcher console logs in (0x4500) with its password after the
// nick: longer than a player's 64 bytes.
inline constexpr std::uint32_t console_login_limit = 256;
inline constexpr std::uint32_t package_limit = 0x3C00;

// The server's TCP side (WinSock 2 / BSD sockets): a thread accepts, a thread
// per connection reads whole packages, a thread per connection sends.
class TcpServer final : public Transport {
public:
    TcpServer();
    ~TcpServer() override;
    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    // Listens on every interface, or on 127.0.0.1 alone (`local`: tests,
    // no firewall prompt); port 0 picks a free one.
    bool listen(std::uint16_t port, std::string& error, bool local = false);
    [[nodiscard]] std::uint16_t port() const noexcept;
    // What came in since the last call, in order.
    [[nodiscard]] std::vector<NetEvent> take_events();
    void send(ConnectionId to, net::Bytes bytes) override;
    void close(ConnectionId to) override;
    void offer_datagrams(ConnectionId to, std::uint64_t token) override;
    bool send_datagram(ConnectionId to, const net::Bytes& bytes) override;
    [[nodiscard]] std::uint16_t datagram_port() const override;
    // Closes everything and waits for the threads.
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace openrts::server
