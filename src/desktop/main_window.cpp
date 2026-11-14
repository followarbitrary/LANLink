#include "main_window.hpp"

#include "pages.hpp"
#include "icons.hpp"

#include <QAction>
#if defined(LANLINK_HAS_QT_NETWORK)
#include <QAbstractSocket>
#include <QNetworkInterface>
#endif
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMouseEvent>
#include <QMenu>
#include <QMetaObject>
#include <QSizeGrip>
#include <QScreen>
#include <QScrollArea>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

#include <utility>

namespace lanlink::desktop {
namespace {

const QString kWindowStyle = QStringLiteral(
    "QMainWindow { background: #1e2935; }"
    "QWidget#titleBar { background: #18222e; }"
    "QWidget#nav { background: #151d28; }"
    "QWidget#footer { background: #151d28; }"
    "QLabel { color: #e9eff6; background: transparent; }"
    "QLabel#muted { color: #93a5ba; }"
    "QPushButton#navButton { color: #9caec2; background: transparent; border: 0; border-radius: 10px; text-align: left; padding: 12px 14px; font-size: 16px; }"
    "QPushButton#navButton:checked { color: #f0f5fb; background: #293f58; }"
    "QPushButton#windowButton { color: #8fa1b5; background: transparent; border: 0; font-size: 18px; }"
    "QPushButton#windowButton:hover { color: #f0f5fb; background: #263646; border-radius: 7px; }"
    "QStackedWidget#content { background: #1e2935; }"
    "QScrollArea#pageScroll { border: 0; background: #1e2935; }"
    "QScrollBar:vertical { background: #1e2935; width: 8px; }"
    "QScrollBar::handle:vertical { background: #40556d; border-radius: 4px; }"
);

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

}  // namespace

MainWindow::MainWindow(QWidget* parent, RuntimeWorker::BackendFactory backend_factory,
                       bool auto_start)
    : QMainWindow(parent), backend_factory_(std::move(backend_factory)) {
  setWindowTitle(QStringLiteral("LANLink"));
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
  titleLayout->setContentsMargins(22, 0, 14, 0);
  titleLayout->setSpacing(10);
  auto* logo = new QLabel(QStringLiteral("▱"));
  logo->setStyleSheet(QStringLiteral("color: #8fc3ff; font-size: 28px; font-weight: 600;"));
  titleLayout->addWidget(logo);
  auto* appName = new QLabel(QStringLiteral("LANLink"));
  appName->setStyleSheet(QStringLiteral("font-size: 24px; font-weight: 600;"));
  titleLayout->addWidget(appName);
  auto* preview = new QLabel(QStringLiteral("Qt6 控制台"));
  preview->setStyleSheet(QStringLiteral("color: #8fa2b8; font-size: 15px;"));
  titleLayout->addWidget(preview);
  titleLayout->addStretch();
  auto* running = new QLabel(QStringLiteral("●  未运行"));
  statusLabel_ = running;
  running->setObjectName(QStringLiteral("runtimeHeaderStatus"));
  running->setAlignment(Qt::AlignCenter);
  running->setMinimumWidth(124);
  running->setStyleSheet(QStringLiteral(
      "color: #f2c77d; background: #5a4321; border-radius: 22px; padding: 9px 15px; font-size: 15px;"));
  titleLayout->addWidget(running);
  controlLabel_ = new QLabel(QStringLiteral("当前控制：本机输入"));
  controlLabel_->setStyleSheet(QStringLiteral("color: #a6b7ca; font-size: 13px;"));
  titleLayout->addWidget(controlLabel_);
  const QVector<QPair<QString, QStyle::StandardPixmap>> buttons = {
      {QStringLiteral("—"), QStyle::SP_TitleBarMinButton},
      {QStringLiteral("□"), QStyle::SP_TitleBarMaxButton},
      {QStringLiteral("×"), QStyle::SP_TitleBarCloseButton},
  };
  for (const auto& [text, icon] : buttons) {
    auto* button = new QPushButton(text);
    button->setObjectName(QStringLiteral("windowButton"));
    button->setFixedSize(34, 34);
    setWindowButtonStyle(button);
    titleLayout->addWidget(button);
    if (icon == QStyle::SP_TitleBarMinButton) {
      connect(button, &QPushButton::clicked, this, &QWidget::showMinimized);
    } else if (icon == QStyle::SP_TitleBarMaxButton) {
      connect(button, &QPushButton::clicked, this, [this]() {
        isMaximized() ? showNormal() : showMaximized();
      });
    } else {
      connect(button, &QPushButton::clicked, this, &QWidget::close);
    }
  }
  rootLayout->addWidget(titleBar);

  auto* body = new QWidget;
  auto* bodyLayout = new QHBoxLayout(body);
  bodyLayout->setContentsMargins(0, 0, 0, 0);
  bodyLayout->setSpacing(0);
  auto* nav = new QWidget;
  nav->setObjectName(QStringLiteral("nav"));
  nav->setFixedWidth(180);
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
  auto* localIcon = new QLabel;
  localIcon->setPixmap(iconPixmap(IconKind::Monitor, QSize(24, 24), QColor(QStringLiteral("#8fc3ff"))));
  localLayout->addWidget(localIcon, 0, Qt::AlignTop);
  auto* localText = new QLabel(QStringLiteral("本机 · LANLink\n等待启动服务"));
  localLabel_ = localText;
  localText->setStyleSheet(QStringLiteral("color: #cfdae7; font-size: 13px;"));
  localLayout->addWidget(localText);
  navLayout->addWidget(local);
  bodyLayout->addWidget(nav);

  content_ = new QStackedWidget;
  content_->setObjectName(QStringLiteral("content"));
  content_->addWidget(makePageContainer(buildOverviewPage(&state_, content_)));
  content_->addWidget(makePageContainer(buildLayoutPage(&state_, content_)));
  content_->addWidget(makePageContainer(buildDevicesPage(state_, content_)));
  content_->addWidget(makePageContainer(buildTransferPage(&state_, content_)));
  content_->addWidget(makePageContainer(
      buildSettingsPage(&state_, content_,
                         [this](const app::AppOptions& options) { startRuntime(options); },
                         [this]() { stopRuntime(); },
                         [this](bool enabled) { toggleCrossScreen(enabled); })));
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
  tray_->setIcon(style()->standardIcon(QStyle::SP_ComputerIcon));
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
  trayClipboard_ = menu->addAction(QStringLiteral("剪贴板同步（未接入）"));
  trayClipboard_->setEnabled(false);
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
  if (trayClipboard_ != nullptr && trayClipboard_->isChecked() != enabled) trayClipboard_->setChecked(enabled);
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
  runtimeWorker_ = new RuntimeWorker(nullptr, backend_factory_, state_.crossScreenEnabled);
  runtimeWorker_->moveToThread(runtimeThread_);
  connect(runtimeThread_, &QThread::finished, runtimeWorker_, &QObject::deleteLater);
  connect(runtimeWorker_, &RuntimeWorker::statusChanged, this, &MainWindow::onRuntimeStatusChanged);
  connect(runtimeWorker_, &RuntimeWorker::peerChanged, this, &MainWindow::onPeerChanged);
  connect(runtimeWorker_, &RuntimeWorker::logMessage, this, &MainWindow::onRuntimeLog);
  connect(runtimeWorker_, &RuntimeWorker::errorOccurred, this, &MainWindow::onRuntimeError);
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
    statusLabel_->setStyleSheet(QStringLiteral(
        "color: %1; background: %2; border-radius: 22px; padding: 9px 15px; font-size: 15px;")
                                    .arg(connected ? QStringLiteral("#9be3be") : QStringLiteral("#f2c77d"))
                                    .arg(connected ? QStringLiteral("#174d3c") : QStringLiteral("#5a4321")));
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
        QPointF(310.0 + 270.0 * static_cast<double>(peerIndex - 1), 68.0),
        QSizeF(220.0, 120.0),
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

void MainWindow::rebuildPages() {
  if (content_ == nullptr) return;
  const int current = content_->currentIndex();
  while (content_->count() > 0) delete content_->widget(0);
  content_->addWidget(makePageContainer(buildOverviewPage(&state_, content_)));
  content_->addWidget(makePageContainer(buildLayoutPage(&state_, content_)));
  content_->addWidget(makePageContainer(buildDevicesPage(state_, content_)));
  content_->addWidget(makePageContainer(buildTransferPage(&state_, content_)));
  content_->addWidget(makePageContainer(
      buildSettingsPage(&state_, content_,
                         [this](const app::AppOptions& options) { startRuntime(options); },
                         [this]() { stopRuntime(); },
                         [this](bool enabled) { toggleCrossScreen(enabled); })));
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
  auto* remoteSide = findChild<QComboBox*>(QStringLiteral("remoteSideCombo"));
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
  if (remoteSide != nullptr) {
    state_.options.remote_side = remoteSide->currentData().toString() == QStringLiteral("left")
                                     ? app::RemoteSide::kLeft
                                     : app::RemoteSide::kRight;
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
    state_.options.remote_side_explicit = !autoTopology->isChecked();
  }
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
