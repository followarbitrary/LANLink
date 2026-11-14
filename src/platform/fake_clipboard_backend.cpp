#include "lanlink/platform/fake_clipboard_backend.hpp"

#include <utility>

namespace lanlink::platform {

Result<void> FakeClipboardBackend::start(ChangeCallback callback) {
  callback_ = std::move(callback);
  running_ = true;
  last_error_.clear();
  return Result<void>::success();
}

void FakeClipboardBackend::stop() {
  running_ = false;
  callback_ = {};
  pending_changes_.clear();
}

Result<void> FakeClipboardBackend::poll() {
  if (!running_) {
    set_error("clipboard backend is not running");
    return Result<void>::failure(last_error_);
  }
  if (pending_changes_.empty()) return Result<void>::success();

  ClipboardData next = std::move(pending_changes_.front());
  pending_changes_.pop_front();
  data_ = next;
  if (callback_) callback_(std::move(next));
  return Result<void>::success();
}

Result<ClipboardData> FakeClipboardBackend::read() const {
  if (!running_) return Result<ClipboardData>::failure("clipboard backend is not running");
  return Result<ClipboardData>::success(data_);
}

Result<void> FakeClipboardBackend::write(const ClipboardData& data) {
  const auto valid = validate_clipboard_data(data);
  if (!valid.ok()) {
    set_error(valid.error());
    return valid;
  }
  if (!running_) {
    set_error("clipboard backend is not running");
    return Result<void>::failure(last_error_);
  }
  data_ = data;
  ++write_count_;
  last_error_.clear();
  return Result<void>::success();
}

Result<void> FakeClipboardBackend::clear() { return write(ClipboardData{}); }

void FakeClipboardBackend::emit_change(ClipboardData data) {
  pending_changes_.push_back(std::move(data));
}

void FakeClipboardBackend::set_data(ClipboardData data) { data_ = std::move(data); }

}  // namespace lanlink::platform
