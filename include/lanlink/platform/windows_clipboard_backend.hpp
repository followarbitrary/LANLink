#pragma once

#include "lanlink/platform/clipboard_backend.hpp"

#include <memory>

namespace lanlink::platform {

class WindowsClipboardBackend final : public IClipboardBackend {
 public:
  struct Impl;

  WindowsClipboardBackend();
  ~WindowsClipboardBackend() override;
  WindowsClipboardBackend(const WindowsClipboardBackend&) = delete;
  WindowsClipboardBackend& operator=(const WindowsClipboardBackend&) = delete;

  Result<void> start(ChangeCallback callback) override;
  void stop() override;
  Result<void> poll() override;
  Result<void> discard_pending_notifications() override;
  Result<ClipboardData> read() const override;
  Result<void> write(const ClipboardData& data) override;
  Result<void> clear() override;
  const std::string& last_error() const noexcept override { return last_error_; }

 private:
  std::unique_ptr<Impl> impl_;
  ChangeCallback callback_;
  std::string last_error_;
};

using NativeWindowsClipboardBackend = WindowsClipboardBackend;

}  // namespace lanlink::platform
