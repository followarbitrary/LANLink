#pragma once

#include "lanlink/common/result.hpp"
#include "lanlink/protocol/wire.hpp"

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace lanlink::platform {

// The protocol reserves a conservative metadata budget so this payload limit
// enforces the 4 MiB cap for the complete on-wire package.
inline constexpr std::size_t kMaxClipboardDataSize = protocol::kMaxClipboardPayloadSize;
inline constexpr std::size_t kMaxClipboardNotificationQueue = 64;
inline constexpr std::array<std::uint8_t, 8> kPngSignature{
    0x89U, 0x50U, 0x4eU, 0x47U, 0x0dU, 0x0aU, 0x1aU, 0x0aU};

inline bool valid_utf8(const std::string& value) noexcept {
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(value.data());
  std::size_t index = 0;
  while (index < value.size()) {
    const auto lead = bytes[index++];
    std::size_t continuation_count = 0;
    std::uint32_t code_point = 0;
    if (lead <= 0x7fU) {
      continue;
    } else if (lead >= 0xc2U && lead <= 0xdfU) {
      continuation_count = 1;
      code_point = lead & 0x1fU;
    } else if (lead >= 0xe0U && lead <= 0xefU) {
      continuation_count = 2;
      code_point = lead & 0x0fU;
    } else if (lead >= 0xf0U && lead <= 0xf4U) {
      continuation_count = 3;
      code_point = lead & 0x07U;
    } else {
      return false;
    }
    if (index + continuation_count > value.size()) return false;
    for (std::size_t count = 0; count < continuation_count; ++count) {
      const auto continuation = bytes[index++];
      if ((continuation & 0xc0U) != 0x80U) return false;
      code_point = (code_point << 6U) | (continuation & 0x3fU);
    }
    if ((continuation_count == 2 && code_point < 0x800U) ||
        (continuation_count == 3 && code_point < 0x10000U) ||
        (code_point >= 0xd800U && code_point <= 0xdfffU) || code_point > 0x10ffffU)
      return false;
  }
  return true;
}

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
  if ((data.text && !valid_utf8(*data.text)) || (data.html && !valid_utf8(*data.html)))
    return Result<void>::failure("clipboard text is not valid UTF-8");
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
  // Discard notifications queued before a fresh enable baseline. Native
  // backends override this; the default is suitable for callback-free custom
  // backends.
  virtual Result<void> discard_pending_notifications() { return Result<void>::success(); }
  virtual Result<ClipboardData> read() const = 0;
  virtual Result<void> write(const ClipboardData& data) = 0;
  virtual Result<void> clear() = 0;
  virtual const std::string& last_error() const noexcept = 0;
};

using ClipboardBackend = IClipboardBackend;

}  // namespace lanlink::platform
