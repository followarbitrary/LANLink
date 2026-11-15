#include "topology_canvas.hpp"

#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace lanlink::desktop {
namespace {
const QColor kCanvas(QStringLiteral("#141e2a"));
const QColor kCard(QStringLiteral("#1b2d3d"));
const QColor kCardSelected(QStringLiteral("#24445e"));
const QColor kBorder(QStringLiteral("#42607a"));
const QColor kText(QStringLiteral("#edf2f8"));
const QColor kMuted(QStringLiteral("#90a3b9"));
const QColor kLink(QStringLiteral("#52d1b0"));
const QColor kOnline(QStringLiteral("#52e0a8"));
}

TopologyCanvas::TopologyCanvas(QWidget* parent) : QWidget(parent) {
  setMouseTracking(true);
  setFocusPolicy(Qt::StrongFocus);
  setMinimumHeight(270);
}

void TopologyCanvas::setState(DesktopState* state) {
  state_ = state;
  if (state_ != nullptr) state_->canvasZoom = std::clamp(state_->canvasZoom, 0.65, 1.4);
  update();
}

void TopologyCanvas::setLayoutMode(bool horizontal) {
  horizontalLayout_ = horizontal;
  setMinimumHeight(horizontalLayout_ ? 270 : 440);
  if (state_ != nullptr && state_->devices.size() >= 2) {
    for (int index = 0; index < state_->devices.size(); ++index) {
      const double offset = static_cast<double>(index);
      state_->devices[index].position = horizontalLayout_
                                            ? QPointF(kTopologyOriginX +
                                                          offset * (kTopologyCardWidth +
                                                                    kTopologyCardGap),
                                                      kTopologyOriginY)
                                            : QPointF(kTopologyOriginX,
                                                      kTopologyOriginY +
                                                          offset * (kTopologyCardHeight +
                                                                    kTopologyCardGap));
    }
    emit layoutChanged();
  }
  update();
}

void TopologyCanvas::resetLayout() {
  if (state_ == nullptr) return;
  state_->resetLayout();
  if (state_->devices.size() >= 2) {
    for (int index = 1; index < state_->devices.size(); ++index) {
      const double offset = static_cast<double>(index);
      state_->devices[index].position = horizontalLayout_
                                            ? QPointF(kTopologyOriginX +
                                                          offset * (kTopologyCardWidth +
                                                                    kTopologyCardGap),
                                                      kTopologyOriginY)
                                            : QPointF(kTopologyOriginX,
                                                      kTopologyOriginY +
                                                          offset * (kTopologyCardHeight +
                                                                    kTopologyCardGap));
    }
  }
  pan_ = QPointF(0.0, 0.0);
  emit layoutChanged();
  update();
}

QPointF TopologyCanvas::toScene(const QPointF& viewPoint) const {
  const double zoom = renderZoom();
  return (viewPoint - QPointF(16.0, 18.0) - pan_) / zoom;
}

double TopologyCanvas::renderZoom() const {
  if (state_ == nullptr) return 1.0;
  const double fitZoom = std::clamp((static_cast<double>(width()) - 32.0) / 800.0, 0.65, 1.0);
  return state_->canvasZoom * fitZoom;
}

QRectF TopologyCanvas::cardRect(int index) const {
  if (state_ == nullptr || index < 0 || index >= state_->devices.size()) return {};
  const auto& device = state_->devices[index];
  return QRectF(device.position, device.size);
}

int TopologyCanvas::hitTest(const QPointF& scenePoint) const {
  if (state_ == nullptr) return -1;
  for (int index = state_->devices.size() - 1; index >= 0; --index) {
    if (cardRect(index).contains(scenePoint)) return index;
  }
  return -1;
}

void TopologyCanvas::snapCard(int index) {
  if (state_ == nullptr || index < 0 || index >= state_->devices.size()) return;
  auto& card = state_->devices[index];
  constexpr double kSnapDistance = 18.0;
  for (int otherIndex = 0; otherIndex < state_->devices.size(); ++otherIndex) {
    if (otherIndex == index) continue;
    const QRectF other = cardRect(otherIndex);
    QRectF current = cardRect(index);
    if (std::abs(current.left() - other.right()) <= kSnapDistance &&
        std::abs(current.top() - other.top()) <= kSnapDistance) {
      card.position = QPointF(other.right(), other.top());
      return;
    }
    if (std::abs(current.right() - other.left()) <= kSnapDistance &&
        std::abs(current.top() - other.top()) <= kSnapDistance) {
      card.position = QPointF(other.left() - current.width(), other.top());
      return;
    }
    if (std::abs(current.top() - other.bottom()) <= kSnapDistance &&
        std::abs(current.left() - other.left()) <= kSnapDistance) {
      card.position = QPointF(other.left(), other.bottom());
      return;
    }
    if (std::abs(current.bottom() - other.top()) <= kSnapDistance &&
        std::abs(current.left() - other.left()) <= kSnapDistance) {
      card.position = QPointF(other.left(), other.top() - current.height());
      return;
    }
  }
}

void TopologyCanvas::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.fillRect(rect(), kCanvas);
  if (state_ == nullptr) return;

  const double zoom = renderZoom();
  painter.translate(16.0 + pan_.x(), 18.0 + pan_.y());
  painter.scale(zoom, zoom);

  QPen linkPen(kLink, 2.2 / zoom, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
  painter.setPen(linkPen);
  for (int index = 0; index + 1 < state_->devices.size(); ++index) {
    const QRectF first = cardRect(index);
    const QRectF second = cardRect(index + 1);
    const bool horizontal = std::abs(first.center().x() - second.center().x()) >=
                            std::abs(first.center().y() - second.center().y());
    const QPointF start = horizontal
                              ? QPointF(second.center().x() >= first.center().x() ? first.right()
                                                                                    : first.left(),
                                        first.center().y())
                              : QPointF(first.center().x(),
                                        second.center().y() >= first.center().y() ? first.bottom()
                                                                                    : first.top());
    const QPointF end = horizontal
                            ? QPointF(second.center().x() >= first.center().x() ? second.left()
                                                                                  : second.right(),
                                      second.center().y())
                            : QPointF(second.center().x(),
                                      second.center().y() >= first.center().y() ? second.top()
                                                                                  : second.bottom());
    painter.drawLine(start, end);
  }

  for (int index = 0; index < state_->devices.size(); ++index) {
    const auto& device = state_->devices[index];
    const QRectF bounds = cardRect(index);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 42));
    painter.drawRoundedRect(bounds.translated(0.0, 4.0), 17.0, 17.0);

    painter.setPen(QPen(index == selectedIndex_ ? QColor(QStringLiteral("#8fc3ff")) : kBorder,
                        index == selectedIndex_ ? 2.2 / zoom : 1.4 / zoom));
    painter.setBrush(index == selectedIndex_ ? kCardSelected : kCard);
    painter.drawRoundedRect(bounds, 17.0, 17.0);

    painter.setPen(Qt::NoPen);
    painter.setBrush(device.accent);
    painter.drawRoundedRect(QRectF(bounds.left() + 11.0, bounds.top() + 14.0, 5.0,
                                   bounds.height() - 28.0), 2.5, 2.5);

    const QRectF iconTile(bounds.left() + 28.0, bounds.top() + 28.0, 64.0, 64.0);
    painter.setBrush(QColor(device.accent.red(), device.accent.green(), device.accent.blue(), 42));
    painter.drawRoundedRect(iconTile, 14.0, 14.0);
    painter.setPen(QPen(device.accent, 2.0 / zoom));
    const QRectF monitor(iconTile.left() + 17.0, iconTile.top() + 16.0, 30.0, 21.0);
    painter.drawRoundedRect(monitor, 4.0, 4.0);
    painter.drawLine(QPointF(iconTile.center().x(), monitor.bottom() + 1.0),
                     QPointF(iconTile.center().x(), iconTile.top() + 46.0));
    painter.drawLine(QPointF(iconTile.center().x() - 9.0, iconTile.top() + 47.0),
                     QPointF(iconTile.center().x() + 9.0, iconTile.top() + 47.0));

    const double textLeft = bounds.left() + 112.0;
    const double textWidth = std::max(0.0, bounds.right() - textLeft - 18.0);
    painter.setPen(kText);
    QFont titleFont = painter.font();
    titleFont.setPointSizeF(16.0);
    titleFont.setWeight(QFont::DemiBold);
    painter.setFont(titleFont);
    const QString title = painter.fontMetrics().elidedText(device.name, Qt::ElideRight,
                                                            static_cast<int>(textWidth));
    painter.drawText(QRectF(textLeft, bounds.top() + 30.0, textWidth, 26.0),
                     Qt::AlignLeft | Qt::AlignVCenter, title);

    const bool online = device.online;
    const QString status = device.local
                               ? (online ? QStringLiteral("本机 · 在线")
                                         : QStringLiteral("本机 · 未启动"))
                               : (online ? QStringLiteral("已连接 · 在线")
                                         : QStringLiteral("已断开"));
    painter.setPen(Qt::NoPen);
    painter.setBrush(online ? kOnline : QColor(QStringLiteral("#71869b")));
    painter.drawEllipse(QPointF(textLeft + 5.0, bounds.top() + 78.0), 4.5, 4.5);

    painter.setPen(kMuted);
    QFont stateFont = painter.font();
    stateFont.setPointSizeF(11.5);
    stateFont.setWeight(QFont::Normal);
    painter.setFont(stateFont);
    painter.drawText(QRectF(textLeft + 18.0, bounds.top() + 66.0, textWidth - 18.0, 26.0),
                     Qt::AlignLeft | Qt::AlignVCenter, status);
  }
}

void TopologyCanvas::mousePressEvent(QMouseEvent* event) {
  lastMousePosition_ = event->position();
  if (event->button() == Qt::MiddleButton) {
    panning_ = true;
    setCursor(Qt::ClosedHandCursor);
    return;
  }
  if (event->button() != Qt::LeftButton) return;
  const QPointF scenePoint = toScene(event->position());
  const int hit = hitTest(scenePoint);
  if (hit < 0) return;
  selectedIndex_ = hit;
  draggingIndex_ = hit;
  dragOffset_ = scenePoint - state_->devices[hit].position;
  emit selectionChanged(hit);
  update();
}

void TopologyCanvas::mouseMoveEvent(QMouseEvent* event) {
  const QPointF current = event->position();
  const QPointF delta = current - lastMousePosition_;
  lastMousePosition_ = current;
  if (panning_) {
    pan_ += delta;
    update();
    return;
  }
  if (draggingIndex_ < 0 || state_ == nullptr) return;
  state_->devices[draggingIndex_].position = toScene(current) - dragOffset_;
  update();
}

void TopologyCanvas::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() == Qt::MiddleButton) {
    panning_ = false;
    unsetCursor();
    return;
  }
  if (event->button() != Qt::LeftButton || draggingIndex_ < 0) return;
  snapCard(draggingIndex_);
  draggingIndex_ = -1;
  emit layoutChanged();
  update();
}

void TopologyCanvas::wheelEvent(QWheelEvent* event) {
  if (state_ == nullptr) return;
  const double nextZoom = state_->canvasZoom + (event->angleDelta().y() > 0 ? 0.1 : -0.1);
  state_->canvasZoom = std::clamp(nextZoom, 0.65, 1.4);
  update();
  event->accept();
}

}  // namespace lanlink::desktop
