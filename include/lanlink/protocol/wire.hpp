#pragma once

#include "lanlink/common/ids.hpp"
#include "lanlink/common/result.hpp"
#include "lanlink/input/arbiter.hpp"
#include "lanlink/input/events.hpp"
#include "lanlink/topology/topology.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace lanlink::protocol {

constexpr std::uint8_t kProtocolVersion = 1;
constexpr std::size_t kHeaderSize = 12;
constexpr std::size_t kMaxFrameSize = 64 * 1024;
constexpr std::size_t kMaxErrorText = 4 * 1024;

enum class MessageType : std::uint8_t {
    kHello = 1,
    kHeartbeat,
    kAcquireRequest,
    kAcquireResponse,
    kActivateRequest,
    kActivateAck,
    kRenewRequest,
    kRenewAck,
    kInput,
    kRelease,
    kRevoke,
    kError,
};

struct Hello {
    DeviceId device;
    std::uint16_t advertised_version{};
    friend bool operator==(const Hello&, const Hello&) = default;
};
struct Heartbeat {
    std::uint64_t counter{};
    friend bool operator==(const Heartbeat&, const Heartbeat&) = default;
};
struct AcquireRequest {
    SourceId source;
    DisplayId target_display;
    RequestId request;
    Point entry;
    friend bool operator==(const AcquireRequest&, const AcquireRequest&) = default;
};
struct AcquireResponse {
    RequestId request;
    bool granted{};
    SessionId session;
    std::uint64_t generation{};
    std::string error;
    friend bool operator==(const AcquireResponse&, const AcquireResponse&) = default;
};
struct ActivateRequest {
    SessionId session;
    std::uint64_t generation{};
    friend bool operator==(const ActivateRequest&, const ActivateRequest&) = default;
};
struct ActivateAck {
    SessionId session;
    std::uint64_t generation{};
    bool success{};
    std::string error;
    friend bool operator==(const ActivateAck&, const ActivateAck&) = default;
};
struct RenewRequest {
    SessionId session;
    std::uint64_t generation{};
    friend bool operator==(const RenewRequest&, const RenewRequest&) = default;
};
struct RenewAck {
    SessionId session;
    std::uint64_t generation{};
    bool success{};
    std::string error;
    friend bool operator==(const RenewAck&, const RenewAck&) = default;
};
struct InputMessage {
    InputEvent event;
    friend bool operator==(const InputMessage&, const InputMessage&);
};
struct Release {
    SessionId session;
    std::uint64_t generation{};
    friend bool operator==(const Release&, const Release&) = default;
};
struct Revoke {
    SessionId session;
    std::uint64_t generation{};
    RevokeReason reason{RevokeReason::kProtocolError};
    friend bool operator==(const Revoke&, const Revoke&) = default;
};
struct ErrorMessage {
    std::uint16_t code{};
    std::string message;
    friend bool operator==(const ErrorMessage&, const ErrorMessage&) = default;
};

using Message =
    std::variant<Hello, Heartbeat, AcquireRequest, AcquireResponse, ActivateRequest, ActivateAck,
                 RenewRequest, RenewAck, InputMessage, Release, Revoke, ErrorMessage>;

Result<std::vector<std::uint8_t>> encode(const Message& message);
Result<Message> decode(std::span<const std::uint8_t> frame);

class FrameDecoder {
  public:
    Result<std::vector<Message>> feed(std::span<const std::uint8_t> bytes);
    Result<void> finish();
    std::size_t buffered_bytes() const noexcept { return buffer_.size(); }
    void reset() noexcept { buffer_.clear(); }

  private:
    std::vector<std::uint8_t> buffer_;
};

} // namespace lanlink::protocol
