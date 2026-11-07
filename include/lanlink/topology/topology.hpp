#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "lanlink/common/ids.hpp"
#include "lanlink/common/result.hpp"

namespace lanlink {

enum class Edge {
  kLeft, kRight, kUp, kDown,
  Left = kLeft, Right = kRight, Up = kUp, Down = kDown,
};
enum class Rotation {
  k0 = 0,
  k90 = 90,
  k180 = 180,
  k270 = 270,
};

struct Point {
  double x{};
  double y{};
  friend bool operator==(const Point&, const Point&) = default;
};

struct Rect {
  double left{};
  double top{};
  double right{};
  double bottom{};
  double width() const noexcept { return right - left; }
  double height() const noexcept { return bottom - top; }
};

struct Display {
  DeviceId device;
  DisplayId id;
  Rect logical_bounds;
  double scale{1.0};
  Rotation rotation{Rotation::k0};
};

struct ScreenLink {
  DisplayId source;
  Edge source_edge;
  DisplayId target;
  Edge target_edge;
  // Normalized source-edge tangent ratios, each in the inclusive range [0,1].
  double tangent_min{};
  double tangent_max{1.0};
  std::uint64_t version{};
};

struct Crossing {
  DisplayId target;
  Edge target_edge;
  double tangent{};
  Point entry_point{};
  // The source-edge tangent expressed as a normalized ratio.  The target
  // computes its concrete local entry point from its own display bounds.
  double tangent_ratio{};
  bool has_tangent_ratio{false};
  // Descriptive aliases retained for callers that use the API vocabulary.
  DisplayId target_display;
  Point entry;

  Crossing(DisplayId target_id, Edge edge, double mapped_tangent, Point point,
           double ratio = 0.0, bool ratio_is_normalized = false)
      : target(target_id), target_edge(edge), tangent(mapped_tangent), entry_point(point),
        tangent_ratio(ratio), has_tangent_ratio(ratio_is_normalized),
        target_display(target_id), entry(point) {}
};

// Returns a safe local entry point on a display edge for a normalized tangent
// ratio.  The result uses the same logical coordinate space as the display
// bounds supplied by the platform backend.
Point edge_entry_point(const Rect& bounds, Edge edge, double tangent_ratio);

class ScreenTopology {
 public:
  Result<void> set_snapshot(std::uint64_t version, std::vector<Display> displays,
                            std::vector<ScreenLink> links);

  std::optional<Crossing> crossing(const DisplayId& source_display, Edge source_edge,
                                   Point candidate_point,
                                   double accumulated_overflow) const;

  Point clamp_entry(const Display& target_display, Edge target_edge, double tangent) const;

  std::uint64_t version() const noexcept { return version_; }
  const std::vector<Display>& displays() const noexcept { return displays_; }
  const std::vector<ScreenLink>& links() const noexcept { return links_; }

 private:
  std::uint64_t version_{};
  std::vector<Display> displays_;
  std::vector<ScreenLink> links_;
  std::unordered_map<DisplayId, std::size_t> display_index_;
};

}  // namespace lanlink
