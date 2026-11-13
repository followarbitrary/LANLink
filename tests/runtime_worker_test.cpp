#include "runtime_worker.hpp"

#include "lanlink/platform/fake_input_backend.hpp"

#include <QCoreApplication>
#include <QMetaObject>
#include <QSignalSpy>
#include <QTest>
#include <QThread>

#include <gtest/gtest.h>

#include <memory>

namespace {

class QtCoreEnvironment final : public testing::Environment {
 public:
  void SetUp() override {
    int argc = 1;
    static char application_name[] = "runtime_worker_test";
    static char* argv[] = {application_name, nullptr};
    app_ = std::make_unique<QCoreApplication>(argc, argv);
  }

  void TearDown() override { app_.reset(); }

 private:
  std::unique_ptr<QCoreApplication> app_;
};

testing::Environment* const qt_core_environment =
    testing::AddGlobalTestEnvironment(new QtCoreEnvironment);

lanlink::app::AppOptions worker_options() {
  const auto base_port = static_cast<std::uint16_t>(46000 +
                                                    (QCoreApplication::applicationPid() % 1000) * 2);
  lanlink::app::AppOptions options;
  options.device_id = 1;
  options.peer_id = 2;
  options.listen_port = base_port;
  options.discovery_port = static_cast<std::uint16_t>(base_port + 1);
  options.peer_display_id = 2;
  return options;
}

lanlink::desktop::RuntimeWorker::BackendFactory fake_backend_factory() {
  return [] {
    auto backend = std::make_unique<lanlink::platform::FakeInputBackend>();
    backend->set_display_bounds(lanlink::Rect{0.0, 0.0, 100.0, 100.0});
    return backend;
  };
}

TEST(RuntimeWorkerTest, RejectsZeroDeviceIdBeforeStarting) {
  lanlink::app::AppOptions options;
  options.listen_port = 41000;

  const auto result = lanlink::desktop::RuntimeWorker::validate_options(options);

  EXPECT_FALSE(result.ok());
  EXPECT_NE(result.error().find("device"), std::string::npos);
}

TEST(RuntimeWorkerTest, RejectsMissingListenPortBeforeStarting) {
  lanlink::app::AppOptions options;
  options.device_id = 1;

  const auto result = lanlink::desktop::RuntimeWorker::validate_options(options);

  EXPECT_FALSE(result.ok());
  EXPECT_NE(result.error().find("listen"), std::string::npos);
}

TEST(RuntimeWorkerTest, AcceptsDiscoveryConfiguration) {
  lanlink::app::AppOptions options;
  options.device_id = 1;
  options.listen_port = 41000;

  const auto result = lanlink::desktop::RuntimeWorker::validate_options(options);

  EXPECT_TRUE(result.ok());
}

TEST(RuntimeWorkerTest, RejectsExplicitConnectionWithoutPeerId) {
  lanlink::app::AppOptions options;
  options.device_id = 1;
  options.listen_port = 41000;
  options.connect = lanlink::transport::Endpoint{"127.0.0.1", 41000};

  const auto result = lanlink::desktop::RuntimeWorker::validate_options(options);

  EXPECT_FALSE(result.ok());
  EXPECT_NE(result.error().find("peer"), std::string::npos);
}

TEST(RuntimeWorkerTest, RejectsInvalidExplicitEndpoint) {
  lanlink::app::AppOptions options;
  options.device_id = 1;
  options.peer_id = 2;
  options.listen_port = 41000;
  options.connect = lanlink::transport::Endpoint{"", 0};

  const auto result = lanlink::desktop::RuntimeWorker::validate_options(options);

  EXPECT_FALSE(result.ok());
  EXPECT_NE(result.error().find("endpoint"), std::string::npos);
}

TEST(RuntimeWorkerTest, RejectsInvalidDiscoveryGroup) {
  lanlink::app::AppOptions options;
  options.device_id = 1;
  options.listen_port = 41000;
  options.group = "lan link";

  const auto result = lanlink::desktop::RuntimeWorker::validate_options(options);

  EXPECT_FALSE(result.ok());
  EXPECT_NE(result.error().find("group"), std::string::npos);
}

TEST(RuntimeWorkerThreadTest, StartsAndStopsOnItsWorkerThread) {
  lanlink::desktop::RuntimeWorker worker(nullptr, fake_backend_factory());
  QThread thread;
  worker.moveToThread(&thread);
  QSignalSpy status_spy(&worker, &lanlink::desktop::RuntimeWorker::statusChanged);
  QSignalSpy finished_spy(&worker, &lanlink::desktop::RuntimeWorker::finished);
  QSignalSpy peer_spy(&worker, &lanlink::desktop::RuntimeWorker::peerChanged);
  QSignalSpy log_spy(&worker, &lanlink::desktop::RuntimeWorker::logMessage);

  thread.start();
  auto options = worker_options();
  options.connect = lanlink::transport::Endpoint{
      "127.0.0.1", static_cast<std::uint16_t>(options.listen_port + 2)};
  ASSERT_TRUE(QMetaObject::invokeMethod(
      &worker, [&worker, options] { worker.start(options); }, Qt::QueuedConnection));
  QTRY_VERIFY_WITH_TIMEOUT(status_spy.count() >= 3, 2000);
  QTRY_VERIFY_WITH_TIMEOUT(peer_spy.count() >= 1, 1000);
  QTRY_VERIFY_WITH_TIMEOUT(log_spy.count() >= 1, 1000);
  EXPECT_TRUE(status_spy.at(1).at(1).toBool());
  EXPECT_FALSE(peer_spy.last().at(5).toBool());
  EXPECT_TRUE(log_spy.at(0).at(0).toString().contains(QStringLiteral("已监听")));

  ASSERT_TRUE(QMetaObject::invokeMethod(&worker, [&worker] { worker.stop(); },
                                       Qt::BlockingQueuedConnection));
  thread.quit();
  ASSERT_TRUE(thread.wait(2000));
  EXPECT_EQ(finished_spy.count(), 1);
  ASSERT_FALSE(status_spy.isEmpty());
  EXPECT_EQ(status_spy.last().at(0).toString(), QStringLiteral("已停止"));
}

TEST(RuntimeWorkerThreadTest, DeliversRecoveryLogThroughQueuedCall) {
  lanlink::desktop::RuntimeWorker worker(nullptr, fake_backend_factory());
  QThread thread;
  worker.moveToThread(&thread);
  QSignalSpy status_spy(&worker, &lanlink::desktop::RuntimeWorker::statusChanged);
  QSignalSpy log_spy(&worker, &lanlink::desktop::RuntimeWorker::logMessage);

  thread.start();
  const auto options = worker_options();
  ASSERT_TRUE(QMetaObject::invokeMethod(
      &worker, [&worker, options] { worker.start(options); }, Qt::QueuedConnection));
  QTRY_VERIFY_WITH_TIMEOUT(status_spy.count() > 0, 2000);

  ASSERT_TRUE(QMetaObject::invokeMethod(&worker, [&worker] { worker.recoverLocalInput(); },
                                       Qt::BlockingQueuedConnection));
  QTRY_VERIFY_WITH_TIMEOUT(log_spy.count() > 0, 1000);
  EXPECT_EQ(log_spy.last().at(0).toString(), QStringLiteral("本地输入已恢复"));

  ASSERT_TRUE(QMetaObject::invokeMethod(&worker, [&worker] { worker.stop(); },
                                       Qt::BlockingQueuedConnection));
  thread.quit();
  ASSERT_TRUE(thread.wait(2000));
}

TEST(RuntimeWorkerThreadTest, InvalidStartReportsErrorAndCanBeStopped) {
  lanlink::desktop::RuntimeWorker worker(nullptr, fake_backend_factory());
  QThread thread;
  worker.moveToThread(&thread);
  QSignalSpy status_spy(&worker, &lanlink::desktop::RuntimeWorker::statusChanged);
  QSignalSpy error_spy(&worker, &lanlink::desktop::RuntimeWorker::errorOccurred);

  thread.start();
  auto options = worker_options();
  options.group = "invalid group";
  ASSERT_TRUE(QMetaObject::invokeMethod(
      &worker, [&worker, options] { worker.start(options); }, Qt::QueuedConnection));
  QTRY_VERIFY_WITH_TIMEOUT(error_spy.count() == 1, 2000);
  QTRY_VERIFY_WITH_TIMEOUT(status_spy.count() == 1, 1000);
  EXPECT_FALSE(status_spy.last().at(1).toBool());
  EXPECT_EQ(status_spy.last().at(0).toString(), QStringLiteral("错误"));

  ASSERT_TRUE(QMetaObject::invokeMethod(&worker, [&worker] { worker.stop(); },
                                       Qt::BlockingQueuedConnection));
  thread.quit();
  ASSERT_TRUE(thread.wait(2000));
}

}  // namespace
