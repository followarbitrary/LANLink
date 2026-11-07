#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include "lanlink/common/ids.hpp"
#include "lanlink/common/result.hpp"
#include "lanlink/topology/topology.hpp"
#include "lanlink/transport/connection.hpp"
namespace lanlink::app {
enum class RemoteSide { kLeft, kRight };
struct AppOptions {
  std::uint64_t device_id{};
  std::uint64_t peer_id{};
  std::uint64_t display_id{1};
  std::uint64_t peer_display_id{2};
  std::uint16_t listen_port{};
  std::optional<transport::Endpoint> connect;
  RemoteSide remote_side{RemoteSide::kRight};
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
}
