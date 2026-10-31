#pragma once

#include "lanlink/input/arbiter.hpp"
#include "lanlink/input/router.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <vector>

namespace lanlink {

class MockBackend {
  public:
    using EventCallback = std::function<void(const InputEvent&)>;

    struct SuppressionRecord { bool enabled{}; };
    std::vector<SuppressionRecord> suppression_history;
    std::vector<InputEvent> injected_events;
    std::vector<InputEvent> physical_events;
    std::vector<SessionId> released_sessions;
    EventCallback emit_physical;
    EventCallback emit_remote_injected;

    void suppress_local(bool enabled = true);
    void restore_local();
    void inject(const InputEvent& event);
    void release_injected_state(const SessionId& session);
    void emit_physical_event(const InputEvent& event);
    void emit_remote_event(const InputEvent& event);
    bool suppressed() const noexcept { return suppressed_; }
    void clear();

  private:
    bool suppressed_{false};
};

class MockTransport {
  public:
    struct Delivery {
        RouterAction action;
        std::size_t due_step{};
    };

    explicit MockTransport(std::size_t queue_capacity = 256);
    bool send(const RouterAction& action);
    bool enqueue(const RouterAction& action) { return send(action); }
    std::vector<RouterAction> deliver();
    std::vector<RouterAction> deliver_ready();
    void advance(std::size_t steps = 1);
    void clear();

    void set_drop_next(bool value = true) { drop_next_ = value; }
    void drop_next() { drop_next_ = true; }
    void set_delay(std::size_t steps) { delay_steps_ = steps; }
    void set_duplicate_next(bool value = true) { duplicate_next_ = value; }
    void duplicate_next() { duplicate_next_ = true; }
    void set_reorder_next(bool value = true) { reorder_next_ = value; }
    void reorder_next() { reorder_next_ = true; }
    void set_queue_capacity(std::size_t capacity) { queue_capacity_ = capacity; }
    std::size_t queue_size() const noexcept { return queue_.size(); }
    bool overflowed() const noexcept { return overflowed_; }
    const std::vector<RouterAction>& dropped() const noexcept { return dropped_; }
    const std::vector<Delivery>& queued() const noexcept { return queue_; }
    void connect(InputRouter* router) { router_ = router; }
    void connect(MockBackend* backend) { backend_ = backend; }
    void connect(InputArbiter* arbiter, const AuthenticatedPeer& peer) {
        arbiter_ = arbiter; peer_ = peer;
    }
    std::vector<RouterAction> pump(InputRouter& router, IClock::time_point now);

  private:
    void execute_actions(const std::vector<RouterAction>& actions, IClock::time_point now);
    static bool can_coalesce(const RouterAction& lhs, const RouterAction& rhs);
    static RouterAction coalesce(const RouterAction& lhs, const RouterAction& rhs);
    std::size_t queue_capacity_;
    std::size_t step_{0};
    std::size_t delay_steps_{0};
    bool drop_next_{false};
    bool duplicate_next_{false};
    bool reorder_next_{false};
    bool overflowed_{false};
    std::vector<Delivery> queue_;
    std::vector<RouterAction> dropped_;
    InputRouter* router_{nullptr};
    InputArbiter* arbiter_{nullptr};
    MockBackend* backend_{nullptr};
    AuthenticatedPeer peer_{};
};

} // namespace lanlink
