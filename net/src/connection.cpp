#include <openrts/net/connection.hpp>

#include <chrono>
#include <cstring>

#include <openrts/net/packages.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace openrts::net {

namespace {

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket no_socket = INVALID_SOCKET;

struct WinsockInit {
    bool ok = false;
    WinsockInit() {
        WSADATA data{};
        ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~WinsockInit() {
        if (ok) WSACleanup();
    }
};

bool network_ready() {
    static WinsockInit init;
    return init.ok;
}

void close_socket(const Socket s) { closesocket(s); }
#else
using Socket = int;
constexpr Socket no_socket = -1;
bool network_ready() { return true; }
void close_socket(const Socket s) { ::close(s); }
#endif

Socket as_socket(const std::uintptr_t value) { return static_cast<Socket>(value); }

// Waits until the socket can be read, up to `timeout_ms`; -1 waits for ever.
bool wait_readable(const Socket s, const int timeout_ms) {
#ifdef _WIN32
    WSAPOLLFD fd{};
    fd.fd = s;
    fd.events = POLLRDNORM;
    return WSAPoll(&fd, 1, timeout_ms) > 0;
#else
    pollfd fd{};
    fd.fd = s;
    fd.events = POLLIN;
    return poll(&fd, 1, timeout_ms) > 0;
#endif
}

// Reads exactly `size` bytes; false on a closed socket, an error or (with a
// timeout) when nothing comes in time.
bool read_exact(const Socket s, std::uint8_t* out, std::size_t size, const int timeout_ms = -1) {
    while (size > 0) {
        if (timeout_ms >= 0 && !wait_readable(s, timeout_ms)) return false;
        const auto chunk = static_cast<int>(std::min<std::size_t>(size, 1 << 20));
        const auto got = ::recv(s, reinterpret_cast<char*>(out), chunk, 0);
        if (got <= 0) return false;
        out += got;
        size -= static_cast<std::size_t>(got);
    }
    return true;
}

bool write_all(const Socket s, const std::uint8_t* data, std::size_t size) {
    while (size > 0) {
        const auto sent = ::send(s, reinterpret_cast<const char*>(data), static_cast<int>(size), 0);
        if (sent <= 0) return false;
        data += sent;
        size -= static_cast<std::size_t>(sent);
    }
    return true;
}

std::uint16_t le16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

// A reply string: u16 n, then n units and a zero unit.
bool read_reply_text(const Socket s, std::u16string& text, const int timeout_ms) {
    std::uint8_t head[2];
    if (!read_exact(s, head, 2, timeout_ms)) return false;
    const auto count = le16(head);
    std::vector<std::uint8_t> units(2u * count + 2u);
    if (!read_exact(s, units.data(), units.size(), timeout_ms)) return false;
    text.resize(count);
    for (std::size_t i = 0; i < count; ++i) text[i] = static_cast<char16_t>(le16(units.data() + 2 * i));
    return true;
}

// The largest package the client takes from a server (the startup snapshot,
// rts1.exe: 0x1388000).
constexpr std::uint32_t largest_package = 0x1388000;

} // namespace

std::optional<ServerAddress> parse_address(const std::string& address) {
    if (address.empty()) return std::nullopt;
    ServerAddress out;
    const auto colon = address.rfind(':');
    // A bare IPv6 address has several colons and no port.
    if (colon != std::string::npos && address.find(':') == colon) {
        out.host = address.substr(0, colon);
        const auto port = address.substr(colon + 1);
        if (port.empty() || port.size() > 5 || port.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
        const auto value = std::stoul(port);
        if (value == 0 || value > 65535) return std::nullopt;
        out.port = static_cast<std::uint16_t>(value);
    } else {
        out.host = address;
    }
    if (out.host.empty()) return std::nullopt;
    return out;
}

RtsConnection::RtsConnection() : socket_(static_cast<std::uintptr_t>(no_socket)) {}

RtsConnection::~RtsConnection() { close(); }

LoginResult RtsConnection::connect(const std::string& address, const std::u16string_view nick, const int timeout_ms, const bool console) {
    LoginResult result;
    close();
    if (!network_ready()) {
        result.error = "the network is not available";
        return result;
    }
    const auto where = parse_address(address);
    if (!where) {
        result.error = "bad server address " + address;
        return result;
    }
    if (nick.empty() || nick.size() > 15) {
        result.error = "a nickname has 1 to 15 characters";
        return result;
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* found = nullptr;
    const auto port = std::to_string(where->port);
    if (getaddrinfo(where->host.c_str(), port.c_str(), &hints, &found) != 0 || !found) {
        result.error = "cannot resolve " + where->host;
        return result;
    }
    Socket s = no_socket;
    for (auto* a = found; a; a = a->ai_next) {
        s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == no_socket) continue;
        if (::connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen)) == 0) break;
        close_socket(s);
        s = no_socket;
    }
    freeaddrinfo(found);
    if (s == no_socket) {
        result.error = "cannot connect to " + address;
        return result;
    }
    int on = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof on);

    // The login: 0x4600 (a console 0x4500), length 12 + 2n, version 101, n,
    // the nick and a zero.
    WireWriter login;
    login.begin(id(console ? Package::console_login : Package::login));
    login.u16(101);
    login.text_u16(nick);
    login.end();
    if (!write_all(s, login.data().data(), login.data().size())) {
        close_socket(s);
        result.error = "the server closed the connection";
        return result;
    }
    // The reply has no length field: 0x4600 and its fields, or a code alone.
    std::uint8_t head[2];
    if (!read_exact(s, head, 2, timeout_ms)) {
        close_socket(s);
        result.error = "no reply to the login";
        return result;
    }
    if (le16(head) != id(Package::login)) {
        close_socket(s);
        result.refused = le16(head);
        result.error = "the server refused the login";
        return result;
    }
    LoginReply reply;
    std::uint8_t version[2];
    std::uint8_t flags[2];
    std::uint8_t counts[16];
    if (!read_exact(s, version, 2, timeout_ms) || !read_reply_text(s, reply.description, timeout_ms) ||
        !read_exact(s, flags, 2, timeout_ms) || !read_reply_text(s, reply.route, timeout_ms) ||
        !read_exact(s, counts, 16, timeout_ms)) {
        close_socket(s);
        result.error = "the login reply was cut short";
        return result;
    }
    reply.version = le16(version);
    reply.season = flags[0];
    reply.weather = flags[1];
    for (std::size_t i = 0; i < 4; ++i) reply.counts[i] = le32(counts + 4 * i);
    result.reply = std::move(reply);

    socket_ = static_cast<std::uintptr_t>(s);
    connected_ = true;
    reader_ = std::thread([this] { read_loop(); });
    return result;
}

void RtsConnection::read_loop() {
    const auto s = as_socket(socket_);
    for (;;) {
        std::uint8_t head[6];
        if (!read_exact(s, head, 6)) break;
        const auto package = le16(head);
        const auto length = le32(head + 2);
        if (length < 6 || length > largest_package) break;
        ReceivedPackage received{package, Bytes(length - 6)};
        if (!received.body.empty() && !read_exact(s, received.body.data(), received.body.size())) break;
        std::lock_guard lock(queue_mutex_);
        queue_.push_back(std::move(received));
    }
    connected_ = false;
    std::lock_guard lock(queue_mutex_);
    queue_.push_back({id(Package::server_down), {}});
}

bool RtsConnection::send(const std::uint16_t package, const ByteView body) {
    if (!connected_) return false;
    WireWriter w;
    w.frame(package, body);
    std::lock_guard lock(send_mutex_);
    return write_all(as_socket(socket_), w.data().data(), w.data().size());
}

std::vector<ReceivedPackage> RtsConnection::receive() {
    std::lock_guard lock(queue_mutex_);
    return std::exchange(queue_, {});
}

void RtsConnection::close() {
    const auto s = as_socket(socket_);
    if (s != no_socket) {
#ifdef _WIN32
        shutdown(s, SD_BOTH);
#else
        shutdown(s, SHUT_RDWR);
#endif
    }
    if (reader_.joinable()) reader_.join();
    if (s != no_socket) close_socket(s);
    socket_ = static_cast<std::uintptr_t>(no_socket);
    connected_ = false;
}

} // namespace openrts::net
