#include "lanlink/input/router.hpp"

#include "lanlink/common/config.hpp"

#include <type_traits>

namespace lanlink {

InputRouter::InputRouter(SourceId source) : source_(source), request_(source) {}

InputRouter::InputRouter(SourceId source, AuthenticatedPeer peer, DisplayId local_display,
                         RequestId request, std::size_t queue_capacity)
    : source_(source), peer_(peer), local_display_(local_display),
      request_(request == RequestId{} ? source : request), queue_capacity_(queue_capacity) {}

bool InputRouter::is_button(const Payload& payload) {
    return std::holds_alternative<MouseButton>(payload);
}

bool InputRouter::is_nonzero_physical(const Payload& payload) {
    return std::visit(
        [](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, MouseDelta>) return !value.zero();
            if constexpr (std::is_same_v<T, MouseWheel>) return !value.zero();
            if constexpr (std::is_same_v<T, Key>) return value.code != 0;
            if constexpr (std::is_same_v<T, MouseButton>) return value.button != 0;
            return false; // PointerWarp is a synchronization hint.
        }, payload);
}

std::vector<RouterAction> InputRouter::begin_acquire(TimePoint) {
    std::vector<RouterAction> actions;
    if (!pending_crossing_.has_value()) return actions;
    state_ = RouterState::kAcquiring;
    auto action = RouterAction::acquire(source_, pending_crossing_->target, request_);
    action.entry = pending_crossing_->entry_point;
    actions.push_back(action);
    return actions;
}

std::vector<RouterAction> InputRouter::recover(TimePoint now) {
    std::vector<RouterAction> actions;
    if (lease_.has_value()) {
        RouterAction release;
        release.kind = RouterActionKind::kRelease;
        release.source = source_;
        release.session = lease_->session;
        release.generation = lease_->generation;
        actions.push_back(release);
    }
    RouterAction clear;
    clear.kind = RouterActionKind::kReleaseInjectedState;
    clear.source = source_;
    if (lease_) { clear.session = lease_->session; clear.generation = lease_->generation; }
    actions.push_back(clear);
    RouterAction restore;
    restore.kind = RouterActionKind::kRestoreLocal;
    restore.source = source_;
    actions.push_back(restore);
    lease_.reset();
    suppression_armed_ = false;
    next_seq_ = 1;
    state_ = RouterState::kRecovering;
    recovering_until_ = now;
    return actions;
}

std::vector<RouterAction> InputRouter::make_input(InputEvent event) {
    std::vector<RouterAction> actions;
    if (!lease_.has_value() || queue_capacity_ == 0) return recover(TimePoint{});
    event.source = source_;
    event.session = lease_->session;
    event.generation = lease_->generation;
    event.seq = next_seq_++;
    event.origin = InputOrigin::kPhysical;
    RouterAction send;
    send.kind = RouterActionKind::kSendInput;
    send.source = source_;
    send.session = event.session;
    send.generation = event.generation;
    send.seq = event.seq;
    send.event = event;
    actions.push_back(send);
    return actions;
}

std::vector<RouterAction> InputRouter::on_local_event(InputEvent event, Point, TimePoint now) {
    std::vector<RouterAction> actions;
    if (event.origin == InputOrigin::kPhysical && is_button(event.payload)) {
        const auto& button = std::get<MouseButton>(event.payload);
        if (button.button != 0) {
            if (button.pressed) held_buttons_.insert(button.button);
            else held_buttons_.erase(button.button);
        }
    }

    if (state_ == RouterState::kHandoff) {
        if (held_buttons_.empty() && event.origin == InputOrigin::kPhysical) {
            return begin_acquire(now);
        }
        return actions;
    }
    if (state_ == RouterState::kRecovering) return actions;

    if (state_ == RouterState::kRemote) {
        if (event.origin == InputOrigin::kPhysical && is_nonzero_physical(event.payload)) {
            return recover(now);
        }
        if (event.origin == InputOrigin::kRemoteInjected) return actions;
        return make_input(event);
    }
    return actions;
}

std::vector<RouterAction> InputRouter::on_crossing(Crossing crossing, TimePoint now) {
    std::vector<RouterAction> actions;
    pending_crossing_ = crossing;
    if (state_ == RouterState::kRemote) {
        if (lease_.has_value()) {
            RouterAction release;
            release.kind = RouterActionKind::kRelease;
            release.source = source_;
            release.session = lease_->session;
            release.generation = lease_->generation;
            actions.push_back(release);
        }
        lease_.reset();
        suppression_armed_ = false;
        state_ = RouterState::kHandoff;
        if (held_buttons_.empty()) {
            auto acquire = begin_acquire(now);
            actions.insert(actions.end(), acquire.begin(), acquire.end());
        }
        return actions;
    }
    if (state_ != RouterState::kLocal) return actions;
    if (!held_buttons_.empty()) {
        state_ = RouterState::kHandoff;
        return actions;
    }
    return begin_acquire(now);
}

std::vector<RouterAction> InputRouter::on_grant(AcquireResult result, TimePoint now) {
    if (state_ != RouterState::kAcquiring) return {};
    if (!result.granted()) return recover(now);
    lease_ = result.lease;
    next_seq_ = 1;
    std::vector<RouterAction> actions;
    RouterAction arm;
    arm.kind = RouterActionKind::kArmLocalSuppression;
    arm.source = source_;
    arm.session = lease_->session;
    arm.generation = lease_->generation;
    actions.push_back(arm);
    suppression_armed_ = true;
    RouterAction activate;
    activate.kind = RouterActionKind::kActivate;
    activate.source = source_;
    activate.session = lease_->session;
    activate.generation = lease_->generation;
    actions.push_back(activate);
    return actions;
}

std::vector<RouterAction> InputRouter::on_activation_ack(const Result<void>& result, TimePoint now) {
    return on_activation_ack(result.ok(), now);
}

std::vector<RouterAction> InputRouter::on_activation_ack(bool success, TimePoint now) {
    if (state_ != RouterState::kAcquiring || !lease_.has_value()) return {};
    if (!success) return recover(now);
    state_ = RouterState::kRemote;
    next_renew_ = now + m0::kLeaseRenewInterval;
    return {};
}

std::vector<RouterAction> InputRouter::on_activation_ack(const SessionId& session,
                                                          std::uint64_t generation,
                                                          bool success, TimePoint now) {
    if (!lease_.has_value() || lease_->session != session || lease_->generation != generation)
        return recover(now);
    return on_activation_ack(success, now);
}

std::vector<RouterAction> InputRouter::on_revoke(RevokeReason, TimePoint now) {
    return recover(now);
}

std::vector<RouterAction> InputRouter::on_revoke(const ValidationResult& result, TimePoint now) {
    return on_revoke(result.reason(), now);
}

std::vector<RouterAction> InputRouter::on_disconnect(TimePoint now) {
    connected_ = false;
    return recover(now);
}

std::vector<RouterAction> InputRouter::on_tick(TimePoint now) {
    if (state_ == RouterState::kRecovering) {
        state_ = RouterState::kLocal;
        connected_ = true;
        return {};
    }
    if (state_ != RouterState::kRemote || !lease_.has_value()) return {};
    if (now >= lease_->expires_at) return recover(now);
    if (now < next_renew_) return {};
    RouterAction renew;
    renew.kind = RouterActionKind::kRenew;
    renew.source = source_;
    renew.session = lease_->session;
    renew.generation = lease_->generation;
    next_renew_ = now + m0::kLeaseRenewInterval;
    return {renew};
}

} // namespace lanlink
