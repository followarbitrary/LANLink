#include <gtest/gtest.h>
#include <chrono>
#include <functional>
#include <unordered_set>
#include "lanlink/common/clock.hpp"
#include "lanlink/common/config.hpp"
#include "lanlink/common/ids.hpp"
#include "lanlink/common/liveness.hpp"
#include "lanlink/common/result.hpp"

using namespace std::chrono_literals;

TEST(CommonTypes, UuidEqualityAndHash) {
  lanlink::Uuid128 a{0x0123456789abcdefULL, 0xfedcba9876543210ULL};
  lanlink::Uuid128 b{0x0123456789abcdefULL, 0xfedcba9876543210ULL};
  lanlink::Uuid128 c{0x0123456789abcdefULL, 0xfedcba9876543211ULL};
  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
  std::unordered_set<lanlink::Uuid128> ids{a};
  EXPECT_EQ(ids.count(b), 1u);
  EXPECT_EQ(ids.count(c), 0u);
}

TEST(CommonTypes, MockClockAdvancesAndSets) {
  lanlink::MockClock clock;
  EXPECT_EQ(clock.now().time_since_epoch(), lanlink::IClock::Duration::zero());
  clock.advance(1500ms);
  EXPECT_EQ(clock.now().time_since_epoch(), 1500ms);
  auto target = lanlink::IClock::time_point{4200ms};
  clock.set(target);
  EXPECT_EQ(clock.now(), target);
}

TEST(CommonTypes, FrozenM0Constants) {
  EXPECT_EQ(lanlink::m0::kHeartbeatInterval, 1000ms);
  EXPECT_EQ(lanlink::m0::kConnectionTimeout, 3000ms);
  EXPECT_EQ(lanlink::m0::kLeaseDuration, 3000ms);
  EXPECT_EQ(lanlink::m0::kLeaseRenewInterval, 1000ms);
  EXPECT_EQ(lanlink::m0::kEdgeThreshold, 4.0);
  EXPECT_EQ(lanlink::m0::kEntryInset, 8.0);
  EXPECT_EQ(lanlink::m0::kInitialGeneration, 1u);
  EXPECT_EQ(lanlink::m0::kInitialSequence, 1u);
}

TEST(CommonTypes, ResultSuccessAndErrorInspection) {
  auto success = lanlink::Result<int>::success(7);
  ASSERT_TRUE(success.ok());
  EXPECT_TRUE(success.has_value());
  EXPECT_EQ(success.value(), 7);

  auto failure = lanlink::Result<int>::failure("bad request");
  EXPECT_FALSE(failure.ok());
  EXPECT_FALSE(failure.has_value());
  EXPECT_EQ(failure.error(), "bad request");

  auto void_success = lanlink::Result<void>::success();
  EXPECT_TRUE(void_success.ok());
  auto void_failure = lanlink::Result<void>::failure("nope");
  EXPECT_FALSE(void_failure.ok());
  EXPECT_EQ(void_failure.error(), "nope");
}

TEST(CommonTypes, ConnectionLivenessUsesInclusiveTimeoutBoundary) {
  lanlink::ConnectionLiveness liveness;
  const auto start = lanlink::IClock::time_point{};
  liveness.mark_alive(start);
  EXPECT_FALSE(liveness.expired(start + 2999ms));
  EXPECT_TRUE(liveness.expired(start + 3000ms));
}
