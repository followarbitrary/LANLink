#include "desktop_state.hpp"

#include <gtest/gtest.h>

namespace lanlink::desktop {

TEST(DesktopStateTest, ResetLayoutRestoresDefaultCanvasState) {
  DesktopState state;
  ASSERT_EQ(state.devices.size(), 2);

  state.devices[1].position = QPointF(11.0, 22.0);
  state.canvasZoom = 1.35;

  state.resetLayout();

  EXPECT_EQ(state.devices[0].position, QPointF(40.0, 68.0));
  EXPECT_EQ(state.devices[1].position, QPointF(310.0, 68.0));
  EXPECT_DOUBLE_EQ(state.canvasZoom, 1.0);
}

}  // namespace lanlink::desktop
