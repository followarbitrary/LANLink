#include "main_window.hpp"

#include "pages.hpp"
#include "icons.hpp"
#include "topology_canvas.hpp"

#include <QAction>
#if defined(LANLINK_HAS_QT_NETWORK)
#include <QAbstractSocket>
#include <QNetworkInterface>
#endif
#include <QApplication>
#include <QCheckBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMouseEvent>
#include <QMenu>
#include <QMetaObject>
#include <QSizeGrip>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>
#include <optional>
#include <utility>

namespace lanlink::desktop {
namespace {

const QString kWindowStyleBase = QStringLiteral(
    "QMainWindow { background: #1e2935; }"
    "QWidget#titleBar { background: #18222e; }"
    "QWidget#nav { background: #151d28; }"
    "QWidget#footer { background: #151d28; }"
    "QLabel { color: #e9eff6; background: transparent; }"
    "QLabel#muted { color: #93a5ba; }"
    "QPushButton#navButton { color: #9caec2; background: transparent; border: 0; border-radius: 10px; text-align: left; padding: 12px 14px; font-size: 16px; }"
    "QPushButton#navButton:checked { color: #f0f5fb; background: #293f58; }"
    "QStackedWidget#content { background: #1e2935; }"
    "QScrollArea#pageScroll { border: 0; background: #1e2935; }"
    "QScrollBar:vertical { background: #1e2935; width: 8px; }"
    "QScrollBar::handle:vertical { background: #40556d; border-radius: 4px; }");

#if defined(Q_OS_MACOS)
const QString kWindowControlStyle = QStringLiteral(
    "QPushButton[windowControl=\"true\"] { color: transparent; border: 0; border-radius: 8px; padding: 0; font-size: 9px; font-weight: 700; }"
    "QPushButton[windowControl=\"true\"]:hover { color: #18222e; }"
    "QPushButton#windowCloseButton { background: #ff5f57; border: 1px solid #e34f49; }"
    "QPushButton#windowMinimizeButton { background: #ffbd2e; border: 1px solid #e0a522; }"
    "QPushButton#windowMaximizeButton { background: #28c840; border: 1px solid #20aa35; }"
    "QPushButton#windowCloseButton:hover { color: #6e1611; background: #ff6b63; }"
    "QPushButton#windowMinimizeButton:hover { color: #72550d; background: #ffc94a; }"
    "QPushButton#windowMaximizeButton:hover { color: #12612c; background: #42d85a; }");
#else
const QString kWindowControlStyle = QStringLiteral(
    "QPushButton[windowControl=\"true\"] { color: #a6b7ca; background: transparent; border: 0; border-radius: 8px; font-size: 16px; padding: 0; }"
    "QPushButton[windowControl=\"true\"]:hover { color: #f0f5fb; background: #263646; }"
    "QPushButton#windowCloseButton:hover { color: #ffffff; background: #b94d59; }");
#endif

const QString kWindowStyle = kWindowStyleBase + kWindowControlStyle;

QPushButton* navButton(const QString& text, IconKind kind) {
  auto* button = new QPushButton(text);
  button->setObjectName(QStringLiteral("navButton"));
  button->setIcon(icon(kind, QColor(QStringLiteral("#9bb1c7"))));
  button->setIconSize(QSize(20, 20));
  button->setCheckable(true);
  button->setCursor(Qt::PointingHandCursor);
  button->setMinimumHeight(48);
  return button;
}

QString local_endpoint_for_port(std::uint16_t port) {
#if defined(LANLINK_HAS_QT_NETWORK)
  QString linkLocalAddress;
  for (const auto& interface : QNetworkInterface::allInterfaces()) {
    const auto flags = interface.flags();
    if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning) ||
        (flags & QNetworkInterface::IsLoopBack)) {
      continue;
    }
    for (const auto& entry : interface.addressEntries()) {
      const auto address = entry.ip();
      if (address.protocol() != QAbstractSocket::IPv4Protocol || address.isLoopback()) continue;
      const auto text = address.toString();
      if (text.startsWith(QStringLiteral("169.254."))) {
        if (linkLocalAddress.isEmpty()) linkLocalAddress = text;
        continue;
      }
      return QStringLiteral("%1:%2").arg(text).arg(port);
    }
  }
  if (!linkLocalAddress.isEmpty()) return QStringLiteral("%1:%2").arg(linkLocalAddress).arg(port);
#endif
  return QStringLiteral("0.0.0.0:%1").arg(port);
}

std::optional<app::RemoteSide> remote_side_from_topology(const QVector<DeviceState>& devices) {
  if (devices.size() < 2) return std::nullopt;
  const auto& local = devices[0];
  const auto& peer = devices[1];
  const QPointF localCenter = local.position + QPointF(local.size.width() / 2.0, local.size.height() / 2.0);
  const QPointF peerCenter = peer.position + QPointF(peer.size.width() / 2.0, peer.size.height() / 2.0);
  const double dx = peerCenter.x() - localCenter.x();
  const double dy = peerCenter.y() - localCenter.y();
  if (std::abs(dx) < std::abs(dy) || std::abs(dx) < 0.001) return std::nullopt;
  return dx < 0.0 ? app::RemoteSide::kLeft : app::RemoteSide::kRight;
}

QString remote_side_text(app::RemoteSide side) {
  return side == app::RemoteSide::kLeft ? QStringLiteral("由拓扑图决定 · 当前在左侧")
                                        : QStringLiteral("由拓扑图决定 · 当前在右侧");
}

}  // namespace

MainWindow::MainWindow(QWidget* parent, RuntimeWorker::BackendFactory backend_factory,
                       bool auto_start,
                       RuntimeWorker::ClipboardBackendFactory clipboard_backend_factory)
    : QMainWindow(parent),
      backend_factory_(std::move(backend_factory)),
      clipboard_backend_factory_(std::move(clipboard_backend_factory)) {
  setWindowTitle(QStringLiteral("LANLink"));
  setWindowIcon(QIcon(QStringLiteral(":/icons/lanlink-icon.png")));
  setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
  setAttribute(Qt::WA_TranslucentBackground, false);
  setMinimumSize(820, 560);
  resize(1080, 700);
  setStyleSheet(kWindowStyle);
  setupUi();
  setupTray();
  if (auto_start) {
    QTimer::singleShot(0, this, [this] {
      captureSettingsFromUi();
      startRuntime(state_.options);
    });
  }
}

MainWindow::MainWindow(QWidget* parent, RuntimeWorker::BackendFactory backend_factory,
                       RuntimeWorker::ClipboardBackendFactory clipboard_backend_factory,
                       bool auto_start)
    : MainWindow(parent, std::move(backend_factory), auto_start,
                 std::move(clipboard_backend_factory)) {}

MainWindow::~MainWindow() { stopRuntime(); }

void MainWindow::setupUi() {
  auto* root = new QWidget;
  auto* rootLayout = new QVBoxLayout(root);
  rootLayout->setContentsMargins(0, 0, 0, 0);
  rootLayout->setSpacing(0);

  auto* titleBar = new QWidget;
  titleBar_ = titleBar;
  titleBar_->installEventFilter(this);
  titleBar->setObjectName(QStringLiteral("titleBar"));
  titleBar->setFixedHeight(58);
  auto* titleLayout = new QHBoxLayout(titleBar);
#if defined(Q_OS_MACOS)
  titleLayout->setContentsMargins(14, 0, 14, 0);
  titleLayout->setSpacing(8);
#else
  titleLayout->setContentsMargins(22, 0, 14, 0);
  titleLayout->setSpacing(10);
#endif

  enum class WindowControlRole { Minimize, Maximize, Close };
  auto* windowControls = new QWidget(titleBar);
  windowControls->setObjectName(QStringLiteral("windowControls"));
  auto* windowControlsLayout = new QHBoxLayout(windowControls);
  windowControlsLayout->setContentsMargins(0, 0, 0, 0);
#if defined(Q_OS_MACOS)
  windowControlsLayout->setSpacing(8);
#else
  windowControlsLayout->setSpacing(2);
#endif
  const auto addWindowControl = [this, windowControlsLayout](const QString& text,
                                                               const QString& objectName,
                                                               WindowControlRole role) {
    auto* button = new QPushButton(text);
    button->setObjectName(objectName);
    button->setProperty("windowControl", true);
    if (role == WindowControlRole::Minimize) {
      button->setAccessibleName(QStringLiteral("最小化窗口"));
    } else if (role == WindowControlRole::Maximize) {
      button->setAccessibleName(QStringLiteral("最大化窗口"));
    } else {
      button->setAccessibleName(QStringLiteral("关闭窗口"));
    }
#if defined(Q_OS_MACOS)
    button->setFixedSize(16, 16);
#else
    button->setFixedSize(34, 34);
#endif
    setWindowButtonStyle(button);
    windowControlsLayout->addWidget(button);
    if (role == WindowControlRole::Minimize) {
      connect(button, &QPushButton::clicked, this, &QWidget::showMinimized);
    } else if (role == WindowControlRole::Maximize) {
      connect(button, &QPushButton::clicked, this, [this]() {
        isMaximized() ? showNormal() : showMaximized();
      });
    } else {
      connect(button, &QPushButton::clicked, this, &QWidget::close);
    }
  };

#if defined(Q_OS_MACOS)
  addWindowControl(QStringLiteral("×"), QStringLiteral("windowCloseButton"),
                   WindowControlRole::Close);
  addWindowControl(QStringLiteral("−"), QStringLiteral("windowMinimizeButton"),
                   WindowControlRole::Minimize);
  addWindowControl(QStringLiteral("↗"), QStringLiteral("windowMaximizeButton"),
                   WindowControlRole::Maximize);
  titleLayout->addWidget(windowControls);
  titleLayout->addSpacing(12);
#else
  addWindowControl(QStringLiteral("—"), QStringLiteral("windowMinimizeButton"),
                   WindowControlRole::Minimize);
  addWindowControl(QStringLiteral("□"), QStringLiteral("windowMaximizeButton"),
                   WindowControlRole::Maximize);
  addWindowControl(QStringLiteral("×"), QStringLiteral("windowCloseButton"),
                   WindowControlRole::Close);
#endif
  auto* logo = new QLabel;
  logo->setObjectName(QStringLiteral("brandIcon"));
  logo->setFixedSize(32, 32);
  logo->setAlignment(Qt::AlignCenter);
  logo->setPixmap(QPixmap(QStringLiteral(":/icons/lanlink-icon.png"))
                      .scaled(30, 30, Qt::KeepAspectRatio, Qt::SmoothTransformation));
  titleLayout->addWidget(logo);
  auto* appName = new QLabel(QStringLiteral("LANLink"));
  appName->setStyleSheet(QStringLiteral("font-size: 24px; font-weight: 600;"));
  titleLayout->addWidget(appName);
  titleLayout->addStretch();
  auto* running = new QLabel(QStringLiteral("●  未运行"));
  statusLabel_ = running;
  running->setObjectName(QStringLiteral("runtimeHeaderStatus"));
  running->setAlignment(Qt::AlignCenter);
  running->setMinimumWidth(118);
  running->setFixedHeight(36);
  running->setStyleSheet(QStringLiteral(
      "color: #f2c77d; background: #3f3524; border: 1px solid #705e3c; border-radius: 17px; padding: 6px 14px; font-size: 14px;"));
  titleLayout->addWidget(running);
  controlLabel_ = new QLabel(QStringLiteral("当前控制：本机输入"));
  controlLabel_->setStyleSheet(QStringLiteral("color: #a6b7ca; font-size: 13px;"));
  titleLayout->addWidget(controlLabel_);
#if !defined(Q_OS_MACOS)
  // Windows and Linux place minimize, maximize, close at the trailing edge.
  titleLayout->addWidget(windowControls);
#endif
  rootLayout->addWidget(titleBar);

  auto* body = new QWidget;
  auto* bodyLayout = new QHBoxLayout(body);
  bodyLayout->setContentsMargins(0, 0, 0, 0);
  bodyLayout->setSpacing(0);
  auto* nav = new QWidget;
  nav->setObjectName(QStringLiteral("nav"));
  nav->setFixedWidth(220);
  auto* navLayout = new QVBoxLayout(nav);
  navLayout->setContentsMargins(14, 20, 14, 16);
  navLayout->setSpacing(8);
  auto* workspace = new QLabel(QStringLiteral("工作空间"));
  workspace->setStyleSheet(QStringLiteral("color: #91a3b8; font-size: 14px; padding: 0 12px 10px;"));
  navLayout->addWidget(workspace);
  const QVector<QPair<QString, IconKind>> names = {
      {QStringLiteral("概览"), IconKind::Grid},
      {QStringLiteral("屏幕布局"), IconKind::Monitor},
      {QStringLiteral("设备管理"), IconKind::Laptop},
      {QStringLiteral("文件传输"), IconKind::Upload},
      {QStringLiteral("设置"), IconKind::Gear}};
  for (const auto& [name, kind] : names) {
    auto* button = navButton(name, kind);
    navButtons_.append(button);
    navLayout->addWidget(button);
    connect(button, &QPushButton::clicked, this, [this, button]() {
      showPage(navButtons_.indexOf(button));
    });
  }
  navLayout->addStretch();
  auto* divider = new QFrame;
  divider->setFrameShape(QFrame::HLine);
  divider->setStyleSheet(QStringLiteral("color: #334457;"));
  navLayout->addWidget(divider);
  auto* local = new QWidget;
  auto* localLayout = new QHBoxLayout(local);
  localLayout->setContentsMargins(12, 10, 8, 10);
  localLayout->setSpacing(8);
  auto* localIcon = new QLabel;
  localIcon->setPixmap(iconPixmap(IconKind::Monitor, QSize(24, 24), QColor(QStringLiteral("#8fc3ff"))));
  localLayout->addWidget(localIcon, 0, Qt::AlignTop);
  auto* localText = new QLabel(QStringLiteral("本机 · LANLink\n等待启动服务"));
  localText->setObjectName(QStringLiteral("localLabel"));
  localText->setWordWrap(true);
  localText->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  localLabel_ = localText;
  localText->setStyleSheet(QStringLiteral("color: #cfdae7; font-size: 13px;"));
  localLayout->addWidget(localText);
  navLayout->addWidget(local);
  bodyLayout->addWidget(nav);

  content_ = new QStackedWidget;
  content_->setObjectName(QStringLiteral("content"));
  const auto addTopologyPage = [this](QWidget* page) {
    if (auto* canvas = page->findChild<TopologyCanvas*>()) {
      connect(canvas, &TopologyCanvas::layoutChanged, this, &MainWindow::onTopologyLayoutChanged);
    }
    content_->addWidget(makePageContainer(page));
  };
  addTopologyPage(buildOverviewPage(&state_, content_));
  addTopologyPage(buildLayoutPage(&state_, content_));
  content_->addWidget(makePageContainer(buildDevicesPage(state_, content_)));
  content_->addWidget(makePageContainer(
      buildTransferPage(&state_, content_,
                        [this](const QString& path) { sendFileOnWorker(path); },
                        [this]() { cancelFileOnWorker(); })));
  content_->addWidget(makePageContainer(
      buildSettingsPage(&state_, content_,
                         [this](const app::AppOptions& options) { startRuntime(options); },
                         [this]() { stopRuntime(); },
                         [this](bool enabled) { toggleCrossScreen(enabled); },
                         [this](bool enabled) { toggleClipboard(enabled); })));
  bodyLayout->addWidget(content_, 1);
  rootLayout->addWidget(body, 1);

  auto* footer = new QWidget;
  footer->setObjectName(QStringLiteral("footer"));
  footer->setFixedHeight(48);
  auto* footerLayout = new QHBoxLayout(footer);
  footerLayout->setContentsMargins(22, 0, 22, 0);
  footerLabel_ = new QLabel(QStringLiteral("LANLink · 未启动"));
  footerLabel_->setObjectName(QStringLiteral("footerStatusLabel"));
  footerLabel_->setStyleSheet(QStringLiteral("color: #97a9bd; font-size: 13px;"));
  footerLayout->addWidget(footerLabel_);
  footerLayout->addStretch();
  auto* network = new QLabel(QStringLiteral("◉  等待服务启动"));
  network->setObjectName(QStringLiteral("networkLabel"));
  networkLabel_ = network;
  network->setStyleSheet(QStringLiteral("color: #a6d8c0; font-size: 13px;"));
  footerLayout->addWidget(network);
  auto* resizeGrip = new QSizeGrip(footer);
  resizeGrip->setFixedSize(18, 18);
  footerLayout->addWidget(resizeGrip);
  rootLayout->addWidget(footer);

  setCentralWidget(root);
  showPage(0);
}

QWidget* MainWindow::makePageContainer(QWidget* page) {
  auto* scroll = new QScrollArea;
  scroll->setObjectName(QStringLiteral("pageScroll"));
  scroll->setAttribute(Qt::WA_StyledBackground, true);
  scroll->viewport()->setStyleSheet(QStringLiteral("background: #1e2935; border: 0;"));
  scroll->setWidgetResizable(true);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  scroll->setWidget(page);
  return scroll;
}

void MainWindow::setWindowButtonStyle(QPushButton* button) {
  button->setCursor(Qt::PointingHandCursor);
}

void MainWindow::showPage(int index) {
  if (content_ == nullptr || index < 0 || index >= content_->count()) return;
  content_->setCurrentIndex(index);
  for (int buttonIndex = 0; buttonIndex < navButtons_.size(); ++buttonIndex) {
    navButtons_[buttonIndex]->setChecked(buttonIndex == index);
  }
}

void MainWindow::setupTray() {
  tray_ = new QSystemTrayIcon(this);
  tray_->setIcon(QIcon(QStringLiteral(":/icons/lanlink-icon.png")));
  tray_->setToolTip(QStringLiteral("LANLink · 未启动"));
  auto* menu = new QMenu(this);
  auto* open = menu->addAction(QStringLiteral("打开控制面板"));
  connect(open, &QAction::triggered, this, [this]() {
    showNormal();
    raise();
    activateWindow();
  });
  trayCrossScreen_ = menu->addAction(QStringLiteral("跨屏键鼠转发（由运行时管理）"));
  trayCrossScreen_->setCheckable(true);
  trayCrossScreen_->setChecked(state_.crossScreenEnabled);
  connect(trayCrossScreen_, &QAction::toggled, this, &MainWindow::toggleCrossScreen);
  trayClipboard_ = menu->addAction(QStringLiteral("剪贴板同步"));
  trayClipboard_->setCheckable(true);
  trayClipboard_->setChecked(state_.clipboardEnabled);
  connect(trayClipboard_, &QAction::toggled, this, &MainWindow::toggleClipboard);
  menu->addAction(QStringLiteral("锁定当前设备"), this, &MainWindow::lockCurrentDevice);
  menu->addAction(QStringLiteral("紧急恢复本地输入"), this, &MainWindow::recoverLocalInput);
  menu->addSeparator();
  menu->addAction(QStringLiteral("退出 LANLink"), this, &MainWindow::quitFromTray);
  tray_->setContextMenu(menu);
  if (QSystemTrayIcon::isSystemTrayAvailable()) tray_->show();
}

void MainWindow::updateStatusMessage(const QString& message) {
  if (footerLabel_ != nullptr) footerLabel_->setText(message);
}

void MainWindow::toggleCrossScreen(bool enabled) {
  state_.crossScreenEnabled = enabled;
  state_.allowRemoteKeys = enabled;
  if (trayCrossScreen_ != nullptr && trayCrossScreen_->isChecked() != enabled) trayCrossScreen_->setChecked(enabled);
  if (runtimeWorker_ != nullptr && runtimeThread_ != nullptr && runtimeThread_->isRunning()) {
    QMetaObject::invokeMethod(runtimeWorker_, &RuntimeWorker::setInputForwardingEnabled,
                              Qt::BlockingQueuedConnection, enabled);
  }
  updateStatusMessage(enabled ? QStringLiteral("LANLink v0.1 · 跨屏切换已启用")
                              : QStringLiteral("LANLink v0.1 · 跨屏切换已暂停"));
}

void MainWindow::toggleClipboard(bool enabled) {
  state_.clipboardEnabled = enabled;
  state_.options.clipboard_enabled = enabled;
  if (trayClipboard_ != nullptr && trayClipboard_->isChecked() != enabled) trayClipboard_->setChecked(enabled);
  if (auto* check = findChild<QCheckBox*>(QStringLiteral("clipboardSyncCheck"));
      check != nullptr && check->isChecked() != enabled) {
    const QSignalBlocker blocker(check);
    check->setChecked(enabled);
  }
  if (runtimeWorker_ != nullptr && runtimeThread_ != nullptr && runtimeThread_->isRunning()) {
    QMetaObject::invokeMethod(runtimeWorker_, &RuntimeWorker::setClipboardSyncEnabled,
                              Qt::BlockingQueuedConnection, enabled);
  }
  updateStatusMessage(enabled ? QStringLiteral("LANLink v0.1 · 剪贴板同步已启用")
                              : QStringLiteral("LANLink v0.1 · 剪贴板同步已暂停"));
}

void MainWindow::lockCurrentDevice() {
  state_.activeDevice = 0;
  if (controlLabel_ != nullptr) controlLabel_->setText(QStringLiteral("当前控制：本机输入"));
  updateStatusMessage(QStringLiteral("LANLink · 已锁定本机输入"));
}

void MainWindow::recoverLocalInput() {
  state_.activeDevice = 0;
  recoverLocalInputOnWorker();
  if (controlLabel_ != nullptr) controlLabel_->setText(QStringLiteral("当前控制：本机输入"));
  updateStatusMessage(QStringLiteral("LANLink · 本地输入已恢复"));
}

void MainWindow::quitFromTray() {
  stopRuntime();
  recoverLocalInput();
  if (tray_ != nullptr) tray_->hide();
  QApplication::quit();
}

void MainWindow::closeEvent(QCloseEvent* event) {
  stopRuntime();
  recoverLocalInput();
  if (tray_ != nullptr) tray_->hide();
  event->accept();
}

void MainWindow::startRuntime(const app::AppOptions& options) {
  if (runtimeThread_ != nullptr) stopRuntime();
  state_.options = options;
  state_.autoDiscoverPeer = !options.peer_id.has_value();
  state_.error.clear();
  state_.status = QStringLiteral("正在启动");
  state_.running = true;
  state_.connected = false;
  state_.localEndpoint = local_endpoint_for_port(options.listen_port);
  updateRuntimeControls();

  runtimeThread_ = new QThread(this);
  runtimeWorker_ = new RuntimeWorker(nullptr, backend_factory_, state_.crossScreenEnabled,
                                     clipboard_backend_factory_);
  runtimeWorker_->moveToThread(runtimeThread_);
  connect(runtimeThread_, &QThread::finished, runtimeWorker_, &QObject::deleteLater);
  connect(runtimeWorker_, &RuntimeWorker::statusChanged, this, &MainWindow::onRuntimeStatusChanged);
  connect(runtimeWorker_, &RuntimeWorker::peerChanged, this, &MainWindow::onPeerChanged);
  connect(runtimeWorker_, &RuntimeWorker::remoteTopologyChanged, this,
          &MainWindow::onRemoteTopologyChanged);
  connect(runtimeWorker_, &RuntimeWorker::logMessage, this, &MainWindow::onRuntimeLog);
  connect(runtimeWorker_, &RuntimeWorker::errorOccurred, this, &MainWindow::onRuntimeError);
  connect(runtimeWorker_, &RuntimeWorker::fileTransferChanged, this,
          &MainWindow::onFileTransferChanged);
  RuntimeWorker* worker = runtimeWorker_;
  const app::AppOptions copied = options;
  connect(runtimeThread_, &QThread::started, worker, [worker, copied]() mutable {
    worker->start(std::move(copied));
  });
  runtimeThread_->start();
}

void MainWindow::stopRuntime() {
  const bool rebuild = state_.running || state_.connected || state_.devices.size() > 1 ||
                       !state_.peerEndpoint.isEmpty();
  if (runtimeThread_ != nullptr && runtimeWorker_ != nullptr) {
    QMetaObject::invokeMethod(runtimeWorker_, &RuntimeWorker::stop, Qt::BlockingQueuedConnection);
    runtimeThread_->quit();
    runtimeThread_->wait();
    runtimeWorker_ = nullptr;
    delete runtimeThread_;
    runtimeThread_ = nullptr;
  }
  state_.running = false;
  state_.connected = false;
  state_.status = QStringLiteral("已停止");
  state_.peerEndpoint.clear();
  state_.peerName.clear();
  state_.fileTransferName.clear();
  state_.fileTransferStatus.clear();
  state_.fileTransferDone = 0;
  state_.fileTransferTotal = 0;
  if (!state_.devices.isEmpty()) state_.devices.resize(1);
  state_.activeDevice = 0;
  state_.selectedTransferTarget = 0;
  if (!state_.devices.isEmpty()) {
    state_.devices[0].online = false;
    state_.devices[0].address = QStringLiteral("未启动");
  }
  if (rebuild) rebuildPages();
  updateRuntimeControls();
}

void MainWindow::recoverLocalInputOnWorker() {
  if (runtimeWorker_ == nullptr || runtimeThread_ == nullptr) return;
  QMetaObject::invokeMethod(runtimeWorker_, &RuntimeWorker::recoverLocalInput,
                            Qt::BlockingQueuedConnection);
}

void MainWindow::sendFileOnWorker(const QString& path) {
  if (runtimeWorker_ == nullptr || runtimeThread_ == nullptr) {
    onFileTransferChanged(QFileInfo(path).fileName(), 0, 0, QStringLiteral("服务未运行"), true);
    return;
  }
  QMetaObject::invokeMethod(runtimeWorker_, &RuntimeWorker::sendFile,
                            Qt::QueuedConnection, path);
}

void MainWindow::cancelFileOnWorker() {
  if (runtimeWorker_ == nullptr || runtimeThread_ == nullptr) return;
  QMetaObject::invokeMethod(runtimeWorker_, &RuntimeWorker::cancelFile,
                            Qt::QueuedConnection);
}

void MainWindow::onRuntimeStatusChanged(const QString& status, bool running, bool connected) {
  state_.status = status;
  state_.running = running;
  state_.connected = connected;
  bool rebuild = false;
  if (!state_.devices.isEmpty()) {
    const QString localAddress = running ? QStringLiteral("监听 %1").arg(state_.localEndpoint)
                                         : QStringLiteral("未启动");
    rebuild = state_.devices[0].online != running ||
              state_.devices[0].address != localAddress;
    state_.devices[0].online = running;
    state_.devices[0].address = localAddress;
  }
  if (statusLabel_ != nullptr) {
    statusLabel_->setText(connected ? QStringLiteral("●  已连接")
                                    : (running ? QStringLiteral("●  运行中")
                                               : QStringLiteral("●  未运行")));
    const QString statusColor = connected ? QStringLiteral("#9bd9ff")
                                          : (running ? QStringLiteral("#9be3be")
                                                     : QStringLiteral("#f2c77d"));
    const QString statusBackground = connected ? QStringLiteral("#1d425b")
                                               : (running ? QStringLiteral("#123f36")
                                                          : QStringLiteral("#3f3524"));
    const QString statusBorder = connected ? QStringLiteral("#4d8db8")
                                           : (running ? QStringLiteral("#2c8a76")
                                                      : QStringLiteral("#705e3c"));
    statusLabel_->setStyleSheet(QStringLiteral(
        "color: %1; background: %2; border: 1px solid %3; border-radius: 17px; padding: 6px 14px; font-size: 14px;")
                                    .arg(statusColor)
                                    .arg(statusBackground)
                                    .arg(statusBorder));
  }
  updateStatusMessage(QStringLiteral("LANLink · %1").arg(status));
  if (tray_ != nullptr)
    tray_->setToolTip(QStringLiteral("LANLink · %1").arg(status));
  if (networkLabel_ != nullptr)
    networkLabel_->setText(connected ? QStringLiteral("◉  局域网 · 已连接")
                                     : (running ? QStringLiteral("◉  局域网 · 等待对端")
                                                : QStringLiteral("◉  等待服务启动")));
  if (localLabel_ != nullptr)
    localLabel_->setText(running ? QStringLiteral("本机 · LANLink\n监听 %1").arg(state_.localEndpoint)
                                 : QStringLiteral("本机 · LANLink\n等待启动服务"));
  if (rebuild) rebuildPages();
  updateRuntimeControls();
}

void MainWindow::onPeerChanged(const QString& name, const QString& platform,
                               const QString& address, qulonglong deviceId,
                               qulonglong displayId, bool online) {
  int peerIndex = -1;
  for (int index = 1; index < state_.devices.size(); ++index) {
    if (state_.devices[index].deviceId == deviceId) {
      peerIndex = index;
      break;
    }
  }
  if (!online) {
    if (peerIndex < 0) return;
    state_.devices.removeAt(peerIndex);
    if (state_.activeDevice == peerIndex || state_.activeDevice >= state_.devices.size()) {
      state_.activeDevice = 0;
    }
    if (state_.selectedTransferTarget == peerIndex ||
        state_.selectedTransferTarget >= state_.devices.size()) {
      state_.selectedTransferTarget = 0;
    } else if (state_.selectedTransferTarget > peerIndex) {
      --state_.selectedTransferTarget;
    }
    state_.peerName.clear();
    state_.peerEndpoint.clear();
    rebuildPages();
    syncTopologyDirection();
    return;
  }
  bool changed = false;
  if (peerIndex < 0) {
    peerIndex = state_.devices.size();
    state_.devices.append(DeviceState{
        name,
        platform,
        address,
        QColor(QStringLiteral("#4db69e")),
        default_new_device_position(state_.devices),
        QSizeF(kTopologyCardWidth, kTopologyCardHeight),
        false,
        online,
        deviceId,
        displayId,
    });
    changed = true;
  }
  auto& peer = state_.devices[peerIndex];
  changed = changed || peer.name != name || peer.platform != platform || peer.address != address ||
            peer.deviceId != deviceId || peer.displayId != displayId || peer.online != online;
  if (!changed) return;
  captureSettingsFromUi();
  peer.name = name;
  peer.platform = platform;
  peer.address = address;
  peer.deviceId = deviceId;
  peer.displayId = displayId;
  peer.online = online;
  state_.peerName = name;
  state_.peerEndpoint = address;
  if (!state_.autoDiscoverPeer) state_.options.peer_id = deviceId;
  if (!state_.options.peer_display_id_explicit) state_.options.peer_display_id = displayId;
  rebuildPages();
  syncTopologyDirection();
}

void MainWindow::onRemoteTopologyChanged(const discovery::TopologySnapshot& snapshot) {
  if (state_.devices.size() < 2) return;
  const auto local_device = app::numeric_id(state_.options.device_id);
  const auto local_display = app::numeric_id(state_.options.display_id);
  const Display* local = nullptr;
  const Display* peer = nullptr;
  for (const auto& display : snapshot.topology.displays()) {
    if (display.device == local_device || display.id == local_display) {
      local = &display;
      continue;
    }
    if (peer == nullptr || display.device.low == state_.devices[1].deviceId ||
        display.id.low == state_.devices[1].displayId) {
      peer = &display;
    }
  }
  if (local == nullptr || peer == nullptr) return;
  const double local_center_x =
      (local->logical_bounds.left + local->logical_bounds.right) / 2.0;
  const double peer_center_x =
      (peer->logical_bounds.left + peer->logical_bounds.right) / 2.0;
  if (!std::isfinite(local_center_x) || !std::isfinite(peer_center_x) ||
      std::abs(peer_center_x - local_center_x) < 0.001) {
    return;
  }
  const bool peer_on_right = peer_center_x > local_center_x;
  state_.devices[0].position = QPointF(kTopologyOriginX, kTopologyOriginY);
  state_.devices[1].position =
      QPointF(peer_on_right ? kTopologyOriginX + kTopologyCardWidth + kTopologyCardGap
                            : kTopologyOriginX - kTopologyCardGap - kTopologyCardWidth,
              state_.devices[0].position.y());
  state_.options.remote_side = peer_on_right ? app::RemoteSide::kRight : app::RemoteSide::kLeft;
  state_.options.remote_side_explicit = true;
  rebuildPages();
  if (auto* direction = findChild<QLabel*>(QStringLiteral("topologyDirectionLabel"))) {
    direction->setText(remote_side_text(state_.options.remote_side));
  }
}

void MainWindow::onTopologyLayoutChanged() { syncTopologyDirection(); }

void MainWindow::syncTopologyDirection() {
  const auto side = remote_side_from_topology(state_.devices);
  if (!side.has_value()) {
    if (auto* direction = findChild<QLabel*>(QStringLiteral("topologyDirectionLabel"))) {
      direction->setText(state_.devices.size() < 2
                             ? QStringLiteral("由拓扑图决定 · 等待对端布局")
                             : QStringLiteral("由拓扑图决定 · 当前为上下布局（运行时仅支持左右）"));
    }
    return;
  }

  state_.options.remote_side = *side;
  state_.options.remote_side_explicit = true;
  if (auto* direction = findChild<QLabel*>(QStringLiteral("topologyDirectionLabel")))
    direction->setText(remote_side_text(*side));

  if (runtimeWorker_ != nullptr && runtimeThread_ != nullptr && runtimeThread_->isRunning()) {
    auto* worker = runtimeWorker_;
    const auto selectedSide = *side;
    QMetaObject::invokeMethod(
        worker, [worker, selectedSide]() { worker->setRemoteSide(selectedSide); },
        Qt::BlockingQueuedConnection);
  }
}

void MainWindow::onRuntimeLog(const QString& message) {
  state_.lastLog = message;
  updateStatusMessage(QStringLiteral("LANLink · %1").arg(message));
}

void MainWindow::onRuntimeError(const QString& message) {
  state_.error = message;
  state_.status = QStringLiteral("错误");
  updateStatusMessage(QStringLiteral("LANLink · 错误：%1").arg(message));
  updateRuntimeControls();
}

void MainWindow::onFileTransferChanged(const QString& name, qulonglong done, qulonglong total,
                                       const QString& status, bool outgoing) {
  state_.fileTransferName = name;
  state_.fileTransferDone = static_cast<std::uint64_t>(done);
  state_.fileTransferTotal = static_cast<std::uint64_t>(total);
  state_.fileTransferStatus = status;
  state_.fileTransferOutgoing = outgoing;
  if (content_ != nullptr && content_->currentIndex() == 3) {
    auto* label = findChild<QLabel*>(QStringLiteral("transferStatusLabel"));
    if (label != nullptr) {
      const bool active = status == QStringLiteral("已接受") ||
                          status == QStringLiteral("传输中") ||
                          status.startsWith(QStringLiteral("传输中："));
      if (!name.isEmpty() && total > 0) {
        label->setText(active
                          ? QStringLiteral("%1 · %2 / %3 (%4%)")
                                .arg(name)
                                .arg(done)
                                .arg(total)
                                .arg((100.0 * done) / total, 0, 'f', 1)
                          : QStringLiteral("%1 · %2 · %3 / %4 (%5%)")
                                .arg(name)
                                .arg(status)
                                .arg(done)
                                .arg(total)
                                .arg((100.0 * done) / total, 0, 'f', 1));
      } else {
        label->setText(status);
      }
    }
    if (auto* cancel = findChild<QPushButton*>(QStringLiteral("transferCancelButton")))
      cancel->setEnabled(status == QStringLiteral("已接受") || status == QStringLiteral("传输中") ||
                         status.startsWith(QStringLiteral("传输中：")));
  }
}

void MainWindow::rebuildPages() {
  if (content_ == nullptr) return;
  const int current = content_->currentIndex();
  while (content_->count() > 0) delete content_->widget(0);
  const auto addTopologyPage = [this](QWidget* page) {
    if (auto* canvas = page->findChild<TopologyCanvas*>()) {
      connect(canvas, &TopologyCanvas::layoutChanged, this, &MainWindow::onTopologyLayoutChanged);
    }
    content_->addWidget(makePageContainer(page));
  };
  addTopologyPage(buildOverviewPage(&state_, content_));
  addTopologyPage(buildLayoutPage(&state_, content_));
  content_->addWidget(makePageContainer(buildDevicesPage(state_, content_)));
  content_->addWidget(makePageContainer(
      buildTransferPage(&state_, content_,
                        [this](const QString& path) { sendFileOnWorker(path); },
                        [this]() { cancelFileOnWorker(); })));
  content_->addWidget(makePageContainer(
      buildSettingsPage(&state_, content_,
                         [this](const app::AppOptions& options) { startRuntime(options); },
                         [this]() { stopRuntime(); },
                         [this](bool enabled) { toggleCrossScreen(enabled); },
                         [this](bool enabled) { toggleClipboard(enabled); })));
  showPage(qBound(0, current, content_->count() - 1));
  updateRuntimeControls();
}

void MainWindow::updateRuntimeControls() {
  if (auto* button = findChild<QPushButton*>(QStringLiteral("runtimeToggle")))
    button->setText(state_.running ? QStringLiteral("停止服务") : QStringLiteral("启动服务"));
  if (auto* status = findChild<QLabel*>(QStringLiteral("runtimeFormStatus")))
    status->setText(state_.status);
}

void MainWindow::captureSettingsFromUi() {
  auto* deviceId = findChild<QLineEdit*>(QStringLiteral("deviceIdEdit"));
  auto* peerId = findChild<QLineEdit*>(QStringLiteral("peerIdEdit"));
  auto* listenPort = findChild<QLineEdit*>(QStringLiteral("listenPortEdit"));
  auto* group = findChild<QLineEdit*>(QStringLiteral("groupEdit"));
  auto* discoveryPort = findChild<QLineEdit*>(QStringLiteral("discoveryPortEdit"));
  auto* connectHost = findChild<QLineEdit*>(QStringLiteral("connectHostEdit"));
  auto* connectPort = findChild<QLineEdit*>(QStringLiteral("connectPortEdit"));
  auto* displayId = findChild<QLineEdit*>(QStringLiteral("displayIdEdit"));
  auto* peerDisplayId = findChild<QLineEdit*>(QStringLiteral("peerDisplayIdEdit"));
  auto* autoTopology = findChild<QCheckBox*>(QStringLiteral("autoTopologyCheck"));
  bool ok = false;
  if (deviceId != nullptr) {
    const auto value = deviceId->text().toULongLong(&ok);
    if (ok && value != 0) state_.options.device_id = value;
  }
  if (peerId != nullptr) {
    const auto text = peerId->text().trimmed();
    state_.autoDiscoverPeer = text.isEmpty();
    if (text.isEmpty()) {
      state_.options.peer_id.reset();
    } else {
      const auto value = text.toULongLong(&ok);
      if (ok && value != 0) state_.options.peer_id = value;
    }
  }
  if (listenPort != nullptr) {
    const auto value = listenPort->text().toUShort(&ok);
    if (ok && value != 0) state_.options.listen_port = value;
  }
  if (group != nullptr) state_.options.group = group->text().trimmed().toStdString();
  if (discoveryPort != nullptr) {
    const auto value = discoveryPort->text().toUShort(&ok);
    if (ok && value != 0) state_.options.discovery_port = value;
  }
  if (displayId != nullptr) {
    const auto value = displayId->text().toULongLong(&ok);
    if (ok && value != 0) state_.options.display_id = value;
  }
  if (peerDisplayId != nullptr) {
    const auto value = peerDisplayId->text().toULongLong(&ok);
    if (ok && value != 0) state_.options.peer_display_id = value;
  }
  if (connectHost != nullptr) {
    const auto host = connectHost->text().trimmed();
    if (host.isEmpty()) {
      state_.options.connect.reset();
    } else if (connectPort != nullptr) {
      const auto port = connectPort->text().trimmed().toUShort(&ok);
      if (ok && port != 0) state_.options.connect = transport::Endpoint{host.toStdString(), port};
    }
  }
  if (autoTopology != nullptr) {
    state_.options.peer_display_id_explicit = !autoTopology->isChecked();
  }
  state_.options.remote_side_explicit = true;
  syncTopologyDirection();
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
  if (watched == titleBar_) {
    if (event->type() == QEvent::MouseButtonPress) {
      auto* mouseEvent = static_cast<QMouseEvent*>(event);
      if (mouseEvent->button() == Qt::LeftButton) {
        draggingWindow_ = true;
        windowDragOffset_ = mouseEvent->globalPosition().toPoint() - frameGeometry().topLeft();
        return true;
      }
    } else if (event->type() == QEvent::MouseMove && draggingWindow_) {
      auto* mouseEvent = static_cast<QMouseEvent*>(event);
      move(mouseEvent->globalPosition().toPoint() - windowDragOffset_);
      return true;
    } else if (event->type() == QEvent::MouseButtonRelease) {
      draggingWindow_ = false;
      return true;
    }
  }
  return QMainWindow::eventFilter(watched, event);
}

}  // namespace lanlink::desktop
