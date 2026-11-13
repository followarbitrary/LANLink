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
    inject(a.discovery_port, packet(a.group, 2, 3, 42002));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    peers = service.value().poll(IClock::time_point{} + std::chrono::milliseconds(2));
    ASSERT_TRUE(peers.ok());
    ASSERT_EQ(peers.value().size(), 1u);
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
    auto peers = service.value().poll(IClock::time_point{} + std::chrono::milliseconds(1));
    ASSERT_TRUE(peers.ok());
    ASSERT_EQ(peers.value().size(), 3u);
    EXPECT_EQ(peers.value()[0].device, id(3));
    EXPECT_EQ(peers.value()[1].device, id(4));
    EXPECT_EQ(peers.value()[2].device, id(5));
    EXPECT_TRUE(service.value().stop().ok());
}
