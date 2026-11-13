#include "lanlink/common/clock.hpp"
#include "lanlink/common/config.hpp"
#include "lanlink/input/arbiter.hpp"

#include <chrono>
#include <gtest/gtest.h>
#include <set>

using namespace std::chrono_literals;
using namespace lanlink;

namespace {
Uuid128 id(std::uint64_t n) { return Uuid128{n, n + 100}; }
InputEvent key_event(const SourceId& source, const SessionId& session, std::uint64_t generation,
                     std::uint64_t seq, std::uint32_t code, bool pressed,
                     InputOrigin origin = InputOrigin::RemoteInjected) {
    return InputEvent{source, session, generation, seq, Key{code, pressed}, origin};
}
InputEvent button_event(const SourceId& source, const SessionId& session, std::uint64_t generation,
                        std::uint64_t seq, std::uint32_t button, bool pressed,
                        InputOrigin origin = InputOrigin::RemoteInjected) {
    return InputEvent{source, session, generation, seq, MouseButton{button, pressed}, origin};
}
InputEvent mouse_delta_event(const SourceId& source, const SessionId& session,
                             std::uint64_t generation, std::uint64_t seq, double dx, double dy,
                             InputOrigin origin = InputOrigin::RemoteInjected) {
    return InputEvent{source, session, generation, seq, MouseDelta{dx, dy}, origin};
}
} // namespace

TEST(InputArbiter, GrantsOneLeaseAndAdvancesGenerationAfterRelease) {
    InputArbiter arbiter;
    AuthenticatedPeer peer{id(1)};
    const SourceId source = id(2);
    const DisplayId display = id(3);
    const RequestId request1 = id(4);
    const RequestId request2 = id(5);

    auto first = arbiter.acquire(peer, source, display, request1, IClock::time_point{});
    ASSERT_TRUE(first.granted());
    EXPECT_EQ(first.lease.generation, 1u);
    EXPECT_EQ(first.lease.expires_at, IClock::time_point{} + m0::kLeaseDuration);

    auto denied = arbiter.acquire(peer, source, display, request2, IClock::time_point{});
    EXPECT_FALSE(denied.granted());
    EXPECT_FALSE(
        arbiter.activate(first.lease.session, first.lease.generation + 1, IClock::time_point{}));
    ASSERT_TRUE(
        arbiter.activate(first.lease.session, first.lease.generation, IClock::time_point{}));
    arbiter.release(first.lease.session, IClock::time_point{} + 1ms);

    auto second = arbiter.acquire(peer, source, display, request2, IClock::time_point{} + 2ms);
    ASSERT_TRUE(second.granted());
    EXPECT_EQ(second.lease.generation, 2u);
}

TEST(InputArbiter, ExpiryIsValidAt2999AndRevokedAt3000) {
    InputArbiter arbiter;
    AuthenticatedPeer peer{id(1)};
    const SourceId source = id(2);
    const DisplayId display = id(3);
    const RequestId request = id(4);
    auto grant = arbiter.acquire(peer, source, display, request, IClock::time_point{});
    ASSERT_TRUE(grant.granted());
    ASSERT_TRUE(
        arbiter.activate(grant.lease.session, grant.lease.generation, IClock::time_point{}));

    EXPECT_TRUE(arbiter.validate(peer, key_event(source, grant.lease.session, 1, 1, 42, true),
                                 IClock::time_point{} + 2999ms));
    EXPECT_FALSE(arbiter.validate(peer, key_event(source, grant.lease.session, 1, 2, 42, false),
                                  IClock::time_point{} + 3000ms));
    EXPECT_FALSE(arbiter.has_lease());
}

TEST(InputArbiter, RenewsOnlyMatchingActiveUnexpiredLease) {
    InputArbiter arbiter;
    AuthenticatedPeer peer{id(1)};
    auto grant = arbiter.acquire(peer, id(2), id(3), id(4), IClock::time_point{});
    ASSERT_TRUE(grant.granted());
    ASSERT_TRUE(
        arbiter.activate(grant.lease.session, grant.lease.generation, IClock::time_point{}));
    ASSERT_TRUE(arbiter.renew(peer, grant.lease.session, grant.lease.generation,
                              IClock::time_point{} + 1000ms));
    ASSERT_TRUE(arbiter.lease().has_value());
    EXPECT_EQ(arbiter.lease()->expires_at, IClock::time_point{} + 4000ms);
    EXPECT_FALSE(arbiter.renew(AuthenticatedPeer{id(99)}, grant.lease.session,
                               grant.lease.generation, IClock::time_point{} + 1001ms));
    EXPECT_FALSE(arbiter.renew(peer, grant.lease.session, grant.lease.generation,
                               IClock::time_point{} + 4000ms));
}

TEST(InputArbiter, DuplicateActivationDoesNotResetSequence) {
    InputArbiter arbiter;
    AuthenticatedPeer peer{id(1)};
    const SourceId source = id(2);
    auto grant = arbiter.acquire(peer, source, id(3), id(4), IClock::time_point{});
    ASSERT_TRUE(grant.granted());
    ASSERT_TRUE(
        arbiter.activate(grant.lease.session, grant.lease.generation, IClock::time_point{}));
    ASSERT_TRUE(arbiter.validate(
        peer, key_event(source, grant.lease.session, grant.lease.generation, 1, 42, true),
        IClock::time_point{}));

    // A delayed duplicate Activate for the live session must be harmless.
    ASSERT_TRUE(
        arbiter.activate(grant.lease.session, grant.lease.generation, IClock::time_point{}));
    EXPECT_TRUE(arbiter.validate(
        peer, key_event(source, grant.lease.session, grant.lease.generation, 2, 42, false),
        IClock::time_point{}));
    EXPECT_TRUE(arbiter.ledger_empty());
}

TEST(InputArbiter, ValidatesIdentityAndStrictSequencesAndCleansLedgerOnGap) {
    InputArbiter arbiter;
    AuthenticatedPeer peer{id(1)};
    const SourceId source = id(2);
    auto grant = arbiter.acquire(peer, source, id(3), id(4), IClock::time_point{});
    ASSERT_TRUE(grant.granted());
    ASSERT_TRUE(
        arbiter.activate(grant.lease.session, grant.lease.generation, IClock::time_point{}));

    EXPECT_FALSE(arbiter.validate(peer, key_event(source, grant.lease.session, 1, 2, 10, true),
                                  IClock::time_point{}));
    EXPECT_FALSE(arbiter.has_lease());

    grant = arbiter.acquire(peer, source, id(3), id(4), IClock::time_point{} + 1ms);
    ASSERT_TRUE(grant.granted());
    ASSERT_TRUE(
        arbiter.activate(grant.lease.session, grant.lease.generation, IClock::time_point{} + 1ms));

    EXPECT_TRUE(arbiter.validate(
        peer, key_event(source, grant.lease.session, grant.lease.generation, 1, 10, true),
        IClock::time_point{}));
    EXPECT_TRUE(arbiter.validate(
        peer, button_event(source, grant.lease.session, grant.lease.generation, 2, 1, true),
        IClock::time_point{}));
    EXPECT_FALSE(arbiter.validate(
        peer, key_event(source, grant.lease.session, grant.lease.generation, 2, 10, false),
        IClock::time_point{}));
    EXPECT_FALSE(arbiter.validate(
        peer, key_event(source, grant.lease.session, grant.lease.generation, 4, 10, false),
        IClock::time_point{}));
    EXPECT_TRUE(arbiter.pressed_keys().empty());
    EXPECT_TRUE(arbiter.pressed_buttons().empty());
    EXPECT_FALSE(arbiter.has_lease());
}

TEST(InputArbiter, LocalPhysicalNonZeroInputPreemptsRemoteLease) {
    InputArbiter arbiter;
    AuthenticatedPeer peer{id(1)};
    const SourceId source = id(2);
    auto grant = arbiter.acquire(peer, source, id(3), id(4), IClock::time_point{});
    ASSERT_TRUE(grant.granted());
    ASSERT_TRUE(
        arbiter.activate(grant.lease.session, grant.lease.generation, IClock::time_point{}));
    ASSERT_TRUE(arbiter.validate(peer, button_event(source, grant.lease.session, 1, 1, 1, true),
                                 IClock::time_point{}));

    arbiter.on_local_input(key_event({}, {}, 0, 0, 0, false, InputOrigin::Physical),
                           IClock::time_point{} + 1ms);
    EXPECT_TRUE(arbiter.has_lease());
    arbiter.on_local_input(button_event({}, {}, 0, 0, 1, true, InputOrigin::Physical),
                           IClock::time_point{} + 2ms);
    EXPECT_FALSE(arbiter.has_lease());
    EXPECT_TRUE(arbiter.pressed_buttons().empty());
}

TEST(InputArbiter, LocalKeyboardDoesNotPreemptRemoteMouseButSameClassStillDoes) {
    InputArbiter arbiter;
    AuthenticatedPeer peer{id(1)};
    const SourceId source = id(2);
    auto grant = arbiter.acquire(peer, source, id(3), id(4), IClock::time_point{});
    ASSERT_TRUE(grant.granted());
    ASSERT_TRUE(
        arbiter.activate(grant.lease.session, grant.lease.generation, IClock::time_point{}));
    ASSERT_TRUE(arbiter.validate(
        peer, mouse_delta_event(source, grant.lease.session, grant.lease.generation, 1, 1.0, 0.0),
        IClock::time_point{}));

    arbiter.on_local_input(
        key_event({}, {}, 0, 0, 0x04, true, InputOrigin::kPhysical),
        IClock::time_point{} + 1ms);
    EXPECT_TRUE(arbiter.has_active_lease());

    arbiter.on_local_input(
        mouse_delta_event({}, {}, 0, 0, 1.0, 0.0, InputOrigin::kPhysical),
        IClock::time_point{} + 2ms);
    EXPECT_FALSE(arbiter.has_lease());
}

TEST(InputArbiter, LocalInputBeforeFirstRemoteEventDoesNotPreemptLease) {
    InputArbiter arbiter;
    AuthenticatedPeer peer{id(1)};
    auto grant = arbiter.acquire(peer, id(2), id(3), id(4), IClock::time_point{});
    ASSERT_TRUE(grant.granted());
    ASSERT_TRUE(
        arbiter.activate(grant.lease.session, grant.lease.generation, IClock::time_point{}));

    arbiter.on_local_input(
        key_event({}, {}, 0, 0, 0x04, true, InputOrigin::kPhysical),
        IClock::time_point{} + 1ms);
    EXPECT_TRUE(arbiter.has_active_lease());
}

TEST(InputArbiter, LocalPointerBeforeFirstRemoteEventPreemptsLease) {
    InputArbiter arbiter;
    AuthenticatedPeer peer{id(1)};
    auto grant = arbiter.acquire(peer, id(2), id(3), id(4), IClock::time_point{});
    ASSERT_TRUE(grant.granted());
    ASSERT_TRUE(
        arbiter.activate(grant.lease.session, grant.lease.generation, IClock::time_point{}));

    arbiter.on_local_input(
        InputEvent{{}, {}, 0, 0, MouseDelta{1.0, 0.0}, InputOrigin::kPhysical},
        IClock::time_point{} + 1ms);
    EXPECT_FALSE(arbiter.has_lease());
}

TEST(InputArbiter, LocalMouseDoesNotPreemptRemoteKeyboardButSameClassStillDoes) {
    InputArbiter arbiter;
    AuthenticatedPeer peer{id(1)};
    const SourceId source = id(2);
    auto grant = arbiter.acquire(peer, source, id(3), id(4), IClock::time_point{});
    ASSERT_TRUE(grant.granted());
    ASSERT_TRUE(
        arbiter.activate(grant.lease.session, grant.lease.generation, IClock::time_point{}));
    ASSERT_TRUE(arbiter.validate(
        peer, key_event(source, grant.lease.session, grant.lease.generation, 1, 0x04, true),
        IClock::time_point{}));

    arbiter.on_local_input(
        mouse_delta_event({}, {}, 0, 0, 1.0, 0.0, InputOrigin::kPhysical),
        IClock::time_point{} + 1ms);
    EXPECT_TRUE(arbiter.has_active_lease());

    arbiter.on_local_input(
        key_event({}, {}, 0, 0, 0x04, true, InputOrigin::kPhysical),
        IClock::time_point{} + 2ms);
    EXPECT_FALSE(arbiter.has_lease());
}

TEST(InputArbiter, RemotePointerMoveCountsAsMouseInputForIsolation) {
    InputArbiter arbiter;
    AuthenticatedPeer peer{id(1)};
    const SourceId source = id(2);
    auto grant = arbiter.acquire(peer, source, id(3), id(4), IClock::time_point{});
    ASSERT_TRUE(grant.granted());
    ASSERT_TRUE(
        arbiter.activate(grant.lease.session, grant.lease.generation, IClock::time_point{}));
    InputEvent move{source, grant.lease.session, grant.lease.generation, 1,
                    PointerMove{10.0, 20.0}, InputOrigin::kRemoteInjected};
    ASSERT_TRUE(arbiter.validate(peer, move, IClock::time_point{}));

    arbiter.on_local_input(
        key_event({}, {}, 0, 0, 0x04, true, InputOrigin::kPhysical),
        IClock::time_point{} + 1ms);
    EXPECT_TRUE(arbiter.has_active_lease());

    arbiter.on_local_input(
        mouse_delta_event({}, {}, 0, 0, 1.0, 0.0, InputOrigin::kPhysical),
        IClock::time_point{} + 2ms);
    EXPECT_FALSE(arbiter.has_lease());
}

TEST(InputArbiter, RemoteInjectedAndSyntheticEventsDoNotPreempt) {
    InputArbiter arbiter;
    AuthenticatedPeer peer{id(1)};
    auto grant = arbiter.acquire(peer, id(2), id(3), id(4), IClock::time_point{});
    ASSERT_TRUE(grant.granted());
    ASSERT_TRUE(
        arbiter.activate(grant.lease.session, grant.lease.generation, IClock::time_point{}));
    arbiter.on_local_input(key_event({}, {}, 0, 0, 65, true, InputOrigin::RemoteInjected),
                           IClock::time_point{});
    arbiter.on_local_input(key_event({}, {}, 0, 0, 65, true, InputOrigin::Synthetic),
                           IClock::time_point{});
    EXPECT_TRUE(arbiter.has_lease());
}

TEST(InputArbiter, PointerSynchronizationDoesNotCountAsPhysicalPreemption) {
    InputArbiter arbiter;
    AuthenticatedPeer peer{id(1)};
    auto grant = arbiter.acquire(peer, id(2), id(3), id(4), IClock::time_point{});
    ASSERT_TRUE(grant.granted());
    ASSERT_TRUE(
        arbiter.activate(grant.lease.session, grant.lease.generation, IClock::time_point{}));
    InputEvent warp{id(2), grant.lease.session,       grant.lease.generation,
                    1,     PointerWarp{100.0, 100.0}, InputOrigin::Physical};
    arbiter.on_local_input(warp, IClock::time_point{});
    InputEvent move{id(2), grant.lease.session,       grant.lease.generation,
                    2,     PointerMove{101.0, 100.0}, InputOrigin::Physical};
    arbiter.on_local_input(move, IClock::time_point{});
    EXPECT_TRUE(arbiter.has_lease());
}
