#pragma once

#include "lanlink/common/result.hpp"
#include "lanlink/input/events.hpp"

#include <cstddef>
#include <functional>
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
  virtual void release_injected_state(const SessionId& session) = 0;
  virtual Result<void> recover() = 0;
  virtual const BackendStatus& status() const noexcept = 0;
};

using InputBackend = IInputBackend;

}  // namespace lanlink::platform
