#include "lanlink/discovery/discovery.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <mutex>
#include <sstream>
#include <unordered_map>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace lanlink::discovery {
namespace {
#if defined(_WIN32)
using Socket = SOCKET;
constexpr Socket kInvalidSocket = INVALID_SOCKET;
#else
using Socket = int;
constexpr Socket kInvalidSocket = -1;
#endif

void close_socket(Socket s) {
    if (s == kInvalidSocket) return;
#if defined(_WIN32)
    closesocket(s);
#else
    close(s);
#endif
}

Result<void> sockets_init() {
#if defined(_WIN32)
    static std::once_flag once;
    static int status = 0;
    std::call_once(once, [] { WSADATA data{}; status = WSAStartup(MAKEWORD(2, 2), &data); });
    if (status != 0) return Result<void>::failure("socket initialization failed");
#endif
    return Result<void>::success();
}

bool valid_group(std::string_view group) {
    if (group.empty() || group.size() > 32) return false;
    for (unsigned char c : group)
        if (!(std::isalnum(c) || c == '-' || c == '_' || c == '.')) return false;
    return true;
}

template <typename T>
Result<T> number(std::string_view value, const char* label) {
    T out{};
    const auto* first = value.data();
    const auto* last = first + value.size();
    auto result = std::from_chars(first, last, out);
    if (result.ec != std::errc{} || result.ptr != last)
        return Result<T>::failure(std::string("invalid ") + label);
    return Result<T>::success(out);
}

bool id_less(const DeviceId& a, const DeviceId& b) {
    return a.high < b.high || (a.high == b.high && a.low < b.low);
}
}

struct DiscoveryService::Impl {
    Socket socket{kInvalidSocket};
    DiscoveryConfig config;
    IClock::time_point next_announce{};
    std::unordered_map<DeviceId, DiscoveryPeer> peers;
};

Result<std::string> DiscoveryService::encode_announcement(const DiscoveryAnnouncement& a) {
    if (!valid_group(a.group)) return Result<std::string>::failure("invalid discovery group");
    if (a.device == DeviceId{} || a.display == DisplayId{})
        return Result<std::string>::failure("discovery IDs must be non-zero");
    if (a.tcp_port == 0) return Result<std::string>::failure("invalid discovery TCP port");
    std::ostringstream out;
    out << "LANLINK1 " << a.group << ' ' << a.device.high << ' ' << a.device.low << ' '
        << a.tcp_port << ' ' << a.display.high << ' ' << a.display.low;
    auto packet = out.str();
    if (packet.size() > 512) return Result<std::string>::failure("discovery packet too large");
    return Result<std::string>::success(std::move(packet));
}

Result<std::string> DiscoveryService::encode_announcement(const DiscoveryConfig& c) {
    return encode_announcement(DiscoveryAnnouncement{c.group, c.device_id, c.display_id, c.tcp_port});
}

Result<DiscoveryAnnouncement> DiscoveryService::decode_announcement(std::string_view packet) {
    if (packet.empty() || packet.size() > 512 || packet.back() == '\n' || packet.back() == '\r')
        return Result<DiscoveryAnnouncement>::failure("invalid discovery packet size");
    std::istringstream in{std::string(packet)};
    std::string magic, group, sh, sl, port, dh, dl, extra;
    if (!(in >> magic >> group >> sh >> sl >> port >> dh >> dl) || (in >> extra) || magic != "LANLINK1")
        return Result<DiscoveryAnnouncement>::failure("invalid discovery packet");
    auto high = number<std::uint64_t>(sh, "device ID");
    auto low = number<std::uint64_t>(sl, "device ID");
    auto tcp = number<std::uint16_t>(port, "TCP port");
    auto disp_high = number<std::uint64_t>(dh, "display ID");
    auto disp_low = number<std::uint64_t>(dl, "display ID");
    if (!high.ok() || !low.ok() || !tcp.ok() || !disp_high.ok() || !disp_low.ok())
        return Result<DiscoveryAnnouncement>::failure("invalid discovery numeric field");
    DiscoveryAnnouncement result{std::move(group), {high.value(), low.value()},
                                 {disp_high.value(), disp_low.value()}, tcp.value()};
    if (!valid_group(result.group) || result.device == DeviceId{} || result.display == DisplayId{} ||
        result.tcp_port == 0)
        return Result<DiscoveryAnnouncement>::failure("invalid discovery announcement");
    return Result<DiscoveryAnnouncement>::success(std::move(result));
}

DiscoveryService::DiscoveryService(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
DiscoveryService::DiscoveryService(DiscoveryService&&) noexcept = default;
DiscoveryService& DiscoveryService::operator=(DiscoveryService&& other) noexcept {
    if (this == &other) return *this;
    (void)stop();
    impl_ = std::move(other.impl_);
    peers_ = std::move(other.peers_);
    return *this;
}
DiscoveryService::~DiscoveryService() { (void)stop(); }

Result<DiscoveryService> DiscoveryService::bind(const DiscoveryConfig& config) {
    if (!valid_group(config.group) || config.discovery_port == 0 || config.tcp_port == 0 ||
        config.device_id == DeviceId{} || config.display_id == DisplayId{} ||
        config.announce_interval.count() <= 0 || config.peer_ttl.count() <= 0)
        return Result<DiscoveryService>::failure("invalid discovery configuration");
    auto init = sockets_init();
    if (!init.ok()) return Result<DiscoveryService>::failure(init.error());
    Socket socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == kInvalidSocket) return Result<DiscoveryService>::failure("discovery socket failed");
    int enabled = 1;
#if defined(_WIN32)
    if (setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&enabled), sizeof(enabled)) != 0 ||
        setsockopt(socket, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&enabled), sizeof(enabled)) != 0) {
#else
    if (setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) != 0 ||
        setsockopt(socket, SOL_SOCKET, SO_BROADCAST, &enabled, sizeof(enabled)) != 0) {
#endif
        close_socket(socket); return Result<DiscoveryService>::failure("discovery socket options failed");
    }
#if defined(_WIN32)
    u_long nonblocking = 1;
    if (ioctlsocket(socket, FIONBIO, &nonblocking) != 0) {
#else
    if (fcntl(socket, F_SETFL, fcntl(socket, F_GETFL, 0) | O_NONBLOCK) != 0) {
#endif
        close_socket(socket); return Result<DiscoveryService>::failure("discovery socket setup failed");
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(config.discovery_port);
    if (::bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close_socket(socket); return Result<DiscoveryService>::failure("discovery bind failed");
    }
    auto impl = std::make_unique<Impl>();
    impl->socket = socket;
    impl->config = config;
    // Epoch makes the first poll announce regardless of the caller's injected clock.
    impl->next_announce = IClock::time_point{};
    return Result<DiscoveryService>::success(DiscoveryService(std::move(impl)));
}

Result<std::vector<DiscoveryPeer>> DiscoveryService::poll(IClock::time_point now) {
    if (!impl_ || impl_->socket == kInvalidSocket) return Result<std::vector<DiscoveryPeer>>::failure("discovery stopped");
    auto encoded = encode_announcement(impl_->config);
    if (now >= impl_->next_announce) {
        sockaddr_in destination{};
        destination.sin_family = AF_INET;
        destination.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        destination.sin_port = htons(impl_->config.discovery_port);
#if defined(_WIN32)
        (void)sendto(impl_->socket, encoded.value().data(), static_cast<int>(encoded.value().size()), 0,
                     reinterpret_cast<sockaddr*>(&destination), sizeof(destination));
#else
        (void)sendto(impl_->socket, encoded.value().data(), encoded.value().size(), 0,
                     reinterpret_cast<sockaddr*>(&destination), sizeof(destination));
#endif
        impl_->next_announce = now + impl_->config.announce_interval;
    }
    std::array<char, 513> packet{};
    for (;;) {
        sockaddr_in source{};
#if defined(_WIN32)
        int source_size = sizeof(source);
        const int received = recvfrom(impl_->socket, packet.data(), static_cast<int>(packet.size()), 0,
                                      reinterpret_cast<sockaddr*>(&source), &source_size);
        if (received < 0 && WSAGetLastError() == WSAEWOULDBLOCK) break;
#else
        socklen_t source_size = sizeof(source);
        const auto received = recvfrom(impl_->socket, packet.data(), packet.size(), 0,
                                       reinterpret_cast<sockaddr*>(&source), &source_size);
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
#endif
        if (received <= 0) break;
        if (received > 512) continue;
        auto decoded = decode_announcement(std::string_view(packet.data(), static_cast<std::size_t>(received)));
        if (!decoded.ok() || decoded.value().group != impl_->config.group || decoded.value().device == impl_->config.device_id)
            continue;
        char address_text[INET_ADDRSTRLEN]{};
        if (inet_ntop(AF_INET, &source.sin_addr, address_text, sizeof(address_text)) == nullptr) continue;
        impl_->peers[decoded.value().device] = DiscoveryPeer{decoded.value().device, decoded.value().display,
            {address_text, decoded.value().tcp_port}, now};
    }
    for (auto it = impl_->peers.begin(); it != impl_->peers.end();) {
        if (now - it->second.last_seen >= impl_->config.peer_ttl) it = impl_->peers.erase(it);
        else ++it;
    }
    peers_.clear();
    for (const auto& [id, peer] : impl_->peers) peers_.push_back(peer);
    std::sort(peers_.begin(), peers_.end(), [](const auto& a, const auto& b) { return id_less(a.device, b.device); });
    return Result<std::vector<DiscoveryPeer>>::success(peers_);
}

Result<void> DiscoveryService::stop() {
    if (impl_ && impl_->socket != kInvalidSocket) {
        close_socket(impl_->socket);
        impl_->socket = kInvalidSocket;
    }
    return Result<void>::success();
}
} // namespace lanlink::discovery
