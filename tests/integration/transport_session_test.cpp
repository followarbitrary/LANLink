#include "lanlink/common/config.hpp"
#include "lanlink/platform/fake_input_backend.hpp"
#include "lanlink/transport/session.hpp"

#include <chrono>
#include <gtest/gtest.h>

namespace {
using namespace lanlink;
using namespace lanlink::platform;
using namespace lanlink::protocol;
using namespace lanlink::transport;
using namespace std::chrono_literals;

Uuid128 id(std::uint64_t value) { return {value, value + 1000}; }

TEST(PeerSession, CompletesAcquireActivateInputAndReleaseOverTcp) {
    auto listener_result = Listener::bind(Endpoint{"127.0.0.1", 0});
    ASSERT_TRUE(listener_result.ok()) << listener_result.error();
    auto listener = std::move(listener_result.value());
    const auto port = listener.local_endpoint().port;

    auto client_result = Connection::connect(Endpoint{"127.0.0.1", port});
    ASSERT_TRUE(client_result.ok()) << client_result.error();
    auto accepted = listener.accept(2s);
    ASSERT_TRUE(accepted.ok()) << accepted.error();

    InputRouter router{id(1), AuthenticatedPeer{id(3)}, id(10), id(4)};
    InputArbiter arbiter;
    FakeInputBackend source_backend;
    FakeInputBackend target_backend;
    ASSERT_TRUE(source_backend.start({}).ok());
    ASSERT_TRUE(target_backend.start({}).ok());
    target_backend.set_display_bounds(Rect{0.0, 0.0, 1920.0, 1080.0});
    PeerSession source(
        std::move(client_result.value()),
        PeerSessionConfig{id(2), AuthenticatedPeer{id(3)}, &router, nullptr, &source_backend});
    PeerSession target(
        std::move(accepted.value()),
        PeerSessionConfig{id(3), AuthenticatedPeer{id(2)}, nullptr, &arbiter, &target_backend});

    ASSERT_TRUE(source.start({}).ok());
    ASSERT_TRUE(target.start({}).ok());
    ASSERT_TRUE(source.pump({}).ok());
    ASSERT_TRUE(target.pump({}).ok());

    auto actions = router.on_crossing(
        Crossing{id(5), Edge::kLeft, 0.5, {20, 10}, 0.5, true}, {});
    ASSERT_TRUE(source.send_actions(actions).ok());
    ASSERT_TRUE(target.pump({}).ok());
    ASSERT_TRUE(source.pump({}).ok());
    ASSERT_TRUE(target.pump({}).ok());
    ASSERT_TRUE(source.pump({}).ok());
    ASSERT_EQ(router.state(), RouterState::kRemote);
    ASSERT_TRUE(arbiter.has_active_lease());
    ASSERT_TRUE(source_backend.status().suppression);
    ASSERT_EQ(target_backend.injected_events().size(), 1u);
    ASSERT_TRUE(std::holds_alternative<PointerWarp>(target_backend.injected_events().front().payload));
    EXPECT_DOUBLE_EQ(std::get<PointerWarp>(target_backend.injected_events().front().payload).x, 8.0);
    EXPECT_DOUBLE_EQ(std::get<PointerWarp>(target_backend.injected_events().front().payload).y, 540.0);

    auto input = router.on_local_event(
        InputEvent{{}, {}, 0, 0, Key{65, true}, InputOrigin::kSynthetic}, {}, {});
    ASSERT_EQ(input.size(), 1u);
    ASSERT_TRUE(source.send_actions(input).ok());
    ASSERT_TRUE(target.pump({}).ok());
    ASSERT_EQ(target_backend.injected_events().size(), 2u);
    ASSERT_TRUE(std::holds_alternative<Key>(target_backend.injected_events().back().payload));
    EXPECT_TRUE(arbiter.pressed_keys().contains(65));

    RouterAction release;
    release.kind = RouterActionKind::kRelease;
    release.session = router.lease()->session;
    release.generation = router.lease()->generation;
    ASSERT_TRUE(source.send_actions({release}).ok());
    ASSERT_TRUE(target.pump({}).ok());
    EXPECT_FALSE(arbiter.has_lease());
    EXPECT_TRUE(arbiter.ledger_empty());
}

TEST(PeerSession, RefreshesDisplayInfoWhenBoundsOrScaleChange) {
    auto listener_result = Listener::bind(Endpoint{"127.0.0.1", 0});
    ASSERT_TRUE(listener_result.ok()) << listener_result.error();
    auto listener = std::move(listener_result.value());
    auto client_result = Connection::connect(Endpoint{"127.0.0.1", listener.local_endpoint().port});
    ASSERT_TRUE(client_result.ok()) << client_result.error();
    auto accepted = listener.accept(2s);
    ASSERT_TRUE(accepted.ok()) << accepted.error();

    InputRouter source_router{id(50), AuthenticatedPeer{id(52)}, id(60), id(61)};
    InputRouter target_router{id(52), AuthenticatedPeer{id(50)}, id(62), id(63)};
    FakeInputBackend source_backend;
    FakeInputBackend target_backend;
    source_backend.set_display_bounds(Rect{0.0, 0.0, 1512.0, 982.0});
    source_backend.set_display_scale(2.0);
    target_backend.set_display_bounds(Rect{0.0, 0.0, 1920.0, 1080.0});
    target_backend.set_display_scale(1.25);
    target_backend.set_absolute_pointer_moves(true);
    ASSERT_TRUE(source_backend.start({}).ok());
    ASSERT_TRUE(target_backend.start({}).ok());

    PeerSession source(
        std::move(client_result.value()),
        PeerSessionConfig{id(51), AuthenticatedPeer{id(52)}, &source_router, nullptr,
                          &source_backend, id(60)});
    PeerSession target(
        std::move(accepted.value()),
        PeerSessionConfig{id(52), AuthenticatedPeer{id(51)}, &target_router, nullptr,
                          &target_backend, id(62)});

    ASSERT_TRUE(source.start({}).ok());
    ASSERT_TRUE(target.start({}).ok());
    ASSERT_TRUE(source.pump({}).ok());
    ASSERT_TRUE(target.pump({}).ok());
    ASSERT_TRUE(source.pump({}).ok());
    ASSERT_TRUE(source_router.peer_display_bounds().has_value());
    EXPECT_EQ(source_router.peer_display(), id(62));
    EXPECT_DOUBLE_EQ(source_router.peer_display_scale(), 1.25);
    EXPECT_TRUE(source_router.peer_supports_absolute_pointer_moves());
    EXPECT_EQ(*source_router.peer_display_bounds(), (Rect{0.0, 0.0, 1920.0, 1080.0}));

    target_backend.set_display_bounds(Rect{-1440.0, 0.0, 0.0, 900.0});
    target_backend.set_display_scale(2.0);
    ASSERT_TRUE(target.tick(IClock::time_point{} + 1s).ok());
    ASSERT_TRUE(source.pump(IClock::time_point{} + 1s).ok());
    ASSERT_TRUE(source_router.peer_display_bounds().has_value());
    EXPECT_DOUBLE_EQ(source_router.peer_display_scale(), 2.0);
    EXPECT_EQ(*source_router.peer_display_bounds(), (Rect{-1440.0, 0.0, 0.0, 900.0}));

    ASSERT_TRUE(source.disconnect(IClock::time_point{} + 1s).ok());
    EXPECT_FALSE(source_router.peer_display_bounds().has_value());
    EXPECT_EQ(source_router.peer_display(), DisplayId{});
}

TEST(PeerSession, ReportsActiveTargetPointerPosition) {
    auto listener_result = Listener::bind(Endpoint{"127.0.0.1", 0});
    ASSERT_TRUE(listener_result.ok()) << listener_result.error();
    auto listener = std::move(listener_result.value());
    auto client_result = Connection::connect(Endpoint{"127.0.0.1", listener.local_endpoint().port});
    ASSERT_TRUE(client_result.ok()) << client_result.error();
    auto accepted = listener.accept(2s);
    ASSERT_TRUE(accepted.ok()) << accepted.error();

    InputRouter router{id(70), AuthenticatedPeer{id(72)}, id(80), id(81)};
    InputArbiter arbiter;
    FakeInputBackend source_backend;
    FakeInputBackend target_backend;
    ASSERT_TRUE(source_backend.start({}).ok());
    ASSERT_TRUE(target_backend.start({}).ok());
    target_backend.set_display_bounds(Rect{0.0, 0.0, 1920.0, 1080.0});
    std::optional<PointerPosition> reported;
    PeerSession source(
        std::move(client_result.value()),
        PeerSessionConfig{id(71), AuthenticatedPeer{id(72)}, &router, nullptr, &source_backend,
                          {}, [&reported](SessionId session, std::uint64_t generation,
                                          Point position) {
                              reported = PointerPosition{session, generation, position};
                          }});
    PeerSession target(
        std::move(accepted.value()),
        PeerSessionConfig{id(72), AuthenticatedPeer{id(71)}, nullptr, &arbiter, &target_backend});

    ASSERT_TRUE(source.start({}).ok());
    ASSERT_TRUE(target.start({}).ok());
    ASSERT_TRUE(source.pump({}).ok());
    ASSERT_TRUE(target.pump({}).ok());

    auto actions = router.on_crossing(
        Crossing{id(82), Edge::kLeft, 0.5, {20, 10}, 0.5, true}, {});
    ASSERT_TRUE(source.send_actions(actions).ok());
    ASSERT_TRUE(target.pump({}).ok());
    ASSERT_TRUE(source.pump({}).ok());
    ASSERT_TRUE(target.pump({}).ok());
    ASSERT_TRUE(source.pump({}).ok());
    ASSERT_TRUE(arbiter.has_active_lease());

    target_backend.set_pointer_position(Point{123.0, 456.0});
    ASSERT_TRUE(target.tick(IClock::time_point{} + m0::kPointerPositionInterval).ok());
    ASSERT_TRUE(source.pump(IClock::time_point{} + m0::kPointerPositionInterval).ok());
    ASSERT_TRUE(reported.has_value());
    EXPECT_EQ(reported->session, arbiter.lease()->session);
    EXPECT_EQ(reported->generation, arbiter.lease()->generation);
    EXPECT_EQ(reported->position, (Point{123.0, 456.0}));
}

TEST(PeerSession, ActivationFailsWhenPointerWarpCannotBeInjected) {
    auto listener_result = Listener::bind(Endpoint{"127.0.0.1", 0});
    ASSERT_TRUE(listener_result.ok());
    auto listener = std::move(listener_result.value());
    auto client_result = Connection::connect(Endpoint{"127.0.0.1", listener.local_endpoint().port});
    ASSERT_TRUE(client_result.ok());
    auto accepted = listener.accept(2s);
    ASSERT_TRUE(accepted.ok());

    InputRouter router{id(5), AuthenticatedPeer{id(7)}, id(8), id(9)};
    InputArbiter arbiter;
    FakeInputBackend source_backend;
    FakeInputBackend target_backend;
    ASSERT_TRUE(source_backend.start({}).ok());
    ASSERT_TRUE(target_backend.start({}).ok());
    target_backend.set_injection_failure(true);

    PeerSession source(
        std::move(client_result.value()),
        PeerSessionConfig{id(6), AuthenticatedPeer{id(7)}, &router, nullptr, &source_backend});
    PeerSession target(
        std::move(accepted.value()),
        PeerSessionConfig{id(7), AuthenticatedPeer{id(6)}, nullptr, &arbiter, &target_backend});

    ASSERT_TRUE(source.start({}).ok());
    ASSERT_TRUE(target.start({}).ok());
    ASSERT_TRUE(source.pump({}).ok());
    ASSERT_TRUE(target.pump({}).ok());

    ASSERT_TRUE(source.send_actions(
                    router.on_crossing(Crossing{id(10), Edge::kLeft, 0.5, {20, 10}}, {}))
                    .ok());
    ASSERT_TRUE(target.pump({}).ok());
    ASSERT_TRUE(source.pump({}).ok());
    ASSERT_TRUE(target.pump({}).ok());
    ASSERT_TRUE(source.pump({}).ok());

    EXPECT_EQ(router.state(), RouterState::kRecovering);
    EXPECT_FALSE(arbiter.has_lease());
    EXPECT_FALSE(source_backend.status().suppression);
    EXPECT_TRUE(target_backend.injected_events().empty());
}

TEST(PeerSession, DisconnectCleansSourceAndTargetState) {
    auto listener_result = Listener::bind(Endpoint{"127.0.0.1", 0});
    ASSERT_TRUE(listener_result.ok());
    auto listener = std::move(listener_result.value());
    auto client_result = Connection::connect(Endpoint{"127.0.0.1", listener.local_endpoint().port});
    ASSERT_TRUE(client_result.ok());
    auto accepted = listener.accept(2s);
    ASSERT_TRUE(accepted.ok());

    InputRouter router{id(11), AuthenticatedPeer{id(13)}, id(20), id(14)};
    InputArbiter arbiter;
    FakeInputBackend source_backend;
    FakeInputBackend target_backend;
    ASSERT_TRUE(source_backend.start({}).ok());
    ASSERT_TRUE(target_backend.start({}).ok());
    PeerSession source(
        std::move(client_result.value()),
        PeerSessionConfig{id(12), AuthenticatedPeer{id(13)}, &router, nullptr, &source_backend});
    PeerSession target(
        std::move(accepted.value()),
        PeerSessionConfig{id(13), AuthenticatedPeer{id(12)}, nullptr, &arbiter, &target_backend});
    ASSERT_TRUE(source.start({}).ok());
    ASSERT_TRUE(target.start({}).ok());
    ASSERT_TRUE(source.pump({}).ok());
    ASSERT_TRUE(target.pump({}).ok());
    auto actions = router.on_crossing(Crossing{id(15), Edge::kLeft, 0.5, {20, 10}}, {});
    ASSERT_TRUE(source.send_actions(actions).ok());
    ASSERT_TRUE(target.pump({}).ok());
    ASSERT_TRUE(source.pump({}).ok());
    ASSERT_TRUE(target.pump({}).ok());
    ASSERT_TRUE(source.pump({}).ok());
    ASSERT_EQ(router.state(), RouterState::kRemote);

    auto down = router.on_local_event(
        InputEvent{{}, {}, 0, 0, Key{65, true}, InputOrigin::kSynthetic}, {}, {});
    ASSERT_EQ(down.size(), 1u);
    ASSERT_TRUE(source.send_actions(down).ok());
    ASSERT_TRUE(target.pump({}).ok());
    ASSERT_TRUE(arbiter.has_lease());
    ASSERT_FALSE(arbiter.ledger_empty());

    ASSERT_TRUE(source.disconnect({}).ok());
    EXPECT_EQ(router.state(), RouterState::kRecovering);
    EXPECT_FALSE(source_backend.status().suppression);
    ASSERT_FALSE(target.pump({}).ok());
    EXPECT_FALSE(arbiter.has_lease());
    EXPECT_TRUE(arbiter.ledger_empty());
    EXPECT_EQ(target_backend.held_count(), 0u);
}

TEST(PeerSession, LivenessTimeoutUsesDisconnectRecovery) {
    auto listener_result = Listener::bind(Endpoint{"127.0.0.1", 0});
    ASSERT_TRUE(listener_result.ok());
    auto listener = std::move(listener_result.value());
    auto client_result = Connection::connect(Endpoint{"127.0.0.1", listener.local_endpoint().port});
    ASSERT_TRUE(client_result.ok());
    auto accepted = listener.accept(2s);
    ASSERT_TRUE(accepted.ok());

    InputRouter router{id(21), AuthenticatedPeer{id(23)}, id(30), id(24)};
    FakeInputBackend source_backend;
    ASSERT_TRUE(source_backend.start({}).ok());
    PeerSession source(
        std::move(client_result.value()),
        PeerSessionConfig{id(22), AuthenticatedPeer{id(23)}, &router, nullptr, &source_backend});
    PeerSession target(
        std::move(accepted.value()),
        PeerSessionConfig{id(23), AuthenticatedPeer{id(22)}, nullptr, nullptr, nullptr});
    ASSERT_TRUE(source.start({}).ok());
    ASSERT_TRUE(target.start({}).ok());
    ASSERT_TRUE(source.pump({}).ok());
    ASSERT_TRUE(target.pump({}).ok());
    EXPECT_FALSE(source.tick(IClock::time_point{} + m0::kConnectionTimeout).ok());
    EXPECT_EQ(router.state(), RouterState::kRecovering);
}

TEST(PeerSession, ReconnectUsesNewGenerationAndRejectsOldInput) {
    auto listener_result = Listener::bind(Endpoint{"127.0.0.1", 0});
    ASSERT_TRUE(listener_result.ok());
    auto listener = std::move(listener_result.value());
    auto client_result = Connection::connect(Endpoint{"127.0.0.1", listener.local_endpoint().port});
    ASSERT_TRUE(client_result.ok());
    auto accepted = listener.accept(2s);
    ASSERT_TRUE(accepted.ok());

    InputRouter router{id(31), AuthenticatedPeer{id(33)}, id(40), id(34)};
    InputArbiter arbiter;
    FakeInputBackend source_backend;
    FakeInputBackend target_backend;
    ASSERT_TRUE(source_backend.start({}).ok());
    ASSERT_TRUE(target_backend.start({}).ok());
    PeerSession source(
        std::move(client_result.value()),
        PeerSessionConfig{id(32), AuthenticatedPeer{id(33)}, &router, nullptr, &source_backend});
    PeerSession target(
        std::move(accepted.value()),
        PeerSessionConfig{id(33), AuthenticatedPeer{id(32)}, nullptr, &arbiter, &target_backend});
    ASSERT_TRUE(source.start({}).ok());
    ASSERT_TRUE(target.start({}).ok());
    ASSERT_TRUE(source.pump({}).ok());
    ASSERT_TRUE(target.pump({}).ok());
    ASSERT_TRUE(
        source.send_actions(router.on_crossing(Crossing{id(35), Edge::kLeft, 0.5, {20, 10}}, {}))
            .ok());
    ASSERT_TRUE(target.pump({}).ok());
    ASSERT_TRUE(source.pump({}).ok());
    ASSERT_TRUE(target.pump({}).ok());
    ASSERT_TRUE(source.pump({}).ok());
    ASSERT_EQ(router.state(), RouterState::kRemote);
    const auto old_session = router.lease()->session;
    const auto old_generation = router.lease()->generation;

    ASSERT_TRUE(source.disconnect({}).ok());
    EXPECT_FALSE(target.pump({}).ok());
    EXPECT_FALSE(arbiter.has_lease());
    ASSERT_EQ(router.state(), RouterState::kRecovering);
    router.on_tick({});
    ASSERT_EQ(router.state(), RouterState::kLocal);

    auto listener2_result = Listener::bind(Endpoint{"127.0.0.1", 0});
    ASSERT_TRUE(listener2_result.ok());
    auto listener2 = std::move(listener2_result.value());
    auto client2_result =
        Connection::connect(Endpoint{"127.0.0.1", listener2.local_endpoint().port});
    ASSERT_TRUE(client2_result.ok());
    auto accepted2 = listener2.accept(2s);
    ASSERT_TRUE(accepted2.ok());
    PeerSession source2(
        std::move(client2_result.value()),
        PeerSessionConfig{id(32), AuthenticatedPeer{id(33)}, &router, nullptr, &source_backend});
    PeerSession target2(
        std::move(accepted2.value()),
        PeerSessionConfig{id(33), AuthenticatedPeer{id(32)}, nullptr, &arbiter, &target_backend});
    ASSERT_TRUE(source2.start({}).ok());
    ASSERT_TRUE(target2.start({}).ok());
    ASSERT_TRUE(source2.pump({}).ok());
    ASSERT_TRUE(target2.pump({}).ok());
    ASSERT_TRUE(
        source2.send_actions(router.on_crossing(Crossing{id(35), Edge::kLeft, 0.5, {20, 10}}, {}))
            .ok());
    ASSERT_TRUE(target2.pump({}).ok());
    ASSERT_TRUE(source2.pump({}).ok());
    ASSERT_TRUE(target2.pump({}).ok());
    ASSERT_TRUE(source2.pump({}).ok());
    ASSERT_EQ(router.state(), RouterState::kRemote);
    ASSERT_GT(router.lease()->generation, old_generation);
    const auto injected_before_stale = target_backend.injected_events().size();

    RouterAction stale;
    stale.kind = RouterActionKind::kSendInput;
    stale.event = InputEvent{router.source(), old_session,
                             old_generation,  1,
                             Key{66, true},   InputOrigin::kRemoteInjected};
    ASSERT_TRUE(source2.send_actions({stale}).ok());
    ASSERT_TRUE(target2.pump({}).ok());
    EXPECT_TRUE(arbiter.has_active_lease());
    EXPECT_EQ(target_backend.injected_events().size(), injected_before_stale);
}

} // namespace
