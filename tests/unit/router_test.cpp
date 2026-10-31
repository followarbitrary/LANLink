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
