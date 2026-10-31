#include "lanlink/input/arbiter.hpp"

#include "lanlink/common/config.hpp"

#include <string>
#include <type_traits>
#include <utility>

namespace lanlink {
namespace {
constexpr bool is_zero(const Payload& payload) {
    return std::visit(
        [](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, MouseDelta>) {
                return value.zero();
            } else if constexpr (std::is_same_v<T, MouseWheel>) {
                return value.zero();
            } else if constexpr (std::is_same_v<T, Key>) {
                return value.code == 0;
            } else if constexpr (std::is_same_v<T, MouseButton>) {
                return value.button == 0;
            } else {
                // Pointer warps are synchronization hints and never constitute local physical
                // input.
                return true;
            }
        },
        payload);
}
} // namespace

AcquireResult InputArbiter::acquire(const AuthenticatedPeer& peer, const SourceId& source,
                                    const DisplayId& target_display, const RequestId& request,
                                    TimePoint now) {
    expire_if_needed(now);
    if (lease_.has_value()) {
        return AcquireResult{false, {}, "target already has a lease"};
    }

    ++generation_;
    if (generation_ == 0)
        ++generation_;
    ControlLease lease;
    lease.peer = peer;
    lease.source = source;
    lease.target_display = target_display;
    lease.request = request;
    // M0 has no separate session allocator; request IDs identify the granted session.
    lease.session = request;
    lease.generation = generation_;
    lease.expires_at = now + m0::kLeaseDuration;
    lease.active = false;
    lease_ = lease;
    last_seq_ = 0;
    released_keys_.clear();
    released_buttons_.clear();
    return AcquireResult{true, lease, {}};
}

Result<void> InputArbiter::activate(const SessionId& session, std::uint64_t generation,
                                    TimePoint now) {
    expire_if_needed(now);
    if (!lease_.has_value())
        return Result<void>::failure("no pending lease");
    if (lease_->session != session || lease_->generation != generation) {
        return Result<void>::failure("session or generation mismatch");
    }
    if (generation == 0)
        return Result<void>::failure("generation must be non-zero");
    lease_->active = true;
    last_seq_ = 0;
    return Result<void>::success();
}

Result<void> InputArbiter::renew(const AuthenticatedPeer& peer, const SessionId& session,
                                 std::uint64_t generation, TimePoint now) {
    expire_if_needed(now);
    if (!lease_.has_value() || !lease_->active)
        return Result<void>::failure("lease is not active");
    if (lease_->peer != peer || lease_->session != session || lease_->generation != generation) {
        return Result<void>::failure("lease identity mismatch");
    }
    lease_->expires_at = now + m0::kLeaseDuration;
    return Result<void>::success();
}

ValidationResult InputArbiter::validate(const AuthenticatedPeer& peer, const InputEvent& event,
                                        TimePoint now) {
    expire_if_needed(now);
    if (!lease_.has_value())
        return reject("no active lease");
    if (!lease_->active)
        return reject("lease is not active");
    if (peer.id != lease_->peer.id)
        return reject("peer mismatch");
    if (event.source != lease_->source)
        return reject("source mismatch");
    if (event.session != lease_->session)
        return reject("session mismatch");
    if (event.generation == 0 || event.generation != lease_->generation) {
        return reject("generation mismatch");
    }
    if (event.seq == 0)
        return reject("sequence must be non-zero");

    const auto expected = last_seq_ + 1;
    if (last_seq_ == 0 && event.seq != 1) {
        return revoke_and_reject(RevokeReason::kSequenceGap, "sequence must start at one", now);
    }
    if (last_seq_ != 0 && event.seq < expected)
        return reject("duplicate or regressing sequence");
    if (last_seq_ != 0 && event.seq > expected) {
        return revoke_and_reject(RevokeReason::kSequenceGap, "sequence gap", now);
    }
    last_seq_ = event.seq;

    std::visit(
        [this](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, Key>) {
                if (value.pressed) {
                    pressed_keys_.insert(value.code);
                } else {
                    pressed_keys_.erase(value.code);
                }
            } else if constexpr (std::is_same_v<T, MouseButton>) {
                if (value.pressed) {
                    pressed_buttons_.insert(value.button);
                } else {
                    pressed_buttons_.erase(value.button);
                }
            }
        },
        event.payload);
    return ValidationResult{true, false, RevokeReason::kProtocolError, {}};
}

void InputArbiter::release(const SessionId& session, TimePoint now) {
    (void)now;
    if (!lease_.has_value() || lease_->session != session)
        return;
    clear_ledger();
    lease_.reset();
    last_seq_ = 0;
    last_revoke_reason_ = RevokeReason::kExplicitRelease;
}

void InputArbiter::revoke_current(RevokeReason reason, TimePoint now) {
    (void)now;
    if (!lease_.has_value())
        return;
    clear_ledger();
    lease_.reset();
    last_seq_ = 0;
    last_revoke_reason_ = reason;
}

void InputArbiter::on_local_input(const InputEvent& event, TimePoint now) {
    if (!lease_.has_value() || !lease_->active)
        return;
    if (event.origin != InputOrigin::kPhysical || is_zero(event.payload))
        return;
    revoke_current(RevokeReason::kLocalPreempt, now);
}

void InputArbiter::clear_ledger() {
    released_keys_.assign(pressed_keys_.begin(), pressed_keys_.end());
    released_buttons_.assign(pressed_buttons_.begin(), pressed_buttons_.end());
    pressed_keys_.clear();
    pressed_buttons_.clear();
}

void InputArbiter::expire_if_needed(TimePoint now) {
    if (lease_.has_value() && now >= lease_->expires_at) {
        revoke_current(RevokeReason::kExpired, now);
    }
}

ValidationResult InputArbiter::reject(std::string message) const {
    return ValidationResult{false, false, RevokeReason::kProtocolError, std::move(message)};
}

ValidationResult InputArbiter::revoke_and_reject(RevokeReason reason, std::string message,
                                                 TimePoint now) {
    revoke_current(reason, now);
    return ValidationResult{false, true, reason, std::move(message)};
}

} // namespace lanlink
