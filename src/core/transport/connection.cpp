#include "lanlink/transport/connection.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace lanlink::transport {
namespace {

#if defined(_WIN32)
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
#endif

void close_socket(NativeSocket socket) {
    if (socket == kInvalidSocket)
        return;
#if defined(_WIN32)
    closesocket(socket);
#else
    close(socket);
#endif
}

bool interrupted() {
#if defined(_WIN32)
    return WSAGetLastError() == WSAEINTR;
#else
    return errno == EINTR;
#endif
}

Result<void> initialize_sockets() {
#if defined(_WIN32)
    static std::once_flag once;
    static int startup_result = 0;
    std::call_once(once, [] {
        WSADATA data{};
        startup_result = WSAStartup(MAKEWORD(2, 2), &data);
    });
    if (startup_result != 0)
        return Result<void>::failure("socket initialization failed");
#endif
    return Result<void>::success();
}

std::string endpoint_service(std::uint16_t port) { return std::to_string(port); }

struct ResolvedAddress {
    addrinfo* info{};
    ResolvedAddress() = default;
    explicit ResolvedAddress(addrinfo* value) : info(value) {}
    ResolvedAddress(const ResolvedAddress&) = delete;
    ResolvedAddress& operator=(const ResolvedAddress&) = delete;
    ResolvedAddress(ResolvedAddress&& other) noexcept : info(other.info) { other.info = nullptr; }
    ResolvedAddress& operator=(ResolvedAddress&& other) noexcept {
        if (this == &other)
            return *this;
        if (info != nullptr)
            freeaddrinfo(info);
        info = other.info;
        other.info = nullptr;
        return *this;
    }
    ~ResolvedAddress() {
        if (info != nullptr)
            freeaddrinfo(info);
    }
};

Result<ResolvedAddress> resolve(const Endpoint& endpoint, bool passive) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (passive)
        hints.ai_flags = AI_PASSIVE;
    const char* host = endpoint.host.empty() ? nullptr : endpoint.host.c_str();
    const auto service = endpoint_service(endpoint.port);
    addrinfo* result = nullptr;
    if (getaddrinfo(host, service.c_str(), &hints, &result) != 0 || result == nullptr)
        return Result<ResolvedAddress>::failure("address resolution failed");
    return Result<ResolvedAddress>::success(ResolvedAddress{result});
}

Result<void> wait_readable(NativeSocket socket, std::chrono::milliseconds timeout) {
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(socket, &read_set);
    timeval value{};
    timeval* value_ptr = nullptr;
    if (timeout.count() >= 0) {
        value.tv_sec = static_cast<long>(timeout.count() / 1000);
        value.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
        value_ptr = &value;
    }
#if defined(_WIN32)
    const int ready = select(0, &read_set, nullptr, nullptr, value_ptr);
#else
    const int ready = select(socket + 1, &read_set, nullptr, nullptr, value_ptr);
#endif
    if (ready < 0) {
        if (interrupted())
            return Result<void>::failure("read wait interrupted");
        return Result<void>::failure("read wait failed");
    }
    if (ready == 0)
        return Result<void>::failure("read timeout");
    return Result<void>::success();
}

Result<void> write_all(NativeSocket socket, const std::vector<std::uint8_t>& bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
#if defined(_WIN32)
        const int sent = send(socket, reinterpret_cast<const char*>(bytes.data() + offset),
                              static_cast<int>(bytes.size() - offset), 0);
#else
        const int sent = static_cast<int>(
            send(socket, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL));
#endif
        if (sent > 0) {
            offset += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && interrupted())
            continue;
        return Result<void>::failure("socket write failed");
    }
    return Result<void>::success();
}

} // namespace

struct Connection::Impl {
    NativeSocket socket{kInvalidSocket};
    ConnectionOptions options;
    protocol::FrameDecoder decoder;
    ConnectionState state{ConnectionState::kClosed};

    ~Impl() { close_socket(socket); }
};

struct Listener::Impl {
    NativeSocket socket{kInvalidSocket};
    ConnectionOptions options;
    Endpoint endpoint;

    ~Impl() { close_socket(socket); }
};

Connection::Connection(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Connection::Connection(Connection&& other) noexcept : impl_(std::move(other.impl_)) {}

Connection& Connection::operator=(Connection&& other) noexcept {
    if (this == &other)
        return *this;
    close();
    impl_ = std::move(other.impl_);
    return *this;
}

Connection::~Connection() { close(); }

Result<Connection> Connection::connect(const Endpoint& endpoint, ConnectionOptions options) {
    auto initialized = initialize_sockets();
    if (!initialized.ok())
        return Result<Connection>::failure(initialized.error());
    if (endpoint.host.empty())
        return Result<Connection>::failure("connection host is empty");
    auto resolved = resolve(endpoint, false);
    if (!resolved.ok())
        return Result<Connection>::failure(resolved.error());

    for (auto* address = resolved.value().info; address != nullptr; address = address->ai_next) {
        const auto socket = static_cast<NativeSocket>(
            ::socket(address->ai_family, address->ai_socktype, address->ai_protocol));
        if (socket == kInvalidSocket)
            continue;
        if (::connect(socket, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0) {
            auto impl = std::make_unique<Impl>();
            impl->socket = socket;
            impl->options = options;
            impl->state = ConnectionState::kConnected;
            return Result<Connection>::success(Connection(std::move(impl)));
        }
        close_socket(socket);
    }
    return Result<Connection>::failure("connection failed");
}

Result<void> Connection::send(const protocol::Message& message) {
    if (!impl_ || impl_->state != ConnectionState::kConnected)
        return Result<void>::failure("connection is closed");
    auto encoded = protocol::encode(message);
    if (!encoded.ok())
        return Result<void>::failure(encoded.error());
    auto result = write_all(impl_->socket, encoded.value());
    if (!result.ok())
        close();
    return result;
}

Result<std::vector<protocol::Message>> Connection::receive(std::chrono::milliseconds timeout) {
    if (!impl_ || impl_->state != ConnectionState::kConnected)
        return Result<std::vector<protocol::Message>>::failure("connection is closed");
    auto waited =
        wait_readable(impl_->socket, timeout.count() >= 0 ? timeout : impl_->options.io_timeout);
    if (!waited.ok())
        return Result<std::vector<protocol::Message>>::failure(waited.error());

    std::array<std::uint8_t, 16 * 1024> bytes{};
#if defined(_WIN32)
    const int received = recv(impl_->socket, reinterpret_cast<char*>(bytes.data()),
                              static_cast<int>(bytes.size()), 0);
#else
    const int received = static_cast<int>(recv(impl_->socket, bytes.data(), bytes.size(), 0));
#endif
    if (received > 0) {
        auto decoded = impl_->decoder.feed(
            std::span<const std::uint8_t>(bytes.data(), static_cast<std::size_t>(received)));
        if (!decoded.ok()) {
            close();
            return decoded;
        }
        return decoded;
    }
    impl_->state = ConnectionState::kClosed;
    close_socket(impl_->socket);
    impl_->socket = kInvalidSocket;
    if (received == 0) {
        auto finished = impl_->decoder.finish();
        if (!finished.ok())
            return Result<std::vector<protocol::Message>>::failure(
                "peer closed with partial frame");
        return Result<std::vector<protocol::Message>>::failure("peer closed");
    }
    return Result<std::vector<protocol::Message>>::failure("socket read failed");
}

Result<void> Connection::close() {
    if (!impl_)
        return Result<void>::success();
    if (impl_->socket != kInvalidSocket) {
#if defined(_WIN32)
        shutdown(impl_->socket, SD_BOTH);
#else
        shutdown(impl_->socket, SHUT_RDWR);
#endif
        close_socket(impl_->socket);
        impl_->socket = kInvalidSocket;
    }
    impl_->state = ConnectionState::kClosed;
    return Result<void>::success();
}

ConnectionState Connection::state() const noexcept {
    return impl_ ? impl_->state : ConnectionState::kClosed;
}

std::size_t Connection::buffered_bytes() const noexcept {
    return impl_ ? impl_->decoder.buffered_bytes() : 0;
}

Listener::Listener(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Listener::Listener(Listener&& other) noexcept : impl_(std::move(other.impl_)) {}

Listener& Listener::operator=(Listener&& other) noexcept {
    if (this == &other)
        return *this;
    close();
    impl_ = std::move(other.impl_);
    return *this;
}

Listener::~Listener() { close(); }

Result<Listener> Listener::bind(const Endpoint& endpoint, ConnectionOptions options) {
    auto initialized = initialize_sockets();
    if (!initialized.ok())
        return Result<Listener>::failure(initialized.error());
    auto resolved = resolve(endpoint, true);
    if (!resolved.ok())
        return Result<Listener>::failure(resolved.error());

    for (auto* address = resolved.value().info; address != nullptr; address = address->ai_next) {
        const auto socket = static_cast<NativeSocket>(
            ::socket(address->ai_family, address->ai_socktype, address->ai_protocol));
        if (socket == kInvalidSocket)
            continue;
        int reuse = 1;
        setsockopt(socket, SOL_SOCKET, SO_REUSEADDR,
#if defined(_WIN32)
                   reinterpret_cast<const char*>(&reuse), sizeof(reuse)
#else
                   &reuse, sizeof(reuse)
#endif
        );
        if (::bind(socket, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0 &&
            ::listen(socket, 16) == 0) {
            auto impl = std::make_unique<Impl>();
            impl->socket = socket;
            impl->options = options;
            impl->endpoint = endpoint;
            sockaddr_storage local{};
#if defined(_WIN32)
            int length = sizeof(local);
#else
            socklen_t length = sizeof(local);
#endif
            if (getsockname(socket, reinterpret_cast<sockaddr*>(&local), &length) == 0) {
                if (local.ss_family == AF_INET)
                    impl->endpoint.port = ntohs(reinterpret_cast<sockaddr_in*>(&local)->sin_port);
                else if (local.ss_family == AF_INET6)
                    impl->endpoint.port = ntohs(reinterpret_cast<sockaddr_in6*>(&local)->sin6_port);
            }
            return Result<Listener>::success(Listener(std::move(impl)));
        }
        close_socket(socket);
    }
    return Result<Listener>::failure("listener bind failed");
}

Result<Connection> Listener::accept(std::chrono::milliseconds timeout) {
    if (!impl_ || impl_->socket == kInvalidSocket)
        return Result<Connection>::failure("listener is closed");
    auto waited = wait_readable(impl_->socket, timeout);
    if (!waited.ok())
        return Result<Connection>::failure(waited.error());
    const auto socket = static_cast<NativeSocket>(::accept(impl_->socket, nullptr, nullptr));
    if (socket == kInvalidSocket)
        return Result<Connection>::failure("accept failed");
    auto connection = std::make_unique<Connection::Impl>();
    connection->socket = socket;
    connection->options = impl_->options;
    connection->state = ConnectionState::kConnected;
    return Result<Connection>::success(Connection(std::move(connection)));
}

Result<void> Listener::close() {
    if (!impl_)
        return Result<void>::success();
    if (impl_->socket != kInvalidSocket) {
#if defined(_WIN32)
        shutdown(impl_->socket, SD_BOTH);
#else
        shutdown(impl_->socket, SHUT_RDWR);
#endif
        close_socket(impl_->socket);
        impl_->socket = kInvalidSocket;
    }
    return Result<void>::success();
}

Endpoint Listener::local_endpoint() const { return impl_ ? impl_->endpoint : Endpoint{}; }

bool Listener::open() const noexcept { return impl_ && impl_->socket != kInvalidSocket; }

} // namespace lanlink::transport
