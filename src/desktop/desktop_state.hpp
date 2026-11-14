#pragma once

#include <QColor>
#include <QPointF>
#include <QSizeF>
#include <QString>
#include <QVector>

#include <cstdint>

#include "lanlink/app/options.hpp"

namespace lanlink::desktop {

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
  int selectedTransferTarget{0};
  QString droppedFile;
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
         QColor(QStringLiteral("#6ea8ff")), QPointF(40.0, 68.0), QSizeF(220.0, 120.0), true,
         false, options.device_id, options.display_id},
    };
  }

  void resetLayout() {
    if (!devices.isEmpty()) devices[0].position = QPointF(40.0, 68.0);
    if (devices.size() > 1) devices[1].position = QPointF(310.0, 68.0);
    canvasZoom = 1.0;
  }
};

}  // namespace lanlink::desktop
