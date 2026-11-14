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

// Version 8 adds chunked file transfer messages. Version 7 adds chunked clipboard transfer messages. Version 6 added the
// target pointer injection capability to DisplayInfo.
// Version 5 added periodic target cursor position corrections and version 4
// added absolute PointerMove input payloads. Each peer advertises the same
// version during Hello so an older executable rejects the connection before
// decoding newer messages.
constexpr std::uint8_t kProtocolVersion = 8;
constexpr std::size_t kHeaderSize = 12;
constexpr std::size_t kMaxFrameSize = 64 * 1024;
constexpr std::size_t kMaxErrorText = 4 * 1024;
constexpr std::uint32_t kMaxClipboardPackageSize = 4 * 1024 * 1024;
// Reserve space for the begin/end frames and per-chunk wire metadata. This
// conservative budget keeps the complete transfer below the 4 MiB package
// cap even when a payload is split into many frames.
constexpr std::uint32_t kClipboardMetadataBudget = 64 * 1024;
constexpr std::uint32_t kMaxClipboardPayloadSize =
    kMaxClipboardPackageSize - kClipboardMetadataBudget;
// ClipboardChunk payload overhead is source (16), sequence (8), format (1),
// offset (4), and data length (4) bytes.
constexpr std::uint32_t kMaxClipboardChunkDataSize =
    static_cast<std::uint32_t>(kMaxFrameSize - kHeaderSize - 16 - 8 - 1 - 4 - 4);
constexpr std::uint64_t kMaxFileSize = 4ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaxFileNameSize = 255;
// FileChunk payload overhead is source (16), transfer (16), offset (8), and
// data length (4) bytes.
constexpr std::size_t kMaxFileChunkDataSize = kMaxFrameSize - kHeaderSize - 16 - 16 - 8 - 4;
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
    kClipboardBegin,
    kClipboardChunk,
    kClipboardEnd,
    kFileBegin,
    kFileChunk,
    kFileEnd,
    kFileCancel,
    kFileStatus,
};

enum class ClipboardFormat : std::uint8_t {
    kText = 1,
    kHtml = 2,
    kPng = 3,
};

enum class FileStatusCode : std::uint8_t {
    kAccepted = 1,
    kProgress,
    kCompleted,
    kFailed,
    kCanceled,
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
    bool absolute_pointer_moves{false};
    friend bool operator==(const DisplayInfo& a, const DisplayInfo& b) {
        return a.display == b.display && a.logical_bounds.left == b.logical_bounds.left &&
               a.logical_bounds.top == b.logical_bounds.top &&
               a.logical_bounds.right == b.logical_bounds.right &&
               a.logical_bounds.bottom == b.logical_bounds.bottom && a.scale == b.scale &&
               a.absolute_pointer_moves == b.absolute_pointer_moves;
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

struct ClipboardBegin {
    DeviceId source;
    std::uint64_t sequence{};
    std::uint8_t format_mask{};
    std::uint32_t total_size{};
    std::uint32_t text_size{};
    std::uint32_t html_size{};
    std::uint32_t png_size{};
    friend bool operator==(const ClipboardBegin&, const ClipboardBegin&) = default;
};
struct ClipboardChunk {
    DeviceId source;
    std::uint64_t sequence{};
    ClipboardFormat format{ClipboardFormat::kText};
    std::uint32_t offset{};
    std::vector<std::uint8_t> data;
    friend bool operator==(const ClipboardChunk&, const ClipboardChunk&) = default;
};
struct ClipboardEnd {
    DeviceId source;
    std::uint64_t sequence{};
    friend bool operator==(const ClipboardEnd&, const ClipboardEnd&) = default;
};
struct FileBegin {
    DeviceId source;
    Uuid128 transfer;
    std::string name;
    std::uint64_t total_size{};
    friend bool operator==(const FileBegin&, const FileBegin&) = default;
};
struct FileChunk {
    DeviceId source;
    Uuid128 transfer;
    std::uint64_t offset{};
    std::vector<std::uint8_t> data;
    friend bool operator==(const FileChunk&, const FileChunk&) = default;
};
struct FileEnd {
    DeviceId source;
    Uuid128 transfer;
    std::uint64_t total_size{};
    friend bool operator==(const FileEnd&, const FileEnd&) = default;
};
struct FileCancel {
    DeviceId source;
    Uuid128 transfer;
    std::string reason;
    friend bool operator==(const FileCancel&, const FileCancel&) = default;
};
struct FileStatus {
    DeviceId source;
    Uuid128 transfer;
    FileStatusCode status{FileStatusCode::kAccepted};
    std::uint64_t bytes{};
    std::uint64_t total_size{};
    std::string message;
    friend bool operator==(const FileStatus&, const FileStatus&) = default;
};

using Message =
    std::variant<Hello, DisplayInfo, PointerPosition, Heartbeat, AcquireRequest, AcquireResponse,
                 ActivateRequest, ActivateAck, RenewRequest, RenewAck, InputMessage, Release,
                 Revoke, ErrorMessage, ClipboardBegin, ClipboardChunk, ClipboardEnd, FileBegin,
                 FileChunk, FileEnd, FileCancel, FileStatus>;

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
