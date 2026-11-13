#pragma once

#include <QColor>
#include <QPointF>
#include <QSizeF>
#include <QString>
#include <QVector>

namespace lanlink::desktop {

struct DeviceState {
  QString name;
  QString platform;
  QString address;
  QColor accent;
  QPointF position;
  QSizeF size{220.0, 120.0};
  bool local{false};
  bool online{true};
};

struct DesktopState {
  QVector<DeviceState> devices;
  int activeDevice{0};
  int selectedTransferTarget{1};
  QString droppedFile;
  bool crossScreenEnabled{true};
  bool clipboardEnabled{true};
  bool allowRemoteKeys{true};
  bool allowReceiveFiles{true};
  bool running{true};
  double canvasZoom{1.0};

  DesktopState() {
    devices = {
        {QStringLiteral("Windows PC"), QStringLiteral("Windows 11"), QStringLiteral("192.168.1.10"),
         QColor(QStringLiteral("#6ea8ff")), QPointF(40.0, 68.0), QSizeF(220.0, 120.0), true, true},
        {QStringLiteral("MacBook Pro"), QStringLiteral("macOS"), QStringLiteral("192.168.1.11"),
         QColor(QStringLiteral("#4db69e")), QPointF(310.0, 68.0), QSizeF(220.0, 120.0), false, true},
        {QStringLiteral("Linux Workstation"), QStringLiteral("Linux"), QStringLiteral("192.168.1.12"),
         QColor(QStringLiteral("#b388ed")), QPointF(580.0, 68.0), QSizeF(220.0, 120.0), false, true},
    };
  }

  void resetLayout() {
    if (devices.size() >= 3) {
      devices[0].position = QPointF(40.0, 68.0);
      devices[1].position = QPointF(310.0, 68.0);
      devices[2].position = QPointF(580.0, 68.0);
    }
    canvasZoom = 1.0;
  }
};

}  // namespace lanlink::desktop
