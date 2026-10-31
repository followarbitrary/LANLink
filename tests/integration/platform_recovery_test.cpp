#include "lanlink/input/router.hpp"
#include "lanlink/platform/fake_input_backend.hpp"

#include <gtest/gtest.h>

namespace lanlink::platform {
namespace {

Uuid128 id(std::uint64_t value) { return {value, value + 100}; }

TEST(PlatformRecovery, RouterCleanupActionsRestoreBackendAndClearInjectedState) {
  const SourceId source = id(1);
  const AuthenticatedPeer peer{id(2)};
  const DisplayId target = id(3);
  const RequestId request = id(4);
  InputRouter router(source, peer, id(5), request);
  InputArbiter arbiter;
  FakeInputBackend backend;
  ASSERT_TRUE(backend.start({}).ok());
  ASSERT_TRUE(backend.set_suppression(true).ok());

  const auto grant = arbiter.acquire(peer, source, target, request, {});
  ASSERT_TRUE(grant.granted());
  router.on_crossing(Crossing{target, Edge::kLeft, 0.5, {}}, {});
  ASSERT_TRUE(router.on_grant(grant, {}).size() == 2u);
  ASSERT_TRUE(router.on_activation_ack(true, {}).empty());
  ASSERT_EQ(router.state(), RouterState::kRemote);

  const InputEvent key{{}, {}, 0, 0, Key{65, true}, InputOrigin::kSynthetic};
  const auto send = router.on_local_event(key, {}, {});
  ASSERT_EQ(send.size(), 1u);
  ASSERT_TRUE(backend.inject(send.front().event).ok());
  ASSERT_EQ(backend.held_count(), 1u);

  const auto cleanup = router.on_disconnect({});
  ASSERT_EQ(cleanup.size(), 3u);
  EXPECT_EQ(cleanup[0].kind, RouterActionKind::kRelease);
  EXPECT_EQ(cleanup[1].kind, RouterActionKind::kReleaseInjectedState);
  EXPECT_EQ(cleanup[2].kind, RouterActionKind::kRestoreLocal);

  for (const auto& action : cleanup) {
    if (action.kind == RouterActionKind::kReleaseInjectedState) {
      backend.release_injected_state(action.session);
    } else if (action.kind == RouterActionKind::kRestoreLocal) {
      ASSERT_TRUE(backend.set_suppression(false).ok());
    }
  }
  EXPECT_EQ(backend.held_count(), 0u);
  EXPECT_FALSE(backend.status().suppression);
  EXPECT_EQ(router.state(), RouterState::kRecovering);
}

}  // namespace
}  // namespace lanlink::platform
