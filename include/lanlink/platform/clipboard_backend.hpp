#pragma once

#include "lanlink/common/result.hpp"

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace lanlink::platform {

inline constexpr std::size_t kMaxClipboardDataSize = 4U * 1024U * 1024U;
inline constexpr std::array<std::uint8_t, 8> kPngSignature{
    0x89U, 0x50U, 0x4eU, 0x47U, 0x0dU, 0x0aU, 0x1aU, 0x0aU};

struct ClipboardData {
  std::optional<std::string> text;
  std::optional<std::string> html;
  std::optional<std::vector<std::uint8_t>> png;

  bool operator==(const ClipboardData&) const = default;

  std::size_t byte_size() const noexcept {
    std::size_t size = text ? text->size() : 0U;
    if (html) size += html->size();
    if (png) size += png->size();
    return size;
  }
};

inline Result<void> validate_clipboard_data(const ClipboardData& data) {
  if (data.byte_size() > kMaxClipboardDataSize)
    return Result<void>::failure("clipboard package exceeds 4 MiB");
  if (data.png.has_value()) {
    if (data.png->size() < kPngSignature.size() ||
        !std::equal(kPngSignature.begin(), kPngSignature.end(), data.png->begin()))
      return Result<void>::failure("clipboard PNG has an invalid signature");
  }
  return Result<void>::success();
}

class IClipboardBackend {
 public:
  using ChangeCallback = std::function<void(ClipboardData)>;
  virtual ~IClipboardBackend() = default;

  virtual Result<void> start(ChangeCallback callback) = 0;
  virtual void stop() = 0;
  virtual Result<void> poll() = 0;
  virtual Result<ClipboardData> read() const = 0;
  virtual Result<void> write(const ClipboardData& data) = 0;
  virtual Result<void> clear() = 0;
  virtual const std::string& last_error() const noexcept = 0;
};

using ClipboardBackend = IClipboardBackend;

}  // namespace lanlink::platform
