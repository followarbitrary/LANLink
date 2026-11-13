#include "lanlink/common/config.hpp"
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
  left_backend.set_display_scale(2.0);
  right_backend.set_display_scale(1.0);
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
  EXPECT_EQ(*right_backend.pointer_position(), (Point{48.0, 50.0}));
  ASSERT_EQ(right_backend.injected_events().size(), 2u);
  // A target that does not advertise absolute pointer injection receives the
  // original relative delta. Quartz coalesces relative motion more smoothly
  // than a stream of absolute warps when the source is a high-rate mouse.
  ASSERT_TRUE(std::holds_alternative<MouseDelta>(right_backend.injected_events()[1].payload));
  const auto& remote_move = std::get<MouseDelta>(right_backend.injected_events()[1].payload);
  EXPECT_DOUBLE_EQ(remote_move.dx, 40.0);
  EXPECT_DOUBLE_EQ(remote_move.dy, 0.0);

  // A local keyboard event on the target is independent from the remote
  // pointer lease and must not interrupt the source mouse.
  right_backend.emit_physical(
      InputEvent{{}, {}, 0, 0, Key{0x04, true}, InputOrigin::kPhysical});
  ASSERT_TRUE(right->poll(now, 0ms).ok());
  EXPECT_TRUE(right->arbiter().has_active_lease());
  EXPECT_EQ(left->router().state(), RouterState::kRemote);
  EXPECT_EQ(right_backend.injected_events().size(), 2u);

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

TEST(InputNodeRuntime, FallsBackToTopologyAfterPeerDisplayInfoTimeout) {
  FakeInputBackend left_backend;
  FakeInputBackend right_backend;
  left_backend.set_display_bounds(Rect{0.0, 0.0, 100.0, 100.0});
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
      InputEvent{{}, {}, 0, 0, MouseDelta{4.0, 0.0}, InputOrigin::kPhysical});
  ASSERT_TRUE(left->poll(now, 0ms).ok());
  EXPECT_EQ(left->router().state(), RouterState::kLocal);

  now += 1s;
  pump_pair(*left, *right, now, 4);
  now += 1s;
  pump_pair(*left, *right, now, 4);
  now += 1s;
  left_backend.set_pointer_position(Point{100.0, 50.0});
  left_backend.emit_physical(
      InputEvent{{}, {}, 0, 0, MouseDelta{4.0, 0.0}, InputOrigin::kPhysical});
  ASSERT_TRUE(left->poll(now, 0ms).ok());
  EXPECT_EQ(left->router().state(), RouterState::kAcquiring);

  pump_pair(*left, *right, now, 12);
  EXPECT_EQ(left->router().state(), RouterState::kRemote);
  ASSERT_EQ(right_backend.injected_events().size(), 1u);
  EXPECT_TRUE(std::holds_alternative<PointerWarp>(right_backend.injected_events().front().payload));
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
  right_backend.set_display_bounds(Rect{0.0, 0.0, 200.0, 100.0});
  right_backend.set_absolute_pointer_moves(true);
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

  // The source is already remote. The virtual target cursor must travel all
  // the way back to its entry edge before the target lease is released.
  left_backend.set_pointer_position(Point{100.0, 50.0});
  for (int index = 0; index < 2; ++index) {
    left_backend.emit_physical(
        InputEvent{{}, {}, 0, 0, MouseDelta{-4.0, 0.0}, InputOrigin::kPhysical});
    ASSERT_TRUE(left->poll(now, 0ms).ok());
    pump_pair(*left, *right, now, 2);
    EXPECT_EQ(left->router().state(), RouterState::kRemote);
  }

  // Simulate target-side drift: the backend reports a position different from
  // the source's accumulated estimate. The periodic position report must
  // correct the virtual cursor before return-edge detection continues.
  right_backend.set_pointer_position(Point{10.0, 50.0});
  now += m0::kPointerPositionInterval;
  pump_pair(*left, *right, now, 2);
  left_backend.emit_physical(
      InputEvent{{}, {}, 0, 0, MouseDelta{-1.0, 0.0}, InputOrigin::kPhysical});
  ASSERT_TRUE(left->poll(now, 0ms).ok());
  pump_pair(*left, *right, now, 2);
  EXPECT_EQ(left->router().state(), RouterState::kRemote);
  ASSERT_GE(right_backend.injected_events().size(), 4u);
  const auto& corrected_move = right_backend.injected_events().back();
  ASSERT_TRUE(std::holds_alternative<PointerMove>(corrected_move.payload));
  EXPECT_DOUBLE_EQ(std::get<PointerMove>(corrected_move.payload).x, 9.0);

  left_backend.emit_physical(
      InputEvent{{}, {}, 0, 0, MouseDelta{-10.0, 0.0}, InputOrigin::kPhysical});
  ASSERT_TRUE(left->poll(now, 0ms).ok());
  pump_pair(*left, *right, now, 4);
  EXPECT_EQ(left->router().state(), RouterState::kLocal);
  EXPECT_FALSE(left_backend.status().suppression);
  EXPECT_FALSE(right->arbiter().has_lease());
}

TEST(InputNodeRuntime, ReturnWarpFailureRecoversBeforeProcessingQueuedEvents) {
  FakeInputBackend left_backend;
  FakeInputBackend right_backend;
  auto left_result = InputNodeRuntime::create(config(
      id(351), id(352), AuthenticatedPeer{id(353)}, id(360), Endpoint{"127.0.0.1", 0},
      &left_backend));
  auto right_result = InputNodeRuntime::create(config(
      id(353), id(354), AuthenticatedPeer{id(351)}, id(370), Endpoint{"127.0.0.1", 0},
      &right_backend));
  ASSERT_TRUE(left_result.ok()) << left_result.error();
  ASSERT_TRUE(right_result.ok()) << right_result.error();
  auto left = std::move(left_result.value());
  auto right = std::move(right_result.value());
  right_backend.set_display_bounds(Rect{0.0, 0.0, 200.0, 100.0});
  ASSERT_TRUE(left->set_topology(topology_for(id(360), id(370), true)).ok());
  ASSERT_TRUE(right->set_topology(topology_for(id(370), id(360), true)).ok());
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

  // Move the target cursor to its entry edge. The next outward delta will
  // request a source warp back to the local display.
  left_backend.set_pointer_position(Point{100.0, 50.0});
  for (int index = 0; index < 2; ++index) {
    left_backend.emit_physical(
        InputEvent{{}, {}, 0, 0, MouseDelta{-4.0, 0.0}, InputOrigin::kPhysical});
    ASSERT_TRUE(left->poll(now, 0ms).ok());
    pump_pair(*left, *right, now, 2);
  }

  // Fail only the return warp, then queue another event in the same drained
  // batch. The second event should still be handled after local recovery and
  // start a fresh acquire rather than being silently discarded.
  left_backend.fail_next_warp();
  left_backend.emit_physical(
      InputEvent{{}, {}, 0, 0, MouseDelta{-1.0, 0.0}, InputOrigin::kPhysical});
  left_backend.emit_physical(
      InputEvent{{}, {}, 0, 0, MouseDelta{4.0, 0.0}, InputOrigin::kPhysical});
  const auto drained = left->poll(now, 0ms);
  ASSERT_TRUE(drained.ok()) << drained.error();
  EXPECT_EQ(left->router().state(), RouterState::kAcquiring);
  EXPECT_TRUE(left_backend.status().suppression);
  ASSERT_FALSE(left_backend.suppression_history().empty());
  EXPECT_TRUE(left_backend.suppression_history().back());
}

TEST(InputNodeRuntime, CorrectsRemoteCursorFromTargetPositionReport) {
  FakeInputBackend left_backend;
  FakeInputBackend right_backend;
  auto left_result = InputNodeRuntime::create(config(
      id(501), id(502), AuthenticatedPeer{id(503)}, id(510), Endpoint{"127.0.0.1", 0},
      &left_backend));
  auto right_result = InputNodeRuntime::create(config(
      id(503), id(504), AuthenticatedPeer{id(501)}, id(520), Endpoint{"127.0.0.1", 0},
      &right_backend));
  ASSERT_TRUE(left_result.ok()) << left_result.error();
  ASSERT_TRUE(right_result.ok()) << right_result.error();
  auto left = std::move(left_result.value());
  auto right = std::move(right_result.value());
  right_backend.set_display_bounds(Rect{0.0, 0.0, 200.0, 100.0});
  ASSERT_TRUE(left->set_topology(topology_for(id(510), id(520), true)).ok());
  ASSERT_TRUE(right->set_topology(topology_for(id(520), id(510), true)).ok());
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

  // Simulate the target cursor drifting to the exact return edge. A periodic
  // target report must correct the source's virtual cursor before this small
  // outward delta is evaluated.
  right_backend.set_pointer_position(Point{0.0, 50.0});
  now += m0::kPointerPositionInterval;
  pump_pair(*left, *right, now, 4);

  left_backend.set_pointer_position(Point{100.0, 50.0});
  left_backend.emit_physical(
      InputEvent{{}, {}, 0, 0, MouseDelta{-1.0, 0.0}, InputOrigin::kPhysical});
  ASSERT_TRUE(left->poll(now, 0ms).ok());
  EXPECT_EQ(left->router().state(), RouterState::kRecovering);
  pump_pair(*left, *right, now, 4);
  EXPECT_EQ(left->router().state(), RouterState::kLocal);
  EXPECT_FALSE(right->arbiter().has_lease());
}

TEST(InputNodeRuntime, RemoteReturnMapsTargetTangentToSourceBounds) {
  FakeInputBackend left_backend;
  FakeInputBackend right_backend;
  auto left_result = InputNodeRuntime::create(config(
      id(401), id(402), AuthenticatedPeer{id(403)}, id(410), Endpoint{"127.0.0.1", 0},
      &left_backend));
  auto right_result = InputNodeRuntime::create(config(
      id(403), id(404), AuthenticatedPeer{id(401)}, id(420), Endpoint{"127.0.0.1", 0},
      &right_backend));
  ASSERT_TRUE(left_result.ok()) << left_result.error();
  ASSERT_TRUE(right_result.ok()) << right_result.error();
  auto left = std::move(left_result.value());
  auto right = std::move(right_result.value());
  right_backend.set_display_bounds(Rect{0.0, 0.0, 200.0, 100.0});

  ScreenTopology left_topology;
  ASSERT_TRUE(left_topology
                  .set_snapshot(
                      1,
                      {Display{id(1), id(410), {0.0, 0.0, 100.0, 200.0}, 1.0, Rotation::k0},
                       Display{id(3), id(420), {100.0, 0.0, 300.0, 100.0}, 1.0,
                               Rotation::k0}},
                      {ScreenLink{id(410), Edge::kRight, id(420), Edge::kLeft, 0.0, 1.0, 1}})
                  .ok());
  ScreenTopology right_topology;
  ASSERT_TRUE(right_topology
                  .set_snapshot(
                      1,
                      {Display{id(3), id(420), {0.0, 0.0, 200.0, 100.0}, 1.0, Rotation::k0},
                       Display{id(1), id(410), {200.0, 0.0, 300.0, 200.0}, 1.0,
                               Rotation::k0}},
                      {ScreenLink{id(420), Edge::kRight, id(410), Edge::kLeft, 0.0, 1.0, 1}})
                  .ok());
  ASSERT_TRUE(left->set_topology(std::move(left_topology)).ok());
  ASSERT_TRUE(right->set_topology(std::move(right_topology)).ok());
  ASSERT_TRUE(left->start().ok());
  ASSERT_TRUE(right->start().ok());
  ASSERT_TRUE(left->connect_peer(right->local_endpoint()).ok());

  IClock::time_point now{};
  pump_pair(*left, *right, now, 20);

  left_backend.set_pointer_position(Point{100.0, 150.0});
  left_backend.emit_physical(
      InputEvent{{}, {}, 0, 0, MouseDelta{4.0, 0.0}, InputOrigin::kPhysical});
  ASSERT_TRUE(left->poll(now, 0ms).ok());
  pump_pair(*left, *right, now, 12);
  ASSERT_EQ(left->router().state(), RouterState::kRemote);
  ASSERT_TRUE(right->arbiter().has_active_lease());
  ASSERT_EQ(right_backend.injected_events().size(), 1u);
  const auto& entry = std::get<PointerWarp>(right_backend.injected_events().front().payload);
  EXPECT_DOUBLE_EQ(entry.x, 8.0);
  EXPECT_DOUBLE_EQ(entry.y, 75.0);

  left_backend.set_pointer_position(Point{100.0, 0.0});
  for (int index = 0; index < 2; ++index) {
    left_backend.emit_physical(
        InputEvent{{}, {}, 0, 0, MouseDelta{-4.0, 0.0}, InputOrigin::kPhysical});
    ASSERT_TRUE(left->poll(now, 0ms).ok());
    pump_pair(*left, *right, now, 2);
    EXPECT_EQ(left->router().state(), RouterState::kRemote);
  }
  left_backend.emit_physical(
      InputEvent{{}, {}, 0, 0, MouseDelta{-1.0, 0.0}, InputOrigin::kPhysical});
  ASSERT_TRUE(left->poll(now, 0ms).ok());
  pump_pair(*left, *right, now, 4);
  EXPECT_EQ(left->router().state(), RouterState::kLocal);
  ASSERT_TRUE(left_backend.pointer_position().has_value());
  EXPECT_DOUBLE_EQ(left_backend.pointer_position()->x, 100.0);
  EXPECT_DOUBLE_EQ(left_backend.pointer_position()->y, 150.0);
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
