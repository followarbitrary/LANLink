#include <gtest/gtest.h>
#include <cmath>
#include <vector>
#include "lanlink/topology/topology.hpp"

namespace {
using lanlink::Display;
using lanlink::DisplayId;
using lanlink::DeviceId;
using lanlink::Edge;
using lanlink::Point;
using lanlink::Rect;
using lanlink::Rotation;
using lanlink::ScreenLink;
using lanlink::ScreenTopology;
using lanlink::Uuid128;

Display display(std::uint64_t id, double left, double top, double right, double bottom,
                double scale = 1.0) {
  return Display{DeviceId{id, 1}, DisplayId{id, 2}, Rect{left, top, right, bottom}, scale,
                 Rotation::k0};
}
ScreenLink link(const Display& source, Edge source_edge, const Display& target,
                Edge target_edge, double min = 0.0, double max = 1.0,
                std::uint64_t version = 1) {
  return ScreenLink{source.id, source_edge, target.id, target_edge, min, max, version};
}
}

TEST(ScreenTopology, RejectsInvalidRectanglesDuplicateIdsAndMalformedLinks) {
  ScreenTopology topology;
  auto bad_rect = display(1, 0, 0, 0, 100);
  EXPECT_FALSE(topology.set_snapshot(1, {bad_rect}, {}).ok());

  auto a = display(1, 0, 0, 100, 100);
  auto duplicate = display(1, 100, 0, 200, 100);
  EXPECT_FALSE(topology.set_snapshot(1, {a, duplicate}, {}).ok());

  auto b = display(2, 100, 0, 200, 100);
  auto bad_link = ScreenLink{a.id, Edge::kRight, b.id, Edge::kLeft, 0.8, 0.2, 1};
  EXPECT_FALSE(topology.set_snapshot(1, {a, b}, {bad_link}).ok());

  auto wrong_version = link(a, Edge::kRight, b, Edge::kLeft, 0, 1, 2);
  EXPECT_FALSE(topology.set_snapshot(1, {a, b}, {wrong_version}).ok());
}

TEST(ScreenTopology, RequiresAtLeastFourLogicalPixelsOfOverflow) {
  ScreenTopology topology;
  auto a = display(1, 0, 0, 100, 100);
  auto b = display(2, 100, 0, 200, 100);
  ASSERT_TRUE(topology.set_snapshot(1, {a, b}, {link(a, Edge::kRight, b, Edge::kLeft)}));
  EXPECT_FALSE(topology.crossing(a.id, Edge::kRight, Point{104, 50}, 3.9).has_value());
  ASSERT_TRUE(topology.crossing(a.id, Edge::kRight, Point{104, 50}, 4.0).has_value());
}

TEST(ScreenTopology, MapsRightAndLeftTangentsWithInset) {
  ScreenTopology topology;
  auto a = display(1, 0, 0, 100, 100);
  auto b = display(2, 100, 0, 300, 200);
  ASSERT_TRUE(topology.set_snapshot(1, {a, b}, {
    link(a, Edge::kRight, b, Edge::kLeft), link(b, Edge::kLeft, a, Edge::kRight)}));

  auto right = topology.crossing(a.id, Edge::kRight, Point{104, 25}, 4.0);
  ASSERT_TRUE(right.has_value());
  EXPECT_EQ(right->target, b.id);
  EXPECT_NEAR(right->tangent, 50.0, 1e-9);
  EXPECT_NEAR(right->entry_point.x, 108.0, 1e-9);
  EXPECT_NEAR(right->entry_point.y, 50.0, 1e-9);

  auto left = topology.crossing(b.id, Edge::kLeft, Point{96, 150}, 4.0);
  ASSERT_TRUE(left.has_value());
  EXPECT_EQ(left->target, a.id);
  EXPECT_NEAR(left->entry_point.x, 92.0, 1e-9);
  EXPECT_NEAR(left->entry_point.y, 75.0, 1e-9);
}

TEST(ScreenTopology, MapsUpAndDownAndClipsPartialOverlap) {
  ScreenTopology topology;
  auto a = display(1, 0, 0, 200, 100);
  auto b = display(2, 50, 100, 150, 300);
  ASSERT_TRUE(topology.set_snapshot(1, {a, b}, {
    link(a, Edge::kDown, b, Edge::kUp, 0.25, 0.75)}));
  auto down = topology.crossing(a.id, Edge::kDown, Point{100, 104}, 4.0);
  ASSERT_TRUE(down.has_value());
  EXPECT_NEAR(down->entry_point.x, 100.0, 1e-9);
  EXPECT_NEAR(down->entry_point.y, 108.0, 1e-9);

  auto outside = topology.crossing(a.id, Edge::kDown, Point{10, 104}, 4.0);
  EXPECT_FALSE(outside.has_value());
}

TEST(ScreenTopology, UsesLogicalCoordinatesAndHandlesGapsAndMultipleDisplays) {
  ScreenTopology topology;
  auto source = display(1, 0, 0, 1920, 1080, 1.0);
  auto hidpi = display(2, 1920, 100, 3200, 1700, 2.0);
  auto other = display(3, 0, 1200, 1920, 2280, 1.0);
  ASSERT_TRUE(topology.set_snapshot(7, {source, hidpi, other}, {
    link(source, Edge::kRight, hidpi, Edge::kLeft, 100.0 / 1080.0, 1, 7),
    link(source, Edge::kDown, other, Edge::kUp, 0, 1, 7)}));

  auto dpi = topology.crossing(source.id, Edge::kRight, Point{1924, 540}, 4.0);
  ASSERT_TRUE(dpi.has_value());
  EXPECT_NEAR(dpi->entry_point.x, 1928.0, 1e-9);
  EXPECT_NEAR(dpi->entry_point.y, 900.0, 1e-9);

  EXPECT_FALSE(topology.crossing(source.id, Edge::kRight, Point{1924, 50}, 4.0).has_value());
  auto down = topology.crossing(source.id, Edge::kDown, Point{960, 1084}, 4.0);
  ASSERT_TRUE(down.has_value());
  EXPECT_EQ(down->target, other.id);
}

TEST(ScreenTopology, ClipsEightPixelInsetForSmallTargets) {
  ScreenTopology topology;
  auto a = display(1, 0, 0, 100, 100);
  auto tiny = display(2, 100, 0, 105, 5);
  ASSERT_TRUE(topology.set_snapshot(1, {a, tiny}, {link(a, Edge::kRight, tiny, Edge::kLeft)}));
  auto crossing = topology.crossing(a.id, Edge::kRight, Point{104, 50}, 4.0);
  ASSERT_TRUE(crossing.has_value());
  EXPECT_NEAR(crossing->entry_point.x, 102.5, 1e-9);
  EXPECT_NEAR(crossing->entry_point.y, 2.5, 1e-9);
}
