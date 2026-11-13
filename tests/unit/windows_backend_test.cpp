#include "lanlink/platform/windows_input_backend.hpp"

#include <gtest/gtest.h>

#include <mutex>

namespace lanlink::platform {
namespace {

TEST(WindowsInputBackend, NonWindowsBuildReportsUnsupported) {
#if !defined(_WIN32)
  WindowsInputBackend backend;
  EXPECT_FALSE(backend.display_bounds().has_value());
  EXPECT_FALSE(backend.start({}).ok());
  EXPECT_NE(backend.status().last_error.find("unsupported"), std::string::npos);
  EXPECT_FALSE(backend.set_suppression(true).ok());
  EXPECT_FALSE(backend.inject(InputEvent{}).ok());
  EXPECT_FALSE(backend.recover().ok());
#else
  GTEST_SKIP() << "fallback behavior is only asserted on non-Windows";
#endif
}

TEST(WindowsInputBackend, StopAndReleaseAreIdempotent) {
  WindowsInputBackend backend;
  backend.stop();
  backend.stop();
  backend.release_injected_state(SessionId{1, 2});
  backend.release_injected_state(SessionId{1, 2});
  EXPECT_FALSE(backend.status().running);
  EXPECT_FALSE(backend.status().suppression);
}

TEST(WindowsInputBackend, HookForwardingReleasesStateLockBeforeCallingNextHook) {
  std::timed_mutex state_mutex;
  bool next_hook_called = false;

  const auto result = detail::dispatch_hook(
      [&] {
        std::lock_guard lock(state_mutex);
        return false;
      },
      [&] {
        next_hook_called = state_mutex.try_lock();
        if (next_hook_called) state_mutex.unlock();
        return 17L;
      });

  EXPECT_EQ(result, 17L);
  EXPECT_TRUE(next_hook_called);
}

TEST(WindowsInputBackend, HookForwardingSuppressesWithoutCallingNextHook) {
  bool next_hook_called = false;

  const auto result = detail::dispatch_hook(
      [] { return true; },
      [&] {
        next_hook_called = true;
        return 17L;
      });

  EXPECT_EQ(result, 1L);
  EXPECT_FALSE(next_hook_called);
}

}  // namespace
}  // namespace lanlink::platform
