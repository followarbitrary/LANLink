#include "lanlink/common/config.hpp"
#include "lanlink/platform/fake_input_backend.hpp"
#include "lanlink/transport/manager.hpp"

#include <chrono>
#include <gtest/gtest.h>

namespace {
using namespace lanlink;
using namespace lanlink::platform;
using namespace lanlink::transport;
using namespace std::chrono_literals;

Uuid128 id(std::uint64_t value) { return {value, value + 1000}; }

OutgoingPeer outgoing(PeerSessionConfig session, Endpoint endpoint = {"127.0.0.1", 0}) {
    OutgoingPeer peer;
    peer.endpoint = std::move(endpoint);
    peer.session = session;
    peer.reconnect.initial_delay = 100ms;
    peer.reconnect.max_delay = 400ms;
    return peer;
}

TEST(ConnectionManager, DrivesOutgoingHelloAndIncomingControlSession) {
    auto listener_result = Listener::bind(Endpoint{"127.0.0.1", 0});
    ASSERT_TRUE(listener_result.ok()) << listener_result.error();
    auto listener = std::move(listener_result.value());

    InputRouter router{id(1), AuthenticatedPeer{id(3)}, id(10), id(4)};
    InputArbiter arbiter;
    FakeInputBackend source_backend;
    FakeInputBackend target_backend;
    ASSERT_TRUE(source_backend.start({}).ok());
    ASSERT_TRUE(target_backend.start({}).ok());

    ConnectionManager manager;
    const auto handle_result = manager.add_outgoing(
        outgoing(PeerSessionConfig{id(2), AuthenticatedPeer{id(3)}, &router, nullptr,
                                   &source_backend},
                Endpoint{"127.0.0.1", listener.local_endpoint().port}));
    ASSERT_TRUE(handle_result.ok()) << handle_result.error();
    const auto handle = handle_result.value();

    ASSERT_TRUE(manager.poll({}, 10ms).ok());
    auto accepted = listener.accept(2s);
    ASSERT_TRUE(accepted.ok()) << accepted.error();
    PeerSession target(
        std::move(accepted.value()),
        PeerSessionConfig{id(3), AuthenticatedPeer{id(2)}, nullptr, &arbiter, &target_backend});
    ASSERT_TRUE(target.start({}).ok());
    ASSERT_TRUE(target.pump({}).ok());
    ASSERT_TRUE(manager.poll({}, 10ms).ok());

    auto actions = router.on_crossing(Crossing{id(5), Edge::kLeft, 0.5, {20, 10}}, {});
    ASSERT_TRUE(manager.send_actions(handle, actions).ok());
    ASSERT_TRUE(target.pump({}).ok());
    ASSERT_TRUE(manager.poll({}, 10ms).ok());
    ASSERT_TRUE(target.pump({}).ok());
    ASSERT_TRUE(manager.poll({}, 10ms).ok());
    EXPECT_EQ(router.state(), RouterState::kRemote);
    EXPECT_TRUE(arbiter.has_active_lease());
    EXPECT_TRUE(source_backend.status().suppression);
}

TEST(ConnectionManager, ReconnectWaitsUntilDue) {
    std::size_t attempts = 0;
    ConnectionManager manager([&](const Endpoint&, const ConnectionOptions&) {
        ++attempts;
        return Result<Connection>::failure("dial failed");
    });
    const auto handle_result = manager.add_outgoing(
        outgoing(PeerSessionConfig{id(10), AuthenticatedPeer{id(11)}, nullptr, nullptr, nullptr}));
    ASSERT_TRUE(handle_result.ok());
    const auto handle = handle_result.value();
    const auto now = IClock::time_point{};

    EXPECT_FALSE(manager.poll(now).ok());
    EXPECT_EQ(attempts, 1u);
    ASSERT_EQ(manager.state(handle), PeerState::kBackoff);
    EXPECT_TRUE(manager.poll(now + 99ms).ok());
    EXPECT_EQ(attempts, 1u);
    EXPECT_FALSE(manager.poll(now + 100ms).ok());
    EXPECT_EQ(attempts, 2u);
}

TEST(ConnectionManager, PollContinuesAfterOnePeerFailure) {
    auto listener_result = Listener::bind(Endpoint{"127.0.0.1", 0});
    ASSERT_TRUE(listener_result.ok());
    auto listener = std::move(listener_result.value());
    std::size_t attempts = 0;
    ConnectionManager manager([&](const Endpoint& endpoint, const ConnectionOptions& options) {
        ++attempts;
        if (attempts == 1)
            return Result<Connection>::failure("first dial failed");
        return Connection::connect(endpoint, options);
    });
    const auto first = manager.add_outgoing(
        outgoing(PeerSessionConfig{id(20), AuthenticatedPeer{id(21)}, nullptr, nullptr, nullptr}));
    const auto second = manager.add_outgoing(
        outgoing(PeerSessionConfig{id(22), AuthenticatedPeer{id(23)}, nullptr, nullptr, nullptr},
                 Endpoint{"127.0.0.1", listener.local_endpoint().port}));
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    EXPECT_FALSE(manager.poll({}, 50ms).ok());
    EXPECT_EQ(attempts, 2u);
    EXPECT_EQ(manager.state(first.value()), PeerState::kBackoff);
    EXPECT_EQ(manager.state(second.value()), PeerState::kConnected);
    auto accepted = listener.accept(2s);
    EXPECT_TRUE(accepted.ok()) << accepted.error();
}

TEST(ConnectionManager, TickTurnsLivenessTimeoutIntoBackoff) {
    auto listener_result = Listener::bind(Endpoint{"127.0.0.1", 0});
    ASSERT_TRUE(listener_result.ok());
    auto listener = std::move(listener_result.value());

    InputRouter router{id(45), AuthenticatedPeer{id(47)}, id(55), id(49)};
    FakeInputBackend backend;
    ASSERT_TRUE(backend.start({}).ok());
    ConnectionManager manager;
    const auto handle_result = manager.add_outgoing(
        outgoing(PeerSessionConfig{id(46), AuthenticatedPeer{id(47)}, &router, nullptr, &backend},
                 Endpoint{"127.0.0.1", listener.local_endpoint().port}));
    ASSERT_TRUE(handle_result.ok());
    const auto now = IClock::time_point{};

    ASSERT_TRUE(manager.poll(now, 10ms).ok());
    auto accepted = listener.accept(2s);
    ASSERT_TRUE(accepted.ok());
    PeerSession target(std::move(accepted.value()),
                       PeerSessionConfig{id(47), AuthenticatedPeer{id(46)}, nullptr, nullptr,
                                         nullptr});
    ASSERT_TRUE(target.start(now).ok());
    ASSERT_TRUE(target.pump(now).ok());
    ASSERT_TRUE(manager.poll(now, 10ms).ok());

    EXPECT_FALSE(manager.tick(now + m0::kConnectionTimeout).ok());
    EXPECT_EQ(manager.state(handle_result.value()), PeerState::kBackoff);
    EXPECT_EQ(router.state(), RouterState::kRecovering);
}

TEST(ConnectionManager, ReconnectKeepsPeerHandleAndRestoresRouter) {
    auto listener_result = Listener::bind(Endpoint{"127.0.0.1", 0});
    ASSERT_TRUE(listener_result.ok());
    auto listener = std::move(listener_result.value());

    InputRouter router{id(50), AuthenticatedPeer{id(52)}, id(60), id(54)};
    FakeInputBackend backend;
    ASSERT_TRUE(backend.start({}).ok());
    ConnectionManager manager;
    const auto handle_result = manager.add_outgoing(
        outgoing(PeerSessionConfig{id(51), AuthenticatedPeer{id(52)}, &router, nullptr, &backend},
                 Endpoint{"127.0.0.1", listener.local_endpoint().port}));
    ASSERT_TRUE(handle_result.ok());
    const auto handle = handle_result.value();
    const auto now = IClock::time_point{};

    ASSERT_TRUE(manager.poll(now, 10ms).ok());
    auto accepted1 = listener.accept(2s);
    ASSERT_TRUE(accepted1.ok());
    PeerSession target1(std::move(accepted1.value()),
                        PeerSessionConfig{id(52), AuthenticatedPeer{id(51)}, nullptr, nullptr,
                                          nullptr});
    ASSERT_TRUE(target1.start(now).ok());
    ASSERT_TRUE(target1.pump(now).ok());
    ASSERT_TRUE(manager.poll(now, 10ms).ok());

    ASSERT_TRUE(target1.disconnect(now).ok());
    EXPECT_FALSE(manager.poll(now, 50ms).ok());
    EXPECT_EQ(manager.state(handle), PeerState::kBackoff);
    EXPECT_EQ(router.state(), RouterState::kRecovering);
    EXPECT_FALSE(router.connected());

    const auto reconnect_at = now + 100ms;
    ASSERT_TRUE(manager.poll(reconnect_at, 10ms).ok());
    auto accepted2 = listener.accept(2s);
    ASSERT_TRUE(accepted2.ok());
    PeerSession target2(std::move(accepted2.value()),
                        PeerSessionConfig{id(52), AuthenticatedPeer{id(51)}, nullptr, nullptr,
                                          nullptr});
    ASSERT_TRUE(target2.start(reconnect_at).ok());
    ASSERT_TRUE(target2.pump(reconnect_at).ok());
    ASSERT_TRUE(manager.poll(reconnect_at, 10ms).ok());
    EXPECT_EQ(manager.state(handle), PeerState::kConnected);
    EXPECT_TRUE(router.connected());
    EXPECT_EQ(router.state(), RouterState::kLocal);
}

TEST(ConnectionManager, RejectsInvalidAndNotConnectedHandles) {
    ConnectionManager manager([](const Endpoint&, const ConnectionOptions&) {
        return Result<Connection>::failure("dial failed");
    });
    const auto handle_result = manager.add_outgoing(
        outgoing(PeerSessionConfig{id(30), AuthenticatedPeer{id(31)}, nullptr, nullptr, nullptr}));
    ASSERT_TRUE(handle_result.ok());
    const auto handle = handle_result.value();
    RouterAction action;
    action.kind = RouterActionKind::kDrop;
    EXPECT_FALSE(manager.send_actions(9999, {action}).ok());
    EXPECT_FALSE(manager.send_actions(handle, {action}).ok());
    EXPECT_FALSE(manager.remove(9999, {}).ok());
    EXPECT_TRUE(manager.remove(handle, {}).ok());
    EXPECT_TRUE(manager.remove(handle, {}).ok());
    EXPECT_EQ(manager.state(handle), PeerState::kClosed);
    const auto next = manager.add_outgoing(
        outgoing(PeerSessionConfig{id(32), AuthenticatedPeer{id(33)}, nullptr, nullptr, nullptr}));
    ASSERT_TRUE(next.ok());
    EXPECT_GT(next.value(), handle);
}

TEST(ConnectionManager, IncomingDisconnectAndRemoveAreIdempotent) {
    auto listener_result = Listener::bind(Endpoint{"127.0.0.1", 0});
    ASSERT_TRUE(listener_result.ok());
    auto listener = std::move(listener_result.value());
    auto client_result = Connection::connect(Endpoint{"127.0.0.1", listener.local_endpoint().port});
    ASSERT_TRUE(client_result.ok());
    auto accepted = listener.accept(2s);
    ASSERT_TRUE(accepted.ok());

    InputRouter router{id(40), AuthenticatedPeer{id(42)}, id(50), id(44)};
    FakeInputBackend backend;
    ASSERT_TRUE(backend.start({}).ok());
    ConnectionManager manager;
    const auto handle_result = manager.add_incoming(
        std::move(accepted.value()),
        PeerSessionConfig{id(41), AuthenticatedPeer{id(42)}, &router, nullptr, &backend}, {});
    ASSERT_TRUE(handle_result.ok());
    auto client = std::move(client_result.value());
    ASSERT_TRUE(client.send(protocol::Hello{id(42), protocol::kProtocolVersion}).ok());
    ASSERT_TRUE(manager.poll({}, 10ms).ok());
    ASSERT_TRUE(client.close().ok());
    EXPECT_FALSE(manager.poll({}, 50ms).ok());
    EXPECT_EQ(manager.state(handle_result.value()), PeerState::kClosed);
    EXPECT_TRUE(manager.remove(handle_result.value(), {}).ok());
    EXPECT_TRUE(manager.remove(handle_result.value(), {}).ok());
    EXPECT_FALSE(backend.status().suppression);
}

} // namespace
