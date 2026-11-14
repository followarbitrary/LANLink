#pragma once

#include <QCloseEvent>
#include <QPoint>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QStackedWidget>
#include <QSystemTrayIcon>
#include <QThread>
#include <QVector>

#include "desktop_state.hpp"
#include "runtime_worker.hpp"

namespace lanlink::desktop {

class MainWindow final : public QMainWindow {
  Q_OBJECT

 public:
  explicit MainWindow(QWidget* parent = nullptr,
                      RuntimeWorker::BackendFactory backend_factory = {},
                      bool auto_start = true,
                      RuntimeWorker::ClipboardBackendFactory clipboard_backend_factory = {});
  MainWindow(QWidget* parent, RuntimeWorker::BackendFactory backend_factory,
             RuntimeWorker::ClipboardBackendFactory clipboard_backend_factory,
             bool auto_start = true);
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
  void onRuntimeStatusChanged(const QString& status, bool running, bool connected);
  void onPeerChanged(const QString& name, const QString& platform, const QString& address,
                     qulonglong deviceId, qulonglong displayId, bool online);
  void onRuntimeLog(const QString& message);
  void onRuntimeError(const QString& message);
  void onFileTransferChanged(const QString& name, qulonglong done, qulonglong total,
                             const QString& status, bool outgoing);

 private:
  QWidget* makePageContainer(QWidget* page);
  void setupUi();
  void setupTray();
  void startRuntime(const app::AppOptions& options);
  void stopRuntime();
  void recoverLocalInputOnWorker();
  void sendFileOnWorker(const QString& path);
  void cancelFileOnWorker();
  void rebuildPages();
  void updateRuntimeControls();
  void captureSettingsFromUi();
  void updateStatusMessage(const QString& message);
  void setWindowButtonStyle(QPushButton* button);

  DesktopState state_;
  QStackedWidget* content_{nullptr};
  QVector<QPushButton*> navButtons_;
  QLabel* statusLabel_{nullptr};
  QLabel* footerLabel_{nullptr};
  QLabel* networkLabel_{nullptr};
  QLabel* localLabel_{nullptr};
  QLabel* controlLabel_{nullptr};
  QWidget* titleBar_{nullptr};
  QSystemTrayIcon* tray_{nullptr};
  QAction* trayCrossScreen_{nullptr};
  QAction* trayClipboard_{nullptr};
  QThread* runtimeThread_{nullptr};
  RuntimeWorker* runtimeWorker_{nullptr};
  RuntimeWorker::BackendFactory backend_factory_;
  RuntimeWorker::ClipboardBackendFactory clipboard_backend_factory_;
  bool draggingWindow_{false};
  QPoint windowDragOffset_;
};

}  // namespace lanlink::desktop
