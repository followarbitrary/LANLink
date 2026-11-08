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

// Version 5 adds periodic target cursor position corrections. Version 4 added
// absolute PointerMove input payloads. Each peer advertises the same version
// during Hello so an older executable rejects the connection before decoding
// newer messages.
constexpr std::uint8_t kProtocolVersion = 5;
constexpr std::size_t kHeaderSize = 12;
constexpr std::size_t kMaxFrameSize = 64 * 1024;
constexpr std::size_t kMaxErrorText = 4 * 1024;
// Keep untrusted display metadata inside a desktop-sized coordinate range.
// This is generous enough for large multi-monitor walls while preventing
// overflow-prone values from entering cursor tracking and coordinate mapping.
constexpr double kMaxDisplayCoordinate = 1.0e7;
constexpr double kMaxDisplayScale = 16.0;

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
    kDisplayInfo,
    kPointerPosition,
};

struct Hello {
    DeviceId device;
    std::uint16_t advertised_version{};
    friend bool operator==(const Hello&, const Hello&) = default;
};
struct DisplayInfo {
    DisplayId display;
    Rect logical_bounds;
    double scale{1.0};
    friend bool operator==(const DisplayInfo& a, const DisplayInfo& b) {
        return a.display == b.display && a.logical_bounds.left == b.logical_bounds.left &&
               a.logical_bounds.top == b.logical_bounds.top &&
               a.logical_bounds.right == b.logical_bounds.right &&
               a.logical_bounds.bottom == b.logical_bounds.bottom && a.scale == b.scale;
    }
};
struct PointerPosition {
    SessionId session;
    std::uint64_t generation{};
    Point position;
    friend bool operator==(const PointerPosition&, const PointerPosition&) = default;
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
    Edge target_edge{Edge::kLeft};
    double tangent_ratio{};
    bool entry_normalized{false};
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
    std::variant<Hello, DisplayInfo, PointerPosition, Heartbeat, AcquireRequest, AcquireResponse,
                 ActivateRequest, ActivateAck, RenewRequest, RenewAck, InputMessage, Release,
                 Revoke, ErrorMessage>;

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
