#pragma once

#include "lanlink/common/clock.hpp"
#include "lanlink/common/ids.hpp"
#include "lanlink/common/result.hpp"
#include "lanlink/transport/connection.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace lanlink::discovery {

struct DiscoveryConfig {
    std::string group{"lanlink"};
    std::uint16_t discovery_port{41001};
    DeviceId device_id{};
    DisplayId display_id{};
    std::uint16_t tcp_port{};
    std::chrono::milliseconds announce_interval{1000};
    std::chrono::milliseconds peer_ttl{3000};
};

struct DiscoveryAnnouncement {
    std::string group;
    DeviceId device;
    DisplayId display;
    std::uint16_t tcp_port{};
};

struct DiscoveryPeer {
    DeviceId device;
    DisplayId display;
    transport::Endpoint endpoint;
    IClock::time_point last_seen{};
    friend bool operator==(const DiscoveryPeer&, const DiscoveryPeer&) = default;
};

class DiscoveryService {
  public:
    DiscoveryService(DiscoveryService&&) noexcept;
    DiscoveryService& operator=(DiscoveryService&&) noexcept;
    DiscoveryService(const DiscoveryService&) = delete;
    DiscoveryService& operator=(const DiscoveryService&) = delete;
    ~DiscoveryService();

    static Result<DiscoveryService> bind(const DiscoveryConfig& config);

    // Drains all available datagrams, sends a periodic announcement, removes expired peers,
    // and returns the currently visible peers sorted by device ID.
    Result<std::vector<DiscoveryPeer>> poll(IClock::time_point now);
    Result<std::vector<DiscoveryPeer>> poll() { return poll(IClock::Clock::now()); }
    const std::vector<DiscoveryPeer>& peers() const noexcept { return peers_; }
    Result<void> stop();

    static Result<std::string> encode_announcement(const DiscoveryConfig& config);
    static Result<std::string> encode_announcement(const DiscoveryAnnouncement& announcement);
    static Result<DiscoveryAnnouncement> decode_announcement(std::string_view packet);

  private:
    struct Impl;
    explicit DiscoveryService(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
    std::vector<DiscoveryPeer> peers_;
};

} // namespace lanlink::discovery
