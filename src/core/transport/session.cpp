#include "lanlink/transport/session.hpp"

#include "lanlink/common/config.hpp"

#include <string>
#include <utility>

namespace lanlink::transport {
namespace {

bool is_timeout(const std::string& error) { return error == "read timeout"; }

} // namespace

PeerSession::PeerSession(Connection connection, PeerSessionConfig config)
    : connection_(std::move(connection)), config_(config) {}

Result<void> PeerSession::start(IClock::time_point now) {
    if (started_)
        return Result<void>::success();
    auto sent = connection_.send(protocol::Hello{config_.local_device, protocol::kProtocolVersion});
    if (!sent.ok())
        return fail_and_disconnect("hello send failed", now);
    started_ = true;
    next_heartbeat_ = now + m0::kHeartbeatInterval;
    return Result<void>::success();
}

Result<void> PeerSession::send_actions(const std::vector<RouterAction>& actions) {
    if (!started_ || disconnected_)
        return Result<void>::failure("session is closed");
    return dispatch_actions(actions, IClock::time_point{});
}

Result<void> PeerSession::dispatch_actions(const std::vector<RouterAction>& actions,
                                           IClock::time_point now) {
    for (const auto& action : actions) {
        Result<void> result = Result<void>::success();
        switch (action.kind) {
        case RouterActionKind::kArmLocalSuppression:
            if (config_.backend)
                result = config_.backend->set_suppression(true);
            break;
        case RouterActionKind::kReleaseInjectedState:
            if (config_.backend)
                config_.backend->release_injected_state(action.session);
            break;
        case RouterActionKind::kRestoreLocal:
            if (config_.backend)
                result = config_.backend->recover();
            break;
        case RouterActionKind::kAcquire:
            pending_acquire_ = action;
            result = connection_.send(protocol::AcquireRequest{action.source, action.target_display,
                                                               action.request, action.entry});
            break;
        case RouterActionKind::kActivate:
            result = connection_.send(protocol::ActivateRequest{action.session, action.generation});
            break;
        case RouterActionKind::kRenew:
            result = connection_.send(protocol::RenewRequest{action.session, action.generation});
            break;
        case RouterActionKind::kSendInput:
            result = connection_.send(protocol::InputMessage{action.event});
            break;
        case RouterActionKind::kRelease:
            result = connection_.send(protocol::Release{action.session, action.generation});
            break;
        case RouterActionKind::kDrop:
            break;
        }
        if (!result.ok())
            return fail_and_disconnect("message send failed", now);
    }
    return Result<void>::success();
}

Result<void> PeerSession::pump(IClock::time_point now, std::chrono::milliseconds timeout) {
    if (!started_ || disconnected_)
        return Result<void>::failure("session is closed");
    auto received = connection_.receive(timeout);
    if (!received.ok()) {
        if (is_timeout(received.error()))
            return Result<void>::success();
        return fail_and_disconnect(received.error().c_str(), now);
    }
    for (const auto& message : received.value()) {
        auto result = dispatch_message(message, now);
        if (!result.ok())
            return result;
    }
    return Result<void>::success();
}

Result<void> PeerSession::dispatch_message(const protocol::Message& message,
                                           IClock::time_point now) {
    if (const auto* hello = std::get_if<protocol::Hello>(&message)) {
        if (config_.peer.id != DeviceId{} && config_.peer.id != hello->device)
            return fail_and_disconnect("peer identity mismatch", now);
        hello_received_ = true;
        liveness_.mark_alive(now);
        return Result<void>::success();
    }
    if (const auto* heartbeat = std::get_if<protocol::Heartbeat>(&message)) {
        if (!hello_received_)
            return fail_and_disconnect("hello required", now);
        (void)heartbeat;
        liveness_.mark_alive(now);
        return Result<void>::success();
    }
    if (!hello_received_)
        return fail_and_disconnect("hello required", now);
    liveness_.mark_alive(now);
    if (std::holds_alternative<protocol::ErrorMessage>(message))
        return fail_and_disconnect("peer reported protocol error", now);

    if (const auto* request = std::get_if<protocol::AcquireRequest>(&message)) {
        if (!config_.arbiter)
            return fail_and_disconnect("unexpected acquire request", now);
        pending_target_acquire_ = *request;
        const auto result = config_.arbiter->acquire(
            config_.peer, request->source, request->target_display, request->request, now);
        protocol::AcquireResponse response{
            request->request, result.granted(), {}, 0, result.error_message};
        if (result.granted()) {
            response.session = result.lease.session;
            response.generation = result.lease.generation;
        } else {
            pending_target_acquire_.reset();
        }
        auto sent = connection_.send(response);
        if (!sent.ok())
            return fail_and_disconnect("acquire response failed", now);
        return Result<void>::success();
    }
    if (const auto* response = std::get_if<protocol::AcquireResponse>(&message)) {
        if (!config_.router || !pending_acquire_ || pending_acquire_->request != response->request)
            return fail_and_disconnect("unexpected acquire response", now);
        AcquireResult result;
        result.granted_value = response->granted;
        result.error_message = response->error;
        if (response->granted) {
            result.lease.peer = config_.peer;
            result.lease.source = pending_acquire_->source;
            result.lease.target_display = pending_acquire_->target_display;
            result.lease.request = response->request;
            result.lease.session = response->session;
            result.lease.generation = response->generation;
            result.lease.expires_at = now + m0::kLeaseDuration;
        }
        pending_acquire_.reset();
        return dispatch_actions(config_.router->on_grant(std::move(result), now), now);
    }
    if (const auto* request = std::get_if<protocol::ActivateRequest>(&message)) {
        if (!config_.arbiter)
            return fail_and_disconnect("unexpected activate request", now);
        const auto result = config_.arbiter->activate(request->session, request->generation, now);
        bool success = result.ok();
        std::string error = result.ok() ? std::string{} : result.error();
        if (success && config_.backend && pending_target_acquire_.has_value() &&
            pending_target_acquire_->request == request->session) {
            const auto& entry = pending_target_acquire_->entry;
            InputEvent warp{pending_target_acquire_->source,
                            request->session,
                            request->generation,
                            0,
                            PointerWarp{entry.x, entry.y},
                            InputOrigin::kRemoteInjected};
            const auto injected = config_.backend->inject(warp);
            if (!injected.ok()) {
                config_.arbiter->revoke_current(RevokeReason::kProtocolError, now);
                config_.backend->release_injected_state(request->session);
                success = false;
                error = injected.error();
            }
        }
        pending_target_acquire_.reset();
        auto sent = connection_.send(protocol::ActivateAck{
            request->session, request->generation, success, error});
        if (!sent.ok())
            return fail_and_disconnect("activate response failed", now);
        return Result<void>::success();
    }
    if (const auto* ack = std::get_if<protocol::ActivateAck>(&message)) {
        if (!config_.router)
            return fail_and_disconnect("unexpected activate response", now);
        return dispatch_actions(
            config_.router->on_activation_ack(ack->session, ack->generation, ack->success, now),
            now);
    }
    if (const auto* request = std::get_if<protocol::RenewRequest>(&message)) {
        if (!config_.arbiter)
            return fail_and_disconnect("unexpected renew request", now);
        const auto result =
            config_.arbiter->renew(config_.peer, request->session, request->generation, now);
        auto sent = connection_.send(protocol::RenewAck{
            request->session, request->generation, result.ok(), result.ok() ? "" : result.error()});
        if (!sent.ok())
            return fail_and_disconnect("renew response failed", now);
        return Result<void>::success();
    }
    if (const auto* ack = std::get_if<protocol::RenewAck>(&message)) {
        if (!config_.router || !config_.router->lease() ||
            config_.router->lease()->session != ack->session ||
            config_.router->lease()->generation != ack->generation)
            return Result<void>::success();
        if (!ack->success)
            return dispatch_actions(config_.router->on_revoke(RevokeReason::kProtocolError, now),
                                    now);
        auto lease = *config_.router->lease();
        lease.expires_at = now + m0::kLeaseDuration;
        return dispatch_actions(config_.router->on_renew_ack(lease, now), now);
    }
    if (const auto* input = std::get_if<protocol::InputMessage>(&message)) {
        if (!config_.arbiter)
            return fail_and_disconnect("unexpected input message", now);
        auto validation = config_.arbiter->validate(config_.peer, input->event, now);
        if (validation.revoked()) {
            auto sent = connection_.send(protocol::Revoke{
                input->event.session, input->event.generation, validation.reason()});
            if (!sent.ok())
                return fail_and_disconnect("revoke send failed", now);
            return Result<void>::success();
        }
        if (validation.accepted() && config_.backend) {
            auto injected = config_.backend->inject(input->event);
            if (!injected.ok()) {
                config_.arbiter->revoke_current(RevokeReason::kProtocolError, now);
                auto sent = connection_.send(protocol::Revoke{
                    input->event.session, input->event.generation, RevokeReason::kProtocolError});
                if (!sent.ok())
                    return fail_and_disconnect("revoke send failed", now);
            }
        }
        return Result<void>::success();
    }
    if (const auto* release = std::get_if<protocol::Release>(&message)) {
        if (!config_.arbiter)
            return fail_and_disconnect("unexpected release message", now);
        config_.arbiter->release(release->session, now);
        if (config_.backend)
            config_.backend->release_injected_state(release->session);
        return Result<void>::success();
    }
    if (const auto* revoke = std::get_if<protocol::Revoke>(&message)) {
        if (!config_.router)
            return fail_and_disconnect("unexpected revoke message", now);
        return dispatch_actions(config_.router->on_revoke(revoke->reason, now), now);
    }
    return fail_and_disconnect("unsupported protocol message", now);
}

Result<void> PeerSession::apply_local_actions(const std::vector<RouterAction>& actions) {
    for (const auto& action : actions) {
        if (action.kind == RouterActionKind::kArmLocalSuppression) {
            if (config_.backend) {
                auto result = config_.backend->set_suppression(true);
                if (!result.ok())
                    return result;
            }
        } else if (action.kind == RouterActionKind::kReleaseInjectedState) {
            if (config_.backend)
                config_.backend->release_injected_state(action.session);
        } else if (action.kind == RouterActionKind::kRestoreLocal && config_.backend) {
            auto result = config_.backend->recover();
            if (!result.ok())
                return result;
        }
    }
    return Result<void>::success();
}

Result<void> PeerSession::disconnect(IClock::time_point now) {
    if (disconnected_)
        return Result<void>::success();
    disconnected_ = true;
    connection_.close();
    if (config_.router) {
        auto actions = config_.router->on_disconnect(now);
        auto local = apply_local_actions(actions);
        if (!local.ok())
            return local;
    }
    if (config_.arbiter && config_.arbiter->has_lease()) {
        const auto session = config_.arbiter->lease()->session;
        config_.arbiter->revoke_current(RevokeReason::kProtocolError, now);
        if (config_.backend)
            config_.backend->release_injected_state(session);
    }
    if (config_.backend && !config_.router)
        return config_.backend->recover();
    return Result<void>::success();
}

Result<void> PeerSession::fail_and_disconnect(const char* error, IClock::time_point now) {
    disconnect(now);
    return Result<void>::failure(error);
}

Result<void> PeerSession::tick(IClock::time_point now) {
    if (!started_ || disconnected_)
        return Result<void>::failure("session is closed");
    if (hello_received_ && liveness_.expired(now))
        return fail_and_disconnect("connection timeout", now);
    if (now >= next_heartbeat_) {
        auto result = connection_.send(protocol::Heartbeat{++heartbeat_counter_});
        if (!result.ok())
            return fail_and_disconnect("heartbeat send failed", now);
        next_heartbeat_ = now + m0::kHeartbeatInterval;
    }
    return Result<void>::success();
}

} // namespace lanlink::transport
