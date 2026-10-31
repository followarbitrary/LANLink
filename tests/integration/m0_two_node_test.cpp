#include "lanlink/common/clock.hpp"
#include "lanlink/common/config.hpp"
#include "lanlink/input/mocks.hpp"
#include "lanlink/input/router.hpp"

#include <chrono>
#include <gtest/gtest.h>

using namespace std::chrono_literals;
using namespace lanlink;

namespace {
Uuid128 id(std::uint64_t n) { return {n, n + 1000}; }

struct TwoNode {
  SourceId source{id(1)};
  DisplayId target{id(2)};
  AuthenticatedPeer peer{id(3)};
  RequestId request{id(4)};
  InputRouter router;
  InputArbiter arbiter;
  MockBackend backend;
  MockTransport transport;
  MockClock clock;

  TwoNode() : router(source, peer, id(10), request), transport(256) {
    transport.connect(&router);
    transport.connect(&arbiter, peer);
    transport.connect(&backend);
  }

  void send(const std::vector<RouterAction>& actions) {
    for (const auto& action : actions) {
      ASSERT_TRUE(transport.send(action));
    }
  }

  void pump(IClock::time_point now) {
    transport.pump(router, now);
  }

  void pump() { pump(clock.now()); }

  void acquire() {
    const auto now = clock.now();
    send(router.on_crossing(Crossing{target, Edge::kLeft, 0.5, {108, 50}}, now));
    pump(now);
    pump(now);
  }
};

InputEvent key(std::uint32_t code, bool pressed) {
  return InputEvent{{}, {}, 0, 0, Key{code, pressed}, InputOrigin::kSynthetic};
}
}  // namespace

TEST(M0TwoNode, AcquireActivateMoveReleaseAndCleanup) {
  TwoNode node;
  node.acquire();
  ASSERT_EQ(node.router.state(), RouterState::kRemote);
  ASSERT_TRUE(node.arbiter.has_active_lease());
  ASSERT_TRUE(node.backend.suppressed());

  node.send(node.router.on_local_event(key(65, true), {}, {}));
  node.pump();
  ASSERT_EQ(node.backend.injected_events.size(), 1u);
  EXPECT_EQ(std::get<Key>(node.backend.injected_events.back().payload).code, 65u);
  EXPECT_TRUE(node.arbiter.pressed_keys().contains(65));

  node.send(node.router.on_local_event(key(65, false), {}, {}));
  node.pump();
  EXPECT_TRUE(node.arbiter.ledger_empty());

  InputEvent move{{}, {}, 0, 0, MouseDelta{3.5, -2.0}, InputOrigin::kSynthetic};
  node.send(node.router.on_local_event(move, {}, {}));
  node.pump();
  ASSERT_EQ(node.backend.injected_events.size(), 3u);
  ASSERT_TRUE(std::holds_alternative<MouseDelta>(node.backend.injected_events.back().payload));
  EXPECT_DOUBLE_EQ(std::get<MouseDelta>(node.backend.injected_events.back().payload).dx, 3.5);
  EXPECT_DOUBLE_EQ(std::get<MouseDelta>(node.backend.injected_events.back().payload).dy, -2.0);

  auto cleanup = node.router.on_disconnect({});
  node.send(cleanup);
  node.pump();
  EXPECT_FALSE(node.arbiter.has_lease());
  EXPECT_EQ(node.router.state(), RouterState::kRecovering);
  ASSERT_FALSE(node.backend.released_sessions.empty());
  EXPECT_FALSE(node.backend.suppressed());
}

TEST(M0TwoNode, ConcurrentRequestsToOneTargetGrantAtMostOne) {
  InputRouter a(id(51), AuthenticatedPeer{id(50)}, id(59), id(61));
  InputRouter c(id(52), AuthenticatedPeer{id(50)}, id(59), id(62));
  InputArbiter target;
  AuthenticatedPeer peer{id(50)};
  MockTransport ta;
  MockTransport tc;
  ta.connect(&a);
  ta.connect(&target, peer);
  tc.connect(&c);
  tc.connect(&target, peer);

  const auto crossing = Crossing{id(60), Edge::kLeft, .5, {1, 1}};
  for (const auto& action : a.on_crossing(crossing, {})) ASSERT_TRUE(ta.send(action));
  for (const auto& action : c.on_crossing(crossing, {})) ASSERT_TRUE(tc.send(action));
  ta.pump(a, {});
  tc.pump(c, {});
  ta.pump(a, {});
  tc.pump(c, {});
  ASSERT_TRUE(target.lease().has_value());
  EXPECT_EQ(target.lease()->request, id(61));
  EXPECT_EQ(a.state(), RouterState::kRemote);
  EXPECT_EQ(c.state(), RouterState::kRecovering);
}

TEST(M0TwoNode, DifferentTargetsHaveIndependentSessions) {
  TwoNode a;
  TwoNode c;
  c.source = id(70);
  c.target = id(72);
  c.router = InputRouter(c.source, c.peer, id(11), id(71));
  c.transport.connect(&c.router);
  a.send(a.router.on_crossing(Crossing{a.target, Edge::kLeft, .5, {1, 1}}, a.clock.now()));
  c.send(c.router.on_crossing(Crossing{c.target, Edge::kLeft, .5, {1, 1}}, c.clock.now()));
  a.pump();
  c.pump();
  a.pump();
  c.pump();
  EXPECT_EQ(a.router.state(), RouterState::kRemote);
  EXPECT_EQ(c.router.state(), RouterState::kRemote);
  EXPECT_TRUE(a.arbiter.has_active_lease());
  EXPECT_TRUE(c.arbiter.has_active_lease());
  EXPECT_NE(a.arbiter.lease()->session, c.arbiter.lease()->session);
}

TEST(M0TwoNode, RejectsStaleGenerationAndSequenceAndHonorsLeaseBoundary) {
  TwoNode node;
  node.acquire();
  const auto first = *node.arbiter.lease();

  node.clock.advance(2999ms);
  node.send(node.router.on_local_event(
      InputEvent{{}, {}, 0, 0, MouseDelta{1.0, 0.0}, InputOrigin::kSynthetic}, {},
      node.clock.now()));
  node.pump();
  ASSERT_TRUE(node.arbiter.has_active_lease());
  ASSERT_EQ(node.backend.injected_events.size(), 1u);

  node.clock.advance(1ms);
  node.send(node.router.on_local_event(
      InputEvent{{}, {}, 0, 0, MouseDelta{0.0, 1.0}, InputOrigin::kSynthetic}, {},
      node.clock.now()));
  node.pump();
  node.pump();
  EXPECT_FALSE(node.arbiter.has_lease());
  EXPECT_EQ(node.router.state(), RouterState::kRecovering);

  node.router.on_tick(node.clock.now() + 1ms);
  node.clock.advance(1ms);
  node.acquire();
  const auto second = *node.arbiter.lease();
  ASSERT_NE(first.generation, second.generation);
  const auto injected_before_stale = node.backend.injected_events.size();

  RouterAction stale;
  stale.kind = RouterActionKind::kSendInput;
  stale.source = node.source;
  stale.session = second.session;
  stale.generation = first.generation;
  stale.seq = 1;
  stale.event = InputEvent{node.source, second.session, first.generation, 1,
                           Key{66, true}, InputOrigin::kRemoteInjected};
  ASSERT_TRUE(node.transport.send(stale));
  node.pump();
  EXPECT_TRUE(node.arbiter.has_active_lease());
  EXPECT_EQ(node.backend.injected_events.size(), injected_before_stale);

  RouterAction gap = stale;
  gap.generation = second.generation;
  gap.seq = 3;
  gap.event.generation = second.generation;
  gap.event.seq = 3;
  ASSERT_TRUE(node.transport.send(gap));
  node.pump();
  node.pump();
  EXPECT_FALSE(node.arbiter.has_lease());
  EXPECT_EQ(node.router.state(), RouterState::kRecovering);
}

TEST(M0TwoNode, LocalPhysicalInputPreemptsRemoteLease) {
  TwoNode node;
  node.acquire();
  ASSERT_TRUE(node.arbiter.has_active_lease());
  node.arbiter.on_local_input(
      InputEvent{{}, {}, 0, 0, Key{65, true}, InputOrigin::kPhysical}, {});
  EXPECT_FALSE(node.arbiter.has_lease());
  EXPECT_EQ(node.arbiter.last_revoke_reason(), RevokeReason::kLocalPreempt);
}
