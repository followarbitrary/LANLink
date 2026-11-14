#include "lanlink/discovery/discovery.hpp"

#include <gtest/gtest.h>
#include <thread>

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace lanlink;
using namespace lanlink::discovery;

namespace {
Uuid128 id(std::uint64_t n) { return {0, n}; }
DiscoveryConfig config(std::uint16_t port) {
    DiscoveryConfig c;
    c.group = "test-group";
    c.discovery_port = port;
    c.device_id = id(1);
    c.display_id = id(2);
    c.tcp_port = 42000;
    c.announce_interval = std::chrono::hours(1);
    c.peer_ttl = std::chrono::seconds(2);
    return c;
}

void inject(std::uint16_t port, std::string_view packet) {
#if defined(_WIN32)
    SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#else
    int socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#endif
    ASSERT_NE(socket, -1);
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(port);
    ASSERT_EQ(inet_pton(AF_INET, "127.0.0.1", &destination.sin_addr), 1);
#if defined(_WIN32)
    ASSERT_EQ(sendto(socket, packet.data(), static_cast<int>(packet.size()), 0,
                     reinterpret_cast<sockaddr*>(&destination), sizeof(destination)),
              static_cast<int>(packet.size()));
    closesocket(socket);
#else
    ASSERT_EQ(sendto(socket, packet.data(), packet.size(), 0,
                     reinterpret_cast<sockaddr*>(&destination), sizeof(destination)),
              static_cast<ssize_t>(packet.size()));
    close(socket);
#endif
}

std::string packet(std::string group, std::uint64_t device, std::uint64_t display,
                   std::uint16_t tcp_port) {
    DiscoveryAnnouncement announcement{std::move(group), id(device), id(display), tcp_port};
    auto encoded = DiscoveryService::encode_announcement(announcement);
    EXPECT_TRUE(encoded.ok());
    return encoded.value();
}
}

TEST(Discovery, AnnouncementRoundTripAndValidation) {
    auto encoded = DiscoveryService::encode_announcement(config(42001));
    ASSERT_TRUE(encoded.ok());
    auto decoded = DiscoveryService::decode_announcement(encoded.value());
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded.value().group, "test-group");
    EXPECT_EQ(decoded.value().device, id(1));
    EXPECT_EQ(decoded.value().display, id(2));
    EXPECT_EQ(decoded.value().tcp_port, 42000);
    EXPECT_FALSE(DiscoveryService::decode_announcement("LANLINK2 test-group 0 1 42 0 2").ok());
    EXPECT_FALSE(DiscoveryService::decode_announcement("LANLINK1 bad/group 0 1 42 0 2").ok());
    EXPECT_FALSE(DiscoveryService::decode_announcement("LANLINK1 test-group x 1 42 0 2").ok());
    EXPECT_FALSE(DiscoveryService::decode_announcement("LANLINK1 test-group 0 1 0 0 2").ok());
}

TEST(Discovery, FiltersSelfAndRefreshesEndpoint) {
    auto a = config(42002);
    auto b = config(42002);
    b.device_id = id(2);
    b.display_id = id(3);
    b.tcp_port = 42001;
    auto service = DiscoveryService::bind(a);
    ASSERT_TRUE(service.ok()) << service.error();
    ASSERT_TRUE(service.value().poll(IClock::time_point{}).ok());
    inject(a.discovery_port, packet("other-group", 2, 3, 42001));
    inject(a.discovery_port, packet(a.group, 1, 3, 42001));
    inject(a.discovery_port, packet(a.group, 2, 3, 42001));
    auto oversized = packet(a.group, 9, 9, 42009);
    oversized.append(600, 'x');
    inject(a.discovery_port, oversized);
    auto peers = service.value().poll(IClock::time_point{} + std::chrono::milliseconds(1));
    ASSERT_TRUE(peers.ok());
    ASSERT_EQ(peers.value().size(), 1u);
    EXPECT_EQ(peers.value()[0].endpoint, (transport::Endpoint{"127.0.0.1", 42001}));
    auto malformed = std::string("LANLINK1 ") + a.group + " malformed";
    auto oversized_before_valid = packet(a.group, 4, 4, 42004);
    oversized_before_valid.append(600, 'x');
    inject(a.discovery_port, malformed);
    inject(a.discovery_port, {});
    inject(a.discovery_port, oversized_before_valid);
    inject(a.discovery_port, packet(a.group, 4, 4, 42004));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    peers = service.value().poll(IClock::time_point{} + std::chrono::milliseconds(2));
    ASSERT_TRUE(peers.ok());
    ASSERT_EQ(peers.value().size(), 2u);
    EXPECT_EQ(peers.value()[1].endpoint.port, 42004);
    inject(a.discovery_port, packet(a.group, 2, 3, 42002));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    peers = service.value().poll(IClock::time_point{} + std::chrono::milliseconds(2));
    ASSERT_TRUE(peers.ok());
    ASSERT_EQ(peers.value().size(), 2u);
    EXPECT_EQ(peers.value()[0].endpoint.port, 42002);
    peers = service.value().poll(IClock::time_point{} + std::chrono::seconds(3));
    ASSERT_TRUE(peers.ok());
    EXPECT_TRUE(peers.value().empty());
    EXPECT_TRUE(service.value().stop().ok());
}

TEST(Discovery, OrdersPeersByDeviceId) {
    auto a = config(42003);
    auto service = DiscoveryService::bind(a);
    ASSERT_TRUE(service.ok());
    ASSERT_TRUE(service.value().poll(IClock::time_point{}).ok());
    inject(a.discovery_port, packet(a.group, 5, 5, 42005));
    inject(a.discovery_port, packet(a.group, 3, 3, 42003));
    inject(a.discovery_port, packet(a.group, 4, 4, 42004));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    auto peers = service.value().poll(IClock::time_point{} + std::chrono::milliseconds(1));
    ASSERT_TRUE(peers.ok());
    ASSERT_EQ(peers.value().size(), 3u);
    EXPECT_EQ(peers.value()[0].device, id(3));
    EXPECT_EQ(peers.value()[1].device, id(4));
    EXPECT_EQ(peers.value()[2].device, id(5));
    EXPECT_TRUE(service.value().stop().ok());
}

namespace {
ScreenTopology sample_topology(std::uint64_t version = 1) {
    ScreenTopology topology;
    Display a{id(10), id(20), Rect{0, 0, 100, 100}, 1.0, Rotation::k0};
    Display b{id(11), id(21), Rect{100, 0, 200, 100}, 2.0, Rotation::k90};
    ScreenLink link{a.id, Edge::kRight, b.id, Edge::kLeft, 0.1, 0.9, version};
    EXPECT_TRUE(topology.set_snapshot(version, {a, b}, {link}).ok());
    return topology;
}
}

TEST(Discovery, TopologyRoundTripAndStrictValidation) {
    TopologySnapshot snapshot{id(7), 4, sample_topology(4), "test-group"};
    auto encoded = DiscoveryService::encode_topology(snapshot);
    ASSERT_TRUE(encoded.ok()) << encoded.error();
    auto decoded = DiscoveryService::decode_topology(encoded.value());
    ASSERT_TRUE(decoded.ok()) << decoded.error();
    EXPECT_EQ(decoded.value().origin, snapshot.origin);
    EXPECT_EQ(decoded.value().revision, 4u);
    ASSERT_EQ(decoded.value().topology.displays().size(), 2u);
    EXPECT_EQ(decoded.value().topology.displays()[1].rotation, Rotation::k90);
    EXPECT_FALSE(DiscoveryService::decode_topology("LANLINK2 test-group 0 0 4 0 0").ok());
    EXPECT_FALSE(DiscoveryService::decode_topology("LANLINK2 test-group 0 7 4 1 0 0 0 1 1 0 0 10 10 1 0").ok());
    auto oversized = encoded.value();
    oversized.append(kMaxTopologyPacketSize, 'x');
    EXPECT_FALSE(DiscoveryService::decode_topology(oversized).ok());
}

TEST(Discovery, TopologyPollKeepsNewestRevisionPerOrigin) {
    auto a = config(42004);
    auto service = DiscoveryService::bind(a);
    ASSERT_TRUE(service.ok()) << service.error();
    auto newer = DiscoveryService::encode_topology(TopologySnapshot{id(2), 2, sample_topology(2), a.group});
    auto older = DiscoveryService::encode_topology(TopologySnapshot{id(2), 1, sample_topology(1), a.group});
    ASSERT_TRUE(newer.ok());
    ASSERT_TRUE(older.ok());
    inject(a.discovery_port, newer.value());
    inject(a.discovery_port, older.value());
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_TRUE(service.value().poll(IClock::time_point{} + std::chrono::milliseconds(1)).ok());
    ASSERT_EQ(service.value().topology_snapshots().size(), 1u);
    EXPECT_EQ(service.value().topology_snapshots()[0].revision, 2u);
    inject(a.discovery_port, DiscoveryService::encode_topology(TopologySnapshot{id(3), 1, sample_topology(1), "other-group"}).value());
    ASSERT_TRUE(service.value().poll(IClock::time_point{} + std::chrono::milliseconds(2)).ok());
    EXPECT_EQ(service.value().topology_snapshots().size(), 1u);
}

TEST(Discovery, BroadcastTopologyStoresLocalSnapshot) {
    auto a = config(42005);
    auto service = DiscoveryService::bind(a);
    ASSERT_TRUE(service.ok());
    ASSERT_TRUE(service.value().broadcast_topology(sample_topology(9), 9).ok());
    ASSERT_EQ(service.value().topology_snapshots().size(), 1u);
    EXPECT_EQ(service.value().topology_snapshots()[0].origin, a.device_id);
    EXPECT_EQ(service.value().topology_snapshots()[0].revision, 9u);
}

TEST(Discovery, RejectsInvalidTopologyFieldsDuringEncoding) {
    ScreenTopology invalid_rotation;
    Display display{id(10), id(20), Rect{0, 0, 10, 10}, 1.0, static_cast<Rotation>(1)};
    ASSERT_TRUE(invalid_rotation.set_snapshot(1, {display}, {}).ok());
    EXPECT_FALSE(DiscoveryService::encode_topology(TopologySnapshot{id(2), 1, invalid_rotation, "test-group"}).ok());

    ScreenTopology invalid_link;
    Display a{id(10), id(20), Rect{0, 0, 10, 10}, 1.0, Rotation::k0};
    Display b{id(11), id(21), Rect{10, 0, 20, 10}, 1.0, Rotation::k0};
    ScreenLink link{a.id, Edge::kRight, b.id, Edge::kLeft, 0, 1, 1};
    ASSERT_TRUE(invalid_link.set_snapshot(1, {a, b}, {link}).ok());
    EXPECT_FALSE(DiscoveryService::encode_topology(TopologySnapshot{id(2), 2, invalid_link, "test-group"}).ok());
}

TEST(Discovery, BroadcastTopologyNeverDowngradesLocalRevision) {
    auto service = DiscoveryService::bind(config(42006));
    ASSERT_TRUE(service.ok());
    ASSERT_TRUE(service.value().broadcast_topology(sample_topology(2), 2).ok());
    ASSERT_TRUE(service.value().broadcast_topology(sample_topology(1), 1).ok());
    ASSERT_EQ(service.value().topology_snapshots().size(), 1u);
    EXPECT_EQ(service.value().topology_snapshots()[0].revision, 2u);
}

TEST(Discovery, RejectsSnapshotRevisionDifferentFromTopologyVersion) {
    ScreenTopology topology;
    Display display{id(10), id(20), Rect{0, 0, 10, 10}, 1.0, Rotation::k0};
    ASSERT_TRUE(topology.set_snapshot(1, {display}, {}).ok());
    EXPECT_FALSE(DiscoveryService::encode_topology(TopologySnapshot{id(2), 2, topology, "test-group"}).ok());
}

TEST(Discovery, ValidatesOlderBroadcastBeforeIgnoringRevision) {
    auto service = DiscoveryService::bind(config(42007));
    ASSERT_TRUE(service.ok());
    ASSERT_TRUE(service.value().broadcast_topology(sample_topology(2), 2).ok());
    ScreenTopology topology;
    Display display{id(10), id(20), Rect{0, 0, 10, 10}, 1.0, Rotation::k0};
    ASSERT_TRUE(topology.set_snapshot(1, {display}, {}).ok());
    EXPECT_FALSE(service.value().broadcast_topology(TopologySnapshot{id(1), 2, topology, "test-group"}).ok());
}
