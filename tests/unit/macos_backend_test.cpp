#include "lanlink/platform/macos_input_backend.hpp"

#include <gtest/gtest.h>

namespace lanlink::platform {
namespace {

TEST(MacOSInputBackend, NonAppleBuildReportsUnsupported) {
#if !defined(__APPLE__)
  MacOSInputBackend backend;
  EXPECT_FALSE(backend.display_bounds().has_value());
  EXPECT_FALSE(backend.start({}).ok());
  EXPECT_NE(backend.status().last_error.find("unsupported"), std::string::npos);
  EXPECT_FALSE(backend.set_suppression(true).ok());
  EXPECT_FALSE(backend.inject(InputEvent{}).ok());
  EXPECT_FALSE(backend.recover().ok());
#else
  GTEST_SKIP() << "fallback behavior is only asserted on non-Apple";
#endif
}

TEST(MacOSInputBackend, StopAndReleaseAreIdempotent) {
  MacOSInputBackend backend;
  backend.stop();
  backend.stop();
  backend.release_injected_state(SessionId{1, 2});
  backend.release_injected_state(SessionId{1, 2});
  EXPECT_FALSE(backend.status().running);
  EXPECT_FALSE(backend.status().suppression);
}

TEST(MacOSInputBackend, StatusIsAvailableWithoutStartingCapture) {
  MacOSInputBackend backend;
  EXPECT_FALSE(backend.status().running);
  EXPECT_FALSE(backend.status().suppression);
}

}  // namespace
}  // namespace lanlink::platform
