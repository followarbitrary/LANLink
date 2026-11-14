#pragma once

#include "lanlink/platform/clipboard_backend.hpp"

#include <cstddef>
#include <deque>
#include <utility>

namespace lanlink::platform {

class FakeClipboardBackend final : public IClipboardBackend {
 public:
  Result<void> start(ChangeCallback callback) override;
  void stop() override;
  Result<void> poll() override;
  Result<void> discard_pending_notifications() override;
  Result<ClipboardData> read() const override;
  Result<void> write(const ClipboardData& data) override;
  Result<void> clear() override;
  const std::string& last_error() const noexcept override { return last_error_; }

  void emit_change(ClipboardData data);
  void set_data(ClipboardData data);
  const ClipboardData& data() const noexcept { return data_; }
  std::size_t write_count() const noexcept { return write_count_; }

 private:
  void set_error(std::string error) { last_error_ = std::move(error); }

  ChangeCallback callback_;
  ClipboardData data_;
  std::deque<ClipboardData> pending_changes_;
  std::size_t write_count_{0};
  bool running_{false};
  std::string last_error_;
};

}  // namespace lanlink::platform
