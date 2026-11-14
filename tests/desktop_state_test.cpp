#include "desktop_state.hpp"

#include <gtest/gtest.h>

namespace lanlink::desktop {

TEST(DesktopStateTest, ResetLayoutRestoresDefaultCanvasState) {
  DesktopState state;
  ASSERT_EQ(state.devices.size(), 1);

  state.canvasZoom = 1.35;

  state.resetLayout();

  EXPECT_EQ(state.devices[0].position, QPointF(40.0, 68.0));
  EXPECT_DOUBLE_EQ(state.canvasZoom, 1.0);
}

TEST(DesktopStateTest, UsesActualLocalPlatformLabel) {
  DesktopState state;

  ASSERT_FALSE(state.devices.isEmpty());
#if defined(__APPLE__)
  EXPECT_EQ(state.devices[0].platform, QStringLiteral("macOS"));
#elif defined(_WIN32)
  EXPECT_EQ(state.devices[0].platform, QStringLiteral("Windows"));
#else
  EXPECT_EQ(state.devices[0].platform, QStringLiteral("Linux"));
#endif
}

}  // namespace lanlink::desktop
