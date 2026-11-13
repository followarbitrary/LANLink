#pragma once

#include <QColor>
#include <QPointF>
#include <QSizeF>
#include <QString>
#include <QVector>

#include <cstdint>

#include "lanlink/app/options.hpp"

namespace lanlink::desktop {

struct DeviceState {
  QString name;
  QString platform;
  QString address;
  QColor accent;
  QPointF position;
  QSizeF size{220.0, 120.0};
  bool local{false};
  bool online{false};
  std::uint64_t deviceId{};
  std::uint64_t displayId{};
};

struct DesktopState {
  app::AppOptions options;
  QVector<DeviceState> devices;
  int activeDevice{0};
  int selectedTransferTarget{1};
  QString droppedFile;
  bool crossScreenEnabled{true};
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
        {QStringLiteral("本机"), QStringLiteral("macOS / Windows"), QStringLiteral("未启动"),
         QColor(QStringLiteral("#6ea8ff")), QPointF(40.0, 68.0), QSizeF(220.0, 120.0), true,
         false, options.device_id, options.display_id},
        {QStringLiteral("等待对端设备"), QStringLiteral("LANLink"), QStringLiteral("自动发现中"),
         QColor(QStringLiteral("#4db69e")), QPointF(310.0, 68.0), QSizeF(220.0, 120.0), false,
         false, 0, options.peer_display_id},
    };
  }

  void resetLayout() {
    if (!devices.isEmpty()) devices[0].position = QPointF(40.0, 68.0);
    if (devices.size() > 1) devices[1].position = QPointF(310.0, 68.0);
    canvasZoom = 1.0;
  }
};

}  // namespace lanlink::desktop
