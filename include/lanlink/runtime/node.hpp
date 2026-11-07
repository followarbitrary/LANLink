#pragma once

#include "lanlink/common/clock.hpp"
#include "lanlink/common/result.hpp"
#include "lanlink/input/arbiter.hpp"
#include "lanlink/input/router.hpp"
#include "lanlink/platform/input_backend.hpp"
#include "lanlink/topology/topology.hpp"
#include "lanlink/transport/manager.hpp"

#include <chrono>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace lanlink::runtime {

struct InputRuntimeConfig {
  DeviceId local_device;
  SourceId source;
  AuthenticatedPeer peer;
  DisplayId local_display;
  transport::Endpoint listen_endpoint;
  transport::ConnectionOptions connection;
  platform::IInputBackend* backend{nullptr};
  std::size_t queue_capacity{256};
};

class InputNodeRuntime {
 public:
  using TimePoint = IClock::time_point;

  static Result<std::unique_ptr<InputNodeRuntime>> create(InputRuntimeConfig config);

  ~InputNodeRuntime();

  Result<void> set_topology(ScreenTopology topology);
  Result<void> start();
  Result<transport::PeerHandle> connect_peer(const transport::Endpoint& endpoint);
  Result<void> poll(TimePoint now,
                    std::chrono::milliseconds timeout = std::chrono::milliseconds{0});
  Result<void> tick(TimePoint now);
  Result<void> stop(TimePoint now);

  bool running() const noexcept { return running_; }
  bool connected() const noexcept;
  transport::Endpoint local_endpoint() const;
  const InputRouter& router() const noexcept { return router_; }
  const InputArbiter& arbiter() const noexcept { return arbiter_; }
  const ScreenTopology& topology() const noexcept { return topology_; }

 private:
  struct CrossingDetection {
    Crossing crossing;
    Point source_boundary;
  };

  struct ReturnDetection {
    Edge edge;
    Point source_boundary;
  };

  struct QueuedEvent {
    InputEvent event;
    std::optional<Point> pointer;
  };

  explicit InputNodeRuntime(InputRuntimeConfig config);

  transport::PeerSessionConfig session_config() noexcept;
  Result<void> accept_incoming(TimePoint now);
  Result<void> drain_events(TimePoint now);
  Result<void> dispatch_actions(const std::vector<RouterAction>& actions, TimePoint now);
  std::optional<CrossingDetection> detect_crossing(const InputEvent& event,
                                                   const Point& pointer);
  std::optional<ReturnDetection> detect_return_to_local(const InputEvent& event,
                                                        const Point& pointer) const;
  Result<void> queue_event(const InputEvent& event);

  InputRuntimeConfig config_;
  InputRouter router_;
  InputArbiter arbiter_;
  ScreenTopology topology_;
  std::unique_ptr<transport::Listener> listener_;
  transport::ConnectionManager manager_;
  std::vector<transport::PeerHandle> peer_handles_;
  std::deque<QueuedEvent> queued_events_;
  mutable std::mutex queue_mutex_;
  bool queue_overflowed_{false};
  std::optional<Edge> edge_overflow_edge_;
  double edge_overflow_{0.0};
  bool warned_remote_without_suppression_{false};
  bool running_{false};
};

}  // namespace lanlink::runtime
