#include "lanlink/platform/fake_input_backend.hpp"

#include <algorithm>

namespace lanlink::platform {

Result<void> FakeInputBackend::start(EventCallback callback) {
  if (!status_.permission) {
    set_error("permission denied");
    status_.running = false;
    return Result<void>::failure(status_.last_error);
  }
  if (!status_.capture) {
    set_error("capture unavailable");
    status_.running = false;
    return Result<void>::failure(status_.last_error);
  }
  callback_ = std::move(callback);
  status_.running = true;
  status_.injection = !injection_failure_;
  status_.last_error.clear();
  refresh_degraded();
  return Result<void>::success();
}

void FakeInputBackend::stop() {
  for (const auto& [session, _] : held_) released_sessions_.push_back(session);
  held_.clear();
  status_.running = false;
  status_.suppression = false;
  callback_ = {};
}

Result<void> FakeInputBackend::set_suppression(bool enabled) {
  if (enabled && (!status_.running || !status_.capture)) {
    set_error("capture unavailable");
    return Result<void>::failure(status_.last_error);
  }
  status_.suppression = enabled;
  suppression_history_.push_back(enabled);
  return Result<void>::success();
}

Result<void> FakeInputBackend::inject(const InputEvent& event) {
  if (!status_.running || injection_failure_ || !status_.injection) {
    status_.injection = false;
    status_.degraded = true;
    set_error("injection failed");
    return Result<void>::failure(status_.last_error);
  }
  auto tagged = event;
  tagged.origin = InputOrigin::kRemoteInjected;
  injected_events_.push_back(tagged);
  if (const auto* warp = std::get_if<PointerWarp>(&tagged.payload)) {
    pointer_position_ = Point{warp->x, warp->y};
    have_pointer_position_ = true;
  } else if (const auto* delta = std::get_if<MouseDelta>(&tagged.payload)) {
    if (have_pointer_position_) {
      pointer_position_.x += delta->dx;
      pointer_position_.y += delta->dy;
    }
  }
  if (const auto* key = std::get_if<Key>(&tagged.payload)) {
    if (key->pressed) {
      held_[tagged.session].keys.insert(key->code);
    } else if (const auto it = held_.find(tagged.session); it != held_.end()) {
      it->second.keys.erase(key->code);
      if (it->second.keys.empty() && it->second.buttons.empty()) held_.erase(it);
    }
  } else if (const auto* button = std::get_if<MouseButton>(&tagged.payload)) {
    if (button->pressed) {
      held_[tagged.session].buttons.insert(button->button);
    } else if (const auto it = held_.find(tagged.session); it != held_.end()) {
      it->second.buttons.erase(button->button);
      if (it->second.keys.empty() && it->second.buttons.empty()) held_.erase(it);
    }
  }
  return Result<void>::success();
}

Result<void> FakeInputBackend::warp_pointer(Point position) {
  if (!status_.running || injection_failure_ || !status_.injection) {
    status_.injection = false;
    status_.degraded = true;
    set_error("pointer warp failed");
    return Result<void>::failure(status_.last_error);
  }
  pointer_position_ = position;
  have_pointer_position_ = true;
  return Result<void>::success();
}

void FakeInputBackend::release_injected_state(const SessionId& session) {
  released_sessions_.push_back(session);
  held_.erase(session);
}

Result<void> FakeInputBackend::recover() {
  status_.suppression = false;
  suppression_history_.push_back(false);
  for (const auto& [session, _] : held_) released_sessions_.push_back(session);
  held_.clear();
  ++status_.recovery_count;
  status_.capture = status_.permission && status_.capture;
  status_.running = status_.permission && status_.capture;
  status_.injection = status_.running && !injection_failure_;
  if (status_.running && status_.capture) {
    status_.last_error.clear();
  }
  refresh_degraded();
  return Result<void>::success();
}

void FakeInputBackend::set_permission(bool available) {
  status_.permission = available;
  if (!available) {
    status_.running = false;
    status_.suppression = false;
  }
  refresh_degraded();
}

void FakeInputBackend::set_capture_available(bool available) {
  status_.capture = available;
  if (!available) {
    status_.running = false;
    status_.suppression = false;
  }
  refresh_degraded();
}

void FakeInputBackend::set_injection_available(bool available) {
  status_.injection = available;
  injection_failure_ = !available;
  refresh_degraded();
}

void FakeInputBackend::set_injection_failure(bool failed) {
  injection_failure_ = failed;
  status_.injection = !failed;
  refresh_degraded();
}

void FakeInputBackend::emit_physical(const InputEvent& event) {
  if (!status_.running || !status_.capture) return;
  auto tagged = event;
  tagged.origin = InputOrigin::kPhysical;
  if (!status_.suppression) {
    if (const auto* delta = std::get_if<MouseDelta>(&tagged.payload)) {
      if (have_pointer_position_) {
        pointer_position_.x += delta->dx;
        pointer_position_.y += delta->dy;
      }
    }
  }
  physical_events_.push_back(tagged);
  if (callback_) callback_(tagged);
}

std::optional<Point> FakeInputBackend::pointer_position() const noexcept {
  if (!have_pointer_position_) return std::nullopt;
  return pointer_position_;
}

void FakeInputBackend::set_pointer_position(Point position) {
  pointer_position_ = position;
  have_pointer_position_ = true;
}

std::size_t FakeInputBackend::held_count() const noexcept {
  std::size_t count = 0;
  for (const auto& [_, held] : held_) count += held.keys.size() + held.buttons.size();
  return count;
}

void FakeInputBackend::set_error(const char* message) {
  status_.last_error = message;
  status_.degraded = true;
}

void FakeInputBackend::refresh_degraded() {
  status_.degraded = !status_.permission || !status_.capture || !status_.injection;
}

}  // namespace lanlink::platform
