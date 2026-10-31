#include "lanlink/common/clock.hpp"
#include "lanlink/input/mocks.hpp"
#include "lanlink/input/router.hpp"

#include <gtest/gtest.h>

using namespace lanlink;

namespace {
Uuid128 id(std::uint64_t n) { return {n, n + 2000}; }

struct Harness {
  InputRouter router{id(1), AuthenticatedPeer{id(2)}, id(3), id(4)};
  InputArbiter arbiter;
  MockBackend backend;
  MockTransport transport;
  MockClock clock;
  Harness() {
    transport.connect(&router);
    transport.connect(&arbiter, AuthenticatedPeer{id(2)});
    transport.connect(&backend);
  }
  void send(const std::vector<RouterAction>& actions) {
    for (const auto& action : actions) EXPECT_TRUE(transport.send(action));
  }
  void acquire() {
    send(router.on_crossing(Crossing{id(5), Edge::kLeft, .5, {1, 1}}, clock.now()));
    transport.pump(router, clock.now());
    transport.pump(router, clock.now());
  }
};
}

TEST(M0Recovery, DroppedActivationRollsBackSuppressionAndLease) {
  Harness h;
  h.router.on_crossing(Crossing{id(5), Edge::kLeft, .5, {1, 1}}, h.clock.now());
  auto grant = h.arbiter.acquire(AuthenticatedPeer{id(2)}, id(1), id(5), id(4), h.clock.now());
  ASSERT_TRUE(grant.granted());
  const auto actions = h.router.on_grant(grant, h.clock.now());
  ASSERT_EQ(actions.size(), 2u);
  h.send({actions.front()});
  h.transport.set_drop_next();
  h.send({actions.back()});
  h.transport.pump(h.router, h.clock.now());
  h.transport.pump(h.router, h.clock.now());
  EXPECT_FALSE(h.arbiter.has_lease());
  EXPECT_FALSE(h.backend.suppressed());
  ASSERT_GE(h.backend.suppression_history.size(), 2u);
  EXPECT_FALSE(h.backend.suppression_history.back().enabled);
  EXPECT_EQ(h.router.state(), RouterState::kRecovering);
}

TEST(M0Recovery, DisconnectReleasesInjectedStateAndRestoresLocal) {
  Harness h;
  h.acquire();
  ASSERT_EQ(h.router.state(), RouterState::kRemote);
  auto actions = h.router.on_disconnect(h.clock.now());
  h.send(actions);
  h.transport.pump(h.router, h.clock.now());
  EXPECT_FALSE(h.arbiter.has_lease());
  EXPECT_FALSE(h.backend.released_sessions.empty());
  EXPECT_FALSE(h.backend.suppressed());
  EXPECT_EQ(h.router.state(), RouterState::kRecovering);
}

TEST(M0Recovery, StaleRenewAfterDisconnectCannotExtendLease) {
  Harness h;
  h.acquire();
  ASSERT_TRUE(h.arbiter.lease().has_value());
  const auto lease = *h.arbiter.lease();
  h.send(h.router.on_disconnect(h.clock.now()));
  h.transport.pump(h.router, h.clock.now());
  h.backend.clear();

  RouterAction stale;
  stale.kind = RouterActionKind::kRenew;
  stale.source = h.router.source();
  stale.session = lease.session;
  stale.generation = lease.generation;
  ASSERT_TRUE(h.transport.send(stale));
  h.transport.pump(h.router, h.clock.now());
  h.transport.pump(h.router, h.clock.now());
  EXPECT_FALSE(h.arbiter.has_lease());
  EXPECT_FALSE(h.backend.released_sessions.empty());
}

TEST(M0Recovery, SequenceGapRevokesAndCleansEveryPressedInput) {
  InputArbiter arbiter;
  const auto peer = AuthenticatedPeer{id(10)};
  const auto source = id(11);
  MockClock clock;
  auto grant = arbiter.acquire(peer, source, id(12), id(13), clock.now());
  ASSERT_TRUE(grant.granted());
  ASSERT_TRUE(arbiter.activate(grant.lease.session, grant.lease.generation, clock.now()));
  EXPECT_TRUE(arbiter.validate(peer, InputEvent{source, grant.lease.session, grant.lease.generation,
                                                1, Key{65, true}, InputOrigin::kRemoteInjected},
                              clock.now()));
  EXPECT_TRUE(arbiter.validate(peer, InputEvent{source, grant.lease.session, grant.lease.generation,
                                                2, MouseButton{1, true}, InputOrigin::kRemoteInjected},
                              clock.now()));
  auto gap = arbiter.validate(peer, InputEvent{source, grant.lease.session, grant.lease.generation,
                                               4, Key{66, true}, InputOrigin::kRemoteInjected},
                                  clock.now());
  EXPECT_TRUE(gap.revoked());
  EXPECT_TRUE(arbiter.ledger_empty());
  EXPECT_EQ(arbiter.released_keys(), std::vector<std::uint32_t>({65}));
  EXPECT_EQ(arbiter.released_buttons(), std::vector<std::uint32_t>({1}));
}

TEST(M0Recovery, QueueOverflowFailsClosedAndAbortsHeldButtonHandoff) {
  Harness h;
  h.acquire();
  ASSERT_EQ(h.router.state(), RouterState::kRemote);
  InputEvent down{{}, {}, 0, 0, MouseButton{1, true}, InputOrigin::kSynthetic};
  h.send(h.router.on_local_event(down, {}, h.clock.now()));
  h.transport.pump(h.router, h.clock.now());
  ASSERT_TRUE(h.arbiter.pressed_buttons().contains(1));

  h.transport.set_queue_capacity(1);
  auto handoff = h.router.on_crossing(Crossing{id(6), Edge::kRight, .4, {2, 2}}, h.clock.now());
  ASSERT_EQ(handoff.size(), 2u);
  ASSERT_TRUE(h.transport.send(handoff.front()));
  EXPECT_FALSE(h.transport.send(handoff.back()));
  EXPECT_TRUE(h.transport.overflowed());
  EXPECT_EQ(h.router.state(), RouterState::kRecovering);
  EXPECT_FALSE(h.arbiter.has_lease());
  EXPECT_FALSE(h.backend.suppressed());
  EXPECT_TRUE(h.arbiter.ledger_empty());
  EXPECT_EQ(h.arbiter.released_buttons(), std::vector<std::uint32_t>({1}));
  EXPECT_FALSE(h.backend.released_sessions.empty());
}
