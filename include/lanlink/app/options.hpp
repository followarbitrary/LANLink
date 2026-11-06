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
struct AppOptions { std::uint64_t device_id{}; std::uint64_t peer_id{}; std::uint64_t display_id{1}; std::uint64_t peer_display_id{2}; std::uint16_t listen_port{}; std::optional<transport::Endpoint> connect; RemoteSide remote_side{RemoteSide::kRight}; double width{1920.0}; double height{1080.0}; };
Result<AppOptions> parse_options(int argc, char* const argv[]);
std::string usage();
Uuid128 numeric_id(std::uint64_t value);
Result<ScreenTopology> make_topology(const AppOptions& options);
}
