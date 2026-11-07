#include "lanlink/platform/fake_input_backend.hpp"
#include "lanlink/runtime/node.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace {
using namespace std::chrono_literals;
using namespace lanlink;
using namespace lanlink::platform;
using namespace lanlink::runtime;
using namespace lanlink::transport;

Uuid128 id(std::uint64_t value) { return {value, value + 1000}; }

ScreenTopology topology_for(DisplayId local, DisplayId remote, bool local_left) {
  ScreenTopology topology;
  const Display first{local_left ? id(1) : id(3), local, {0, 0, 100, 100}, 1.0,
                       Rotation::k0};
  const Display second{local_left ? id(3) : id(1), remote, {100, 0, 200, 100}, 1.0,
                        Rotation::k0};
  const ScreenLink link{local, Edge::kRight, remote, Edge::kLeft, 0.0, 1.0, 1};
  EXPECT_TRUE(topology.set_snapshot(1, {first, second}, {link}).ok());
  return topology;
}

InputRuntimeConfig config(DeviceId local_device, SourceId source, AuthenticatedPeer peer,
                          DisplayId local_display, Endpoint listen,
                          IInputBackend* backend, std::size_t queue_capacity = 256) {
  InputRuntimeConfig value;
  value.local_device = local_device;
  value.source = source;
  value.peer = peer;
  value.local_display = local_display;
  value.listen_endpoint = std::move(listen);
  value.backend = backend;
  value.queue_capacity = queue_capacity;
  return value;
}

void pump_pair(InputNodeRuntime& left, InputNodeRuntime& right,
               IClock::time_point& now, int iterations = 12) {
  for (int i = 0; i < iterations; ++i) {
    ASSERT_TRUE(left.poll(now, 5ms).ok());
    ASSERT_TRUE(right.poll(now, 5ms).ok());
    ASSERT_TRUE(left.tick(now).ok());
    ASSERT_TRUE(right.tick(now).ok());
    now += 1ms;
  }
}

TEST(InputNodeRuntime, CrossesAtExactThresholdAndForwardsSuppressedInput) {
  FakeInputBackend left_backend;
  FakeInputBackend right_backend;
  auto left_result = InputNodeRuntime::create(config(
      id(1), id(2), AuthenticatedPeer{id(3)}, id(10), Endpoint{"127.0.0.1", 0}, &left_backend));
  auto right_result = InputNodeRuntime::create(config(
      id(3), id(4), AuthenticatedPeer{id(1)}, id(20), Endpoint{"127.0.0.1", 0},
      &right_backend));
  ASSERT_TRUE(left_result.ok()) << left_result.error();
  ASSERT_TRUE(right_result.ok()) << right_result.error();
  auto left = std::move(left_result.value());
  auto right = std::move(right_result.value());
  ASSERT_TRUE(left->set_topology(topology_for(id(10), id(20), true)).ok());
  ASSERT_TRUE(right->set_topology(topology_for(id(20), id(10), false)).ok());
  ASSERT_TRUE(left->start().ok());
  ASSERT_TRUE(right->start().ok());
  ASSERT_TRUE(left->connect_peer(right->local_endpoint()).ok());

  IClock::time_point now{};
  pump_pair(*left, *right, now, 20);
  ASSERT_TRUE(left->connected());
  ASSERT_TRUE(right->connected());

  left_backend.set_pointer_position(Point{100.0, 50.0});
  left_backend.emit_physical(
      InputEvent{{}, {}, 0, 0, MouseDelta{3.0, 0.0}, InputOrigin::kPhysical});
  ASSERT_TRUE(left->poll(now, 0ms).ok());
  EXPECT_EQ(left->router().state(), RouterState::kLocal);

  left_backend.set_pointer_position(Point{100.0, 50.0});
  left_backend.emit_physical(
      InputEvent{{}, {}, 0, 0, MouseDelta{4.0, 0.0}, InputOrigin::kPhysical});
  ASSERT_TRUE(left->poll(now, 0ms).ok());
  EXPECT_EQ(left->router().state(), RouterState::kAcquiring);

  pump_pair(*left, *right, now, 12);
  ASSERT_EQ(left->router().state(), RouterState::kRemote);
  ASSERT_TRUE(right->arbiter().has_active_lease());
  ASSERT_EQ(right_backend.injected_events().size(), 1u);
  ASSERT_TRUE(std::holds_alternative<PointerWarp>(right_backend.injected_events().front().payload));
  ASSERT_TRUE(left_backend.pointer_position().has_value());
  EXPECT_EQ(*left_backend.pointer_position(), (Point{100.0, 50.0}));
  ASSERT_TRUE(right_backend.pointer_position().has_value());
  EXPECT_EQ(*right_backend.pointer_position(), (Point{8.0, 50.0}));

  const auto entry = std::get<PointerWarp>(right_backend.injected_events().front().payload);
  EXPECT_DOUBLE_EQ(entry.x, 8.0);
  EXPECT_DOUBLE_EQ(entry.y, 50.0);

  // Once the handoff is active, source motion is consumed by the hook while
  // the target alone advances from its local entry point.
  left_backend.set_pointer_position(Point{80.0, 50.0});
  left_backend.emit_physical(
      InputEvent{{}, {}, 0, 0, MouseDelta{20.0, 0.0}, InputOrigin::kPhysical});
  ASSERT_TRUE(left->poll(now, 0ms).ok());
  pump_pair(*left, *right, now, 4);
  EXPECT_EQ(*left_backend.pointer_position(), (Point{80.0, 50.0}));
  EXPECT_EQ(*right_backend.pointer_position(), (Point{28.0, 50.0}));

  left_backend.emit_physical(
      InputEvent{{}, {}, 0, 0, Key{0x04, true}, InputOrigin::kPhysical});
  left_backend.emit_physical(
      InputEvent{{}, {}, 0, 0, MouseButton{1, true}, InputOrigin::kPhysical});
  ASSERT_TRUE(left->poll(now, 0ms).ok());
  pump_pair(*left, *right, now, 4);
  ASSERT_EQ(right_backend.injected_events().size(), 4u);
  EXPECT_EQ(std::get<Key>(right_backend.injected_events()[2].payload).code, 0x04u);
  EXPECT_EQ(std::get<MouseButton>(right_backend.injected_events()[3].payload).button, 1u);

  left_backend.emit_physical(
      InputEvent{{}, {}, 0, 0, MouseButton{1, false}, InputOrigin::kPhysical});
  left_backend.emit_physical(
      InputEvent{{}, {}, 0, 0, Key{0x04, false}, InputOrigin::kPhysical});
  ASSERT_TRUE(left->poll(now, 0ms).ok());
  for (int i = 0; i < 4; ++i) ASSERT_TRUE(right->poll(now, 10ms).ok());
  EXPECT_EQ(right_backend.held_count(), 0u);

  // Local input wins over the remote lease on the target.
  ASSERT_TRUE(right->arbiter().has_active_lease());
  right_backend.emit_physical(
      InputEvent{{}, {}, 0, 0, Key{0x04, true}, InputOrigin::kPhysical});
  ASSERT_TRUE(right->poll(now, 0ms).ok());
  ASSERT_TRUE(left->poll(now, 10ms).ok());
  EXPECT_FALSE(right->arbiter().has_lease());
  EXPECT_EQ(left->router().state(), RouterState::kRecovering);
  EXPECT_FALSE(left_backend.status().suppression);

  ASSERT_TRUE(left->tick(now).ok());
  ASSERT_TRUE(right->tick(now).ok());
  EXPECT_EQ(left->router().state(), RouterState::kLocal);

  // The former target can immediately acquire control in the reverse direction.
  right_backend.set_pointer_position(Point{100.0, 50.0});
  right_backend.emit_physical(
      InputEvent{{}, {}, 0, 0, MouseDelta{4.0, 0.0}, InputOrigin::kPhysical});
  ASSERT_TRUE(right->poll(now, 0ms).ok());
  pump_pair(*left, *right, now, 12);
  EXPECT_EQ(right->router().state(), RouterState::kRemote);
  EXPECT_TRUE(left->arbiter().has_active_lease());

  ASSERT_TRUE(right->stop(now).ok());
  (void)left->poll(now, 0ms);
  EXPECT_EQ(left->router().state(), RouterState::kRecovering);
  EXPECT_FALSE(left_backend.status().suppression);
  ASSERT_TRUE(left->tick(now).ok());
  EXPECT_EQ(left->router().state(), RouterState::kLocal);
}

TEST(InputNodeRuntime, AccumulatesOutwardDeltaWhenPointerIsClampedAtEdge) {
  FakeInputBackend left_backend;
  FakeInputBackend right_backend;
  auto left_result = InputNodeRuntime::create(config(
      id(201), id(202), AuthenticatedPeer{id(203)}, id(210), Endpoint{"127.0.0.1", 0},
      &left_backend));
  auto right_result = InputNodeRuntime::create(config(
      id(203), id(204), AuthenticatedPeer{id(201)}, id(220), Endpoint{"127.0.0.1", 0},
      &right_backend));
  ASSERT_TRUE(left_result.ok()) << left_result.error();
  ASSERT_TRUE(right_result.ok()) << right_result.error();
  auto left = std::move(left_result.value());
  auto right = std::move(right_result.value());
  ASSERT_TRUE(left->set_topology(topology_for(id(210), id(220), true)).ok());
  ASSERT_TRUE(right->set_topology(topology_for(id(220), id(210), false)).ok());
  ASSERT_TRUE(left->start().ok());
  ASSERT_TRUE(right->start().ok());
  ASSERT_TRUE(left->connect_peer(right->local_endpoint()).ok());

  IClock::time_point now{};
  pump_pair(*left, *right, now, 20);
  ASSERT_TRUE(left->connected());
  ASSERT_TRUE(right->connected());

  // Fake the native desktop clamping every event at x=100. The physical
  // deltas remain non-zero, as Quartz reports at a screen edge.
  for (int index = 0; index < 3; ++index) {
    left_backend.set_pointer_position(Point{99.0, 50.0});
    left_backend.emit_physical(
        InputEvent{{}, {}, 0, 0, MouseDelta{1.0, 0.0}, InputOrigin::kPhysical});
    ASSERT_TRUE(left->poll(now, 0ms).ok());
    EXPECT_EQ(left->router().state(), RouterState::kLocal);
  }

  // Moving away from the edge abandons the partial crossing budget.
  left_backend.set_pointer_position(Point{100.0, 50.0});
  left_backend.emit_physical(
      InputEvent{{}, {}, 0, 0, MouseDelta{-1.0, 0.0}, InputOrigin::kPhysical});
  ASSERT_TRUE(left->poll(now, 0ms).ok());
  EXPECT_EQ(left->router().state(), RouterState::kLocal);

  for (int index = 0; index < 4; ++index) {
    left_backend.set_pointer_position(Point{99.0, 50.0});
    left_backend.emit_physical(
        InputEvent{{}, {}, 0, 0, MouseDelta{1.0, 0.0}, InputOrigin::kPhysical});
    ASSERT_TRUE(left->poll(now, 0ms).ok());
  }
  EXPECT_EQ(left->router().state(), RouterState::kAcquiring);

  pump_pair(*left, *right, now, 12);
  EXPECT_EQ(left->router().state(), RouterState::kRemote);
  ASSERT_EQ(right_backend.injected_events().size(), 1u);
  const auto& entry = std::get<PointerWarp>(right_backend.injected_events().front().payload);
  EXPECT_DOUBLE_EQ(entry.x, 8.0);
  EXPECT_DOUBLE_EQ(entry.y, 50.0);
}

TEST(InputNodeRuntime, RemoteSourceReturnsToLocalAtEdge) {
  FakeInputBackend left_backend;
  FakeInputBackend right_backend;
  auto left_result = InputNodeRuntime::create(config(
      id(301), id(302), AuthenticatedPeer{id(303)}, id(310), Endpoint{"127.0.0.1", 0},
      &left_backend));
  auto right_result = InputNodeRuntime::create(config(
      id(303), id(304), AuthenticatedPeer{id(301)}, id(320), Endpoint{"127.0.0.1", 0},
      &right_backend));
  ASSERT_TRUE(left_result.ok()) << left_result.error();
  ASSERT_TRUE(right_result.ok()) << right_result.error();
  auto left = std::move(left_result.value());
  auto right = std::move(right_result.value());
  ASSERT_TRUE(left->set_topology(topology_for(id(310), id(320), true)).ok());
  ASSERT_TRUE(right->set_topology(topology_for(id(320), id(310), true)).ok());
  ASSERT_TRUE(left->start().ok());
  ASSERT_TRUE(right->start().ok());
  ASSERT_TRUE(left->connect_peer(right->local_endpoint()).ok());

  IClock::time_point now{};
  pump_pair(*left, *right, now, 20);

  left_backend.set_pointer_position(Point{100.0, 50.0});
  left_backend.emit_physical(
      InputEvent{{}, {}, 0, 0, MouseDelta{4.0, 0.0}, InputOrigin::kPhysical});
  ASSERT_TRUE(left->poll(now, 0ms).ok());
  pump_pair(*left, *right, now, 12);
  ASSERT_EQ(left->router().state(), RouterState::kRemote);
  ASSERT_TRUE(right->arbiter().has_active_lease());

  // The source is already remote. Moving inward from its local edge releases
  // the target lease and restores this node's local cursor.
  left_backend.set_pointer_position(Point{100.0, 50.0});
  left_backend.emit_physical(
      InputEvent{{}, {}, 0, 0, MouseDelta{-4.0, 0.0}, InputOrigin::kPhysical});
  ASSERT_TRUE(left->poll(now, 0ms).ok());
  pump_pair(*left, *right, now, 4);
  EXPECT_EQ(left->router().state(), RouterState::kLocal);
  EXPECT_FALSE(left_backend.status().suppression);
  EXPECT_FALSE(right->arbiter().has_lease());
}

TEST(InputNodeRuntime, NoPeerOverflowStillReleasesInjectedStateAndRestoresLocal) {
  FakeInputBackend backend;
  auto result = InputNodeRuntime::create(config(
      id(101), id(102), AuthenticatedPeer{id(103)}, id(104), Endpoint{"127.0.0.1", 0},
      &backend, 1));
  ASSERT_TRUE(result.ok()) << result.error();
  auto runtime = std::move(result.value());
  ASSERT_TRUE(runtime->start().ok());

  backend.emit_physical(InputEvent{{}, {}, 0, 0, Key{0x04, true}, InputOrigin::kPhysical});
  backend.emit_physical(InputEvent{{}, {}, 0, 0, Key{0x04, false}, InputOrigin::kPhysical});
  auto drained = runtime->poll(IClock::time_point{}, 0ms);
  EXPECT_FALSE(drained.ok());
  EXPECT_EQ(backend.released_sessions().size(), 1u);
  EXPECT_EQ(backend.status().recovery_count, 1u);
  EXPECT_EQ(runtime->router().state(), RouterState::kRecovering);
}

TEST(InputNodeRuntime, StopClearsQueuedOverflowBeforeRestart) {
  FakeInputBackend backend;
  auto result = InputNodeRuntime::create(config(
      id(111), id(112), AuthenticatedPeer{id(113)}, id(114), Endpoint{"127.0.0.1", 0},
      &backend, 1));
  ASSERT_TRUE(result.ok()) << result.error();
  auto runtime = std::move(result.value());
  ASSERT_TRUE(runtime->start().ok());
  backend.emit_physical(InputEvent{{}, {}, 0, 0, Key{0x04, true}, InputOrigin::kPhysical});
  backend.emit_physical(InputEvent{{}, {}, 0, 0, Key{0x04, false}, InputOrigin::kPhysical});
  ASSERT_TRUE(runtime->stop(IClock::time_point{}).ok());
  ASSERT_TRUE(runtime->start().ok());
  EXPECT_TRUE(runtime->poll(IClock::time_point{}, 0ms).ok());
}

TEST(InputNodeRuntime, DestructorStopsRunningBackend) {
  FakeInputBackend backend;
  {
    auto result = InputNodeRuntime::create(config(
        id(121), id(122), AuthenticatedPeer{id(123)}, id(124), Endpoint{"127.0.0.1", 0},
        &backend));
    ASSERT_TRUE(result.ok()) << result.error();
    auto runtime = std::move(result.value());
    ASSERT_TRUE(runtime->start().ok());
    EXPECT_TRUE(backend.status().running);
  }
  EXPECT_FALSE(backend.status().running);
}

TEST(InputNodeRuntime, MissingPointerDoesNotTriggerCrossing) {
  FakeInputBackend backend;
  auto result = InputNodeRuntime::create(config(
      id(131), id(132), AuthenticatedPeer{id(133)}, id(134), Endpoint{"127.0.0.1", 0},
      &backend));
  ASSERT_TRUE(result.ok()) << result.error();
  auto runtime = std::move(result.value());
  ScreenTopology topology;
  EXPECT_TRUE(topology.set_snapshot(1,
      {Display{id(1), id(134), {-200, 0, -100, 100}, 1.0, Rotation::k0},
       Display{id(2), id(135), {-100, 0, 0, 100}, 1.0, Rotation::k0}},
      {ScreenLink{id(134), Edge::kRight, id(135), Edge::kLeft, 0.0, 1.0, 1}}).ok());
  ASSERT_TRUE(runtime->set_topology(std::move(topology)).ok());
  ASSERT_TRUE(runtime->start().ok());
  backend.emit_physical(InputEvent{{}, {}, 0, 0, Key{0x04, true}, InputOrigin::kPhysical});
  ASSERT_TRUE(runtime->poll(IClock::time_point{}, 0ms).ok());
  EXPECT_EQ(runtime->router().state(), RouterState::kLocal);
}

TEST(InputNodeRuntime, KeyEventAtStaleOutsidePointerDoesNotTriggerCrossing) {
  FakeInputBackend backend;
  auto result = InputNodeRuntime::create(config(
      id(141), id(142), AuthenticatedPeer{id(143)}, id(144), Endpoint{"127.0.0.1", 0},
      &backend));
  ASSERT_TRUE(result.ok()) << result.error();
  auto runtime = std::move(result.value());
  ScreenTopology topology;
  EXPECT_TRUE(topology.set_snapshot(1,
      {Display{id(1), id(144), {0, 0, 100, 100}, 1.0, Rotation::k0},
       Display{id(2), id(145), {100, 0, 200, 100}, 1.0, Rotation::k0}},
      {ScreenLink{id(144), Edge::kRight, id(145), Edge::kLeft, 0.0, 1.0, 1}}).ok());
  ASSERT_TRUE(runtime->set_topology(std::move(topology)).ok());
  ASSERT_TRUE(runtime->start().ok());
  backend.set_pointer_position(Point{101.0, 50.0});
  backend.emit_physical(InputEvent{{}, {}, 0, 0, Key{0x04, true}, InputOrigin::kPhysical});
  ASSERT_TRUE(runtime->poll(IClock::time_point{}, 0ms).ok());
  EXPECT_EQ(runtime->router().state(), RouterState::kLocal);
}

}  // namespace
