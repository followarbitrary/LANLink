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

TEST(DesktopStateTest, UsesCompactTopologyCardAndRightSideSpacing) {
  DesktopState state;
  state.devices.append(DeviceState{
      QStringLiteral("设备 2"),
      QStringLiteral("macOS"),
      QStringLiteral("192.168.1.2:41000"),
      QColor(QStringLiteral("#4db69e")),
      QPointF(0.0, 0.0),
      QSizeF(220.0, 120.0),
      false,
      true,
      2,
      2,
  });

  state.resetLayout();

  ASSERT_EQ(state.devices.size(), 2);
  EXPECT_EQ(state.devices[0].size, QSizeF(280.0, 120.0));
  EXPECT_GT(state.devices[1].position.x(),
            state.devices[0].position.x() + state.devices[0].size.width());
  EXPECT_EQ(state.devices[1].position, QPointF(368.0, 68.0));
}

TEST(DesktopStateTest, PlacesNewDeviceToRightOfCurrentRightmostDevice) {
  DesktopState state;
  state.devices.append(DeviceState{
      QStringLiteral("设备 2"),
      QStringLiteral("macOS"),
      QStringLiteral("192.168.1.2:41000"),
      QColor(QStringLiteral("#4db69e")),
      QPointF(640.0, 212.0),
      QSizeF(kTopologyCardWidth, kTopologyCardHeight),
      false,
      true,
      2,
      2,
  });

  EXPECT_EQ(default_new_device_position(state.devices), QPointF(968.0, 212.0));
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
