#pragma once

#include <QCloseEvent>
#include <QPoint>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QStackedWidget>
#include <QSystemTrayIcon>
#include <QVector>

#include "desktop_state.hpp"

namespace lanlink::desktop {

class MainWindow final : public QMainWindow {
  Q_OBJECT

 public:
  explicit MainWindow(QWidget* parent = nullptr);
  ~MainWindow() override;

  void showPage(int index);
  void recoverLocalInput();

 protected:
  void closeEvent(QCloseEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) override;

 private slots:
  void toggleCrossScreen(bool enabled);
  void toggleClipboard(bool enabled);
  void lockCurrentDevice();
  void quitFromTray();

 private:
  QWidget* makePageContainer(QWidget* page);
  void setupUi();
  void setupTray();
  void updateStatusMessage(const QString& message);
  void setWindowButtonStyle(QPushButton* button);

  DesktopState state_;
  QStackedWidget* content_{nullptr};
  QVector<QPushButton*> navButtons_;
  QLabel* statusLabel_{nullptr};
  QLabel* footerLabel_{nullptr};
  QLabel* controlLabel_{nullptr};
  QWidget* titleBar_{nullptr};
  QSystemTrayIcon* tray_{nullptr};
  QAction* trayCrossScreen_{nullptr};
  QAction* trayClipboard_{nullptr};
  bool draggingWindow_{false};
  QPoint windowDragOffset_;
};

}  // namespace lanlink::desktop
