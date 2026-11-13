#pragma once

#include "lanlink/common/clock.hpp"
#include "lanlink/common/ids.hpp"
#include "lanlink/common/result.hpp"
#include "lanlink/input/events.hpp"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace lanlink {

struct AuthenticatedPeer {
    DeviceId id;
    friend bool operator==(const AuthenticatedPeer&, const AuthenticatedPeer&) = default;
};

struct ControlLease {
    AuthenticatedPeer peer;
    SourceId source;
    DisplayId target_display;
    RequestId request;
    SessionId session;
    std::uint64_t generation{};
    IClock::time_point expires_at{};
    bool active{};
    // Local-only marker: a lease acquired after local preemption resumes the
    // target cursor instead of warping it to the crossing edge.
    bool resume_pointer{};

    IClock::time_point expiry() const noexcept { return expires_at; }
};

enum class RevokeReason {
    kExplicitRelease,
    kExpired,
    kSequenceGap,
    kLocalPreempt,
    kPeerMismatch,
    kProtocolError,
    kSeqGap = kSequenceGap,
    ExplicitRelease = kExplicitRelease,
    Expired = kExpired,
    SeqGap = kSequenceGap,
    LocalPreempt = kLocalPreempt,
    PeerMismatch = kPeerMismatch,
    ProtocolError = kProtocolError,
    LOCAL_PREEMPT = kLocalPreempt,
};

struct AcquireResult {
    bool granted_value{};
    ControlLease lease{};
    std::string error_message;

    bool granted() const noexcept { return granted_value; }
    bool ok() const noexcept { return granted_value; }
    explicit operator bool() const noexcept { return granted_value; }
    const std::string& error() const noexcept { return error_message; }
};

struct ValidationResult {
    bool accepted_value{};
    bool revoked_value{};
    RevokeReason revoke_reason{RevokeReason::kProtocolError};
    std::string error_message;

    bool accepted() const noexcept { return accepted_value; }
    bool valid() const noexcept { return accepted_value; }
    bool ok() const noexcept { return accepted_value; }
    bool revoked() const noexcept { return revoked_value; }
    RevokeReason reason() const noexcept { return revoke_reason; }
    explicit operator bool() const noexcept { return accepted_value; }
    const std::string& error() const noexcept { return error_message; }
};

class InputArbiter {
  public:
    using TimePoint = IClock::time_point;

    AcquireResult acquire(const AuthenticatedPeer& peer, const SourceId& source,
                          const DisplayId& target_display, const RequestId& request, TimePoint now);
    Result<void> activate(const SessionId& session, std::uint64_t generation, TimePoint now);
    Result<void> renew(const AuthenticatedPeer& peer, const SessionId& session,
                       std::uint64_t generation, TimePoint now);
    ValidationResult validate(const AuthenticatedPeer& peer, const InputEvent& event,
                              TimePoint now);

    void release(const SessionId& session, TimePoint now);
    void revoke_current(RevokeReason reason, TimePoint now);
    void on_local_input(const InputEvent& event, TimePoint now);

    bool has_lease() const noexcept { return lease_.has_value(); }
    bool has_active_lease() const noexcept { return lease_.has_value() && lease_->active; }
    const std::optional<ControlLease>& lease() const noexcept { return lease_; }
    const std::optional<ControlLease>& current_lease() const noexcept { return lease_; }
    bool can_resume_pointer(const SourceId& source, const DisplayId& target_display) const noexcept;
    void discard_resume_pointer() noexcept { resumable_lease_.reset(); }
    std::uint64_t generation() const noexcept { return generation_; }
    RevokeReason last_revoke_reason() const noexcept { return last_revoke_reason_; }

    const std::set<std::uint32_t>& pressed_keys() const noexcept { return pressed_keys_; }
    const std::set<std::uint32_t>& pressed_buttons() const noexcept { return pressed_buttons_; }
    const std::vector<std::uint32_t>& released_keys() const noexcept { return released_keys_; }
    const std::vector<std::uint32_t>& released_buttons() const noexcept {
        return released_buttons_;
    }
    bool ledger_empty() const noexcept { return pressed_keys_.empty() && pressed_buttons_.empty(); }

  private:
    void clear_ledger();
    void expire_if_needed(TimePoint now);
    ValidationResult reject(std::string message) const;
    ValidationResult revoke_and_reject(RevokeReason reason, std::string message, TimePoint now);

    std::optional<ControlLease> lease_;
    std::optional<ControlLease> resumable_lease_;
    std::uint64_t generation_{};
    std::uint64_t last_seq_{};
    RevokeReason last_revoke_reason_{RevokeReason::kExplicitRelease};
    std::set<std::uint32_t> pressed_keys_;
    std::set<std::uint32_t> pressed_buttons_;
    std::vector<std::uint32_t> released_keys_;
    std::vector<std::uint32_t> released_buttons_;
    // Input classes observed on the remote lease. Activation seeds the mask
    // with pointer ownership because every handoff starts at a cursor edge;
    // the first remote event replaces that provisional value. A local event
    // only revokes the lease when its class is present in this mask.
    std::uint8_t remote_input_mask_{};
};

} // namespace lanlink
