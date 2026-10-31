#pragma once
#include <cstdint>
#include <functional>

namespace lanlink {
struct Uuid128 {
  std::uint64_t high{};
  std::uint64_t low{};
  friend constexpr bool operator==(const Uuid128&, const Uuid128&) = default;
};
using DeviceId = Uuid128;
using SessionId = Uuid128;
using RequestId = Uuid128;
using DisplayId = Uuid128;
using SourceId = Uuid128;
}

namespace std {
template <> struct hash<lanlink::Uuid128> {
  size_t operator()(const lanlink::Uuid128& id) const noexcept {
    const auto h = std::hash<std::uint64_t>{}(id.high);
    const auto l = std::hash<std::uint64_t>{}(id.low);
    return h ^ (l + static_cast<size_t>(0x9e3779b97f4a7c15ULL) + (h << 6U) + (h >> 2U));
  }
};
}
