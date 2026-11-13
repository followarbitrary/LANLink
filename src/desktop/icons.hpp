#pragma once

#include <QIcon>
#include <QPainter>
#include <QPixmap>

#include <cmath>

namespace lanlink::desktop {

enum class IconKind { Grid, Monitor, Laptop, Upload, Gear, ArrowUp, Pointer };

inline QPixmap iconPixmap(IconKind kind, const QSize& size, const QColor& color) {
  QPixmap pixmap(size);
  pixmap.fill(Qt::transparent);
  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setPen(QPen(color, 2.0));
  painter.setBrush(Qt::NoBrush);
  const QRectF bounds = QRectF(3.0, 3.0, size.width() - 6.0, size.height() - 6.0);

  switch (kind) {
    case IconKind::Grid: {
      const double cell = (bounds.width() - 4.0) / 2.0;
      painter.drawRoundedRect(QRectF(bounds.left(), bounds.top(), cell, cell), 1.5, 1.5);
      painter.drawRoundedRect(QRectF(bounds.left() + cell + 4.0, bounds.top(), cell, cell), 1.5, 1.5);
      painter.drawRoundedRect(QRectF(bounds.left(), bounds.top() + cell + 4.0, cell, cell), 1.5, 1.5);
      painter.drawRoundedRect(QRectF(bounds.left() + cell + 4.0, bounds.top() + cell + 4.0, cell, cell), 1.5, 1.5);
      break;
    }
    case IconKind::Monitor:
      painter.drawRoundedRect(QRectF(bounds.left(), bounds.top(), bounds.width(), bounds.height() - 5.0), 2.5, 2.5);
      painter.drawLine(QPointF(size.width() / 2.0, bounds.bottom() - 5.0),
                       QPointF(size.width() / 2.0, bounds.bottom()));
      painter.drawLine(QPointF(bounds.left() + 7.0, bounds.bottom()), QPointF(bounds.right() - 7.0, bounds.bottom()));
      break;
    case IconKind::Laptop:
      painter.drawRoundedRect(QRectF(bounds.left() + 2.0, bounds.top(), bounds.width() - 4.0, bounds.height() - 7.0), 2.5,
                              2.5);
      painter.drawLine(QPointF(bounds.left(), bounds.bottom() - 4.0), QPointF(bounds.right(), bounds.bottom() - 4.0));
      painter.drawLine(QPointF(bounds.left() + 3.0, bounds.bottom()), QPointF(bounds.right() - 3.0, bounds.bottom()));
      break;
    case IconKind::Upload:
      painter.drawRoundedRect(QRectF(bounds.left() + 2.0, bounds.top() + 7.0, bounds.width() - 4.0, bounds.height() - 7.0),
                              2.5, 2.5);
      painter.drawLine(QPointF(size.width() / 2.0, bounds.top()), QPointF(size.width() / 2.0, bounds.top() + 12.0));
      painter.drawLine(QPointF(size.width() / 2.0, bounds.top()),
                       QPointF(size.width() / 2.0 - 5.0, bounds.top() + 6.0));
      painter.drawLine(QPointF(size.width() / 2.0, bounds.top()),
                       QPointF(size.width() / 2.0 + 5.0, bounds.top() + 6.0));
      break;
    case IconKind::Gear:
      {
        QPolygonF teeth;
        const QPointF center(size.width() / 2.0, size.height() / 2.0);
        for (int index = 0; index < 16; ++index) {
          const double angle = index * 22.5 * 3.141592653589793 / 180.0;
          const double radius = index % 2 == 0 ? 10.5 : 8.0;
          teeth << center + QPointF(std::cos(angle) * radius, std::sin(angle) * radius);
        }
        painter.drawPolygon(teeth);
        painter.drawEllipse(center, 3.5, 3.5);
      }
      break;
    case IconKind::ArrowUp:
      painter.drawLine(QPointF(size.width() / 2.0, bounds.top()), QPointF(size.width() / 2.0, bounds.bottom()));
      painter.drawLine(QPointF(size.width() / 2.0, bounds.top()),
                       QPointF(size.width() / 2.0 - 6.0, bounds.top() + 7.0));
      painter.drawLine(QPointF(size.width() / 2.0, bounds.top()),
                       QPointF(size.width() / 2.0 + 6.0, bounds.top() + 7.0));
      break;
    case IconKind::Pointer: {
      QPolygonF pointer;
      pointer << QPointF(bounds.left() + 4.0, bounds.top() + 2.0)
              << QPointF(bounds.left() + 10.0, bounds.bottom() - 2.0)
              << QPointF(bounds.left() + 13.0, bounds.bottom() - 9.0)
              << QPointF(bounds.right() - 3.0, bounds.bottom() - 6.0)
              << QPointF(bounds.left() + 4.0, bounds.top() + 2.0);
      painter.drawPolyline(pointer);
      break;
    }
  }
  return pixmap;
}

inline QIcon icon(IconKind kind, const QColor& color, const QSize& size = QSize(22, 22)) {
  return QIcon(iconPixmap(kind, size, color));
}

}  // namespace lanlink::desktop
