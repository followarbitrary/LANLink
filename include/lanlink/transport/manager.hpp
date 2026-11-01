#pragma once

#include "lanlink/common/clock.hpp"
#include "lanlink/common/result.hpp"
#include "lanlink/transport/session.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <memory>
#include <string>
#include <vector>

namespace lanlink::transport {

using PeerHandle = std::uint64_t;
using ConnectionDialer =
    std::function<Result<Connection>(const Endpoint&, const ConnectionOptions&)>;

enum class PeerState {
    kConnecting,
    kConnected,
    kBackoff,
    kClosed,
    Connecting = kConnecting,
    Connected = kConnected,
    Backoff = kBackoff,
    Closed = kClosed,
};

struct ReconnectPolicy {
    std::chrono::milliseconds initial_delay{100};
    std::chrono::milliseconds max_delay{2000};
};

struct OutgoingPeer {
    Endpoint endpoint;
    ConnectionOptions connection;
    PeerSessionConfig session;
    ReconnectPolicy reconnect;
};

class ConnectionManager {
  public:
    explicit ConnectionManager(ConnectionDialer dialer = {});
    ~ConnectionManager();

    ConnectionManager(ConnectionManager&&) noexcept = default;
    ConnectionManager& operator=(ConnectionManager&&) noexcept = default;
    ConnectionManager(const ConnectionManager&) = delete;
    ConnectionManager& operator=(const ConnectionManager&) = delete;

    Result<PeerHandle> add_outgoing(OutgoingPeer peer);
    Result<PeerHandle> add_incoming(Connection connection, PeerSessionConfig session,
                                    IClock::time_point now);

    Result<void> poll(IClock::time_point now,
                      std::chrono::milliseconds timeout = std::chrono::milliseconds{0});
    Result<void> tick(IClock::time_point now);
    Result<void> send_actions(PeerHandle handle, const std::vector<RouterAction>& actions,
                              IClock::time_point now);
    Result<void> send_actions(PeerHandle handle, const std::vector<RouterAction>& actions);
    Result<void> remove(PeerHandle handle, IClock::time_point now);

    std::optional<PeerState> state(PeerHandle handle) const;
    std::size_t size() const noexcept;

  private:
    struct Slot {
        PeerHandle handle{};
        PeerState state{PeerState::kClosed};
        bool outgoing{false};
        Endpoint endpoint;
        ConnectionOptions connection;
        PeerSessionConfig session_config;
        ReconnectPolicy reconnect;
        std::unique_ptr<PeerSession> session;
        std::chrono::milliseconds next_delay{};
        IClock::time_point next_retry{};
        std::string last_error;
    };

    Slot* find(PeerHandle handle) noexcept;
    const Slot* find(PeerHandle handle) const noexcept;
    Result<void> attempt(Slot& slot, IClock::time_point now);
    Result<void> handle_failure(Slot& slot, const std::string& error,
                                IClock::time_point now);
    void schedule_retry(Slot& slot, IClock::time_point now);
    Result<void> drive(Slot& slot, IClock::time_point now,
                       std::chrono::milliseconds timeout);

    ConnectionDialer dialer_;
    std::vector<Slot> slots_;
    PeerHandle next_handle_{1};
};

} // namespace lanlink::transport
