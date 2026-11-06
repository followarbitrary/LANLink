#pragma once

#include "lanlink/platform/input_backend.hpp"

#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lanlink::platform {

class FakeInputBackend final : public IInputBackend {
 public:
  Result<void> start(EventCallback callback) override;
  void stop() override;
  Result<void> set_suppression(bool enabled) override;
  Result<void> inject(const InputEvent& event) override;
  std::optional<Point> pointer_position() const noexcept override;
  void release_injected_state(const SessionId& session) override;
  Result<void> recover() override;
  const BackendStatus& status() const noexcept override { return status_; }

  void set_permission(bool available);
  void set_pointer_position(Point position);
  void set_permission_available(bool available) { set_permission(available); }
  void set_capture_available(bool available);
  void set_capture_failure(bool failed) { set_capture_available(!failed); }
  void set_injection_available(bool available);
  void set_injection_failure(bool failed);
  void emit_physical(const InputEvent& event);
  void emit_physical_event(const InputEvent& event) { emit_physical(event); }

  const std::vector<bool>& suppression_history() const noexcept { return suppression_history_; }
  const std::vector<InputEvent>& physical_events() const noexcept { return physical_events_; }
  const std::vector<InputEvent>& injected_events() const noexcept { return injected_events_; }
  const std::vector<SessionId>& released_sessions() const noexcept { return released_sessions_; }
  std::size_t held_count() const noexcept;

 private:
  struct HeldState {
    std::unordered_set<std::uint32_t> keys;
    std::unordered_set<std::uint32_t> buttons;
  };

  void set_error(const char* message);
  void refresh_degraded();

  BackendStatus status_{};
  EventCallback callback_;
  bool injection_failure_{false};
  std::vector<bool> suppression_history_;
  std::vector<InputEvent> physical_events_;
  std::vector<InputEvent> injected_events_;
  std::vector<SessionId> released_sessions_;
  std::unordered_map<SessionId, HeldState> held_;
  Point pointer_position_{};
  bool have_pointer_position_{false};
};

}  // namespace lanlink::platform
