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
  const ScreenLink link{local, local_left ? Edge::kRight : Edge::kLeft, remote,
                        local_left ? Edge::kLeft : Edge::kRight, 0.0, 1.0, 1};
  EXPECT_TRUE(topology.set_snapshot(1, {first, second}, {link}).ok());
  return topology;
}

InputRuntimeConfig config(DeviceId local_device, SourceId source, AuthenticatedPeer peer,
                          DisplayId local_display, Endpoint listen,
                          IInputBackend* backend) {
  InputRuntimeConfig value;
  value.local_device = local_device;
  value.source = source;
  value.peer = peer;
  value.local_display = local_display;
  value.listen_endpoint = std::move(listen);
  value.backend = backend;
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

  left_backend.emit_physical(
      InputEvent{{}, {}, 0, 0, Key{0x04, true}, InputOrigin::kPhysical});
  left_backend.emit_physical(
      InputEvent{{}, {}, 0, 0, MouseButton{1, true}, InputOrigin::kPhysical});
  ASSERT_TRUE(left->poll(now, 0ms).ok());
  pump_pair(*left, *right, now, 4);
  ASSERT_EQ(right_backend.injected_events().size(), 3u);
  EXPECT_EQ(std::get<Key>(right_backend.injected_events()[1].payload).code, 0x04u);
  EXPECT_EQ(std::get<MouseButton>(right_backend.injected_events()[2].payload).button, 1u);

  left_backend.emit_physical(
      InputEvent{{}, {}, 0, 0, MouseButton{1, false}, InputOrigin::kPhysical});
  left_backend.emit_physical(
      InputEvent{{}, {}, 0, 0, Key{0x04, false}, InputOrigin::kPhysical});
  ASSERT_TRUE(left->poll(now, 0ms).ok());
  for (int i = 0; i < 4; ++i) ASSERT_TRUE(right->poll(now, 10ms).ok());
  EXPECT_EQ(right_backend.held_count(), 0u);

  ASSERT_TRUE(right->stop(now).ok());
  (void)left->poll(now, 0ms);
  EXPECT_EQ(left->router().state(), RouterState::kRecovering);
  EXPECT_FALSE(left_backend.status().suppression);
  ASSERT_TRUE(left->tick(now).ok());
  EXPECT_EQ(left->router().state(), RouterState::kLocal);
}

}  // namespace
