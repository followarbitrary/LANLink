#include "main_window.hpp"

#include "lanlink/platform/fake_input_backend.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPushButton>
#include <QThread>

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
  auto* footer = window.findChild<QLabel*>(QStringLiteral("footerStatusLabel"));
  ASSERT_NE(toggle, nullptr);
  ASSERT_NE(status, nullptr);
  ASSERT_NE(network, nullptr);
  ASSERT_NE(header, nullptr);
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

  const auto checks = window.findChildren<QCheckBox*>();
  // The first check box belongs to the connection form's auto-topology
  // setting; the next three are the preference rows shown in the screenshot.
  ASSERT_GE(checks.size(), 4);
  ASSERT_TRUE(checks[1] != nullptr);
  ASSERT_TRUE(checks[2] != nullptr);
  ASSERT_TRUE(checks[3] != nullptr);

  EXPECT_TRUE(checks[1]->isEnabled());
  EXPECT_FALSE(checks[2]->isEnabled());
  EXPECT_FALSE(checks[3]->isEnabled());

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
