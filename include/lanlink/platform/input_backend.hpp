#pragma once

#include "lanlink/common/result.hpp"
#include "lanlink/input/events.hpp"
#include "lanlink/topology/topology.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>

namespace lanlink::platform {

struct BackendStatus {
  bool running{false};
  bool suppression{false};
  bool permission{true};
  bool injection{true};
  bool capture{true};
  bool degraded{false};
  std::size_t recovery_count{0};
  std::string last_error;
};

class IInputBackend {
 public:
  using EventCallback = std::function<void(const InputEvent&)>;
  virtual ~IInputBackend() = default;

  virtual Result<void> start(EventCallback callback) = 0;
  virtual void stop() = 0;
  virtual Result<void> set_suppression(bool enabled) = 0;
  virtual Result<void> inject(const InputEvent& event) = 0;
  // Return the logical coordinates of the local virtual desktop. Native
  // backends report the same coordinate space used by pointer events (Quartz
  // points on macOS, DPI-aware Win32 pixels on Windows). Backends that cannot
  // query a desktop, such as an unsupported platform build, return nullopt.
  virtual std::optional<Rect> display_bounds() const noexcept { return std::nullopt; }
  // Return the native backing scale for the same coordinate space. A scale of
  // 1.0 is the safe fallback when the platform cannot report one.
  virtual double display_scale() const noexcept { return 1.0; }
  // Whether remote pointer motion should be delivered as absolute positions.
  // Windows uses this to avoid applying the user's pointer acceleration curve;
  // macOS can consume relative deltas smoothly.
  virtual bool supports_absolute_pointer_moves() const noexcept { return false; }
  // Move the local cursor without treating the move as a remote input event.
  // Runtime handoff uses this to return the source cursor to its edge while
  // the control lease is being negotiated.
  virtual Result<void> warp_pointer(Point point) {
    return inject(InputEvent{{}, {}, 0, 0, PointerWarp{point.x, point.y},
                             InputOrigin::kSynthetic});
  }
  virtual std::optional<Point> pointer_position() const noexcept = 0;
  virtual void release_injected_state(const SessionId& session) = 0;
  virtual Result<void> recover() = 0;
  virtual const BackendStatus& status() const noexcept = 0;
};

using InputBackend = IInputBackend;

}  // namespace lanlink::platform
