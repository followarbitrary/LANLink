#include "lanlink/transport/connection.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <utility>

#if LANLINK_HAS_OPENSSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

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
#include <netinet/tcp.h>
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

bool set_tcp_no_delay(NativeSocket socket) {
    int enabled = 1;
#if defined(_WIN32)
    return setsockopt(socket, IPPROTO_TCP, TCP_NODELAY,
                      reinterpret_cast<const char*>(&enabled), sizeof(enabled)) == 0;
#else
    return setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) == 0;
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

#if LANLINK_HAS_OPENSSL
void release_tls(SSL*& ssl, SSL_CTX*& context) {
    if (ssl != nullptr) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
        ssl = nullptr;
    }
    if (context != nullptr) {
        SSL_CTX_free(context);
        context = nullptr;
    }
}

Result<void> configure_tls_client(SSL*& ssl, SSL_CTX*& context, NativeSocket socket,
                                  const TlsOptions& options) {
    context = SSL_CTX_new(TLS_client_method());
    if (context == nullptr)
        return Result<void>::failure("TLS context setup failed");
    SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION);
    if (options.verify_peer) {
        SSL_CTX_set_verify(context, SSL_VERIFY_PEER, nullptr);
        if (options.ca_file.empty() ||
            SSL_CTX_load_verify_locations(context, options.ca_file.c_str(), nullptr) != 1) {
            release_tls(ssl, context);
            return Result<void>::failure("TLS trust configuration failed");
        }
    } else {
        SSL_CTX_set_verify(context, SSL_VERIFY_NONE, nullptr);
    }
    ssl = SSL_new(context);
    if (ssl == nullptr) {
        release_tls(ssl, context);
        return Result<void>::failure("TLS session setup failed");
    }
    if (!options.server_name.empty()) {
        SSL_set_tlsext_host_name(ssl, options.server_name.c_str());
        if (options.verify_peer &&
            X509_VERIFY_PARAM_set1_host(SSL_get0_param(ssl), options.server_name.c_str(), 0) != 1) {
            release_tls(ssl, context);
            return Result<void>::failure("TLS hostname setup failed");
        }
    }
    SSL_set_fd(ssl, static_cast<int>(socket));
    if (SSL_connect(ssl) != 1) {
        release_tls(ssl, context);
        return Result<void>::failure("TLS handshake failed");
    }
    return Result<void>::success();
}

Result<void> configure_tls_server(SSL*& ssl, SSL_CTX*& context, NativeSocket socket,
                                  const TlsOptions& options) {
    context = SSL_CTX_new(TLS_server_method());
    if (context == nullptr)
        return Result<void>::failure("TLS context setup failed");
    SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION);
    if (options.certificate_file.empty() || options.private_key_file.empty() ||
        SSL_CTX_use_certificate_file(context, options.certificate_file.c_str(), SSL_FILETYPE_PEM) !=
            1 ||
        SSL_CTX_use_PrivateKey_file(context, options.private_key_file.c_str(), SSL_FILETYPE_PEM) !=
            1 ||
        SSL_CTX_check_private_key(context) != 1) {
        release_tls(ssl, context);
        return Result<void>::failure("TLS server credentials failed");
    }
    if (options.verify_peer) {
        SSL_CTX_set_verify(context, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
        if (options.ca_file.empty() ||
            SSL_CTX_load_verify_locations(context, options.ca_file.c_str(), nullptr) != 1) {
            release_tls(ssl, context);
            return Result<void>::failure("TLS trust configuration failed");
        }
    }
    ssl = SSL_new(context);
    if (ssl == nullptr) {
        release_tls(ssl, context);
        return Result<void>::failure("TLS session setup failed");
    }
    SSL_set_fd(ssl, static_cast<int>(socket));
    if (SSL_accept(ssl) != 1) {
        release_tls(ssl, context);
        return Result<void>::failure("TLS handshake failed");
    }
    return Result<void>::success();
}

Result<void> write_all_tls(SSL* ssl, const std::vector<std::uint8_t>& bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto remaining = bytes.size() - offset;
        const int written = SSL_write(ssl, bytes.data() + offset, static_cast<int>(remaining));
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        return Result<void>::failure("TLS write failed");
    }
    return Result<void>::success();
}
#endif

} // namespace

struct Connection::Impl {
    NativeSocket socket{kInvalidSocket};
    ConnectionOptions options;
    protocol::FrameDecoder decoder;
    ConnectionState state{ConnectionState::kClosed};
#if LANLINK_HAS_OPENSSL
    SSL_CTX* tls_context{nullptr};
    SSL* tls{nullptr};
#endif

    ~Impl() {
#if LANLINK_HAS_OPENSSL
        release_tls(tls, tls_context);
#endif
        close_socket(socket);
    }
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
#if !LANLINK_HAS_OPENSSL
    if (options.tls.enabled)
        return Result<Connection>::failure("TLS unavailable");
#endif
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
            if (!set_tcp_no_delay(socket)) {
                close_socket(socket);
                continue;
            }
            auto impl = std::make_unique<Impl>();
            impl->socket = socket;
            impl->options = options;
            impl->state = ConnectionState::kConnected;
#if LANLINK_HAS_OPENSSL
            if (options.tls.enabled) {
                auto tls = configure_tls_client(impl->tls, impl->tls_context, socket, options.tls);
                if (!tls.ok())
                    return Result<Connection>::failure(tls.error());
            }
#endif
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
#if LANLINK_HAS_OPENSSL
    auto result = impl_->tls != nullptr ? write_all_tls(impl_->tls, encoded.value())
                                        : write_all(impl_->socket, encoded.value());
#else
    auto result = write_all(impl_->socket, encoded.value());
#endif
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
#if LANLINK_HAS_OPENSSL
    const int received = impl_->tls != nullptr
                              ? SSL_read(impl_->tls, bytes.data(), static_cast<int>(bytes.size()))
#if defined(_WIN32)
                              : recv(impl_->socket, reinterpret_cast<char*>(bytes.data()),
                                     static_cast<int>(bytes.size()), 0);
#else
                              : static_cast<int>(recv(impl_->socket, bytes.data(), bytes.size(), 0));
#endif
#elif defined(_WIN32)
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
#if LANLINK_HAS_OPENSSL
    if (impl_->tls != nullptr && received <= 0) {
        const int error = SSL_get_error(impl_->tls, received);
        if (error != SSL_ERROR_ZERO_RETURN && error != SSL_ERROR_SYSCALL) {
            close();
            return Result<std::vector<protocol::Message>>::failure("TLS read failed");
        }
    }
#endif
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
#if LANLINK_HAS_OPENSSL
    release_tls(impl_->tls, impl_->tls_context);
#endif
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
#if !LANLINK_HAS_OPENSSL
    if (options.tls.enabled)
        return Result<Listener>::failure("TLS unavailable");
#endif
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
    if (!set_tcp_no_delay(socket)) {
        close_socket(socket);
        return Result<Connection>::failure("TCP_NODELAY setup failed");
    }
    auto connection = std::make_unique<Connection::Impl>();
    connection->socket = socket;
    connection->options = impl_->options;
    connection->state = ConnectionState::kConnected;
#if LANLINK_HAS_OPENSSL
    if (impl_->options.tls.enabled) {
        auto tls = configure_tls_server(connection->tls, connection->tls_context, socket,
                                        impl_->options.tls);
        if (!tls.ok())
            return Result<Connection>::failure(tls.error());
    }
#endif
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
