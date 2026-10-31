#pragma once

#include "lanlink/common/clock.hpp"
#include "lanlink/common/ids.hpp"
#include "lanlink/common/result.hpp"
#include "lanlink/input/arbiter.hpp"
#include "lanlink/input/events.hpp"
#include "lanlink/topology/topology.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace lanlink {

enum class RouterState {
    kLocal,
    kAcquiring,
    kRemote,
    kHandoff,
    kRecovering,
    Local = kLocal,
    Acquiring = kAcquiring,
    Remote = kRemote,
    Handoff = kHandoff,
    Recovering = kRecovering,
    LOCAL = kLocal,
    ACQUIRING = kAcquiring,
    REMOTE = kRemote,
    HANDOFF = kHandoff,
    RECOVERING = kRecovering,
};
using SourceState = RouterState;

enum class RouterActionKind {
    kAcquire,
    kArmLocalSuppression,
    kActivate,
    kRenew,
    kSendInput,
    kRelease,
    kRestoreLocal,
    kReleaseInjectedState,
    kDrop,
    Acquire = kAcquire,
    ArmLocalSuppression = kArmLocalSuppression,
    Activate = kActivate,
    Renew = kRenew,
    SendInput = kSendInput,
    Release = kRelease,
    RestoreLocal = kRestoreLocal,
    ReleaseInjectedState = kReleaseInjectedState,
    Drop = kDrop,
    ACQUIRE = kAcquire,
    ARM_LOCAL_SUPPRESSION = kArmLocalSuppression,
    ACTIVATE = kActivate,
    RENEW = kRenew,
    SEND_INPUT = kSendInput,
    RELEASE = kRelease,
    RESTORE_LOCAL = kRestoreLocal,
    RELEASE_INJECTED_STATE = kReleaseInjectedState,
    DROP = kDrop,
};
using ActionKind = RouterActionKind;

struct RouterAction {
    RouterActionKind kind{RouterActionKind::kDrop};
    SourceId source{};
    DisplayId target_display{};
    RequestId request{};
    SessionId session{};
    std::uint64_t generation{};
    std::uint64_t seq{};
    Point entry{};
    InputEvent event{};
    bool success{true};
    std::string error;

    static RouterAction acquire(const SourceId& source, const DisplayId& target,
                                const RequestId& request) {
        RouterAction a; a.kind = RouterActionKind::kAcquire; a.source = source;
        a.target_display = target; a.request = request; return a;
    }
};

class InputRouter {
  public:
    using TimePoint = IClock::time_point;
    using State = RouterState;
    using Action = RouterAction;

    explicit InputRouter(SourceId source = {});
    InputRouter(SourceId source, AuthenticatedPeer peer, DisplayId local_display,
                RequestId request = {}, std::size_t queue_capacity = 256);

    std::vector<RouterAction> on_local_event(InputEvent event, Point point, TimePoint now);
    std::vector<RouterAction> on_crossing(Crossing crossing, TimePoint now);
    std::vector<RouterAction> on_grant(AcquireResult result, TimePoint now);
    std::vector<RouterAction> on_activation_ack(const Result<void>& result, TimePoint now);
    std::vector<RouterAction> on_activation_ack(bool success, TimePoint now);
    std::vector<RouterAction> on_revoke(RevokeReason reason, TimePoint now);
    std::vector<RouterAction> on_disconnect(TimePoint now);
    std::vector<RouterAction> on_tick(TimePoint now);

    RouterState state() const noexcept { return state_; }
    RouterState current_state() const noexcept { return state_; }
    const SourceId& source() const noexcept { return source_; }
    const std::optional<ControlLease>& lease() const noexcept { return lease_; }
    const std::optional<Crossing>& pending_crossing() const noexcept { return pending_crossing_; }
    std::uint64_t next_sequence() const noexcept { return next_seq_; }
    bool suppression_armed() const noexcept { return suppression_armed_; }
    bool connected() const noexcept { return connected_; }

    void set_peer(AuthenticatedPeer peer) { peer_ = peer; }
    void set_local_display(DisplayId display) { local_display_ = display; }
    void set_request(RequestId request) { request_ = request; }
    void set_queue_capacity(std::size_t capacity) { queue_capacity_ = capacity; }

  private:
    std::vector<RouterAction> begin_acquire(TimePoint now);
    std::vector<RouterAction> recover(TimePoint now);
    std::vector<RouterAction> make_input(InputEvent event);
    static bool is_nonzero_physical(const Payload& payload);
    static bool is_button(const Payload& payload);

    RouterState state_{RouterState::kLocal};
    SourceId source_{};
    AuthenticatedPeer peer_{};
    DisplayId local_display_{};
    RequestId request_{};
    std::size_t queue_capacity_{256};
    bool connected_{true};
    bool suppression_armed_{false};
    std::optional<ControlLease> lease_;
    std::optional<Crossing> pending_crossing_;
    std::set<std::uint32_t> held_buttons_;
    std::uint64_t next_seq_{1};
    TimePoint next_renew_{};
    TimePoint recovering_until_{};
};

} // namespace lanlink
