#include "runtime_worker.hpp"

#include <gtest/gtest.h>

namespace {

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

}  // namespace
