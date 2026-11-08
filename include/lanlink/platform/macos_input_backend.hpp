#pragma once

#include "lanlink/platform/input_backend.hpp"

#include <memory>

namespace lanlink::platform {

class MacOSInputBackend final : public IInputBackend {
 public:
  struct Impl;

  MacOSInputBackend();
  ~MacOSInputBackend() override;
  MacOSInputBackend(const MacOSInputBackend&) = delete;
  MacOSInputBackend& operator=(const MacOSInputBackend&) = delete;

  Result<void> start(EventCallback callback) override;
  void stop() override;
  Result<void> set_suppression(bool enabled) override;
  Result<void> inject(const InputEvent& event) override;
  std::optional<Rect> display_bounds() const noexcept override;
  double display_scale() const noexcept override;
  Result<void> warp_pointer(Point point) override;
  std::optional<Point> pointer_position() const noexcept override;
  void release_injected_state(const SessionId& session) override;
  Result<void> recover() override;
  const BackendStatus& status() const noexcept override { return status_; }

 private:
  std::unique_ptr<Impl> impl_;
  BackendStatus status_{};
};

using NativeMacOSInputBackend = MacOSInputBackend;

}  // namespace lanlink::platform
