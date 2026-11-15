#pragma once

#include <QColor>
#include <QPointF>
#include <QSizeF>
#include <QString>
#include <QVector>

#include <cstdint>

#include "lanlink/app/options.hpp"

namespace lanlink::desktop {

inline constexpr double kTopologyOriginX = 40.0;
inline constexpr double kTopologyOriginY = 68.0;
inline constexpr double kTopologyCardWidth = 280.0;
inline constexpr double kTopologyCardHeight = 150.0;
inline constexpr double kTopologyCardGap = 48.0;

inline QString local_platform_name() {
#if defined(__APPLE__)
  return QStringLiteral("macOS");
#elif defined(_WIN32)
  return QStringLiteral("Windows");
#else
  return QStringLiteral("Linux");
#endif
}

struct DeviceState {
  QString name;
  QString platform;
  QString address;
  QColor accent;
  QPointF position;
  QSizeF size{kTopologyCardWidth, kTopologyCardHeight};
  bool local{false};
  bool online{false};
  std::uint64_t deviceId{};
  std::uint64_t displayId{};
};

inline QPointF default_new_device_position(const QVector<DeviceState>& devices) {
  if (devices.isEmpty()) return QPointF(kTopologyOriginX, kTopologyOriginY);

  int rightmostIndex = 0;
  double rightmostEdge = devices.front().position.x() + devices.front().size.width();
  for (int index = 1; index < devices.size(); ++index) {
    const auto& device = devices[index];
    const double edge = device.position.x() + device.size.width();
    if (edge > rightmostEdge) {
      rightmostEdge = edge;
      rightmostIndex = index;
    }
  }
  return QPointF(rightmostEdge + kTopologyCardGap, devices[rightmostIndex].position.y());
}

struct DesktopState {
  app::AppOptions options;
  QVector<DeviceState> devices;
  int activeDevice{0};
  int selectedTransferTarget{0};
  QString droppedFile;
  QString fileTransferName;
  QString fileTransferStatus;
  std::uint64_t fileTransferDone{0};
  std::uint64_t fileTransferTotal{0};
  bool fileTransferOutgoing{true};
  bool crossScreenEnabled{true};
  // Clipboard synchronization is opt-in to keep local clipboard contents private by default.
  bool clipboardEnabled{false};
  bool allowRemoteKeys{true};
  bool allowReceiveFiles{false};
  bool running{false};
  bool connected{false};
  bool autoDiscoverPeer{true};
  QString status{QStringLiteral("未启动")};
  QString lastLog;
  QString error;
  QString localEndpoint;
  QString peerEndpoint;
  QString peerName;
  double canvasZoom{1.0};

  DesktopState() {
    options.device_id = 1;
    options.listen_port = 41000;
    options.group = "lanlink";
    options.discovery_port = 41001;
    options.display_id = 1;
    options.peer_display_id = 2;
    devices = {
        {QStringLiteral("本机"), local_platform_name(), QStringLiteral("未启动"),
         QColor(QStringLiteral("#6ea8ff")), QPointF(kTopologyOriginX, kTopologyOriginY),
         QSizeF(kTopologyCardWidth, kTopologyCardHeight), true,
         false, options.device_id, options.display_id},
    };
  }

  void resetLayout() {
    if (!devices.isEmpty()) devices[0].position = QPointF(kTopologyOriginX, kTopologyOriginY);
    if (devices.size() > 1) {
      devices[1].position = QPointF(kTopologyOriginX + kTopologyCardWidth + kTopologyCardGap,
                                    kTopologyOriginY);
    }
    canvasZoom = 1.0;
  }
};

}  // namespace lanlink::desktop
