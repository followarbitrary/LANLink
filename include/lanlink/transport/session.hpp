#pragma once

#include "lanlink/common/clock.hpp"
#include "lanlink/common/ids.hpp"
#include "lanlink/common/liveness.hpp"
#include "lanlink/common/result.hpp"
#include "lanlink/input/arbiter.hpp"
#include "lanlink/input/router.hpp"
#include "lanlink/platform/input_backend.hpp"
#include "lanlink/transport/connection.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace lanlink::transport {

struct PeerSessionConfig {
    DeviceId local_device;
    AuthenticatedPeer peer;
    InputRouter* router{nullptr};
    InputArbiter* arbiter{nullptr};
    platform::IInputBackend* backend{nullptr};
};

class PeerSession {
  public:
    PeerSession(Connection connection, PeerSessionConfig config);

    Result<void> start(IClock::time_point now);
    Result<void> send_actions(const std::vector<RouterAction>& actions);
    Result<void> pump(IClock::time_point now,
                      std::chrono::milliseconds timeout = std::chrono::milliseconds{-1});
    Result<void> tick(IClock::time_point now);
    Result<void> disconnect(IClock::time_point now);

    bool connected() const noexcept { return connection_.connected() && !disconnected_; }
    bool hello_received() const noexcept { return hello_received_; }

  private:
    Result<void> dispatch_actions(const std::vector<RouterAction>& actions, IClock::time_point now);
    Result<void> dispatch_message(const protocol::Message& message, IClock::time_point now);
    Result<void> apply_local_actions(const std::vector<RouterAction>& actions);
    Result<void> fail_and_disconnect(const char* error, IClock::time_point now);

    Connection connection_;
    PeerSessionConfig config_;
    ConnectionLiveness liveness_;
    std::optional<RouterAction> pending_acquire_;
    std::uint64_t heartbeat_counter_{0};
    IClock::time_point next_heartbeat_{};
    bool started_{false};
    bool hello_received_{false};
    bool disconnected_{false};
};

} // namespace lanlink::transport
