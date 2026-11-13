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

TEST(WindowsInputBackend, UnlockedSystemCallRunsOutsideStateMutex) {
  std::timed_mutex state_mutex;
  bool operation_saw_unlocked_mutex = false;
  bool commit_saw_locked_mutex = false;

  const auto result = detail::run_unlocked(
      state_mutex,
      [] { return 42; },
      [&](int&) {
        operation_saw_unlocked_mutex = state_mutex.try_lock();
        if (operation_saw_unlocked_mutex) state_mutex.unlock();
        return operation_saw_unlocked_mutex;
      },
      [&](int&, bool operation_ok) {
        commit_saw_locked_mutex = !state_mutex.try_lock();
        if (!commit_saw_locked_mutex) state_mutex.unlock();
        return operation_ok;
      });

  EXPECT_TRUE(result);
  EXPECT_TRUE(operation_saw_unlocked_mutex);
  EXPECT_TRUE(commit_saw_locked_mutex);
}

TEST(WindowsInputBackend, RunningBackendSkipsJoiningPreviousThread) {
  bool joined = false;

  detail::join_previous_thread_if_stopped([] { return true; }, [&] { joined = true; });

  EXPECT_FALSE(joined);
}

TEST(WindowsInputBackend, StoppedBackendJoinsPreviousThread) {
  bool joined = false;

  detail::join_previous_thread_if_stopped([] { return false; }, [&] { joined = true; });

  EXPECT_TRUE(joined);
}

TEST(WindowsInputBackend, StartWhileRunningDoesNotWaitForHookThread) {
#if defined(_WIN32)
  WindowsInputBackend backend;
  const auto first_start = backend.start({});
  if (!first_start.ok()) GTEST_SKIP() << first_start.error();

  EXPECT_TRUE(backend.start({}).ok());
  backend.stop();
#else
  GTEST_SKIP() << "Windows hook lifecycle is only exercised on Windows";
#endif
}

}  // namespace
}  // namespace lanlink::platform
