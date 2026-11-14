#include "lanlink/platform/fake_input_backend.hpp"
#include "lanlink/platform/fake_clipboard_backend.hpp"

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

TEST(BackendContract, PointerPositionTracksPhysicalAndInjectedMovement) {
  FakeInputBackend backend;
  ASSERT_TRUE(backend.start({}).ok());
  backend.set_pointer_position(Point{10.0, 20.0});
  ASSERT_TRUE(backend.pointer_position().has_value());
  EXPECT_EQ(*backend.pointer_position(), (Point{10.0, 20.0}));

  backend.emit_physical(InputEvent{{}, {}, 0, 0, MouseDelta{3.0, -4.0}, InputOrigin::kPhysical});
  ASSERT_TRUE(backend.pointer_position().has_value());
  EXPECT_EQ(*backend.pointer_position(), (Point{13.0, 16.0}));

  ASSERT_TRUE(backend.inject(
      InputEvent{{}, {}, 1, 0, PointerWarp{50.0, 60.0}, InputOrigin::kRemoteInjected})
                  .ok());
  ASSERT_TRUE(backend.pointer_position().has_value());
  EXPECT_EQ(*backend.pointer_position(), (Point{50.0, 60.0}));
}

TEST(BackendContract, ReportsConfiguredDisplayBounds) {
  FakeInputBackend backend;
  EXPECT_FALSE(backend.display_bounds().has_value());

  backend.set_display_bounds(Rect{-1440.0, 0.0, 0.0, 900.0});
  ASSERT_TRUE(backend.display_bounds().has_value());
  const auto configured = *backend.display_bounds();
  EXPECT_DOUBLE_EQ(configured.left, -1440.0);
  EXPECT_DOUBLE_EQ(configured.top, 0.0);
  EXPECT_DOUBLE_EQ(configured.right, 0.0);
  EXPECT_DOUBLE_EQ(configured.bottom, 900.0);
}

TEST(BackendContract, ReportsConfiguredDisplayScale) {
  FakeInputBackend backend;
  EXPECT_DOUBLE_EQ(backend.display_scale(), 1.0);
  backend.set_display_scale(1.5);
  EXPECT_DOUBLE_EQ(backend.display_scale(), 1.5);
}

TEST(ClipboardBackendContract, PollDeliversQueuedChangeExactlyOnce) {
  FakeClipboardBackend backend;
  ClipboardData expected;
  expected.text = "hello";
  expected.html = "<b>hello</b>";
  expected.png = std::vector<std::uint8_t>{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  std::vector<ClipboardData> seen;
  ASSERT_TRUE(backend.start([&](ClipboardData data) { seen.push_back(std::move(data)); }).ok());

  backend.emit_change(expected);
  ASSERT_TRUE(backend.poll().ok());
  ASSERT_EQ(seen.size(), 1u);
  EXPECT_EQ(seen.front(), expected);
  ASSERT_TRUE(backend.poll().ok());
  EXPECT_EQ(seen.size(), 1u);
}

TEST(ClipboardBackendContract, WriteUpdatesDataWithoutEchoAndClearEmptiesIt) {
  FakeClipboardBackend backend;
  ASSERT_TRUE(backend.start({}).ok());
  ClipboardData expected;
  expected.text = "copied";
  ASSERT_TRUE(backend.write(expected).ok());
  EXPECT_EQ(backend.data(), expected);
  EXPECT_EQ(backend.write_count(), 1u);
  ASSERT_TRUE(backend.poll().ok());

  ASSERT_TRUE(backend.clear().ok());
  EXPECT_EQ(backend.data(), ClipboardData{});
  EXPECT_EQ(backend.write_count(), 2u);
  ASSERT_TRUE(backend.poll().ok());
}

TEST(ClipboardBackendContract, EqualityDistinguishesAbsentAndEmptyTextAndHtml) {
  ClipboardData absent;
  ClipboardData empty_text;
  empty_text.text = "";
  ClipboardData empty_html;
  empty_html.html = "";
  EXPECT_NE(absent, empty_text);
  EXPECT_NE(absent, empty_html);
  EXPECT_NE(empty_text, empty_html);
  EXPECT_EQ(absent.byte_size(), 0u);
  EXPECT_EQ(empty_text.byte_size(), 0u);
}

TEST(ClipboardBackendContract, RejectsOversizedAndInvalidPngWrites) {
  FakeClipboardBackend backend;
  ASSERT_TRUE(backend.start({}).ok());

  ClipboardData oversized;
  oversized.text = std::string(kMaxClipboardDataSize + 1, 'x');
  EXPECT_FALSE(backend.write(oversized).ok());
  EXPECT_FALSE(backend.last_error().empty());

  ClipboardData invalid_png;
  invalid_png.png = std::vector<std::uint8_t>{0, 1, 2, 3};
  EXPECT_FALSE(backend.write(invalid_png).ok());
}

TEST(ClipboardBackendContract, FakeRejectsInvalidChangesAndSeedData) {
  FakeClipboardBackend backend;
  ASSERT_TRUE(backend.start({}).ok());
  ClipboardData original;
  original.text = "original";
  ASSERT_TRUE(backend.write(original).ok());

  ClipboardData invalid;
  invalid.text = std::string("\xc3\x28", 2);
  backend.emit_change(invalid);
  ASSERT_TRUE(backend.poll().ok());
  EXPECT_EQ(backend.data(), original);

  backend.set_data(invalid);
  EXPECT_EQ(backend.data(), original);
  EXPECT_FALSE(backend.last_error().empty());

  ClipboardData oversized;
  oversized.html = std::string(kMaxClipboardDataSize + 1, 'x');
  backend.emit_change(oversized);
  ASSERT_TRUE(backend.poll().ok());
  EXPECT_EQ(backend.data(), original);
  backend.set_data(oversized);
  EXPECT_EQ(backend.data(), original);
}

}  // namespace
}  // namespace lanlink::platform
