#include "lanlink/transport/manager.hpp"

#include <algorithm>
#include <utility>

namespace lanlink::transport {
namespace {

ConnectionDialer default_dialer() {
    return [](const Endpoint& endpoint, const ConnectionOptions& options) {
        return Connection::connect(endpoint, options);
    };
}

Result<void> first_error(Result<void> current, const Result<void>& next) {
    if (current.ok() && !next.ok())
        return Result<void>::failure(next.error());
    return current;
}

} // namespace

ConnectionManager::ConnectionManager(ConnectionDialer dialer)
    : dialer_(dialer ? std::move(dialer) : default_dialer()) {}

ConnectionManager::~ConnectionManager() {
    for (auto& slot : slots_) {
        if (slot.session)
            (void)slot.session->disconnect(IClock::time_point{});
    }
}

Result<PeerHandle> ConnectionManager::add_outgoing(OutgoingPeer peer) {
    if (peer.reconnect.initial_delay.count() < 0 || peer.reconnect.max_delay.count() < 0 ||
        peer.reconnect.max_delay < peer.reconnect.initial_delay) {
        return Result<PeerHandle>::failure("invalid reconnect policy");
    }

    Slot slot;
    slot.handle = next_handle_++;
    slot.state = PeerState::kBackoff;
    slot.outgoing = true;
    slot.endpoint = std::move(peer.endpoint);
    slot.connection = std::move(peer.connection);
    slot.session_config = peer.session;
    slot.reconnect = peer.reconnect;
    slot.next_delay = slot.reconnect.initial_delay;
    slot.next_retry = IClock::time_point{};
    slots_.push_back(std::move(slot));
    return Result<PeerHandle>::success(slots_.back().handle);
}

Result<PeerHandle> ConnectionManager::add_incoming(Connection connection,
                                                   PeerSessionConfig session,
                                                   IClock::time_point now) {
    if (!connection.connected())
        return Result<PeerHandle>::failure("connection is closed");

    Slot slot;
    slot.handle = next_handle_++;
    slot.state = PeerState::kConnecting;
    slot.session_config = session;
    slot.session = std::make_unique<PeerSession>(std::move(connection), session);
    auto started = slot.session->start(now);
    if (!started.ok()) {
        (void)slot.session->disconnect(now);
        return Result<PeerHandle>::failure(started.error());
    }
    slot.state = PeerState::kConnected;
    slots_.push_back(std::move(slot));
    return Result<PeerHandle>::success(slots_.back().handle);
}

ConnectionManager::Slot* ConnectionManager::find(PeerHandle handle) noexcept {
    for (auto& slot : slots_) {
        if (slot.handle == handle)
            return &slot;
    }
    return nullptr;
}

const ConnectionManager::Slot* ConnectionManager::find(PeerHandle handle) const noexcept {
    for (const auto& slot : slots_) {
        if (slot.handle == handle)
            return &slot;
    }
    return nullptr;
}

void ConnectionManager::schedule_retry(Slot& slot, IClock::time_point now) {
    slot.state = PeerState::kBackoff;
    slot.next_retry = now + slot.next_delay;
    if (slot.next_delay < slot.reconnect.max_delay) {
        const auto doubled = slot.next_delay * 2;
        slot.next_delay = std::min(doubled, slot.reconnect.max_delay);
    }
}

Result<void> ConnectionManager::handle_failure(Slot& slot, const std::string& error,
                                               IClock::time_point now) {
    std::string failure = error;
    if (slot.session) {
        auto cleanup = slot.session->disconnect(now);
        if (failure.empty() && !cleanup.ok())
            failure = cleanup.error();
        slot.session.reset();
    }

    slot.last_error = failure;
    if (slot.outgoing)
        schedule_retry(slot, now);
    else
        slot.state = PeerState::kClosed;
    return Result<void>::failure(failure.empty() ? "peer connection failed" : failure);
}

Result<void> ConnectionManager::attempt(Slot& slot, IClock::time_point now) {
    slot.state = PeerState::kConnecting;
    auto connection = dialer_(slot.endpoint, slot.connection);
    if (!connection.ok())
        return handle_failure(slot, connection.error(), now);

    slot.session = std::make_unique<PeerSession>(std::move(connection.value()), slot.session_config);
    auto started = slot.session->start(now);
    if (!started.ok())
        return handle_failure(slot, started.error(), now);

    if (slot.session_config.router &&
        slot.session_config.router->state() == RouterState::kRecovering) {
        auto recovery = slot.session_config.router->on_tick(now);
        if (!recovery.empty()) {
            auto sent = slot.session->send_actions(recovery);
            if (!sent.ok())
                return handle_failure(slot, sent.error(), now);
        }
    }

    slot.state = PeerState::kConnected;
    slot.last_error.clear();
    slot.next_delay = slot.reconnect.initial_delay;
    return Result<void>::success();
}

Result<void> ConnectionManager::drive(Slot& slot, IClock::time_point now,
                                      std::chrono::milliseconds timeout) {
    if (!slot.session)
        return handle_failure(slot, "peer session is missing", now);
    auto pumped = slot.session->pump(now, timeout);
    if (!pumped.ok())
        return handle_failure(slot, pumped.error(), now);
    return Result<void>::success();
}

Result<void> ConnectionManager::poll(IClock::time_point now,
                                     std::chrono::milliseconds timeout) {
    Result<void> result = Result<void>::success();
    for (auto& slot : slots_) {
        if (slot.state == PeerState::kClosed)
            continue;
        if (slot.outgoing && slot.state == PeerState::kBackoff && now >= slot.next_retry) {
            result = first_error(std::move(result), attempt(slot, now));
        }
        if (slot.state == PeerState::kConnected)
            result = first_error(std::move(result), drive(slot, now, timeout));
    }
    return result;
}

Result<void> ConnectionManager::tick(IClock::time_point now) {
    Result<void> result = Result<void>::success();
    for (auto& slot : slots_) {
        if (slot.state == PeerState::kClosed)
            continue;
        if (slot.outgoing && slot.state == PeerState::kBackoff && now >= slot.next_retry)
            result = first_error(std::move(result), attempt(slot, now));
        if (slot.state != PeerState::kConnected || !slot.session)
            continue;
        auto ticked = slot.session->tick(now);
        if (!ticked.ok())
            result = first_error(std::move(result), handle_failure(slot, ticked.error(), now));
    }
    return result;
}

Result<void> ConnectionManager::send_actions(PeerHandle handle,
                                              const std::vector<RouterAction>& actions,
                                              IClock::time_point now) {
    auto* slot = find(handle);
    if (!slot)
        return Result<void>::failure("unknown peer handle");
    if (slot->state != PeerState::kConnected || !slot->session)
        return Result<void>::failure("peer is not connected");
    auto sent = slot->session->send_actions(actions);
    if (sent.ok())
        return sent;
    return handle_failure(*slot, sent.error(), now);
}

Result<void> ConnectionManager::send_actions(PeerHandle handle,
                                              const std::vector<RouterAction>& actions) {
    return send_actions(handle, actions, IClock::Clock::now());
}

Result<void> ConnectionManager::remove(PeerHandle handle, IClock::time_point now) {
    auto* slot = find(handle);
    if (!slot)
        return Result<void>::failure("unknown peer handle");
    if (slot->state == PeerState::kClosed)
        return Result<void>::success();

    Result<void> result = Result<void>::success();
    if (slot->session) {
        result = slot->session->disconnect(now);
        slot->session.reset();
    }
    slot->state = PeerState::kClosed;
    return result;
}

std::optional<PeerState> ConnectionManager::state(PeerHandle handle) const {
    const auto* slot = find(handle);
    if (!slot)
        return std::nullopt;
    return slot->state;
}

bool ConnectionManager::ready(PeerHandle handle) const noexcept {
    const auto* slot = find(handle);
    return slot != nullptr && slot->state == PeerState::kConnected && slot->session &&
           slot->session->ready();
}

std::size_t ConnectionManager::size() const noexcept {
    std::size_t count = 0;
    for (const auto& slot : slots_) {
        if (slot.state != PeerState::kClosed)
            ++count;
    }
    return count;
}

} // namespace lanlink::transport
