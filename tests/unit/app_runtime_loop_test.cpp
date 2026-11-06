#include "lanlink/app/runtime_loop.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace {
using namespace std::chrono_literals;

TEST(AppRuntimeLoop, IgnoresInterruptedPollOnlyDuringShutdown) {
  EXPECT_TRUE(lanlink::app::is_shutdown_interrupt(true, "read wait interrupted"));
  EXPECT_FALSE(lanlink::app::is_shutdown_interrupt(false, "read wait interrupted"));
  EXPECT_FALSE(lanlink::app::is_shutdown_interrupt(true, "socket read failed"));
}

TEST(AppRuntimeLoop, BacksOffListenerOnlyLoopWithoutDelayingConnectedLoop) {
  EXPECT_EQ(lanlink::app::idle_wait(false), 10ms);
  EXPECT_EQ(lanlink::app::idle_wait(true), 0ms);
}
}  // namespace
