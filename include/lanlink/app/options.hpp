#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "lanlink/common/ids.hpp"
#include "lanlink/common/result.hpp"
#include "lanlink/discovery/discovery.hpp"
#include "lanlink/topology/topology.hpp"
#include "lanlink/transport/connection.hpp"
namespace lanlink::app {
enum class RemoteSide { kLeft, kRight };
struct AppOptions {
  std::uint64_t device_id{};
  std::optional<std::uint64_t> peer_id;
  std::uint64_t display_id{1};
  std::uint64_t peer_display_id{2};
  std::uint16_t listen_port{};
  std::string group{"lanlink"};
  std::uint16_t discovery_port{41001};
  std::optional<transport::Endpoint> connect;
  bool clipboard_enabled{false};
  RemoteSide remote_side{RemoteSide::kRight};
  bool peer_display_id_explicit{false};
  bool remote_side_explicit{false};
  // Zero means detect the local display bounds from the native backend.
  double width{};
  double height{};
  double peer_width{};
  double peer_height{};
  bool width_explicit{false};
  bool height_explicit{false};
  // Native display origin, populated by the app after backend detection.
  double left{};
  double top{};
};
Result<AppOptions> parse_options(int argc, char* const argv[]);
std::string usage();
Uuid128 numeric_id(std::uint64_t value);
Result<ScreenTopology> make_topology(const AppOptions& options);
std::optional<discovery::DiscoveryPeer> select_discovery_peer(
    const std::vector<discovery::DiscoveryPeer>& peers,
    std::optional<std::uint64_t> requested_peer);
}
