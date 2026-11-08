#include "lanlink/transport/session.hpp"

#include "lanlink/common/config.hpp"
#include "lanlink/common/debug.hpp"

#include <cmath>
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
    if (config_.router)
        config_.router->clear_peer_display_info();
    local_display_info_.reset();
    debug::log("send hello local_device=", config_.local_device.low,
               " protocol=", static_cast<unsigned>(protocol::kProtocolVersion));
    auto sent = connection_.send(protocol::Hello{config_.local_device, protocol::kProtocolVersion});
    if (!sent.ok())
        return fail_and_disconnect("hello send failed", now);
    auto display = send_display_info_if_changed(true, now);
    if (!display.ok())
        return display;
    started_ = true;
    next_heartbeat_ = now + m0::kHeartbeatInterval;
    next_display_info_check_ = now + m0::kHeartbeatInterval;
    next_pointer_position_ = now;
    return Result<void>::success();
}

Result<void> PeerSession::send_display_info_if_changed(bool force, IClock::time_point now) {
    if (config_.local_display == DisplayId{} || config_.backend == nullptr)
        return Result<void>::success();
    const auto bounds = config_.backend->display_bounds();
    if (!bounds.has_value())
        return Result<void>::success();
    auto scale = config_.backend->display_scale();
    if (!std::isfinite(scale) || scale <= 0.0 || scale > protocol::kMaxDisplayScale)
        scale = 1.0;
    protocol::DisplayInfo info{config_.local_display, *bounds, scale};
    if (!force && local_display_info_.has_value() && *local_display_info_ == info)
        return Result<void>::success();
    auto sent = connection_.send(info);
    if (!sent.ok())
        return fail_and_disconnect("display info send failed", now);
    local_display_info_ = info;
    debug::log("send display info display=", info.display.low,
               " bounds=", info.logical_bounds.left, ",", info.logical_bounds.top, " ",
               info.logical_bounds.right, "x", info.logical_bounds.bottom,
               " scale=", info.scale);
    return Result<void>::success();
}

Result<void> PeerSession::send_pointer_position_if_due(IClock::time_point now) {
    if (now < next_pointer_position_)
        return Result<void>::success();
    next_pointer_position_ = now + m0::kPointerPositionInterval;
    if (config_.arbiter == nullptr || config_.backend == nullptr ||
        !config_.arbiter->has_active_lease())
        return Result<void>::success();
    const auto lease = config_.arbiter->lease();
    if (!lease.has_value() || now >= lease->expires_at)
        return Result<void>::success();
    const auto position = config_.backend->pointer_position();
    if (!position.has_value() || !std::isfinite(position->x) ||
        !std::isfinite(position->y))
        return Result<void>::success();
    debug::log("send pointer position session=", lease->session.low,
               " generation=", lease->generation, " point=", position->x, ",", position->y);
    auto sent = connection_.send(
        protocol::PointerPosition{lease->session, lease->generation, *position});
    if (!sent.ok())
        return fail_and_disconnect("pointer position send failed", now);
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
            debug::log("suppression on session=", action.session.low,
                       " generation=", action.generation);
            if (config_.backend)
                result = config_.backend->set_suppression(true);
            break;
        case RouterActionKind::kReleaseInjectedState:
            if (config_.backend)
                config_.backend->release_injected_state(action.session);
            break;
        case RouterActionKind::kRestoreLocal:
            debug::log("suppression off/recover");
            if (config_.backend)
                result = config_.backend->recover();
            break;
        case RouterActionKind::kAcquire:
            debug::log("send acquire target_display=", action.target_display.low,
                       " request=", action.request.low, " entry=", action.entry.x, ",",
                       action.entry.y, " target_edge=", static_cast<unsigned>(action.target_edge),
                       " tangent_ratio=", action.tangent_ratio,
                       " normalized=", action.entry_normalized);
            pending_acquire_ = action;
            result = connection_.send(protocol::AcquireRequest{action.source, action.target_display,
                                                               action.request, action.entry,
                                                               action.target_edge,
                                                               action.tangent_ratio,
                                                               action.entry_normalized});
            break;
        case RouterActionKind::kActivate:
            debug::log("send activate session=", action.session.low,
                       " generation=", action.generation);
            result = connection_.send(protocol::ActivateRequest{action.session, action.generation});
            break;
        case RouterActionKind::kRenew:
            debug::log("send renew session=", action.session.low,
                       " generation=", action.generation);
            result = connection_.send(protocol::RenewRequest{action.session, action.generation});
            break;
        case RouterActionKind::kSendInput:
            result = connection_.send(protocol::InputMessage{action.event});
            break;
        case RouterActionKind::kRelease:
            result = connection_.send(protocol::Release{action.session, action.generation});
            break;
        case RouterActionKind::kRevoke:
            debug::log("send lease revoke session=", action.session.low,
                       " generation=", action.generation,
                       " reason=", static_cast<unsigned>(action.reason));
            result = connection_.send(
                protocol::Revoke{action.session, action.generation, action.reason});
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
        debug::log("receive hello device=", hello->device.low,
                   " protocol=", static_cast<unsigned>(hello->advertised_version));
        if (config_.peer.id != DeviceId{} && config_.peer.id != hello->device)
            return fail_and_disconnect("peer identity mismatch", now);
        hello_received_ = true;
        liveness_.mark_alive(now);
        return Result<void>::success();
    }
    if (const auto* display = std::get_if<protocol::DisplayInfo>(&message)) {
        if (!hello_received_)
            return fail_and_disconnect("hello required", now);
        debug::log("receive display info display=", display->display.low,
                   " bounds=", display->logical_bounds.left, ",", display->logical_bounds.top,
                   " ", display->logical_bounds.right, "x", display->logical_bounds.bottom,
                   " scale=", display->scale);
        if (config_.router)
            config_.router->set_peer_display_info(display->display, display->logical_bounds,
                                                  display->scale);
        return Result<void>::success();
    }
    if (const auto* position = std::get_if<protocol::PointerPosition>(&message)) {
        if (!hello_received_)
            return fail_and_disconnect("hello required", now);
        liveness_.mark_alive(now);
        debug::log("receive pointer position session=", position->session.low,
                   " generation=", position->generation, " point=", position->position.x, ",",
                   position->position.y);
        if (config_.pointer_position_callback)
            config_.pointer_position_callback(position->session, position->generation,
                                              position->position);
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
        debug::log("receive acquire source=", request->source.low,
                   " target_display=", request->target_display.low,
                   " request=", request->request.low,
                   " target_edge=", static_cast<unsigned>(request->target_edge),
                   " tangent_ratio=", request->tangent_ratio,
                   " normalized=", request->entry_normalized);
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
        debug::log("acquire ", result.granted() ? "granted" : "rejected",
                   " request=", request->request.low,
                   result.granted() ? "" : " error=" + result.error_message);
        auto sent = connection_.send(response);
        if (!sent.ok())
            return fail_and_disconnect("acquire response failed", now);
        return Result<void>::success();
    }
    if (const auto* response = std::get_if<protocol::AcquireResponse>(&message)) {
        if (!config_.router || !pending_acquire_ || pending_acquire_->request != response->request)
            return fail_and_disconnect("unexpected acquire response", now);
        debug::log("receive acquire response request=", response->request.low,
                   " granted=", response->granted,
                   response->granted ? "" : " error=" + response->error);
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
        debug::log("receive activate session=", request->session.low,
                   " generation=", request->generation);
        const auto result = config_.arbiter->activate(request->session, request->generation, now);
        bool success = result.ok();
        std::string error = result.ok() ? std::string{} : result.error();
        if (success && config_.backend && pending_target_acquire_.has_value() &&
            pending_target_acquire_->request == request->session) {
            Point entry = pending_target_acquire_->entry;
            if (pending_target_acquire_->entry_normalized) {
                const auto bounds = config_.backend->display_bounds();
                if (bounds.has_value()) {
                    debug::log("target display bounds=", bounds->left, ",", bounds->top,
                               " ", bounds->right, "x", bounds->bottom);
                    entry = edge_entry_point(*bounds, pending_target_acquire_->target_edge,
                                             pending_target_acquire_->tangent_ratio);
                } else {
                    debug::log("target display bounds unavailable; using wire entry");
                }
            }
            InputEvent warp{pending_target_acquire_->source,
                            request->session,
                            request->generation,
                            0,
                            PointerWarp{entry.x, entry.y},
                            InputOrigin::kRemoteInjected};
            debug::log("target warp session=", request->session.low, " point=", entry.x, ",",
                       entry.y);
            const auto injected = config_.backend->inject(warp);
            if (!injected.ok()) {
                debug::log("target warp failed session=", request->session.low,
                           " error=", injected.error());
                config_.arbiter->revoke_current(RevokeReason::kProtocolError, now);
                config_.backend->release_injected_state(request->session);
                success = false;
                error = injected.error();
            }
        }
        pending_target_acquire_.reset();
        auto sent = connection_.send(protocol::ActivateAck{
            request->session, request->generation, success, error});
        debug::log("activate ", success ? "completed" : "failed",
                   " session=", request->session.low,
                   success ? "" : " error=" + error);
        if (!sent.ok())
            return fail_and_disconnect("activate response failed", now);
        return Result<void>::success();
    }
    if (const auto* ack = std::get_if<protocol::ActivateAck>(&message)) {
        if (!config_.router)
            return fail_and_disconnect("unexpected activate response", now);
        debug::log("receive activate ack session=", ack->session.low,
                   " success=", ack->success,
                   ack->success ? "" : " error=" + ack->error);
        return dispatch_actions(
            config_.router->on_activation_ack(ack->session, ack->generation, ack->success, now),
            now);
    }
    if (const auto* request = std::get_if<protocol::RenewRequest>(&message)) {
        if (!config_.arbiter)
            return fail_and_disconnect("unexpected renew request", now);
        debug::log("receive renew session=", request->session.low,
                   " generation=", request->generation);
        const auto result =
            config_.arbiter->renew(config_.peer, request->session, request->generation, now);
        debug::log("renew ", result.ok() ? "accepted" : "rejected",
                   " session=", request->session.low,
                   result.ok() ? "" : " error=" + result.error());
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
        debug::log("receive renew ack session=", ack->session.low,
                   " generation=", ack->generation,
                   " success=", ack->success,
                   ack->success ? "" : " error=" + ack->error);
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
            debug::log("lease revoke on input session=", input->event.session.low,
                       " generation=", input->event.generation,
                       " reason=", static_cast<unsigned>(validation.reason()),
                       " error=", validation.error());
            auto sent = connection_.send(protocol::Revoke{
                input->event.session, input->event.generation, validation.reason()});
            if (!sent.ok())
                return fail_and_disconnect("revoke send failed", now);
            return Result<void>::success();
        }
        if (validation.accepted() && config_.backend) {
            auto injected = config_.backend->inject(input->event);
            if (!injected.ok()) {
                debug::log("remote input injection failed session=", input->event.session.low,
                           " error=", injected.error());
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
        debug::log("lease release session=", release->session.low,
                   " generation=", release->generation);
        config_.arbiter->release(release->session, now);
        if (config_.backend)
            config_.backend->release_injected_state(release->session);
        return Result<void>::success();
    }
    if (const auto* revoke = std::get_if<protocol::Revoke>(&message)) {
        if (!config_.router)
            return fail_and_disconnect("unexpected revoke message", now);
        debug::log("receive lease revoke session=", revoke->session.low,
                   " generation=", revoke->generation,
                   " reason=", static_cast<unsigned>(revoke->reason));
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
    debug::log("session disconnect peer=", config_.peer.id.low);
    disconnected_ = true;
    connection_.close();
    if (config_.router) {
        config_.router->clear_peer_display_info();
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
    debug::log("session failure peer=", config_.peer.id.low, " error=", error);
    disconnect(now);
    return Result<void>::failure(error);
}

Result<void> PeerSession::tick(IClock::time_point now) {
    if (!started_ || disconnected_)
        return Result<void>::failure("session is closed");
    if (hello_received_ && liveness_.expired(now))
        return fail_and_disconnect("connection timeout", now);
    if (now >= next_display_info_check_) {
        auto display = send_display_info_if_changed(false, now);
        if (!display.ok())
            return display;
        next_display_info_check_ = now + m0::kHeartbeatInterval;
    }
    auto pointer = send_pointer_position_if_due(now);
    if (!pointer.ok())
        return pointer;
    if (now >= next_heartbeat_) {
        auto result = connection_.send(protocol::Heartbeat{++heartbeat_counter_});
        if (!result.ok())
            return fail_and_disconnect("heartbeat send failed", now);
        next_heartbeat_ = now + m0::kHeartbeatInterval;
    }
    return Result<void>::success();
}

} // namespace lanlink::transport
