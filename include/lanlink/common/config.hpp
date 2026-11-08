#pragma once
#include <chrono>
#include <cstdint>

namespace lanlink::m0 {
inline constexpr std::chrono::milliseconds kHeartbeatInterval{1000};
inline constexpr std::chrono::milliseconds kConnectionTimeout{3000};
inline constexpr std::chrono::milliseconds kLeaseDuration{3000};
inline constexpr std::chrono::milliseconds kLeaseRenewInterval{1000};
inline constexpr std::chrono::milliseconds kDisplayInfoWaitTimeout{3000};
inline constexpr std::chrono::milliseconds kPointerPositionInterval{100};
inline constexpr double kEdgeThreshold = 4.0;
inline constexpr double kEntryInset = 8.0;
inline constexpr std::uint64_t kInitialGeneration = 1;
inline constexpr std::uint64_t kInitialSequence = 1;
}
