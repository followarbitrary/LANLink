#include "runtime_worker.hpp"

#include "lanlink/discovery/discovery.hpp"
#include "lanlink/platform/fake_clipboard_backend.hpp"
#include "lanlink/platform/fake_input_backend.hpp"

#include <QCoreApplication>
#include <QMetaObject>
#include <QSignalSpy>
#include <QTest>
#include <QThread>

#include <gtest/gtest.h>

#include <memory>
#include <thread>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

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

void inject_topology(std::uint16_t port, const lanlink::discovery::TopologySnapshot& snapshot) {
  const auto encoded = lanlink::discovery::DiscoveryService::encode_topology(snapshot);
  ASSERT_TRUE(encoded.ok());
#if defined(_WIN32)
  SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  ASSERT_NE(socket, INVALID_SOCKET);
#else
  int socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  ASSERT_GE(socket, 0);
#endif
  sockaddr_in destination{};
  destination.sin_family = AF_INET;
  destination.sin_port = htons(port);
  ASSERT_EQ(inet_pton(AF_INET, "127.0.0.1", &destination.sin_addr), 1);
#if defined(_WIN32)
  const auto sent = sendto(socket, encoded.value().data(), static_cast<int>(encoded.value().size()), 0,
                           reinterpret_cast<sockaddr*>(&destination), sizeof(destination));
  closesocket(socket);
#else
  const auto sent = sendto(socket, encoded.value().data(), encoded.value().size(), 0,
                           reinterpret_cast<sockaddr*>(&destination), sizeof(destination));
  close(socket);
#endif
  ASSERT_EQ(sent, static_cast<decltype(sent)>(encoded.value().size()));
}

struct ClipboardFactoryProbe {
  lanlink::platform::FakeClipboardBackend* backend{};
  lanlink::desktop::RuntimeWorker::ClipboardBackendFactory factory() {
    return [this] {
      auto backend = std::make_unique<lanlink::platform::FakeClipboardBackend>();
      this->backend = backend.get();
      return backend;
    };
  }
};

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

TEST(RuntimeWorkerTest, RejectsFileSendWithoutOnlinePeer) {
  lanlink::desktop::RuntimeWorker worker(nullptr, fake_backend_factory());
  QSignalSpy transfer_spy(&worker, &lanlink::desktop::RuntimeWorker::fileTransferChanged);
  worker.sendFile(QStringLiteral("/tmp/report.txt"));
  ASSERT_EQ(transfer_spy.count(), 1);
  EXPECT_EQ(transfer_spy.at(0).at(0).toString(), QStringLiteral("report.txt"));
  EXPECT_EQ(transfer_spy.at(0).at(3).toString(), QStringLiteral("无在线对端"));
  EXPECT_TRUE(transfer_spy.at(0).at(4).toBool());
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

TEST(RuntimeWorkerThreadTest, StartsClipboardBackendWithConfiguredPreferenceAndTogglesAtRuntime) {
  ClipboardFactoryProbe clipboard_probe;
  lanlink::desktop::RuntimeWorker worker(nullptr, fake_backend_factory(), true,
                                          clipboard_probe.factory());
  QThread thread;
  worker.moveToThread(&thread);
  QSignalSpy error_spy(&worker, &lanlink::desktop::RuntimeWorker::errorOccurred);

  thread.start();
  auto options = worker_options();
  options.clipboard_enabled = true;
  options.connect = lanlink::transport::Endpoint{
      "127.0.0.1", static_cast<std::uint16_t>(options.listen_port + 2)};
  ASSERT_TRUE(QMetaObject::invokeMethod(
      &worker, [&worker, options] { worker.start(options); }, Qt::QueuedConnection));
  QTRY_VERIFY_WITH_TIMEOUT(clipboard_probe.backend != nullptr, 2000);
  QTRY_VERIFY_WITH_TIMEOUT(error_spy.isEmpty(), 500);

  ASSERT_TRUE(QMetaObject::invokeMethod(
      &worker, [&worker] { worker.setClipboardSyncEnabled(false); },
      Qt::BlockingQueuedConnection));
  EXPECT_TRUE(error_spy.isEmpty());
  ASSERT_TRUE(QMetaObject::invokeMethod(
      &worker, [&worker] { worker.setClipboardSyncEnabled(true); },
      Qt::BlockingQueuedConnection));
  EXPECT_TRUE(error_spy.isEmpty());

  ASSERT_TRUE(QMetaObject::invokeMethod(&worker, [&worker] { worker.stop(); },
                                       Qt::BlockingQueuedConnection));
  thread.quit();
  ASSERT_TRUE(thread.wait(2000));
}

TEST(RuntimeWorkerThreadTest, AppliesRemoteTopologyWhileRuntimeIsActive) {
  lanlink::desktop::RuntimeWorker worker(nullptr, fake_backend_factory());
  QThread thread;
  worker.moveToThread(&thread);
  QSignalSpy topology_spy(&worker, &lanlink::desktop::RuntimeWorker::remoteTopologyChanged);
  QSignalSpy status_spy(&worker, &lanlink::desktop::RuntimeWorker::statusChanged);

  thread.start();
  auto options = worker_options();
  options.connect = lanlink::transport::Endpoint{
      "127.0.0.1", static_cast<std::uint16_t>(options.listen_port + 2)};
  ASSERT_TRUE(QMetaObject::invokeMethod(
      &worker, [&worker, options] { worker.start(options); }, Qt::QueuedConnection));
  QTRY_VERIFY_WITH_TIMEOUT(status_spy.count() >= 2, 2000);

  const lanlink::DeviceId local{0, 1};
  const lanlink::DeviceId peer{0, 2};
  const lanlink::DisplayId local_display{0, 1};
  const lanlink::DisplayId peer_display{0, 2};
  lanlink::ScreenTopology topology;
  ASSERT_TRUE(topology
                  .set_snapshot(2,
                                {{local, local_display, {0, 0, 100, 100}, 1.0},
                                 {peer, peer_display, {-200, 0, 0, 100}, 1.0}},
                                {{local_display, lanlink::Edge::kLeft, peer_display,
                                  lanlink::Edge::kRight, 0.0, 1.0, 2}})
                  .ok());
  inject_topology(options.discovery_port,
                  lanlink::discovery::TopologySnapshot{peer, 2, topology, options.group});
  QTRY_VERIFY_WITH_TIMEOUT(topology_spy.count() == 1, 2000);
  EXPECT_EQ(worker.topology_revision(), 2u);

  ASSERT_TRUE(QMetaObject::invokeMethod(&worker, [&worker] { worker.stop(); },
                                        Qt::BlockingQueuedConnection));
  thread.quit();
  ASSERT_TRUE(thread.wait(2000));
}

}  // namespace
