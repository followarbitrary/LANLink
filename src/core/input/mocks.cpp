#include "lanlink/input/mocks.hpp"

#include <algorithm>
#include <type_traits>

namespace lanlink {

void MockBackend::suppress_local(bool enabled) {
    suppressed_ = enabled;
    suppression_history.push_back(SuppressionRecord{enabled});
}

void MockBackend::restore_local() { suppress_local(false); }

void MockBackend::inject(const InputEvent& event) {
    auto tagged = event;
    tagged.origin = InputOrigin::kRemoteInjected;
    injected_events.push_back(tagged);
    if (emit_remote_injected) emit_remote_injected(tagged);
}

void MockBackend::release_injected_state(const SessionId& session) {
    released_sessions.push_back(session);
}

void MockBackend::emit_physical_event(const InputEvent& event) {
    auto tagged = event;
    tagged.origin = InputOrigin::kPhysical;
    physical_events.push_back(tagged);
    if (emit_physical) emit_physical(tagged);
}

void MockBackend::emit_remote_event(const InputEvent& event) {
    auto tagged = event;
    tagged.origin = InputOrigin::kRemoteInjected;
    injected_events.push_back(tagged);
    if (emit_remote_injected) emit_remote_injected(tagged);
}

void MockBackend::clear() {
    suppression_history.clear();
    injected_events.clear();
    physical_events.clear();
    released_sessions.clear();
}

MockTransport::MockTransport(std::size_t queue_capacity) : queue_capacity_(queue_capacity) {}

bool MockTransport::can_coalesce(const RouterAction& lhs, const RouterAction& rhs) {
    if (lhs.kind != RouterActionKind::kSendInput || rhs.kind != RouterActionKind::kSendInput)
        return false;
    if (lhs.source != rhs.source || lhs.session != rhs.session || lhs.generation != rhs.generation)
        return false;
    if (lhs.event.session != rhs.event.session || lhs.event.generation != rhs.event.generation)
        return false;
    if (!std::holds_alternative<MouseDelta>(lhs.event.payload) ||
        !std::holds_alternative<MouseDelta>(rhs.event.payload)) return false;
    return true;
}

RouterAction MockTransport::coalesce(const RouterAction& lhs, const RouterAction& rhs) {
    RouterAction out = rhs;
    const auto& a = std::get<MouseDelta>(lhs.event.payload);
    const auto& b = std::get<MouseDelta>(rhs.event.payload);
    out.event.payload = MouseDelta{a.dx + b.dx, a.dy + b.dy};
    out.seq = rhs.seq;
    out.event.seq = rhs.event.seq;
    return out;
}

bool MockTransport::send(const RouterAction& action) {
    if (drop_next_) {
        drop_next_ = false;
        dropped_.push_back(action);
        return true;
    }
    if (action.kind == RouterActionKind::kSendInput && !queue_.empty() &&
        can_coalesce(queue_.back().action, action) && queue_.back().due_step <= step_ + delay_steps_) {
        queue_.back().action = coalesce(queue_.back().action, action);
        return true;
    }
    if (queue_capacity_ == 0 || queue_.size() >= queue_capacity_) {
        overflowed_ = true;
        dropped_.push_back(action);
        std::vector<RouterAction> queued_actions;
        queued_actions.reserve(queue_.size());
        for (const auto& delivery : queue_) queued_actions.push_back(delivery.action);
        queue_.clear(); // fail closed: stale input cannot leak after an overflow.
        if (arbiter_ || backend_) execute_actions(queued_actions, IClock::time_point{});
        if (router_) {
            const auto cleanup = router_->on_disconnect(IClock::time_point{});
            execute_actions(cleanup, IClock::time_point{});
        }
        return false;
    }
    Delivery d{action, step_ + delay_steps_};
    queue_.push_back(d);
    if (duplicate_next_) {
        duplicate_next_ = false;
        if (queue_.size() < queue_capacity_) queue_.push_back(d);
    }
    if (reorder_next_ && queue_.size() > 1) {
        reorder_next_ = false;
        std::swap(queue_[queue_.size() - 1], queue_[queue_.size() - 2]);
    }
    return true;
}

std::vector<RouterAction> MockTransport::deliver_ready() {
    std::vector<RouterAction> out;
    auto it = queue_.begin();
    while (it != queue_.end()) {
        if (it->due_step <= step_) {
            out.push_back(it->action);
            it = queue_.erase(it);
        } else {
            ++it;
        }
    }
    return out;
}

std::vector<RouterAction> MockTransport::deliver() { return deliver_ready(); }

void MockTransport::advance(std::size_t steps) { step_ += steps; }

void MockTransport::clear() {
    queue_.clear();
    dropped_.clear();
    overflowed_ = false;
}

void MockTransport::execute_actions(const std::vector<RouterAction>& actions,
                                     IClock::time_point now) {
    for (const auto& action : actions) {
        switch (action.kind) {
        case RouterActionKind::kArmLocalSuppression:
            if (backend_) backend_->suppress_local();
            break;
        case RouterActionKind::kRelease:
            if (arbiter_) arbiter_->release(action.session, now);
            break;
        case RouterActionKind::kReleaseInjectedState:
            if (backend_) backend_->release_injected_state(action.session);
            break;
        case RouterActionKind::kRestoreLocal:
            if (backend_) backend_->restore_local();
            break;
        default:
            break;
        }
    }
}

std::vector<RouterAction> MockTransport::pump(InputRouter& router, IClock::time_point now) {
    std::vector<RouterAction> produced;
    for (const auto& action : deliver_ready()) {
        std::vector<RouterAction> next;
        switch (action.kind) {
        case RouterActionKind::kAcquire:
            if (arbiter_) next = router.on_grant(
                arbiter_->acquire(peer_, action.source, action.target_display, action.request, now), now);
            break;
        case RouterActionKind::kActivate:
            if (arbiter_) {
                const auto ok = arbiter_->activate(action.session, action.generation, now);
                next = router.on_activation_ack(action.session, action.generation, ok.ok(), now);
            }
            break;
        case RouterActionKind::kRenew:
            if (arbiter_ && router.lease()) {
                auto ok = arbiter_->renew(peer_, action.session, action.generation, now);
                if (!ok) next = router.on_revoke(RevokeReason::kProtocolError, now);
                else if (arbiter_->lease()) router.on_renew_ack(*arbiter_->lease(), now);
            }
            break;
        case RouterActionKind::kSendInput:
            if (arbiter_) {
                auto validation = arbiter_->validate(peer_, action.event, now);
                if (validation.revoked() || !arbiter_->has_lease())
                    next = router.on_revoke(validation.reason(), now);
                else if (backend_)
                    backend_->inject(action.event);
            }
            break;
        case RouterActionKind::kReleaseInjectedState:
            if (backend_) backend_->release_injected_state(action.session);
            break;
        case RouterActionKind::kRestoreLocal:
            if (backend_) backend_->restore_local();
            break;
        case RouterActionKind::kRelease:
            if (arbiter_) arbiter_->release(action.session, now);
            break;
        default:
            break;
        }
        for (const auto& generated : next) {
            produced.push_back(generated);
            send(generated);
        }
    }
    return produced;
}

} // namespace lanlink
