#include <openrts/server/transport.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

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
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace openrts::server {

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
void shut(const Socket s) { shutdown(s, SD_BOTH); }
#else
using Socket = int;
constexpr Socket no_socket = -1;
bool network_ready() { return true; }
void close_socket(const Socket s) { ::close(s); }
void shut(const Socket s) { shutdown(s, SHUT_RDWR); }
#endif

bool read_exact(const Socket s, std::uint8_t* out, std::size_t size) {
    while (size > 0) {
        const auto got = ::recv(s, reinterpret_cast<char*>(out), static_cast<int>(std::min<std::size_t>(size, 1 << 16)), 0);
        if (got <= 0) return false;
        out += got;
        size -= static_cast<std::size_t>(got);
    }
    return true;
}

bool write_all(const Socket s, const std::uint8_t* data, std::size_t size) {
    while (size > 0) {
        const auto sent = ::send(s, reinterpret_cast<const char*>(data), static_cast<int>(std::min<std::size_t>(size, 1 << 20)), 0);
        if (sent <= 0) return false;
        data += sent;
        size -= static_cast<std::size_t>(sent);
    }
    return true;
}

std::string address_text(const sockaddr_storage& address) {
    char host[INET6_ADDRSTRLEN]{};
    std::uint16_t port = 0;
    if (address.ss_family == AF_INET) {
        const auto& a = reinterpret_cast<const sockaddr_in&>(address);
        inet_ntop(AF_INET, &a.sin_addr, host, sizeof host);
        port = ntohs(a.sin_port);
    } else if (address.ss_family == AF_INET6) {
        const auto& a = reinterpret_cast<const sockaddr_in6&>(address);
        inet_ntop(AF_INET6, &a.sin6_addr, host, sizeof host);
        port = ntohs(a.sin6_port);
    }
    return std::string(host) + ":" + std::to_string(port);
}

} // namespace

struct TcpServer::Impl {
    struct Connection {
        Socket socket = no_socket;
        std::mutex mutex;
        std::condition_variable wake;
        std::deque<net::Bytes> queue;
        bool closing = false; // close once the queue is empty
        bool gone = false;    // the reader saw the end
        ~Connection() {
            if (socket != no_socket) close_socket(socket);
        }
    };

    Socket listener = no_socket;
    std::uint16_t bound_port = 0;
    std::atomic<bool> running = false;
    std::thread acceptor;
    std::mutex mutex; // connections, events
    std::map<ConnectionId, std::shared_ptr<Connection>> connections;
    std::vector<NetEvent> events;
    std::atomic<int> live = 0; // connection threads still running
    ConnectionId next_id = 1;

    // A connection thread: detached, counted so stop() can wait for it.
    template <class Body>
    void start(Body body) {
        ++live;
        std::thread([this, body = std::move(body)]() mutable {
            body();
            --live;
        }).detach();
    }

    void push(NetEvent event) {
        std::lock_guard lock(mutex);
        events.push_back(std::move(event));
    }

    void reader(const ConnectionId id, const std::shared_ptr<Connection> connection) {
        bool first = true;
        for (;;) {
            std::uint8_t head[6];
            if (!read_exact(connection->socket, head, sizeof head)) break;
            const auto package = static_cast<std::uint16_t>(head[0] | (head[1] << 8));
            const auto length = static_cast<std::uint32_t>(head[2]) | (static_cast<std::uint32_t>(head[3]) << 8) |
                                (static_cast<std::uint32_t>(head[4]) << 16) | (static_cast<std::uint32_t>(head[5]) << 24);
            // RTS drops a user whose package is too long or too short (0x47FF).
            if (length < 6 || length > (first ? login_limit : package_limit)) break;
            NetEvent event{.kind = NetEvent::Kind::package, .connection = id, .id = package};
            event.body.resize(length - 6);
            if (!event.body.empty() && !read_exact(connection->socket, event.body.data(), event.body.size())) break;
            first = false;
            push(std::move(event));
        }
        {
            std::lock_guard lock(connection->mutex);
            connection->gone = true;
            connection->closing = true;
        }
        connection->wake.notify_all();
        push({.kind = NetEvent::Kind::closed, .connection = id});
    }

    void sender(const std::shared_ptr<Connection> connection) {
        for (;;) {
            net::Bytes next;
            {
                std::unique_lock lock(connection->mutex);
                connection->wake.wait(lock, [&] { return !connection->queue.empty() || connection->closing; });
                if (connection->queue.empty() || connection->gone) break;
                next = std::move(connection->queue.front());
                connection->queue.pop_front();
            }
            if (!write_all(connection->socket, next.data(), next.size())) break;
        }
        // Ends the reader too (it may be waiting for data).
        shut(connection->socket);
    }

    void accept_loop() {
        while (running) {
            sockaddr_storage address{};
            socklen_t size = sizeof address;
            const Socket s = ::accept(listener, reinterpret_cast<sockaddr*>(&address), &size);
            if (s == no_socket) {
                if (!running) break;
                continue;
            }
            int on = 1;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof on);
            auto connection = std::make_shared<Connection>();
            connection->socket = s;
            std::lock_guard lock(mutex);
            const auto id = next_id++;
            connections[id] = connection;
            events.push_back({.kind = NetEvent::Kind::connected, .connection = id, .address = address_text(address)});
            start([this, id, connection] { reader(id, connection); });
            start([this, connection] { sender(connection); });
        }
    }
};

TcpServer::TcpServer() : impl_(std::make_unique<Impl>()) {}

TcpServer::~TcpServer() { stop(); }

bool TcpServer::listen(const std::uint16_t port, std::string& error, const bool local) {
    if (!network_ready()) {
        error = "the network could not start";
        return false;
    }
    auto& impl = *impl_;
    // IPv6 with IPv4 mapped where it works, else IPv4 alone.
    Socket s = local ? no_socket : ::socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    bool six = s != no_socket;
    if (six) {
        int off = 0;
        setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&off), sizeof off);
        sockaddr_in6 address{};
        address.sin6_family = AF_INET6;
        address.sin6_addr = in6addr_any;
        address.sin6_port = htons(port);
        if (::bind(s, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0) {
            close_socket(s);
            six = false;
        }
    }
    if (!six) {
        s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == no_socket) {
            error = "no socket";
            return false;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(local ? INADDR_LOOPBACK : INADDR_ANY);
        address.sin_port = htons(port);
        if (::bind(s, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0) {
            close_socket(s);
            error = "port " + std::to_string(port) + " is taken";
            return false;
        }
    }
    if (::listen(s, 16) != 0) {
        close_socket(s);
        error = "cannot listen";
        return false;
    }
    sockaddr_storage bound{};
    socklen_t size = sizeof bound;
    getsockname(s, reinterpret_cast<sockaddr*>(&bound), &size);
    impl.bound_port = bound.ss_family == AF_INET6 ? ntohs(reinterpret_cast<const sockaddr_in6&>(bound).sin6_port)
                                                  : ntohs(reinterpret_cast<const sockaddr_in&>(bound).sin_port);
    impl.listener = s;
    impl.running = true;
    impl.acceptor = std::thread([&impl] { impl.accept_loop(); });
    return true;
}

std::uint16_t TcpServer::port() const noexcept { return impl_->bound_port; }

std::vector<NetEvent> TcpServer::take_events() {
    std::lock_guard lock(impl_->mutex);
    auto events = std::move(impl_->events);
    impl_->events.clear();
    // Connections whose reader has ended are no longer needed here.
    for (auto it = impl_->connections.begin(); it != impl_->connections.end();) {
        bool done = false;
        {
            std::lock_guard connection_lock(it->second->mutex);
            done = it->second->gone && it->second->queue.empty();
        }
        it = done ? impl_->connections.erase(it) : std::next(it);
    }
    return events;
}

void TcpServer::send(const ConnectionId to, net::Bytes bytes) {
    std::shared_ptr<Impl::Connection> connection;
    {
        std::lock_guard lock(impl_->mutex);
        const auto it = impl_->connections.find(to);
        if (it == impl_->connections.end()) return;
        connection = it->second;
    }
    {
        std::lock_guard lock(connection->mutex);
        if (connection->closing) return;
        connection->queue.push_back(std::move(bytes));
    }
    connection->wake.notify_all();
}

void TcpServer::close(const ConnectionId to) {
    std::shared_ptr<Impl::Connection> connection;
    {
        std::lock_guard lock(impl_->mutex);
        const auto it = impl_->connections.find(to);
        if (it == impl_->connections.end()) return;
        connection = it->second;
    }
    {
        std::lock_guard lock(connection->mutex);
        connection->closing = true;
    }
    connection->wake.notify_all();
}

void TcpServer::stop() {
    auto& impl = *impl_;
    if (impl.running.exchange(false)) {
        close_socket(impl.listener);
        impl.listener = no_socket;
    }
    if (impl.acceptor.joinable()) impl.acceptor.join();
    {
        std::lock_guard lock(impl.mutex);
        for (auto& [_, connection] : impl.connections) {
            {
                std::lock_guard connection_lock(connection->mutex);
                connection->closing = true;
                connection->gone = true;
            }
            connection->wake.notify_all();
            shut(connection->socket);
        }
    }
    // The threads end once their sockets are shut (at most a few seconds).
    for (int i = 0; i < 500 && impl.live > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::lock_guard lock(impl.mutex);
    impl.connections.clear();
}

} // namespace openrts::server
