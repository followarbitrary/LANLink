#include "lanlink/platform/fake_input_backend.hpp"

#include <gtest/gtest.h>

namespace lanlink::platform {
namespace {

InputEvent key_event(std::uint32_t code, bool pressed, SessionId session = {}) {
  return InputEvent{{}, session, 1, 1, Key{code, pressed}, InputOrigin::kSynthetic};
}

TEST(BackendContract, StartCapturesPhysicalEventsAndSuppressionIsObservable) {
  FakeInputBackend backend;
  std::vector<InputEvent> seen;
  ASSERT_TRUE(backend.start([&](const InputEvent& event) { seen.push_back(event); }).ok());
  ASSERT_TRUE(backend.set_suppression(true).ok());
  backend.emit_physical(key_event(65, true));
  ASSERT_EQ(seen.size(), 1u);
  EXPECT_EQ(seen.front().origin, InputOrigin::kPhysical);
  EXPECT_TRUE(backend.status().running);
  EXPECT_TRUE(backend.status().suppression);
  ASSERT_TRUE(backend.set_suppression(false).ok());
  EXPECT_FALSE(backend.status().suppression);
}

TEST(BackendContract, InjectedEventsDoNotLoopBackAndLedgerCanBeReleased) {
  FakeInputBackend backend;
  std::vector<InputEvent> seen;
  ASSERT_TRUE(backend.start([&](const InputEvent& event) { seen.push_back(event); }).ok());
  const SessionId session{1, 2};
  ASSERT_TRUE(backend.inject(key_event(65, true, session)).ok());
  EXPECT_EQ(seen.size(), 0u);
  ASSERT_EQ(backend.injected_events().size(), 1u);
  EXPECT_EQ(backend.injected_events().front().origin, InputOrigin::kRemoteInjected);
  EXPECT_EQ(backend.held_count(), 1u);
  backend.release_injected_state(session);
  EXPECT_EQ(backend.held_count(), 0u);
}

TEST(BackendContract, FakeLedgerTracksDistinctPressedInputs) {
  FakeInputBackend backend;
  ASSERT_TRUE(backend.start({}).ok());
  const SessionId session{9, 10};

  ASSERT_TRUE(backend.inject(key_event(65, true, session)).ok());
  ASSERT_TRUE(backend.inject(key_event(65, true, session)).ok());
  ASSERT_TRUE(backend.inject(key_event(66, true, session)).ok());
  ASSERT_EQ(backend.held_count(), 2u);

  ASSERT_TRUE(backend.inject(key_event(65, false, session)).ok());
  EXPECT_EQ(backend.held_count(), 1u);

  InputEvent button{{}, session, 1, 1, MouseButton{1, true}, InputOrigin::kSynthetic};
  ASSERT_TRUE(backend.inject(button).ok());
  ASSERT_TRUE(backend.inject(button).ok());
  EXPECT_EQ(backend.held_count(), 2u);
  button.payload = MouseButton{1, false};
  ASSERT_TRUE(backend.inject(button).ok());
  EXPECT_EQ(backend.held_count(), 1u);
}

TEST(BackendContract, InjectedReleaseEventsClearFakeLedger) {
  FakeInputBackend backend;
  ASSERT_TRUE(backend.start({}).ok());
  const SessionId session{3, 4};
  ASSERT_TRUE(backend.inject(key_event(65, true, session)).ok());
  ASSERT_EQ(backend.held_count(), 1u);
  ASSERT_TRUE(backend.inject(key_event(65, false, session)).ok());
  EXPECT_EQ(backend.held_count(), 0u);
}

TEST(BackendContract, DuplicateKeyDownNeedsOnlyOneRelease) {
  FakeInputBackend backend;
  ASSERT_TRUE(backend.start({}).ok());
  const SessionId session{5, 6};
  ASSERT_TRUE(backend.inject(key_event(65, true, session)).ok());
  ASSERT_TRUE(backend.inject(key_event(65, true, session)).ok());
  EXPECT_EQ(backend.held_count(), 1u);
  ASSERT_TRUE(backend.inject(key_event(65, false, session)).ok());
  EXPECT_EQ(backend.held_count(), 0u);
}

TEST(BackendContract, DuplicateButtonDownNeedsOnlyOneRelease) {
  FakeInputBackend backend;
  ASSERT_TRUE(backend.start({}).ok());
  const SessionId session{9, 10};
  auto button_event = [session](bool pressed) {
    return InputEvent{{}, session, 1, 1, MouseButton{1, pressed}, InputOrigin::kSynthetic};
  };
  ASSERT_TRUE(backend.inject(button_event(true)).ok());
  ASSERT_TRUE(backend.inject(button_event(true)).ok());
  EXPECT_EQ(backend.held_count(), 1u);
  ASSERT_TRUE(backend.inject(button_event(false)).ok());
  EXPECT_EQ(backend.held_count(), 0u);
}

TEST(BackendContract, InjectionFailureIsExplicitAndRecoverClearsState) {
  FakeInputBackend backend;
  ASSERT_TRUE(backend.start({}).ok());
  ASSERT_TRUE(backend.set_suppression(true).ok());
  const SessionId session{7, 8};
  ASSERT_TRUE(backend.inject(key_event(65, true, session)).ok());
  backend.set_injection_failure(true);
  EXPECT_FALSE(backend.inject(key_event(66, true, session)).ok());
  EXPECT_TRUE(backend.status().degraded);
  ASSERT_TRUE(backend.recover().ok());
  EXPECT_FALSE(backend.status().suppression);
  EXPECT_EQ(backend.held_count(), 0u);
  ASSERT_FALSE(backend.released_sessions().empty());
  EXPECT_EQ(backend.released_sessions().back(), session);
  EXPECT_EQ(backend.status().recovery_count, 1u);
  ASSERT_TRUE(backend.recover().ok());
  EXPECT_EQ(backend.status().recovery_count, 2u);
}

TEST(BackendContract, PermissionAndCaptureFailuresAreExplicit) {
  FakeInputBackend backend;
  backend.set_permission(false);
  auto denied = backend.start({});
  EXPECT_FALSE(denied.ok());
  EXPECT_FALSE(backend.status().permission);
  backend.set_permission(true);
  backend.set_capture_available(false);
  auto unavailable = backend.start({});
  EXPECT_FALSE(unavailable.ok());
  EXPECT_FALSE(backend.status().capture);
}

}  // namespace
}  // namespace lanlink::platform
