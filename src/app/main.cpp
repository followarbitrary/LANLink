#include "lanlink/app/options.hpp"
#include "lanlink/app/runtime_loop.hpp"
#include "lanlink/common/debug.hpp"
#include "lanlink/discovery/discovery.hpp"
#include "lanlink/runtime/node.hpp"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include "lanlink/platform/windows_clipboard_backend.hpp"
#include "lanlink/platform/windows_input_backend.hpp"
#elif defined(__APPLE__)
#include "lanlink/platform/macos_clipboard_backend.hpp"
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

bool local_id_less(std::uint64_t local, const lanlink::DeviceId& peer) {
  return peer.high == 0 && local < peer.low;
}

bool device_id_less(const lanlink::DeviceId& left, const lanlink::DeviceId& right) {
  return left.high < right.high || (left.high == right.high && left.low < right.low);
}

lanlink::Result<lanlink::ScreenTopology> topology_with_revision(
    const lanlink::ScreenTopology& source, std::uint64_t revision) {
  auto displays = source.displays();
  auto links = source.links();
  for (auto& link : links) link.version = revision;
  lanlink::ScreenTopology updated;
  auto set = updated.set_snapshot(revision, std::move(displays), std::move(links));
  if (!set.ok()) return lanlink::Result<lanlink::ScreenTopology>::failure(set.error());
  return lanlink::Result<lanlink::ScreenTopology>::success(std::move(updated));
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
  std::unique_ptr<lanlink::platform::IClipboardBackend> clipboard_backend;
#if defined(_WIN32)
  backend = std::make_unique<lanlink::platform::WindowsInputBackend>();
  clipboard_backend = std::make_unique<lanlink::platform::WindowsClipboardBackend>();
#elif defined(__APPLE__)
  backend = std::make_unique<lanlink::platform::MacOSInputBackend>();
  clipboard_backend = std::make_unique<lanlink::platform::MacOSClipboardBackend>();
#else
  std::cerr << "error: unsupported platform; LANLink requires Windows or macOS\n";
  return 3;
#endif

  const auto detected_bounds = backend->display_bounds();
  if (!detected_bounds.has_value()) {
    std::cerr << "error: unable to detect the local display bounds\n";
    return 3;
  }
  lanlink::debug::log("local display bounds=", detected_bounds->left, ",", detected_bounds->top,
                      " ", detected_bounds->right, "x", detected_bounds->bottom,
                      " width_override=", options.width_explicit,
                      " height_override=", options.height_explicit);
  if (!options.width_explicit) options.width = detected_bounds->width();
  if (!options.height_explicit) options.height = detected_bounds->height();
  options.left = detected_bounds->left;
  options.top = detected_bounds->top;

  lanlink::discovery::DiscoveryConfig discovery_config;
  discovery_config.group = options.group;
  discovery_config.discovery_port = options.discovery_port;
  discovery_config.device_id = lanlink::app::numeric_id(options.device_id);
  discovery_config.display_id = lanlink::app::numeric_id(options.display_id);
  discovery_config.tcp_port = options.listen_port;
  auto discovery_result = lanlink::discovery::DiscoveryService::bind(discovery_config);
  if (!discovery_result.ok()) {
    std::cerr << "error: " << discovery_result.error() << '\n';
    return 1;
  }
  auto discovery = std::move(discovery_result.value());

  std::signal(SIGINT, request_stop);
  std::signal(SIGTERM, request_stop);

  const auto stop_discovery = [&]() {
    auto stopped = discovery.stop();
    if (!stopped.ok()) std::cerr << "discovery stop error: " << stopped.error() << '\n';
  };

  // A missing peer ID is resolved before constructing the runtime so the
  // discovered display and ID can participate in the initial topology.
  const bool discover_peer_before_start = !options.peer_id.has_value() && !options.connect.has_value();
  const auto poll_discovery = [&]() -> std::optional<lanlink::discovery::DiscoveryPeer> {
    auto peers = discovery.poll(lanlink::IClock::Clock::now());
    if (!peers.ok()) {
      std::cerr << "error: " << peers.error() << '\n';
      stop_requested.store(true, std::memory_order_relaxed);
      return std::nullopt;
    }
    return lanlink::app::select_discovery_peer(peers.value(), options.peer_id);
  };

  auto selected_peer = poll_discovery();
  if (stop_requested.load(std::memory_order_relaxed)) {
    stop_discovery();
    return 1;
  }
  while (discover_peer_before_start && !selected_peer.has_value() &&
         !stop_requested.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
    selected_peer = poll_discovery();
  }
  if (stop_requested.load(std::memory_order_relaxed)) {
    stop_discovery();
    return 0;
  }
  if (discover_peer_before_start && !selected_peer.has_value()) {
    stop_discovery();
    return 1;
  }
  if (selected_peer.has_value()) {
    options.peer_id = selected_peer->device.low;
    if (!options.peer_display_id_explicit) options.peer_display_id = selected_peer->display.low;
    if (!options.remote_side_explicit) {
      options.remote_side = local_id_less(options.device_id, selected_peer->device)
                                ? lanlink::app::RemoteSide::kRight
                                : lanlink::app::RemoteSide::kLeft;
    }
  }
  std::optional<lanlink::DeviceId> selected_device_id;
  if (selected_peer.has_value()) selected_device_id = selected_peer->device;
  bool topology_selected = selected_peer.has_value();

  auto topology = lanlink::app::make_topology(options);
  if (!topology.ok()) {
    std::cerr << "error: " << topology.error() << '\n';
    stop_discovery();
    return 2;
  }

  lanlink::runtime::InputRuntimeConfig config;
  config.local_device = lanlink::app::numeric_id(options.device_id);
  config.source = lanlink::app::numeric_id(options.device_id);
  config.peer = lanlink::AuthenticatedPeer{lanlink::app::numeric_id(*options.peer_id)};
  config.local_display = lanlink::app::numeric_id(options.display_id);
  config.listen_endpoint = lanlink::transport::Endpoint{"0.0.0.0", options.listen_port};
  config.connection = lanlink::transport::ConnectionOptions{};
  config.backend = backend.get();
  config.clipboard_backend = clipboard_backend.get();
  config.clipboard_sync_enabled = options.clipboard_enabled;
  config.queue_capacity = 256;
  config.accept_incoming = options.connect.has_value() || !options.peer_id.has_value() ||
                           selected_peer.has_value();

  auto runtime_result = lanlink::runtime::InputNodeRuntime::create(std::move(config));
  if (!runtime_result.ok()) {
    std::cerr << "error: " << runtime_result.error() << '\n';
    stop_discovery();
    return 1;
  }
  auto runtime = std::move(runtime_result.value());

  auto local_topology = topology.value();
  std::uint64_t local_topology_revision = local_topology.version();
  auto set_topology = runtime->set_topology(local_topology);
  if (!set_topology.ok()) {
    std::cerr << "error: " << set_topology.error() << '\n';
    stop_discovery();
    (void)runtime->stop(lanlink::IClock::Clock::now());
    return 1;
  }
  auto started = runtime->start();
  if (!started.ok()) {
    std::cerr << "error: " << started.error() << '\n';
    stop_discovery();
    (void)runtime->stop(lanlink::IClock::Clock::now());
    return 1;
  }

  const auto broadcast_local_topology = [&]() {
    auto result = discovery.broadcast_topology(local_topology, local_topology_revision);
    if (!result.ok()) std::cerr << "topology broadcast error: " << result.error() << '\n';
    return result.ok();
  };
  if (!broadcast_local_topology()) {
    stop_discovery();
    (void)runtime->stop(lanlink::IClock::Clock::now());
    return 1;
  }

  std::optional<std::pair<std::uint64_t, lanlink::DeviceId>> latest_remote_topology;
  const auto apply_remote_topologies = [&]() {
    const lanlink::discovery::TopologySnapshot* newest = nullptr;
    for (const auto& snapshot : discovery.topology_snapshots()) {
      if (snapshot.origin == lanlink::app::numeric_id(options.device_id)) continue;
      const std::pair<std::uint64_t, lanlink::DeviceId> key{snapshot.revision, snapshot.origin};
      if (latest_remote_topology.has_value() &&
          !(latest_remote_topology->first < key.first ||
            (latest_remote_topology->first == key.first &&
             device_id_less(latest_remote_topology->second, key.second)))) {
        continue;
      }
      if (newest == nullptr || newest->revision < snapshot.revision ||
          (newest->revision == snapshot.revision &&
           device_id_less(newest->origin, snapshot.origin))) {
        newest = &snapshot;
      }
    }
    if (newest == nullptr) return true;
    auto updated = runtime->set_topology(newest->topology);
    if (!updated.ok()) {
      std::cerr << "remote topology update error: " << updated.error() << '\n';
      return false;
    }
    latest_remote_topology = std::pair<std::uint64_t, lanlink::DeviceId>{newest->revision,
                                                                          newest->origin};
    local_topology_revision = std::max(local_topology_revision, newest->revision);
    return true;
  };

  bool discovery_connect_requested = false;
  if (options.connect.has_value()) {
    auto connected = runtime->connect_peer(*options.connect);
    if (!connected.ok()) {
      std::cerr << "error: " << connected.error() << '\n';
      stop_discovery();
      (void)runtime->stop(lanlink::IClock::Clock::now());
      return 1;
    }
    discovery_connect_requested = true;
  } else if (selected_peer.has_value() && local_id_less(options.device_id, selected_peer->device)) {
    auto connected = runtime->connect_peer(selected_peer->endpoint);
    if (!connected.ok()) {
      std::cerr << "error: " << connected.error() << '\n';
      stop_discovery();
      (void)runtime->stop(lanlink::IClock::Clock::now());
      return 1;
    }
    discovery_connect_requested = true;
  } else if (selected_peer.has_value()) {
    discovery_connect_requested = true;
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
    auto discovered = discovery.poll(now);
    if (!discovered.ok()) {
      std::cerr << "discovery error: " << discovered.error() << '\n';
    } else if (!options.connect.has_value()) {
      const auto candidate = lanlink::app::select_discovery_peer(discovered.value(), options.peer_id);
      if (candidate.has_value() &&
          (!selected_device_id.has_value() || candidate->device == *selected_device_id)) {
        const auto& peer = *candidate;
        if (!topology_selected) {
          if (!options.peer_display_id_explicit) options.peer_display_id = peer.display.low;
          if (!options.remote_side_explicit) {
            options.remote_side = local_id_less(options.device_id, peer.device)
                                      ? lanlink::app::RemoteSide::kRight
                                      : lanlink::app::RemoteSide::kLeft;
          }
          auto selected_topology = lanlink::app::make_topology(options);
          if (!selected_topology.ok()) {
            std::cerr << "error: " << selected_topology.error() << '\n';
            stop_requested.store(true, std::memory_order_relaxed);
            continue;
          }
          const auto next_revision =
              std::max(local_topology_revision, local_topology.version()) + 1;
          auto revised = topology_with_revision(selected_topology.value(), next_revision);
          if (!revised.ok()) {
            std::cerr << "error: " << revised.error() << '\n';
            stop_requested.store(true, std::memory_order_relaxed);
            continue;
          }
          auto updated = runtime->set_topology(revised.value());
          if (!updated.ok()) {
            std::cerr << "error: " << updated.error() << '\n';
            stop_requested.store(true, std::memory_order_relaxed);
            continue;
          }
          local_topology = revised.value();
          local_topology_revision = next_revision;
          (void)broadcast_local_topology();
          topology_selected = true;
          selected_device_id = peer.device;
          auto enabled = runtime->set_accept_incoming(true);
          if (!enabled.ok()) {
            std::cerr << "error: " << enabled.error() << '\n';
            stop_requested.store(true, std::memory_order_relaxed);
            continue;
          }
        }
        selected_peer = peer;
        if (discovery_connect_requested && local_id_less(options.device_id, peer.device)) {
          auto refreshed = runtime->update_peer_endpoint(peer.endpoint);
          if (!refreshed.ok()) {
            std::cerr << "discovery endpoint refresh error: " << refreshed.error() << '\n';
          }
        }
        // The lower device ID owns the outgoing connection. The higher ID
        // remains a listener and accepts the reciprocal connection.
        if (!discovery_connect_requested) {
          if (local_id_less(options.device_id, peer.device)) {
            auto connected = runtime->connect_peer(peer.endpoint);
            if (!connected.ok()) {
              std::cerr << "error: " << connected.error() << '\n';
            } else {
              discovery_connect_requested = true;
            }
          } else {
            discovery_connect_requested = true;
          }
        }
      }
    }
    if (!apply_remote_topologies()) {
      stop_requested.store(true, std::memory_order_relaxed);
      continue;
    }
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

  stop_discovery();
  auto stopped = runtime->stop(lanlink::IClock::Clock::now());
  if (!stopped.ok()) {
    std::cerr << "runtime stop error: " << stopped.error() << '\n';
    exit_code = 1;
  }
  return exit_code;
}
