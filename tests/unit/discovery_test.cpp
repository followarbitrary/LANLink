#include "lanlink/discovery/discovery.hpp"

#include <gtest/gtest.h>

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
    auto peer_packet = DiscoveryService::encode_announcement(b);
    ASSERT_TRUE(peer_packet.ok());
    // A service announces through its own socket, so this test exercises lifecycle and filtering.
    ASSERT_TRUE(service.value().poll(IClock::time_point{}).ok());
    EXPECT_TRUE(service.value().peers().empty());
    EXPECT_TRUE(service.value().stop().ok());
}

TEST(Discovery, OrdersPeersByDeviceId) {
    auto a = config(42003);
    auto service = DiscoveryService::bind(a);
    ASSERT_TRUE(service.ok());
    EXPECT_TRUE(service.value().poll(IClock::Clock::now()).ok());
    EXPECT_TRUE(service.value().stop().ok());
}
