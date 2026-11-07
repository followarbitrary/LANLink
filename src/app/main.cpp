#include "lanlink/app/options.hpp"
#include "lanlink/app/runtime_loop.hpp"
#include "lanlink/runtime/node.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <string_view>
#include <thread>

#if defined(_WIN32)
#include "lanlink/platform/windows_input_backend.hpp"
#elif defined(__APPLE__)
#include "lanlink/platform/macos_input_backend.hpp"
#endif

namespace {

std::atomic<bool> stop_requested{false};

void request_stop(int) noexcept { stop_requested.store(true, std::memory_order_relaxed); }

bool has_help_flag(int argc, char* const argv[]) {
  for (int index = 1; index < argc; ++index) {
    if (argv[index] != nullptr && std::string_view(argv[index]) == "--help") return true;
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  if (has_help_flag(argc, argv)) {
    std::cout << lanlink::app::usage();
    return 0;
  }

  const auto parsed = lanlink::app::parse_options(argc, argv);
  if (!parsed.ok()) {
    std::cerr << "error: " << parsed.error() << '\n';
    return 2;
  }
  auto options = parsed.value();

  std::unique_ptr<lanlink::platform::IInputBackend> backend;
#if defined(_WIN32)
  backend = std::make_unique<lanlink::platform::WindowsInputBackend>();
#elif defined(__APPLE__)
  backend = std::make_unique<lanlink::platform::MacOSInputBackend>();
#else
  std::cerr << "error: unsupported platform; LANLink requires Windows or macOS\n";
  return 3;
#endif

  const auto detected_bounds = backend->display_bounds();
  if (!detected_bounds.has_value()) {
    std::cerr << "error: unable to detect the local display bounds\n";
    return 3;
  }
  if (!options.width_explicit) options.width = detected_bounds->width();
  if (!options.height_explicit) options.height = detected_bounds->height();
  options.left = detected_bounds->left;
  options.top = detected_bounds->top;

  auto topology = lanlink::app::make_topology(options);
  if (!topology.ok()) {
    std::cerr << "error: " << topology.error() << '\n';
    return 2;
  }

  lanlink::runtime::InputRuntimeConfig config;
  config.local_device = lanlink::app::numeric_id(options.device_id);
  config.source = lanlink::app::numeric_id(options.device_id);
  config.peer = lanlink::AuthenticatedPeer{lanlink::app::numeric_id(options.peer_id)};
  config.local_display = lanlink::app::numeric_id(options.display_id);
  config.listen_endpoint = lanlink::transport::Endpoint{"0.0.0.0", options.listen_port};
  config.connection = lanlink::transport::ConnectionOptions{};
  config.backend = backend.get();
  config.queue_capacity = 256;

  auto runtime_result = lanlink::runtime::InputNodeRuntime::create(std::move(config));
  if (!runtime_result.ok()) {
    std::cerr << "error: " << runtime_result.error() << '\n';
    return 1;
  }
  auto runtime = std::move(runtime_result.value());

  auto set_topology = runtime->set_topology(std::move(topology.value()));
  if (!set_topology.ok()) {
    std::cerr << "error: " << set_topology.error() << '\n';
    (void)runtime->stop(lanlink::IClock::Clock::now());
    return 1;
  }
  std::signal(SIGINT, request_stop);
  std::signal(SIGTERM, request_stop);
  auto started = runtime->start();
  if (!started.ok()) {
    std::cerr << "error: " << started.error() << '\n';
    (void)runtime->stop(lanlink::IClock::Clock::now());
    return 1;
  }

  if (options.connect.has_value()) {
    auto connected = runtime->connect_peer(*options.connect);
    if (!connected.ok()) {
      std::cerr << "error: " << connected.error() << '\n';
      (void)runtime->stop(lanlink::IClock::Clock::now());
      return 1;
    }
  }

  const auto endpoint = runtime->local_endpoint();
  std::cout << "LANLink listening on " << endpoint.host << ':' << endpoint.port << '\n';

  int exit_code = 0;
  bool was_connected = runtime->connected();
  if (was_connected) {
    std::cout << "LANLink connected" << '\n';
  }
  while (!stop_requested.load(std::memory_order_relaxed)) {
    const auto now = lanlink::IClock::Clock::now();
    auto polled = runtime->poll(now, std::chrono::milliseconds{10});
    if (!polled.ok()) {
      if (lanlink::app::is_shutdown_interrupt(
              stop_requested.load(std::memory_order_relaxed), polled.error())) {
        break;
      }
      std::cerr << "runtime error: " << polled.error() << '\n';
      // ConnectionManager has already closed the failed session and either
      // scheduled an outgoing retry or discarded the incoming peer. Keep the
      // listener and input backend alive for transient peer/socket failures.
    }
    auto ticked = runtime->tick(now);
    if (!ticked.ok()) {
      if (lanlink::app::is_shutdown_interrupt(
              stop_requested.load(std::memory_order_relaxed), ticked.error())) {
        break;
      }
      std::cerr << "runtime error: " << ticked.error() << '\n';
      // The manager keeps reconnect/backoff state; continue serving the node.
    }
    const bool connected = runtime->connected();
    if (connected != was_connected) {
      std::cout << (connected ? "LANLink connected" : "LANLink disconnected") << '\n';
      was_connected = connected;
    }
    if (!stop_requested.load(std::memory_order_relaxed)) {
      std::this_thread::sleep_for(lanlink::app::idle_wait(connected));
    }
  }

  auto stopped = runtime->stop(lanlink::IClock::Clock::now());
  if (!stopped.ok()) {
    std::cerr << "runtime stop error: " << stopped.error() << '\n';
    exit_code = 1;
  }
  return exit_code;
}
