#include "lanlink/runtime/node.hpp"

#include "lanlink/common/config.hpp"
#include "lanlink/common/debug.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace lanlink::runtime {

namespace {

Result<ScreenTopology> normalize_topology_for_local(const InputRuntimeConfig& config,
                                                     ScreenTopology topology) {
  auto displays = topology.displays();
  auto links = topology.links();
  auto local_it = std::find_if(
      displays.begin(), displays.end(), [&config](const Display& display) {
        return display.device == config.local_device && display.id == config.local_display;
      });
  if (local_it == displays.end()) {
    // Display IDs are unique within a validated snapshot, so an older local
    // snapshot that omitted the device metadata can still be normalized by
    // the configured display ID. A missing display remains an actionable
    // error and is never installed.
    local_it = std::find_if(
        displays.begin(), displays.end(), [&config](const Display& display) {
          return display.id == config.local_display;
        });
    if (local_it == displays.end()) {
      return Result<ScreenTopology>::failure(
          "topology does not contain the configured local display");
    }
  }

  const auto native_bounds = config.backend->display_bounds();
  if (native_bounds.has_value()) {
    const double dx = native_bounds->left - local_it->logical_bounds.left;
    const double dy = native_bounds->top - local_it->logical_bounds.top;
    for (auto& display : displays) {
      display.logical_bounds.left += dx;
      display.logical_bounds.top += dy;
      display.logical_bounds.right += dx;
      display.logical_bounds.bottom += dy;
    }
    const auto normalized_local_it = std::find_if(
        displays.begin(), displays.end(), [&config](const Display& display) {
          return display.id == config.local_display;
        });
    normalized_local_it->logical_bounds = *native_bounds;
  }

  for (auto& link : links) {
    if (link.target != config.local_display) continue;
    std::swap(link.source, link.target);
    std::swap(link.source_edge, link.target_edge);
  }

  ScreenTopology normalized;
  const auto set = normalized.set_snapshot(topology.version(), std::move(displays),
                                           std::move(links));
  if (!set.ok()) return Result<ScreenTopology>::failure(set.error());
  return Result<ScreenTopology>::success(std::move(normalized));
}

}  // namespace

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
  if (config.clipboard_sync_enabled && config.clipboard_backend == nullptr) {
    return Result<std::unique_ptr<InputNodeRuntime>>::failure(
        "clipboard backend is required when clipboard sync is enabled");
  }
  if (config.queue_capacity == 0) {
    return Result<std::unique_ptr<InputNodeRuntime>>::failure("queue capacity must be positive");
  }
  return Result<std::unique_ptr<InputNodeRuntime>>::success(
      std::unique_ptr<InputNodeRuntime>(new InputNodeRuntime(std::move(config))));
}

Result<void> InputNodeRuntime::set_topology(ScreenTopology topology) {
  auto normalized = normalize_topology_for_local(config_, std::move(topology));
  if (!normalized.ok()) return Result<void>::failure(normalized.error());
  topology = std::move(normalized.value());
  if (running_ && manager_.size() != 0) {
    const auto now = IClock::Clock::now();
    Result<void> cleanup = Result<void>::success();

    if (arbiter_.has_lease()) {
      const auto lease = *arbiter_.lease();
      arbiter_.revoke_current(RevokeReason::kExplicitRelease, now);
      config_.backend->release_injected_state(lease.session);
      RouterAction revoke;
      revoke.kind = RouterActionKind::kRevoke;
      revoke.source = config_.source;
      revoke.session = lease.session;
      revoke.generation = lease.generation;
      revoke.reason = RevokeReason::kExplicitRelease;
      auto notified = dispatch_actions({revoke}, now);
      if (!notified.ok()) cleanup = notified;
    }

    clear_remote_cursor();
    edge_overflow_edge_.reset();
    edge_overflow_ = 0.0;
    {
      std::lock_guard lock(queue_mutex_);
      queued_events_.clear();
      queue_overflowed_ = false;
    }

    auto actions = router_.on_revoke(RevokeReason::kExplicitRelease, now);
    auto dispatched = dispatch_actions(actions, now);
    if (cleanup.ok() && !dispatched.ok()) cleanup = dispatched;
    (void)router_.on_tick(now);
    topology_ = std::move(topology);
    return cleanup;
  }
  topology_ = std::move(topology);
  return Result<void>::success();
}

Result<void> InputNodeRuntime::set_accept_incoming(bool accept) {
  config_.accept_incoming = accept;
  return Result<void>::success();
}

Result<void> InputNodeRuntime::set_input_forwarding_enabled(bool enabled, TimePoint now) {
  if (input_forwarding_enabled_ == enabled) return Result<void>::success();
  input_forwarding_enabled_ = enabled;
  if (enabled || !running_) return Result<void>::success();

  if (arbiter_.has_lease()) {
    const auto lease = *arbiter_.lease();
    arbiter_.revoke_current(RevokeReason::kExplicitRelease, now);
    config_.backend->release_injected_state(lease.session);
    RouterAction revoke;
    revoke.kind = RouterActionKind::kRevoke;
    revoke.source = config_.source;
    revoke.session = lease.session;
    revoke.generation = lease.generation;
    revoke.reason = RevokeReason::kExplicitRelease;
    auto notified = dispatch_actions({revoke}, now);
    if (!notified.ok()) return notified;
  }

  clear_remote_cursor();
  edge_overflow_edge_.reset();
  edge_overflow_ = 0.0;
  {
    std::lock_guard lock(queue_mutex_);
    queued_events_.clear();
    queue_overflowed_ = false;
  }
  auto cleanup = router_.on_disconnect(now);
  return dispatch_actions(cleanup, now);
}

Result<void> InputNodeRuntime::set_clipboard_sync_enabled(bool enabled, TimePoint) {
  if (enabled && config_.clipboard_backend == nullptr) {
    return Result<void>::failure("clipboard backend is required when clipboard sync is enabled");
  }
  if (config_.clipboard_sync_enabled == enabled) return Result<void>::success();
  config_.clipboard_sync_enabled = enabled;
  manager_.discard_clipboard_state();
  {
    std::lock_guard lock(queue_mutex_);
    queued_clipboard_changes_.clear();
    clipboard_fingerprint_.reset();
  }
  if (config_.clipboard_backend != nullptr) {
    auto discarded = config_.clipboard_backend->discard_pending_notifications();
    if (!discarded.ok()) debug::warn("clipboard baseline drain failed error=", discarded.error());
  }
  if (enabled && running_) {
    auto started = start_clipboard_backend();
    if (!started.ok()) return started;
  }
  return Result<void>::success();
}

transport::PeerSessionConfig InputNodeRuntime::session_config() {
  return transport::PeerSessionConfig{config_.local_device, config_.peer, &router_, &arbiter_,
                                      config_.backend, config_.local_display,
                                      [this](SessionId session, std::uint64_t generation,
                                             Point position) {
                                        update_remote_cursor_position(session, generation, position);
                                      },
                                      [this] { return input_forwarding_enabled_; },
                                      config_.clipboard_backend,
                                      [this] { return config_.clipboard_sync_enabled; },
                                      config_.receive_directory,
                                      config_.file_status_callback};
}

Result<void> InputNodeRuntime::start() {
  if (running_) return Result<void>::success();
  {
    std::lock_guard lock(queue_mutex_);
    queued_events_.clear();
    queued_clipboard_changes_.clear();
    clipboard_fingerprint_.reset();
    queue_overflowed_ = false;
  }
  edge_overflow_edge_.reset();
  edge_overflow_ = 0.0;
  remote_cursor_.reset();
  require_peer_display_info_ = config_.backend->display_bounds().has_value();
  peer_display_wait_started_.reset();
  warned_peer_display_fallback_ = false;
  local_display_scale_ = config_.backend->display_scale();
  if (!std::isfinite(local_display_scale_) || local_display_scale_ <= 0.0)
    local_display_scale_ = 1.0;
  warned_remote_without_suppression_ = false;
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
  if (config_.clipboard_backend != nullptr) {
    auto clipboard_started = start_clipboard_backend();
    if (!clipboard_started.ok())
      debug::warn("clipboard backend start failed error=", clipboard_started.error());
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
  outgoing_peer_handle_ = handle.value();
  return handle;
}

Result<void> InputNodeRuntime::update_peer_endpoint(const transport::Endpoint& endpoint) {
  if (!outgoing_peer_handle_.has_value())
    return Result<void>::failure("outgoing peer is not configured");
  return manager_.update_outgoing_endpoint(*outgoing_peer_handle_, endpoint);
}

Result<Uuid128> InputNodeRuntime::send_file(const std::filesystem::path& path,
                                            std::string display_name) {
  if (!running_) return Result<Uuid128>::failure("runtime is not started");
  for (const auto handle : peer_handles_) {
    if (manager_.ready(handle))
      return manager_.send_file(handle, path, std::move(display_name), IClock::Clock::now());
  }
  return Result<Uuid128>::failure("peer is not connected");
}

Result<void> InputNodeRuntime::cancel_file(const Uuid128& transfer) {
  if (!running_) return Result<void>::failure("runtime is not started");
  for (const auto handle : peer_handles_) {
    if (manager_.ready(handle)) {
      auto result = manager_.cancel_file(handle, transfer, IClock::Clock::now());
      if (result.ok()) return result;
    }
  }
  return Result<void>::failure("file transfer is not active");
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

void InputNodeRuntime::queue_clipboard_change(platform::ClipboardData data) {
  std::lock_guard lock(queue_mutex_);
  const auto capacity = std::min<std::size_t>(config_.queue_capacity,
                                               platform::kMaxClipboardNotificationQueue);
  if (queued_clipboard_changes_.size() >= capacity) {
    queued_clipboard_changes_.pop_front();
    debug::warn("clipboard notification queue full; dropping oldest change");
  }
  queued_clipboard_changes_.push_back(std::move(data));
}

Result<void> InputNodeRuntime::start_clipboard_backend() {
  if (config_.clipboard_backend == nullptr || clipboard_backend_started_)
    return Result<void>::success();
  auto started = config_.clipboard_backend->start(
      [this](platform::ClipboardData data) { queue_clipboard_change(std::move(data)); });
  if (!started.ok()) {
    clipboard_backend_started_ = false;
    config_.clipboard_backend->stop();
    return started;
  }
  clipboard_backend_started_ = true;
  return Result<void>::success();
}

Result<void> InputNodeRuntime::dispatch_actions(const std::vector<RouterAction>& actions,
                                                TimePoint now) {
  if (actions.empty()) return Result<void>::success();
  transport::PeerHandle connected_handle{};
  bool has_connected = false;
  for (const auto handle : peer_handles_) {
    const auto state = manager_.state(handle);
    if (state.has_value() && *state == transport::PeerState::kConnected &&
        manager_.ready(handle)) {
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
      debug::log("suppression off/recover (transport unavailable)");
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
    const InputEvent& event, const Point& pointer, TimePoint now) {
  if (!connected()) return std::nullopt;
  // Native backends advertise their own bounds during the handshake. Do not
  // initialize the virtual remote cursor from the synthetic topology size
  // until the peer has sent its real logical bounds; otherwise the first
  // handoff on a scaled display would use a permanently wrong return edge.
  if (require_peer_display_info_ && !router_.peer_display_bounds()) {
    if (!peer_display_wait_started_.has_value()) peer_display_wait_started_ = now;
    if (now - *peer_display_wait_started_ < m0::kDisplayInfoWaitTimeout) {
      debug::log("edge crossing deferred until peer display info arrives");
      return std::nullopt;
    }
    if (!warned_peer_display_fallback_) {
      debug::warn("peer display info unavailable; falling back to topology bounds");
      warned_peer_display_fallback_ = true;
    }
  } else {
    peer_display_wait_started_.reset();
    warned_peer_display_fallback_ = false;
  }
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

  const auto* delta = std::get_if<MouseDelta>(&event.payload);
  if (overflow < 0.0 && delta != nullptr) {
    // Native desktops clamp the cursor at the monitor edge. In that case the
    // pointer coordinate never becomes outside the rectangle, so accumulate
    // the outward deltas until the same threshold used by topology crossing
    // is reached.
    constexpr double kBoundaryTolerance = 1.0;
    std::optional<Edge> contact_edge;
    double contact_motion = 0.0;
    const auto consider_contact = [&](Edge candidate, double motion) {
      if (motion > contact_motion) {
        contact_edge = candidate;
        contact_motion = motion;
      }
    };
    if (pointer.x >= bounds.right - kBoundaryTolerance && delta->dx > 0.0)
      consider_contact(Edge::kRight, delta->dx);
    if (pointer.x <= bounds.left + kBoundaryTolerance && delta->dx < 0.0)
      consider_contact(Edge::kLeft, -delta->dx);
    if (pointer.y >= bounds.bottom - kBoundaryTolerance && delta->dy > 0.0)
      consider_contact(Edge::kDown, delta->dy);
    if (pointer.y <= bounds.top + kBoundaryTolerance && delta->dy < 0.0)
      consider_contact(Edge::kUp, -delta->dy);

    if (contact_edge.has_value()) {
      if (edge_overflow_edge_ == contact_edge) {
        edge_overflow_ += contact_motion;
      } else {
        edge_overflow_edge_ = contact_edge;
        edge_overflow_ = contact_motion;
      }
      edge = *contact_edge;
      overflow = edge_overflow_;
    } else {
      edge_overflow_edge_.reset();
      edge_overflow_ = 0.0;
    }
  } else if (overflow >= 0.0) {
    edge_overflow_edge_ = edge;
    edge_overflow_ = overflow;
  } else {
    edge_overflow_edge_.reset();
    edge_overflow_ = 0.0;
  }
  if (overflow < 0.0) return std::nullopt;

  Point candidate_point = pointer;
  // ScreenTopology uses a half-open rectangle. Keep a clamped edge tangent
  // when a native API reports the exact bottom/right coordinate.
  candidate_point.x = std::clamp(candidate_point.x, bounds.left,
                                 std::nextafter(bounds.right, bounds.left));
  candidate_point.y = std::clamp(candidate_point.y, bounds.top,
                                 std::nextafter(bounds.bottom, bounds.top));
  auto crossing = topology_.crossing(config_.local_display, edge, candidate_point, overflow);
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
  edge_overflow_edge_.reset();
  edge_overflow_ = 0.0;
  return CrossingDetection{std::move(*crossing), source_boundary, edge};
}

void InputNodeRuntime::begin_remote_cursor(const Crossing& crossing, Edge source_edge) {
  Rect bounds{0.0, 0.0, 1.0, 1.0};
  Rect source_bounds{0.0, 0.0, 1.0, 1.0};
  Point position = crossing.entry_point;
  const auto source_it = std::find_if(
      topology_.displays().begin(), topology_.displays().end(),
      [this](const Display& display) { return display.id == config_.local_display; });
  if (source_it != topology_.displays().end()) source_bounds = source_it->logical_bounds;
  const auto target_it = std::find_if(
      topology_.displays().begin(), topology_.displays().end(),
      [&crossing](const Display& display) { return display.id == crossing.target; });
  if (target_it != topology_.displays().end()) {
    bounds = Rect{0.0, 0.0, target_it->logical_bounds.width(),
                  target_it->logical_bounds.height()};
  }

  if (router_.peer_display_bounds().has_value() &&
      (router_.peer_display() == crossing.target || router_.peer_display() == DisplayId{})) {
    bounds = *router_.peer_display_bounds();
    if (crossing.has_tangent_ratio)
      position = edge_entry_point(bounds, crossing.target_edge, crossing.tangent_ratio);
  } else {
    position.x = std::clamp(position.x, bounds.left, std::nextafter(bounds.right, bounds.left));
    position.y = std::clamp(position.y, bounds.top, std::nextafter(bounds.bottom, bounds.top));
  }
  double target_scale = router_.peer_display_scale();
  if (!router_.peer_display_bounds() && target_it != topology_.displays().end())
    target_scale = target_it->scale;
  double movement_scale = local_display_scale_ / target_scale;
  if (!std::isfinite(movement_scale) || movement_scale <= 0.0) movement_scale = 1.0;
  remote_cursor_ = RemoteCursorState{position, bounds, source_bounds, movement_scale,
                                     source_edge, crossing.target_edge};
  debug::log("remote cursor initialized position=", position.x, ",", position.y,
             " bounds=", bounds.left, ",", bounds.top, " ", bounds.right, "x", bounds.bottom,
             " source_bounds=", source_bounds.left, ",", source_bounds.top, " ",
             source_bounds.right, "x", source_bounds.bottom,
             " movement_scale=", movement_scale,
             " source_edge=", static_cast<unsigned>(source_edge),
             " target_edge=", static_cast<unsigned>(crossing.target_edge));
}

void InputNodeRuntime::update_remote_cursor_position(SessionId session,
                                                      std::uint64_t generation,
                                                      Point position) noexcept {
  if (!remote_cursor_.has_value() || router_.state() != RouterState::kRemote ||
      !router_.lease().has_value() || router_.lease()->session != session ||
      router_.lease()->generation != generation || !std::isfinite(position.x) ||
      !std::isfinite(position.y))
    return;
  auto& cursor = *remote_cursor_;
  cursor.position.x = std::clamp(position.x, cursor.bounds.left,
                                 std::nextafter(cursor.bounds.right, cursor.bounds.left));
  cursor.position.y = std::clamp(position.y, cursor.bounds.top,
                                 std::nextafter(cursor.bounds.bottom, cursor.bounds.top));
  debug::log("remote cursor position corrected session=", session.low,
             " generation=", generation, " point=", cursor.position.x, ",", cursor.position.y);
}

std::optional<Edge> InputNodeRuntime::detect_remote_exit(const InputEvent& event) {
  if (!remote_cursor_.has_value() || event.origin != InputOrigin::kPhysical) return std::nullopt;
  const auto* delta = std::get_if<MouseDelta>(&event.payload);
  if (delta == nullptr || delta->zero()) return std::nullopt;

  auto& cursor = *remote_cursor_;
  if (const auto peer_bounds = router_.peer_display_bounds(); peer_bounds.has_value()) {
    const auto old_bounds = cursor.bounds;
    if (old_bounds.left != peer_bounds->left || old_bounds.top != peer_bounds->top ||
        old_bounds.right != peer_bounds->right || old_bounds.bottom != peer_bounds->bottom) {
      const double x_ratio = old_bounds.width() > 0.0
                                 ? std::clamp((cursor.position.x - old_bounds.left) /
                                                  old_bounds.width(),
                                              0.0, 1.0)
                                 : 0.5;
      const double y_ratio = old_bounds.height() > 0.0
                                 ? std::clamp((cursor.position.y - old_bounds.top) /
                                                  old_bounds.height(),
                                              0.0, 1.0)
                                 : 0.5;
      cursor.bounds = *peer_bounds;
      cursor.position.x = cursor.bounds.left + x_ratio * cursor.bounds.width();
      cursor.position.y = cursor.bounds.top + y_ratio * cursor.bounds.height();
      debug::log("remote cursor bounds refreshed bounds=", cursor.bounds.left, ",",
                 cursor.bounds.top, " ", cursor.bounds.right, "x", cursor.bounds.bottom);
    }
    double target_scale = router_.peer_display_scale();
    if (!std::isfinite(target_scale) || target_scale <= 0.0) target_scale = 1.0;
    cursor.movement_scale = local_display_scale_ / target_scale;
    if (!std::isfinite(cursor.movement_scale) || cursor.movement_scale <= 0.0)
      cursor.movement_scale = 1.0;
  }
  const Point next{cursor.position.x + delta->dx * cursor.movement_scale,
                   cursor.position.y + delta->dy * cursor.movement_scale};
  bool exits = false;
  switch (cursor.target_edge) {
    case Edge::kLeft:
      exits = delta->dx < 0.0 && next.x < cursor.bounds.left;
      break;
    case Edge::kRight:
      exits = delta->dx > 0.0 && next.x >= cursor.bounds.right;
      break;
    case Edge::kUp:
      exits = delta->dy < 0.0 && next.y < cursor.bounds.top;
      break;
    case Edge::kDown:
      exits = delta->dy > 0.0 && next.y >= cursor.bounds.bottom;
      break;
  }
  cursor.position.x = std::clamp(next.x, cursor.bounds.left,
                                 std::nextafter(cursor.bounds.right, cursor.bounds.left));
  cursor.position.y = std::clamp(next.y, cursor.bounds.top,
                                 std::nextafter(cursor.bounds.bottom, cursor.bounds.top));
  if (exits) return cursor.target_edge;
  return std::nullopt;
}

Result<void> InputNodeRuntime::drain_events(TimePoint now) {
  if (!input_forwarding_enabled_) {
    std::lock_guard lock(queue_mutex_);
    queued_events_.clear();
    queue_overflowed_ = false;
    return Result<void>::success();
  }
  if (!connected()) {
    peer_display_wait_started_.reset();
    warned_peer_display_fallback_ = false;
  }
  bool overflowed = false;
  std::deque<QueuedEvent> events;
  {
    std::lock_guard lock(queue_mutex_);
    overflowed = queue_overflowed_;
    queue_overflowed_ = false;
    events.swap(queued_events_);
  }
  if (overflowed) {
    clear_remote_cursor();
    auto cleanup = router_.on_disconnect(now);
    auto dispatched = dispatch_actions(cleanup, now);
    if (!dispatched.ok()) return dispatched;
    return Result<void>::failure("input event queue overflowed");
  }

  for (const auto& queued : events) {
    const auto& event = queued.event;
    const Point pointer = queued.pointer.value_or(Point{});
    std::vector<RouterAction> actions;
    bool local_preempted_remote_lease = false;
    const auto recover_after_handoff_failure = [&] {
      clear_remote_cursor();
      const auto cleanup = router_.on_disconnect(now);
      (void)dispatch_actions(cleanup, now);
      // Make the router usable for the remaining events already drained from
      // this batch. The next outer tick would normally perform this transition,
      // but returning here would silently discard those events first.
      (void)router_.on_tick(now);
    };

    if (event.origin == InputOrigin::kPhysical && arbiter_.has_active_lease()) {
      const auto inbound = *arbiter_.lease();
      arbiter_.on_local_input(event, now);
      if (!arbiter_.has_lease()) {
        debug::log("lease revoked by local input session=", inbound.session.low,
                   " generation=", inbound.generation);
        config_.backend->release_injected_state(inbound.session);
        RouterAction revoke;
        revoke.kind = RouterActionKind::kRevoke;
        revoke.session = inbound.session;
        revoke.generation = inbound.generation;
        revoke.reason = RevokeReason::kLocalPreempt;
        if (const auto position = config_.backend->pointer_position(); position.has_value() &&
            std::isfinite(position->x) && std::isfinite(position->y)) {
          revoke.has_pointer_position = true;
          revoke.pointer_position = *position;
        }
        auto notified = dispatch_actions({revoke}, now);
        if (!notified.ok()) return notified;
        local_preempted_remote_lease = true;
      }
    }

    if (std::holds_alternative<MouseDelta>(event.payload) &&
        router_.state() == RouterState::kRemote) {
      if (const auto returning = detect_remote_exit(event)) {
        const auto display_it = std::find_if(
            topology_.displays().begin(), topology_.displays().end(),
            [this](const Display& display) { return display.id == config_.local_display; });
        if (display_it == topology_.displays().end() || !remote_cursor_.has_value()) {
          recover_after_handoff_failure();
          return Result<void>::failure("local display bounds unavailable during return");
        }
        const auto& source_bounds = remote_cursor_->source_bounds;
        const auto& target_bounds = remote_cursor_->bounds;
        const bool horizontal_target_edge = remote_cursor_->target_edge == Edge::kLeft ||
                                            remote_cursor_->target_edge == Edge::kRight;
        const double target_start = horizontal_target_edge ? target_bounds.top : target_bounds.left;
        const double target_extent = horizontal_target_edge ? target_bounds.height() : target_bounds.width();
        const double target_tangent = horizontal_target_edge ? remote_cursor_->position.y
                                                             : remote_cursor_->position.x;
        const double tangent_ratio = target_extent > 0.0
                                         ? std::clamp((target_tangent - target_start) / target_extent,
                                                      0.0, 1.0)
                                         : 0.5;
        const bool horizontal_source_edge = remote_cursor_->source_edge == Edge::kLeft ||
                                            remote_cursor_->source_edge == Edge::kRight;
        const double source_start = horizontal_source_edge ? source_bounds.top : source_bounds.left;
        const double source_extent = horizontal_source_edge ? source_bounds.height() : source_bounds.width();
        const double source_tangent = source_start + tangent_ratio * source_extent;
        Point source_boundary{};
        switch (remote_cursor_->source_edge) {
          case Edge::kLeft:
            source_boundary.x = source_bounds.left;
            source_boundary.y = std::clamp(source_tangent, source_bounds.top,
                                           std::nextafter(source_bounds.bottom, source_bounds.top));
            break;
          case Edge::kRight:
            source_boundary.x = source_bounds.right;
            source_boundary.y = std::clamp(source_tangent, source_bounds.top,
                                           std::nextafter(source_bounds.bottom, source_bounds.top));
            break;
          case Edge::kUp:
            source_boundary.x = std::clamp(source_tangent, source_bounds.left,
                                           std::nextafter(source_bounds.right, source_bounds.left));
            source_boundary.y = source_bounds.top;
            break;
          case Edge::kDown:
            source_boundary.x = std::clamp(source_tangent, source_bounds.left,
                                           std::nextafter(source_bounds.right, source_bounds.left));
            source_boundary.y = source_bounds.bottom;
            break;
        }
        debug::log("remote cursor exited target edge=", static_cast<unsigned>(*returning),
                   " tangent_ratio=", tangent_ratio, " source_boundary=", source_boundary.x,
                   ",", source_boundary.y);
        auto warped = config_.backend->warp_pointer(source_boundary);
        if (!warped.ok()) {
          debug::log("source return boundary warp failed error=", warped.error());
          recover_after_handoff_failure();
          continue;
        }
        clear_remote_cursor();
        actions = router_.on_return_to_local(now);
      }
    }

    // The event that wins a local preemption belongs to the local desktop. Do
    // not reuse its edge motion as a new handoff; a later event must cross the
    // edge after the old remote session has exited.
    if (!local_preempted_remote_lease && actions.empty() && queued.pointer.has_value() &&
        std::holds_alternative<MouseDelta>(event.payload) &&
        (router_.state() == RouterState::kLocal || router_.state() == RouterState::kHandoff)) {
      if (const auto crossing = detect_crossing(event, pointer, now)) {
        auto handoff = crossing->crossing;
        const bool resume_marker = router_.can_resume_remote_cursor(handoff.target);
        const bool resume_remote_cursor = resume_marker && remote_cursor_.has_value();
        if (resume_remote_cursor) {
          handoff.entry_point = remote_cursor_->position;
          handoff.entry = remote_cursor_->position;
          handoff.tangent_ratio = 0.0;
          handoff.has_tangent_ratio = false;
        }
        debug::log("edge crossing target_display=", handoff.target.low,
                   " entry=", handoff.entry_point.x, ",", handoff.entry_point.y,
                   " target_edge=", static_cast<unsigned>(handoff.target_edge),
                   " tangent_ratio=", handoff.tangent_ratio,
                   " source_boundary=", crossing->source_boundary.x, ",",
                   crossing->source_boundary.y);
        // Put the source cursor back on its own edge before the asynchronous
        // acquire/activate handshake. This prevents the native desktop from
        // continuing past the edge while the target cursor is entering.
        auto warped = config_.backend->warp_pointer(crossing->source_boundary);
        if (!warped.ok()) {
          debug::log("source boundary warp failed error=", warped.error());
          recover_after_handoff_failure();
          return warped;
        }
        debug::log("source cursor warped to boundary=", crossing->source_boundary.x, ",",
                   crossing->source_boundary.y);
        actions = router_.on_crossing(handoff, now);
        if (resume_marker)
          router_.consume_resume_remote_cursor();
        if (router_.state() == RouterState::kAcquiring ||
            router_.state() == RouterState::kHandoff) {
          begin_remote_cursor(handoff, crossing->source_edge);
        }
      }
    }
    if (actions.empty()) {
      const bool remote_physical = router_.state() == RouterState::kRemote &&
                                   event.origin == InputOrigin::kPhysical;
      if (remote_physical && !config_.backend->status().suppression) {
        if (!warned_remote_without_suppression_) {
          debug::log("WARNING remote physical input while suppression=off; local cursor may move");
          warned_remote_without_suppression_ = true;
        }
      } else if (config_.backend->status().suppression) {
        warned_remote_without_suppression_ = false;
      }
      if (router_.state() == RouterState::kRemote &&
          event.origin == InputOrigin::kPhysical && config_.backend->status().suppression) {
        // The runtime owns the target-side virtual cursor. Windows targets
        // receive its absolute position so SendInput does not apply pointer
        // acceleration; relative-capable targets receive a scaled delta so
        // high-rate motion remains smooth across display scales.
        InputEvent forwarded = event;
        if (remote_cursor_.has_value() &&
            std::holds_alternative<MouseDelta>(forwarded.payload)) {
          if (router_.peer_supports_absolute_pointer_moves()) {
            forwarded.payload = PointerMove{remote_cursor_->position.x,
                                            remote_cursor_->position.y};
          } else {
            const auto delta = std::get<MouseDelta>(forwarded.payload);
            forwarded.payload = MouseDelta{delta.dx * remote_cursor_->movement_scale,
                                           delta.dy * remote_cursor_->movement_scale};
          }
        }
        actions = router_.on_suppressed_event(std::move(forwarded), pointer, now);
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
      debug::log("suppression on before acquire");
      auto suppressed = config_.backend->set_suppression(true);
      if (!suppressed.ok()) {
        debug::log("suppression on failed error=", suppressed.error());
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
  if (!listener_ || !config_.accept_incoming) return Result<void>::success();
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
  Result<void> result = Result<void>::success();
  if (config_.clipboard_backend != nullptr && config_.clipboard_sync_enabled &&
      !clipboard_backend_started_) {
    auto started = start_clipboard_backend();
    if (!started.ok()) {
      debug::warn("clipboard backend retry start failed error=", started.error());
      result = Result<void>::failure(started.error());
    }
  }
  if (config_.clipboard_backend != nullptr && clipboard_backend_started_) {
    auto polled = config_.clipboard_backend->poll();
    if (!polled.ok()) {
      debug::warn("clipboard backend poll failed error=", polled.error());
      result = Result<void>::failure(polled.error());
    }
  }
  std::deque<platform::ClipboardData> clipboard_changes;
  {
    std::lock_guard lock(queue_mutex_);
    clipboard_changes.swap(queued_clipboard_changes_);
  }
  if (config_.clipboard_sync_enabled) {
    for (const auto& data : clipboard_changes) {
      if (clipboard_fingerprint_.has_value() && *clipboard_fingerprint_ == data) continue;
      clipboard_fingerprint_ = data;
      auto broadcasted = manager_.broadcast_clipboard(data, now);
      if (!broadcasted.ok()) {
        debug::warn("clipboard broadcast failed error=", broadcasted.error());
        if (result.ok()) result = broadcasted;
      }
    }
  }
  auto actions = router_.on_tick(now);
  auto dispatched = dispatch_actions(actions, now);
  if (!dispatched.ok() && result.ok()) result = dispatched;
  auto ticked = manager_.tick(now);
  if (!ticked.ok() && result.ok()) result = ticked;
  return result;
}

Result<void> InputNodeRuntime::stop(TimePoint now) {
  if (!running_) {
    std::lock_guard lock(queue_mutex_);
    queued_events_.clear();
    queued_clipboard_changes_.clear();
    clipboard_fingerprint_.reset();
    queue_overflowed_ = false;
    return Result<void>::success();
  }
  Result<void> result = Result<void>::success();
  for (const auto handle : peer_handles_) {
    auto removed = manager_.remove(handle, now);
    if (result.ok() && !removed.ok()) result = removed;
  }
  peer_handles_.clear();
  outgoing_peer_handle_.reset();
  if (listener_) {
    auto closed = listener_->close();
    if (result.ok() && !closed.ok()) result = closed;
    listener_.reset();
  }
  config_.backend->stop();
  if (config_.clipboard_backend != nullptr) {
    config_.clipboard_backend->stop();
    clipboard_backend_started_ = false;
  }
  running_ = false;
  edge_overflow_edge_.reset();
  edge_overflow_ = 0.0;
  remote_cursor_.reset();
  peer_display_wait_started_.reset();
  warned_peer_display_fallback_ = false;
  warned_remote_without_suppression_ = false;
  {
    std::lock_guard lock(queue_mutex_);
    queued_events_.clear();
    queued_clipboard_changes_.clear();
    clipboard_fingerprint_.reset();
    queue_overflowed_ = false;
  }
  return result;
}

bool InputNodeRuntime::connected() const noexcept {
  for (const auto handle : peer_handles_) {
    const auto state = manager_.state(handle);
    if (state.has_value() && *state == transport::PeerState::kConnected &&
        manager_.ready(handle))
      return true;
  }
  return false;
}

transport::Endpoint InputNodeRuntime::local_endpoint() const {
  return listener_ ? listener_->local_endpoint() : config_.listen_endpoint;
}

}  // namespace lanlink::runtime
