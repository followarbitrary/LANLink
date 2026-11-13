#include "main_window.hpp"

#include "pages.hpp"
#include "icons.hpp"

#include <QAction>
#include <QApplication>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QMenu>
#include <QSizeGrip>
#include <QScreen>
#include <QScrollArea>
#include <QStyle>
#include <QVBoxLayout>

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

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  setWindowTitle(QStringLiteral("LANLink"));
  setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
  setAttribute(Qt::WA_TranslucentBackground, false);
  setMinimumSize(820, 560);
  resize(1080, 700);
  setStyleSheet(kWindowStyle);
  setupUi();
  setupTray();
}

MainWindow::~MainWindow() = default;

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
  auto* preview = new QLabel(QStringLiteral("Desktop Preview"));
  preview->setStyleSheet(QStringLiteral("color: #8fa2b8; font-size: 15px;"));
  titleLayout->addWidget(preview);
  titleLayout->addStretch();
  auto* running = new QLabel(QStringLiteral("●  运行中"));
  running->setAlignment(Qt::AlignCenter);
  running->setMinimumWidth(124);
  running->setStyleSheet(QStringLiteral(
      "color: #9be3be; background: #174d3c; border-radius: 22px; padding: 9px 15px; font-size: 15px;"));
  titleLayout->addWidget(running);
  controlLabel_ = new QLabel(QStringLiteral("当前控制：Windows PC"));
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
  auto* localText = new QLabel(QStringLiteral("本机 · Windows PC\n已加入共享网络"));
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
  content_->addWidget(makePageContainer(buildSettingsPage(&state_, content_)));
  bodyLayout->addWidget(content_, 1);
  rootLayout->addWidget(body, 1);

  auto* footer = new QWidget;
  footer->setObjectName(QStringLiteral("footer"));
  footer->setFixedHeight(48);
  auto* footerLayout = new QHBoxLayout(footer);
  footerLayout->setContentsMargins(22, 0, 22, 0);
  footerLabel_ = new QLabel(QStringLiteral("LANLink v0.1 · 多主控 P2P"));
  footerLabel_->setStyleSheet(QStringLiteral("color: #97a9bd; font-size: 13px;"));
  footerLayout->addWidget(footerLabel_);
  footerLayout->addStretch();
  auto* network = new QLabel(QStringLiteral("◉  本地网络 · 加密连接"));
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
  tray_->setToolTip(QStringLiteral("LANLink · 运行中"));
  auto* menu = new QMenu(this);
  auto* open = menu->addAction(QStringLiteral("打开控制面板"));
  connect(open, &QAction::triggered, this, [this]() {
    showNormal();
    raise();
    activateWindow();
  });
  trayCrossScreen_ = menu->addAction(QStringLiteral("启用跨屏切换"));
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
  if (trayCrossScreen_ != nullptr && trayCrossScreen_->isChecked() != enabled) trayCrossScreen_->setChecked(enabled);
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
  if (controlLabel_ != nullptr) controlLabel_->setText(QStringLiteral("当前控制：Windows PC"));
  updateStatusMessage(QStringLiteral("LANLink v0.1 · 已锁定本机输入"));
}

void MainWindow::recoverLocalInput() {
  state_.activeDevice = 0;
  if (controlLabel_ != nullptr) controlLabel_->setText(QStringLiteral("当前控制：Windows PC · 本地"));
  updateStatusMessage(QStringLiteral("LANLink v0.1 · 本地输入已恢复"));
}

void MainWindow::quitFromTray() {
  recoverLocalInput();
  if (tray_ != nullptr) tray_->hide();
  QApplication::quit();
}

void MainWindow::closeEvent(QCloseEvent* event) {
  recoverLocalInput();
  if (tray_ != nullptr) tray_->hide();
  event->accept();
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
