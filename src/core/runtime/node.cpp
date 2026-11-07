#include "lanlink/runtime/node.hpp"

#include "lanlink/common/config.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace lanlink::runtime {

InputNodeRuntime::InputNodeRuntime(InputRuntimeConfig config)
    : config_(std::move(config)),
      router_(config_.source, config_.peer, config_.local_display, {}, config_.queue_capacity) {}

InputNodeRuntime::~InputNodeRuntime() { (void)stop(TimePoint{}); }

Result<std::unique_ptr<InputNodeRuntime>> InputNodeRuntime::create(InputRuntimeConfig config) {
  if (config.local_device == DeviceId{} || config.source == SourceId{} ||
      config.peer.id == DeviceId{} || config.local_display == DisplayId{}) {
    return Result<std::unique_ptr<InputNodeRuntime>>::failure("runtime identifiers must be non-zero");
  }
  if (config.backend == nullptr) {
    return Result<std::unique_ptr<InputNodeRuntime>>::failure("input backend is required");
  }
  if (config.queue_capacity == 0) {
    return Result<std::unique_ptr<InputNodeRuntime>>::failure("queue capacity must be positive");
  }
  return Result<std::unique_ptr<InputNodeRuntime>>::success(
      std::unique_ptr<InputNodeRuntime>(new InputNodeRuntime(std::move(config))));
}

Result<void> InputNodeRuntime::set_topology(ScreenTopology topology) {
  if (running_) return Result<void>::failure("cannot change topology while running");
  topology_ = std::move(topology);
  return Result<void>::success();
}

transport::PeerSessionConfig InputNodeRuntime::session_config() noexcept {
  return transport::PeerSessionConfig{config_.local_device, config_.peer, &router_, &arbiter_,
                                      config_.backend};
}

Result<void> InputNodeRuntime::start() {
  if (running_) return Result<void>::success();
  {
    std::lock_guard lock(queue_mutex_);
    queued_events_.clear();
    queue_overflowed_ = false;
  }
  auto listener = transport::Listener::bind(config_.listen_endpoint, config_.connection);
  if (!listener.ok()) return Result<void>::failure(listener.error());
  listener_ = std::make_unique<transport::Listener>(std::move(listener.value()));

  auto started = config_.backend->start([this](const InputEvent& event) {
    (void)queue_event(event);
  });
  if (!started.ok()) {
    listener_->close();
    listener_.reset();
    return started;
  }
  running_ = true;
  return Result<void>::success();
}

Result<transport::PeerHandle> InputNodeRuntime::connect_peer(const transport::Endpoint& endpoint) {
  if (!running_) return Result<transport::PeerHandle>::failure("runtime is not started");
  transport::OutgoingPeer peer;
  peer.endpoint = endpoint;
  peer.connection = config_.connection;
  peer.session = session_config();
  auto handle = manager_.add_outgoing(std::move(peer));
  if (!handle.ok()) return handle;
  peer_handles_.push_back(handle.value());
  return handle;
}

Result<void> InputNodeRuntime::queue_event(const InputEvent& event) {
  std::lock_guard lock(queue_mutex_);
  if (queued_events_.size() >= config_.queue_capacity) {
    queued_events_.clear();
    queue_overflowed_ = true;
    return Result<void>::failure("input event queue overflowed");
  }
  queued_events_.push_back(QueuedEvent{event, config_.backend->pointer_position()});
  return Result<void>::success();
}

Result<void> InputNodeRuntime::dispatch_actions(const std::vector<RouterAction>& actions,
                                                TimePoint now) {
  if (actions.empty()) return Result<void>::success();
  transport::PeerHandle connected_handle{};
  bool has_connected = false;
  for (const auto handle : peer_handles_) {
    const auto state = manager_.state(handle);
    if (state.has_value() && *state == transport::PeerState::kConnected) {
      connected_handle = handle;
      has_connected = true;
      break;
    }
  }

  if (has_connected) {
    return manager_.send_actions(connected_handle, actions, now);
  }

  // Cleanup actions must still restore local input if the transport is already gone.
  for (const auto& action : actions) {
    if (action.kind == RouterActionKind::kReleaseInjectedState) {
      config_.backend->release_injected_state(action.session);
    } else if (action.kind == RouterActionKind::kRestoreLocal) {
      auto recovered = config_.backend->recover();
      if (!recovered.ok()) return recovered;
    } else if (action.kind == RouterActionKind::kArmLocalSuppression ||
               action.kind == RouterActionKind::kActivate ||
               action.kind == RouterActionKind::kRenew ||
               action.kind == RouterActionKind::kSendInput ||
               action.kind == RouterActionKind::kAcquire) {
      return Result<void>::failure("peer is not connected");
    }
  }
  return Result<void>::success();
}

std::optional<InputNodeRuntime::CrossingDetection> InputNodeRuntime::detect_crossing(
    const Point& pointer) const {
  const auto display_it = std::find_if(
      topology_.displays().begin(), topology_.displays().end(),
      [this](const Display& display) { return display.id == config_.local_display; });
  if (display_it == topology_.displays().end()) return std::nullopt;
  const auto& bounds = display_it->logical_bounds;

  Edge edge = Edge::kRight;
  double overflow = -1.0;
  const auto consider = [&](Edge candidate, double value) {
    if (value > overflow) {
      edge = candidate;
      overflow = value;
    }
  };
  if (pointer.x < bounds.left) consider(Edge::kLeft, bounds.left - pointer.x);
  if (pointer.x > bounds.right) consider(Edge::kRight, pointer.x - bounds.right);
  if (pointer.y < bounds.top) consider(Edge::kUp, bounds.top - pointer.y);
  if (pointer.y > bounds.bottom) consider(Edge::kDown, pointer.y - bounds.bottom);
  if (overflow < 0.0) return std::nullopt;
  auto crossing = topology_.crossing(config_.local_display, edge, pointer, overflow);
  if (!crossing.has_value()) return std::nullopt;

  // Topology coordinates describe the displays in one logical layout. The
  // target process, however, injects into its own local desktop, whose origin
  // is the target display's top-left corner. Convert the handoff entry before
  // it is sent over the wire so a right-side target receives x ~= 0 instead of
  // the layout coordinate x ~= source_width.
  const auto target_it = std::find_if(
      topology_.displays().begin(), topology_.displays().end(),
      [&crossing](const Display& display) { return display.id == crossing->target; });
  if (target_it == topology_.displays().end()) return std::nullopt;
  crossing->entry_point.x -= target_it->logical_bounds.left;
  crossing->entry_point.y -= target_it->logical_bounds.top;
  crossing->entry = crossing->entry_point;

  Point source_boundary = pointer;
  switch (edge) {
    case Edge::kLeft:
      source_boundary.x = bounds.left;
      source_boundary.y = std::clamp(pointer.y, bounds.top, bounds.bottom);
      break;
    case Edge::kRight:
      source_boundary.x = bounds.right;
      source_boundary.y = std::clamp(pointer.y, bounds.top, bounds.bottom);
      break;
    case Edge::kUp:
      source_boundary.x = std::clamp(pointer.x, bounds.left, bounds.right);
      source_boundary.y = bounds.top;
      break;
    case Edge::kDown:
      source_boundary.x = std::clamp(pointer.x, bounds.left, bounds.right);
      source_boundary.y = bounds.bottom;
      break;
  }
  return CrossingDetection{std::move(*crossing), source_boundary};
}

Result<void> InputNodeRuntime::drain_events(TimePoint now) {
  bool overflowed = false;
  std::deque<QueuedEvent> events;
  {
    std::lock_guard lock(queue_mutex_);
    overflowed = queue_overflowed_;
    queue_overflowed_ = false;
    events.swap(queued_events_);
  }
  if (overflowed) {
    auto cleanup = router_.on_disconnect(now);
    auto dispatched = dispatch_actions(cleanup, now);
    if (!dispatched.ok()) return dispatched;
    return Result<void>::failure("input event queue overflowed");
  }

  for (const auto& queued : events) {
    const auto& event = queued.event;
    const Point pointer = queued.pointer.value_or(Point{});
    std::vector<RouterAction> actions;
    const auto recover_after_handoff_failure = [&] {
      const auto cleanup = router_.on_disconnect(now);
      (void)dispatch_actions(cleanup, now);
    };

    if (event.origin == InputOrigin::kPhysical && arbiter_.has_active_lease()) {
      const auto inbound = *arbiter_.lease();
      arbiter_.on_local_input(event, now);
      if (!arbiter_.has_lease()) {
        config_.backend->release_injected_state(inbound.session);
        RouterAction revoke;
        revoke.kind = RouterActionKind::kRevoke;
        revoke.session = inbound.session;
        revoke.generation = inbound.generation;
        revoke.reason = RevokeReason::kLocalPreempt;
        auto notified = dispatch_actions({revoke}, now);
        if (!notified.ok()) return notified;
      }
    }

    if (queued.pointer.has_value() && std::holds_alternative<MouseDelta>(event.payload) &&
        (router_.state() == RouterState::kLocal || router_.state() == RouterState::kHandoff)) {
      if (const auto crossing = detect_crossing(pointer)) {
        // Put the source cursor back on its own edge before the asynchronous
        // acquire/activate handshake. This prevents the native desktop from
        // continuing past the edge while the target cursor is entering.
        auto warped = config_.backend->warp_pointer(crossing->source_boundary);
        if (!warped.ok()) {
          recover_after_handoff_failure();
          return warped;
        }
        actions = router_.on_crossing(crossing->crossing, now);
      }
    }
    if (actions.empty()) {
      if (router_.state() == RouterState::kRemote &&
          event.origin == InputOrigin::kPhysical && config_.backend->status().suppression) {
        actions = router_.on_suppressed_event(event, pointer, now);
      } else {
        actions = router_.on_local_event(event, pointer, now);
      }
    }

    // Arm suppression only once an acquire is actually being sent. A held
    // mouse button keeps the router in handoff until its release reaches the
    // local desktop; suppressing that release would leave the button stuck.
    const auto acquire_requested = std::any_of(
        actions.begin(), actions.end(), [](const RouterAction& action) {
          return action.kind == RouterActionKind::kAcquire;
        });
    if (acquire_requested && !config_.backend->status().suppression) {
      auto suppressed = config_.backend->set_suppression(true);
      if (!suppressed.ok()) {
        recover_after_handoff_failure();
        return suppressed;
      }
    }

    auto dispatched = dispatch_actions(actions, now);
    if (!dispatched.ok()) {
      // The source suppression was enabled before the control-plane request
      // was sent. If transport dispatch fails, roll the router and backend
      // back to local input immediately instead of leaving the cursor locked.
      if (config_.backend->status().suppression) {
        recover_after_handoff_failure();
      }
      return dispatched;
    }
  }
  return Result<void>::success();
}

Result<void> InputNodeRuntime::accept_incoming(TimePoint now) {
  if (!listener_) return Result<void>::success();
  while (true) {
    auto accepted = listener_->accept(std::chrono::milliseconds{0});
    if (!accepted.ok()) {
      if (accepted.error() == "read timeout") return Result<void>::success();
      return Result<void>::failure(accepted.error());
    }
    auto handle = manager_.add_incoming(std::move(accepted.value()), session_config(), now);
    if (!handle.ok()) return Result<void>::failure(handle.error());
    peer_handles_.push_back(handle.value());
  }
}

Result<void> InputNodeRuntime::poll(TimePoint now, std::chrono::milliseconds timeout) {
  if (!running_) return Result<void>::failure("runtime is not started");
  auto accepted = accept_incoming(now);
  auto drained = drain_events(now);
  auto driven = manager_.poll(now, timeout);
  if (!accepted.ok()) return accepted;
  if (!drained.ok()) return drained;
  return driven;
}

Result<void> InputNodeRuntime::tick(TimePoint now) {
  if (!running_) return Result<void>::failure("runtime is not started");
  auto actions = router_.on_tick(now);
  auto dispatched = dispatch_actions(actions, now);
  if (!dispatched.ok()) return dispatched;
  return manager_.tick(now);
}

Result<void> InputNodeRuntime::stop(TimePoint now) {
  if (!running_) {
    std::lock_guard lock(queue_mutex_);
    queued_events_.clear();
    queue_overflowed_ = false;
    return Result<void>::success();
  }
  Result<void> result = Result<void>::success();
  for (const auto handle : peer_handles_) {
    auto removed = manager_.remove(handle, now);
    if (result.ok() && !removed.ok()) result = removed;
  }
  peer_handles_.clear();
  if (listener_) {
    auto closed = listener_->close();
    if (result.ok() && !closed.ok()) result = closed;
    listener_.reset();
  }
  config_.backend->stop();
  running_ = false;
  {
    std::lock_guard lock(queue_mutex_);
    queued_events_.clear();
    queue_overflowed_ = false;
  }
  return result;
}

bool InputNodeRuntime::connected() const noexcept {
  for (const auto handle : peer_handles_) {
    const auto state = manager_.state(handle);
    if (state.has_value() && *state == transport::PeerState::kConnected) return true;
  }
  return false;
}

transport::Endpoint InputNodeRuntime::local_endpoint() const {
  return listener_ ? listener_->local_endpoint() : config_.listen_endpoint;
}

}  // namespace lanlink::runtime
