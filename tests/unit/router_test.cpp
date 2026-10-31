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
  EXPECT_EQ(r.on_tick(IClock::time_point{} + 1000ms)[0].kind, RouterActionKind::kRenew);
  InputEvent injected{{}, {}, 0, 0, Key{65, true}, InputOrigin::kRemoteInjected}; EXPECT_TRUE(r.on_local_event(injected, {}, {}).empty());
  InputEvent physical{{}, {}, 0, 0, Key{65, true}, InputOrigin::kPhysical}; EXPECT_EQ(r.on_local_event(physical, {}, {}).size(), 3u); EXPECT_EQ(r.state(), RouterState::kRecovering);
}
TEST(MockTransport, OverflowFailsClosedAndRecoversConnectedRouter) {
  InputRouter r(id(40)); r.on_crossing(Crossing{id(41), Edge::kLeft, .5, {}}, {}); MockTransport t(0); t.connect(&r); RouterAction a; a.kind=RouterActionKind::kSendInput; EXPECT_FALSE(t.send(a)); EXPECT_EQ(r.state(), RouterState::kRecovering); EXPECT_TRUE(t.overflowed());
}
