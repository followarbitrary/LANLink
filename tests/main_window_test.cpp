#include "main_window.hpp"
#include "topology_canvas.hpp"

#include "lanlink/platform/fake_clipboard_backend.hpp"
#include "lanlink/platform/fake_input_backend.hpp"

#include <QApplication>
#include <QImage>
#include <QAction>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPushButton>
#include <QSystemTrayIcon>
#include <QThread>
#include <QVariantAnimation>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>

namespace {

class QtWidgetsEnvironment final : public testing::Environment {
 public:
  void SetUp() override {
    int argc = 1;
    static char application_name[] = "main_window_test";
    static char* argv[] = {application_name, nullptr};
    app_ = std::make_unique<QApplication>(argc, argv);
  }

  void TearDown() override { app_.reset(); }

 private:
  std::unique_ptr<QApplication> app_;
};

testing::Environment* const qt_widgets_environment =
    testing::AddGlobalTestEnvironment(new QtWidgetsEnvironment);

using lanlink::desktop::MainWindow;
using lanlink::desktop::RuntimeWorker;
using lanlink::desktop::TopologyCanvas;
using lanlink::desktop::DesktopState;
using lanlink::desktop::DeviceState;
using lanlink::desktop::kTopologyCardGap;
using lanlink::desktop::kTopologyCardHeight;
using lanlink::desktop::kTopologyCardWidth;
using lanlink::desktop::kTopologyOriginX;
using lanlink::desktop::kTopologyOriginY;

RuntimeWorker::BackendFactory fake_backend_factory() {
  return [] {
    auto backend = std::make_unique<lanlink::platform::FakeInputBackend>();
    backend->set_display_bounds(lanlink::Rect{0.0, 0.0, 100.0, 100.0});
    return backend;
  };
}

RuntimeWorker::BackendFactory failing_backend_factory() {
  return [] { return std::unique_ptr<lanlink::platform::IInputBackend>{}; };
}

RuntimeWorker::ClipboardBackendFactory fake_clipboard_backend_factory() {
  return [] { return std::make_unique<lanlink::platform::FakeClipboardBackend>(); };
}

template <typename Predicate>
bool wait_until(Predicate&& predicate, int timeout_ms = 2500) {
  QElapsedTimer timer;
  timer.start();
  while (!predicate()) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(5);
    if (timer.elapsed() >= timeout_ms) return false;
  }
  return true;
}

QPushButton* runtime_toggle(MainWindow& window) {
  return window.findChild<QPushButton*>(QStringLiteral("runtimeToggle"));
}

QLabel* runtime_status(MainWindow& window) {
  return window.findChild<QLabel*>(QStringLiteral("runtimeFormStatus"));
}

QLineEdit* field(MainWindow& window, const QString& name) {
  return window.findChild<QLineEdit*>(name);
}

int label_count(MainWindow& window, const QString& text) {
  int count = 0;
  for (auto* label : window.findChildren<QLabel*>()) {
    if (label->text() == text) ++count;
  }
  return count;
}

bool label_contains(MainWindow& window, const QString& text) {
  for (auto* label : window.findChildren<QLabel*>()) {
    if (label->text().contains(text)) return true;
  }
  return false;
}

std::uint16_t test_port() {
  return static_cast<std::uint16_t>(46000 +
                                    (QCoreApplication::applicationPid() % 1000) * 2);
}

}  // namespace

TEST(MainWindowTest, StartsRuntimeAutomaticallyOnConstruction) {
  MainWindow window(nullptr, fake_backend_factory());
  auto* toggle = runtime_toggle(window);
  auto* status = runtime_status(window);
  auto* header = window.findChild<QLabel*>(QStringLiteral("runtimeHeaderStatus"));
  ASSERT_NE(toggle, nullptr);
  ASSERT_NE(status, nullptr);
  ASSERT_NE(header, nullptr);

  ASSERT_TRUE(wait_until([&] {
    auto* current_toggle = runtime_toggle(window);
    auto* current_status = runtime_status(window);
    auto* current_header = window.findChild<QLabel*>(QStringLiteral("runtimeHeaderStatus"));
    return current_toggle != nullptr && current_status != nullptr && current_header != nullptr &&
           current_toggle->text() == QStringLiteral("停止服务") &&
           current_header->text() == QStringLiteral("●  运行中") &&
           (current_status->text().contains(QStringLiteral("发现")) ||
            current_status->text().contains(QStringLiteral("监听")) ||
            current_status->text().contains(QStringLiteral("等待")));
  }));
  EXPECT_EQ(label_count(window, QStringLiteral("等待对端设备")), 0);
  EXPECT_FALSE(label_contains(window, QStringLiteral("macOS / Windows")));
#if defined(__APPLE__)
  EXPECT_TRUE(label_contains(window, QStringLiteral("macOS · 监听 ")));
  EXPECT_FALSE(label_contains(window, QStringLiteral("0.0.0.0:")));
#endif
}

TEST(MainWindowTest, DoesNotShowPlaceholderWithoutPeer) {
  MainWindow window(nullptr, fake_backend_factory(), false);

  EXPECT_EQ(label_count(window, QStringLiteral("等待对端设备")), 0);
}

TEST(MainWindowTest, HidesQtConsoleSubtitleFromTitleBar) {
  MainWindow window(nullptr, fake_backend_factory(), false);

  EXPECT_FALSE(label_contains(window, QStringLiteral("Qt6 控制台")));
}

TEST(MainWindowTest, UsesLanLinkIconForWindowAndTray) {
  MainWindow window(nullptr, fake_backend_factory(), false);

  EXPECT_FALSE(window.windowIcon().isNull());
  auto* tray = window.findChild<QSystemTrayIcon*>();
  ASSERT_NE(tray, nullptr);
  EXPECT_FALSE(tray->icon().isNull());
}

TEST(MainWindowTest, UsesLanLinkMarkInTitleBar) {
  MainWindow window(nullptr, fake_backend_factory(), false);

  auto* brand = window.findChild<QLabel*>(QStringLiteral("brandIcon"));
  ASSERT_NE(brand, nullptr);
  EXPECT_FALSE(brand->pixmap().isNull());
}

TEST(MainWindowTest, KeepsSettingsControlsCompactAndReadable) {
  MainWindow window(nullptr, fake_backend_factory(), false);

  auto* direction = window.findChild<QLabel*>(QStringLiteral("topologyDirectionLabel"));
  auto* runtime = runtime_toggle(window);
  ASSERT_NE(direction, nullptr);
  ASSERT_NE(runtime, nullptr);
  EXPECT_TRUE(direction->text().contains(QStringLiteral("由拓扑图决定")));
  EXPECT_GE(runtime->minimumWidth(), 112);
  EXPECT_LE(runtime->maximumHeight(), 44);
}

TEST(MainWindowTest, AddsPeerOnlyAfterPeerSignal) {
  MainWindow window(nullptr, fake_backend_factory(), false);

  ASSERT_EQ(label_count(window, QStringLiteral("等待对端设备")), 0);
  const bool invoked = QMetaObject::invokeMethod(
      &window, "onPeerChanged", Qt::DirectConnection,
      Q_ARG(QString, QStringLiteral("设备 2")),
      Q_ARG(QString, QStringLiteral("LANLink 节点")),
      Q_ARG(QString, QStringLiteral("192.168.1.2:41000")), Q_ARG(qulonglong, 2),
      Q_ARG(qulonglong, 2), Q_ARG(bool, true));

  ASSERT_TRUE(invoked);
  EXPECT_GE(label_count(window, QStringLiteral("设备 2")), 1);
  EXPECT_EQ(label_count(window, QStringLiteral("等待对端设备")), 0);
}

TEST(MainWindowTest, SingleNodeTopologyDoesNotRenderConnectionLine) {
  DesktopState state;
  TopologyCanvas canvas;
  canvas.setState(&state);
  canvas.resize(800, 300);

  const QColor background(QStringLiteral("#141e2a"));
  QImage image(canvas.size(), QImage::Format_ARGB32);
  const auto has_link_pixels = [&]() {
    for (int y = 110; y <= 170; ++y) {
      for (int x = 326; x <= 366; ++x) {
        const QColor pixel = image.pixelColor(x, y);
        if (pixel != background && pixel.green() > background.green()) return true;
      }
    }
    return false;
  };
  const auto render = [&]() {
    image.fill(Qt::transparent);
    canvas.render(&image);
  };

  render();
  EXPECT_FALSE(has_link_pixels());

  state.devices.append(DeviceState{
      QStringLiteral("设备 2"),
      QStringLiteral("macOS"),
      QStringLiteral("192.168.1.2:41000"),
      QColor(QStringLiteral("#4db69e")),
      default_new_device_position(state.devices),
      QSizeF(kTopologyCardWidth, kTopologyCardHeight),
      false,
      true,
      2,
      2,
  });
  render();
  EXPECT_TRUE(has_link_pixels());

  state.devices.removeLast();
  render();
  EXPECT_FALSE(has_link_pixels());
}

TEST(MainWindowTest, LShapeLayoutKeepsCardsSeparatedVertically) {
  DesktopState state;
  state.devices.append(DeviceState{
      QStringLiteral("设备 2"),
      QStringLiteral("macOS"),
      QStringLiteral("192.168.1.2:41000"),
      QColor(QStringLiteral("#4db69e")),
      QPointF(0.0, 0.0),
      QSizeF(kTopologyCardWidth, kTopologyCardHeight),
      false,
      true,
      2,
      2,
  });
  TopologyCanvas canvas;
  canvas.setState(&state);
  canvas.setLayoutMode(false);

  ASSERT_EQ(state.devices.size(), 2);
  EXPECT_EQ(state.devices[0].position, QPointF(kTopologyOriginX, kTopologyOriginY));
  EXPECT_EQ(state.devices[1].position,
            QPointF(kTopologyOriginX, kTopologyOriginY + kTopologyCardHeight + kTopologyCardGap));
}

TEST(MainWindowTest, LShapeLayoutStacksAllDevicesWithoutOverlap) {
  DesktopState state;
  for (int index = 0; index < 2; ++index) {
    state.devices.append(DeviceState{
        QStringLiteral("设备 %1").arg(index + 2),
        QStringLiteral("macOS"),
        QStringLiteral("192.168.1.%1:41000").arg(index + 2),
        QColor(QStringLiteral("#4db69e")),
        QPointF(0.0, 0.0),
        QSizeF(kTopologyCardWidth, kTopologyCardHeight),
        false,
        true,
        static_cast<std::uint64_t>(index + 2),
        static_cast<std::uint64_t>(index + 2),
    });
  }
  TopologyCanvas canvas;
  canvas.setState(&state);
  canvas.setLayoutMode(false);

  ASSERT_EQ(state.devices.size(), 3);
  for (int index = 0; index < state.devices.size(); ++index) {
    EXPECT_EQ(state.devices[index].position,
              QPointF(kTopologyOriginX,
                      kTopologyOriginY +
                          static_cast<double>(index) * (kTopologyCardHeight + kTopologyCardGap)));
  }
}

TEST(MainWindowTest, TopologyLayoutDrivesPeerDirection) {
  MainWindow window(nullptr, fake_backend_factory(), false);
  ASSERT_TRUE(QMetaObject::invokeMethod(
      &window, "onPeerChanged", Qt::DirectConnection,
      Q_ARG(QString, QStringLiteral("设备 2")),
      Q_ARG(QString, QStringLiteral("LANLink 节点")),
      Q_ARG(QString, QStringLiteral("192.168.1.2:41000")), Q_ARG(qulonglong, 2),
      Q_ARG(qulonglong, 2), Q_ARG(bool, true)));

  window.showPage(1);
  window.show();
  QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

  auto* direction = window.findChild<QLabel*>(QStringLiteral("topologyDirectionLabel"));
  ASSERT_NE(direction, nullptr);
  EXPECT_TRUE(direction->text().contains(QStringLiteral("当前在右侧")));

  TopologyCanvas* canvas = nullptr;
  for (auto* candidate : window.findChildren<TopologyCanvas*>()) {
    if (candidate->isVisible()) {
      canvas = candidate;
      break;
    }
  }
  ASSERT_NE(canvas, nullptr);
  ASSERT_GT(canvas->width(), 0);

  const QPointF press(404.0, 106.0);
  const QPointF move(36.0, 106.0);
  QMouseEvent pressEvent(QEvent::MouseButtonPress, press, press, press, Qt::LeftButton,
                         Qt::LeftButton, Qt::NoModifier);
  QMouseEvent moveEvent(QEvent::MouseMove, move, move, move, Qt::NoButton, Qt::LeftButton,
                        Qt::NoModifier);
  QMouseEvent releaseEvent(QEvent::MouseButtonRelease, move, move, move, Qt::LeftButton,
                           Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(canvas, &pressEvent);
  QApplication::sendEvent(canvas, &moveEvent);
  QApplication::sendEvent(canvas, &releaseEvent);

  EXPECT_TRUE(direction->text().contains(QStringLiteral("当前在左侧")));
  window.close();
}

TEST(MainWindowTest, RemovesPeerAfterDiscoveryTimeoutOrDisconnect) {
  MainWindow window(nullptr, fake_backend_factory(), false);

  ASSERT_TRUE(QMetaObject::invokeMethod(
      &window, "onPeerChanged", Qt::DirectConnection,
      Q_ARG(QString, QStringLiteral("设备 2")),
      Q_ARG(QString, QStringLiteral("LANLink 节点")),
      Q_ARG(QString, QStringLiteral("192.168.1.2:41000")), Q_ARG(qulonglong, 2),
      Q_ARG(qulonglong, 2), Q_ARG(bool, false)));
  EXPECT_EQ(label_count(window, QStringLiteral("设备 2")), 0);

  ASSERT_TRUE(QMetaObject::invokeMethod(
      &window, "onPeerChanged", Qt::DirectConnection,
      Q_ARG(QString, QStringLiteral("设备 2")),
      Q_ARG(QString, QStringLiteral("LANLink 节点")),
      Q_ARG(QString, QStringLiteral("192.168.1.2:41000")), Q_ARG(qulonglong, 2),
      Q_ARG(qulonglong, 2), Q_ARG(bool, true)));
  ASSERT_GE(label_count(window, QStringLiteral("设备 2")), 1);

  ASSERT_TRUE(QMetaObject::invokeMethod(
      &window, "onPeerChanged", Qt::DirectConnection,
      Q_ARG(QString, QStringLiteral("设备 2")),
      Q_ARG(QString, QStringLiteral("LANLink 节点")),
      Q_ARG(QString, QStringLiteral("192.168.1.2:41000")), Q_ARG(qulonglong, 2),
      Q_ARG(qulonglong, 2), Q_ARG(bool, false)));

  EXPECT_EQ(label_count(window, QStringLiteral("设备 2")), 0);
}

TEST(MainWindowTest, FileTransferControlsFollowOnlinePeerAndProgressSignals) {
  MainWindow window(nullptr, fake_backend_factory(), false);
  window.showPage(3);
  auto* browse = window.findChild<QPushButton*>(QStringLiteral("transferBrowseButton"));
  auto* drop = window.findChild<QWidget*>(QStringLiteral("transferDropZone"));
  ASSERT_NE(browse, nullptr);
  ASSERT_NE(drop, nullptr);
  EXPECT_FALSE(browse->isEnabled());
  EXPECT_FALSE(drop->isEnabled());

  ASSERT_TRUE(QMetaObject::invokeMethod(
      &window, "onPeerChanged", Qt::DirectConnection,
      Q_ARG(QString, QStringLiteral("设备 2")),
      Q_ARG(QString, QStringLiteral("LANLink 节点")),
      Q_ARG(QString, QStringLiteral("192.168.1.2:41000")), Q_ARG(qulonglong, 2),
      Q_ARG(qulonglong, 2), Q_ARG(bool, true)));
  window.showPage(3);
  browse = window.findChild<QPushButton*>(QStringLiteral("transferBrowseButton"));
  drop = window.findChild<QWidget*>(QStringLiteral("transferDropZone"));
  ASSERT_NE(browse, nullptr);
  ASSERT_NE(drop, nullptr);
  EXPECT_TRUE(browse->isEnabled());
  EXPECT_TRUE(drop->isEnabled());

  ASSERT_TRUE(QMetaObject::invokeMethod(
      &window, "onFileTransferChanged", Qt::DirectConnection,
      Q_ARG(QString, QStringLiteral("report.txt")), Q_ARG(qulonglong, 5),
      Q_ARG(qulonglong, 10), Q_ARG(QString, QStringLiteral("传输中")), Q_ARG(bool, true)));
  auto* status = window.findChild<QLabel*>(QStringLiteral("transferStatusLabel"));
  ASSERT_NE(status, nullptr);
  EXPECT_TRUE(status->text().contains(QStringLiteral("report.txt")));
  EXPECT_TRUE(status->text().contains(QStringLiteral("50.0%")));

  ASSERT_TRUE(QMetaObject::invokeMethod(
      &window, "onFileTransferChanged", Qt::DirectConnection,
      Q_ARG(QString, QStringLiteral("report.txt")), Q_ARG(qulonglong, 10),
      Q_ARG(qulonglong, 10), Q_ARG(QString, QStringLiteral("已完成")), Q_ARG(bool, true)));
  EXPECT_TRUE(status->text().contains(QStringLiteral("已完成")));

  ASSERT_TRUE(QMetaObject::invokeMethod(
      &window, "onPeerChanged", Qt::DirectConnection,
      Q_ARG(QString, QStringLiteral("设备 2")),
      Q_ARG(QString, QStringLiteral("LANLink 节点")),
      Q_ARG(QString, QStringLiteral("192.168.1.2:41000")), Q_ARG(qulonglong, 2),
      Q_ARG(qulonglong, 2), Q_ARG(bool, false)));
  window.showPage(3);
  browse = window.findChild<QPushButton*>(QStringLiteral("transferBrowseButton"));
  ASSERT_NE(browse, nullptr);
  EXPECT_FALSE(browse->isEnabled());
}

TEST(MainWindowTest, RejectsInvalidSettingsWithoutStartingWorker) {
  MainWindow window(nullptr, fake_backend_factory(), false);
  auto* device_id = field(window, QStringLiteral("deviceIdEdit"));
  auto* toggle = runtime_toggle(window);
  auto* status = runtime_status(window);
  ASSERT_NE(device_id, nullptr);
  ASSERT_NE(toggle, nullptr);
  ASSERT_NE(status, nullptr);

  device_id->setText(QStringLiteral("0"));
  toggle->click();

  EXPECT_EQ(toggle->text(), QStringLiteral("启动服务"));
  EXPECT_EQ(status->text(), QStringLiteral("设备 ID 必须是正整数"));
}

TEST(MainWindowTest, StartsAndStopsRuntimeThroughSettingsButton) {
  MainWindow window(nullptr, fake_backend_factory(), false);
  ASSERT_NE(field(window, QStringLiteral("listenPortEdit")), nullptr);
  ASSERT_NE(field(window, QStringLiteral("discoveryPortEdit")), nullptr);
  const auto base_port = test_port();
  field(window, QStringLiteral("listenPortEdit"))->setText(QString::number(base_port));
  field(window, QStringLiteral("discoveryPortEdit"))
      ->setText(QString::number(static_cast<std::uint16_t>(base_port + 1)));
  auto* toggle = runtime_toggle(window);
  auto* status = runtime_status(window);
  auto* network = window.findChild<QLabel*>(QStringLiteral("networkLabel"));
  auto* header = window.findChild<QLabel*>(QStringLiteral("runtimeHeaderStatus"));
  auto* local = window.findChild<QLabel*>(QStringLiteral("localLabel"));
  auto* footer = window.findChild<QLabel*>(QStringLiteral("footerStatusLabel"));
  ASSERT_NE(toggle, nullptr);
  ASSERT_NE(status, nullptr);
  ASSERT_NE(network, nullptr);
  ASSERT_NE(header, nullptr);
  ASSERT_NE(local, nullptr);
  ASSERT_NE(footer, nullptr);

  toggle->click();
  const bool started = wait_until([&] {
    auto* current_status = runtime_status(window);
    return current_status != nullptr &&
           (current_status->text().contains(QStringLiteral("发现")) ||
            current_status->text().contains(QStringLiteral("监听")) ||
            current_status->text().contains(QStringLiteral("等待")));
  });
  ASSERT_TRUE(started) << "runtime status: "
                       << runtime_status(window)->text().toStdString();
  auto* running_toggle = runtime_toggle(window);
  ASSERT_NE(running_toggle, nullptr);
  EXPECT_EQ(running_toggle->text(), QStringLiteral("停止服务"));
  EXPECT_TRUE(network->text().contains(QStringLiteral("局域网")));
  EXPECT_EQ(header->text(), QStringLiteral("●  运行中"));
  EXPECT_TRUE(header->styleSheet().contains(QStringLiteral("#123f36")));
  EXPECT_TRUE(header->styleSheet().contains(QStringLiteral("border: 1px solid")));
  EXPECT_TRUE(local->wordWrap());
  EXPECT_TRUE(local->text().contains(QStringLiteral("监听 ")));
  EXPECT_TRUE(footer->text().startsWith(QStringLiteral("LANLink · ")));

  running_toggle->click();
  ASSERT_TRUE(wait_until([&] {
    auto* current_status = runtime_status(window);
    auto* current_toggle = runtime_toggle(window);
    auto* current_header = window.findChild<QLabel*>(QStringLiteral("runtimeHeaderStatus"));
    return current_status != nullptr && current_toggle != nullptr &&
           current_header != nullptr &&
           current_status->text() == QStringLiteral("已停止") &&
           current_toggle->text() == QStringLiteral("启动服务") &&
           current_header->text() == QStringLiteral("●  未运行");
  }));
  EXPECT_TRUE(window.findChild<QLabel*>(QStringLiteral("runtimeHeaderStatus"))
                  ->styleSheet()
                  .contains(QStringLiteral("#3f3524")));
  EXPECT_EQ(label_count(window, QStringLiteral("等待对端设备")), 0);
}

TEST(MainWindowTest, ShowsWorkerStartupErrorAndRemainsRestartable) {
  MainWindow window(nullptr, failing_backend_factory(), false);
  auto* toggle = runtime_toggle(window);
  auto* status = runtime_status(window);
  ASSERT_NE(toggle, nullptr);
  ASSERT_NE(status, nullptr);

  toggle->click();
  ASSERT_TRUE(wait_until([&] {
    auto* current_status = runtime_status(window);
    auto* current_toggle = runtime_toggle(window);
    return current_status != nullptr && current_toggle != nullptr &&
           current_status->text() == QStringLiteral("错误") &&
           current_toggle->text() == QStringLiteral("启动服务");
  }));
  EXPECT_TRUE(window.findChild<QLabel*>(QStringLiteral("footerStatusLabel"))
                  ->text()
                  .contains(QStringLiteral("错误")));
}

TEST(MainWindowTest, CrossScreenPreferenceCanBeToggled) {
  MainWindow window(nullptr, fake_backend_factory(), false);

  EXPECT_TRUE(label_contains(window, QStringLiteral("跨屏输入运行参数")));
  EXPECT_FALSE(label_contains(window, QStringLiteral("M0 输入引擎参数")));

  const auto checks = window.findChildren<QCheckBox*>();
  // The first check box belongs to the connection form's auto-topology
  // setting; the next three are the preference rows shown in the screenshot.
  ASSERT_GE(checks.size(), 4);
  ASSERT_TRUE(checks[1] != nullptr);
  ASSERT_TRUE(checks[2] != nullptr);
  ASSERT_TRUE(checks[3] != nullptr);

  EXPECT_EQ(checks[1]->sizeHint(), QSize(48, 28));
  EXPECT_TRUE(checks[1]->parentWidget()->objectName() == QStringLiteral("preferenceRow"));
  EXPECT_TRUE(checks[1]->parentWidget()->styleSheet().contains(QStringLiteral("transparent")));
  EXPECT_TRUE(checks[1]->isEnabled());
  EXPECT_TRUE(checks[2]->isEnabled());
  EXPECT_FALSE(checks[3]->isEnabled());
  const auto animations = checks[1]->findChildren<QVariantAnimation*>();
  ASSERT_EQ(animations.size(), 1);
  EXPECT_EQ(animations.front()->duration(), 160);

  auto* footer = window.findChild<QLabel*>(QStringLiteral("footerStatusLabel"));
  ASSERT_NE(footer, nullptr);
  EXPECT_TRUE(checks[1]->isChecked());

  checks[1]->click();
  EXPECT_FALSE(checks[1]->isChecked());
  EXPECT_TRUE(footer->text().contains(QStringLiteral("跨屏切换已暂停")));

  checks[1]->click();
  EXPECT_TRUE(checks[1]->isChecked());
  EXPECT_TRUE(footer->text().contains(QStringLiteral("跨屏切换已启用")));
}

TEST(MainWindowTest, ClipboardPreferenceAndTrayActionAreEnabledAndDefaultOff) {
  MainWindow window(nullptr, fake_backend_factory(), false, fake_clipboard_backend_factory());

  const auto checks = window.findChildren<QCheckBox*>();
  ASSERT_GE(checks.size(), 4);
  EXPECT_TRUE(checks[2]->isEnabled());
  EXPECT_FALSE(checks[2]->isChecked());
  EXPECT_TRUE(label_contains(window, QStringLiteral("剪贴板同步")));
  EXPECT_FALSE(label_contains(window, QStringLiteral("剪贴板同步（未接入）")));

  QAction* clipboard_action = nullptr;
  for (auto* action : window.findChildren<QAction*>()) {
    if (action->text().contains(QStringLiteral("剪贴板同步"))) {
      clipboard_action = action;
      break;
    }
  }
  ASSERT_NE(clipboard_action, nullptr);
  EXPECT_TRUE(clipboard_action->isEnabled());
  EXPECT_FALSE(clipboard_action->isChecked());
}

TEST(MainWindowTest, ClipboardToggleWhileRunningForwardsToRuntimeWorker) {
  MainWindow window(nullptr, fake_backend_factory(), false, fake_clipboard_backend_factory());
  const auto base_port = test_port();
  field(window, QStringLiteral("listenPortEdit"))->setText(QString::number(base_port));
  field(window, QStringLiteral("discoveryPortEdit"))
      ->setText(QString::number(static_cast<std::uint16_t>(base_port + 1)));
  field(window, QStringLiteral("peerIdEdit"))->setText(QStringLiteral("2"));
  field(window, QStringLiteral("connectHostEdit"))->setText(QStringLiteral("127.0.0.1"));
  field(window, QStringLiteral("connectPortEdit"))
      ->setText(QString::number(static_cast<std::uint16_t>(base_port + 2)));
  runtime_toggle(window)->click();
  ASSERT_TRUE(wait_until([&] {
    auto* toggle = runtime_toggle(window);
    return toggle != nullptr && toggle->text() == QStringLiteral("停止服务");
  }));

  const auto checks = window.findChildren<QCheckBox*>();
  ASSERT_GE(checks.size(), 4);
  auto* footer = window.findChild<QLabel*>(QStringLiteral("footerStatusLabel"));
  ASSERT_NE(footer, nullptr);
  checks[2]->click();
  ASSERT_TRUE(wait_until([&] {
    return footer->text().contains(QStringLiteral("剪贴板同步已启用"));
  }));
  EXPECT_TRUE(checks[2]->isChecked());
}
