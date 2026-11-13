#include "topology_canvas.hpp"

#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace lanlink::desktop {
namespace {
const QColor kCanvas(QStringLiteral("#141e2a"));
const QColor kCard(QStringLiteral("#202b39"));
const QColor kCardSelected(QStringLiteral("#2b4159"));
const QColor kBorder(QStringLiteral("#53647a"));
const QColor kText(QStringLiteral("#edf2f8"));
const QColor kMuted(QStringLiteral("#90a3b9"));
const QColor kLink(QStringLiteral("#83d3ae"));
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
  setMinimumHeight(horizontalLayout_ ? 270 : 370);
  if (state_ != nullptr && state_->devices.size() >= 3) {
    if (horizontalLayout_) {
      state_->devices[0].position = QPointF(40.0, 68.0);
      state_->devices[1].position = QPointF(310.0, 68.0);
      state_->devices[2].position = QPointF(580.0, 68.0);
    } else {
      state_->devices[0].position = QPointF(40.0, 68.0);
      state_->devices[1].position = QPointF(310.0, 68.0);
      state_->devices[2].position = QPointF(310.0, 210.0);
    }
    emit layoutChanged();
  }
  update();
}

void TopologyCanvas::resetLayout() {
  if (state_ == nullptr) return;
  state_->resetLayout();
  if (!horizontalLayout_ && state_->devices.size() >= 3) state_->devices[2].position = QPointF(310.0, 210.0);
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

  QPen linkPen(kLink, 4.0 / zoom, Qt::DashLine);
  painter.setPen(linkPen);
  for (int index = 0; index + 1 < state_->devices.size(); ++index) {
    const QRectF first = cardRect(index);
    const QRectF second = cardRect(index + 1);
    painter.drawLine(first.center().x() + first.width() / 2.0, first.center().y(),
                     second.center().x() - second.width() / 2.0, second.center().y());
  }

  for (int index = 0; index < state_->devices.size(); ++index) {
    const auto& device = state_->devices[index];
    const QRectF bounds = cardRect(index);
    painter.setPen(QPen(index == selectedIndex_ ? QColor(QStringLiteral("#8fc3ff")) : kBorder,
                        index == selectedIndex_ ? 2.5 / zoom : 1.8 / zoom));
    painter.setBrush(index == selectedIndex_ ? kCardSelected : kCard);
    painter.drawRoundedRect(bounds, 14.0, 14.0);

    painter.setPen(QPen(device.accent, 1.8 / zoom));
    painter.setBrush(QColor(device.accent.red(), device.accent.green(), device.accent.blue(), 48));
    painter.drawRoundedRect(QRectF(bounds.left() + 20.0, bounds.top() + 20.0, 68.0, 48.0), 7.0, 7.0);
    painter.drawLine(QPointF(bounds.left() + 45.0, bounds.top() + 69.0),
                     QPointF(bounds.left() + 63.0, bounds.top() + 69.0));

    painter.setPen(kText);
    QFont titleFont = painter.font();
    titleFont.setPointSizeF(15.0);
    titleFont.setWeight(QFont::DemiBold);
    painter.setFont(titleFont);
    painter.drawText(QRectF(bounds.left(), bounds.top() + 70.0, bounds.width(), 24.0), Qt::AlignCenter,
                     device.name);
    painter.setPen(kMuted);
    QFont stateFont = painter.font();
    stateFont.setPointSizeF(11.0);
    stateFont.setWeight(QFont::Normal);
    painter.setFont(stateFont);
    painter.drawText(QRectF(bounds.left(), bounds.top() + 94.0, bounds.width(), 20.0), Qt::AlignCenter,
                     device.local ? QStringLiteral("本机 · 在线") : QStringLiteral("已连接 · 在线"));
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
