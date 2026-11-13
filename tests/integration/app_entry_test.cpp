#include "lanlink/discovery/discovery.hpp"
#include "lanlink/platform/fake_input_backend.hpp"
#include "lanlink/runtime/node.hpp"
#include "lanlink/topology/topology.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <stdexcept>
#include <cstdint>
#include <thread>

#if defined(_WIN32)
#include <winsock2.h>
#include <process.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

std::string quote_shell(const std::string& value) {
#if defined(_WIN32)
  std::string quoted{"\""};
  for (const char character : value) {
    if (character == '"') quoted += '\\';
    quoted += character;
  }
  quoted += '"';
  return quoted;
#else
  std::string quoted{"'"};
  for (const char character : value) {
    if (character == '\'') {
      quoted += "'\\''";
    } else {
      quoted += character;
    }
  }
  quoted += '\'';
  return quoted;
#endif
}

unsigned long long process_id() {
#if defined(_WIN32)
  return static_cast<unsigned long long>(_getpid());
#else
  return static_cast<unsigned long long>(getpid());
#endif
}

lanlink::Uuid128 id(std::uint64_t value) { return {0, value}; }

void inject(std::uint16_t port, std::string_view packet) {
#if defined(_WIN32)
  SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (socket == INVALID_SOCKET) throw std::runtime_error("discovery test socket failed");
#else
  int socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (socket < 0) throw std::runtime_error("discovery test socket failed");
#endif
  sockaddr_in destination{};
  destination.sin_family = AF_INET;
  destination.sin_port = htons(port);
  if (inet_pton(AF_INET, "127.0.0.1", &destination.sin_addr) != 1)
    throw std::runtime_error("discovery test address failed");
#if defined(_WIN32)
  const auto sent = sendto(socket, packet.data(), static_cast<int>(packet.size()), 0,
                           reinterpret_cast<sockaddr*>(&destination), sizeof(destination));
  closesocket(socket);
#else
  const auto sent = sendto(socket, packet.data(), packet.size(), 0,
                           reinterpret_cast<sockaddr*>(&destination), sizeof(destination));
  close(socket);
#endif
  if (sent != static_cast<decltype(sent)>(packet.size()))
    throw std::runtime_error("discovery test send failed");
}

bool running_topology_update_case();

int discovery_flow_case() {
  const auto port = static_cast<std::uint16_t>(43000u + (process_id() % 1000u));
  lanlink::discovery::DiscoveryConfig first;
  first.group = "entry-test";
  first.discovery_port = port;
  first.device_id = id(10);
  first.display_id = id(110);
  first.tcp_port = 44110;
  first.announce_interval = std::chrono::hours(1);
  first.peer_ttl = std::chrono::seconds(2);
  auto second = first;
  second.discovery_port = static_cast<std::uint16_t>(port + 1);
  second.device_id = id(2);
  second.display_id = id(22);
  second.tcp_port = 44002;

  auto first_service = lanlink::discovery::DiscoveryService::bind(first);
  auto second_service = lanlink::discovery::DiscoveryService::bind(second);
  if (!first_service.ok() || !second_service.ok()) {
    std::cerr << "unable to bind two discovery services " << first_service.ok() << " "
              << second_service.ok() << "\n";
    return 1;
  }
  if (!first_service.value().poll(lanlink::IClock::time_point{}).ok() ||
      !second_service.value().poll(lanlink::IClock::time_point{}).ok()) {
    std::cerr << "initial poll failed\n";
    return 1;
  }

  auto second_packet = lanlink::discovery::DiscoveryService::encode_announcement(second);
  auto first_packet = lanlink::discovery::DiscoveryService::encode_announcement(first);
  if (!second_packet.ok() || !first_packet.ok()) {
    std::cerr << "encode failed\n";
    return 1;
  }
  inject(port, second_packet.value());
  inject(second.discovery_port, first_packet.value());
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  auto first_peers = first_service.value().poll(lanlink::IClock::time_point{} +
                                                 std::chrono::milliseconds(1));
  auto second_peers = second_service.value().poll(lanlink::IClock::time_point{} +
                                                  std::chrono::milliseconds(1));
  if (!first_peers.ok() || !second_peers.ok() || first_peers.value().size() != 1 ||
      second_peers.value().size() != 1) {
    std::cerr << "first peer counts " << first_peers.ok() << ":"
              << (first_peers.ok() ? first_peers.value().size() : 0) << " "
              << second_peers.ok() << ":"
              << (second_peers.ok() ? second_peers.value().size() : 0) << "\n";
    return 1;
  }
  if (first_peers.value()[0].device != id(2) ||
      first_peers.value()[0].display != id(22) ||
      first_peers.value()[0].endpoint.port != 44002) {
    std::cerr << "first peer mismatch\n";
    return 1;
  }

  // An explicitly requested peer may announce after startup's first poll.
  // The next nonblocking poll must expose its endpoint and display before
  // topology selection proceeds.
  auto delayed = first;
  delayed.discovery_port = static_cast<std::uint16_t>(port + 2);
  auto delayed_service = lanlink::discovery::DiscoveryService::bind(delayed);
  if (!delayed_service.ok()) {
    std::cerr << "delayed service bind failed\n";
    return 1;
  }
  auto delayed_empty = delayed_service.value().poll(lanlink::IClock::time_point{});
  if (!delayed_empty.ok() || !delayed_empty.value().empty()) {
    std::cerr << "delayed service was not initially empty\n";
    return 1;
  }
  inject(delayed.discovery_port, second_packet.value());
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  auto delayed_peers = delayed_service.value().poll(lanlink::IClock::time_point{} +
                                                     std::chrono::milliseconds(1));
  if (!delayed_peers.ok() || delayed_peers.value().size() != 1 ||
      delayed_peers.value()[0].device != id(2) ||
      delayed_peers.value()[0].display != id(22) ||
      delayed_peers.value()[0].endpoint.port != 44002) {
    std::cerr << "delayed peer selection failed\n";
    return 1;
  }
  if (!delayed_service.value().stop().ok()) return 1;

  // DiscoveryService filters by group. The app's deterministic choice is the
  // first visible peer because the service returns peers sorted by device ID.
  auto wrong_group = second;
  wrong_group.group = "other-group";
  wrong_group.device_id = id(1);
  auto higher_peer = second;
  higher_peer.device_id = id(5);
  auto wrong_packet = lanlink::discovery::DiscoveryService::encode_announcement(wrong_group);
  auto higher_packet = lanlink::discovery::DiscoveryService::encode_announcement(higher_peer);
  if (!wrong_packet.ok() || !higher_packet.ok()) {
    std::cerr << "secondary encode failed\n";
    return 1;
  }
  inject(first.discovery_port, wrong_packet.value());
  inject(first.discovery_port, higher_packet.value());
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  first_peers = first_service.value().poll(lanlink::IClock::time_point{} +
                                            std::chrono::milliseconds(2));
  if (!first_peers.ok() || first_peers.value().size() != 2 ||
      first_peers.value()[0].device != id(2) || first_peers.value()[1].device != id(5)) {
    std::cerr << "sorted peer mismatch " << first_peers.ok() << ":"
              << (first_peers.ok() ? first_peers.value().size() : 0) << "\n";
    return 1;
  }

  if (first_service.value().stop().ok() == false || second_service.value().stop().ok() == false)
    return 1;
  if (!running_topology_update_case()) {
    std::cerr << "running topology update failed\n";
    return 1;
  }
  return 0;
}

bool running_topology_update_case() {
  lanlink::platform::FakeInputBackend backend;
  lanlink::runtime::InputRuntimeConfig config;
  config.local_device = id(1);
  config.source = id(1);
  config.peer = lanlink::AuthenticatedPeer{id(2)};
  config.local_display = id(11);
  config.listen_endpoint = {"127.0.0.1", 0};
  config.backend = &backend;
  auto runtime_result = lanlink::runtime::InputNodeRuntime::create(config);
  if (!runtime_result.ok()) return false;
  auto runtime = std::move(runtime_result.value());

  lanlink::ScreenTopology initial;
  if (!initial.set_snapshot(1,
                            {{id(1), id(11), {0, 0, 100, 100}, 1.0},
                             {id(2), id(22), {100, 0, 200, 100}, 1.0}},
                            {{id(11), lanlink::Edge::kRight, id(22), lanlink::Edge::kLeft,
                              0.0, 1.0, 1}})
           .ok())
    return false;
  if (!runtime->set_topology(std::move(initial)).ok() || !runtime->start().ok()) return false;

  lanlink::ScreenTopology discovered;
  if (!discovered.set_snapshot(2,
                               {{id(1), id(11), {0, 0, 100, 100}, 1.0},
                                {id(2), id(44), {100, 0, 300, 100}, 1.0}},
                               {{id(11), lanlink::Edge::kRight, id(44), lanlink::Edge::kLeft,
                                 0.0, 1.0, 2}})
            .ok())
    return false;
  const bool updated = runtime->set_topology(std::move(discovered)).ok();
  const bool stopped = runtime->stop(lanlink::IClock::Clock::now()).ok();
  return updated && stopped;
}

int run_case(const std::string& executable, const std::string& arguments,
             bool expect_success, const std::string& expected_output) {
  static std::atomic<unsigned long long> sequence{0};
  const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto unique_sequence = sequence.fetch_add(1, std::memory_order_relaxed);
  const auto output_path = std::filesystem::temp_directory_path() /
                           ("lanlink_app_entry_test_" +
                            std::to_string(static_cast<unsigned long long>(timestamp)) + "_" +
                            std::to_string(process_id()) + "_" +
                            std::to_string(unique_sequence) +
                            ".txt");
  const std::string command = quote_shell(executable) + " " + arguments + " > " +
                              quote_shell(output_path.string()) + " 2>&1";
  const int status = std::system(command.c_str());

  std::ifstream output_file(output_path);
  const std::string output((std::istreambuf_iterator<char>(output_file)),
                           std::istreambuf_iterator<char>());
  std::error_code ignored;
  std::filesystem::remove(output_path, ignored);

  const bool succeeded = status == 0;
  if (succeeded != expect_success) {
    std::cerr << "unexpected exit status " << status << "\n" << output;
    return 1;
  }
  if (output.find(expected_output) == std::string::npos) {
    std::cerr << "expected output containing " << expected_output << "\n" << output;
    return 1;
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: app_entry_test <lanlink_app> <help|invalid-port|discovery-flow>\n";
    return 2;
  }
  const std::string executable = argv[1];
  const std::string mode = argv[2];
  if (mode == "help") return run_case(executable, "--help", true, "--discovery-port");
  if (mode == "invalid-port") {
    return run_case(executable,
                    "--device-id 1 --peer-id 2 --listen-port 0",
                    false, "--listen-port");
  }
  if (mode == "discovery-flow") return discovery_flow_case();
  std::cerr << "unknown smoke-test mode: " << mode << '\n';
  return 2;
}
