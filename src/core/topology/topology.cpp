#include "lanlink/topology/topology.hpp"

#include <cmath>
#include <algorithm>
#include <limits>
#include <string>

#include "lanlink/common/config.hpp"

namespace lanlink {
namespace {
bool finite(double value) { return std::isfinite(value); }

bool valid_edge(Edge edge) {
  switch (edge) {
    case Edge::kLeft:
    case Edge::kRight:
    case Edge::kUp:
    case Edge::kDown:
      return true;
  }
  return false;
}

double edge_start(const Rect& bounds, Edge edge) {
  return (edge == Edge::kLeft || edge == Edge::kRight) ? bounds.top : bounds.left;
}

double edge_extent(const Rect& bounds, Edge edge) {
  return (edge == Edge::kLeft || edge == Edge::kRight) ? bounds.height() : bounds.width();
}

double point_tangent(const Point& point, Edge edge) {
  return (edge == Edge::kLeft || edge == Edge::kRight) ? point.y : point.x;
}

}  // namespace

Result<void> ScreenTopology::set_snapshot(std::uint64_t version, std::vector<Display> displays,
                                          std::vector<ScreenLink> links) {
  std::unordered_map<DisplayId, std::size_t> indexes;
  for (std::size_t i = 0; i < displays.size(); ++i) {
    const auto& display = displays[i];
    const auto& bounds = display.logical_bounds;
    if (!finite(bounds.left) || !finite(bounds.top) || !finite(bounds.right) ||
        !finite(bounds.bottom) || !(bounds.left < bounds.right) || !(bounds.top < bounds.bottom)) {
      return Result<void>::failure("display rectangle must have finite positive dimensions");
    }
    if (!finite(display.scale) || display.scale <= 0.0) {
      return Result<void>::failure("display scale must be finite and positive");
    }
    if (!indexes.emplace(display.id, i).second) {
      return Result<void>::failure("duplicate display id");
    }
  }

  for (const auto& link : links) {
    if (!valid_edge(link.source_edge) || !valid_edge(link.target_edge)) {
      return Result<void>::failure("invalid link edge");
    }
    if (link.source == link.target) return Result<void>::failure("self-link is invalid");
    if (!indexes.contains(link.source) || !indexes.contains(link.target)) {
      return Result<void>::failure("link references unknown display");
    }
    if (link.version != version) return Result<void>::failure("link version mismatch");
    if (!finite(link.tangent_min) || !finite(link.tangent_max) || link.tangent_min < 0.0 ||
        link.tangent_max > 1.0 || !(link.tangent_min < link.tangent_max)) {
      return Result<void>::failure("link tangent range must be within [0,1]");
    }
  }

  version_ = version;
  displays_ = std::move(displays);
  links_ = std::move(links);
  display_index_ = std::move(indexes);
  return Result<void>::success();
}

Point ScreenTopology::clamp_entry(const Display& target_display, Edge target_edge,
                                  double tangent) const {
  const Rect& bounds = target_display.logical_bounds;
  const double extent = edge_extent(bounds, target_edge);
  const double inset = std::min(m0::kEntryInset, extent / 2.0);
  const double min_tangent = edge_start(bounds, target_edge) + inset;
  const double max_tangent = edge_start(bounds, target_edge) + extent - inset;
  const double clamped = std::clamp(tangent, min_tangent, max_tangent);

  switch (target_edge) {
    case Edge::kLeft:
      return Point{bounds.left + inset, clamped};
    case Edge::kRight:
      return Point{bounds.right - inset, clamped};
    case Edge::kUp:
      return Point{clamped, bounds.top + inset};
    case Edge::kDown:
      return Point{clamped, bounds.bottom - inset};
  }
  return Point{};
}

std::optional<Crossing> ScreenTopology::crossing(const DisplayId& source_display, Edge source_edge,
                                                 Point candidate_point,
                                                 double accumulated_overflow) const {
  if (!finite(candidate_point.x) || !finite(candidate_point.y) ||
      !finite(accumulated_overflow) || accumulated_overflow < m0::kEdgeThreshold) {
    return std::nullopt;
  }
  const auto source_it = display_index_.find(source_display);
  if (source_it == display_index_.end()) return std::nullopt;
  const auto& source = displays_[source_it->second];
  const double extent = edge_extent(source.logical_bounds, source_edge);
  if (!(extent > 0.0)) return std::nullopt;
  const double tangent = point_tangent(candidate_point, source_edge);
  const double ratio = (tangent - edge_start(source.logical_bounds, source_edge)) / extent;
  if (ratio < 0.0 || ratio >= 1.0) return std::nullopt;

  for (const auto& link : links_) {
    if (link.source != source_display || link.source_edge != source_edge) continue;
    if (ratio < link.tangent_min || ratio > link.tangent_max) continue;
    const auto target_it = display_index_.find(link.target);
    if (target_it == display_index_.end()) continue;
    const auto& target = displays_[target_it->second];
    const double target_tangent = edge_start(target.logical_bounds, link.target_edge) +
                                  ratio * edge_extent(target.logical_bounds, link.target_edge);
    return Crossing{target.id, link.target_edge, target_tangent,
                    clamp_entry(target, link.target_edge, target_tangent)};
  }
  return std::nullopt;
}

}  // namespace lanlink
