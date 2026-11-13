#include "lanlink/input/mocks.hpp"
#include <gtest/gtest.h>
#include <chrono>
using namespace std::chrono_literals;
using namespace lanlink;
namespace { Uuid128 id(std::uint64_t n){ return {n,n+100}; } }
TEST(InputRouter, LocalAcquireGrantActivateRemote) {
  InputRouter r(id(1), AuthenticatedPeer{id(2)}, id(3), id(4));
  auto a=r.on_crossing(Crossing{id(5),Edge::kLeft,0.5,{1,2}},{});
  ASSERT_EQ(a.size(),1u); EXPECT_EQ(a[0].kind,RouterActionKind::kAcquire); EXPECT_EQ(r.state(),RouterState::kAcquiring);
  InputArbiter arb; MockTransport t; t.connect(&arb,AuthenticatedPeer{id(2)}); for(auto& x:a)t.send(x); auto generated=t.pump(r,{}); ASSERT_EQ(generated.size(),2u);
  for(auto& x:generated)t.send(x); t.pump(r,{}); EXPECT_EQ(r.state(),RouterState::kRemote);
}

TEST(InputRouter, ReplaysHeldModifierWhenHandoffActivates) {
  InputRouter r(id(5), AuthenticatedPeer{id(6)}, id(7), id(8));
  ASSERT_TRUE(r.on_local_event(
                  InputEvent{{}, {}, 0, 0, Key{0xE3, true}, InputOrigin::kPhysical}, {}, {})
                  .empty());
  ASSERT_TRUE(r.on_local_event(
                  InputEvent{{}, {}, 0, 0, Key{0xE0, true}, InputOrigin::kPhysical}, {}, {})
                  .empty());

  ASSERT_EQ(r.on_crossing(Crossing{id(9), Edge::kRight, 0.5, {10, 20}}, {}).size(), 1u);
  InputArbiter arbiter;
  auto grant = arbiter.acquire(AuthenticatedPeer{id(6)}, id(5), id(9), id(8), {});
  ASSERT_TRUE(grant.granted());
  ASSERT_EQ(r.on_grant(grant, {}).size(), 2u);

  const auto sync = r.on_activation_ack(true, {});
  ASSERT_EQ(sync.size(), 2u);
  ASSERT_TRUE(std::holds_alternative<Key>(sync[0].event.payload));
  ASSERT_TRUE(std::holds_alternative<Key>(sync[1].event.payload));
  EXPECT_EQ(std::get<Key>(sync[0].event.payload).code, 0xE0u);
  EXPECT_EQ(std::get<Key>(sync[1].event.payload).code, 0xE3u);
  EXPECT_TRUE(std::get<Key>(sync[0].event.payload).pressed);
  EXPECT_TRUE(std::get<Key>(sync[1].event.payload).pressed);
  EXPECT_EQ(sync[0].event.seq, 1u);
  EXPECT_EQ(sync[1].event.seq, 2u);
}

TEST(MockTransport, CoalescesOnlyAdjacentDeltas) {
  MockTransport t; RouterAction a; a.kind=RouterActionKind::kSendInput; a.source=id(1); a.session=id(2); a.generation=1; a.event={id(1),id(2),1,1,MouseDelta{1,2},InputOrigin::kPhysical};
  auto b=a; b.seq=2;b.event.seq=2;b.event.payload=MouseDelta{3,4}; EXPECT_TRUE(t.send(a)); EXPECT_TRUE(t.send(b)); EXPECT_EQ(t.queue_size(),1u); auto out=t.deliver(); ASSERT_EQ(out.size(),1u); auto d=std::get<MouseDelta>(out[0].event.payload); EXPECT_DOUBLE_EQ(d.dx,4); EXPECT_DOUBLE_EQ(d.dy,6);
}
TEST(InputRouter, HeldButtonDelaysHandoffUntilRelease) {
  InputRouter r(id(10), AuthenticatedPeer{id(11)}, id(12), id(13));
  InputEvent down{{}, {}, 0, 0, MouseButton{1, true}, InputOrigin::kPhysical};
  r.on_local_event(down, {}, {});
  EXPECT_TRUE(r.on_crossing(Crossing{id(14), Edge::kRight, .2, {3, 4}}, {}).empty());
  EXPECT_EQ(r.state(), RouterState::kHandoff);
  auto actions = r.on_local_event(InputEvent{{}, {}, 0, 0, MouseButton{1, false}, InputOrigin::kPhysical}, {}, {});
  ASSERT_EQ(actions.size(), 1u); EXPECT_EQ(actions[0].kind, RouterActionKind::kAcquire);
}
TEST(InputRouter, ActivationFailureRollsBackAndRenewIsScheduled) {
  InputRouter r(id(20), AuthenticatedPeer{id(21)}, id(22), id(23));
  auto acquire = r.on_crossing(Crossing{id(24), Edge::kLeft, .5, {}}, {});
  InputArbiter arb; auto grant = arb.acquire(AuthenticatedPeer{id(21)}, id(20), id(24), id(23), {});
  ASSERT_TRUE(grant.granted()); r.on_grant(grant, {}); EXPECT_EQ(r.on_activation_ack(false, {}).size(), 3u);
  EXPECT_EQ(r.state(), RouterState::kRecovering);
  r.on_tick(IClock::time_point{} + 1ms); EXPECT_EQ(r.state(), RouterState::kLocal);
  (void)acquire;
}
TEST(InputRouter, LocalPreemptionAndRenew) {
  InputRouter r(id(30), AuthenticatedPeer{id(31)}, id(32), id(33));
  auto c = r.on_crossing(Crossing{id(34), Edge::kLeft, .5, {}}, {});
  InputArbiter arb; MockTransport t; t.connect(&arb, AuthenticatedPeer{id(31)}); for (auto& a : c) t.send(a); auto g=t.pump(r, {}); for(auto& a:g)t.send(a); t.pump(r, {}); ASSERT_EQ(r.state(), RouterState::kRemote);
  auto renew = r.on_tick(IClock::time_point{} + 1000ms); ASSERT_EQ(renew.size(), 1u); EXPECT_EQ(renew[0].kind, RouterActionKind::kRenew); for (auto& a : renew) t.send(a); t.pump(r, IClock::time_point{} + 1000ms);
  EXPECT_EQ(r.state(), RouterState::kRemote); ASSERT_TRUE(r.lease().has_value()); EXPECT_GT(r.lease()->expires_at, IClock::time_point{} + 3000ms);
  InputEvent injected{{}, {}, 0, 0, Key{65, true}, InputOrigin::kRemoteInjected}; EXPECT_TRUE(r.on_local_event(injected, {}, {}).empty());
  InputEvent physical{{}, {}, 0, 0, Key{65, true}, InputOrigin::kPhysical}; EXPECT_EQ(r.on_local_event(physical, {}, {}).size(), 3u); EXPECT_EQ(r.state(), RouterState::kRecovering);
}

TEST(InputRouter, SuppressedPhysicalInputIsForwarded) {
  InputRouter r(id(35), AuthenticatedPeer{id(36)}, id(37), id(38));
  InputArbiter arb;
  MockTransport t;
  t.connect(&arb, AuthenticatedPeer{id(36)});
  for (const auto& action : r.on_crossing(Crossing{id(39), Edge::kRight, .5, {}}, {}))
    ASSERT_TRUE(t.send(action));
  auto granted = t.pump(r, {});
  for (const auto& action : granted) ASSERT_TRUE(t.send(action));
  t.pump(r, {});
  ASSERT_EQ(r.state(), RouterState::kRemote);

  auto forwarded = r.on_suppressed_event(
      InputEvent{{}, {}, 0, 0, Key{0x04, true}, InputOrigin::kPhysical}, {}, {});
  ASSERT_EQ(forwarded.size(), 1u);
  EXPECT_EQ(forwarded.front().kind, RouterActionKind::kSendInput);
  EXPECT_EQ(r.state(), RouterState::kRemote);
}
TEST(MockTransport, OverflowFailsClosedAndRecoversConnectedRouter) {
  InputRouter r(id(40)); r.on_crossing(Crossing{id(41), Edge::kLeft, .5, {}}, {}); MockTransport t(0); t.connect(&r); RouterAction a; a.kind=RouterActionKind::kSendInput; EXPECT_FALSE(t.send(a)); EXPECT_EQ(r.state(), RouterState::kRecovering); EXPECT_TRUE(t.overflowed());
}

TEST(InputRouter, StaleGrantAndAckAreIgnored) {
  InputRouter r(id(50), AuthenticatedPeer{id(51)}, id(52), id(53));
  r.on_crossing(Crossing{id(54), Edge::kLeft, .5, {}}, {});
  AcquireResult stale; stale.granted_value = true; stale.lease.source = id(50); stale.lease.peer = AuthenticatedPeer{id(51)}; stale.lease.target_display = id(55); stale.lease.request = id(99); stale.lease.session = id(99); stale.lease.generation = 1;
  EXPECT_TRUE(r.on_grant(stale, {}).empty()); EXPECT_EQ(r.state(), RouterState::kAcquiring);
  AcquireResult valid = stale; valid.lease.target_display = id(54); valid.lease.request = id(53); valid.lease.session = id(53);
  ASSERT_EQ(r.on_grant(valid, {}).size(), 2u);
  EXPECT_TRUE(r.on_activation_ack(id(99), 1, true, {}).empty()); EXPECT_EQ(r.state(), RouterState::kAcquiring);
  EXPECT_TRUE(r.on_activation_ack(id(53), valid.lease.generation, true, {}).empty()); EXPECT_EQ(r.state(), RouterState::kRemote);
}

TEST(InputRouter, ReleasePrecedesNextAcquire) {
  InputRouter r(id(60), AuthenticatedPeer{id(61)}, id(62), id(63));
  InputArbiter arb; MockTransport t; t.connect(&arb, AuthenticatedPeer{id(61)});
  auto first = r.on_crossing(Crossing{id(64), Edge::kLeft, .5, {}}, {}); for (auto& a : first) t.send(a); auto grant=t.pump(r, {}); for(auto& a:grant)t.send(a); t.pump(r, {}); ASSERT_EQ(r.state(), RouterState::kRemote);
  auto handoff = r.on_crossing(Crossing{id(65), Edge::kRight, .5, {}}, {}); ASSERT_EQ(handoff.size(), 2u);
  EXPECT_EQ(handoff[0].kind, RouterActionKind::kRelease); EXPECT_EQ(handoff[1].kind, RouterActionKind::kAcquire);
}

TEST(InputRouter, PhysicalZeroWarpAndInjectedDoNotPreempt) {
  InputRouter r(id(70), AuthenticatedPeer{id(71)}, id(72), id(73));
  InputArbiter arb; MockTransport t; t.connect(&arb, AuthenticatedPeer{id(71)}); auto c=r.on_crossing(Crossing{id(74),Edge::kLeft,.5,{}},{}); for(auto&a:c)t.send(a); auto g=t.pump(r,{}); for(auto&a:g)t.send(a); t.pump(r,{}); ASSERT_EQ(r.state(),RouterState::kRemote);
  r.on_local_event(InputEvent{{},{},0,0,Key{0,true},InputOrigin::kPhysical},{},{});
  r.on_local_event(InputEvent{{},{},0,0,MouseWheel{0},InputOrigin::kPhysical},{},{});
  r.on_local_event(InputEvent{{},{},0,0,PointerWarp{2,3},InputOrigin::kPhysical},{},{});
  EXPECT_EQ(r.state(),RouterState::kRemote);
}

TEST(MockBackend, OriginHelpersTagCallbacksDeterministically) {
  MockBackend b; InputEvent e{{},{},0,0,Key{65,true},InputOrigin::kPhysical}; InputEvent seen{}; b.emit_remote_injected=[&](const InputEvent& x){seen=x;};
  b.emit_remote_event(e); EXPECT_EQ(seen.origin, InputOrigin::kRemoteInjected); EXPECT_EQ(b.injected_events.back().origin, InputOrigin::kRemoteInjected);
  b.emit_physical=[&](const InputEvent& x){seen=x;}; b.emit_physical_event(InputEvent{{},{},0,0,Key{65,true},InputOrigin::kRemoteInjected}); EXPECT_EQ(seen.origin, InputOrigin::kPhysical);
}

TEST(MockTransport, OverflowExecutesCleanupAgainstArbiter) {
  InputRouter r(id(80), AuthenticatedPeer{id(81)}, id(82), id(83)); InputArbiter arb; MockBackend backend; MockTransport t; t.connect(&arb, AuthenticatedPeer{id(81)}); t.connect(&r); t.connect(&backend);
  auto c=r.on_crossing(Crossing{id(84),Edge::kLeft,.5,{}},{}); for(auto&a:c)t.send(a); auto g=t.pump(r,{}); for(auto&a:g)t.send(a); t.pump(r,{}); ASSERT_EQ(r.state(),RouterState::kRemote);
  InputEvent key{{},{},0,0,Key{65,true},InputOrigin::kSynthetic}; auto input=r.on_local_event(key,{},{}); for(auto&a:input)t.send(a); t.pump(r,{}); ASSERT_FALSE(arb.ledger_empty());
  t.set_queue_capacity(0); RouterAction trigger; trigger.kind=RouterActionKind::kSendInput; EXPECT_FALSE(t.send(trigger)); EXPECT_TRUE(arb.ledger_empty()); EXPECT_FALSE(arb.has_lease()); EXPECT_FALSE(backend.released_sessions.empty()); EXPECT_EQ(r.state(),RouterState::kRecovering);
}

TEST(MockTransport, FaultControlsAreDeterministic) {
  MockTransport t; RouterAction a; a.kind=RouterActionKind::kDrop; t.set_drop_next(); EXPECT_TRUE(t.send(a)); EXPECT_EQ(t.queue_size(),0u);
  t.set_delay(2); EXPECT_TRUE(t.send(a)); EXPECT_EQ(t.deliver().size(),0u); t.advance(2); EXPECT_EQ(t.deliver().size(),1u);
  t.set_duplicate_next(); EXPECT_TRUE(t.send(a)); EXPECT_EQ(t.queue_size(),2u);
  t.set_reorder_next(); EXPECT_TRUE(t.send(a)); EXPECT_GE(t.queue_size(),2u);
}

TEST(MockTransport, HandoffOverflowReleasesQueuedOldLease) {
  InputRouter r(id(90), AuthenticatedPeer{id(91)}, id(92), id(93));
  InputArbiter arb; MockBackend backend; MockTransport t; t.connect(&arb, AuthenticatedPeer{id(91)}); t.connect(&r); t.connect(&backend);
  auto acquire = r.on_crossing(Crossing{id(94), Edge::kLeft, .5, {}}, {}); for (auto& a : acquire) t.send(a); auto granted = t.pump(r, {}); for (auto& a : granted) t.send(a); t.pump(r, {}); ASSERT_EQ(r.state(), RouterState::kRemote); ASSERT_TRUE(arb.has_active_lease());
  t.set_queue_capacity(1);
  auto handoff = r.on_crossing(Crossing{id(95), Edge::kRight, .5, {}}, {}); ASSERT_EQ(handoff.size(), 2u);
  ASSERT_TRUE(t.send(handoff[0])); EXPECT_FALSE(t.send(handoff[1]));
  EXPECT_FALSE(arb.has_lease()); EXPECT_FALSE(backend.released_sessions.empty()); EXPECT_EQ(r.state(), RouterState::kRecovering);
}
