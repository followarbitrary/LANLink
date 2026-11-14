#pragma once

#include "lanlink/platform/clipboard_backend.hpp"

#include <memory>

namespace lanlink::platform {

class MacOSClipboardBackend final : public IClipboardBackend {
 public:
  struct Impl;

  MacOSClipboardBackend();
  ~MacOSClipboardBackend() override;
  MacOSClipboardBackend(const MacOSClipboardBackend&) = delete;
  MacOSClipboardBackend& operator=(const MacOSClipboardBackend&) = delete;

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
  bool running_{false};
  std::string last_error_;
};

using NativeMacOSClipboardBackend = MacOSClipboardBackend;

}  // namespace lanlink::platform
