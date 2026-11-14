#include "lanlink/discovery/discovery.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <mutex>
#include <optional>
#include <sstream>
#include <iomanip>
#include <cmath>
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
    IClock::time_point next_topology_announce{};
    std::unordered_map<DeviceId, DiscoveryPeer> peers;
    std::unordered_map<DeviceId, TopologySnapshot> topology_snapshots;
    std::optional<TopologySnapshot> local_topology;
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

namespace {
int edge_value(Edge edge) { return static_cast<int>(edge); }
bool valid_edge_value(int value) { return value >= 0 && value <= 3; }
bool finite_value(double value) { return std::isfinite(value); }

Result<double> decimal(std::string_view value, const char* label) {
    double out{};
    const auto* first = value.data();
    const auto* last = first + value.size();
    auto result = std::from_chars(first, last, out);
    if (result.ec != std::errc{} || result.ptr != last || !finite_value(out))
        return Result<double>::failure(std::string("invalid ") + label);
    return Result<double>::success(out);
}

bool send_packet(Socket socket, std::uint16_t port, std::string_view packet) {
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    destination.sin_port = htons(port);
#if defined(_WIN32)
    return sendto(socket, packet.data(), static_cast<int>(packet.size()), 0,
                  reinterpret_cast<sockaddr*>(&destination), sizeof(destination)) == static_cast<int>(packet.size());
#else
    return sendto(socket, packet.data(), packet.size(), 0,
                  reinterpret_cast<sockaddr*>(&destination), sizeof(destination)) == static_cast<ssize_t>(packet.size());
#endif
}
}

Result<std::string> DiscoveryService::encode_topology(const TopologySnapshot& snapshot) {
    if (!valid_group(snapshot.group) || snapshot.origin == DeviceId{} ||
        snapshot.topology.displays().size() > kMaxTopologyDisplays ||
        snapshot.topology.links().size() > kMaxTopologyLinks ||
        snapshot.revision != snapshot.topology.version())
        return Result<std::string>::failure("invalid topology snapshot");
    std::ostringstream out;
    out << std::setprecision(17) << "LANLINK2 " << snapshot.group << ' ' << snapshot.origin.high << ' '
        << snapshot.origin.low << ' ' << snapshot.revision << ' ' << snapshot.topology.displays().size() << ' '
        << snapshot.topology.links().size();
    for (const auto& display : snapshot.topology.displays()) {
        const auto& r = display.logical_bounds;
        const auto rotation = static_cast<int>(display.rotation);
        if (display.device == DeviceId{} || display.id == DisplayId{} || !finite_value(r.left) || !finite_value(r.top) ||
            !finite_value(r.right) || !finite_value(r.bottom) || !finite_value(display.scale) ||
            (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270))
            return Result<std::string>::failure("invalid topology display");
        out << ' ' << display.device.high << ' ' << display.device.low << ' ' << display.id.high << ' '
            << display.id.low << ' ' << r.left << ' ' << r.top << ' ' << r.right << ' ' << r.bottom << ' '
            << display.scale << ' ' << rotation;
    }
    for (const auto& link : snapshot.topology.links()) {
        if (link.source == DisplayId{} || link.target == DisplayId{} || link.source == link.target ||
            !valid_edge_value(edge_value(link.source_edge)) || !valid_edge_value(edge_value(link.target_edge)) ||
            !finite_value(link.tangent_min) || !finite_value(link.tangent_max) || link.tangent_min < 0.0 ||
            link.tangent_max > 1.0 || !(link.tangent_min < link.tangent_max) ||
            link.version != snapshot.revision)
            return Result<std::string>::failure("invalid topology link");
        out << ' ' << link.source.high << ' ' << link.source.low << ' ' << link.target.high << ' ' << link.target.low
            << ' ' << edge_value(link.source_edge) << ' ' << edge_value(link.target_edge) << ' '
            << link.tangent_min << ' ' << link.tangent_max << ' ' << link.version;
    }
    auto packet = out.str();
    if (packet.size() > kMaxTopologyPacketSize) return Result<std::string>::failure("topology packet too large");
    return Result<std::string>::success(std::move(packet));
}

Result<TopologySnapshot> DiscoveryService::decode_topology(std::string_view packet) {
    if (packet.empty() || packet.size() > kMaxTopologyPacketSize || packet.back() == '\n' || packet.back() == '\r')
        return Result<TopologySnapshot>::failure("invalid topology packet size");
    std::istringstream in{std::string(packet)};
    std::string magic, group, oh, ol, rev, ds, ls, extra;
    if (!(in >> magic >> group >> oh >> ol >> rev >> ds >> ls) || magic != "LANLINK2")
        return Result<TopologySnapshot>::failure("invalid topology packet");
    auto origin_high = number<std::uint64_t>(oh, "topology origin");
    auto origin_low = number<std::uint64_t>(ol, "topology origin");
    auto revision = number<std::uint64_t>(rev, "topology revision");
    auto display_count = number<std::size_t>(ds, "topology display count");
    auto link_count = number<std::size_t>(ls, "topology link count");
    if (!origin_high.ok() || !origin_low.ok() || !revision.ok() || !display_count.ok() || !link_count.ok() ||
        !valid_group(group) || DeviceId{origin_high.value(), origin_low.value()} == DeviceId{} ||
        display_count.value() > kMaxTopologyDisplays || link_count.value() > kMaxTopologyLinks)
        return Result<TopologySnapshot>::failure("invalid topology header");
    std::vector<Display> displays;
    displays.reserve(display_count.value());
    for (std::size_t i = 0; i < display_count.value(); ++i) {
        std::string dh, dl, ih, il, left, top, right, bottom, scale, rotation;
        if (!(in >> dh >> dl >> ih >> il >> left >> top >> right >> bottom >> scale >> rotation))
            return Result<TopologySnapshot>::failure("invalid topology display");
        auto d0=number<std::uint64_t>(dh,"display device"), d1=number<std::uint64_t>(dl,"display device");
        auto i0=number<std::uint64_t>(ih,"display id"), i1=number<std::uint64_t>(il,"display id");
        auto a=decimal(left,"display bounds"), b=decimal(top,"display bounds"), c=decimal(right,"display bounds"), d=decimal(bottom,"display bounds"), e=decimal(scale,"display scale");
        auto rot=number<int>(rotation,"display rotation");
        if (!d0.ok()||!d1.ok()||!i0.ok()||!i1.ok()||!a.ok()||!b.ok()||!c.ok()||!d.ok()||!e.ok()||!rot.ok()||
            DeviceId{d0.value(),d1.value()}==DeviceId{} || DisplayId{i0.value(),i1.value()}==DisplayId{} ||
            (rot.value()!=0 && rot.value()!=90 && rot.value()!=180 && rot.value()!=270))
            return Result<TopologySnapshot>::failure("invalid topology display");
        displays.push_back(Display{{d0.value(),d1.value()},{i0.value(),i1.value()},{a.value(),b.value(),c.value(),d.value()},e.value(),static_cast<Rotation>(rot.value())});
    }
    std::vector<ScreenLink> links;
    links.reserve(link_count.value());
    for (std::size_t i=0;i<link_count.value();++i) {
        std::string sh,sl,th,tl,se,te,tmin,tmax,version;
        if (!(in>>sh>>sl>>th>>tl>>se>>te>>tmin>>tmax>>version)) return Result<TopologySnapshot>::failure("invalid topology link");
        auto s0=number<std::uint64_t>(sh,"link source"); auto s1=number<std::uint64_t>(sl,"link source");
        auto t0=number<std::uint64_t>(th,"link target"); auto t1=number<std::uint64_t>(tl,"link target");
        auto sv=number<int>(se,"link edge"); auto tv=number<int>(te,"link edge");
        auto ver=number<std::uint64_t>(version,"link version");
        auto mn=decimal(tmin,"link tangent"), mx=decimal(tmax,"link tangent");
        if (!s0.ok()||!s1.ok()||!t0.ok()||!t1.ok()||!sv.ok()||!tv.ok()||!ver.ok()||!mn.ok()||!mx.ok()||!valid_edge_value(sv.value())||!valid_edge_value(tv.value())) return Result<TopologySnapshot>::failure("invalid topology link");
        links.push_back(ScreenLink{{s0.value(),s1.value()},static_cast<Edge>(sv.value()),{t0.value(),t1.value()},static_cast<Edge>(tv.value()),mn.value(),mx.value(),ver.value()});
    }
    if (in >> extra) return Result<TopologySnapshot>::failure("invalid topology trailing data");
    TopologySnapshot result{{origin_high.value(),origin_low.value()},revision.value(),{},std::move(group)};
    auto set = result.topology.set_snapshot(result.revision,std::move(displays),std::move(links));
    if (!set.ok()) return Result<TopologySnapshot>::failure(set.error());
    return Result<TopologySnapshot>::success(std::move(result));
}

DiscoveryService::DiscoveryService(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
DiscoveryService::DiscoveryService(DiscoveryService&&) noexcept = default;
DiscoveryService& DiscoveryService::operator=(DiscoveryService&& other) noexcept {
    if (this == &other) return *this;
    (void)stop();
    impl_ = std::move(other.impl_);
    peers_ = std::move(other.peers_);
    topology_snapshots_ = std::move(other.topology_snapshots_);
    return *this;
}
DiscoveryService::~DiscoveryService() { (void)stop(); }

Result<DiscoveryService> DiscoveryService::bind(const DiscoveryConfig& config) {
    if (!valid_group(config.group) || config.discovery_port == 0 || config.tcp_port == 0 ||
        config.device_id == DeviceId{} || config.display_id == DisplayId{} ||
        config.announce_interval.count() <= 0 || config.peer_ttl.count() <= 0 ||
        config.topology_announce_interval.count() <= 0)
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
    impl->next_topology_announce = IClock::time_point{};
    return Result<DiscoveryService>::success(DiscoveryService(std::move(impl)));
}

Result<std::vector<DiscoveryPeer>> DiscoveryService::poll(IClock::time_point now) {
    if (!impl_ || impl_->socket == kInvalidSocket) return Result<std::vector<DiscoveryPeer>>::failure("discovery stopped");
    auto encoded = encode_announcement(impl_->config);
    if (now >= impl_->next_announce) {
        (void)send_packet(impl_->socket, impl_->config.discovery_port, encoded.value());
        impl_->next_announce = now + impl_->config.announce_interval;
    }
    if (impl_->local_topology.has_value() && now >= impl_->next_topology_announce) {
        auto topology_packet = encode_topology(*impl_->local_topology);
        if (topology_packet.ok()) (void)send_packet(impl_->socket, impl_->config.discovery_port, topology_packet.value());
        impl_->next_topology_announce = now + impl_->config.topology_announce_interval;
    }
    std::array<char, kMaxTopologyPacketSize + 1> packet{};
    for (;;) {
        sockaddr_in source{};
#if defined(_WIN32)
        int source_size = sizeof(source);
        const int received = recvfrom(impl_->socket, packet.data(), static_cast<int>(packet.size()), 0,
                                      reinterpret_cast<sockaddr*>(&source), &source_size);
        if (received < 0) {
            const auto error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK) break;
            if (error == WSAEMSGSIZE) continue;
            return Result<std::vector<DiscoveryPeer>>::failure("discovery recvfrom failed");
        }
#else
        socklen_t source_size = sizeof(source);
        const auto received = recvfrom(impl_->socket, packet.data(), packet.size(), 0,
                                       reinterpret_cast<sockaddr*>(&source), &source_size);
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return Result<std::vector<DiscoveryPeer>>::failure("discovery recvfrom failed");
        }
#endif
        if (received == 0) continue;
        const std::string_view received_packet(packet.data(), static_cast<std::size_t>(received));
        if (received_packet.substr(0, 8) == "LANLINK2") {
            auto topology = decode_topology(received_packet);
            if (!topology.ok() || topology.value().group != impl_->config.group ||
                topology.value().origin == impl_->config.device_id) continue;
            auto origin = topology.value().origin;
            auto existing = impl_->topology_snapshots.find(origin);
            if (existing == impl_->topology_snapshots.end() || topology.value().revision > existing->second.revision) {
                impl_->topology_snapshots[origin] = topology.value();
            }
            continue;
        }
        if (received_packet.size() > 512) continue;
        auto decoded = decode_announcement(received_packet);
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
    topology_snapshots_.clear();
    for (const auto& [id, snapshot] : impl_->topology_snapshots) topology_snapshots_.push_back(snapshot);
    std::sort(topology_snapshots_.begin(), topology_snapshots_.end(), [](const auto& a, const auto& b) {
        return id_less(a.origin, b.origin);
    });
    return Result<std::vector<DiscoveryPeer>>::success(peers_);
}

Result<void> DiscoveryService::broadcast_topology(const TopologySnapshot& snapshot) {
    if (!impl_ || impl_->socket == kInvalidSocket) return Result<void>::failure("discovery stopped");
    if (snapshot.group.empty()) {
        TopologySnapshot with_group = snapshot;
        with_group.group = impl_->config.group;
        return broadcast_topology(with_group);
    }
    if (snapshot.group != impl_->config.group || snapshot.origin != impl_->config.device_id)
        return Result<void>::failure("topology snapshot origin or group mismatch");
    auto encoded = encode_topology(snapshot);
    if (!encoded.ok()) return Result<void>::failure(encoded.error());
    auto existing = impl_->topology_snapshots.find(snapshot.origin);
    if (existing != impl_->topology_snapshots.end() && snapshot.revision <= existing->second.revision)
        return Result<void>::success();
    if (!send_packet(impl_->socket, impl_->config.discovery_port, encoded.value()))
        return Result<void>::failure("topology broadcast failed");
    impl_->local_topology = snapshot;
    impl_->topology_snapshots[snapshot.origin] = snapshot;
    impl_->next_topology_announce = IClock::Clock::now() + impl_->config.topology_announce_interval;
    topology_snapshots_.clear();
    for (const auto& [id, value] : impl_->topology_snapshots) topology_snapshots_.push_back(value);
    std::sort(topology_snapshots_.begin(), topology_snapshots_.end(), [](const auto& a, const auto& b) { return id_less(a.origin, b.origin); });
    return Result<void>::success();
}

Result<void> DiscoveryService::broadcast_topology(const ScreenTopology& topology, std::uint64_t revision) {
    return broadcast_topology(TopologySnapshot{impl_ ? impl_->config.device_id : DeviceId{}, revision, topology,
                                                impl_ ? impl_->config.group : std::string{}});
}

Result<void> DiscoveryService::stop() {
    if (impl_ && impl_->socket != kInvalidSocket) {
        close_socket(impl_->socket);
        impl_->socket = kInvalidSocket;
    }
    return Result<void>::success();
}
} // namespace lanlink::discovery
