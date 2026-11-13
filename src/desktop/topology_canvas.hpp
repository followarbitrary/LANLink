#pragma once

#include <QPointF>
#include <QWidget>

#include "desktop_state.hpp"

namespace lanlink::desktop {

class TopologyCanvas final : public QWidget {
  Q_OBJECT

 public:
  explicit TopologyCanvas(QWidget* parent = nullptr);

  void setState(DesktopState* state);
  void setLayoutMode(bool horizontal);
  void resetLayout();
  QSize minimumSizeHint() const override { return QSize(600, 270); }

 signals:
  void layoutChanged();
  void selectionChanged(int index);

 protected:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;

 private:
  QPointF toScene(const QPointF& viewPoint) const;
  double renderZoom() const;
  QRectF cardRect(int index) const;
  int hitTest(const QPointF& scenePoint) const;
  void snapCard(int index);

  DesktopState* state_{nullptr};
  int selectedIndex_{0};
  int draggingIndex_{-1};
  QPointF dragOffset_;
  QPointF lastMousePosition_;
  QPointF pan_{0.0, 0.0};
  bool panning_{false};
  bool horizontalLayout_{true};
};

}  // namespace lanlink::desktop
